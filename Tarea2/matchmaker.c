/* matchmaker.c - cola pasiva de jugadores, emparejamiento y tope de M mesas (sem_t). */
#include "common.h"
#include "matchmaker.h"
#include <errno.h>

/* ---- jugadores ---- */
static const char *res_str(Result r) {
    switch (r) {
    case RES_WIN: return "VICTORIA"; case RES_LOSE: return "DERROTA"; case RES_DRAW: return "EMPATE";
    case RES_UNPAIRED: return "SIN PAREJA"; case RES_CANCELLED: return "CANCELADO"; default: return "?";
    }
}

/* Caller debe tener s->mtx. Orden de locks: s->mtx -> p->mtx. */
static void finish_player_locked(Server *s, Player *p, Result r) {
    pthread_mutex_lock(&p->mtx);
    if (p->state != PS_DONE) {
        p->state = PS_DONE;
        p->result = r;
        s->finished++;
        pthread_cond_broadcast(&p->cv);
        pthread_cond_broadcast(&s->done_cv);
        LOG("Jugador %d: %s", p->id, res_str(r));
    }
    pthread_mutex_unlock(&p->mtx);
}

void finish_player(Server *s, Player *p, Result r) {
    pthread_mutex_lock(&s->mtx);
    finish_player_locked(s, p, r);
    pthread_mutex_unlock(&s->mtx);
}

static void *player_main(void *arg) {
    Player *p = arg;
    Server *s = p->srv;
    sleep_ms(rng_range(&p->seed, 0, 60));          /* llegada escalonada */
    pthread_mutex_lock(&s->mtx);
    if (s->shutdown) { finish_player_locked(s, p, RES_CANCELLED); pthread_mutex_unlock(&s->mtx); return NULL; }
    pthread_mutex_lock(&p->mtx); p->state = PS_QUEUED; pthread_mutex_unlock(&p->mtx);
    p->next = NULL;
    if (s->qtail) s->qtail->next = p; else s->qhead = p;
    s->qtail = p; s->qcount++; s->enqueued++;
    LOG("Jugador %d: llega a la cola (en espera: %d)", p->id, s->qcount);
    pthread_cond_broadcast(&s->queue_cv);
    pthread_mutex_unlock(&s->mtx);
    /* Espera pasiva hasta que su partida termine */
    pthread_mutex_lock(&p->mtx);
    while (p->state != PS_DONE) pthread_cond_wait(&p->cv, &p->mtx);
    pthread_mutex_unlock(&p->mtx);
    return NULL;
}

static Player *queue_pop(Server *s) {          /* s->mtx tomado */
    Player *p = s->qhead;
    s->qhead = p->next;
    if (!s->qhead) s->qtail = NULL;
    p->next = NULL; s->qcount--;
    return p;
}

/* ---- Matchmaker ---- */
static void *matchmaker_main(void *arg) {
    Server *s = arg;
    for (;;) {
        pthread_mutex_lock(&s->mtx);
        while (!s->shutdown && !s->closing && s->qcount < 2 &&
               !(s->qcount == 1 && s->enqueued == s->expected))
            pthread_cond_wait(&s->queue_cv, &s->mtx);           /* cola pasiva: sin busy-wait */
        if (s->shutdown || s->closing) { pthread_mutex_unlock(&s->mtx); break; }
        if (s->qcount < 2) {                                     /* jugador impar sin rival */
            finish_player_locked(s, queue_pop(s), RES_UNPAIRED);
            pthread_mutex_unlock(&s->mtx);
            continue;
        }
        Player *a = queue_pop(s), *b = queue_pop(s);
        if (rand_r(&s->seed) & 1) { Player *tmp = a; a = b; b = tmp; }
        pthread_mutex_unlock(&s->mtx);

        /* Tope de M mesas: bloquea sin CPU hasta que alguna mesa se libere (o SIGINT haga sem_post) */
        while (sem_wait(&s->slots) == -1 && errno == EINTR) { }

        pthread_mutex_lock(&s->mtx);
        if (s->shutdown) {
            finish_player_locked(s, a, RES_CANCELLED);
            finish_player_locked(s, b, RES_CANCELLED);
            pthread_mutex_unlock(&s->mtx);
            break;
        }
        Table *t = table_create(s, s->ntables + 1, a, b);
        bool ok = t != NULL;
        if (ok && s->ntables == s->tcap) {
            int nc = s->tcap ? s->tcap * 2 : 8;
            Table **nt = realloc(s->tables, (size_t)nc * sizeof *nt);
            if (nt) { s->tables = nt; s->tcap = nc; } else ok = false;
        }
        if (ok) {
            s->tables[s->ntables++] = t;
            a->role = ROLE_PUMA; b->role = ROLE_DOGS; a->table = b->table = t;
            pthread_mutex_lock(&a->mtx); a->state = PS_PLAYING; pthread_mutex_unlock(&a->mtx);
            pthread_mutex_lock(&b->mtx); b->state = PS_PLAYING; pthread_mutex_unlock(&b->mtx);
            if (pthread_create(&t->judge, NULL, table_judge_main, t) == 0) {
                t->judge_started = true;
                s->active++;
                if (s->active > s->peak_active) s->peak_active = s->active;
                LOG("Matchmaker: mesa %d asignada (activas=%d/%d)", t->id, s->active, s->max_tables);
                pthread_cond_broadcast(&s->active_cv);
            } else {
                t->finished = true; ok = false;
            }
        } else if (t) { table_destroy(t); }
        if (!ok) {
            sem_post(&s->slots);
            finish_player_locked(s, a, RES_CANCELLED);
            finish_player_locked(s, b, RES_CANCELLED);
        }
        pthread_mutex_unlock(&s->mtx);
    }
    return NULL;
}

