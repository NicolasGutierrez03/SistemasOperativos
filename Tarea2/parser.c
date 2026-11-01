#include "common.h"
#include "parser.h"
#include "board.h"
#include "matchmaker.h"
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>

#define MAX_TOK 32

typedef struct {
    int dn, dm;
    Config cfg;
    Board board; bool board_ready; bool scripted_over;
    Server *srv;
    bool in_suite, in_test, failed, stop, sim_sigint;
    int suite_expected, suite_seen, tid, lineno;
    int tests_pass, tests_fail;
    int mv_ok, mv_rej;
    char name[160];
} Ctx;

static int tokenize(char *line, char **tok, int max) {
    int n = 0;
    char *p = line;
    while (*p && n < max) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (*p == '"') { p++; tok[n++] = p; while (*p && *p != '"') p++; if (*p) *p++ = 0; }
        else { tok[n++] = p; while (*p && !isspace((unsigned char)*p)) p++; if (*p) *p++ = 0; }
    }
    return n;
}

static int parse_int(const char *s, int *out) {
    char *end; errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || end == s || *end || v < -1000000 || v > 1000000) return -1;
    *out = (int)v;
    return 0;
}

/* Nodo = TRONO_TOP|TRONO_LEFT|TRONO_RIGHT  ó  "<x> <y>" (x = columna, y = fila; y=4 junto al trono).
 * Devuelve 0 ok, 1 si la sintaxis es correcta pero esta fuera del tablero (*out = -1:
 * el Juez rechazara la jugada), -1 si hay error de sintaxis. */
static int parse_node(char **tok, int n, int *i, int *out) {
    if (*i >= n) return -1;
    int kw = node_from_keyword(tok[*i]);
    if (kw >= 0) { *out = kw; (*i)++; return 0; }
    int x, y;
    if (*i + 1 >= n || parse_int(tok[*i], &x) || parse_int(tok[*i + 1], &y)) return -1;
    *i += 2;
    *out = node_from_xy(x, y);
    return *out < 0 ? 1 : 0;
}

static void fail(Ctx *c, const char *fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    LOG("  [linea %d] ERROR: %s", c->lineno, buf);
    if (c->in_test) c->failed = true;
}

static bool ensure_board(Ctx *c) {
    if (c->board_ready) return true;
    if (board_init(&c->board, c->cfg.puma_init) != 0) { fail(c, "no se pudo inicializar el tablero"); return false; }
    c->board_ready = true; c->scripted_over = false;
    char buf[1024]; board_dump(&c->board, buf, sizeof buf);
    LOG("  tablero scripted inicial:\n%s", buf);
    return true;
}

static void cleanup_test(Ctx *c) {
    if (c->srv) { server_destroy(c->srv); c->srv = NULL; }
    if (c->board_ready) {
        char buf[1024];
        board_dump(&c->board, buf, sizeof buf);
        LOG("  tablero scripted final:\n%s", buf);
        if (board_check(&c->board) != 0) { LOG("  *** invariantes violados ***"); c->failed = true; }
        board_destroy(&c->board); c->board_ready = false;
    }
}

/* ---------------- eventos ---------------- */
static void check_scripted_victory(Ctx *c) {
    if (atomic_load(&c->board.captures) >= WIN_CAPTURES) {
        LOG("  VICTORIA PUMA (>= %d capturas)", WIN_CAPTURES); c->scripted_over = true; return;
    }
    Move mv[MAX_MOVES];
    if (board_moves(&c->board, PUMA_ID, atomic_load(&c->board.pos[PUMA_ID]), mv) == 0) {
        LOG("  VICTORIA PERROS (Puma acorralado)"); c->scripted_over = true;
    }
}

/* MOVE = solicitud de jugada que el Juez valida. Una jugada rechazada NO falla el caso (es el
 * comportamiento esperado del arbitro); solo falla si se agrega "EXPECT ACCEPT|REJECT" y no coincide. */
