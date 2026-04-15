/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 */

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
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 2048
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

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
    if (errno != 0 || end == value || *end != '\0') return -1;
    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req, int argc, char *argv[], int start_index)
{
    for (int i = start_index; i < argc; i += 2) {
        if (i + 1 >= argc) return -1;
        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0) return -1;
        } else if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0) return -1;
        } else if (strcmp(argv[i], "--nice") == 0) {
            req->nice_value = atoi(argv[i+1]);
        }
    }
    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
        case CONTAINER_STARTING: return "starting";
        case CONTAINER_RUNNING: return "running";
        case CONTAINER_STOPPED: return "stopped";
        case CONTAINER_KILLED: return "killed";
        case CONTAINER_EXITED: return "exited";
        default: return "unknown";
    }
}

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    memset(buffer, 0, sizeof(*buffer));
    pthread_mutex_init(&buffer->mutex, NULL);
    pthread_cond_init(&buffer->not_empty, NULL);
    pthread_cond_init(&buffer->not_full, NULL);
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

int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
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

int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
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

void *logging_thread(void *arg)
{
    bounded_buffer_t *buffer = (bounded_buffer_t *)arg;
    log_item_t item;
    char path[PATH_MAX];
    while (bounded_buffer_pop(buffer, &item) == 0) {
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);
        int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            if (write(fd, item.data, item.length) < 0) perror("write log");
            close(fd);
        }
    }
    return NULL;
}

int child_fn(void *arg)
{
    child_config_t *config = (child_config_t *)arg;
    sethostname(config->id, strlen(config->id));
    mount("none", "/proc", "proc", 0, NULL);
    if (chroot(config->rootfs) < 0 || chdir("/") < 0) return 1;
    mount("proc", "/proc", "proc", 0, NULL);
    setpriority(PRIO_PROCESS, 0, config->nice_value);
    dup2(config->log_write_fd, STDOUT_FILENO);
    dup2(config->log_write_fd, STDERR_FILENO);
    close(config->log_write_fd);
    char *argv[] = {"/bin/sh", "-c", config->command, NULL};
    execv("/bin/sh", argv);
    return 1;
}

int register_with_monitor(int fd, const char *id, pid_t pid, unsigned long soft, unsigned long hard)
{
    struct monitor_request req = {0};
    req.pid = pid;
    req.soft_limit_bytes = soft;
    req.hard_limit_bytes = hard;
    strncpy(req.container_id, id, sizeof(req.container_id) - 1);
    return ioctl(fd, MONITOR_REGISTER, &req);
}

static void add_container(supervisor_ctx_t *ctx, container_record_t *rec)
{
    pthread_mutex_lock(&ctx->metadata_lock);
    rec->next = ctx->containers;
    ctx->containers = rec;
    pthread_mutex_unlock(&ctx->metadata_lock);
}

static container_record_t *find_container(supervisor_ctx_t *ctx, const char *id)
{
    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *curr = ctx->containers;
    while (curr && strcmp(curr->id, id) != 0) curr = curr->next;
    pthread_mutex_unlock(&ctx->metadata_lock);
    return curr;
}

static supervisor_ctx_t *g_ctx = NULL;
static void sigchld_handler(int sig) { (void)sig; while (waitpid(-1, NULL, WNOHANG) > 0); }
static void shutdown_handler(int sig) { (void)sig; if (g_ctx) g_ctx->should_stop = 1; }

static void *producer_thread(void *arg)
{
    struct { int fd; char id[CONTAINER_ID_LEN]; bounded_buffer_t *buffer; } *info = arg;
    char chunk[LOG_CHUNK_SIZE];
    ssize_t n;
    while ((n = read(info->fd, chunk, sizeof(chunk))) > 0) {
        log_item_t item = {0};
        strncpy(item.container_id, info->id, sizeof(item.container_id) - 1);
        item.length = n;
        memcpy(item.data, chunk, n);
        if (bounded_buffer_push(info->buffer, &item) != 0) break;
    }
    close(info->fd); free(info); return NULL;
}

