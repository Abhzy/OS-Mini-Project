/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Intentionally partial starter:
 *   - command-line shape is defined
 *   - key runtime data structures are defined
 *   - bounded-buffer skeleton is defined
 *   - supervisor / client split is outlined
 *
 * Students are expected to design:
 *   - the control-plane IPC implementation
 *   - container lifecycle and metadata synchronization
 *   - clone + namespace setup for each container
 *   - producer/consumer behavior for log buffering
 *   - signal handling and graceful shutdown
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
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)
#define DEVICE_NAME "container_monitor"

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

static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
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

static int parse_optional_flags(control_request_t *req,
                                int argc,
                                char *argv[],
                                int start_index)
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
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
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
    case CONTAINER_STARTING:
        return "starting";
    case CONTAINER_RUNNING:
        return "running";
    case CONTAINER_STOPPED:
        return "stopped";
    case CONTAINER_KILLED:
        return "killed";
    case CONTAINER_EXITED:
        return "exited";
    default:
        return "unknown";
    }
}

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;

    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0)
        return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

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

/*
 * TODO:
 * Implement producer-side insertion into the bounded buffer.
 *
 * Requirements:
 *   - block or fail according to your chosen policy when the buffer is full
 *   - wake consumers correctly
 *   - stop cleanly if shutdown begins
 */
int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);
    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);
    }
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
    while (buffer->count == 0 && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);
    }
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

/*
 * TODO:
 * Implement the logging consumer thread.
 *
 * Suggested responsibilities:
 *   - remove log chunks from the bounded buffer
 *   - route each chunk to the correct per-container log file
 *   - exit cleanly when shutdown begins and pending work is drained
 */
void *logging_thread(void *arg)
{
    bounded_buffer_t *buffer = (bounded_buffer_t *)arg;
    log_item_t item;
    mkdir(LOG_DIR, 0755);

    while (bounded_buffer_pop(buffer, &item) == 0) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);
        
        int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            if (write(fd, item.data, item.length) < 0) {
                perror("write log");
            }
            close(fd);
        } else {
            perror("open log");
        }
    }
    return NULL;
}

typedef struct {
    char id[CONTAINER_ID_LEN];
    int read_fd;
    bounded_buffer_t *buffer;
} producer_args_t;

void *producer_thread(void *arg)
{
    producer_args_t *args = (producer_args_t *)arg;
    char chunk[LOG_CHUNK_SIZE];
    ssize_t n;

    while ((n = read(args->read_fd, chunk, LOG_CHUNK_SIZE)) > 0) {
        log_item_t item;
        memset(&item, 0, sizeof(item));
        strncpy(item.container_id, args->id, sizeof(item.container_id) - 1);
        item.length = n;
        memcpy(item.data, chunk, n);
        
        if (bounded_buffer_push(args->buffer, &item) != 0) break;
    }
    
    close(args->read_fd);
    free(args);
    return NULL;
}

/*
 * TODO:
 * Implement the clone child entrypoint.
 *
 * Required outcomes:
 *   - isolated PID / UTS / mount context
 *   - chroot or pivot_root into rootfs
 *   - working /proc inside container
 *   - stdout / stderr redirected to the supervisor logging path
 *   - configured command executed inside the container
 */
int child_fn(void *arg)
{
    child_config_t *config = (child_config_t *)arg;

    // 0) Ensure private mount namespace for chroot/mounts
    mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);

    // 1) Hostname isolation
    if (sethostname(config->id, strlen(config->id)) < 0) {
        perror("sethostname");
        return 1;
    }

    // 2) Filesystem isolation via pivot_root
    // pivot_root requires the new root to be a mount point
    if (mount(config->rootfs, config->rootfs, NULL, MS_BIND | MS_REC, NULL) < 0) {
        perror("mount bind rootfs");
        return 1;
    }

    char put_old[PATH_MAX];
    snprintf(put_old, sizeof(put_old), "%s/.old_root", config->rootfs);
    mkdir(put_old, 0777);

    if (syscall(SYS_pivot_root, config->rootfs, put_old) < 0) {
        perror("pivot_root");
        return 1;
    }

    if (chdir("/") < 0) {
        perror("chdir /");
        return 1;
    }

    // Unmount old root
    if (umount2("/.old_root", MNT_DETACH) < 0) {
        perror("umount2 .old_root");
    }
    rmdir("/.old_root");

    // 3) Working /proc
    if (mount("proc", "/proc", "proc", 0, NULL) < 0) {
        perror("mount /proc");
    }

    // 4) Priority
    if (setpriority(PRIO_PROCESS, 0, config->nice_value) < 0) {
        perror("setpriority");
    }

    // 5) Redirect stdout/stderr to pipe
    if (dup2(config->log_write_fd, STDOUT_FILENO) < 0) {
        perror("dup2 stdout");
        return 1;
    }
    if (dup2(config->log_write_fd, STDERR_FILENO) < 0) {
        perror("dup2 stderr");
        return 1;
    }
    close(config->log_write_fd);

    // 6) Execute command
    char *exec_argv[] = {"sh", "-c", config->command, NULL};
    
    setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);
    setenv("TERM", "xterm", 1);
    
    execv("/bin/sh", exec_argv);
    execv("/bin/busybox", exec_argv);

    perror("execv failed");
    return 1;
}

