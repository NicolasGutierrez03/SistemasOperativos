#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <signal.h>
#include <time.h>
#include <stdbool.h>
#include <ctype.h>
#include <sys/resource.h>

#define MAX_NODES 10005
#define MAX_DEPS 100
#define MAX_LINE 2048

typedef enum { WAITING, RUNNING, FINISHED, FAILED, CANCELED } State;

typedef struct {
    char id[32];
    char name[128];
    int time_ms;
    
    char deps_str[MAX_DEPS][32];
    int deps_indices[MAX_DEPS];
    int num_deps;
    
    int num_dependents; // Cuántos nodos dependen de este
    int deps_started;   // Cuántos dependientes han iniciado (para cerrar pipes)
    
    State state;
    pid_t pid;
    int pipe_fd[2]; // Pipe para enviar el mensaje a sus dependientes
} Activity;

Activity dag[MAX_NODES];
int total_nodes = 0;
volatile sig_atomic_t seremi_llegada = 0;

// Manejador de señal SIGINT (Ctrl+C)
void handle_sigint(int sig) {
    (void)sig;
    seremi_llegada = 1;
}

// Función para limpiar espacios al inicio y final
char* trim(char* str) {
    if (!str) return NULL;
    while (isspace((unsigned char)*str)) str++;
    if (*str == 0) return str;
    char* end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return str;
}

// Aumentar el límite de File Descriptors para la carga de estrés (10000 nodos)
void setup_limits() {
    struct rlimit rl;
    getrlimit(RLIMIT_NOFILE, &rl);
    rl.rlim_cur = rl.rlim_max; 
    setrlimit(RLIMIT_NOFILE, &rl);
}

// Parsear el archivo de texto
void parse_plan(const char* filename) {
    FILE* f = fopen(filename, "r");
    if (!f) {
        perror("Error abriendo el archivo");
        exit(EXIT_FAILURE);
    }

    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f)) {
        if (trim(line)[0] == '\0') continue;

        char* p_id = line;
        char* p_name = strchr(p_id, ':');
        if (!p_name) continue;
        *p_name = '\0'; p_name++;

        char* p_time = strchr(p_name, ':');
        if (!p_time) continue;
        *p_time = '\0'; p_time++;

        char* p_deps = strchr(p_time, ':');
        if (p_deps) {
            *p_deps = '\0'; p_deps++;
        }

        Activity* a = &dag[total_nodes];
        strncpy(a->id, trim(p_id), 31);
        strncpy(a->name, trim(p_name), 127);
        
        char* time_str = trim(p_time);
        if (strlen(time_str) == 0) {
            a->time_ms = (rand() % 4901) + 100; // 100 a 5000
        } else {
            a->time_ms = atoi(time_str);
        }

        a->num_deps = 0;
        a->num_dependents = 0;
        a->deps_started = 0;
        a->state = WAITING;

        if (p_deps) {
            char* token = strtok(p_deps, ",");
            while (token) {
                strncpy(a->deps_str[a->num_deps++], trim(token), 31);
                token = strtok(NULL, ",");
            }
        }
        total_nodes++;
    }
    fclose(f);
}

// Enlazar dependencias para obtener índices en el arreglo
void build_dependencies() {
    for (int i = 0; i < total_nodes; i++) {
        for (int d = 0; d < dag[i].num_deps; d++) {
            int found = -1;
            for (int j = 0; j < total_nodes; j++) {
                if (strcmp(dag[i].deps_str[d], dag[j].id) == 0) {
                    found = j;
                    dag[j].num_dependents++;
                    break;
                }
            }
            dag[i].deps_indices[d] = found;
        }
    }
}

