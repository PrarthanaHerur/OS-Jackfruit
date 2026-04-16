#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sched.h>
#include <time.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include "monitor_ioctl.h"
/* ─── CONSTANTS ─── */
#define MAX_CONTAINERS   16
#define STACK_SIZE       (1024 * 1024)
#define DEFAULT_SOFT_MIB 40
#define DEFAULT_HARD_MIB 64
#define SOCK_PATH        "/tmp/engine.sock"
#define LOG_DIR          "logs"
#define LOG_BUFFER_SIZE  64
#define LOG_LINE_MAX     1024

/* ─── CONTAINER STATES ─── */
typedef enum {
    STATE_STARTING,
    STATE_RUNNING,
    STATE_STOPPED,
    STATE_KILLED,
    STATE_HARD_LIMIT_KILLED
} ContainerState;

/* ─── CONTAINER METADATA ─── */
typedef struct {
    char            id[64];
    char            rootfs[256];
    char            command[256];
    pid_t           pid;
    time_t          start_time;
    ContainerState  state;
    int             soft_mib;
    int             hard_mib;
    int             nice_val;
    char            log_path[256];
    int             exit_code;
    int             stop_requested;

    /* logging pipe */
    int             pipe_fd[2];  /* pipe_fd[0]=read, pipe_fd[1]=write */
    pthread_t       log_thread;
} Container;

/* ─── LOG BUFFER (bounded) ─── */
typedef struct {
    char        lines[LOG_BUFFER_SIZE][LOG_LINE_MAX];
    int         head, tail, count;
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
    int         done;       /* set to 1 when producer is finished */
    char        log_path[256];
} LogBuffer;

/* ─── GLOBALS ─── */
static Container containers[MAX_CONTAINERS];
static int       container_count = 0;
static pthread_mutex_t meta_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int supervisor_running = 1;

/* ════════════════════════════════════════════
   HELPER: find container by id
   ════════════════════════════════════════════ */
static Container *find_container(const char *id) {
    for (int i = 0; i < container_count; i++)
        if (strcmp(containers[i].id, id) == 0)
            return &containers[i];
    return NULL;
}

/* ════════════════════════════════════════════
   LOGGING: consumer thread
   writes from bounded buffer to log file
   ════════════════════════════════════════════ */
static void *log_consumer(void *arg) {
    LogBuffer *buf = (LogBuffer *)arg;
    FILE *f = fopen(buf->log_path, "a");
    if (!f) { perror("fopen log"); return NULL; }

    while (1) {
        pthread_mutex_lock(&buf->lock);
        /* wait until there's something to consume */
        while (buf->count == 0 && !buf->done)
            pthread_cond_wait(&buf->not_empty, &buf->lock);

        if (buf->count == 0 && buf->done) {
            pthread_mutex_unlock(&buf->lock);
            break;
        }

        char line[LOG_LINE_MAX];
        strncpy(line, buf->lines[buf->head], LOG_LINE_MAX);
        buf->head = (buf->head + 1) % LOG_BUFFER_SIZE;
        buf->count--;
        pthread_cond_signal(&buf->not_full);
        pthread_mutex_unlock(&buf->lock);

        fputs(line, f);
        fflush(f);
    }

    /* flush remaining */
    pthread_mutex_lock(&buf->lock);
    while (buf->count > 0) {
        fputs(buf->lines[buf->head], f);
        buf->head = (buf->head + 1) % LOG_BUFFER_SIZE;
        buf->count--;
    }
    pthread_mutex_unlock(&buf->lock);

    fclose(f);
    free(buf);
    return NULL;
}

/* ════════════════════════════════════════════
   LOGGING: producer thread
   reads from pipe, pushes to bounded buffer
   ════════════════════════════════════════════ */
typedef struct { int fd; LogBuffer *buf; } ProducerArg;

static void *log_producer(void *arg) {
    ProducerArg *pa = (ProducerArg *)arg;
    int fd = pa->fd;
    LogBuffer *buf = pa->buf;
    free(pa);

    char line[LOG_LINE_MAX];
    FILE *pipe_file = fdopen(fd, "r");
    if (!pipe_file) { perror("fdopen"); return NULL; }

    while (fgets(line, sizeof(line), pipe_file)) {
        pthread_mutex_lock(&buf->lock);
        while (buf->count == LOG_BUFFER_SIZE)
            pthread_cond_wait(&buf->not_full, &buf->lock);

        strncpy(buf->lines[buf->tail], line, LOG_LINE_MAX);
        buf->tail = (buf->tail + 1) % LOG_BUFFER_SIZE;
        buf->count++;
        pthread_cond_signal(&buf->not_empty);
        pthread_mutex_unlock(&buf->lock);
    }

    /* signal consumer we're done */
    pthread_mutex_lock(&buf->lock);
    buf->done = 1;
    pthread_cond_signal(&buf->not_empty);
    pthread_mutex_unlock(&buf->lock);

    fclose(pipe_file);
    return NULL;
}

