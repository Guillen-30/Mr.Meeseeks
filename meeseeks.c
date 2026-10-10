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
 *   Paso 4: memoria compartida + semaforo para contadores de Meeseeks.
 *   Paso 5: texto libre con dificultad, espera simulada y caso facil (>85).
 * ============================================================================
 */
 
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <semaphore.h>
 
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
    double     duration;    /* segundos reales de ejecucion (sin espera simulada) */
    double     sim_wait;    /* segundos de espera simulada descontados */
    int        completed;   /* 1 = terminada, 0 = no */
    int        meeseeks;    /* cantidad de Meeseeks creados */
} task_t;
 
static task_t tasks[MAX_TASKS];
static int    task_count = 0;
 
/* ===========================================================================
 * MEMORIA COMPARTIDA Y SEMAFORO
 *
 * El Box crea (antes de cualquier fork) una region de memoria compartida
 * anonima con mmap(MAP_SHARED). Todos los procesos Meeseeks la heredan y ven
 * la misma memoria. Contiene los contadores globales de niveles e instancias.
 *
 * El acceso esta protegido con un semaforo POSIX (sem_t, pshared = 1) que
 * administra el sistema operativo. Cada lectura o escritura de los contadores
 * ocurre entre sem_wait() y sem_post() (exclusion mutua).
 * ========================================================================= */
 
#define MAX_LEVELS 64
 
typedef struct {
    sem_t lock;                          /* semaforo binario (mutex entre procesos) */
    int   total_created;                 /* Meeseeks creados en la solicitud actual */
    int   active;                        /* Meeseeks vivos en este momento */
    int   max_level;                     /* nivel N mas profundo alcanzado */
    double sim_wait;                     /* espera simulada (s) a descontar del tiempo real */
    int   level_count[MAX_LEVELS + 1];   /* instancias creadas por nivel */
} shared_t;
 
static shared_t *shared = NULL;
 
static void shared_lock(void)
{
    while (sem_wait(&shared->lock) < 0)
        ;   /* reintenta si una senal interrumpe la espera (EINTR) */
}
 
static void shared_unlock(void)
{
    sem_post(&shared->lock);
}
 
