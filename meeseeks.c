/*
 * ============================================================================
 * Tarea 3 - Mr. Meeseeks
 * Curso:       Sistemas Operativos
 * Estudiante:  Sebastian Guillen Guzman    
 * Carne:       2022123818
 * Fecha:       15 de Octubre 2026
 * Compilacion: make
 * Ejecucion:   ./meeseeks [-m max_meeseeks] [-f factor_coordinacion] [-T segundos] [-N] [-o archivo.csv]
 *
 * Descripcion:
 *   Programa Box de Mr. Meeseeks. Recibe solicitudes del usuario y crea
 *   procesos Meeseeks (fork) para atenderlas. Tipos de solicitud:
 *     - Texto libre (simulacion con dificultad 0 a 100)
 *     - Calculo aritmetico o logico            (prefijo "calc:")
 *     - Ejecucion de un programa externo       (prefijo "run:")
 *   Escribir "salir" para despedirse y ver el informe final.
 *
 *   Texto libre: se puede fijar la dificultad y el tiempo de simulacion con
 *   un prefijo opcional:   dif=30,t=2: quiero un cafe frances
 *   Sin prefijo, la dificultad (0-100) y el tiempo (0.5-5 s) salen al azar.
 *
 * Estado del desarrollo:
 *   Paso 1: esqueleto del Box, lectura de solicitudes y estadisticas.
 *   Paso 2: solicitudes "run:" (fork + exec + waitpid, con argumentos).
 *   Paso 3: solicitudes "calc:" (programa bc mediante pipes).
 *   Paso 4: memoria compartida + semaforo para contadores de Meeseeks.
 *   Paso 5: texto libre con dificultad, espera simulada y caso facil (>85).
 *   Paso 6: creacion recursiva de Meeseeks, solicitud por pipe, reparto de trabajo y limite de Meeseeks.
 *   Paso 7: resultado hacia el padre por pipe, orden de terminar a los hermanos, y despedida en cadena hasta el Meeseeks original.
 *   Paso 8: tiempo maximo (caos planetario) y modo de medicion de tiempo extra.
 *   Paso 9: salida CSV (-o) con una fila por solicitud, para los experimentos.
 * ============================================================================
 */
 
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/prctl.h>
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
    double     overtime;    /* segundos que se paso del tiempo maximo (modo -N) */
    int        chaos;       /* 1 = el caos planetario la elimino */
    int        max_level;   /* nivel N mas profundo que se alcanzo */
    int        terminated;  /* Meeseeks mandados a desaparecer por orden */
    double     difficulty;  /* dificultad real de la solicitud (-1 si no aplica) */
    int        completed;   /* 1 = terminada, 0 = no */
    int        meeseeks;    /* cantidad de Meeseeks creados */
} task_t;
 
static task_t tasks[MAX_TASKS];
static int    task_count = 0;
 
/* ---------------------------------------------------------------------------
 * job_t: mensaje privado que un Meeseeks recibe por su pipe al nacer.
 * Lleva la solicitud del usuario y lo que necesita para trabajar. El Box se lo
 * manda al primer Meeseeks y cada Meeseeks se lo manda a sus ayudantes.
 * ------------------------------------------------------------------------- */
typedef struct {
    int    level;        /* N: nivel del Meeseeks */
    int    instance;     /* i: numero de instancia dentro de su nivel */
    double work;         /* trabajo restante = 100 - dificultad; < 0: decidir al azar */
    double wait;         /* tiempo de simulacion fijado por el usuario; < 0: al azar */
    double chain_wait;   /* espera simulada acumulada por los ancestros (s) */
    int    impossible;   /* 1 si la solicitud original tiene dificultad 0 */
    char   text[MAX_REQ];/* solicitud del usuario */
} job_t;
 
/* ---------------------------------------------------------------------------
 * msg_t: mensaje corto que viaja por los pipes entre padre e hijo.
 *   hacia abajo (padre -> hijo):  MSG_TERMINATE (hay que desaparecer)
 *   hacia arriba (hijo -> padre): MSG_SOLVED (alguien resolvio la solicitud;
 *                                 trae los datos del Meeseeks que la resolvio)
 *                                 MSG_FAILED (ni yo ni mis ayudantes pudimos)
 * ------------------------------------------------------------------------- */
typedef enum { MSG_TERMINATE = 1, MSG_SOLVED = 2, MSG_FAILED = 3 } msg_type_t;
 
typedef struct {
    int type;       /* msg_type_t */
    int pid;        /* datos del Meeseeks que resolvio (solo en MSG_SOLVED) */
    int ppid;
    int level;      /* N */
    int instance;   /* i */
} msg_t;
 
/* helper_t: lo que un creador guarda de cada Meeseeks que crea */
typedef struct {
    pid_t pid;
    int   to_child;     /* extremo de escritura: ordenes hacia el hijo */
    int   from_child;   /* extremo de lectura: resultados del hijo */
} helper_t;
 
/* Un Meeseeks recibe su mensaje de trabajo por down_fd (de lectura) y manda
 * sus resultados al padre por up_fd (de escritura). */