static void handle_request(supervisor_ctx_t *ctx, int client_fd, control_request_t *req)
{
    control_response_t res = {0, "OK"};
    if (req->kind == CMD_START || req->kind == CMD_RUN) {
        int p[2];
        if (pipe(p) == 0) {
            child_config_t *cfg = malloc(sizeof(*cfg));
            strncpy(cfg->id, req->container_id, sizeof(cfg->id)-1);
            strncpy(cfg->rootfs, req->rootfs, sizeof(cfg->rootfs)-1);
            strncpy(cfg->command, req->command, sizeof(cfg->command)-1);
            cfg->nice_value = req->nice_value;
            cfg->log_write_fd = p[1];
            char *stack = malloc(STACK_SIZE);
            pid_t pid = clone(child_fn, stack + STACK_SIZE, CLONE_NEWNS|CLONE_NEWPID|CLONE_NEWUTS|SIGCHLD, cfg);
            if (pid > 0) {
                close(p[1]);
                container_record_t *rec = calloc(1, sizeof(*rec));
                strncpy(rec->id, req->container_id, sizeof(rec->id)-1);
                rec->host_pid = pid; rec->state = CONTAINER_RUNNING;
                rec->soft_limit_bytes = req->soft_limit_bytes; rec->hard_limit_bytes = req->hard_limit_bytes;
                add_container(ctx, rec);
                struct { int fd; char id[CONTAINER_ID_LEN]; bounded_buffer_t *buffer; } *pinfo = malloc(sizeof(*pinfo));
                pinfo->fd = p[0]; strncpy(pinfo->id, rec->id, sizeof(pinfo->id)-1); pinfo->buffer = &ctx->log_buffer;
                pthread_t pt; pthread_create(&pt, NULL, producer_thread, pinfo); pthread_detach(pt);
                if (ctx->monitor_fd >= 0) register_with_monitor(ctx->monitor_fd, rec->id, pid, rec->soft_limit_bytes, rec->hard_limit_bytes);
                if (req->kind == CMD_RUN) {
                    int status; waitpid(pid, &status, 0);
                    rec->state = CONTAINER_EXITED;
                    snprintf(res.message, sizeof(res.message), "Container exited");
                } else snprintf(res.message, sizeof(res.message), "Container started PID %d", pid);
            } else { res.status = 1; snprintf(res.message, sizeof(res.message), "clone failed"); close(p[0]); close(p[1]); free(cfg); free(stack); }
        }
    } else if (req->kind == CMD_PS) {
        strcpy(res.message, "ID\tPID\tSTATE\n");
        pthread_mutex_lock(&ctx->metadata_lock);
        for (container_record_t *c = ctx->containers; c; c = c->next) {
            char l[128]; snprintf(l, sizeof(l), "%s\t%d\t%s\n", c->id, c->host_pid, state_to_string(c->state));
            strncat(res.message, l, sizeof(res.message)-strlen(res.message)-1);
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
    } else if (req->kind == CMD_STOP) {
        container_record_t *c = find_container(ctx, req->container_id);
        if (c && c->state == CONTAINER_RUNNING) { kill(c->host_pid, SIGTERM); c->state = CONTAINER_STOPPED; snprintf(res.message, sizeof(res.message), "Stopped %s", c->id); }
        else { res.status = 1; strcpy(res.message, "Not found/running"); }
    } else if (req->kind == CMD_LOGS) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, req->container_id);
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            ssize_t n = read(fd, res.message, sizeof(res.message) - 1);
            if (n >= 0) res.message[n] = '\0';
            else strcpy(res.message, "Error reading log file");
            close(fd);
        } else {
            res.status = 1;
            snprintf(res.message, sizeof(res.message), "Log file not found: %s", path);
        }
    }
    send(client_fd, &res, sizeof(res), 0);
}

static int run_supervisor(const char *rootfs)
{
    (void)rootfs;
    supervisor_ctx_t ctx = {0};
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path)-1);
    unlink(CONTROL_PATH);
    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(ctx.server_fd, 5) < 0) return 1;
    mkdir(LOG_DIR, 0755);
    pthread_mutex_init(&ctx.metadata_lock, NULL);
    bounded_buffer_init(&ctx.log_buffer);
    g_ctx = &ctx;
    signal(SIGCHLD, sigchld_handler); signal(SIGINT, shutdown_handler); signal(SIGTERM, shutdown_handler);
    pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx.log_buffer);
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    while (!ctx.should_stop) {
        struct timeval tv = {1, 0}; fd_set fds; FD_ZERO(&fds); FD_SET(ctx.server_fd, &fds);
        if (select(ctx.server_fd + 1, &fds, NULL, NULL, &tv) > 0) {
            int cfd = accept(ctx.server_fd, NULL, NULL);
            if (cfd >= 0) {
                control_request_t req;
                if (recv(cfd, &req, sizeof(req), 0) == sizeof(req)) handle_request(&ctx, cfd, &req);
                close(cfd);
            }
        }
    }
    bounded_buffer_begin_shutdown(&ctx.log_buffer); pthread_join(ctx.logger_thread, NULL);
    bounded_buffer_destroy(&ctx.log_buffer); pthread_mutex_destroy(&ctx.metadata_lock);
    close(ctx.server_fd); if (ctx.monitor_fd >= 0) close(ctx.monitor_fd); unlink(CONTROL_PATH);
    return 0;
}

static int send_control_request(const control_request_t *req)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path)-1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) return 1;
    send(fd, req, sizeof(*req), 0);
    control_response_t res;
    if (recv(fd, &res, sizeof(res), 0) > 0) {
        if (res.status == 0) printf("%s\n", res.message);
        else fprintf(stderr, "Error: %s\n", res.message);
    }
    close(fd); return res.status;
}

int main(int argc, char *argv[])
{
    if (argc < 2) { usage(argv[0]); return 1; }
    if (strcmp(argv[1], "supervisor") == 0) return run_supervisor(argv[2]);
    control_request_t req = {0};
    if (strcmp(argv[1], "start") == 0 || strcmp(argv[1], "run") == 0) {
        if (argc < 5) return 1;
        req.kind = (strcmp(argv[1], "start") == 0) ? CMD_START : CMD_RUN;
        strncpy(req.container_id, argv[2], sizeof(req.container_id)-1);
        strncpy(req.rootfs, argv[3], sizeof(req.rootfs)-1);
        strncpy(req.command, argv[4], sizeof(req.command)-1);
        req.soft_limit_bytes = DEFAULT_SOFT_LIMIT; req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
        parse_optional_flags(&req, argc, argv, 5);
    } else if (strcmp(argv[1], "ps") == 0) req.kind = CMD_PS;
    else if (strcmp(argv[1], "stop") == 0) { if (argc < 3) return 1; req.kind = CMD_STOP; strncpy(req.container_id, argv[2], sizeof(req.container_id)-1); }
    else if (strcmp(argv[1], "logs") == 0) { if (argc < 3) return 1; req.kind = CMD_LOGS; strncpy(req.container_id, argv[2], sizeof(req.container_id)-1); }
    else { usage(argv[0]); return 1; }
    return send_control_request(&req);
}