/* ════════════════════════════════════════════
   CONTAINER CHILD: runs inside the namespace
   ════════════════════════════════════════════ */
typedef struct {
    char rootfs[256];
    char command[256];
    int  nice_val;
    int  pipe_write_fd;
} ChildArgs;

static int container_child(void *arg) {
    ChildArgs *a = (ChildArgs *)arg;

    /* redirect stdout and stderr into the pipe */
    dup2(a->pipe_write_fd, STDOUT_FILENO);
    dup2(a->pipe_write_fd, STDERR_FILENO);
    close(a->pipe_write_fd);

    /* set hostname to something identifiable */
    sethostname("container", 9);

    /* chroot into the container's rootfs */
    if (chroot(a->rootfs) != 0) {
        perror("chroot");
        return 1;
    }
    chdir("/");

    /* mount /proc so ps, top etc. work inside */
    mount("proc", "/proc", "proc", 0, NULL);

    /* apply nice value if set */
    if (a->nice_val != 0)
        nice(a->nice_val);

    /* exec the requested command */
    char *argv[] = { a->command, NULL };
    execv(a->command, argv);

    /* if execv returns, something went wrong */
    perror("execv");
    return 1;
}

/* ════════════════════════════════════════════
   LAUNCH a container
   ════════════════════════════════════════════ */
static int launch_container(Container *c) {
    /* create the log directory if needed */
    mkdir(LOG_DIR, 0755);
    snprintf(c->log_path, sizeof(c->log_path), "%s/%s.log", LOG_DIR, c->id);

    /* create pipe: container writes to [1], supervisor reads from [0] */
    if (pipe(c->pipe_fd) != 0) {
        perror("pipe");
        return -1;
    }

    /* set up the bounded log buffer */
    LogBuffer *buf = calloc(1, sizeof(LogBuffer));
    pthread_mutex_init(&buf->lock, NULL);
    pthread_cond_init(&buf->not_empty, NULL);
    pthread_cond_init(&buf->not_full, NULL);
    strncpy(buf->log_path, c->log_path, sizeof(buf->log_path));

    /* start consumer thread */
    pthread_t consumer_tid;
    pthread_create(&consumer_tid, NULL, log_consumer, buf);
    pthread_detach(consumer_tid);

    /* start producer thread */
    ProducerArg *pa = malloc(sizeof(ProducerArg));
    pa->fd  = c->pipe_fd[0];
    pa->buf = buf;
    pthread_create(&c->log_thread, NULL, log_producer, pa);

    /* set up child args */
    static ChildArgs ca;   /* static so it survives after this function */
    strncpy(ca.rootfs,  c->rootfs,  sizeof(ca.rootfs));
    strncpy(ca.command, c->command, sizeof(ca.command));
    ca.nice_val     = c->nice_val;
    ca.pipe_write_fd = c->pipe_fd[1];

    /* allocate stack for clone() */
    char *stack = malloc(STACK_SIZE);
    char *stack_top = stack + STACK_SIZE;  /* stack grows downward */

    /* clone with namespace flags */
    int flags = SIGCHLD | CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS;
    pid_t pid = clone(container_child, stack_top, flags, &ca);

    if (pid < 0) {
        perror("clone");
        free(stack);
        return -1;
    }

    /* supervisor closes the write end — only child writes to it */
    close(c->pipe_fd[1]);

    c->pid        = pid;
    c->start_time = time(NULL);
    c->state      = STATE_RUNNING;

    printf("[supervisor] container '%s' started, pid=%d\n", c->id, pid);
    /* register with kernel monitor */
    int mfd = open("/dev/container_monitor", O_RDWR);
    if (mfd >= 0) {
        struct monitor_request req;
        memset(&req, 0, sizeof(req));
        req.pid = pid;
        req.soft_limit_bytes = (unsigned long)c->soft_mib * 1024 * 1024;
        req.hard_limit_bytes = (unsigned long)c->hard_mib * 1024 * 1024;
        strncpy(req.container_id, c->id, MONITOR_NAME_LEN-1);
        ioctl(mfd, MONITOR_REGISTER, &req);
        close(mfd);
    }
    return 0;
}

/* ════════════════════════════════════════════
   SIGCHLD handler — reap dead children
   ════════════════════════════════════════════ */
