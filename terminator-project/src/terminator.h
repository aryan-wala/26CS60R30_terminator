#ifndef TERMINATOR_H
#define TERMINATOR_H

#include <pthread.h>
#include <semaphore.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define TERM_MAX_TABS 8
#define TERM_INPUT_CAP 4096
#define TERM_OUTPUT_CAP 2048
#define TERM_SCROLLBACK 512
#define TERM_MAX_JOBS 64
#define TERM_MAX_STAGES 32
#define TERM_MAX_ARGS 128
#define TERM_HISTORY_CAP 10000
#define TERM_COMPLETION_CHOICES 64
#define TERM_NAME_CAP 256

typedef enum {
    REQUEST_RUN = 1,
    REQUEST_INTERRUPT,
    REQUEST_SUSPEND,
    REQUEST_SHUTDOWN
} RequestKind;

typedef struct {
    uint32_t kind;
    uint32_t length;
} RequestHeader;

typedef struct {
    uint32_t count;
    uint32_t next;
    char commands[TERM_HISTORY_CAP][TERM_INPUT_CAP];
} HistoryStore;

typedef struct {
    HistoryStore *store;
    sem_t *lock;
    char semaphore_name[128];
    char history_path[4096];
    unsigned file_trim_counter;
} HistoryHandle;

typedef struct {
    pid_t pid;
    int id;
    char command[TERM_OUTPUT_CAP];
    int running;
    int stopped;
} Job;

typedef struct {
    char cwd[4096];
    int last_status;
    pid_t foreground_pgid;
    pid_t supervisor_pgid;
    Job jobs[TERM_MAX_JOBS];
    int next_job_id;
    int output_fd;
    int input_fd;
    int shutdown;
    HistoryStore *history;
    sem_t *history_lock;
} ShellContext;

typedef struct {
    char input[TERM_INPUT_CAP];
    size_t length;
    size_t cursor;
    char lines[TERM_SCROLLBACK][TERM_OUTPUT_CAP];
    size_t line_count;
    size_t scroll_offset;
    int continuation;
    size_t history_browse;
    char saved_input[TERM_INPUT_CAP];
    int search_mode;
    int completion_pending;
    char completion_names[TERM_COMPLETION_CHOICES][TERM_NAME_CAP];
    size_t completion_count;
    char completion_path[TERM_INPUT_CAP];
    size_t completion_start;
    size_t completion_cursor;
    char completion_number[4];
} TabView;

typedef struct {
    int to_worker;
    int from_worker;
    pid_t process;
    pthread_t thread;
    int is_thread;
    int alive;
    int busy;
    int output_eof;
    char cwd[4096];
    char marker[4096];
    size_t marker_length;
    int in_marker;
    TabView view;
} Tab;

typedef struct {
    char *text[TERM_MAX_ARGS];
    char storage[TERM_INPUT_CAP];
    size_t storage_used;
    size_t count;
    int append;
    char *input_path;
    char *output_path;
    int background;
} ParsedCommand;

int write_all(int fd, const void *buffer, size_t length);
int read_all(int fd, void *buffer, size_t length);
void shell_worker_loop(int input_fd, int output_fd, HistoryStore *history,
                       sem_t *history_lock);
void shell_run_request(ShellContext *shell, const char *command);
void shell_handle_request(ShellContext *shell, uint32_t kind,
                          const char *payload, size_t payload_length);
void tab_append_output(TabView *view, const char *data, size_t length);
int history_open(HistoryHandle *history);
void history_close(HistoryHandle *history);
void history_add(HistoryHandle *history, const char *command);
size_t history_count(HistoryHandle *history);
int history_latest(HistoryHandle *history, size_t back, char *buffer, size_t capacity);
void history_print(HistoryStore *store, sem_t *lock, int output_fd);
int history_find(HistoryHandle *history, const char *term,
                 char ***results, size_t *count);
void history_free_results(char **results, size_t count);
int tab_send_request(Tab *tab, uint32_t kind, const char *payload,
                     size_t payload_length);
int tab_start(Tab *tab, int process_mode, int x_connection_fd,
              HistoryStore *history, sem_t *history_lock);
void tab_stop(Tab *tab);

#endif
