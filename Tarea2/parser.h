/* parser.h - parser/ejecutor de archivos de casos de prueba. */
#ifndef PARSER_H
#define PARSER_H
#include "common.h"
/* Ejecuta el archivo. default_n / default_m se usan si SPAWN_PLAYERS omite COUNT / MESAS_MAX.
 * Devuelve 0 si pudo ejecutarse (aunque algún caso falle), -1 si no se pudo abrir. */
int run_config(const char *path, int default_n, int default_m);
#endif
