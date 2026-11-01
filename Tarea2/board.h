/* board.h - tablero 5x5 + Trono (triángulo), adyacencias y movimientos atómicos.
 *
 * Coordenadas externas (config/log): (x,y) con x = columna 0..4 (izq->der) e y = fila 0..4
 * (0 abajo, 4 arriba junto al trono). Internamente cada casilla tiene id = (4-y)*5 + x, es decir
 * la fila 0 interna es la de arriba. TRONO_TOP=25, TRONO_LEFT=26, TRONO_RIGHT=27.
 * Cada nodo tiene su propio pthread_mutex_t. Regla anti-deadlock: SIEMPRE se adquieren
 * los mutex de varios nodos en orden ascendente de id. */
#ifndef BOARD_H
#define BOARD_H
#include "common.h"

#define GRID        5
#define NN          28
#define TRONO_TOP   25
#define TRONO_LEFT  26
#define TRONO_RIGHT 27
#define PUMA_ID     0          /* índice de pieza: 0 = Puma, 1..12 = Perros */
#define MAX_MOVES   24

typedef struct { int to; int over; } Move;   /* over = -1 si es movimiento simple */

typedef struct Board {
    pthread_mutex_t lock[NN];  /* un mutex por nodo */
    int occ[NN];               /* 0 libre, -1 Puma, k>0 Perro k (protegido por lock[nodo]) */
    _Atomic int pos[NUM_DOGS + 1];   /* posición de cada pieza; -1 = capturada */
    atomic_int captures;
    atomic_int moves;
} Board;

typedef enum {
    MV_OK, MV_BAD_POS, MV_NOT_ADJ, MV_OCCUPIED, MV_NO_PIECE, MV_BAD_CAPTURE
} MoveResult;

typedef struct { bool has_capture; int cap_dog; int cap_node; } MoveExpect;

int         node_from_xy(int x, int y);               /* -1 si fuera de rango */
int         node_from_keyword(const char *s);         /* TRONO_* -> id, -1 si no */
const char *node_label(int id, char *buf, size_t n);
const char *move_result_str(MoveResult r);

int         board_init(Board *b, int puma_pos);       /* 0 ok */
void        board_destroy(Board *b);
MoveResult  board_move(Board *b, int piece, int from, int to,
                       const MoveExpect *ex, int *captured_dog);
int         board_moves(Board *b, int piece, int from, Move *out);  /* -1 si la pieza ya no está en from */
void        board_dump(Board *b, char *out, size_t sz);
int         board_check(Board *b);                    /* invariantes; 0 ok */

#endif
