#define _XOPEN_SOURCE 700
#include "terminator.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    char value[TERM_INPUT_CAP];
    int kind; /* 0 word, '|', '<', '>', '&', ';', or '>>' */
    int globable;
} Token;

static void output(ShellContext *sh, const char *s)
{
    (void)write_all(sh->output_fd, s, strlen(s));
}

static void outputf(ShellContext *sh, const char *format, const char *arg)
{
    char buffer[TERM_OUTPUT_CAP];
    snprintf(buffer, sizeof(buffer), format, arg);
    output(sh, buffer);
}

static int open_in_cwd(const ShellContext *sh, const char *path, int flags, mode_t mode)
{
    if (path[0] == '/') return open(path, flags, mode);
    char absolute[TERM_INPUT_CAP];
    snprintf(absolute, sizeof(absolute), "%s/%s", sh->cwd, path);
    return open(absolute, flags, mode);
}

int write_all(int fd, const void *buffer, size_t length)
{
    const char *p = buffer;
    while (length) {
        ssize_t n = write(fd, p, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n;
        length -= (size_t)n;
    }
    return 0;
}

int read_all(int fd, void *buffer, size_t length)
{
    char *p = buffer;
    size_t got = 0;
    while (got < length) {
        ssize_t n = read(fd, p + got, length - got);
        if (n < 0 && errno == EINTR) continue;
        if (n == 0) return got == 0 ? 0 : -1;
        if (n < 0) return -1;
        got += (size_t)n;
    }
    return 1;
}

static int copy_char(char *dst, size_t cap, size_t *used, char c)
{
    if (*used + 1 >= cap) return -1;
    dst[(*used)++] = c;
    dst[*used] = '\0';
    return 0;
}

static int variable_end(const char *s, size_t start, size_t *end,
                        char *name, size_t name_cap)
{
    size_t i = start + 1, n = 0;
    if (s[i] == '?') { name[n++] = '?'; ++i; }
    else if (s[i] == '{') {
        ++i;
        while (s[i] && s[i] != '}' && n + 1 < name_cap) name[n++] = s[i++];
        if (s[i] != '}') return 0;
        ++i;
    } else {
        if (!(isalpha((unsigned char)s[i]) || s[i] == '_')) return 0;
        while ((isalnum((unsigned char)s[i]) || s[i] == '_') && n + 1 < name_cap)
            name[n++] = s[i++];
    }
    name[n] = '\0';
    *end = i;
    return 1;
}

static int find_command_end(const char *s, size_t start, size_t *end, int backtick)
{
    int depth = 1, single = 0, dbl = 0, escaped = 0;
    for (size_t i = start; s[i]; ++i) {
        char c = s[i];
        if (escaped) { escaped = 0; continue; }
        if (c == '\\' && !single) { escaped = 1; continue; }
        if (backtick && c == '`') { *end = i; return 1; }
        if (!backtick) {
            if (c == '\'' && !dbl) single = !single;
            else if (c == '"' && !single) dbl = !dbl;
            else if (!single && !dbl && c == '(') ++depth;
            else if (!single && !dbl && c == ')' && --depth == 0) {
                *end = i;
                return 1;
            }
        }
    }
    return 0;
}

static int run_capture(ShellContext *sh, const char *command, char *result,
                       size_t capacity)
{
    FILE *capture = tmpfile();
    if (!capture) return -1;
    int capture_fd = fileno(capture);
    ShellContext *nested = malloc(sizeof(*nested));
    if (!nested) { fclose(capture); return -1; }
    *nested = *sh;
    nested->output_fd = capture_fd;
    nested->input_fd = -1;
    nested->foreground_pgid = 0;
    nested->shutdown = 0;
    nested->last_status = 0;
    shell_run_request(nested, command);
    free(nested);
    if (fflush(capture) != 0 || lseek(capture_fd, 0, SEEK_SET) < 0) {
        fclose(capture);
        return -1;
    }
    size_t used = 0;
    while (used + 1 < capacity) {
        ssize_t n = read(capture_fd, result + used, capacity - used - 1);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        used += (size_t)n;
    }
    fclose(capture);
    while (used && (result[used - 1] == '\n' || result[used - 1] == '\r')) --used;
    result[used] = '\0';
    return 0;
}

static int expand_text(ShellContext *sh, const char *input, char *out, size_t cap)
{
    size_t used = 0;
    int single = 0, dbl = 0, escaped = 0;
    out[0] = '\0';
    for (size_t i = 0; input[i];) {
        char c = input[i];
        if (escaped) {
            if (copy_char(out, cap, &used, c) != 0) return -1;
            escaped = 0; ++i; continue;
        }
        if (c == '\\' && !single) {
            if (copy_char(out, cap, &used, c) != 0) return -1;
            escaped = 1; ++i; continue;
        }
        if (c == '\'' && !dbl) single = !single;
        else if (c == '"' && !single) dbl = !dbl;

        if (!single && c == '$' && input[i + 1] == '(') {
            size_t end;
            if (!find_command_end(input, i + 2, &end, 0)) return 1;
            char body[TERM_INPUT_CAP], value[TERM_INPUT_CAP];
            size_t n = end - (i + 2);
            if (n >= sizeof(body)) return -1;
            memcpy(body, input + i + 2, n); body[n] = '\0';
            if (run_capture(sh, body, value, sizeof(value)) != 0) return -1;
            for (size_t k = 0; value[k]; ++k)
                if (copy_char(out, cap, &used, value[k]) != 0) return -1;
            i = end + 1; continue;
        }
        if (!single && c == '`') {
            size_t end;
            if (!find_command_end(input, i + 1, &end, 1)) return 1;
            char body[TERM_INPUT_CAP], value[TERM_INPUT_CAP];
            size_t n = end - (i + 1);
            memcpy(body, input + i + 1, n); body[n] = '\0';
            if (run_capture(sh, body, value, sizeof(value)) != 0) return -1;
            for (size_t k = 0; value[k]; ++k)
                if (copy_char(out, cap, &used, value[k]) != 0) return -1;
            i = end + 1; continue;
        }
        if (!single && c == '$') {
            char name[256]; size_t end;
            if (variable_end(input, i, &end, name, sizeof(name))) {
                char value[TERM_INPUT_CAP] = "";
                if (strcmp(name, "?") == 0) snprintf(value, sizeof(value), "%d", sh->last_status);
                else if (strcmp(name, "$") == 0) snprintf(value, sizeof(value), "%ld", (long)getpid());
                else if (strcmp(name, "PWD") == 0) snprintf(value, sizeof(value), "%s", sh->cwd);
                else {
                    const char *env = getenv(name);
                    if (env) snprintf(value, sizeof(value), "%s", env);
                }
                for (size_t k = 0; value[k]; ++k)
                    if (copy_char(out, cap, &used, value[k]) != 0) return -1;
                i = end; continue;
            }
        }
        if (!single && c == '~' && (i == 0 || isspace((unsigned char)input[i - 1])) &&
            (input[i + 1] == '/' || input[i + 1] == '\0' || isspace((unsigned char)input[i + 1]))) {
            const char *home = getenv("HOME");
            if (home) {
                for (size_t k = 0; home[k]; ++k)
                    if (copy_char(out, cap, &used, home[k]) != 0) return -1;
                ++i; continue;
            }
        }
        if (copy_char(out, cap, &used, c) != 0) return -1;
        ++i;
    }
    return 0;
}

static int tokenize(const char *s, Token *tokens, size_t *count)
{
    size_t i = 0, n = 0;
    while (s[i]) {
        while (isspace((unsigned char)s[i])) ++i;
        if (!s[i]) break;
        if (n >= TERM_MAX_ARGS * 2) return -1;
        Token *t = &tokens[n];
        memset(t, 0, sizeof(*t));
        t->globable = 1;
        if (s[i] == '|') { t->kind = '|'; ++i; ++n; continue; }
        if (s[i] == '<' || s[i] == '>' || s[i] == '&' || s[i] == ';') {
            t->kind = s[i++];
            if (t->kind == '>' && s[i] == '>') { t->kind = 256; ++i; }
            ++n; continue;
        }
        size_t used = 0;
        int single = 0, dbl = 0, escaped = 0;
        while (s[i]) {
            char c = s[i];
            if (escaped) { if (copy_char(t->value, sizeof(t->value), &used, c)) return -1; escaped = 0; ++i; continue; }
            if (c == '\\' && !single) { t->globable = 0; escaped = 1; ++i; continue; }
            if (c == '\'' && !dbl) { t->globable = 0; single = !single; ++i; continue; }
            if (c == '"' && !single) { t->globable = 0; dbl = !dbl; ++i; continue; }
            if (!single && !dbl && (isspace((unsigned char)c) || c == '|' || c == '<' || c == '>' || c == '&' || c == ';')) break;
            if (copy_char(t->value, sizeof(t->value), &used, c)) return -1;
            ++i;
        }
        if (single || dbl || escaped) return 1;
        ++n;
    }
    *count = n;
    return 0;
}

static int parse_commands(ShellContext *sh, const char *line,
                          ParsedCommand stages[TERM_MAX_STAGES], size_t *stage_count)
{
    char expanded[TERM_INPUT_CAP];
    int e = expand_text(sh, line, expanded, sizeof(expanded));
    if (e != 0) return e;
    Token *tokens = calloc(TERM_MAX_ARGS * 2, sizeof(*tokens));
    if (!tokens) return -1;
    size_t nt = 0;
    int t = tokenize(expanded, tokens, &nt);
    if (t != 0) { free(tokens); return t; }
    memset(stages, 0, sizeof(ParsedCommand) * TERM_MAX_STAGES);
    size_t si = 0;
    int result = 0;
    for (size_t i = 0; i < nt; ++i) {
        ParsedCommand *cmd = &stages[si];
        Token *tok = &tokens[i];
        if (tok->kind == '|') {
            if (cmd->count == 0 || si + 1 >= TERM_MAX_STAGES) { result = -1; goto done; }
            ++si; continue;
        }
        if (tok->kind == ';') {
            /* Execute semicolon-separated commands sequentially via recursion. */
            result = 2; goto done;
        }
        if (tok->kind == '<' || tok->kind == '>' || tok->kind == 256) {
            if (++i >= nt || tokens[i].kind || !tokens[i].value[0]) { result = -1; goto done; }
            size_t len = strlen(tokens[i].value) + 1;
            if (cmd->storage_used + len > sizeof(cmd->storage)) { result = -1; goto done; }
            char *copy = cmd->storage + cmd->storage_used;
            memcpy(copy, tokens[i].value, len); cmd->storage_used += len;
            if (tok->kind == '<') cmd->input_path = copy;
            else { cmd->output_path = copy; cmd->append = tok->kind == 256; }
            continue;
        }
        if (tok->kind == '&') {
            if (i + 1 != nt) { result = -1; goto done; }
            cmd->background = 1; continue;
        }
        char pattern[TERM_INPUT_CAP];
        int relative_pattern = tok->value[0] != '/';
        if (tok->globable && strpbrk(tok->value, "*?[")) {
            if (relative_pattern) snprintf(pattern, sizeof(pattern), "%s/%s", sh->cwd, tok->value);
            else snprintf(pattern, sizeof(pattern), "%s", tok->value);
            glob_t matches; memset(&matches, 0, sizeof(matches));
            int gr = glob(pattern, GLOB_NOCHECK, NULL, &matches);
            if (gr == 0) {
                for (size_t k = 0; k < matches.gl_pathc; ++k) {
                    const char *value = matches.gl_pathv[k];
                    size_t cwdlen = strlen(sh->cwd);
                    if (relative_pattern && strncmp(value, sh->cwd, cwdlen) == 0 && value[cwdlen] == '/')
                        value += cwdlen + 1;
                    size_t len = strlen(value) + 1;
                    if (cmd->count + 1 >= TERM_MAX_ARGS || cmd->storage_used + len > sizeof(cmd->storage)) { result = -1; break; }
                    cmd->text[cmd->count++] = cmd->storage + cmd->storage_used;
                    memcpy(cmd->storage + cmd->storage_used, value, len); cmd->storage_used += len;
                }
                globfree(&matches);
                if (result != 0) goto done;
                continue;
            }
            globfree(&matches);
        }
        size_t len = strlen(tok->value) + 1;
        if (cmd->count + 1 >= TERM_MAX_ARGS || cmd->storage_used + len > sizeof(cmd->storage)) { result = -1; goto done; }
        cmd->text[cmd->count++] = cmd->storage + cmd->storage_used;
        memcpy(cmd->storage + cmd->storage_used, tok->value, len);
        cmd->storage_used += len;
    }
    if (nt && stages[si].count == 0) result = -1;
    *stage_count = nt ? si + 1 : 0;
done:
    free(tokens);
    return result;
}

static void add_job(ShellContext *sh, pid_t pgid, const char *command, int stopped)
{
    for (int i = 0; i < TERM_MAX_JOBS; ++i) if (!sh->jobs[i].running) {
        sh->jobs[i].pid = pgid;
        sh->jobs[i].id = ++sh->next_job_id;
        snprintf(sh->jobs[i].command, sizeof(sh->jobs[i].command), "%s", command);
        sh->jobs[i].running = 1;
        sh->jobs[i].stopped = stopped;
        char b[TERM_OUTPUT_CAP];
        snprintf(b, sizeof(b), "[%d] %ld\n", sh->jobs[i].id, (long)pgid);
        output(sh, b);
        return;
    }
    output(sh, "terminator: job table full\n");
}

static void read_wait_control(ShellContext *sh)
{
    if (sh->input_fd < 0) return;
    struct pollfd pfd = { .fd = sh->input_fd, .events = POLLIN };
    int ready;
    do { ready = poll(&pfd, 1, 80); } while (ready < 0 && errno == EINTR);
    if (ready <= 0 || !(pfd.revents & POLLIN)) return;
    RequestHeader h;
    if (read_all(sh->input_fd, &h, sizeof(h)) <= 0 || h.length > TERM_INPUT_CAP) {
        sh->shutdown = 1;
        if (sh->foreground_pgid > 0) kill(-sh->foreground_pgid, SIGHUP);
        return;
    }
    char payload[TERM_INPUT_CAP];
    if (h.length && read_all(sh->input_fd, payload, h.length) <= 0) {
        sh->shutdown = 1;
        if (sh->foreground_pgid > 0) kill(-sh->foreground_pgid, SIGHUP);
        return;
    }
    if (h.kind == REQUEST_INTERRUPT && sh->foreground_pgid > 0) {
        output(sh, "^C\n");
        kill(-sh->foreground_pgid, SIGINT);
    } else if (h.kind == REQUEST_SUSPEND && sh->foreground_pgid > 0) {
        output(sh, "^Z\n");
        kill(-sh->foreground_pgid, SIGTSTP);
    } else if (h.kind == REQUEST_SHUTDOWN) {
        sh->shutdown = 1;
        if (sh->foreground_pgid > 0) kill(-sh->foreground_pgid, SIGHUP);
    }
}

static int run_pipeline(ShellContext *sh, ParsedCommand *cmds, size_t n,
                        int background, const char *line)
{
    int pipes[TERM_MAX_STAGES - 1][2];
    pid_t pids[TERM_MAX_STAGES];
    for (size_t i = 0; i + 1 < n; ++i) if (pipe(pipes[i]) != 0) {
        output(sh, "terminator: could not create pipe\n"); return 1;
    }
    pid_t pgid = sh->supervisor_pgid;
    for (size_t i = 0; i < n; ++i) {
        pid_t pid = fork();
        if (pid == 0) {
            if (!pgid) pgid = getpid();
            setpgid(0, pgid);
            signal(SIGPIPE, SIG_DFL);
            if (chdir(sh->cwd) != 0) { perror("chdir"); _exit(126); }
            if (i > 0) dup2(pipes[i - 1][0], STDIN_FILENO);
            if (i + 1 < n) dup2(pipes[i][1], STDOUT_FILENO);
            else dup2(sh->output_fd, STDOUT_FILENO);
            dup2(sh->output_fd, STDERR_FILENO);
            if (sh->output_fd > STDERR_FILENO) close(sh->output_fd);
            if (i == 0 && !cmds[i].input_path) {
                int nullfd = open("/dev/null", O_RDONLY);
                if (nullfd >= 0) { dup2(nullfd, STDIN_FILENO); close(nullfd); }
            }
            for (size_t k = 0; k + 1 < n; ++k) { close(pipes[k][0]); close(pipes[k][1]); }
            if (cmds[i].input_path) {
                int fd = open(cmds[i].input_path, O_RDONLY);
                if (fd < 0) { perror(cmds[i].input_path); _exit(1); }
                dup2(fd, STDIN_FILENO); close(fd);
            }
            if (cmds[i].output_path) {
                int flags = O_WRONLY | O_CREAT | (cmds[i].append ? O_APPEND : O_TRUNC);
                int fd = open(cmds[i].output_path, flags, 0666);
                if (fd < 0) { perror(cmds[i].output_path); _exit(1); }
                dup2(fd, STDOUT_FILENO); close(fd);
            }
            cmds[i].text[cmds[i].count] = NULL;
            execvp(cmds[i].text[0], cmds[i].text);
            int exec_error = errno;
            dprintf(STDERR_FILENO, "terminator: %s: command not found\n", cmds[i].text[0]);
            _exit(exec_error == ENOENT ? 127 : 126);
        }
        if (pid < 0) { output(sh, "terminator: could not fork command\n"); return 1; }
        if (!pgid) pgid = pid;
        setpgid(pid, pgid);
        pids[i] = pid;
    }
    for (size_t i = 0; i + 1 < n; ++i) { close(pipes[i][0]); close(pipes[i][1]); }
    if (background) { add_job(sh, pgid, line, 0); return 0; }
    sh->foreground_pgid = pgid;
    int last_status = 0, remaining = (int)n, stopped = 0;
    int finished[TERM_MAX_STAGES] = {0};
    if (sh->input_fd < 0) {
        for (size_t i = 0; i < n; ++i) {
            int status; pid_t w;
            do { w = waitpid(pids[i], &status, WUNTRACED); } while (w < 0 && errno == EINTR);
            if (w > 0) {
                if (WIFSTOPPED(status)) { stopped = 1; last_status = 128 + WSTOPSIG(status); }
                else if (i + 1 == n && WIFEXITED(status)) last_status = WEXITSTATUS(status);
                else if (i + 1 == n && WIFSIGNALED(status)) last_status = 128 + WTERMSIG(status);
            }
        }
        sh->foreground_pgid = 0;
        return last_status;
    }
    while (remaining > 0 && !stopped) {
        for (size_t i = 0; i < n; ++i) if (!finished[i]) {
            int status;
            pid_t w = waitpid(pids[i], &status, WNOHANG | WUNTRACED);
            if (w == pids[i]) {
                if (WIFSTOPPED(status)) {
                    stopped = 1;
                    (void)kill(-pgid, SIGSTOP);
                    add_job(sh, pgid, line, 1);
                    last_status = 128 + WSTOPSIG(status);
                } else {
                    finished[i] = 1; --remaining;
                    if (i + 1 == n) {
                        if (WIFEXITED(status)) last_status = WEXITSTATUS(status);
                        else if (WIFSIGNALED(status)) last_status = 128 + WTERMSIG(status);
                    }
                }
            }
        }
        if (remaining > 0 && !stopped) read_wait_control(sh);
    }
    sh->foreground_pgid = 0;
    return last_status;
}

typedef struct {
    char command[TERM_INPUT_CAP];
    pid_t pid;
    int fd;
    int active;
    int header_printed;
    int ended_newline;
    time_t next_run;
} WatchCommand;

static int parse_watch_list(const char *line, WatchCommand *items, size_t *count)
{
    const char *p = strchr(line, '[');
    if (!p) return -1;
    ++p; *count = 0;
    while (*p && *p != ']') {
        while (isspace((unsigned char)*p) || *p == ',') ++p;
        if (*p == ']') break;
        if (*p != '\'' && *p != '"') return -1;
        char quote = *p++;
        WatchCommand *item = &items[*count];
        size_t used = 0;
        while (*p && *p != quote) {
            char c = *p++;
            if (c == '\\' && *p) c = *p++;
            if (used + 1 >= sizeof(item->command)) return -1;
            item->command[used++] = c;
        }
        if (*p != quote) return -1;
        ++p; item->command[used] = '\0';
        if (*count + 1 >= TERM_MAX_STAGES) return -1;
        item->fd = -1; item->active = 0; item->header_printed = 0;
        item->ended_newline = 1; item->next_run = 0;
        ++*count;
    }
    return (*p == ']' && *count > 0) ? 0 : -1;
}

static int watch_spawn(ShellContext *sh, WatchCommand *item)
{
    int fds[2];
    if (pipe(fds) != 0) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        setpgid(0, 0);
        ShellContext nested = *sh;
        nested.input_fd = -1;
        nested.output_fd = fds[1];
        nested.foreground_pgid = 0;
        nested.supervisor_pgid = getpid();
        shell_run_request(&nested, item->command);
        close(fds[1]);
        _exit(nested.last_status & 255);
    }
    close(fds[1]);
    if (pid < 0) { close(fds[0]); return -1; }
    (void)setpgid(pid, pid);
    int flags = fcntl(fds[0], F_GETFL);
    if (flags >= 0) (void)fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);
    item->pid = pid; item->fd = fds[0]; item->active = 1;
    item->header_printed = 0; item->ended_newline = 1;
    return 0;
}

