#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 64
#define CHILD_COMMAND_LEN 256
#define DEFAULT_SOFT_LIMIT (48UL * 1024 * 1024)
#define DEFAULT_HARD_LIMIT (80UL * 1024 * 1024)

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    char log_path[PATH_MAX];
    int stop_requested;
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;
static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag, const char *value, unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;
    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }
    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }
    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req, int argc, char *argv[], int start_index)
{
    int i;
    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;
        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }
        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr, "Invalid value for --nice (expected -20..19): %s\n", argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }
        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }
    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }
    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING: return "starting";
    case CONTAINER_RUNNING:  return "running";
    case CONTAINER_STOPPED:  return "stopped";
    case CONTAINER_KILLED:   return "killed";
    case CONTAINER_EXITED:   return "exited";
    default:                 return "unknown";
    }
}

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;
    memset(buffer, 0, sizeof(*buffer));
    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0) return rc;
    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) { pthread_mutex_destroy(&buffer->mutex); return rc; }
    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }
    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

static int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);
    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);
    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }
    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;
    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

static int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);
    while (buffer->count == 0 && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);
    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }
    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;
    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}
static void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;
    while (bounded_buffer_pop(&ctx->log_buffer, &item) == 0) {
        char log_path[PATH_MAX];
        snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, item.container_id);
        int fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            write(fd, item.data, item.length);
            close(fd);
        }
    }
    return NULL;
}

static int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    if (cfg->nice_value != 0)
        nice(cfg->nice_value);

    if (chroot(cfg->rootfs) != 0) {
        perror("chroot");
        return 1;
    }
    if (chdir("/") != 0) {
        perror("chdir");
        return 1;
    }

    mount("proc", "/proc", "proc", 0, NULL);

    dup2(cfg->log_write_fd, STDOUT_FILENO);
    dup2(cfg->log_write_fd, STDERR_FILENO);
    close(cfg->log_write_fd);

    char *args[] = { "/bin/sh", "-c", cfg->command, NULL };
    execv("/bin/sh", args);
    perror("execv");
    return 1;
}
static container_record_t *find_container(supervisor_ctx_t *ctx, const char *id)
{
    container_record_t *c = ctx->containers;
    while (c) {
        if (strcmp(c->id, id) == 0) return c;
        c = c->next;
    }
    return NULL;
}

static container_record_t *create_container(supervisor_ctx_t *ctx, const control_request_t *req)
{
    container_record_t *c = calloc(1, sizeof(container_record_t));
    if (!c) return NULL;
    strncpy(c->id, req->container_id, CONTAINER_ID_LEN - 1);
    c->state = CONTAINER_STARTING;
    c->started_at = time(NULL);
    c->soft_limit_bytes = req->soft_limit_bytes ? req->soft_limit_bytes : DEFAULT_SOFT_LIMIT;
    c->hard_limit_bytes = req->hard_limit_bytes ? req->hard_limit_bytes : DEFAULT_HARD_LIMIT;
    snprintf(c->log_path, PATH_MAX, "%s/%s.log", LOG_DIR, c->id);
    c->next = ctx->containers;
    ctx->containers = c;
    return c;
}

static int launch_container(supervisor_ctx_t *ctx, const control_request_t *req,
                            control_response_t *resp)
{
    int pipefd[2];
    char *stack;
    pid_t pid;
    child_config_t *cfg;
    container_record_t *rec;

    pthread_mutex_lock(&ctx->metadata_lock);
    if (find_container(ctx, req->container_id)) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        resp->status = -1;
        snprintf(resp->message, CONTROL_MESSAGE_LEN,
                 "Container %s already exists", req->container_id);
        return -1;
    }
    rec = create_container(ctx, req);
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (!rec) {
        resp->status = -1;
        snprintf(resp->message, CONTROL_MESSAGE_LEN, "Out of memory");
        return -1;
    }

    if (pipe(pipefd) != 0) {
        resp->status = -1;
        snprintf(resp->message, CONTROL_MESSAGE_LEN, "pipe() failed: %s", strerror(errno));
        return -1;
    }

    mkdir(LOG_DIR, 0755);

    cfg = calloc(1, sizeof(child_config_t));
    strncpy(cfg->id, req->container_id, CONTAINER_ID_LEN - 1);
    strncpy(cfg->rootfs, req->rootfs, PATH_MAX - 1);
    strncpy(cfg->command, req->command, CHILD_COMMAND_LEN - 1);
    cfg->nice_value = req->nice_value;
    cfg->log_write_fd = pipefd[1];

    stack = malloc(STACK_SIZE);
    pid = clone(child_fn, stack + STACK_SIZE,
                CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWIPC | SIGCHLD,
                cfg);

    close(pipefd[1]);

    if (pid < 0) {
        close(pipefd[0]);
        free(stack);
        free(cfg);
        resp->status = -1;
        snprintf(resp->message, CONTROL_MESSAGE_LEN, "clone() failed: %s", strerror(errno));
        return -1;
    }

    pthread_mutex_lock(&ctx->metadata_lock);
    rec->host_pid = pid;
    rec->state = CONTAINER_RUNNING;
    if (ctx->monitor_fd >= 0) {
    	struct monitor_request mreq;
    	mreq.pid = pid;
    	mreq.soft_limit_bytes = rec->soft_limit_bytes;
    	mreq.hard_limit_bytes = rec->hard_limit_bytes;
    	strncpy(mreq.container_id, rec->id, MONITOR_NAME_LEN - 1);
    	ioctl(ctx->monitor_fd, MONITOR_REGISTER, &mreq);
     }
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Reader thread to push logs into bounded buffer */
    {
        int *read_fd = malloc(sizeof(int));
        char *cid = malloc(CONTAINER_ID_LEN);
        pthread_t t;
        typedef struct { int fd; char id[CONTAINER_ID_LEN]; bounded_buffer_t *buf; } reader_arg_t;
        reader_arg_t *ra = malloc(sizeof(reader_arg_t));
        ra->fd = pipefd[0];
        strncpy(ra->id, req->container_id, CONTAINER_ID_LEN - 1);
        ra->buf = &ctx->log_buffer;
        free(read_fd); free(cid);

        extern void *log_reader_thread(void *);
        pthread_create(&t, NULL, log_reader_thread, ra);
        pthread_detach(t);
    }

    resp->status = 0;
    snprintf(resp->message, CONTROL_MESSAGE_LEN, "%s", req->container_id);
    return 0;
}

