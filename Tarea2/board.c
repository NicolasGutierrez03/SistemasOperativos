#include "common.h"
#include "board.h"
#include <stdlib.h>

typedef struct {
    int adj[8];  int nadj;
    struct { int over, to; } jumps[8];  int njumps;
} Node;

static Node g_topo[NN];                 /* sólo lectura tras pthread_once */
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static bool is_adj(int a, int b) {
    for (int i = 0; i < g_topo[a].nadj; i++) if (g_topo[a].adj[i] == b) return true;
    return false;
}
static void add_edge(int a, int b) {
    if (a == b || is_adj(a, b)) return;
    g_topo[a].adj[g_topo[a].nadj++] = b;
    g_topo[b].adj[g_topo[b].nadj++] = a;
}
static void add_jump(int a, int o, int t) {
    if (!is_adj(a, o) || !is_adj(o, t)) return;
    Node *n = &g_topo[a];
    n->jumps[n->njumps].over = o;
    n->jumps[n->njumps].to = t;
    n->njumps++;
}

/* Tablero segun la figura del enunciado:
 *  - Cuadricula 5x5 SOLO con aristas ortogonales (sin diagonales).
 *  - Trono: T (apice) y dos nodos LEFT/RIGHT unidos entre si (base horizontal).
 *  - T se une a LEFT, a RIGHT y baja en vertical hasta (0,2).
 *  - LEFT y RIGHT se unen en diagonal a las esquinas (0,0) y (0,4): el "techo" T-LEFT-(0,0)
 *    y T-RIGHT-(0,4) son rectas continuas, por lo que tambien admiten saltos.
 *  - TRONO_VERTICAL_EDGES=1: LEFT/RIGHT ademas bajan en vertical a (0,1)/(0,3).
 *    No es claro en la figura; poner 0 si el enunciado no las dibuja. */
#define TRONO_VERTICAL_EDGES 1

static void topo_build(void) {
    memset(g_topo, 0, sizeof g_topo);
    for (int x = 0; x < GRID; x++)
        for (int y = 0; y < GRID; y++) {
            int id = x * GRID + y;
            if (y + 1 < GRID) add_edge(id, id + 1);
            if (x + 1 < GRID) add_edge(id, id + GRID);
        }
    add_edge(TRONO_TOP, TRONO_LEFT);
    add_edge(TRONO_TOP, TRONO_RIGHT);
    add_edge(TRONO_LEFT, TRONO_RIGHT);
    add_edge(TRONO_TOP, 0 * GRID + 2);
    add_edge(TRONO_LEFT, 0 * GRID + 0);
    add_edge(TRONO_RIGHT, 0 * GRID + 4);
#if TRONO_VERTICAL_EDGES
    add_edge(TRONO_LEFT, 0 * GRID + 1);
    add_edge(TRONO_RIGHT, 0 * GRID + 3);
#endif
    /* Saltos (a, sobre, c): tres nodos colineales con ambos tramos adyacentes */
    for (int a = 0; a < GRID * GRID; a++)
        for (int i = 0; i < g_topo[a].nadj; i++) {
            int b = g_topo[a].adj[i];
            if (b >= GRID * GRID) continue;
            int ax = a / GRID, ay = a % GRID, bx = b / GRID, by = b % GRID;
            int cx = bx + (bx - ax), cy = by + (by - ay);
            if (cx < 0 || cx >= GRID || cy < 0 || cy >= GRID) continue;
            add_jump(a, b, cx * GRID + cy);
        }
    /* Lineas que entran al trono */
    add_jump(1 * GRID + 2, 0 * GRID + 2, TRONO_TOP);
    add_jump(TRONO_TOP, 0 * GRID + 2, 1 * GRID + 2);
    add_jump(TRONO_TOP, TRONO_LEFT, 0 * GRID + 0);
    add_jump(0 * GRID + 0, TRONO_LEFT, TRONO_TOP);
    add_jump(TRONO_TOP, TRONO_RIGHT, 0 * GRID + 4);
    add_jump(0 * GRID + 4, TRONO_RIGHT, TRONO_TOP);
#if TRONO_VERTICAL_EDGES
    add_jump(1 * GRID + 1, 0 * GRID + 1, TRONO_LEFT);
    add_jump(TRONO_LEFT, 0 * GRID + 1, 1 * GRID + 1);
    add_jump(1 * GRID + 3, 0 * GRID + 3, TRONO_RIGHT);
    add_jump(TRONO_RIGHT, 0 * GRID + 3, 1 * GRID + 3);
#endif
}

static int find_jump(int from, int to) {
    for (int i = 0; i < g_topo[from].njumps; i++)
        if (g_topo[from].jumps[i].to == to) return g_topo[from].jumps[i].over;
    return -1;
}