static void sigchld_handler(int sig) {
    (void)sig;
    int status;
    pid_t pid;

    /* WNOHANG = don't block, just check */
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&meta_lock);
        for (int i = 0; i < container_count; i++) {
            if (containers[i].pid == pid) {
                if (WIFEXITED(status)) {
                    containers[i].exit_code = WEXITSTATUS(status);
                    containers[i].state = STATE_STOPPED;
                } else if (WIFSIGNALED(status)) {
                    int sig2 = WTERMSIG(status);
                    containers[i].exit_code = 128 + sig2;
                    if (sig2 == SIGKILL && !containers[i].stop_requested)
                        containers[i].state = STATE_HARD_LIMIT_KILLED;
                    else
                        containers[i].state = STATE_KILLED;
                }
                printf("[supervisor] container '%s' (pid=%d) exited\n",
                       containers[i].id, pid);
                break;
            }
        }
        pthread_mutex_unlock(&meta_lock);
    }
}

/* ════════════════════════════════════════════
   SIGTERM/SIGINT handler — orderly shutdown
   ════════════════════════════════════════════ */
static void sigterm_handler(int sig) {
    (void)sig;
    printf("\n[supervisor] shutting down...\n");
    supervisor_running = 0;
}

/* ════════════════════════════════════════════
   CLI COMMAND HANDLERS (run in supervisor)
   ════════════════════════════════════════════ */
static void handle_ps(int client_fd) {
    char buf[4096] = {0};
    char line[256];
    pthread_mutex_lock(&meta_lock);

    snprintf(line, sizeof(line),
        "%-12s %-8s %-10s %-6s %-6s %s\n",
        "ID", "PID", "STATE", "SOFT", "HARD", "STARTED");
    strncat(buf, line, sizeof(buf) - strlen(buf) - 1);

    const char *state_str[] = {
        "starting", "running", "stopped", "killed", "hard_killed"
    };

    for (int i = 0; i < container_count; i++) {
        Container *c = &containers[i];
        char tstr[32];
        struct tm *tm = localtime(&c->start_time);
        strftime(tstr, sizeof(tstr), "%H:%M:%S", tm);

        snprintf(line, sizeof(line),
            "%-12s %-8d %-10s %-6d %-6d %s\n",
            c->id, c->pid,
            state_str[c->state],
            c->soft_mib, c->hard_mib, tstr);
        strncat(buf, line, sizeof(buf) - strlen(buf) - 1);
    }
    pthread_mutex_unlock(&meta_lock);
    write(client_fd, buf, strlen(buf));
}

static void handle_logs(int client_fd, const char *id) {
    pthread_mutex_lock(&meta_lock);
    Container *c = find_container(id);
    char log_path[256] = {0};
    if (c) strncpy(log_path, c->log_path, sizeof(log_path));
    pthread_mutex_unlock(&meta_lock);

    if (!c) {
        write(client_fd, "error: container not found\n", 27);
        return;
    }

    FILE *f = fopen(log_path, "r");
    if (!f) {
        write(client_fd, "no logs yet\n", 12);
        return;
    }
    char line[1024];
    while (fgets(line, sizeof(line), f))
        write(client_fd, line, strlen(line));
    fclose(f);
}

static void handle_stop(int client_fd, const char *id) {
    pthread_mutex_lock(&meta_lock);
    Container *c = find_container(id);
    if (!c) {
        pthread_mutex_unlock(&meta_lock);
        write(client_fd, "error: not found\n", 17);
        return;
    }
    c->stop_requested = 1;
    kill(c->pid, SIGTERM);
    pthread_mutex_unlock(&meta_lock);
    write(client_fd, "ok\n", 3);
}

