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
#include <sys/types.h>
#include <sys/wait.h>
 
#define MAX_ARGS    32    /* maximo de argumentos de un programa externo */
 
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
 * meeseeks_greet: mensaje de bienvenida obligatorio de cada Meeseeks.
 * level = N (nivel), instance = i (numero de instancia).
 * ------------------------------------------------------------------------- */
static void meeseeks_greet(int level, int instance)
{
    printf("Hi, I'm Mr. Meeseeks! Look at Meeeee. (%d, %d, %d, %d)\n",
           getpid(), getppid(), level, instance);
    fflush(stdout);
}
 
/* ---------------------------------------------------------------------------
 * meeseeks_say: todo mensaje de un Meeseeks al usuario lleva pid y ppid.
 * ------------------------------------------------------------------------- */
static void meeseeks_say(const char *msg)
{
    printf("[pid %d, ppid %d] %s\n", getpid(), getppid(), msg);
    fflush(stdout);
}
 
/* ---------------------------------------------------------------------------
 * split_args: parte una linea en palabras separadas por espacios.
 * Modifica 'line'. Llena argv terminado en NULL. Devuelve la cantidad.
 * ------------------------------------------------------------------------- */
static int split_args(char *line, char **argv, int max)
{
    int n = 0;
    char *tok = strtok(line, " \t");
    while (tok != NULL && n < max - 1) {
        argv[n++] = tok;
        tok = strtok(NULL, " \t");
    }
    argv[n] = NULL;
    return n;
}
 
/* ---------------------------------------------------------------------------
 * meeseeks_run_program: cuerpo del Meeseeks que atiende un "run:".
 * Crea un proceso trabajador que ejecuta el programa externo (exec) y espera
 * su codigo de retorno. Solo importa si termino con exito (0) o no.
 * Termina con exit 0 si la tarea se completo y exit 1 si no.
 * ------------------------------------------------------------------------- */
static void meeseeks_run_program(const char *payload)
{
    char  buf[MAX_REQ];
    char *argv[MAX_ARGS];
    char  msg[MAX_REQ + 64];
 
    meeseeks_greet(1, 1);
 
    strncpy(buf, payload, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    if (split_args(buf, argv, MAX_ARGS) == 0) {
        meeseeks_say("No me pidieron ningun programa. No puedo ayudar.");
        _exit(1);
    }
 
    snprintf(msg, sizeof(msg), "Voy a ejecutar el programa: %s", argv[0]);
    meeseeks_say(msg);
 
    fflush(stdout);
    pid_t worker = fork();
    if (worker < 0) {
        perror("fork");
        _exit(1);
    }
    if (worker == 0) {
        /* Trabajador: se convierte en el programa externo */
        execvp(argv[0], argv);
        perror("execvp");
        _exit(127);
    }
 
    /* Meeseeks: espera el resultado del programa externo */
    int status = 0;
    if (waitpid(worker, &status, 0) < 0) {
        perror("waitpid");
        _exit(1);
    }
 
    int ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (WIFEXITED(status))
        snprintf(msg, sizeof(msg), "El programa termino con codigo %d.",
                 WEXITSTATUS(status));
    else if (WIFSIGNALED(status))
        snprintf(msg, sizeof(msg), "El programa fue terminado por la senal %d.",
                 WTERMSIG(status));
    else
        snprintf(msg, sizeof(msg), "El programa termino de forma anormal.");
    meeseeks_say(msg);
 
    if (ok)
        meeseeks_say("Solicitud completada. Gracias por dejarme servirle. Adios!");
    else
        meeseeks_say("No pude completar la solicitud. Adios!");
    _exit(ok ? 0 : 1);
}
 
/* ---------------------------------------------------------------------------
 * handle_request: punto de entrada para atender una solicitud.
 * Devuelve 1 si la tarea se completo, 0 si no.
 * ------------------------------------------------------------------------- */
static int handle_request(req_type_t type, const char *payload, int *meeseeks_created)
{
    if (type != REQ_PROG) {
        /* Texto y calculos se implementan en pasos posteriores */
        printf("[Box %d] (pendiente) tipo %d: %s\n", getpid(), type, payload);
        *meeseeks_created = 0;
        return 1;
    }
 
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        *meeseeks_created = 0;
        return 0;
    }
    if (pid == 0) {
        meeseeks_run_program(payload);   /* no regresa */
        _exit(1);
    }
 
    *meeseeks_created = 1;
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
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