static void ev_move(Ctx *c, char **tok, int n) {
    int i = 2, piece, from, to, id;
    if (i >= n) { fail(c, "MOVE incompleto"); return; }
    if (!strcmp(tok[i], "PUMA")) {
        piece = PUMA_ID; i++;
        if (i < n && strcmp(tok[i], "FROM") && parse_int(tok[i], &id) == 0) i++;   /* id opcional */
    } else if (!strcmp(tok[i], "DOG")) {
        if (i + 1 >= n || parse_int(tok[i + 1], &id) || id < 0 || id >= NUM_DOGS) {
            fail(c, "id de perro invalido (rango 0..%d)", NUM_DOGS - 1); return;
        }
        piece = id + 1;                       /* ids externos 0..11 -> piezas internas 1..12 */
        i += 2;
    } else { fail(c, "MOVE espera PUMA o DOG"); return; }
    if (i >= n || strcmp(tok[i], "FROM")) { fail(c, "falta FROM"); return; }
    i++;
    if (parse_node(tok, n, &i, &from) < 0) { fail(c, "origen invalido"); return; }
    if (i >= n || strcmp(tok[i], "TO")) { fail(c, "falta TO"); return; }
    i++;
    if (parse_node(tok, n, &i, &to) < 0) { fail(c, "destino invalido"); return; }
    MoveExpect ex = { false, 0, -1 };
    int expect = -1;                          /* -1 sin expectativa, 0 REJECT, 1 ACCEPT */
    while (i < n) {
        if (i + 2 < n && !strcmp(tok[i], "CAPTURE") && !strcmp(tok[i + 1], "DOG") &&
            parse_int(tok[i + 2], &id) == 0 && id >= 0 && id < NUM_DOGS) {
            ex.cap_dog = id + 1;
            i += 3;
            if (i >= n || strcmp(tok[i], "AT")) { fail(c, "falta AT"); return; }
            i++;
            if (parse_node(tok, n, &i, &ex.cap_node) < 0) { fail(c, "casilla de captura invalida"); return; }
            ex.has_capture = true;
        } else if (i + 1 < n && !strcmp(tok[i], "EXPECT") &&
                   (!strcmp(tok[i + 1], "ACCEPT") || !strcmp(tok[i + 1], "REJECT"))) {
            expect = !strcmp(tok[i + 1], "ACCEPT");
            i += 2;
        } else { fail(c, "sintaxis: [CAPTURE DOG <id> AT <x> <y>] [EXPECT ACCEPT|REJECT]"); return; }
    }
    if (!ensure_board(c)) return;
    char a[32], b[32], d[32], who[24];
    node_label(from, a, sizeof a); node_label(to, b, sizeof b);
    if (piece == PUMA_ID) snprintf(who, sizeof who, "Puma"); else snprintf(who, sizeof who, "perro %d", piece - 1);
    int cap = -1;
    bool ok = false;
    const char *why = "la partida ya termino";
    if (!c->scripted_over) {
        MoveResult r = board_move(&c->board, piece, from, to, ex.has_capture ? &ex : NULL, &cap);
        ok = (r == MV_OK);
        why = move_result_str(r);
    }
    if (ok) {
        c->mv_ok++;
        if (cap >= 0) {
            LOG("  MOVE ACEPTADA %s %s -> %s: CAPTURA perro %d%s%s", who, a, b, cap - 1,
                ex.has_capture ? " en " : "", ex.has_capture ? node_label(ex.cap_node, d, sizeof d) : "");
        } else LOG("  MOVE ACEPTADA %s %s -> %s", who, a, b);
        check_scripted_victory(c);
    } else {
        c->mv_rej++;
        LOG("  MOVE RECHAZADA %s %s -> %s: %s", who, a, b, why);
    }
    if (expect >= 0 && expect != (ok ? 1 : 0)) fail(c, "se esperaba %s", expect ? "ACCEPT" : "REJECT");
}

static void ev_spawn(Ctx *c, char **tok, int n) {
    int count = c->dn, m = c->dm;
    for (int i = 2; i < n; i += 2) {
        int v;
        if (i + 1 >= n || parse_int(tok[i + 1], &v) || v < 1) { fail(c, "SPAWN_PLAYERS: valor invalido"); return; }
        if (!strcmp(tok[i], "COUNT")) count = v;
        else if (!strcmp(tok[i], "MESAS_MAX")) m = v;
        else { fail(c, "SPAWN_PLAYERS: clave desconocida %s", tok[i]); return; }
    }
    if (!c->srv) {
        c->srv = server_create(m, &c->cfg);
        if (!c->srv) { fail(c, "no se pudo crear el servidor"); return; }
    }
    LOG("  SPAWN_PLAYERS count=%d mesas_max=%d", count, m);
    server_spawn(c->srv, count);
}

typedef struct { Board *b; int count; unsigned seed; int ok, rej; } BurstArg;

static void *burst_worker(void *arg) {
    BurstArg *a = arg;
    for (int i = 0; i < a->count && !atomic_load(&g_shutdown); i++) {
        int piece = rng_range(&a->seed, 0, NUM_DOGS);
        int from = atomic_load(&a->b->pos[piece]);
        if (from < 0) { a->rej++; continue; }
        Move mv[MAX_MOVES];
        int cnt = board_moves(a->b, piece, from, mv), to;
        if (cnt > 0 && rng_range(&a->seed, 0, 9) < 8) to = mv[rng_range(&a->seed, 0, cnt - 1)].to;
        else to = rng_range(&a->seed, 0, NN - 1);           /* intento (posiblemente) ilegal */
        if (board_move(a->b, piece, from, to, NULL, NULL) == MV_OK) a->ok++; else a->rej++;
    }
    return NULL;
}