int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;

    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;

    return 0;
}

/*
 * TODO:
 * Implement the long-running supervisor process.
 *
 * Suggested responsibilities:
 *   - create and bind the control-plane IPC endpoint
 *   - initialize shared metadata and the bounded buffer
 *   - start the logging thread
 *   - accept control requests and update container state
 *   - reap children and respond to signals
 */
static supervisor_ctx_t *g_ctx = NULL;

static void sigchld_handler(int sig)
{
    (void)sig;
    pid_t pid;
    int status;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (g_ctx) {
            pthread_mutex_lock(&g_ctx->metadata_lock);
            container_record_t *cur = g_ctx->containers;
            while (cur) {
                if (cur->host_pid == pid && cur->state == CONTAINER_RUNNING) {
                    if (WIFEXITED(status)) {
                        cur->state = CONTAINER_EXITED;
                        cur->exit_code = WEXITSTATUS(status);
                    } else if (WIFSIGNALED(status)) {
                        cur->state = CONTAINER_KILLED;
                        cur->exit_signal = WTERMSIG(status);
                    }
                    if (g_ctx->monitor_fd >= 0) {
                        unregister_from_monitor(g_ctx->monitor_fd, cur->id, cur->host_pid);
                    }
                    break;
                }
                cur = cur->next;
            }
            pthread_mutex_unlock(&g_ctx->metadata_lock);
        }
    }
}

static void sigterm_handler(int sig)
{
    (void)sig;
    if (g_ctx) {
        g_ctx->should_stop = 1;
        // Wake up accept() by shutting down the listener
        shutdown(g_ctx->server_fd, SHUT_RDWR);
    }
}

typedef struct {
    int client_fd;
    supervisor_ctx_t *ctx;
} client_handler_args_t;