/* Crea la region compartida e inicializa el semaforo (solo el Box). */
static void shared_init(void)
{
    shared = mmap(NULL, sizeof(shared_t), PROT_READ | PROT_WRITE,
                  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shared == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
    if (sem_init(&shared->lock, 1, 1) < 0) {   /* 1 = compartido entre procesos */
        perror("sem_init");
        exit(1);
    }
}
 
/* Pone los contadores en cero al iniciar una solicitud nueva. */
static void shared_reset(void)
{
    shared_lock();
    shared->total_created = 0;
    shared->active = 0;
    shared->max_level = 0;
    shared->sim_wait = 0;
    memset(shared->level_count, 0, sizeof(shared->level_count));
    shared_unlock();
}
 
/* Un Meeseeks nuevo se anota en los contadores globales. */
static void shared_register(int level)
{
    int idx = (level > MAX_LEVELS) ? MAX_LEVELS : level;
    shared_lock();
    shared->total_created++;
    shared->active++;
    shared->level_count[idx]++;
    if (level > shared->max_level)
        shared->max_level = level;
    shared_unlock();
}
 
/* Un Meeseeks que desaparece se resta de los activos. */
static void shared_unregister(void)
{
    shared_lock();
    shared->active--;
    shared_unlock();
}
 
/* Registra el tiempo de espera simulado (conserva el mayor). */
static void shared_note_wait(double seconds)
{
    shared_lock();
    if (seconds > shared->sim_wait)
        shared->sim_wait = seconds;
    shared_unlock();
}
 
/* Copia de forma segura los contadores actuales. */
static void shared_snapshot(int *total, int *active, int *max_level,
                            double *sim_wait)
{
    shared_lock();
    *total     = shared->total_created;
    *active    = shared->active;
    *max_level = shared->max_level;
    *sim_wait  = shared->sim_wait;
    shared_unlock();
}
 
/* Libera el semaforo y la memoria compartida (solo el Box, al terminar). */
static void shared_cleanup(void)
{
    sem_destroy(&shared->lock);
    munmap(shared, sizeof(shared_t));
    shared = NULL;
}
 
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
 * meeseeks_start: el Meeseeks se anota en la memoria compartida y da su
 * mensaje de bienvenida. Debe ser lo primero que hace cada Meeseeks.
 * ------------------------------------------------------------------------- */
static void meeseeks_start(int level, int instance)
{
    shared_register(level);
    meeseeks_greet(level, instance);
}
 
/* ---------------------------------------------------------------------------
 * meeseeks_exit: el Meeseeks desaparece. Se resta de los activos y termina.
 * ------------------------------------------------------------------------- */
static void meeseeks_exit(int code)
{
    shared_unregister();
    fflush(stdout);
    _exit(code);
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
 
    meeseeks_start(1, 1);
 
    strncpy(buf, payload, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    if (split_args(buf, argv, MAX_ARGS) == 0) {
        meeseeks_say("No me pidieron ningun programa. No puedo ayudar.");
        meeseeks_exit(1);
    }
 
    snprintf(msg, sizeof(msg), "Voy a ejecutar el programa: %s", argv[0]);
    meeseeks_say(msg);
 
    fflush(stdout);
    pid_t worker = fork();
    if (worker < 0) {
        perror("fork");
        meeseeks_exit(1);
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
        meeseeks_exit(1);
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
    meeseeks_exit(ok ? 0 : 1);
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
 
    meeseeks_start(1, 1);
 
    snprintf(msg, sizeof(msg), "Voy a calcular: %s", payload);
    meeseeks_say(msg);
 
    if (!run_bc(payload, out, sizeof(out))) {
        snprintf(msg, sizeof(msg), "Error en la expresion: %s", out);
        meeseeks_say(msg);
        meeseeks_say("No pude completar la solicitud. Adios!");
        meeseeks_exit(1);
    }
 
    snprintf(msg, sizeof(msg), "Resultado = %s", out);
    meeseeks_say(msg);
    meeseeks_say("Solicitud completada. Gracias por dejarme servirle. Adios!");
    meeseeks_exit(0);
}
 
/* ===========================================================================
 * TEXTO LIBRE (parte A): simulacion con dificultad
 * ========================================================================= */
 
#define MAX_USER_WAIT   300.0   /* tiempo de simulacion maximo que puede fijar el usuario */
#define EASY_THRESHOLD   85.0   /* dificultad > 85: lo resuelve un solo Meeseeks */
 
/* Opciones de una solicitud de texto. -1 significa "al azar". */
typedef struct {
    double      difficulty;     /* 0 a 100, o -1 */
    double      wait;           /* segundos de simulacion, o -1 */
    const char *text;           /* solicitud sin el prefijo */
} text_opts_t;
 
/* ---------------------------------------------------------------------------
 * parse_text_options: lee el prefijo opcional "dif=<0-100>,t=<seg>:".
 * Devuelve 1 si es valido, 0 si hay error (ya avisado al usuario).
 * ------------------------------------------------------------------------- */
static int parse_text_options(const char *payload, text_opts_t *o)
{
    o->difficulty = -1;
    o->wait = -1;
    o->text = payload;
 
    if (strncmp(payload, "dif=", 4) == 0 || strncmp(payload, "t=", 2) == 0) {
        const char *colon = strchr(payload, ':');
        char hdr[64];
        if (colon == NULL || (size_t)(colon - payload) >= sizeof(hdr)) {
            printf("[Box %d] Formato: dif=<0-100>,t=<seg>: <solicitud>\n", getpid());
            return 0;
        }
        memcpy(hdr, payload, (size_t)(colon - payload));
        hdr[colon - payload] = '\0';
 
        for (char *tok = strtok(hdr, ","); tok != NULL; tok = strtok(NULL, ",")) {
            char *end;
            if (strncmp(tok, "dif=", 4) == 0) {
                double v = strtod(tok + 4, &end);
                if (end == tok + 4 || *end != '\0' || v < 0 || v > 100) {
                    printf("[Box %d] La dificultad debe estar entre 0 y 100.\n", getpid());
                    return 0;
                }
                o->difficulty = v;
            } else if (strncmp(tok, "t=", 2) == 0) {
                double v = strtod(tok + 2, &end);
                if (end == tok + 2 || *end != '\0' || v < 0 || v > MAX_USER_WAIT) {
                    printf("[Box %d] El tiempo debe estar entre 0 y %.0f segundos.\n",
                           getpid(), MAX_USER_WAIT);
                    return 0;
                }
                o->wait = v;
            } else {
                printf("[Box %d] Opcion desconocida: %s (use dif= o t=)\n", getpid(), tok);
                return 0;
            }
        }
        o->text = colon + 1;
        while (*o->text == ' ' || *o->text == '\t')
            o->text++;
    }
 
    if (o->text[0] == '\0') {
        printf("[Box %d] La solicitud esta vacia.\n", getpid());
        return 0;
    }
    return 1;
}
 
/* ---------------------------------------------------------------------------
 * random_difficulty: dificultad al azar con distribucion normal (Box-Muller),
 * media 50 y desviacion 25, recortada al rango [0, 100]. Asi hay pocas tareas
 * imposibles o muy faciles y muchas de dificultad media.
 * ------------------------------------------------------------------------- */
static double random_difficulty(void)
{
    double u1 = 1.0 - drand48();   /* (0, 1], evita log(0) */
    double u2 = drand48();
    double z  = sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
    double d  = 50.0 + 25.0 * z;
    if (d < 0)   d = 0;
    if (d > 100) d = 100;
    return d;
}
 
/* ---------------------------------------------------------------------------
 * meeseeks_text: cuerpo del Meeseeks que atiende una solicitud de texto.
 * Paso 5: solo el caso facil (dificultad > 85) se resuelve. Para dificultad
 * menor, el Meeseeks avisa que necesita ayuda (se implementa en el paso 6).
 * ------------------------------------------------------------------------- */
static void meeseeks_text(const text_opts_t *opts)
{
    char msg[MAX_REQ + 160];
 
    meeseeks_start(1, 1);
    /* Cada proceso necesita su propia semilla: si no, todos darian lo mismo */
    srand48(time(NULL) ^ ((long)getpid() << 16));
 
    snprintf(msg, sizeof(msg), "Solicitud recibida: \"%s\"", opts->text);
    meeseeks_say(msg);
 
    double d = opts->difficulty;
    const char *origin = "definida por el usuario";
    if (d < 0) {
        d = random_difficulty();
        origin = "calculada al azar";
    }
    snprintf(msg, sizeof(msg), "Dificultad: %.2f (%s)", d, origin);
    meeseeks_say(msg);
 
    double w = opts->wait;
    if (w < 0)
        w = 0.5 + drand48() * 4.5;   /* uniforme entre 0.5 y 5 s */
    snprintf(msg, sizeof(msg), "Voy a trabajar en esto durante %.2f s...", w);
    meeseeks_say(msg);
 
    shared_note_wait(w);             /* el Box descontara este tiempo */
    usleep((useconds_t)(w * 1e6));
 
    if (d > EASY_THRESHOLD) {
        meeseeks_say("Listo, era facil. Solicitud completada. Gracias por dejarme servirle. Adios!");
        meeseeks_exit(0);
    }
 
    snprintf(msg, sizeof(msg),
             "Esto es dificil (%.2f). Necesito ayuda de otros Meeseeks (pendiente: paso 6).", d);
    meeseeks_say(msg);
    meeseeks_say("No pude completar la solicitud. Adios!");
    meeseeks_exit(1);
}
 
/* ---------------------------------------------------------------------------
 * handle_request: punto de entrada para atender una solicitud.
 * Crea el primer Meeseeks con fork y espera a que termine. Devuelve 1 si la
 * tarea se completo, 0 si no. Llena la cantidad de Meeseeks creados y la
 * espera simulada (para descontarla del tiempo real).
 * ------------------------------------------------------------------------- */
static int handle_request(req_type_t type, const char *payload,
                          int *meeseeks_created, double *sim_wait)
{
    text_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    *meeseeks_created = 0;
    *sim_wait = 0;
 
    if (type == REQ_TEXT && !parse_text_options(payload, &opts))
        return 0;
 
    shared_reset();
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 0;
    }
    if (pid == 0) {
        if (type == REQ_CALC)
            meeseeks_calc(payload);          /* no regresa */
        else if (type == REQ_PROG)
            meeseeks_run_program(payload);   /* no regresa */
        else
            meeseeks_text(&opts);            /* no regresa */
        _exit(1);
    }
 
    int status = 0;
    waitpid(pid, &status, 0);
 
    int total, active, max_level;
    shared_snapshot(&total, &active, &max_level, sim_wait);
    *meeseeks_created = total;
    printf("[Box %d] Meeseeks creados: %d | activos: %d | nivel maximo: %d | espera simulada: %.3f s\n",
           getpid(), total, active, max_level, *sim_wait);
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
        printf("%3d. \"%s\" | duracion real: %.3f s | espera simulada: %.3f s | %s | Meeseeks: %d\n",
               i + 1, tasks[i].request, tasks[i].duration, tasks[i].sim_wait,
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
 
    shared_init();
 
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
        t->completed = handle_request(type, payload, &t->meeseeks, &t->sim_wait);
        /* Tiempo real = tiempo total menos la espera aleatoria de la simulacion */
        t->duration = now_seconds() - start - t->sim_wait;
        if (t->duration < 0)
            t->duration = 0;
 
        task_count++;
    }
 
    shared_cleanup();
    print_report();
    return 0;
}