// Propagar cancelación en caso de error
void cancel_dependents(int failed_idx) {
    for (int i = 0; i < total_nodes; i++) {
        if (dag[i].state == WAITING || dag[i].state == CANCELED) {
            for (int d = 0; d < dag[i].num_deps; d++) {
                if (dag[i].deps_indices[d] == failed_idx) {
                    if (dag[i].state != CANCELED) {
                        dag[i].state = CANCELED;
                        printf("[Error Aislado] Abortando rama dependiente: %s (%s)\n", dag[i].id, dag[i].name);
                        cancel_dependents(i); // Recursivo
                    }
                }
            }
        }
    }
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Uso: %s plan.txt K\n", argv[0]);
        return EXIT_FAILURE;
    }

    srand(time(NULL));
    setup_limits();

    int K = atoi(argv[2]);
    if (K <= 0) K = 1;

    parse_plan(argv[1]);
    build_dependencies();

    struct sigaction sa;
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    int running_count = 0;
    int completed_count = 0;

    // Crear tuberías solo para nodos que tienen dependientes
    for (int i = 0; i < total_nodes; i++) {
        if (dag[i].num_dependents > 0) {
            if (pipe(dag[i].pipe_fd) == -1) {
                perror("pipe");
                exit(EXIT_FAILURE);
            }
        }
    }

    printf("Iniciando Planificador Dieciochero con K=%d\n", K);

    while (completed_count < total_nodes && !seremi_llegada) {
        bool started_any = false;

        // Intentar lanzar procesos nuevos si hay cupo
        for (int i = 0; i < total_nodes && running_count < K && !seremi_llegada; i++) {
            if (dag[i].state == WAITING) {
                bool can_start = true;
                // Verificar que todas las dependencias estén FINISHED
                for (int d = 0; d < dag[i].num_deps; d++) {
                    int dep_idx = dag[i].deps_indices[d];
                    if (dep_idx != -1 && dag[dep_idx].state != FINISHED) {
                        can_start = false;
                        break;
                    }
                }

                if (can_start) {
                    pid_t pid = fork();
                    if (pid == 0) {
                        // CÓDIGO DEL HIJO
                        
                        // 1. Leer mensajes de las dependencias
                        for (int d = 0; d < dag[i].num_deps; d++) {
                            int dep_idx = dag[i].deps_indices[d];
                            if (dep_idx != -1) {
                                char buf[128];
                                // Leer exactamente un mensaje del pipe de la dependencia
                                read(dag[dep_idx].pipe_fd[0], buf, sizeof(buf));
                            }
                        }

                        // 2. Simular trabajo
                        printf("-> [Iniciando] %s: %s (%d ms)\n", dag[i].id, dag[i].name, dag[i].time_ms);
                        usleep(dag[i].time_ms * 1000); // usleep usa microsegundos

                        // 3. Enviar mensaje a los dependientes
                        if (dag[i].num_dependents > 0) {
                            char msg[128];
                            snprintf(msg, sizeof(msg), "Insumo de %s completado.", dag[i].id);
                            // Escribir N mensajes, uno para cada dependiente futuro
                            for (int k = 0; k < dag[i].num_dependents; k++) {
                                write(dag[i].pipe_fd[1], msg, sizeof(msg));
                            }
                        }
                        
                        exit(EXIT_SUCCESS);
                    } else if (pid > 0) {
                        // CÓDIGO DEL PADRE
                        dag[i].pid = pid;
                        dag[i].state = RUNNING;
                        running_count++;
                        started_any = true;

                        // Registrar que este dependiente ya leyó su dependencia
                        for (int d = 0; d < dag[i].num_deps; d++) {
                            int dep_idx = dag[i].deps_indices[d];
                            if (dep_idx != -1) {
                                dag[dep_idx].deps_started++;
                                // Si todos los dependientes ya iniciaron, cerrar el extremo de lectura en el padre
                                // para evitar agotar FDs en pruebas de estrés.
                                if (dag[dep_idx].deps_started == dag[dep_idx].num_dependents) {
                                    close(dag[dep_idx].pipe_fd[0]);
                                    close(dag[dep_idx].pipe_fd[1]);
                                }
                            }
                        }
                    }
                }
            }
        }

        // Si llegamos al límite K, o no pudimos iniciar nada, debemos esperar a que termine algún hijo
        // Evitamos usar busy-waiting usando wait() que bloquea el proceso padre hasta que un hijo termine.
        if (running_count == K || (!started_any && running_count > 0)) {
            int status;
            pid_t p = wait(&status);
            if (p > 0) {
                running_count--;
                completed_count++;
                
                for (int i = 0; i < total_nodes; i++) {
                    if (dag[i].pid == p) {
                        if (WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS) {
                            dag[i].state = FINISHED;
                            printf("<- [Finalizado] %s: %s\n", dag[i].id, dag[i].name);
                        } else {
                            dag[i].state = FAILED;
                            printf("XX [Fallo] %s: %s finalizó con error.\n", dag[i].id, dag[i].name);
                            cancel_dependents(i);
                        }
                        break;
                    }
                }
            }
        } else if (running_count == 0 && !started_any) {
            // Deadlock o fin por cancelaciones
            break;
        }
    }

    if (seremi_llegada) {
        printf("\n¡Ha llegado la Seremi (SIGINT)! Abortando todas las actividades...\n");
        for (int i = 0; i < total_nodes; i++) {
            if (dag[i].state == RUNNING) {
                kill(dag[i].pid, SIGKILL);
            }
        }
        // Esperar a que mueran
        while (wait(NULL) > 0);
    }

    printf("Planificación finalizada.\n");
    return EXIT_SUCCESS;
}