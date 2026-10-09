/*
 * ============================================================================
 * Tarea 3 - Mr. Meeseeks
 * Curso:       Sistemas Operativos
 * Estudiante:  Sebastian Guillen Guzman    
 * Carne:       2022123818
 * Fecha:       15 de Octubre 2026
 * Compilacion: make
 * Ejecucion:   ./meeseeks
 *
 * Descripcion:
 *   Programa Box de Mr. Meeseeks. Recibe solicitudes del usuario y crea
 *   procesos Meeseeks (fork) para atenderlas. Tipos de solicitud:
 *     - Texto libre (simulacion con dificultad 0 a 100)
 *     - Calculo aritmetico o logico            (prefijo "calc:")
 *     - Ejecucion de un programa externo       (prefijo "run:")
 *   Escribir "salir" para despedirse y ver el informe final.
 *
 * Estado del desarrollo:
 *   Paso 1: esqueleto del Box, lectura de solicitudes y estadisticas.
 * ============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#define MAX_REQ     256   /* largo maximo de una solicitud */
#define MAX_TASKS   1024  /* maximo de solicitudes por sesion */

/* Tipos de solicitud que puede recibir el Box */
typedef enum {
    REQ_TEXT = 0,
    REQ_CALC = 1,
    REQ_PROG = 2
} req_type_t;

/* Registro de cada tarea para el informe final */
typedef struct {
    char       request[MAX_REQ];
    req_type_t type;
    double     duration;    /* segundos reales de ejecucion */
    int        completed;   /* 1 = terminada, 0 = no */
    int        meeseeks;    /* cantidad de Meeseeks creados */
} task_t;

static task_t tasks[MAX_TASKS];
static int    task_count = 0;

/* ---------------------------------------------------------------------------
 * now_seconds: tiempo monotono en segundos (para medir duraciones reales)
 * ------------------------------------------------------------------------- */
static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ---------------------------------------------------------------------------
 * classify_request: decide el tipo segun el prefijo y deja en *payload
 * el texto sin prefijo.
 * ------------------------------------------------------------------------- */
static req_type_t classify_request(const char *line, const char **payload)
{
    if (strncmp(line, "calc:", 5) == 0) {
        *payload = line + 5;
        return REQ_CALC;
    }
    if (strncmp(line, "run:", 4) == 0) {
        *payload = line + 4;
        return REQ_PROG;
    }
    *payload = line;
    return REQ_TEXT;
}

/* ---------------------------------------------------------------------------
 * handle_request: punto de entrada para atender una solicitud.
 * Por ahora es un stub; en los siguientes pasos hara fork del Meeseeks.
 * Devuelve 1 si la tarea se completo, 0 si no.
 * ------------------------------------------------------------------------- */
static int handle_request(req_type_t type, const char *payload, int *meeseeks_created)
{
    (void)type;
    printf("[Box %d] (stub) atendiendo: %s\n", getpid(), payload);
    *meeseeks_created = 0;
    return 1;
}

/* ---------------------------------------------------------------------------
 * print_report: informe final al cerrar el Box
 * ------------------------------------------------------------------------- */
static void print_report(void)
{
    int done = 0;
    printf("\n========== INFORME FINAL ==========\n");
    printf("Tareas solicitadas: %d\n", task_count);
    for (int i = 0; i < task_count; i++) {
        printf("%3d. \"%s\" | duracion: %.3f s | %s | Meeseeks: %d\n",
               i + 1, tasks[i].request, tasks[i].duration,
               tasks[i].completed ? "TERMINADA" : "NO TERMINADA",
               tasks[i].meeseeks);
        if (tasks[i].completed) done++;
    }
    printf("Terminadas: %d | No terminadas: %d\n", done, task_count - done);
    printf("===================================\n");
}

int main(void)
{
    char line[MAX_REQ];

    printf("Box de Mr. Meeseeks (pid %d)\n", getpid());
    printf("Escriba una solicitud, 'calc:<expresion>', 'run:<programa>' o 'salir'.\n");

    while (1) {
        printf("\nBox> ");
        fflush(stdout);
        if (fgets(line, sizeof(line), stdin) == NULL)
            break;
        line[strcspn(line, "\n")] = '\0';

        if (line[0] == '\0')
            continue;
        if (strcmp(line, "salir") == 0)
            break;
        if (task_count >= MAX_TASKS) {
            printf("Limite de tareas alcanzado.\n");
            break;
        }

        const char *payload;
        req_type_t type = classify_request(line, &payload);

        task_t *t = &tasks[task_count];
        strncpy(t->request, line, MAX_REQ - 1);
        t->type = type;

        double start = now_seconds();
        t->completed = handle_request(type, payload, &t->meeseeks);
        t->duration = now_seconds() - start;

        task_count++;
    }

    print_report();
    return 0;
}