typedef void (*meeseeks_body_t)(const job_t *job, int down_fd, int up_fd);
 
static int spawn_meeseeks(meeseeks_body_t body, const job_t *job, helper_t *h,
                         int new_group);
 
#define DEFAULT_MAX_MEESEEKS 100   /* limite de Meeseeks por solicitud (-m) */
#define DEFAULT_COORD_FACTOR 1.5   /* costo de coordinacion al repartir (-f) */
 
static double coord_factor = DEFAULT_COORD_FACTOR;
 
/* --- Tiempo maximo ("caos planetario") ---
 * Si una solicitud tarda mas de max_time segundos reales (la espera simulada
 * tambien cuenta, porque es tiempo real que transcurre), el Box decreta el
 * caos planetario: elimina a todos los Meeseeks de la solicitud y cierra el
 * programa. Con la opcion -N el Box solo avisa y sigue esperando, para medir
 * cuanto tiempo adicional necesita la solicitud para terminar. */
#define DEFAULT_MAX_TIME 300.0     /* 5 minutos (-T para cambiarlo) */
 
static double max_time = DEFAULT_MAX_TIME;
static int    kill_on_timeout = 1;               /* 0 con la opcion -N */
static volatile sig_atomic_t current_pgid = 0;   /* grupo de la solicitud en curso */
 
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
 *
 * Limite de Meeseeks (plan de contingencia): quien quiera crear ayudantes
 * primero los "reserva" (shared_reserve). Si crearlos pasaria del limite, la
 * reserva se niega y no se crea ningun proceso. La reserva es atomica gracias
 * al semaforo, asi que el limite nunca se sobrepasa aunque muchos Meeseeks
 * pidan ayuda al mismo tiempo.
 * ========================================================================= */
 
#define MAX_LEVELS 64
 
typedef struct {
    sem_t lock;                          /* semaforo binario (mutex entre procesos) */
    int   max_meeseeks;                  /* limite de Meeseeks por solicitud */
    int   total_created;                 /* Meeseeks creados (reservados) en la solicitud */
    int   active;                        /* Meeseeks vivos en este momento */
    int   max_level;                     /* nivel N mas profundo alcanzado */
    int   terminated;                    /* Meeseeks mandados a desaparecer antes de terminar */
    double difficulty;                   /* dificultad de la solicitud (la fija el primer Meeseeks) */
    double sim_wait;                     /* espera simulada (s) a descontar del tiempo real */
    int   level_count[MAX_LEVELS + 1];   /* Meeseeks creados por nivel */
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
static void shared_init(int max_meeseeks)
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
    shared->max_meeseeks = max_meeseeks;
}
 
/* Pone los contadores en cero al iniciar una solicitud nueva. */
static void shared_reset(void)
{
    shared_lock();
    shared->total_created = 0;
    shared->active = 0;
    shared->max_level = 0;
    shared->terminated = 0;
    shared->difficulty = -1;
    shared->sim_wait = 0;
    memset(shared->level_count, 0, sizeof(shared->level_count));
    shared_unlock();
}
 
/* Reserva 'count' Meeseeks del nivel 'level'. Devuelve 0 si pasaria el limite. */
static int shared_reserve(int count, int level)
{
    int idx = (level > MAX_LEVELS) ? MAX_LEVELS : level;
    int ok = 0;
    shared_lock();
    if (shared->total_created + count <= shared->max_meeseeks) {
        shared->total_created += count;
        shared->level_count[idx] += count;
        if (level > shared->max_level)
            shared->max_level = level;
        ok = 1;
    }
    shared_unlock();
    return ok;
}
 
/* Devuelve una reserva que no se pudo usar (por ejemplo, fork fallo). */
static void shared_release(int level)
{
    int idx = (level > MAX_LEVELS) ? MAX_LEVELS : level;
    shared_lock();
    shared->total_created--;
    shared->level_count[idx]--;
    shared_unlock();
}
 
/* Un Meeseeks recien nacido se anota como activo. */
static void shared_register(void)
{
    shared_lock();
    shared->active++;
    shared_unlock();
}
 
/* Un Meeseeks que desaparece se resta de los activos. */
static void shared_unregister(void)
{
    shared_lock();
    shared->active--;
    shared_unlock();
}
 
/* El primer Meeseeks anota la dificultad de la solicitud (dada o al azar). */
static void shared_set_difficulty(double d)
{
    shared_lock();
    shared->difficulty = d;
    shared_unlock();
}
 
/* Un Meeseeks se despide por orden de su padre (no por haber terminado). */
static void shared_note_terminated(void)
{
    shared_lock();
    shared->terminated++;
    shared_unlock();
}
 
/* Registra el tiempo de espera simulado (conserva el mayor camino). */
static void shared_note_wait(double seconds)
{
    shared_lock();
    if (seconds > shared->sim_wait)
        shared->sim_wait = seconds;
    shared_unlock();
}
 