void *client_handler_thread(void *arg)
{
    client_handler_args_t *args = (client_handler_args_t *)arg;
    int client_fd = args->client_fd;
    supervisor_ctx_t *ctx = args->ctx;
    control_request_t req;
    control_response_t res;

    if (read(client_fd, &req, sizeof(req)) == sizeof(req)) {
        memset(&res, 0, sizeof(res));
        res.status = 0;
        
        switch (req.kind) {
            case CMD_START:
            case CMD_RUN: {
                pthread_mutex_lock(&ctx->metadata_lock);
                container_record_t *cur = ctx->containers;
                int exists = 0;
                while (cur) {
                    if (strcmp(cur->id, req.container_id) == 0) {
                        exists = 1;
                        break;
                    }
                    cur = cur->next;
                }
                if (exists) {
                    res.status = -1;
                    snprintf(res.message, sizeof(res.message), "Container %s already defined (must be unique)", req.container_id);
                    pthread_mutex_unlock(&ctx->metadata_lock);
                    break;
                }

                int log_pipe[2];
                if (pipe(log_pipe) < 0) {
                    res.status = -1;
                    snprintf(res.message, sizeof(res.message), "Failed to create log pipe");
                    pthread_mutex_unlock(&ctx->metadata_lock);
                    break;
                }

                char abs_rootfs[PATH_MAX];
                if (realpath(req.rootfs, abs_rootfs) == NULL) {
                    res.status = -1;
                    snprintf(res.message, sizeof(res.message), "Failed to resolve rootfs path: %s", req.rootfs);
                    pthread_mutex_unlock(&ctx->metadata_lock);
                    break;
                }

                child_config_t *c_config = malloc(sizeof(*c_config));
                strncpy(c_config->id, req.container_id, sizeof(c_config->id) - 1);
                strncpy(c_config->rootfs, abs_rootfs, sizeof(c_config->rootfs) - 1);
                strncpy(c_config->command, req.command, sizeof(c_config->command) - 1);
                c_config->nice_value = req.nice_value;
                c_config->log_write_fd = log_pipe[1];

                char *stack = malloc(STACK_SIZE);
                pid_t child_pid = clone(child_fn, stack + STACK_SIZE,
                                        CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                                        c_config);
                
                if (child_pid < 0) {
                    res.status = -1;
                    snprintf(res.message, sizeof(res.message), "Failed to clone: %s", strerror(errno));
                    close(log_pipe[0]);
                    close(log_pipe[1]);
                    free(stack);
                    free(c_config);
                } else {
                    close(log_pipe[1]);
                    if (ctx->monitor_fd >= 0) {
                        register_with_monitor(ctx->monitor_fd, req.container_id, child_pid,
                                              req.soft_limit_bytes, req.hard_limit_bytes);
                    }

                    container_record_t *new_rec = malloc(sizeof(*new_rec));
                    memset(new_rec, 0, sizeof(*new_rec));
                    strncpy(new_rec->id, req.container_id, sizeof(new_rec->id) - 1);
                    new_rec->host_pid = child_pid;
                    new_rec->started_at = time(NULL);
                    new_rec->state = CONTAINER_RUNNING;
                    new_rec->soft_limit_bytes = req.soft_limit_bytes;
                    new_rec->hard_limit_bytes = req.hard_limit_bytes;
                    snprintf(new_rec->log_path, sizeof(new_rec->log_path), "logs/%s.log", req.container_id);
                    new_rec->next = ctx->containers;
                    ctx->containers = new_rec;

                    snprintf(res.message, sizeof(res.message), "Container %s started with PID %d", req.container_id, child_pid);
                    
                    producer_args_t *p_args = malloc(sizeof(*p_args));
                    strncpy(p_args->id, req.container_id, sizeof(p_args->id) - 1);
                    p_args->read_fd = log_pipe[0];
                    p_args->buffer = &ctx->log_buffer;
                    
                    pthread_t pt_p;
                    pthread_create(&pt_p, NULL, producer_thread, p_args);
                    pthread_detach(pt_p);

                    if (req.kind == CMD_RUN) {
                        pthread_mutex_unlock(&ctx->metadata_lock);
                        while (1) {
                            usleep(50000);
                            pthread_mutex_lock(&ctx->metadata_lock);
                            container_record_t *rec = ctx->containers;
                            while (rec) {
                                if (strcmp(rec->id, req.container_id) == 0) break;
                                rec = rec->next;
                            }
                            if (!rec || rec->state == CONTAINER_EXITED || rec->state == CONTAINER_KILLED || rec->state == CONTAINER_STOPPED) {
                                if (rec) {
                                    res.status = rec->exit_code;
                                    if (rec->state == CONTAINER_KILLED) res.status = 128 + rec->exit_signal;
                                    snprintf(res.message, sizeof(res.message), "Container exited with status %d", res.status);
                                }
                                pthread_mutex_unlock(&ctx->metadata_lock);
                                break;
                            }
                            pthread_mutex_unlock(&ctx->metadata_lock);
                        }
                        pthread_mutex_lock(&ctx->metadata_lock);
                    }
                }
                pthread_mutex_unlock(&ctx->metadata_lock);
                break;
            }
            case CMD_PS: {
                pthread_mutex_lock(&ctx->metadata_lock);
                container_record_t *cur = ctx->containers;
                int len = 0;
                len += snprintf(res.message + len, sizeof(res.message) - len, "ID\tPID\tSTATUS\n");
                while (cur && len < (int)sizeof(res.message) - 64) {
                    len += snprintf(res.message + len, sizeof(res.message) - len, "%s\t%d\t%s\n",
                                    cur->id, cur->host_pid, state_to_string(cur->state));
                    cur = cur->next;
                }
                pthread_mutex_unlock(&ctx->metadata_lock);
                break;
            }
            case CMD_LOGS: {
                pthread_mutex_lock(&ctx->metadata_lock);
                char path[PATH_MAX];
                snprintf(path, sizeof(path), "logs/%s.log", req.container_id);
                pthread_mutex_unlock(&ctx->metadata_lock);
                
                int fd = open(path, O_RDONLY);
                if (fd >= 0) {
                    ssize_t n = read(fd, res.message, sizeof(res.message) - 1);
                    if (n > 0) res.message[n] = '\0';
                    else strcpy(res.message, "(empty)");
                    close(fd);
                } else {
                    res.status = -1;
                    snprintf(res.message, sizeof(res.message), "Log file %s not found", path);
                }
                break;
            }
            case CMD_STOP: {
                pthread_mutex_lock(&ctx->metadata_lock);
                container_record_t *cur = ctx->containers;
                int found = 0;
                while (cur) {
                    if (strcmp(cur->id, req.container_id) == 0 && cur->state == CONTAINER_RUNNING) {
                        kill(cur->host_pid, SIGTERM);
                        cur->state = CONTAINER_STOPPED;
                        found = 1;
                        break;
                    }
                    cur = cur->next;
                }
                if (found) {
                    snprintf(res.message, sizeof(res.message), "Container %s stopped", req.container_id);
                } else {
                    res.status = -1;
                    snprintf(res.message, sizeof(res.message), "Container %s not found or not running", req.container_id);
                }
                pthread_mutex_unlock(&ctx->metadata_lock);
                break;
            }
            default:
                snprintf(res.message, sizeof(res.message), "Command %d not implemented", req.kind);
                break;
        }
        write(client_fd, &res, sizeof(res));
    }
    close(client_fd);
    free(args);
    return NULL;
}

