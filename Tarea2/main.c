/* main.c - ./torneo N M [config.txt] */
#include "common.h"
#include "matchmaker.h"
#include "parser.h"
#include <errno.h>

static int parse_pos(const char *s, int max) {
    char *end; errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || end == s || *end || v < 1 || v > max) return -1;
    return (int)v;
}

int main(int argc, char **argv) {
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "Uso: %s N M [config.txt]\n  N: jugadores, M: mesas concurrentes maximas\n", argv[0]);
        return 1;
    }
    int n = parse_pos(argv[1], 100000), m = parse_pos(argv[2], 10000);
    if (n < 0 || m < 0) { fprintf(stderr, "N y M deben ser enteros positivos (N<=100000, M<=10000)\n"); return 1; }

    log_init();
    if (sig_start() != 0) { fprintf(stderr, "No se pudo instalar el manejador de SIGINT\n"); return 1; }

    int rc = 0;
    if (argc == 4) {
        rc = run_config(argv[3], n, m) == 0 ? 0 : 1;
    } else {
        Config cfg; cfg_default(&cfg);
        cfg.seed = (unsigned)time(NULL);
        Server *s = server_create(m, &cfg);
        if (!s) { fprintf(stderr, "No se pudo crear el servidor\n"); rc = 1; }
        else {
            server_spawn(s, n);
            server_destroy(s);          /* espera a que todos los jugadores terminen (o SIGINT) */
        }
        if (atomic_load(&g_shutdown)) LOG("Ejecucion interrumpida por SIGINT; recursos liberados");
    }
    sig_stop();
    log_close();
    return rc;
}