/* Copia de forma segura los contadores actuales. */
static void shared_snapshot(int *total, int *active, int *max_level,
                            int *terminated, double *sim_wait, double *difficulty,
                            char *levels, size_t levels_size)
{
    shared_lock();
    *terminated = shared->terminated;
    *difficulty = shared->difficulty;
    *total     = shared->total_created;
    *active    = shared->active;
    *max_level = shared->max_level;
    *sim_wait  = shared->sim_wait;
    levels[0] = '\0';
    size_t used = 0;
    for (int l = 1; l <= shared->max_level && l <= MAX_LEVELS && used < levels_size; l++)
        used += (size_t)snprintf(levels + used, levels_size - used,
                                 "%sN%d=%d", l > 1 ? " " : "", l, shared->level_count[l]);
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
    shared_register();
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
static void meeseeks_run_program(const job_t *job, int down_fd, int up_fd)
{
    close(down_fd);   /* este tipo de Meeseeks no usa los pipes de ayudantes */
    close(up_fd);
    char  buf[MAX_REQ];
    char *argv[MAX_ARGS];
    char  msg[MAX_REQ + 64];
 
    meeseeks_start(job->level, job->instance);
 
    strncpy(buf, job->text, sizeof(buf) - 1);
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
        /* El programa externo no debe leer del teclado del Box (ni quedar
         * detenido por estar en otro grupo de procesos) */
        int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0) {
            dup2(nul, STDIN_FILENO);
            close(nul);
        }
        signal(SIGPIPE, SIG_DFL);   /* el programa externo la espera normal */
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
        signal(SIGPIPE, SIG_DFL);
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
static void meeseeks_calc(const job_t *job, int down_fd, int up_fd)
{
    close(down_fd);   /* este tipo de Meeseeks no usa los pipes de ayudantes */
    close(up_fd);
    char msg[MAX_CALC_OUT + 128];
    char out[MAX_CALC_OUT];
 
    meeseeks_start(job->level, job->instance);
 
    snprintf(msg, sizeof(msg), "Voy a calcular: %s", job->text);
    meeseeks_say(msg);
 
    if (!run_bc(job->text, out, sizeof(out))) {
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
 *
 * Modelo de trabajo (dificultad invertida):
 *      trabajo = 100 - dificultad        (0 = nada que hacer, 100 = imposible)
 *
 *  - Un Meeseeks con dificultad > 85 (trabajo < 15) resuelve su parte solo.
 *  - Si no, pide ayuda: crea 5 Meeseeks (dificultad 0 a 45) o 2 (45.01 a 85)
 *    y reparte el trabajo entre ellos de forma desigual (pesos al azar) y con
 *    un costo de coordinacion (coord_factor):
 *          trabajo_hijo = trabajo * coord_factor * peso_i / suma_de_pesos
 *    Asi el trabajo de cada Meeseeks baja nivel a nivel (aging) hasta que
 *    alguno puede resolver su parte.
 *  - Dificultad 0 (imposible): el trabajo NO se diluye, cada hijo hereda el
 *    trabajo completo. Los Meeseeks se multiplican hasta que el limite de
 *    Meeseeks (-m) se lo impide; entonces los de la frontera se niegan a
 *    crear mas y la solicitud termina sin resolverse.
 * ========================================================================= */
 
#define MAX_USER_WAIT    300.0  /* tiempo de simulacion maximo que puede fijar el usuario */
#define EASY_THRESHOLD    85.0  /* dificultad > 85: lo resuelve un solo Meeseeks */
#define HARD_THRESHOLD    45.0  /* dificultad <= 45: se piden 5 ayudantes, si no 2 */
#define MAX_HELPERS         5
 
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
 * write_all / read_all: escriben o leen exactamente n bytes de un pipe.
 * ------------------------------------------------------------------------- */
static int write_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;      /* por ejemplo EPIPE: el otro lado ya no existe */
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}
 
static int read_all(int fd, void *buf, size_t n)
{
    char *p = buf;
    while (n > 0) {
        ssize_t r = read(fd, p, n);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0)
            return -1;      /* el pipe se cerro antes de tiempo */
        p += r;
        n -= (size_t)r;
    }
    return 0;
}
 
/* Envia un mensaje corto; 'who' son los datos del que resolvio (o NULL). */
static void send_msg(int fd, int type, const msg_t *who)
{
    msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = type;
    if (who != NULL) {
        m.pid      = who->pid;
        m.ppid     = who->ppid;
        m.level    = who->level;
        m.instance = who->instance;
    }
    write_all(fd, &m, sizeof(m));   /* si el receptor ya no existe, no importa */
}
 
/* ---------------------------------------------------------------------------
 * told_to_terminate: el Meeseeks "trabaja" durante 'seconds' pero sigue
 * atento al pipe de su padre. Devuelve 1 si le ordenaron terminar (o si el
 * padre desaparecio), 0 si el tiempo se cumplio.
 * ------------------------------------------------------------------------- */
static int told_to_terminate(int down_fd, double seconds)
{
    double deadline = now_seconds() + seconds;
 
    for (;;) {
        double remaining = deadline - now_seconds();
        if (remaining <= 0)
            return 0;
 
        struct pollfd p = { down_fd, POLLIN, 0 };
        int r = poll(&p, 1, (int)(remaining * 1000.0) + 1);
        if (r < 0) {
            if (errno == EINTR) continue;
            return 0;
        }
        if (r == 0)
            continue;               /* se cumplio el tiempo; el ciclo lo confirma */
 
        msg_t m;
        ssize_t n = read(down_fd, &m, sizeof(m));
        if (n != (ssize_t)sizeof(m) || m.type == MSG_TERMINATE)
            return 1;
    }
}
 
/* El Meeseeks original informa al usuario quien resolvio la solicitud. */
static void report_solver(const msg_t *who)
{
    char msg[200];
    snprintf(msg, sizeof(msg),
             "RESULTADO: la solicitud fue resuelta por el Meeseeks (pid %d, ppid %d, N=%d, i=%d).",
             who->pid, who->ppid, who->level, who->instance);
    meeseeks_say(msg);
}
 
/* ---------------------------------------------------------------------------
 * meeseeks_text: cuerpo de un Meeseeks que atiende texto libre (nivel 1 o un
 * ayudante de cualquier nivel). Resuelve su parte o pide ayuda a otros
 * Meeseeks.
 *
 * Comunicacion por pipes:
 *   - Resolver:   el que resuelve avisa a su padre (MSG_SOLVED con sus datos).
 *                 El padre manda MSG_TERMINATE a los demas hijos, espera a que
 *                 todos desaparezcan, se despide y avisa a SU padre, y asi
 *                 hacia atras hasta el Meeseeks original, que informa al
 *                 usuario quien resolvio.
 *   - Terminar:   quien recibe MSG_TERMINATE se lo pasa a sus propios
 *                 ayudantes (la orden baja en cascada), espera a que se
 *                 vayan, se despide y desaparece.
 *   - Fallar:     si ni el ni sus ayudantes pueden, avisa MSG_FAILED.
 *
 * Termina con exit 0 si se resolvio, 1 si no, y 3 si lo mandaron a terminar.
 * ------------------------------------------------------------------------- */
static void meeseeks_text(const job_t *job, int down_fd, int up_fd)
{
    char msg[MAX_REQ + 256];
    int  level = job->level;
 
    meeseeks_start(level, job->instance);
    /* Cada proceso necesita su propia semilla: si no, todos darian lo mismo */
    srand48(time(NULL) ^ ((long)getpid() << 16));
 
    msg_t me;
    memset(&me, 0, sizeof(me));
    me.type     = MSG_SOLVED;
    me.pid      = getpid();
    me.ppid     = getppid();
    me.level    = level;
    me.instance = job->instance;
 
    snprintf(msg, sizeof(msg), "Solicitud recibida por pipe: \"%s\"", job->text);
    meeseeks_say(msg);
 
    /* --- dificultad de mi parte --- */
    double work = job->work;
    int impossible = job->impossible;
    double d;
    const char *origin;
    if (work < 0) {                       /* solo el primer Meeseeks: al azar */
        d = random_difficulty();
        work = 100.0 - d;
        impossible = (d <= 0.0);
        origin = "calculada al azar";
    } else {
        d = 100.0 - work;
        origin = (level == 1) ? "definida por el usuario" : "parte que me dio mi padre";
    }
    snprintf(msg, sizeof(msg), "Dificultad: %.2f (%s)%s", d, origin,
             impossible ? " IMPOSIBLE" : "");
    meeseeks_say(msg);
 
    if (level == 1)
        shared_set_difficulty(d);     /* queda en el informe y en el CSV */
 
    /* --- tiempo de simulacion (atento a una orden de terminar) --- */
    double w = job->wait;
    if (w < 0)
        w = 0.5 + drand48() * 4.5;        /* uniforme entre 0.5 y 5 s */
    double chain = job->chain_wait + w;   /* espera acumulada desde el primer Meeseeks */
    snprintf(msg, sizeof(msg), "Voy a trabajar en esto durante %.2f s...", w);
    meeseeks_say(msg);
 
    if (told_to_terminate(down_fd, w)) {
        meeseeks_say("Mi padre me pidio terminar. Me despido. Adios!");
        shared_note_terminated();
        meeseeks_exit(3);
    }
    /* Solo cuenta la espera de quien la completo: asi el Box descuenta el
     * camino de espera del Meeseeks que realmente resolvio. */
    shared_note_wait(chain);
 
    /* --- caso facil: lo resuelvo yo solo --- */
    if (!impossible && d > EASY_THRESHOLD) {
        meeseeks_say("Era facil, resolvi mi parte. Gracias por dejarme servirle. Adios!");
        if (level == 1)
            report_solver(&me);
        else
            send_msg(up_fd, MSG_SOLVED, &me);
        meeseeks_exit(0);
    }
 
    /* --- caso dificil: pido ayuda --- */
    int k = (d <= HARD_THRESHOLD) ? 5 : 2;
    if (!shared_reserve(k, level + 1)) {
        snprintf(msg, sizeof(msg),
                 "Necesito %d ayudantes pero se alcanzo el limite de Meeseeks. "
                 "Para no colapsar el sistema, no creo mas.", k);
        meeseeks_say(msg);
        meeseeks_say("No pude completar mi parte. Adios!");
        if (level > 1)
            send_msg(up_fd, MSG_FAILED, NULL);
        meeseeks_exit(1);
    }
    snprintf(msg, sizeof(msg),
             "Esto es dificil (%.2f). Creo %d Meeseeks nuevos en el nivel %d.",
             d, k, level + 1);
    meeseeks_say(msg);
 
    /* Reparto desigual del trabajo: pesos al azar entre 0.7 y 1.3 */
    double weights[MAX_HELPERS], sum = 0;
    for (int i = 0; i < k; i++) {
        weights[i] = 0.7 + 0.6 * drand48();
        sum += weights[i];
    }
 
    helper_t kids[MAX_HELPERS];
    int      created = 0;
    for (int i = 0; i < k; i++) {
        job_t child;
        memset(&child, 0, sizeof(child));
        child.level      = level + 1;
        child.instance   = i + 1;
        child.work       = impossible ? work : work * coord_factor * weights[i] / sum;
        child.wait       = job->wait;
        child.chain_wait = chain;
        child.impossible = impossible;
        snprintf(child.text, sizeof(child.text), "%s", job->text);
 
        if (spawn_meeseeks(meeseeks_text, &child, &kids[created], 0) < 0) {
            shared_release(level + 1);    /* no nacio: devuelve la reserva */
            continue;
        }
        created++;
    }
 
    /* --- espero noticias: de mis ayudantes (resultados) o de mi padre (orden) --- */
    struct pollfd pfds[1 + MAX_HELPERS];
    int    done[MAX_HELPERS] = { 0 };     /* ya reporto su resultado */
    int    pending = created;
    int    solved = 0, terminated = 0;
    msg_t  winner;
    memset(&winner, 0, sizeof(winner));
 
    while (pending > 0 && !solved && !terminated) {
        pfds[0].fd = down_fd;
        pfds[0].events = POLLIN;
        for (int i = 0; i < created; i++) {
            pfds[1 + i].fd = done[i] ? -1 : kids[i].from_child;   /* -1: poll lo ignora */
            pfds[1 + i].events = POLLIN;
        }
        if (poll(pfds, (nfds_t)(1 + created), -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
 
        if (pfds[0].revents) {            /* orden de mi padre */
            msg_t m;
            ssize_t n = read(down_fd, &m, sizeof(m));
            if (n != (ssize_t)sizeof(m) || m.type == MSG_TERMINATE) {
                terminated = 1;
                break;
            }
        }
        for (int i = 0; i < created; i++) {   /* resultados de mis ayudantes */
            if (done[i] || !pfds[1 + i].revents)
                continue;
            msg_t m;
            ssize_t n = read(kids[i].from_child, &m, sizeof(m));
            done[i] = 1;
            pending--;
            if (n == (ssize_t)sizeof(m) && m.type == MSG_SOLVED && !solved) {
                solved = 1;
                winner = m;
            }
        }
    }
 
    /* Si ya se resolvio (o me mandaron terminar), los ayudantes que aun
     * trabajan reciben la orden de desaparecer. */
    if (solved || terminated) {
        int told = 0;
        for (int i = 0; i < created; i++)
            if (!done[i]) {
                send_msg(kids[i].to_child, MSG_TERMINATE, NULL);
                told++;
            }
        if (told > 0) {
            snprintf(msg, sizeof(msg), "Mando a desaparecer a %d ayudante(s) que aun trabajan.", told);
            meeseeks_say(msg);
        }
    }
 
    /* Espero a que todos mis ayudantes desaparezcan (se despiden primero) */
    for (int i = 0; i < created; i++) {
        int status = 0;
        while (waitpid(kids[i].pid, &status, 0) < 0 && errno == EINTR)
            ;
        close(kids[i].to_child);
        close(kids[i].from_child);
    }
 
    if (terminated) {
        meeseeks_say("Mi padre me pidio terminar y mis ayudantes ya se fueron. Me despido. Adios!");
        shared_note_terminated();
        meeseeks_exit(3);
    }
    if (solved) {
        meeseeks_say("Uno de mis ayudantes resolvio la solicitud. Gracias por dejarme servirle. Adios!");
        if (level == 1)
            report_solver(&winner);       /* el original informa al usuario */
        else
            send_msg(up_fd, MSG_SOLVED, &winner);   /* se lo paso a mi padre */
        meeseeks_exit(0);
    }
 
    meeseeks_say("Mis ayudantes no lograron resolverlo. No pude completar la solicitud. Adios!");
    if (level > 1)
        send_msg(up_fd, MSG_FAILED, NULL);
    meeseeks_exit(1);
}
 
/* ---------------------------------------------------------------------------
 * spawn_meeseeks: crea un Meeseeks con fork y le abre dos pipes privados:
 *   - hacia el hijo: por ahi viaja primero su mensaje de trabajo (job_t) y
 *     despues las ordenes (MSG_TERMINATE)
 *   - desde el hijo: por ahi vuelve su resultado
 * El hijo cierra todos los demas descriptores heredados (los pipes de sus
 * hermanos y del padre), asi cada pipe tiene solo los extremos que corresponden
 * y se puede detectar cuando alguien desaparece. El hijo ejecuta 'body' y
 * nunca regresa. Devuelve 0 si todo salio bien, -1 si fallo.
 *
 * Si new_group es 1 (solo el primer Meeseeks de cada solicitud), el nuevo
 * proceso pasa a ser lider de un grupo de procesos propio. Todos sus
 * descendientes heredan ese grupo, asi el Box puede eliminar el arbol
 * completo con un solo kill(-grupo) si se decreta el caos planetario.
 * ------------------------------------------------------------------------- */
static int spawn_meeseeks(meeseeks_body_t body, const job_t *job, helper_t *h,
                         int new_group)
{
    int down[2], up[2];
 
    if (pipe(down) < 0) {
        perror("pipe");
        return -1;
    }
    if (pipe(up) < 0) {
        perror("pipe");
        close(down[0]);
        close(down[1]);
        return -1;
    }
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        close(down[0]); close(down[1]);
        close(up[0]);   close(up[1]);
        return -1;
    }
    if (pid == 0) {
        /* Hijo: se queda solo con su extremo de lectura de ordenes y su
         * extremo de escritura de resultados */
        if (new_group)
            setpgid(0, 0);
        int my_down = down[0], my_up = up[1];
        for (int fd = 3; fd < 64; fd++)
            if (fd != my_down && fd != my_up)
                close(fd);
 
        job_t mine;
        if (read_all(my_down, &mine, sizeof(mine)) < 0)
            _exit(1);
        body(&mine, my_down, my_up);    /* no regresa */
        _exit(1);
    }
 
    /* Padre: se queda con los extremos opuestos */
    close(down[0]);
    close(up[1]);
    if (new_group)
        setpgid(pid, pid);   /* antes de enviar el trabajo: el grupo existe
                              * antes de que el hijo cree a nadie */
    h->pid        = pid;
    h->to_child   = down[1];
    h->from_child = up[0];
    write_all(h->to_child, job, sizeof(*job));
    return 0;
}
 
/* ---------------------------------------------------------------------------
 * wait_with_deadline: el Box espera al primer Meeseeks, pero no mas de
 * max_time segundos desde 'started'. Devuelve 1 si el Meeseeks termino
 * (estado en *status), 0 si se decreto el caos planetario.
 *
 * Con -N (kill_on_timeout = 0) no se elimina a nadie: se avisa una vez, se
 * sigue esperando y se anota en t->overtime cuanto se paso del limite.
 *
 * La espera es exacta y sin consultas periodicas: SIGCHLD se bloquea y
 * sigtimedwait() duerme hasta que el hijo termine o venza el plazo. Como la
 * senal queda pendiente aunque llegue antes de dormir, no hay carreras.
 * ------------------------------------------------------------------------- */
static int wait_with_deadline(pid_t pid, int *status, double started, task_t *t)
{
    sigset_t set, old;
    sigemptyset(&set);
    sigaddset(&set, SIGCHLD);
    sigprocmask(SIG_BLOCK, &set, &old);
 
    double deadline = started + max_time;
    int finished = 0;
 
    for (;;) {
        pid_t r = waitpid(pid, status, WNOHANG);
        if (r == pid) {
            finished = 1;
            break;
        }
 
        double remaining = deadline - now_seconds();
        if (remaining <= 0) {
            printf("[Box %d] CAOS PLANETARIO: pasaron %.0f s y la solicitud no termina.\n",
                   getpid(), max_time);
            fflush(stdout);
            if (kill_on_timeout)
                break;                          /* finished = 0: hay que eliminar a todos */
            printf("[Box %d] Modo -N: sigo esperando para medir el tiempo adicional...\n",
                   getpid());
            fflush(stdout);
            while (waitpid(pid, status, 0) < 0 && errno == EINTR)
                ;
            t->overtime = now_seconds() - deadline;
            finished = 1;
            break;
        }
 
        struct timespec ts;
        ts.tv_sec  = (time_t)remaining;
        ts.tv_nsec = (long)((remaining - (double)ts.tv_sec) * 1e9);
        sigtimedwait(&set, NULL, &ts);          /* despierta con SIGCHLD o al vencer */
    }
 
    sigprocmask(SIG_SETMASK, &old, NULL);
    return finished;
}
 
/* Espera a todos los procesos hijo que queden (el Box es "subreaper": los
 * Meeseeks huerfanos del caos pasan a ser suyos). Termina cuando no queda ninguno. */
static void reap_all_children(void)
{
    int st;
    for (;;) {
        pid_t r = waitpid(-1, &st, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;      /* ECHILD: ya no queda ninguno */
        }
    }
}
 
/* Si el usuario interrumpe el Box (Ctrl-C), no deben quedar Meeseeks vivos:
 * estan en otro grupo de procesos y no reciben la senal del teclado. */
static void on_interrupt(int sig)
{
    (void)sig;
    if (current_pgid > 0)
        kill(-current_pgid, SIGKILL);
    _exit(130);
}
 
/* ---------------------------------------------------------------------------
 * handle_request: punto de entrada para atender una solicitud.
 * Arma el mensaje, crea el primer Meeseeks (nivel 1, instancia 1) y espera a
 * que termine todo su arbol de ayudantes, dentro del tiempo maximo. Devuelve
 * 1 si la tarea se completo, 0 si no. Llena en 't' la cantidad de Meeseeks
 * creados, la espera simulada (para descontarla del tiempo real), el tiempo
 * extra (modo -N) y si el caos planetario la elimino.
 * ------------------------------------------------------------------------- */
static int handle_request(req_type_t type, const char *payload, task_t *t)
{
    text_opts_t     opts;
    job_t           job;
    meeseeks_body_t body;
 
    memset(&opts, 0, sizeof(opts));
    memset(&job, 0, sizeof(job));
    job.level    = 1;
    job.instance = 1;
    job.work     = -1;      /* al azar, salvo que el usuario fije la dificultad */
    job.wait     = -1;
    t->meeseeks = 0;
    t->sim_wait = 0;
    t->overtime = 0;
    t->chaos    = 0;
    t->max_level  = 0;
    t->terminated = 0;
    t->difficulty = -1;
 
    if (type == REQ_TEXT) {
        if (!parse_text_options(payload, &opts))
            return 0;
        if (opts.difficulty >= 0) {
            job.work = 100.0 - opts.difficulty;
            job.impossible = (opts.difficulty == 0);
        }
        job.wait = opts.wait;
        snprintf(job.text, sizeof(job.text), "%s", opts.text);
        body = meeseeks_text;
    } else {
        snprintf(job.text, sizeof(job.text), "%s", payload);
        body = (type == REQ_CALC) ? meeseeks_calc : meeseeks_run_program;
    }
 
    shared_reset();
    shared_reserve(1, 1);               /* el primer Meeseeks */
 
    helper_t first;
    double started = now_seconds();
    if (spawn_meeseeks(body, &job, &first, 1) < 0) {
        shared_release(1);
        return 0;
    }
    current_pgid = first.pid;           /* grupo de procesos de esta solicitud */
 
    int status = 0;
    int finished = wait_with_deadline(first.pid, &status, started, t);
 
    if (!finished) {
        /* Caos planetario: el Box termina a todos los procesos hijos */
        printf("[Box %d] Termino a todos los Meeseeks de la solicitud y cierro el programa.\n",
               getpid());
        fflush(stdout);
        kill(-first.pid, SIGKILL);
        reap_all_children();
        /* Un Meeseeks pudo morir justo con el semaforo tomado: se reinicia */
        sem_destroy(&shared->lock);
        sem_init(&shared->lock, 1, 1);
        t->chaos = 1;
    }
    current_pgid = 0;
    close(first.to_child);
    close(first.from_child);
 
    int  total, active, max_level, terminated;
    char levels[256];
    shared_snapshot(&total, &active, &max_level, &terminated, &t->sim_wait,
                    &t->difficulty, levels, sizeof(levels));
    t->meeseeks   = total;
    t->max_level  = max_level;
    t->terminated = terminated;
    if (t->chaos)
        printf("[Box %d] Meeseeks creados: %d (%s) | eliminados por el caos: %d | terminados por orden: %d | nivel maximo: %d\n",
               getpid(), total, levels, active, terminated, max_level);
    else
        printf("[Box %d] Meeseeks creados: %d (%s) | terminados por orden: %d | activos: %d | nivel maximo: %d | espera simulada: %.3f s\n",
               getpid(), total, levels, terminated, active, max_level, t->sim_wait);
    if (t->overtime > 0)
        printf("[Box %d] La solicitud termino %.3f s despues del tiempo maximo.\n",
               getpid(), t->overtime);
    return finished && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
 
/* ---------------------------------------------------------------------------
 * print_report: informe final al cerrar el Box
 * ------------------------------------------------------------------------- */
static void print_report(void)
{
    int done = 0, chaos = 0;
    printf("\n========== INFORME FINAL ==========\n");
    printf("Tareas solicitadas: %d\n", task_count);
    for (int i = 0; i < task_count; i++) {
        const char *status = tasks[i].chaos     ? "CAOS PLANETARIO"
                           : tasks[i].completed ? "TERMINADA"
                                                : "NO TERMINADA";
        char extra[80] = "";
        if (tasks[i].overtime > 0)
            snprintf(extra, sizeof(extra), " | excedio el tiempo maximo por %.3f s",
                     tasks[i].overtime);
        printf("%3d. \"%s\" | duracion real: %.3f s | espera simulada: %.3f s | %s | Meeseeks: %d%s\n",
               i + 1, tasks[i].request, tasks[i].duration, tasks[i].sim_wait,
               status, tasks[i].meeseeks, extra);
        if (tasks[i].completed) done++;
        if (tasks[i].chaos)     chaos++;
    }
    printf("Terminadas: %d | No terminadas: %d (por caos planetario: %d)\n",
           done, task_count - done, chaos);
    printf("===================================\n");
}
 
/* ---------------------------------------------------------------------------
 * Salida CSV (opcion -o): una fila por solicitud, para analizar los
 * experimentos con herramientas como awk. La solicitud se limpia (comas y
 * comillas) para que el archivo se pueda separar por comas sin ambiguedad.
 * ------------------------------------------------------------------------- */
static FILE *csv_file = NULL;
 
#define CSV_HEADER "id,tipo,solicitud,dificultad,duracion_real_s,espera_simulada_s," \
                   "estado,meeseeks,nivel_max,terminados,exceso_s,factor," \
                   "limite_meeseeks,tiempo_max_s\n"
 
static void write_csv_row(int id, const task_t *t)
{
    static const char *type_names[] = { "texto", "calc", "run" };
    char   req[MAX_REQ];
    size_t j = 0;
 
    for (size_t i = 0; t->request[i] != '\0' && j + 1 < sizeof(req); i++) {
        char c = t->request[i];
        req[j++] = (c == ',') ? ';' : (c == '"' || c == '\n') ? '\'' : c;
    }
    req[j] = '\0';
 
    const char *state = t->chaos ? "CAOS" : t->completed ? "TERMINADA" : "NO_TERMINADA";
    fprintf(csv_file, "%d,%s,%s,%.2f,%.6f,%.6f,%s,%d,%d,%d,%.6f,%.2f,%d,%.0f\n",
            id, type_names[t->type], req, t->difficulty, t->duration, t->sim_wait,
            state, t->meeseeks, t->max_level, t->terminated, t->overtime,
            coord_factor, shared->max_meeseeks, max_time);
    fflush(csv_file);       /* que la fila quede guardada aunque el Box se interrumpa */
}
 
static void usage(const char *prog)
{
    fprintf(stderr, "Uso: %s [-m max_meeseeks] [-f factor_coordinacion] [-T segundos] [-N] [-o archivo.csv]\n"
                    "  -m  limite de Meeseeks por solicitud (defecto %d)\n"
                    "  -f  costo de coordinacion al repartir trabajo, 0.5 a 3.0 (defecto %.1f)\n"
                    "  -T  tiempo maximo por solicitud antes del caos planetario (defecto %.0f s)\n"
                    "  -N  al vencer el tiempo maximo no eliminar a nadie: solo avisar y medir\n"
                    "      cuanto tiempo adicional necesita la solicitud\n"
                    "  -o  guardar una fila CSV por solicitud (para los experimentos)\n",
            prog, DEFAULT_MAX_MEESEEKS, DEFAULT_COORD_FACTOR, DEFAULT_MAX_TIME);
}
 
int main(int argc, char **argv)
{
    char line[MAX_REQ];
    int  max_meeseeks = DEFAULT_MAX_MEESEEKS;
    int  opt;
    const char *csv_path = NULL;
 
    while ((opt = getopt(argc, argv, "m:f:T:No:")) != -1) {
        switch (opt) {
        case 'm':
            max_meeseeks = atoi(optarg);
            if (max_meeseeks < 1) {
                usage(argv[0]);
                return 1;
            }
            break;
        case 'f':
            coord_factor = atof(optarg);
            if (coord_factor < 0.5 || coord_factor > 3.0) {
                usage(argv[0]);
                return 1;
            }
            break;
        case 'T':
            max_time = atof(optarg);
            if (max_time <= 0) {
                usage(argv[0]);
                return 1;
            }
            break;
        case 'N':
            kill_on_timeout = 0;
            break;
        case 'o':
            csv_path = optarg;
            break;
        default:
            usage(argv[0]);
            return 1;
        }
    }
 
    shared_init(max_meeseeks);
 
    if (csv_path != NULL) {
        csv_file = fopen(csv_path, "a");     /* "a": varias corridas pueden sumar al mismo archivo */
        if (csv_file == NULL) {
            perror(csv_path);
            return 1;
        }
        fseek(csv_file, 0, SEEK_END);
        if (ftell(csv_file) == 0)
            fputs(CSV_HEADER, csv_file);
    }
    signal(SIGPIPE, SIG_IGN);   /* escribir a un Meeseeks que ya se fue no debe matarnos */
    prctl(PR_SET_CHILD_SUBREAPER, 1);   /* los Meeseeks huerfanos del caos pasan a ser del Box */
    signal(SIGINT, on_interrupt);
    signal(SIGTERM, on_interrupt);
 
    printf("Box de Mr. Meeseeks (pid %d)\n", getpid());
    printf("Limite de Meeseeks por solicitud: %d | factor de coordinacion: %.2f\n",
           max_meeseeks, coord_factor);
    printf("Tiempo maximo por solicitud: %.0f s (%s)\n", max_time,
           kill_on_timeout ? "luego se decreta el caos planetario"
                           : "modo -N: solo se avisa y se mide el tiempo extra");
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
        t->completed = handle_request(type, payload, t);
        /* Tiempo real = tiempo total menos la espera aleatoria de la simulacion */
        t->duration = now_seconds() - start - t->sim_wait;
        if (t->duration < 0)
            t->duration = 0;
 
        task_count++;
        if (csv_file != NULL)
            write_csv_row(task_count, t);
 
        if (t->chaos) {
            printf("\n[Box %d] El caos planetario obliga a terminar el programa.\n", getpid());
            break;
        }
    }
 
    if (csv_file != NULL)
        fclose(csv_file);
    shared_cleanup();
    print_report();
    return 0;
}