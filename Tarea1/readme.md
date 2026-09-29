# Planificador Dieciochero - Sistemas Operativos

Este repositorio contiene la solución para el Planificador Dieciochero, modelando las actividades como un Grafo Acíclico Dirigido (DAG)[cite: 2]. Todo el código fuente y las explicaciones requeridas sobre su diseño se encuentran detallados a continuación[cite: 1].

## Cómo compilar y ejecutar

El programa está escrito en C y cumple con la directiva de compilación estricta requerida para la entrega[cite: 1].

Para compilar, ejecute el siguiente comando en la terminal:
`gcc -Wall -Wextra -std=c17 -lpthread -o planificador planificador.c`[cite: 1]

Para ejecutar el simulador, utilice el siguiente formato de invocación, donde `K` es el límite de concurrencia:
`./planificador plan.txt K`[cite: 2]

## Funciones Implementadas

*   `parse_plan`: Lee el archivo de texto plano para modelar matemáticamente los días como un DAG, asignando tiempos aleatorios si corresponde[cite: 2].
*   `build_dependencies`: Enlaza las dependencias de cada actividad procesando los IDs separados por comas[cite: 2].
*   `cancel_dependents`: Implementa el aislamiento de errores abortando únicamente la rama del plan que dependía de una actividad fallida de forma recursiva[cite: 2].
*   `handle_sigint`: Captura la señal de inspección de la Seremi (Ctrl+C) para abortar inmediatamente todas las actividades[cite: 2].

## Decisiones de Diseño y Justificación

*   **Procesos vs Hilos**: Se utilizó exclusivamente la llamada al sistema `fork()` para la creación de procesos concurrentes, acatando la prohibición estricta de usar hilos o sus respectivos mecanismos de sincronización[cite: 1, 2].
*   **Paso de Mensajes**: La comunicación de notificaciones de insumos hacia las actividades dependientes se resolvió mediante tuberías (`pipes`), enviando un mensaje de texto acotado[cite: 1, 2]. 
*   **Control de Concurrencia**: Para lograr el control de concurrencia sin incurrir en *busy-waiting* ni en *race conditions*, el proceso padre coordina el límite `K` delegando el bloqueo de manera segura en la llamada de sistema `wait()`[cite: 1].
*   **Carga de Estrés**: Para soportar las pruebas de estrés que cargan planificaciones de hasta 10000 actividades, se implementó el uso de `setrlimit` para elevar los *File Descriptors* del sistema y se cierran proactivamente los extremos de los *pipes* que ya no se utilizan[cite: 2].