static void watch_header(ShellContext *sh, WatchCommand *item)
{
    if (item->header_printed) return;
    char b[TERM_OUTPUT_CAP];
    snprintf(b, sizeof(b), "\"%s\", %ld:\n----------------------------------------------------\n",
             item->command, (long)time(NULL));
    output(sh, b);
    item->header_printed = 1;
}

static int multi_watch(ShellContext *sh, const char *line)
{
    WatchCommand items[TERM_MAX_STAGES];
    memset(items, 0, sizeof(items));
    size_t count = 0;
    if (parse_watch_list(line, items, &count) != 0) {
        output(sh, "multiWatch: expected [\"command\", \"command\"]\n"); return 2;
    }
    for (size_t i = 0; i < count; ++i)
        if (watch_spawn(sh, &items[i]) != 0) { output(sh, "multiWatch: could not start command\n"); return 1; }

    int stopped = 0;
    while (!stopped) {
        struct pollfd pfds[TERM_MAX_STAGES + 1];
        int which[TERM_MAX_STAGES + 1];
        nfds_t nfds = 0;
        pfds[nfds] = (struct pollfd){ .fd = sh->input_fd, .events = POLLIN };
        which[nfds++] = -1;
        time_t now = time(NULL), nearest = now + 1;
        for (size_t i = 0; i < count; ++i) {
            WatchCommand *item = &items[i];
            if (item->active) {
                pfds[nfds] = (struct pollfd){ .fd = item->fd, .events = POLLIN | POLLHUP };
                which[nfds++] = (int)i;
            } else if (item->next_run <= now) {
                if (watch_spawn(sh, item) != 0) { item->next_run = now + 2; continue; }
                pfds[nfds] = (struct pollfd){ .fd = item->fd, .events = POLLIN | POLLHUP };
                which[nfds++] = (int)i;
            } else if (item->next_run < nearest) nearest = item->next_run;
        }
        int timeout = nearest <= now ? 0 : (int)((nearest - now) * 1000);
        if (nfds == 1 && timeout == 0) timeout = 100;
        int ready;
        do { ready = poll(pfds, nfds, timeout); } while (ready < 0 && errno == EINTR);
        if (ready < 0) break;
        for (nfds_t j = 0; j < nfds; ++j) if (pfds[j].revents) {
            if (which[j] == -1) {
                RequestHeader h;
                if (read_all(sh->input_fd, &h, sizeof(h)) <= 0) { stopped = 1; break; }
                char payload[TERM_INPUT_CAP];
                if (h.length > TERM_INPUT_CAP || (h.length && read_all(sh->input_fd, payload, h.length) <= 0)) { stopped = 1; break; }
                if (h.kind == REQUEST_INTERRUPT || h.kind == REQUEST_SHUTDOWN) {
                    stopped = 1;
                    if (h.kind == REQUEST_SHUTDOWN) sh->shutdown = 1;
                    break;
                }
            } else {
                WatchCommand *item = &items[which[j]];
                char buffer[4096];
                for (;;) {
                    ssize_t n = read(item->fd, buffer, sizeof(buffer));
                    if (n < 0 && errno == EINTR) continue;
                    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                    if (n <= 0) {
                        watch_header(sh, item);
                        if (!item->ended_newline) output(sh, "\n");
                        output(sh, "----------------------------------------------------\n");
                        close(item->fd); item->fd = -1; item->active = 0;
                        int status; (void)waitpid(item->pid, &status, 0);
                        item->next_run = time(NULL) + 2;
                        break;
                    }
                    watch_header(sh, item);
                    (void)write_all(sh->output_fd, buffer, (size_t)n);
                    item->ended_newline = buffer[n - 1] == '\n';
                }
            }
        }
    }
    for (size_t i = 0; i < count; ++i) if (items[i].active) {
        (void)kill(-items[i].pid, SIGTERM);
        (void)kill(items[i].pid, SIGKILL);
        close(items[i].fd);
        (void)waitpid(items[i].pid, NULL, 0);
    }
    output(sh, "^C\n");
    return 0;
}