static int run_supervisor_loop(supervisor_ctx_t *ctx)
{
    while (!ctx->should_stop) {
        int client_fd = accept(ctx->server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            if (ctx->should_stop) break;
            perror("accept");
            break;
        }

        client_handler_args_t *h_args = malloc(sizeof(*h_args));
        h_args->client_fd = client_fd;
        h_args->ctx = ctx;

        pthread_t pt_h;
        pthread_create(&pt_h, NULL, client_handler_thread, h_args);
        pthread_detach(pt_h);
    }
    return 0;
}

static int run_supervisor(const char *rootfs_base)
{
    supervisor_ctx_t ctx;
    int rc;
    struct sockaddr_un addr;

    (void)rootfs_base;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) {
        errno = rc;
        perror("pthread_mutex_init");
        return 1;
    }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        errno = rc;
        perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    ctx.monitor_fd = open("/dev/" DEVICE_NAME, O_RDWR);
    if (ctx.monitor_fd < 0) {
        perror("open /dev/" DEVICE_NAME);
    }

    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) {
        perror("socket");
        return 1;
    }

    g_ctx = &ctx;
    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    
    sa.sa_handler = sigchld_handler;
    sigaction(SIGCHLD, &sa, NULL);

    sa.sa_handler = sigterm_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
    unlink(CONTROL_PATH);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }

    if (listen(ctx.server_fd, 5) < 0) {
        perror("listen");
        return 1;
    }

    printf("Supervisor started. Listening on %s\n", CONTROL_PATH);

    pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx.log_buffer);

    run_supervisor_loop(&ctx);

    close(ctx.server_fd);
    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
    unlink(CONTROL_PATH);
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);
    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);
    return 0;
}

/*
 * TODO:
 * Implement the client-side control request path.
 *
 * The CLI commands should use a second IPC mechanism distinct from the
 * logging pipe. A UNIX domain socket is the most direct option, but a
 * FIFO or shared memory design is also acceptable if justified.
 */
static int send_control_request(const control_request_t *req)
{
    int client_fd;
    struct sockaddr_un addr;
    control_response_t res;

    client_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (client_fd < 0) {
        perror("socket");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(client_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(client_fd);
        return 1;
    }

    if (write(client_fd, req, sizeof(*req)) < (ssize_t)sizeof(*req)) {
        perror("write");
        close(client_fd);
        return 1;
    }

    if (read(client_fd, &res, sizeof(res)) < (ssize_t)sizeof(res)) {
        perror("read");
        close(client_fd);
        return 1;
    }

    printf("Supervisor response: %s (status: %d)\n", res.message, res.status);
    close(client_fd);
    return res.status;
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;

    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;

    /*
     * TODO:
     * The supervisor should respond with container metadata.
     * Keep the rendering format simple enough for demos and debugging.
     */
    printf("Expected states include: %s, %s, %s, %s, %s\n",
           state_to_string(CONTAINER_STARTING),
           state_to_string(CONTAINER_RUNNING),
           state_to_string(CONTAINER_STOPPED),
           state_to_string(CONTAINER_KILLED),
           state_to_string(CONTAINER_EXITED));
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0)
        return cmd_start(argc, argv);

    if (strcmp(argv[1], "run") == 0)
        return cmd_run(argc, argv);

    if (strcmp(argv[1], "ps") == 0)
        return cmd_ps();

    if (strcmp(argv[1], "logs") == 0)
        return cmd_logs(argc, argv);

    if (strcmp(argv[1], "stop") == 0)
        return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
