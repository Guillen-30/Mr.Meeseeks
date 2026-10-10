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
 *   Paso 2: solicitudes "run:" (fork + exec + waitpid, con argumentos).
 *   Paso 3: solicitudes "calc:" (programa bc mediante pipes).
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
 
/* ===========================================================================
 * CALCULOS (solicitudes "calc:")
 *
 * Se usa el programa externo "bc" (calculadora estandar de Linux), tal como
 * permite el enunciado ("escribir o buscar un pequeno programa").
 *
 * Comunicacion mediante pipes (sin pasar por el shell, para que el texto del
 * usuario nunca se interprete como comandos):
 *
 *      Meeseeks --(pipe to_bc)-->  stdin  de bc
 *      Meeseeks <-(pipe from_bc)-- stdout y stderr de bc
 *
 * Consideraciones especiales:
 *  - bc termina con codigo 0 incluso cuando hay errores, por lo que el exito
 *    se decide leyendo su salida (busca "error", "illegal" o "(standard_in)").
 *  - Se envia "scale=10" primero, para que las divisiones den decimales
 *    (por defecto bc divide como enteros: 18/74 daria 0). Los decimales se
 *    truncan (no se redondean), por eso 28+18/74*321 da 106.0810810672.
 *  - Excepcion: si la expresion contiene "%" se usa scale=0 (enteros), porque
 *    bc da resultados incorrectos con modulo cuando scale > 0.
 *  - BC_LINE_LENGTH=0 evita que bc parta en varias lineas resultados largos.
 *  - Operadores soportados por bc: + - * / % ^, comparaciones, && || !.
 *    Los logicos devuelven 1 (verdadero) o 0 (falso).
 * ========================================================================= */
 
#define MAX_CALC_OUT 1024
 
/* ---------------------------------------------------------------------------
 * run_bc: ejecuta bc con la expresion. Deja la salida (sin salto de linea
 * final) en 'out'. Devuelve 1 si hubo un resultado valido, 0 si hubo error.
 * ------------------------------------------------------------------------- */
static int run_bc(const char *expr, char *out, size_t out_size)
{
    int to_bc[2], from_bc[2];
 
    if (pipe(to_bc) < 0 || pipe(from_bc) < 0) {
        perror("pipe");
        snprintf(out, out_size, "no pude crear los pipes");
        return 0;
    }
 
    fflush(stdout);
    pid_t worker = fork();
    if (worker < 0) {
        perror("fork");
        snprintf(out, out_size, "no pude crear el proceso de calculo");
        return 0;
    }
 
    if (worker == 0) {
        /* Trabajador: stdin <- to_bc, stdout y stderr -> from_bc */
        dup2(to_bc[0], STDIN_FILENO);
        dup2(from_bc[1], STDOUT_FILENO);
        dup2(from_bc[1], STDERR_FILENO);
        close(to_bc[0]);   close(to_bc[1]);
        close(from_bc[0]); close(from_bc[1]);
        setenv("BC_LINE_LENGTH", "0", 1);
        execlp("bc", "bc", (char *)NULL);
        perror("execlp bc");
        _exit(127);
    }
 
    /* Meeseeks: manda la expresion por el pipe y cierra para que bc termine */
    close(to_bc[0]);
    close(from_bc[1]);
    /* bc calcula mal "%" cuando scale > 0 (10%3 daria .0000000001), por lo
     * que si la expresion usa modulo se trabaja con enteros (scale=0). */
    dprintf(to_bc[1], "scale=%d\n%s\n", strchr(expr, '%') ? 0 : 10, expr);
    close(to_bc[1]);
 
    /* Lee todo lo que bc responda */
    size_t used = 0;
    ssize_t n;
    while (used < out_size - 1 &&
           (n = read(from_bc[0], out + used, out_size - 1 - used)) > 0)
        used += (size_t)n;
    out[used] = '\0';
    close(from_bc[0]);
 
    int status = 0;
    waitpid(worker, &status, 0);
 
    /* Quita saltos de linea finales */
    while (used > 0 && (out[used - 1] == '\n' || out[used - 1] == '\r'))
        out[--used] = '\0';
 
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        snprintf(out, out_size, "no se pudo ejecutar bc (esta instalado?)");
        return 0;
    }
    if (used == 0) {
        snprintf(out, out_size, "la expresion no produjo ningun resultado");
        return 0;
    }
    if (strstr(out, "error") || strstr(out, "illegal") ||
        strstr(out, "(standard_in)"))
        return 0;
    return 1;
}
 
/* ---------------------------------------------------------------------------
 * meeseeks_calc: cuerpo del Meeseeks que atiende un "calc:".
 * Es una tarea sencilla: un solo Meeseeks (N=1, i=1) pide el calculo a bc y
 * reporta el resultado o el error. exit 0 = completada, exit 1 = no.
 * ------------------------------------------------------------------------- */
static void meeseeks_calc(const char *payload)
{
    char msg[MAX_CALC_OUT + 128];
    char out[MAX_CALC_OUT];
 
    meeseeks_greet(1, 1);
 
    snprintf(msg, sizeof(msg), "Voy a calcular: %s", payload);
    meeseeks_say(msg);
 
    if (!run_bc(payload, out, sizeof(out))) {
        snprintf(msg, sizeof(msg), "Error en la expresion: %s", out);
        meeseeks_say(msg);
        meeseeks_say("No pude completar la solicitud. Adios!");
        _exit(1);
    }
 
    snprintf(msg, sizeof(msg), "Resultado = %s", out);
    meeseeks_say(msg);
    meeseeks_say("Solicitud completada. Gracias por dejarme servirle. Adios!");
    _exit(0);
}
 
/* ---------------------------------------------------------------------------
 * handle_request: punto de entrada para atender una solicitud.
 * Devuelve 1 si la tarea se completo, 0 si no.
 * ------------------------------------------------------------------------- */
static int handle_request(req_type_t type, const char *payload, int *meeseeks_created)
{
    if (type == REQ_TEXT) {
        /* El texto libre (con dificultad) se implementa en pasos posteriores */
        printf("[Box %d] (pendiente) texto: %s\n", getpid(), payload);
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
        if (type == REQ_CALC)
            meeseeks_calc(payload);      /* no regresa */
        else
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