/* ---- API servidor ---- */
void server_shutdown(Server *s) {
    pthread_mutex_lock(&s->mtx);
    if (!s->shutdown) {
        s->shutdown = true;
        LOG("Servidor: SIGINT -> deteniendo ingreso y cancelando partidas activas");
        for (int i = 0; i < s->ntables; i++)
            if (s->tables[i]->judge_started && !s->tables[i]->finished) table_abort(s->tables[i], "SIGINT");
        for (int i = 0; i < s->nplayers; i++) finish_player_locked(s, s->players[i], RES_CANCELLED);
        pthread_cond_broadcast(&s->queue_cv);
        pthread_cond_broadcast(&s->done_cv);
        pthread_cond_broadcast(&s->active_cv);
        sem_post(&s->slots);        /* despierta al Matchmaker si dormía en sem_wait */
    }
    pthread_mutex_unlock(&s->mtx);
}
static void shutdown_cb(void *a) { server_shutdown(a); }

Server *server_create(int max_tables, const Config *cfg) {
    Server *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->max_tables = max_tables; s->cfg = *cfg; s->seed = cfg->seed ? cfg->seed : 1;
    if (sem_init(&s->slots, 0, (unsigned)max_tables) != 0) { free(s); return NULL; }
    if (pthread_mutex_init(&s->mtx, NULL) != 0) { sem_destroy(&s->slots); free(s); return NULL; }
    if (pthread_cond_init(&s->queue_cv, NULL) || pthread_cond_init(&s->done_cv, NULL) ||
        cond_init_mono(&s->active_cv)) {
        pthread_mutex_destroy(&s->mtx); sem_destroy(&s->slots); free(s); return NULL;
    }
    if (pthread_create(&s->mm, NULL, matchmaker_main, s) != 0) {
        pthread_cond_destroy(&s->queue_cv); pthread_cond_destroy(&s->done_cv);
        pthread_cond_destroy(&s->active_cv); pthread_mutex_destroy(&s->mtx);
        sem_destroy(&s->slots); free(s); return NULL;
    }
    sig_set_server(shutdown_cb, s);
    return s;
}