static int builtin(ShellContext *sh, ParsedCommand *cmd, const char *line, int *handled)
{
    char **argv = cmd->text;
    argv[cmd->count] = NULL;
    *handled = 1;
    if (!cmd->count) return 0;
    if (!strcmp(argv[0], "cd")) {
        const char *dest = cmd->count > 1 ? argv[1] : getenv("HOME");
        if (!dest) dest = sh->cwd;
        char candidate[4096];
        if (dest[0] == '/') snprintf(candidate, sizeof(candidate), "%s", dest);
        else snprintf(candidate, sizeof(candidate), "%s/%s", sh->cwd, dest);
        char resolved[4096];
        if (!realpath(candidate, resolved) || access(resolved, X_OK) != 0) {
            outputf(sh, "cd: %s: no such directory\n", dest);
            return 1;
        }
        struct stat st;
        if (stat(resolved, &st) != 0 || !S_ISDIR(st.st_mode)) {
            outputf(sh, "cd: %s: not a directory\n", dest);
            return 1;
        }
        snprintf(sh->cwd, sizeof(sh->cwd), "%s", resolved);
        return 0;
    }
    if (!strcmp(argv[0], "exit")) { sh->shutdown = 1; return 0; }
    if (!strcmp(argv[0], "history")) {
        history_print(sh->history, sh->history_lock, sh->output_fd); return 0;
    }
    if (!strcmp(argv[0], "jobs")) {
        for (int i = 0; i < TERM_MAX_JOBS; ++i) if (sh->jobs[i].running) {
            int status;
            while (waitpid(-sh->jobs[i].pid, &status, WNOHANG) > 0) {}
            if (waitpid(-sh->jobs[i].pid, &status, WNOHANG) < 0 && errno == ECHILD)
                sh->jobs[i].running = 0;
        }
        for (int i = 0; i < TERM_MAX_JOBS; ++i) if (sh->jobs[i].running) {
            char b[TERM_OUTPUT_CAP];
            snprintf(b, sizeof(b), "[%d] %s %s\n", sh->jobs[i].id,
                     sh->jobs[i].stopped ? "Stopped" : "Running", sh->jobs[i].command);
            output(sh, b);
        }
        return 0;
    }
    if (!strcmp(argv[0], "fg") || !strcmp(argv[0], "bg")) {
        int index = -1;
        if (cmd->count > 1) {
            int wanted = atoi(argv[1] + (argv[1][0] == '%'));
            for (int i = 0; i < TERM_MAX_JOBS; ++i)
                if (sh->jobs[i].running && sh->jobs[i].id == wanted) { index = i; break; }
        } else {
            for (int i = TERM_MAX_JOBS - 1; i >= 0; --i)
                if (sh->jobs[i].running) { index = i; break; }
        }
        if (index >= TERM_MAX_JOBS) { output(sh, "terminator: no such job\n"); return 1; }
        Job *j = &sh->jobs[index];
        if (!strcmp(argv[0], "bg")) {
            kill(-j->pid, SIGCONT); j->stopped = 0;
            char b[TERM_OUTPUT_CAP]; snprintf(b, sizeof(b), "[%d]+ %s &\n", j->id, j->command); output(sh, b);
            return 0;
        }
        sh->foreground_pgid = j->pid;
        kill(-j->pid, SIGCONT); j->stopped = 0;
        int done = 0;
        while (!done) {
            int status;
            pid_t w = waitpid(-j->pid, &status, WNOHANG | WUNTRACED);
            if (w > 0 && WIFSTOPPED(status)) {
                (void)kill(-j->pid, SIGSTOP); j->stopped = 1; done = 1;
            } else if (w < 0 && errno == ECHILD) {
                j->running = 0; done = 1;
            } else read_wait_control(sh);
        }
        sh->foreground_pgid = 0;
        return 0;
    }
    if (!strcmp(argv[0], "source")) {
        if (cmd->count < 2) { output(sh, "source: filename required\n"); return 2; }
        char path[TERM_INPUT_CAP];
        if (argv[1][0] == '/') snprintf(path, sizeof(path), "%s", argv[1]);
        else snprintf(path, sizeof(path), "%s/%s", sh->cwd, argv[1]);
        FILE *f = fopen(path, "r");
        if (!f) { char b[TERM_OUTPUT_CAP]; snprintf(b, sizeof(b), "source: %s: %s\n", argv[1], strerror(errno)); output(sh, b); return 1; }
        char buffer[TERM_INPUT_CAP]; int status = 0;
        while (fgets(buffer, sizeof(buffer), f)) {
            size_t len = strlen(buffer);
            while (len && (buffer[len - 1] == '\n' || buffer[len - 1] == '\r')) buffer[--len] = '\0';
            char *first = buffer; while (isspace((unsigned char)*first)) ++first;
            if (!*first || *first == '#') continue;
            shell_run_request(sh, buffer); status = sh->last_status;
            if (sh->shutdown) break;
        }
        fclose(f); return status;
    }
    if (!strcmp(argv[0], "multiWatch")) {
        return multi_watch(sh, line);
    }
    *handled = 0;
    (void)line;
    return 0;
}