void *log_reader_thread(void *arg)
{
    typedef struct { int fd; char id[CONTAINER_ID_LEN]; bounded_buffer_t *buf; } reader_arg_t;
    reader_arg_t *ra = (reader_arg_t *)arg;
    log_item_t item;
    ssize_t n;
    memset(&item, 0, sizeof(item));
    strncpy(item.container_id, ra->id, CONTAINER_ID_LEN - 1);
    while ((n = read(ra->fd, item.data, LOG_CHUNK_SIZE)) > 0) {
        item.length = (size_t)n;
        bounded_buffer_push(ra->buf, &item);
        memset(item.data, 0, LOG_CHUNK_SIZE);
    }
    close(ra->fd);
    free(ra);
    return NULL;
}
static void handle_ps(supervisor_ctx_t *ctx, control_response_t *resp)
{
    char buf[CONTROL_MESSAGE_LEN * 8] = {0};
    char line[256];
    container_record_t *c;
    pthread_mutex_lock(&ctx->metadata_lock);
    c = ctx->containers;
    while (c) {
        snprintf(line, sizeof(line), "%s\t%d\t%s\n",
                 c->id, c->host_pid, state_to_string(c->state));
        strncat(buf, line, sizeof(buf) - strlen(buf) - 1);
        c = c->next;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);
    resp->status = 0;
    strncpy(resp->message, buf[0] ? buf : "no containers\n", CONTROL_MESSAGE_LEN - 1);
}

static void handle_logs(supervisor_ctx_t *ctx, const char *id, control_response_t *resp)
{
    char log_path[PATH_MAX];
    char buf[CONTROL_MESSAGE_LEN];
    int fd;
    ssize_t n;
    snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, id);
    fd = open(log_path, O_RDONLY);
    if (fd < 0) {
        resp->status = -1;
        snprintf(resp->message, CONTROL_MESSAGE_LEN, "No logs for %s", id);
        return;
    }
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n < 0) n = 0;
    buf[n] = '\0';
    resp->status = 0;
    strncpy(resp->message, buf, CONTROL_MESSAGE_LEN - 1);
}

static void handle_stop(supervisor_ctx_t *ctx, const char *id, control_response_t *resp)
{
    container_record_t *c;
    pthread_mutex_lock(&ctx->metadata_lock);
    c = find_container(ctx, id);
    if (!c) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        resp->status = -1;
        snprintf(resp->message, CONTROL_MESSAGE_LEN, "No such container: %s", id);
        return;
    }
    c->stop_requested = 1;
    if (c->state == CONTAINER_RUNNING) {
        kill(c->host_pid, SIGTERM);
        if (ctx->monitor_fd >= 0) {
    		struct monitor_request mreq;
    		mreq.pid = c->host_pid;
    		strncpy(mreq.container_id, c->id, MONITOR_NAME_LEN - 1);
    		ioctl(ctx->monitor_fd, MONITOR_UNREGISTER, &mreq);
	}
        c->state = CONTAINER_STOPPED;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);
    resp->status = 0;
    snprintf(resp->message, CONTROL_MESSAGE_LEN, "Stopped %s", id);
}

static void reap_children(supervisor_ctx_t *ctx)
{
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        container_record_t *c;
        pthread_mutex_lock(&ctx->metadata_lock);
        c = ctx->containers;
        while (c) {
            if (c->host_pid == pid) {
                if (WIFEXITED(status)) {
                    c->exit_code = WEXITSTATUS(status);
                    c->state = CONTAINER_EXITED;
                } else if (WIFSIGNALED(status)) {
                    c->exit_signal = WTERMSIG(status);
                    c->state = CONTAINER_KILLED;
                }
                break;
            }
            c = c->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
    }
}