static void ev_burst(Ctx *c, char **tok, int n) {
    int count;
    if (n < 4 || strcmp(tok[2], "COUNT") || parse_int(tok[3], &count) || count < 1) {
        fail(c, "BURST_RANDOM_MOVES COUNT <c>"); return;
    }
    if (!ensure_board(c)) return;
    enum { W = 8 };
    pthread_t th[W]; BurstArg ar[W]; bool st[W] = { false };
    for (int w = 0; w < W; w++) {
        ar[w] = (BurstArg){ &c->board, count / W + (w < count % W), c->cfg.seed * 131u + (unsigned)w, 0, 0 };
        st[w] = pthread_create(&th[w], NULL, burst_worker, &ar[w]) == 0;
    }
    int ok = 0, rej = 0;
    for (int w = 0; w < W; w++) if (st[w]) { pthread_join(th[w], NULL); ok += ar[w].ok; rej += ar[w].rej; }
    int inv = board_check(&c->board);
    LOG("  BURST_RANDOM_MOVES: %d intentos por %d hilos -> %d aplicados, %d rechazados, invariantes %s",
        count, W, ok, rej, inv == 0 ? "OK" : "VIOLADOS");
    if (inv != 0) c->failed = true;
}

static void handle_event(Ctx *c, char **tok, int n) {
    if (n < 2) { fail(c, "EVENT sin tipo"); return; }
    const char *e = tok[1];
    if (!strcmp(e, "MOVE")) ev_move(c, tok, n);
    else if (!strcmp(e, "SPAWN_PLAYERS")) ev_spawn(c, tok, n);
    else if (!strcmp(e, "WAIT_QUEUED_PLAYERS")) {
        if (!c->srv) fail(c, "WAIT_QUEUED_PLAYERS sin SPAWN_PLAYERS previo");
        else { server_wait_done(c->srv); LOG("  WAIT_QUEUED_PLAYERS: cola vaciada"); }
    } else if (!strcmp(e, "DISCONNECT")) {
        bool is_puma = n >= 3 && !strcmp(tok[2], "PLAYER_PUMA");
        bool is_dog = n >= 5 && !strcmp(tok[2], "PLAYER_DOG") && !strcmp(tok[3], "ID");
        int id = 0;
        if (!is_puma && !is_dog) { fail(c, "DISCONNECT PLAYER_PUMA | PLAYER_DOG ID <id>"); return; }
        if (is_dog && parse_int(tok[4], &id)) { fail(c, "id invalido en DISCONNECT"); return; }
        if (!c->srv) {                    /* mesa scripted: el Juez declara abandono */
            if (!ensure_board(c)) return;
            if (c->scripted_over) { LOG("  DISCONNECT ignorado: la partida ya termino"); return; }
            LOG("  DISCONNECT %s: abandono -> %s", is_puma ? "PLAYER_PUMA" : "PLAYER_DOG",
                is_puma ? "GANAN PERROS" : "GANA PUMA");
            c->scripted_over = true;
            return;
        }
        if (is_puma) {
            if (server_disconnect_puma(c->srv) != 0) fail(c, "no hubo mesa activa para desconectar al Puma");
        } else if (server_disconnect_player(c->srv, id) != 0) fail(c, "jugador %d inexistente", id);
    } else if (!strcmp(e, "SIGNAL_SIGINT")) {
        LOG("  SIGNAL_SIGINT: enviando SIGINT al proceso");
        raise(SIGINT);
        sig_wait_shutdown();
        c->sim_sigint = true;     /* cierra el servidor de este caso; la suite continua */
    } else if (!strcmp(e, "BURST_RANDOM_MOVES")) ev_burst(c, tok, n);
    else fail(c, "evento desconocido: %s", e);
}

static void handle_config(Ctx *c, char **tok, int n) {
    if (c->board_ready || c->srv) LOG("  aviso: CONFIG tras eventos; no afecta al tablero/servidor ya creados");
    Config nc = c->cfg;
    int i = 1, v;
    while (i < n) {
        if (!strcmp(tok[i], "SEED") && i + 1 < n && parse_int(tok[i + 1], &v) == 0 && v >= 0) { nc.seed = (unsigned)v; i += 2; }
        else if (!strcmp(tok[i], "DELAY_MIN") && i + 1 < n && parse_int(tok[i + 1], &v) == 0 && v >= 0) { nc.delay_min = v; i += 2; }
        else if (!strcmp(tok[i], "DELAY_MAX") && i + 1 < n && parse_int(tok[i + 1], &v) == 0 && v >= 0) { nc.delay_max = v; i += 2; }
        else if (!strcmp(tok[i], "PUMA_INIT")) {
            i++;
            if (parse_node(tok, n, &i, &nc.puma_init)) { fail(c, "PUMA_INIT invalido"); return; }
        } else { fail(c, "CONFIG: parametro invalido '%s'", tok[i]); return; }
    }
    if (nc.delay_min > nc.delay_max) { fail(c, "DELAY_MIN > DELAY_MAX"); return; }
    c->cfg = nc;
    char b[24];
    LOG("  CONFIG seed=%u delay=%d-%dms puma_init=%s", nc.seed, nc.delay_min, nc.delay_max,
        node_label(nc.puma_init, b, sizeof b));
}

