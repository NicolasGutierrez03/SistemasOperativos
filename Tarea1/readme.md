# Planificador Dieciochero - Sistemas Operativos

Este repositorio contiene la solución para el Planificador Dieciochero, modelando las actividades como un Grafo Acíclico Dirigido (DAG). Todo el código fuente y las explicaciones requeridas sobre su diseño se encuentran detallados a continuación.

## Cómo compilar y ejecutar

El programa está escrito en C y cumple con la directiva de compilación estricta requerida para la entrega.

Para compilar, ejecute el siguiente comando en la terminal:
`gcc -Wall -Wextra -std=c17 -lpthread -o planificador planificador.c`

Para ejecutar el simulador, utilice el siguiente formato de invocación, donde `K` es el límite de concurrencia:
`./planificador plan.txt K`
*(Ejemplo: `./planificador plan.txt 4`)*

## Funciones Implementadas

*   `parse_plan`: Lee el archivo de texto plano para modelar matemáticamente los días como un DAG, asignando tiempos aleatorios si corresponde.
*   `build_dependencies`: Enlaza las dependencias de cada actividad procesando los IDs separados por comas.
*   `cancel_dependents`: Implementa el aislamiento de errores abortando únicamente la rama del plan que dependía de una actividad fallida de forma recursiva.
*   `handle_sigint`: Captura la señal de inspección de la Seremi (Ctrl+C) para abortar inmediatamente todas las actividades.

## Decisiones de Diseño y Justificación

*   **Procesos vs Hilos**: Se utilizó exclusivamente la llamada al sistema `fork()` para la creación de procesos concurrentes, acatando la prohibición estricta de usar hilos o sus respectivos mecanismos de sincronización.
*   **Paso de Mensajes**: La comunicación de notificaciones de insumos hacia las actividades dependientes se resolvió mediante tuberías (`pipes`), enviando un mensaje de texto acotado. 
*   **Control de Concurrencia**: Para lograr el control de concurrencia sin incurrir en *busy-waiting* ni en *race conditions*, el proceso padre coordina el límite `K` delegando el bloqueo de manera segura en la llamada de sistema `wait()`.
*   **Carga de Estrés**: Para soportar las pruebas de estrés que cargan planificaciones de hasta 10000 actividades, se implementó el uso de `setrlimit` para elevar los *File Descriptors* del sistema y se cierran proactivamente los extremos de los *pipes* que ya no se utilizan.

## Verificación y Pruebas

### Comprobación del Paso de Mensajes (Pipes)
Para visualizar claramente en la consola la comunicación entre procesos, el código incluye una impresión explícita justo después de que un proceso lee exitosamente el pipe de su predecesor:
`printf("   [Mensaje recibido por %s] %s\n", dag[i].id, buf);`
Esto demuestra en tiempo de ejecución que el insumo ha sido notificado a la actividad dependiente antes de iniciar su simulación.

### Simulación de Falla Real (Aislamiento de Errores)
Para comprobar el requisito de tolerancia a fallos sin modificar el código fuente, se puede simular un error catastrófico abortando un proceso hijo directamente desde el sistema operativo:

1. Ejecute el planificador con un archivo `plan.txt` que contenga una tarea de larga duración (ej. 30000 ms).
2. Abra una segunda terminal y busque el árbol de procesos en ejecución con el comando:
   `ps xf | grep planificador`
3. Identifique el identificador de proceso (PID) de uno de los procesos hijos (se muestran debajo del padre con el símbolo `\_`) y mátelo enviando la señal `SIGKILL`:
   `kill -9 <PID>`
4. En la terminal original, el proceso padre detectará la muerte anormal del hijo (status != 0) y activará el aislamiento: abortará únicamente la rama de actividades que dependía de ese proceso fallido y permitirá que el resto de las tareas independientes sigan corriendo hasta finalizar con éxito.