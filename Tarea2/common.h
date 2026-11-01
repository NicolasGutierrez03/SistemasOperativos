/* common.h - constantes, configuración, log y manejo de SIGINT.
 * IMPORTANTE: debe ser el PRIMER include de cada .c (define _POSIX_C_SOURCE). */
#ifndef COMMON_H
#define COMMON_H

#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NUM_DOGS          12
#define WIN_CAPTURES      6
#define DEFAULT_DELAY_MIN 50
#define DEFAULT_DELAY_MAX 300
#define MAX_TABLE_MOVES   300   /* tope de jugadas por mesa -> empate (evita partidas infinitas) */

typedef struct {
    unsigned seed;
    int delay_min, delay_max;   /* ms */
    int puma_init;              /* id de nodo (ver board.h) */
} Config;

void cfg_default(Config *c);

extern atomic_int g_shutdown;   /* 1 tras SIGINT */
extern int g_verbose;           /* TORNEO_VERBOSE=1 -> loguea cada jugada */

void log_init(void);
void log_close(void);
void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define LOG(...) logf_(__VA_ARGS__)

int  rng_range(unsigned *s, int lo, int hi);   /* rand_r, reentrante */
void sleep_ms(int ms);
int  cond_init_mono(pthread_cond_t *cv);       /* condvar con reloj monotónico */
void deadline_in_ms(struct timespec *ts, int ms);

/* --- SIGINT: handler (sigaction) async-signal-safe + hilo vigilante (self-pipe) --- */
int  sig_start(void);
void sig_stop(void);
void sig_set_server(void (*cb)(void *), void *arg);  /* registra/desregistra callback de cierre */
void sig_reset(void);                                /* limpia el estado de SIGINT simulado (entre casos de prueba) */
void sig_wait_shutdown(void);                        /* bloquea hasta que el vigilante terminó el cierre */

#endif