int server_spawn(Server *s, int count) {
    int created = 0;
    for (int i = 0; i < count; i++) {
        Player *p = calloc(1, sizeof *p);
        if (!p) break;
        p->srv = s; p->state = PS_NEW; atomic_init(&p->disconnected, 0);
        if (pthread_mutex_init(&p->mtx, NULL) != 0) { free(p); break; }
        if (pthread_cond_init(&p->cv, NULL) != 0) { pthread_mutex_destroy(&p->mtx); free(p); break; }
        pthread_mutex_lock(&s->mtx);
        if (s->nplayers == s->pcap) {
            int nc = s->pcap ? s->pcap * 2 : 16;
            Player **np = realloc(s->players, (size_t)nc * sizeof *np);
            if (!np) {
                pthread_mutex_unlock(&s->mtx);
                pthread_cond_destroy(&p->cv); pthread_mutex_destroy(&p->mtx); free(p); break;
            }
            s->players = np; s->pcap = nc;
        }
        p->id = s->nplayers + 1;
        p->seed = s->cfg.seed * 31u + (unsigned)p->id * 7919u;
        s->players[s->nplayers++] = p;
        s->expected++;
        pthread_mutex_unlock(&s->mtx);
        if (pthread_create(&p->tid, NULL, player_main, p) == 0) p->started = true;
        else {
            pthread_mutex_lock(&s->mtx);
            s->enqueued++;                              /* cuenta como "llegado" para el cierre */
            finish_player_locked(s, p, RES_CANCELLED);
            pthread_cond_broadcast(&s->queue_cv);
            pthread_mutex_unlock(&s->mtx);
        }
        created++;
    }
    return created;
}

void server_wait_done(Server *s) {
    pthread_mutex_lock(&s->mtx);
    while (!s->shutdown && s->finished < s->expected) pthread_cond_wait(&s->done_cv, &s->mtx);
    pthread_mutex_unlock(&s->mtx);
}

int server_disconnect_puma(Server *s) {
    struct timespec dl;
    deadline_in_ms(&dl, 5000);
    pthread_mutex_lock(&s->mtx);
    Table *t = NULL;
    while (!s->shutdown && !t) {
        for (int i = 0; i < s->ntables; i++)
            if (s->tables[i]->judge_started && !s->tables[i]->finished) { t = s->tables[i]; break; }
        if (t) break;
        if (pthread_cond_timedwait(&s->active_cv, &s->mtx, &dl) == ETIMEDOUT) break;
    }
    if (t) atomic_store(&t->puma_p->disconnected, 1);
    pthread_mutex_unlock(&s->mtx);
    if (!t) return -1;
    LOG("Mesa %d: el jugador Puma (%d) se DESCONECTA", t->id, t->puma_p->id);
    table_poke(t);
    return 0;
}

int server_disconnect_player(Server *s, int pid) {
    pthread_mutex_lock(&s->mtx);
    Player *p = NULL;
    for (int i = 0; i < s->nplayers; i++) if (s->players[i]->id == pid) { p = s->players[i]; break; }
    Table *t = NULL;
    if (p) { atomic_store(&p->disconnected, 1); t = p->table; }
    pthread_mutex_unlock(&s->mtx);
    if (!p) return -1;
    LOG("Jugador %d se DESCONECTA", pid);
    if (t) table_poke(t);
    return 0;
}

void server_destroy(Server *s) {
    if (!s) return;
    server_wait_done(s);                 /* SIGINT también lo libera */
    sig_set_server(NULL, NULL);          /* espera a que termine un cierre en curso */
    pthread_mutex_lock(&s->mtx);
    s->closing = true;
    pthread_cond_broadcast(&s->queue_cv);
    pthread_mutex_unlock(&s->mtx);
    pthread_join(s->mm, NULL);
    for (int i = 0; i < s->ntables; i++)
        if (s->tables[i]->judge_started) pthread_join(s->tables[i]->judge, NULL);
    for (int i = 0; i < s->nplayers; i++)
        if (s->players[i]->started) pthread_join(s->players[i]->tid, NULL);
    LOG("Servidor: resumen -> partidas=%d (Puma=%d, Perros=%d, empates=%d, canceladas=%d), "
        "pico de mesas activas=%d (tope M=%d)",
        s->st_games, s->st_puma, s->st_dogs, s->st_draw, s->st_abort, s->peak_active, s->max_tables);
    for (int i = 0; i < s->ntables; i++) table_destroy(s->tables[i]);
    for (int i = 0; i < s->nplayers; i++) {
        pthread_cond_destroy(&s->players[i]->cv);
        pthread_mutex_destroy(&s->players[i]->mtx);
        free(s->players[i]);
    }
    free(s->tables); free(s->players);
    pthread_cond_destroy(&s->queue_cv); pthread_cond_destroy(&s->done_cv);
    pthread_cond_destroy(&s->active_cv); pthread_mutex_destroy(&s->mtx);
    sem_destroy(&s->slots);
    free(s);
}
