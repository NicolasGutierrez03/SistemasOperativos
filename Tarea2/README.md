# Torneo de Komikan Concurrente

Tarea 2 de Sistemas Operativos: servidor Matchmaker, mesas de juego con árbitro (Juez) y fichas concurrentes en tiempo real, escrito en C17 con primitivas POSIX (`pthread_mutex`, `pthread_cond`, `sem_t`).

## Compilar

```bash
gcc -Wall -Wextra -std=c17 -pthread *.c -o torneo
```

Todos los `.c` y `.h` deben estar en la misma carpeta. Requiere Linux y gcc.

## Ejecutar

```bash
./torneo N M [config.txt]
```

- `N`: número de jugadores (clientes) a procesar.
- `M`: máximo de mesas activas al mismo tiempo.
- `config.txt` (opcional): archivo de casos de prueba.

Ejemplos:

```bash
./torneo 10 3                    # 10 jugadores, 3 mesas, partidas aleatorias
./torneo 4 2 test_cases.txt   # ejecuta los casos de prueba del enunciado
```

Variables de entorno opcionales:

| Variable | Efecto |
|---|---|
| `TORNEO_VERBOSE=1` | Registra cada jugada y el tablero en cada una |
| `TORNEO_LOG=archivo` | Además de la consola, guarda el log en un archivo |

Con Ctrl+C el programa detiene el ingreso de jugadores, cancela las partidas activas, libera todos los recursos y termina con código 0.

## Estructura

| Archivo | Contenido |
|---|---|
| `main.c` | Argumentos y arranque |
| `common.c/h` | Configuración, log con mutex, RNG, manejo de SIGINT (handler + hilo vigilante) |
| `board.c/h` | Tablero, adyacencias, saltos, mutex por nodo, movimientos atómicos |
| `matchmaker.c/h` | Jugadores, cola pasiva, emparejamiento, semáforo de M mesas, cierre ordenado |
| `table.c` | Mesa: hilo Juez e hilos de fichas (1 Puma + 12 Perros) |
| `parser.c/h` | Lectura y ejecución del archivo de casos de prueba |
| `test_cases.txt` | Casos de prueba entregados con la tarea |
| `Informe_Tarea2_SO.pdf` | Informe (diseño, riesgos de concurrencia, evaluación) |

## Diseño en breve

- **Tablero:** 28 nodos (cuadrícula 5×5 con aristas horizontales y verticales, más el trono con `TRONO_TOP`, `TRONO_LEFT`, `TRONO_RIGHT`). Cada nodo tiene su propio mutex y los locks se toman **siempre en orden ascendente de id**, lo que evita deadlock. Un movimiento valida y escribe bajo los locks de origen, destino y (si hay captura) casilla saltada.
- **Tiempo real:** cada ficha es un hilo que espera un retardo aleatorio (50–300 ms por defecto) con `pthread_cond_timedwait` y luego intenta moverse. Si otra ficha gana la casilla, la jugada se rechaza y reintenta en el siguiente ciclo.
- **Matchmaker:** cola de jugadores con mutex y variable de condición; un `sem_t` inicializado en `M` limita las mesas concurrentes. Las esperas son pasivas (sin busy-waiting).
- **Juez:** un hilo por mesa. Duerme hasta que una ficha notifica una jugada; detecta victoria (6 capturas, Puma acorralado), abandono o tope de 300 jugadas (empate), y registra el tablero cada 10 jugadas.
- **Resiliencia:** una desconexión da la victoria al rival por abandono. En SIGINT el handler (`sigaction`) solo escribe en un pipe; un hilo vigilante hace el cierre ordenado.

## Convenciones del archivo de pruebas

- Coordenadas `(x, y)`: `x` = columna (0–4, izquierda a derecha), `y` = fila (0 abajo, 4 arriba junto al trono). El trono se indica con `TRONO_TOP`, `TRONO_LEFT`, `TRONO_RIGHT`.
- Ids de perro: `0` a `11`.
- Disposición inicial por defecto: perros 0–4 en `y=0`, 5–9 en `y=1`, perro 10 en `(0,2)` y perro 11 en `(4,2)`; el Puma en `PUMA_INIT` (por defecto `(2,2)`).
- `EVENT MOVE` es una **solicitud** que el Juez valida: si es ilegal se rechaza y se registra como `MOVE RECHAZADA`, sin hacer fallar el caso. Opcionalmente se puede agregar `EXPECT ACCEPT` o `EXPECT REJECT` al final para verificar el resultado.
- `EVENT DISCONNECT` sin `SPAWN_PLAYERS` previo actúa sobre la mesa de prueba: `PLAYER_PUMA` da la victoria a los Perros y `PLAYER_DOG` al Puma, por abandono.
- `EVENT SIGNAL_SIGINT` simula Ctrl+C: cierra el servidor del caso actual y la suite continúa. Un Ctrl+C real detiene toda la ejecución.

Comandos soportados: `SUITE_START`, `SUITE_END`, `TEST_CASE`, `TEST_END`, `CONFIG` (`SEED`, `DELAY_MIN`, `DELAY_MAX`, `PUMA_INIT`) y `EVENT` (`MOVE`, `SPAWN_PLAYERS`, `WAIT_QUEUED_PLAYERS`, `DISCONNECT`, `SIGNAL_SIGINT`, `BURST_RANDOM_MOVES`).

## Supuestos

- Las conexiones del trono siguen nuestra lectura de la figura del enunciado: `TRONO_TOP` con `TRONO_LEFT`, `TRONO_RIGHT` y la casilla `(2,4)`; los nodos laterales entre sí, con las esquinas `(0,4)` y `(4,4)` y con `(1,4)` y `(3,4)`.
- El tope de 300 jugadas por partida (empate) es una decisión propia.
- El archivo de pruebas no trae resultados esperados: se verifica que cada caso se ejecute sin fallos y que el Juez rechace lo ilegal.