static void handle_start(int client_fd, char *args) {
    /* args format: <id> <rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N] */
    char id[64], rootfs[256], command[256];
    int soft = DEFAULT_SOFT_MIB, hard = DEFAULT_HARD_MIB, nval = 0;

    char *tok = strtok(args, " ");
    if (!tok) goto bad;
    strncpy(id, tok, sizeof(id));

    tok = strtok(NULL, " ");
    if (!tok) goto bad;
    strncpy(rootfs, tok, sizeof(rootfs));

    tok = strtok(NULL, " ");
    if (!tok) goto bad;
    strncpy(command, tok, sizeof(command));

    /* parse optional flags */
    while ((tok = strtok(NULL, " ")) != NULL) {
        if (strcmp(tok, "--soft-mib") == 0) {
            tok = strtok(NULL, " ");
            if (tok) soft = atoi(tok);
        } else if (strcmp(tok, "--hard-mib") == 0) {
            tok = strtok(NULL, " ");
            if (tok) hard = atoi(tok);
        } else if (strcmp(tok, "--nice") == 0) {
            tok = strtok(NULL, " ");
            if (tok) nval = atoi(tok);
        }
    }

    pthread_mutex_lock(&meta_lock);
    if (container_count >= MAX_CONTAINERS) {
        pthread_mutex_unlock(&meta_lock);
        write(client_fd, "error: max containers reached\n", 30);
        return;
    }

    Container *c = &containers[container_count++];
    memset(c, 0, sizeof(*c));
    strncpy(c->id,      id,      sizeof(c->id));
    strncpy(c->rootfs,  rootfs,  sizeof(c->rootfs));
    strncpy(c->command, command, sizeof(c->command));
    c->soft_mib = soft;
    c->hard_mib = hard;
    c->nice_val = nval;
    c->state    = STATE_STARTING;
    pthread_mutex_unlock(&meta_lock);

    if (launch_container(c) != 0) {
        write(client_fd, "error: launch failed\n", 21);
        return;
    }
    write(client_fd, "ok\n", 3);
    return;

bad:
    write(client_fd, "error: bad arguments\n", 21);
}

/* ════════════════════════════════════════════
   SUPERVISOR: main loop
   listens on UNIX socket for CLI commands
   ════════════════════════════════════════════ */
static void run_supervisor(const char *rootfs_base) {
    (void)rootfs_base;

    /* set up signal handlers */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sa.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    sa.sa_handler = sigterm_handler;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    /* create UNIX domain socket */
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);

    unlink(SOCK_PATH);   /* remove stale socket if any */
    bind(server_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(server_fd, 8);

    /* make accept() non-blocking so we can check supervisor_running */
    fcntl(server_fd, F_SETFL, O_NONBLOCK);

    printf("[supervisor] ready, listening on %s\n", SOCK_PATH);

    while (supervisor_running) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(100000);   /* 100ms sleep, then retry */
                continue;
            }
            break;
        }

        /* read the command line */
        char cmd[1024] = {0};
        read(client_fd, cmd, sizeof(cmd) - 1);

        /* strip trailing newline */
        cmd[strcspn(cmd, "\n")] = 0;

        /* dispatch */
        if (strncmp(cmd, "ps", 2) == 0) {
            handle_ps(client_fd);
        } else if (strncmp(cmd, "logs ", 5) == 0) {
            handle_logs(client_fd, cmd + 5);
        } else if (strncmp(cmd, "stop ", 5) == 0) {
            handle_stop(client_fd, cmd + 5);
        } else if (strncmp(cmd, "start ", 6) == 0) {
            handle_start(client_fd, cmd + 6);
        } else {
            write(client_fd, "unknown command\n", 16);
        }

        close(client_fd);
    }

    /* cleanup on shutdown */
    close(server_fd);
    unlink(SOCK_PATH);

    /* stop all running containers */
    pthread_mutex_lock(&meta_lock);
    for (int i = 0; i < container_count; i++)
        if (containers[i].state == STATE_RUNNING)
            kill(containers[i].pid, SIGTERM);
    pthread_mutex_unlock(&meta_lock);

    /* wait for all children */
    while (wait(NULL) > 0);
    printf("[supervisor] exited cleanly\n");
}

/* ════════════════════════════════════════════
   CLI CLIENT: sends command to supervisor
   ════════════════════════════════════════════ */
static void run_cli(int argc, char *argv[]) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "error: cannot connect to supervisor. Is it running?\n");
        exit(1);
    }

    /* build command string from argv */
    char cmd[1024] = {0};
    for (int i = 1; i < argc; i++) {
        strncat(cmd, argv[i], sizeof(cmd) - strlen(cmd) - 2);
        if (i < argc - 1)
            strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
    }
    strncat(cmd, "\n", sizeof(cmd) - strlen(cmd) - 1);
    write(fd, cmd, strlen(cmd));

    /* print response */
    char buf[4096];
    int n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(STDOUT_FILENO, buf, n);

    close(fd);
}

/* ════════════════════════════════════════════
   MAIN
   ════════════════════════════════════════════ */
int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr,
            "Usage:\n"
            "  engine supervisor <rootfs-base>\n"
            "  engine start <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  engine stop  <id>\n"
            "  engine ps\n"
            "  engine logs  <id>\n"
        );
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: engine supervisor <rootfs-base>\n");
            return 1;
        }
        run_supervisor(argv[2]);
    } else {
        run_cli(argc, argv);
    }

    return 0;
}
