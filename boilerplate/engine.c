#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mount.h>
#include <sys/stat.h>

#define STACK_SIZE (1024 * 1024)
#define CONTROL_PATH "/tmp/mini_runtime.sock"

/* ================= CONTAINER ================= */

typedef struct container {
    char id[64];
    pid_t pid;
    int state;
    int stop_requested;
    int killed_by_limit;
    struct container *next;
} container_t;

#define CONTAINER_RUNNING 1
#define CONTAINER_EXITED  2

typedef struct {
    container_t *containers;
} context_t;

context_t *g_ctx;

/* ================= FIND ================= */

container_t *find_container_by_pid(pid_t pid) {
    container_t *c = g_ctx->containers;
    while (c) {
        if (c->pid == pid)
            return c;
        c = c->next;
    }
    return NULL;
}

/* ================= CHILD ================= */

int child_fn(void *arg) {
    char **args = (char **)arg;

    char *id     = args[0];
    char *rootfs = args[1];
    char *cmd    = args[2];

    sethostname(id, strlen(id));

    if (chroot(rootfs) != 0) {
        perror("chroot");
        return 1;
    }

    chdir("/");

    mkdir("/proc", 0555);
    mount("proc", "/proc", "proc", 0, NULL);

    execl(cmd, cmd, NULL);

    perror("exec failed");
    return 1;
}

/* ================= LAUNCH ================= */

pid_t launch_container(char *id, char *rootfs, char *cmd) {
    char *stack = malloc(STACK_SIZE);
    char *stack_top = stack + STACK_SIZE;

    char **args = malloc(sizeof(char*) * 3);
    args[0] = id;
    args[1] = rootfs;
    args[2] = cmd;

    int flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;

    pid_t pid = clone(child_fn, stack_top, flags, args);
    return pid;
}

/* ================= SIGCHLD ================= */

void sigchld_handler(int sig) {
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {

        container_t *c = find_container_by_pid(pid);
        if (!c) continue;

        if (WIFSIGNALED(status)) {
            int s = WTERMSIG(status);

            if (s == SIGKILL && !c->stop_requested) {
                c->killed_by_limit = 1;
            }
        }

        c->state = CONTAINER_EXITED;
    }
}

/* ================= HANDLE CLIENT ================= */

void handle_client(int client) {
    char buf[256] = {0};

    int n = read(client, buf, sizeof(buf) - 1);
    if (n <= 0) {
        close(client);
        return;
    }

    char cmd_name[32] = {0};
    char id[64] = {0};
    char rootfs[128] = {0};
    char cmd[128] = {0};

    sscanf(buf, "%s %s %s %s", cmd_name, id, rootfs, cmd);

    /* ===== START ===== */
    if (strcmp(cmd_name, "start") == 0) {

        pid_t pid = launch_container(id, rootfs, cmd);

        container_t *newc = malloc(sizeof(container_t));
        strcpy(newc->id, id);
        newc->pid = pid;
        newc->state = CONTAINER_RUNNING;
        newc->stop_requested = 0;
        newc->killed_by_limit = 0;

        newc->next = g_ctx->containers;
        g_ctx->containers = newc;

        write(client, "OK\n", 3);
    }

    /* ===== PS ===== */
    else if (strcmp(cmd_name, "ps") == 0) {

        container_t *c = g_ctx->containers;

        if (!c) {
            write(client, "No containers\n", 14);
        }

        while (c) {
            char line[128];

            if (c->killed_by_limit)
                snprintf(line, sizeof(line), "%s %d hard_limit_killed\n", c->id, c->pid);
            else if (c->stop_requested)
                snprintf(line, sizeof(line), "%s %d stopped\n", c->id, c->pid);
            else if (c->state == CONTAINER_RUNNING)
                snprintf(line, sizeof(line), "%s %d running\n", c->id, c->pid);
            else
                snprintf(line, sizeof(line), "%s %d exited\n", c->id, c->pid);

            write(client, line, strlen(line));
            c = c->next;
        }
    }

    /* ===== STOP ===== */
    else if (strcmp(cmd_name, "stop") == 0) {

        container_t *c = g_ctx->containers;

        while (c) {
            if (strcmp(c->id, id) == 0) {
                c->stop_requested = 1;
                kill(c->pid, SIGTERM);
                write(client, "Stopped\n", 8);
                break;
            }
            c = c->next;
        }
    }

    close(client);
}

/* ================= SUPERVISOR ================= */

void run_supervisor() {
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);

    struct sockaddr_un addr;
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, CONTROL_PATH);

    unlink(CONTROL_PATH);

    bind(server_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(server_fd, 5);

    printf("Supervisor ready on %s\n", CONTROL_PATH);

    signal(SIGCHLD, sigchld_handler);

    while (1) {
        int client = accept(server_fd, NULL, NULL);
        if (client >= 0) {
            handle_client(client);
        }
    }
}

/* ================= CLIENT ================= */

void send_cmd(char *msg) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);

    struct sockaddr_un addr;
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, CONTROL_PATH);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        return;
    }

    write(fd, msg, strlen(msg));

    char buf[512];
    int n;

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        write(STDOUT_FILENO, buf, n);
    }

    close(fd);
}

/* ================= MAIN ================= */

int main(int argc, char *argv[]) {

    if (argc < 2) {
        printf("Usage:\n");
        return 0;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        g_ctx = calloc(1, sizeof(context_t));
        run_supervisor();
    }

    else if (strcmp(argv[1], "start") == 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "start %s %s %s",
                 argv[2], argv[3], argv[4]);
        send_cmd(msg);
    }

    else if (strcmp(argv[1], "ps") == 0) {
        send_cmd("ps");
    }

    else if (strcmp(argv[1], "stop") == 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "stop %s", argv[2]);
        send_cmd(msg);
    }

    return 0;
}