int node_from_xy(int x, int y) {
    if (x < 0 || x >= GRID || y < 0 || y >= GRID) return -1;
    return (GRID - 1 - y) * GRID + x;     /* x = columna, y = fila (y=4 es la fila junto al trono) */
}
int node_from_keyword(const char *s) {
    if (!strcmp(s, "TRONO_TOP")) return TRONO_TOP;
    if (!strcmp(s, "TRONO_LEFT")) return TRONO_LEFT;
    if (!strcmp(s, "TRONO_RIGHT")) return TRONO_RIGHT;
    return -1;
}
const char *node_label(int id, char *buf, size_t n) {
    if (id == TRONO_TOP) snprintf(buf, n, "TRONO_TOP");
    else if (id == TRONO_LEFT) snprintf(buf, n, "TRONO_LEFT");
    else if (id == TRONO_RIGHT) snprintf(buf, n, "TRONO_RIGHT");
    else if (id >= 0 && id < GRID * GRID) snprintf(buf, n, "(%d,%d)", id % GRID, GRID - 1 - id / GRID);
    else snprintf(buf, n, "fuera_del_tablero");
    return buf;
}
const char *move_result_str(MoveResult r) {
    switch (r) {
    case MV_OK: return "ok";
    case MV_BAD_POS: return "posicion invalida";
    case MV_NOT_ADJ: return "destino no adyacente ni salto valido";
    case MV_OCCUPIED: return "casilla destino ocupada";
    case MV_NO_PIECE: return "la pieza no esta en el origen";
    case MV_BAD_CAPTURE: return "captura invalida";
    }
    return "?";
}

/* ---- bloqueo ordenado de conjuntos de nodos ---- */
static int cmp_int(const void *a, const void *b) {
    return *(const int *)a - *(const int *)b;
}
static int lock_set(Board *b, int *ids, int n) {
    qsort(ids, (size_t)n, sizeof(int), cmp_int);
    int m = 0;
    for (int i = 0; i < n; i++) if (m == 0 || ids[m - 1] != ids[i]) ids[m++] = ids[i];  /* dedup */
    for (int i = 0; i < m; i++) pthread_mutex_lock(&b->lock[ids[i]]);   /* orden ascendente */
    return m;
}
static void unlock_set(Board *b, const int *ids, int m) {
    for (int i = m - 1; i >= 0; i--) pthread_mutex_unlock(&b->lock[ids[i]]);
}

int board_init(Board *b, int puma_pos) {
    pthread_once(&g_once, topo_build);
    if (puma_pos < 0 || puma_pos >= NN) return -1;
    memset(b->occ, 0, sizeof b->occ);
    atomic_init(&b->captures, 0);
    atomic_init(&b->moves, 0);
    for (int i = 0; i < NN; i++)
        if (pthread_mutex_init(&b->lock[i], NULL) != 0) {
            while (--i >= 0) pthread_mutex_destroy(&b->lock[i]);
            return -1;
        }
    b->occ[puma_pos] = -1;
    atomic_init(&b->pos[PUMA_ID], puma_pos);
    static const int cand[] = { 20,21,22,23,24, 15,16,17,18,19, 10,14, 11,13, 5,9 };
    int k = 1;
    for (size_t i = 0; i < sizeof cand / sizeof cand[0] && k <= NUM_DOGS; i++) {
        if (cand[i] == puma_pos) continue;       /* el Puma manda sobre la casilla */
        b->occ[cand[i]] = k;
        atomic_init(&b->pos[k], cand[i]);
        k++;
    }
    return 0;
}

void board_destroy(Board *b) {
    for (int i = 0; i < NN; i++) pthread_mutex_destroy(&b->lock[i]);
}

/* Movimiento/captura atómico: toma los mutex de origen, destino (y casilla saltada)
 * en orden de id y valida TODO bajo exclusión mutua -> dos fichas que apuntan al
 * mismo slot "en el mismo milisegundo" se serializan; la segunda ve OCCUPIED. */
