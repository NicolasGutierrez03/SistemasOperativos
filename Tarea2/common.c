#include "common.h"
#include <errno.h>
#include <stdarg.h>
#include <unistd.h>

atomic_int g_shutdown = 0;
int g_verbose = 0;

static pthread_mutex_t g_log_mtx = PTHREAD_MUTEX_INITIALIZER;
static FILE *g_logf = NULL;
static struct timespec g_t0;

void cfg_default(Config *c) {
    c->seed = 1;
    c->delay_min = DEFAULT_DELAY_MIN;
    c->delay_max = DEFAULT_DELAY_MAX;
    c->puma_init = 2 * 5 + 2;   /* casilla (2,2) */
}

void log_init(void) {
    clock_gettime(CLOCK_MONOTONIC, &g_t0);
    const char *p = getenv("TORNEO_LOG");
    if (p && *p) g_logf = fopen(p, "w");
    const char *v = getenv("TORNEO_VERBOSE");
    g_verbose = (v && *v == '1');
}

void log_close(void) {
    if (g_logf) { fclose(g_logf); g_logf = NULL; }
}

void logf_(const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    struct timespec n;
    clock_gettime(CLOCK_MONOTONIC, &n);
    double el = (double)(n.tv_sec - g_t0.tv_sec) + (double)(n.tv_nsec - g_t0.tv_nsec) / 1e9;
    /* Un solo mutex serializa la salida: líneas de hilos distintos no se mezclan */
    pthread_mutex_lock(&g_log_mtx);
    printf("[%8.3f] %s\n", el, buf);
    fflush(stdout);
    if (g_logf) { fprintf(g_logf, "[%8.3f] %s\n", el, buf); fflush(g_logf); }
    pthread_mutex_unlock(&g_log_mtx);
}

int rng_range(unsigned *s, int lo, int hi) {
    if (hi <= lo) return lo;
    return lo + (int)((unsigned)rand_r(s) % (unsigned)(hi - lo + 1));
}

void sleep_ms(int ms) {
    if (ms <= 0) return;
    struct timespec r = { ms / 1000, (long)(ms % 1000) * 1000000L };
    while (nanosleep(&r, &r) == -1 && errno == EINTR) { }
}

int cond_init_mono(pthread_cond_t *cv) {
    pthread_condattr_t a;
    if (pthread_condattr_init(&a)) return -1;
    int rc = pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    if (!rc) rc = pthread_cond_init(cv, &a);
    pthread_condattr_destroy(&a);
    return rc ? -1 : 0;
}

void deadline_in_ms(struct timespec *ts, int ms) {
    clock_gettime(CLOCK_MONOTONIC, ts);
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) { ts->tv_sec++; ts->tv_nsec -= 1000000000L; }
}

/* ===================== SIGINT ===================== */
/* El handler sólo hace write() (async-signal-safe) sobre un pipe. Un hilo vigilante
 * bloqueado en read() (sin busy-wait) ejecuta el cierre ordenado con mutex/condvars,
 * cosa imposible de hacer de forma segura dentro del handler. */
static int g_pipe[2] = { -1, -1 };
static pthread_t g_watcher;
static bool g_watcher_on = false;
static pthread_mutex_t g_reg_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_reg_cv  = PTHREAD_COND_INITIALIZER;
static void (*g_cb)(void *) = NULL;
static void *g_cb_arg = NULL;
static int g_sd_done = 0;

static void on_sigint(int sig) {
    (void)sig;
    int e = errno;
    char c = 'S';
    ssize_t r = write(g_pipe[1], &c, 1);
    (void)r;
    errno = e;
}

static void *watcher_main(void *a) {
    (void)a;
    for (;;) {
        char c;
        ssize_t r = read(g_pipe[0], &c, 1);
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (r == 0 || c == 'Q') break;
        atomic_store(&g_shutdown, 1);
        pthread_mutex_lock(&g_reg_mtx);
        if (g_cb) g_cb(g_cb_arg);
        g_sd_done = 1;
        pthread_cond_broadcast(&g_reg_cv);
        pthread_mutex_unlock(&g_reg_mtx);
    }
    return NULL;
}

int sig_start(void) {
    if (pipe(g_pipe) != 0) return -1;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGINT, &sa, NULL) != 0) return -1;
    if (pthread_create(&g_watcher, NULL, watcher_main, NULL) != 0) return -1;
    g_watcher_on = true;
    return 0;
}

void sig_stop(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    if (g_watcher_on) {
        char c = 'Q';
        ssize_t r = write(g_pipe[1], &c, 1);
        (void)r;
        pthread_join(g_watcher, NULL);
        g_watcher_on = false;
    }
    if (g_pipe[0] >= 0) close(g_pipe[0]);
    if (g_pipe[1] >= 0) close(g_pipe[1]);
    g_pipe[0] = g_pipe[1] = -1;
}

void sig_set_server(void (*cb)(void *), void *arg) {
    pthread_mutex_lock(&g_reg_mtx);   /* también espera a que termine un cierre en curso */
    g_cb = cb;
    g_cb_arg = arg;
    if (cb && atomic_load(&g_shutdown)) cb(arg);   /* SIGINT llegó antes de registrar */
    pthread_mutex_unlock(&g_reg_mtx);
}

void sig_reset(void) {
    pthread_mutex_lock(&g_reg_mtx);
    g_sd_done = 0;
    atomic_store(&g_shutdown, 0);
    pthread_mutex_unlock(&g_reg_mtx);
}

void sig_wait_shutdown(void) {
    pthread_mutex_lock(&g_reg_mtx);
    while (!g_sd_done) pthread_cond_wait(&g_reg_cv, &g_reg_mtx);
    pthread_mutex_unlock(&g_reg_mtx);
}
