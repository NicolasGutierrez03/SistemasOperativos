/* matchmaker.h - servidor Matchmaker, jugadores, mesas y Juez. */
#ifndef MATCHMAKER_H
#define MATCHMAKER_H
#include "common.h"
#include "board.h"

typedef enum { ROLE_NONE, ROLE_PUMA, ROLE_DOGS } Role;
typedef enum { PS_NEW, PS_QUEUED, PS_PLAYING, PS_DONE } PState;
typedef enum { RES_NONE, RES_WIN, RES_LOSE, RES_DRAW, RES_UNPAIRED, RES_CANCELLED } Result;
typedef enum { OUT_NONE, OUT_PUMA_WINS, OUT_DOGS_WIN, OUT_DRAW, OUT_ABORT } Outcome;

struct Server;
struct Table;

typedef struct Player {
    int id;
    Role role;
    PState state;                 /* protegido por mtx */
    Result result;
    atomic_int disconnected;      /* 1 = el cliente se cayó */
    pthread_mutex_t mtx;
    pthread_cond_t cv;            /* el hilo jugador espera pasivamente aquí a que termine su partida */
    pthread_t tid;
    bool started;
    unsigned seed;
    struct Server *srv;
    struct Table *table;          /* protegido por srv->mtx */
    struct Player *next;          /* cola de espera (lista enlazada) */
} Player;

typedef struct { struct Table *t; int piece; } PieceArg;

typedef struct Table {
    int id;
    struct Server *srv;
    Player *puma_p, *dog_p;
    Config cfg;
    Board board;
    pthread_t judge;
    bool judge_started;           /* protegido por srv->mtx */
    bool finished;                /* protegido por srv->mtx */
    pthread_mutex_t ev_mtx;       /* protege dirty/outcome y sirve de mutex a las condvars */
    pthread_cond_t judge_cv;      /* despierta al Juez (jugada hecha / abandono / abort) */
    pthread_cond_t sleep_cv;      /* despierta a las fichas dormidas cuando la partida acaba */
    int dirty;
    atomic_int over;
    Outcome outcome;
    const char *reason;
    pthread_t piece_th[NUM_DOGS + 1];
    PieceArg pa[NUM_DOGS + 1];
} Table;

typedef struct Server {
    int max_tables;
    sem_t slots;                  /* M permisos: tope estricto de mesas concurrentes */
    pthread_mutex_t mtx;          /* protege cola, contadores, arrays */
    pthread_cond_t queue_cv, done_cv, active_cv;
    Player **players; int nplayers, pcap;
    Player *qhead, *qtail; int qcount;
    int expected, enqueued, finished;
    bool shutdown, closing;
    Table **tables; int ntables, tcap;
    int active, peak_active;
    int st_games, st_puma, st_dogs, st_draw, st_abort;
    Config cfg;
    unsigned seed;
    pthread_t mm;
} Server;

Server *server_create(int max_tables, const Config *cfg);
int     server_spawn(Server *s, int count);
void    server_wait_done(Server *s);
int     server_disconnect_puma(Server *s);
int     server_disconnect_player(Server *s, int pid);
void    server_shutdown(Server *s);          /* idempotente, seguro desde el hilo vigilante */
void    server_destroy(Server *s);
void    finish_player(Server *s, Player *p, Result r);   /* NO llamar con s->mtx tomado */

Table  *table_create(Server *s, int id, Player *puma, Player *dogs);
void    table_destroy(Table *t);
void   *table_judge_main(void *arg);
void    table_poke(Table *t);
void    table_abort(Table *t, const char *reason);

#endif
