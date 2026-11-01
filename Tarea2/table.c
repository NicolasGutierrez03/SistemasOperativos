/* table.c - Mesa: hilo Juez + hilos de fichas (1 Puma + 12 Perros). */
#include "common.h"
#include "matchmaker.h"
#include <errno.h>

Table *table_create(Server *s, int id, Player *puma, Player *dogs) {
    Table *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->id = id; t->srv = s; t->puma_p = puma; t->dog_p = dogs;
    t->cfg = s->cfg;
    t->cfg.seed = s->cfg.seed + (unsigned)id;
    atomic_init(&t->over, 0);
    if (pthread_mutex_init(&t->ev_mtx, NULL) != 0) { free(t); return NULL; }
    if (cond_init_mono(&t->judge_cv) != 0) { pthread_mutex_destroy(&t->ev_mtx); free(t); return NULL; }
    if (cond_init_mono(&t->sleep_cv) != 0) {
        pthread_cond_destroy(&t->judge_cv); pthread_mutex_destroy(&t->ev_mtx); free(t); return NULL;
    }
    for (int k = 0; k <= NUM_DOGS; k++) { t->pa[k].t = t; t->pa[k].piece = k; }
    return t;
}

void table_destroy(Table *t) {
    if (!t) return;
    pthread_cond_destroy(&t->judge_cv);
    pthread_cond_destroy(&t->sleep_cv);
    pthread_mutex_destroy(&t->ev_mtx);
    free(t);
}

/* Fija el resultado una sola vez y despierta a todos (Juez y fichas dormidas). */
static void table_finish(Table *t, Outcome o, const char *reason) {
    pthread_mutex_lock(&t->ev_mtx);
    if (!atomic_load(&t->over)) {
        t->outcome = o; t->reason = reason;
        atomic_store(&t->over, 1);
    }
    pthread_cond_broadcast(&t->sleep_cv);
    pthread_cond_broadcast(&t->judge_cv);
    pthread_mutex_unlock(&t->ev_mtx);
}

void table_abort(Table *t, const char *reason) { table_finish(t, OUT_ABORT, reason); }

void table_poke(Table *t) {
    pthread_mutex_lock(&t->ev_mtx);
    t->dirty = 1;
    pthread_cond_signal(&t->judge_cv);
    pthread_mutex_unlock(&t->ev_mtx);
}

/* Espera pasiva (pthread_cond_timedwait) = "retardo de decisión" sin busy-wait.
 * Devuelve false si la partida terminó mientras dormía. */
static bool piece_sleep(Table *t, int ms) {
    struct timespec dl;
    deadline_in_ms(&dl, ms);
    pthread_mutex_lock(&t->ev_mtx);
    while (!atomic_load(&t->over))
        if (pthread_cond_timedwait(&t->sleep_cv, &t->ev_mtx, &dl) == ETIMEDOUT) break;
    bool alive = !atomic_load(&t->over);
    pthread_mutex_unlock(&t->ev_mtx);
    return alive;
}

static void *piece_main(void *arg) {
    PieceArg *a = arg;
    Table *t = a->t;
    int k = a->piece;
    Player *pl = (k == PUMA_ID) ? t->puma_p : t->dog_p;
    unsigned seed = t->cfg.seed * 2654435761u + (unsigned)t->id * 40503u + (unsigned)k * 977u + 1u;
    char b1[24], b2[24], b3[24];
    while (!atomic_load(&t->over)) {
        if (!piece_sleep(t, rng_range(&seed, t->cfg.delay_min, t->cfg.delay_max))) break;
        if (atomic_load(&pl->disconnected)) { table_poke(t); break; }   /* el Juez declarará abandono */
        int from = atomic_load(&t->board.pos[k]);
        if (from < 0) break;                                           /* perro capturado */
        Move mv[MAX_MOVES];
        int n = board_moves(&t->board, k, from, mv);
        if (n <= 0) continue;
        int pick = -1;
        if (k == PUMA_ID)
            for (int i = 0; i < n; i++) if (mv[i].over >= 0) { pick = i; break; }   /* prefiere capturar */
        if (pick < 0) pick = rng_range(&seed, 0, n - 1);
        int cap = -1;
        MoveResult r = board_move(&t->board, k, from, mv[pick].to, NULL, &cap);
        if (r != MV_OK) continue;      /* otra ficha ganó la carrera: se reintenta en el próximo ciclo */
        if (cap >= 0)
            LOG("Mesa %d: Puma %s -> %s CAPTURA perro %d en %s (capturas=%d)", t->id,
                node_label(from, b1, sizeof b1), node_label(mv[pick].to, b2, sizeof b2), cap,
                node_label(mv[pick].over, b3, sizeof b3), atomic_load(&t->board.captures));
        else if (g_verbose)
            LOG("Mesa %d: %s %d %s -> %s", t->id, k == PUMA_ID ? "Puma" : "Perro", k,
                node_label(from, b1, sizeof b1), node_label(mv[pick].to, b2, sizeof b2));
        table_poke(t);
    }
    return NULL;
}