static void end_test(Ctx *c, const char *why) {
    cleanup_test(c);
    if (c->failed) c->tests_fail++; else c->tests_pass++;
    if (c->mv_ok || c->mv_rej) LOG("  jugadas: %d aceptadas, %d rechazadas por el Juez", c->mv_ok, c->mv_rej);
    LOG("<< TEST %d \"%s\": %s%s", c->tid, c->name, c->failed ? "FAIL" : "PASS", why);
    c->in_test = false;
    if (c->sim_sigint) { sig_reset(); c->sim_sigint = false; }   /* servidor ya destruido: se rearma para el siguiente caso */
}

int run_config(const char *path, int default_n, int default_m) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "No se pudo abrir '%s': %s\n", path, strerror(errno)); return -1; }
    Ctx *c = calloc(1, sizeof *c);
    if (!c) { fclose(f); return -1; }
    c->dn = default_n; c->dm = default_m;
    cfg_default(&c->cfg);
    char line[1024], *tok[MAX_TOK];
    /* Ctrl+C real detiene toda la suite; un SIGNAL_SIGINT simulado solo cierra el caso actual */
    while (!c->stop && !(atomic_load(&g_shutdown) && !c->sim_sigint) && fgets(line, sizeof line, f)) {
        c->lineno++;
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (!*p || *p == '#') continue;
        int n = tokenize(p, tok, MAX_TOK);
        if (n == 0) continue;
        const char *cmd = tok[0];
        if (!strcmp(cmd, "SUITE_START")) {
            c->in_suite = true; c->suite_seen = 0; c->suite_expected = 0;
            if (n > 1) parse_int(tok[1], &c->suite_expected);
            LOG("=== SUITE (%d casos esperados) ===", c->suite_expected);
        } else if (!strcmp(cmd, "SUITE_END")) {
            if (c->in_test) end_test(c, " (sin TEST_END)");
            LOG("=== FIN SUITE: %d ejecutados (esperados %d): %d PASS, %d FAIL ===",
                c->suite_seen, c->suite_expected, c->tests_pass, c->tests_fail);
            c->in_suite = false;
        } else if (!strcmp(cmd, "TEST_CASE")) {
            if (c->in_test) end_test(c, " (sin TEST_END)");
            int id = 0;
            if (n < 2 || parse_int(tok[1], &id)) { fail(c, "TEST_CASE <id> \"<nombre>\""); continue; }
            c->in_test = true; c->failed = false; c->tid = id; c->suite_seen++; c->mv_ok = c->mv_rej = 0;
            snprintf(c->name, sizeof c->name, "%s", n > 2 ? tok[2] : "");
            cfg_default(&c->cfg);
            LOG(">> TEST %d \"%s\"", id, c->name);
        } else if (!strcmp(cmd, "TEST_END")) {
            int id = 0;
            if (!c->in_test) { fail(c, "TEST_END sin TEST_CASE"); continue; }
            if (n < 2 || parse_int(tok[1], &id) || id != c->tid) fail(c, "TEST_END con id distinto de %d", c->tid);
            end_test(c, c->sim_sigint ? " (SIGINT simulado: servidor cerrado)" : "");
        } else if (!strcmp(cmd, "CONFIG")) {
            if (!c->in_test) fail(c, "CONFIG fuera de TEST_CASE"); else handle_config(c, tok, n);
        } else if (!strcmp(cmd, "EVENT")) {
            if (!c->in_test) fail(c, "EVENT fuera de TEST_CASE"); else handle_event(c, tok, n);
        } else fail(c, "comando desconocido: %s", cmd);
    }
    fclose(f);
    if (c->in_test) end_test(c, atomic_load(&g_shutdown) ? " (interrumpido por SIGINT)" : " (sin TEST_END)");
    if (atomic_load(&g_shutdown)) LOG("Ejecucion interrumpida por SIGINT; recursos liberados");
    LOG("RESUMEN: %d PASS, %d FAIL", c->tests_pass, c->tests_fail);
    free(c);
    return 0;
}
