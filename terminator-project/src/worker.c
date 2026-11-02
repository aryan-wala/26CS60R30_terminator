#include "terminator.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
    int input_fd;
    int output_fd;
    HistoryStore *history;
    sem_t *history_lock;
} ThreadArgs;

static void set_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFD);
    if (flags >= 0) (void)fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

static void close_unrelated_fds(int keep_a, int keep_b)
{
    long max_fd = sysconf(_SC_OPEN_MAX);
    if (max_fd < 0 || max_fd > 65536) max_fd = 65536;
    for (int fd = 3; fd < max_fd; ++fd)
        if (fd != keep_a && fd != keep_b) close(fd);
}

static void *thread_entry(void *opaque)
{
    ThreadArgs *args = opaque;
    int input_fd = args->input_fd, output_fd = args->output_fd;
    HistoryStore *history = args->history;
    sem_t *history_lock = args->history_lock;
    free(args);
    shell_worker_loop(input_fd, output_fd, history, history_lock);
    close(input_fd);
    close(output_fd);
    return NULL;
}

int tab_start(Tab *tab, int process_mode, int x_connection_fd,
              HistoryStore *history, sem_t *history_lock)
{
    int to_worker[2] = {-1, -1}, from_worker[2] = {-1, -1};
    if (pipe(to_worker) != 0) return -1;
    if (pipe(from_worker) != 0) { close(to_worker[0]); close(to_worker[1]); return -1; }
    for (int i = 0; i < 2; ++i) { set_cloexec(to_worker[i]); set_cloexec(from_worker[i]); }

    tab->to_worker = to_worker[1];
    tab->from_worker = from_worker[0];
    tab->process = -1;
    tab->alive = 1;
    tab->is_thread = !process_mode;
    if (!getcwd(tab->cwd, sizeof(tab->cwd))) snprintf(tab->cwd, sizeof(tab->cwd), "/");

    if (process_mode) {
        pid_t pid = fork();
        if (pid < 0) goto fail;
        if (pid == 0) {
            close(to_worker[1]);
            close(from_worker[0]);
            if (x_connection_fd >= 0) close(x_connection_fd);
            close_unrelated_fds(to_worker[0], from_worker[1]);
            shell_worker_loop(to_worker[0], from_worker[1], history, history_lock);
            _exit(0);
        }
        tab->process = pid;
        close(to_worker[0]);
        close(from_worker[1]);
    } else {
        ThreadArgs *args = malloc(sizeof(*args));
        if (!args) goto fail;
        args->input_fd = to_worker[0];
        args->output_fd = from_worker[1];
        args->history = history;
        args->history_lock = history_lock;
        if (pthread_create(&tab->thread, NULL, thread_entry, args) != 0) {
            free(args); goto fail;
        }
        /* Threads share the descriptor table; the worker closes its own
           endpoints when it exits. */
    }
    int flags = fcntl(tab->from_worker, F_GETFL);
    if (flags >= 0) (void)fcntl(tab->from_worker, F_SETFL, flags | O_NONBLOCK);
    return 0;

fail:
    close(to_worker[0]); close(to_worker[1]);
    close(from_worker[0]); close(from_worker[1]);
    return -1;
}

int tab_send_request(Tab *tab, uint32_t kind, const char *payload,
                     size_t payload_length)
{
    if (!tab->alive || tab->to_worker < 0 || payload_length > TERM_INPUT_CAP) return -1;
    RequestHeader h = { .kind = kind, .length = (uint32_t)payload_length };
    if (write_all(tab->to_worker, &h, sizeof(h)) != 0) return -1;
    if (payload_length && write_all(tab->to_worker, payload, payload_length) != 0) return -1;
    return 0;
}

void tab_stop(Tab *tab)
{
    if (!tab->alive) return;
    (void)tab_send_request(tab, REQUEST_SHUTDOWN, NULL, 0);
    close(tab->to_worker);
    tab->to_worker = -1;
    if (tab->from_worker >= 0) {
        int flags = fcntl(tab->from_worker, F_GETFL);
        if (flags >= 0) (void)fcntl(tab->from_worker, F_SETFL, flags | O_NONBLOCK);
        char discard[8192];
        struct pollfd pfd = { .fd = tab->from_worker, .events = POLLIN | POLLHUP };
        for (;;) {
            int ready = poll(&pfd, 1, 100);
            if (ready < 0 && errno == EINTR) continue;
            ssize_t n = read(tab->from_worker, discard, sizeof(discard));
            if (n == 0) break;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                if (ready > 0 && (pfd.revents & POLLHUP)) break;
                continue;
            }
            if (n < 0 && errno != EINTR) break;
        }
    }
    if (tab->is_thread) pthread_join(tab->thread, NULL);
    else if (tab->process > 0) {
        int status;
        while (waitpid(tab->process, &status, 0) < 0 && errno == EINTR) {}
    }
    close(tab->from_worker);
    tab->from_worker = -1;
    tab->alive = 0;
}

void tab_append_output(TabView *view, const char *data, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)data[i];
        if (c == '\r') continue;
        if (c == '\n') {
            if (view->line_count >= TERM_SCROLLBACK - 1) {
                memmove(view->lines, view->lines + 1,
                        sizeof(view->lines[0]) * (TERM_SCROLLBACK - 1));
                view->line_count = TERM_SCROLLBACK - 2;
            }
            ++view->line_count;
            view->lines[view->line_count][0] = '\0';
        } else if (view->line_count < TERM_SCROLLBACK) {
            size_t used = strlen(view->lines[view->line_count]);
            if (used + 1 < sizeof(view->lines[view->line_count])) {
                view->lines[view->line_count][used] = (char)c;
                view->lines[view->line_count][used + 1] = '\0';
            }
        }
    }
}