static const char *top_level_semicolon(const char *line)
{
    int single = 0, dbl = 0, escaped = 0, backtick = 0, depth = 0;
    for (size_t i = 0; line[i]; ++i) {
        char c = line[i];
        if (escaped) { escaped = 0; continue; }
        if (c == '\\' && !single) { escaped = 1; continue; }
        if (c == '`' && !single) { backtick = !backtick; continue; }
        if (c == '\'' && !dbl && !backtick) single = !single;
        else if (c == '"' && !single && !backtick) dbl = !dbl;
        else if (!single && c == '(' && i > 0 && line[i - 1] == '$') ++depth;
        else if (!single && c == ')' && depth > 0) --depth;
        else if (c == ';' && !single && !dbl && !backtick && depth == 0) return line + i;
    }
    return NULL;
}

void shell_run_request(ShellContext *sh, const char *line)
{
    if (!line || !*line) return;
    const char *semi = top_level_semicolon(line);
    if (semi) {
        char left[TERM_INPUT_CAP]; size_t n = (size_t)(semi - line);
        if (n >= sizeof(left)) n = sizeof(left) - 1;
        memcpy(left, line, n); left[n] = '\0';
        shell_run_request(sh, left);
        if (!sh->shutdown) shell_run_request(sh, semi + 1);
        return;
    }
    ParsedCommand *cmds = calloc(TERM_MAX_STAGES, sizeof(*cmds));
    if (!cmds) { output(sh, "terminator: out of memory\n"); sh->last_status = 1; return; }
    size_t n = 0;
    int parsed = parse_commands(sh, line, cmds, &n);
    if (parsed == 1) { output(sh, "> "); free(cmds); return; }
    if (parsed < 0) { output(sh, "terminator: syntax error\n"); sh->last_status = 2; free(cmds); return; }
    if (parsed == 2) {
        /* Top-level semicolons are split quote-aware by the GUI before dispatch. */
        output(sh, "terminator: use one command per line\n"); sh->last_status = 2; free(cmds); return;
    }
    if (!n) { sh->last_status = 0; free(cmds); return; }
    if (n == 1) {
        int input_fd = -1, output_fd = -1;
        int previous_output_fd = sh->output_fd;
        if (cmds[0].input_path) {
            input_fd = open_in_cwd(sh, cmds[0].input_path, O_RDONLY, 0);
            if (input_fd < 0) {
                char message[TERM_OUTPUT_CAP];
                snprintf(message, sizeof(message), "%s: %s\n", cmds[0].input_path, strerror(errno));
                output(sh, message); sh->last_status = 1; free(cmds); return;
            }
        }
        if (cmds[0].output_path) {
            int flags = O_WRONLY | O_CREAT | (cmds[0].append ? O_APPEND : O_TRUNC);
            output_fd = open_in_cwd(sh, cmds[0].output_path, flags, 0666);
            if (output_fd < 0) {
                char message[TERM_OUTPUT_CAP];
                snprintf(message, sizeof(message), "%s: %s\n", cmds[0].output_path, strerror(errno));
                output(sh, message); if (input_fd >= 0) close(input_fd);
                sh->last_status = 1; free(cmds); return;
            }
            sh->output_fd = output_fd;
        }
        int handled;
        int status = builtin(sh, &cmds[0], line, &handled);
        if (output_fd >= 0) sh->output_fd = previous_output_fd;
        if (output_fd >= 0) close(output_fd);
        if (input_fd >= 0) close(input_fd);
        if (handled) { sh->last_status = status; free(cmds); return; }
    }
    sh->last_status = run_pipeline(sh, cmds, n, cmds[n - 1].background, line);
    free(cmds);
}