/* Árbitro: decide si la partida terminó. Devuelve true si over. */
static bool evaluate(Table *t) {
    if (atomic_load(&t->over)) return true;
    bool pd = atomic_load(&t->puma_p->disconnected), dd = atomic_load(&t->dog_p->disconnected);
    if (pd && dd) { table_finish(t, OUT_ABORT, "ambos jugadores desconectados"); return true; }
    if (pd) { table_finish(t, OUT_DOGS_WIN, "abandono del jugador Puma"); return true; }
    if (dd) { table_finish(t, OUT_PUMA_WINS, "abandono del jugador Perros"); return true; }
    if (atomic_load(&t->board.captures) >= WIN_CAPTURES) {
        table_finish(t, OUT_PUMA_WINS, "Puma alcanzo 6 capturas"); return true;
    }
    Move mv[MAX_MOVES];
    int n = -1;
    for (int tries = 0; tries < 3 && n < 0; tries++)
        n = board_moves(&t->board, PUMA_ID, atomic_load(&t->board.pos[PUMA_ID]), mv);
    if (n == 0) { table_finish(t, OUT_DOGS_WIN, "Puma acorralado"); return true; }
    if (atomic_load(&t->board.moves) >= MAX_TABLE_MOVES) {
        table_finish(t, OUT_DRAW, "limite de jugadas"); return true;
    }
    return false;
}

static const char *outcome_str(Outcome o) {
    switch (o) {
    case OUT_PUMA_WINS: return "GANA PUMA";
    case OUT_DOGS_WIN: return "GANAN PERROS";
    case OUT_DRAW: return "EMPATE";
    case OUT_ABORT: return "PARTIDA CANCELADA";
    default: return "?";
    }
}

void *table_judge_main(void *arg) {
    Table *t = arg;
    Server *s = t->srv;
    char buf[1024];
    bool board_ok = (board_init(&t->board, t->cfg.puma_init) == 0);
    if (!board_ok) table_finish(t, OUT_ABORT, "fallo al inicializar el tablero");

    LOG("Mesa %d: INICIA Jugador %d (Puma) vs Jugador %d (Perros)", t->id, t->puma_p->id, t->dog_p->id);
    bool started[NUM_DOGS + 1] = { false };
    int last_dump = 0;
    if (board_ok) {
        board_dump(&t->board, buf, sizeof buf);
        LOG("Mesa %d: estado inicial\n%s", t->id, buf);
        if (!evaluate(t)) {
            for (int k = 0; k <= NUM_DOGS; k++) {
                if (pthread_create(&t->piece_th[k], NULL, piece_main, &t->pa[k]) != 0) {
                    table_finish(t, OUT_ABORT, "fallo al crear hilo de ficha");
                    break;
                }
                started[k] = true;
            }
            /* Bucle del Juez: duerme en condvar hasta que alguna ficha notifica una jugada */
            for (;;) {
                pthread_mutex_lock(&t->ev_mtx);
                while (!atomic_load(&t->over) && !t->dirty) pthread_cond_wait(&t->judge_cv, &t->ev_mtx);
                t->dirty = 0;
                pthread_mutex_unlock(&t->ev_mtx);
                /* Registro periodico del tablero: cada jugada con TORNEO_VERBOSE=1, si no cada 10 jugadas */
                int mv_now = atomic_load(&t->board.moves);
                if (mv_now - last_dump >= (g_verbose ? 1 : 10)) {
                    last_dump = mv_now;
                    board_dump(&t->board, buf, sizeof buf);
                    LOG("Mesa %d: estado tras %d jugadas\n%s", t->id, mv_now, buf);
                }
                if (evaluate(t)) break;
            }
        }
    }
    for (int k = 0; k <= NUM_DOGS; k++) if (started[k]) pthread_join(t->piece_th[k], NULL);

    if (board_ok) {
        board_dump(&t->board, buf, sizeof buf);
        LOG("Mesa %d: estado final\n%s", t->id, buf);
        if (board_check(&t->board) != 0) LOG("Mesa %d: *** INVARIANTES DEL TABLERO VIOLADOS ***", t->id);
        board_destroy(&t->board);     /* ninguna ficha retiene ya mutex */
    }
    LOG("Mesa %d: FIN -> %s (%s)", t->id, outcome_str(t->outcome), t->reason ? t->reason : "");

    Result rp = RES_CANCELLED, rd = RES_CANCELLED;
    if (t->outcome == OUT_PUMA_WINS) { rp = RES_WIN; rd = RES_LOSE; }
    else if (t->outcome == OUT_DOGS_WIN) { rp = RES_LOSE; rd = RES_WIN; }
    else if (t->outcome == OUT_DRAW) { rp = rd = RES_DRAW; }

    /* Liberar la mesa: contadores + sem_post permiten que el Matchmaker lance otra pareja */
    pthread_mutex_lock(&s->mtx);
    t->finished = true;
    s->active--;
    s->st_games++;
    if (t->outcome == OUT_PUMA_WINS) s->st_puma++;
    else if (t->outcome == OUT_DOGS_WIN) s->st_dogs++;
    else if (t->outcome == OUT_DRAW) s->st_draw++;
    else s->st_abort++;
    pthread_mutex_unlock(&s->mtx);
    sem_post(&s->slots);

    finish_player(s, t->puma_p, rp);
    finish_player(s, t->dog_p, rd);
    return NULL;
}