static volatile sig_atomic_t g_got_sigchld = 0;
static volatile sig_atomic_t g_should_stop = 0;

static void sigchld_handler(int sig) { (void)sig; g_got_sigchld = 1; }
static void sigterm_handler(int sig) { (void)sig; g_should_stop = 1; }

static int run_supervisor(supervisor_ctx_t *ctx)
{
    struct sockaddr_un addr;
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);
    sa.sa_handler = sigterm_handler;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    mkdir(LOG_DIR, 0755);

    ctx->server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx->server_fd < 0) { perror("socket"); return 1; }

    unlink(CONTROL_PATH);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx->server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(ctx->server_fd, 8) < 0) { perror("listen"); return 1; }

    bounded_buffer_init(&ctx->log_buffer);
    pthread_mutex_init(&ctx->metadata_lock, NULL);
    pthread_create(&ctx->logger_thread, NULL, logging_thread, ctx);
    
    ctx->monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx->monitor_fd < 0)
    	fprintf(stderr, "Warning: could not open monitor device: %s\n", strerror(errno));
    fprintf(stdout, "supervisor ready\n");
    fflush(stdout);

    while (!g_should_stop) {
        fd_set rfds;
        struct timeval tv = {1, 0};
        int rc;

        if (g_got_sigchld) { g_got_sigchld = 0; reap_children(ctx); }

        FD_ZERO(&rfds);
        FD_SET(ctx->server_fd, &rfds);
        rc = select(ctx->server_fd + 1, &rfds, NULL, NULL, &tv);
        if (rc < 0 && errno == EINTR) continue;
        if (rc <= 0) continue;

        {
            int client_fd = accept(ctx->server_fd, NULL, NULL);
            control_request_t req;
            control_response_t resp = {0};
            if (client_fd < 0) continue;
            if (read(client_fd, &req, sizeof(req)) != sizeof(req)) {
                close(client_fd); continue;
            }
            switch (req.kind) {
            case CMD_START:
            case CMD_RUN:
                launch_container(ctx, &req, &resp);
                break;
            case CMD_PS:
                handle_ps(ctx, &resp);
                break;
            case CMD_LOGS:
                handle_logs(ctx, req.container_id, &resp);
                break;
            case CMD_STOP:
                handle_stop(ctx, req.container_id, &resp);
                break;
            default:
                resp.status = -1;
                snprintf(resp.message, CONTROL_MESSAGE_LEN, "Unknown command");
            }
            write(client_fd, &resp, sizeof(resp));
            close(client_fd);
        }
    }

    reap_children(ctx);
    bounded_buffer_begin_shutdown(&ctx->log_buffer);
    pthread_join(ctx->logger_thread, NULL);
    bounded_buffer_destroy(&ctx->log_buffer);
    pthread_mutex_destroy(&ctx->metadata_lock);
    close(ctx->server_fd);
    unlink(CONTROL_PATH);
    return 0;
}

static int send_control_request(const control_request_t *req)
{
    int fd;
    struct sockaddr_un addr;
    control_response_t resp;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect"); close(fd); return 1;
    }
    write(fd, req, sizeof(*req));
    if (read(fd, &resp, sizeof(resp)) == sizeof(resp)) {
        printf("%s\n", resp.message);
    }
    close(fd);
    return 0;
}

int main(int argc, char *argv[])
{
    control_request_t req;
    supervisor_ctx_t ctx;

    if (argc < 2) { usage(argv[0]); return 1; }

    memset(&req, 0, sizeof(req));
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (strcmp(argv[1], "supervisor") == 0) {
        memset(&ctx, 0, sizeof(ctx));
        return run_supervisor(&ctx);
    } else if (strcmp(argv[1], "start") == 0 || strcmp(argv[1], "run") == 0) {
        if (argc < 5) { usage(argv[0]); return 1; }
        req.kind = (strcmp(argv[1], "start") == 0) ? CMD_START : CMD_RUN;
        strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
        strncpy(req.rootfs, argv[3], PATH_MAX - 1);
        strncpy(req.command, argv[4], CHILD_COMMAND_LEN - 1);
        if (argc > 5 && parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
        return send_control_request(&req);
    } else if (strcmp(argv[1], "ps") == 0) {
        req.kind = CMD_PS;
        return send_control_request(&req);
    } else if (strcmp(argv[1], "logs") == 0) {
        if (argc < 3) { usage(argv[0]); return 1; }
        req.kind = CMD_LOGS;
        strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
        return send_control_request(&req);
    } else if (strcmp(argv[1], "stop") == 0) {
        if (argc < 3) { usage(argv[0]); return 1; }
        req.kind = CMD_STOP;
        strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
        return send_control_request(&req);
    } else {
        usage(argv[0]);
        return 1;
    }
}