void shell_handle_request(ShellContext *sh, uint32_t kind,
                          const char *payload, size_t payload_length)
{
    if (kind == REQUEST_RUN) {
        char command[TERM_INPUT_CAP];
        if (payload_length >= sizeof(command)) payload_length = sizeof(command) - 1;
        memcpy(command, payload, payload_length); command[payload_length] = '\0';
        shell_run_request(sh, command);
    } else if (kind == REQUEST_INTERRUPT && sh->foreground_pgid > 0) {
        kill(-sh->foreground_pgid, SIGINT);
    } else if (kind == REQUEST_SUSPEND && sh->foreground_pgid > 0) {
        kill(-sh->foreground_pgid, SIGTSTP);
    } else if (kind == REQUEST_SHUTDOWN) {
        sh->shutdown = 1;
        for (int i = 0; i < TERM_MAX_JOBS; ++i)
            if (sh->jobs[i].running) kill(-sh->jobs[i].pid, SIGHUP);
    }
}

void shell_worker_loop(int input_fd, int output_fd, HistoryStore *history,
                       sem_t *history_lock)
{
    ShellContext sh;
    memset(&sh, 0, sizeof(sh));
    sh.input_fd = input_fd;
    sh.output_fd = output_fd;
    sh.history = history;
    sh.history_lock = history_lock;
    (void)fcntl(input_fd, F_SETFD, FD_CLOEXEC);
    sh.next_job_id = 0;
    if (!getcwd(sh.cwd, sizeof(sh.cwd))) snprintf(sh.cwd, sizeof(sh.cwd), "/");
    char marker[sizeof(sh.cwd) + 8];
    int marker_length = snprintf(marker, sizeof(marker), "\x1fP%s\x1f", sh.cwd);
    if (marker_length > 0) (void)write_all(output_fd, marker, (size_t)marker_length);
    for (;;) {
        RequestHeader h;
        int r = read_all(input_fd, &h, sizeof(h));
        if (r <= 0) break;
        if (h.length > TERM_INPUT_CAP) break;
        char payload[TERM_INPUT_CAP];
        if (h.length && read_all(input_fd, payload, h.length) <= 0) break;
        shell_handle_request(&sh, h.kind, payload, h.length);
        if (h.kind == REQUEST_RUN) {
            marker_length = snprintf(marker, sizeof(marker), "\x1fR%s\x1f", sh.cwd);
            if (marker_length > 0) (void)write_all(output_fd, marker, (size_t)marker_length);
        }
        if (sh.shutdown) break;
    }
    for (int i = 0; i < TERM_MAX_JOBS; ++i)
        if (sh.jobs[i].running) kill(-sh.jobs[i].pid, SIGHUP);
}