MoveResult board_move(Board *b, int piece, int from, int to, const MoveExpect *ex, int *captured_dog) {
    if (captured_dog) *captured_dog = -1;
    if (piece < 0 || piece > NUM_DOGS || from < 0 || from >= NN || to < 0 || to >= NN || from == to)
        return MV_BAD_POS;
    int over = -1;
    if (!is_adj(from, to)) {
        if (piece != PUMA_ID) return MV_NOT_ADJ;      /* los perros no saltan */
        over = find_jump(from, to);
        if (over < 0) return MV_NOT_ADJ;
    }
    if (ex && ex->has_capture && over < 0) return MV_BAD_CAPTURE;
    int set[3] = { from, to, over >= 0 ? over : to };
    int m = lock_set(b, set, over >= 0 ? 3 : 2);
    MoveResult r = MV_OK;
    int me = (piece == PUMA_ID) ? -1 : piece;
    int dog = 0;
    if (b->occ[from] != me) r = MV_NO_PIECE;
    else if (b->occ[to] != 0) r = MV_OCCUPIED;
    else if (over >= 0) {
        dog = b->occ[over];
        if (dog <= 0) r = MV_BAD_CAPTURE;
        else if (ex && ex->has_capture && (ex->cap_node != over || ex->cap_dog != dog)) r = MV_BAD_CAPTURE;
    }
    if (r == MV_OK) {
        b->occ[to] = me;
        b->occ[from] = 0;
        atomic_store(&b->pos[piece], to);
        if (over >= 0) {
            b->occ[over] = 0;
            atomic_store(&b->pos[dog], -1);
            atomic_fetch_add(&b->captures, 1);
            if (captured_dog) *captured_dog = dog;
        }
        atomic_fetch_add(&b->moves, 1);
    }
    unlock_set(b, set, m);
    return r;
}

/* Movimientos legales de una pieza desde 'from' (instantánea consistente bajo locks). */
int board_moves(Board *b, int piece, int from, Move *out) {
    if (from < 0 || from >= NN) return -1;
    const Node *nd = &g_topo[from];
    int set[NN], n = 0;
    set[n++] = from;
    for (int i = 0; i < nd->nadj; i++) set[n++] = nd->adj[i];
    if (piece == PUMA_ID)
        for (int i = 0; i < nd->njumps; i++) { set[n++] = nd->jumps[i].over; set[n++] = nd->jumps[i].to; }
    int m = lock_set(b, set, n);
    int cnt = -1;
    if (b->occ[from] == (piece == PUMA_ID ? -1 : piece)) {
        cnt = 0;
        for (int i = 0; i < nd->nadj; i++)
            if (b->occ[nd->adj[i]] == 0) { out[cnt].to = nd->adj[i]; out[cnt].over = -1; cnt++; }
        if (piece == PUMA_ID)
            for (int i = 0; i < nd->njumps; i++)
                if (b->occ[nd->jumps[i].over] > 0 && b->occ[nd->jumps[i].to] == 0) {
                    out[cnt].to = nd->jumps[i].to; out[cnt].over = nd->jumps[i].over; cnt++;
                }
    }
    unlock_set(b, set, m);
    return cnt;
}

static void lock_all(Board *b)   { for (int i = 0; i < NN; i++) pthread_mutex_lock(&b->lock[i]); }
static void unlock_all(Board *b) { for (int i = NN - 1; i >= 0; i--) pthread_mutex_unlock(&b->lock[i]); }

void board_dump(Board *b, char *out, size_t sz) {
    lock_all(b);
    char c[NN];
    for (int i = 0; i < NN; i++) c[i] = b->occ[i] < 0 ? 'P' : (b->occ[i] > 0 ? 'd' : '.');
    int off = 0, caps = atomic_load(&b->captures);
    off += snprintf(out + off, sz - (size_t)off, "    TRONO  TOP[%c] LEFT[%c] RIGHT[%c]\n", c[25], c[26], c[27]);
    off += snprintf(out + off, sz - (size_t)off, "           x: 0 1 2 3 4\n");
    for (int x = 0; x < GRID && (size_t)off < sz; x++)
        off += snprintf(out + off, sz - (size_t)off, "    y=%d      %c %c %c %c %c\n", GRID - 1 - x,
                        c[x * 5], c[x * 5 + 1], c[x * 5 + 2], c[x * 5 + 3], c[x * 5 + 4]);
    snprintf(out + off, sz - (size_t)off, "    capturas=%d  jugadas=%d", caps, atomic_load(&b->moves));
    unlock_all(b);
}

int board_check(Board *b) {
    lock_all(b);
    int pumas = 0, dogs_cells = 0, dogs_alive = 0, bad = 0;
    for (int i = 0; i < NN; i++) { if (b->occ[i] < 0) pumas++; else if (b->occ[i] > 0) dogs_cells++; }
    for (int k = 0; k <= NUM_DOGS; k++) {
        int p = atomic_load(&b->pos[k]);
        if (p < 0) { if (k == PUMA_ID) bad = 1; continue; }
        if (b->occ[p] != (k == PUMA_ID ? -1 : k)) bad = 1;
        if (k > 0) dogs_alive++;
    }
    if (pumas != 1 || dogs_cells != dogs_alive || dogs_alive != NUM_DOGS - atomic_load(&b->captures)) bad = 1;
    unlock_all(b);
    return bad ? -1 : 0;
}
