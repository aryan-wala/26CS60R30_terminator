#define _DARWIN_C_SOURCE 1
#define _DEFAULT_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "terminator.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static void lock_history(sem_t *lock)
{
    while (sem_wait(lock) != 0 && errno == EINTR) {}
}

static void unlock_history(sem_t *lock) { (void)sem_post(lock); }

static void insert_command(HistoryStore *store, const char *command)
{
    snprintf(store->commands[store->next], TERM_INPUT_CAP, "%s", command);
    store->next = (store->next + 1) % TERM_HISTORY_CAP;
    if (store->count < TERM_HISTORY_CAP) ++store->count;
}

static void write_escaped(FILE *f, const char *command)
{
    for (const char *p = command; *p; ++p) {
        if (*p == '\n') fputs("\\n", f);
        else if (*p == '\\') fputs("\\\\", f);
        else fputc(*p, f);
    }
    fputc('\n', f);
}

static void rewrite_history_file(HistoryHandle *history)
{
    char temporary[4200];
    snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", history->history_path, (long)getpid());
    FILE *f = fopen(temporary, "w");
    if (!f) return;
    lock_history(history->lock);
    HistoryStore *s = history->store;
    size_t first = (s->next + TERM_HISTORY_CAP - s->count) % TERM_HISTORY_CAP;
    for (size_t i = 0; i < s->count; ++i)
        write_escaped(f, s->commands[(first + i) % TERM_HISTORY_CAP]);
    unlock_history(history->lock);
    if (fclose(f) == 0) (void)rename(temporary, history->history_path);
    else (void)unlink(temporary);
}

static void decode_history_line(char *line)
{
    size_t r = 0, w = 0;
    while (line[r]) {
        if (line[r] == '\\' && line[r + 1] == 'n') { line[w++] = '\n'; r += 2; }
        else if (line[r] == '\\' && line[r + 1] == '\\') { line[w++] = '\\'; r += 2; }
        else line[w++] = line[r++];
    }
    line[w] = '\0';
}

int history_open(HistoryHandle *history)
{
    memset(history, 0, sizeof(*history));
    history->store = mmap(NULL, sizeof(HistoryStore), PROT_READ | PROT_WRITE,
                          MAP_SHARED | MAP_ANON, -1, 0);
    if (history->store == MAP_FAILED) { history->store = NULL; return -1; }
    memset(history->store, 0, sizeof(HistoryStore));
    snprintf(history->semaphore_name, sizeof(history->semaphore_name),
             "/terminator-history-%ld", (long)getpid());
    (void)sem_unlink(history->semaphore_name);
    history->lock = sem_open(history->semaphore_name, O_CREAT | O_EXCL, 0600, 1);
    if (history->lock == SEM_FAILED) { history->lock = NULL; munmap(history->store, sizeof(HistoryStore)); history->store = NULL; return -1; }
    const char *home = getenv("HOME");
    if (!home) home = ".";
    snprintf(history->history_path, sizeof(history->history_path), "%s/.terminator_history", home);
    FILE *f = fopen(history->history_path, "r");
    if (f) {
        char *line = NULL; size_t cap = 0;
        while (getline(&line, &cap, f) >= 0) {
            size_t len = strlen(line);
            while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
            decode_history_line(line);
            insert_command(history->store, line);
        }
        free(line); fclose(f);
    }
    if (history->store->count == TERM_HISTORY_CAP) rewrite_history_file(history);
    return 0;
}

void history_close(HistoryHandle *history)
{
    if (history->lock) { sem_close(history->lock); sem_unlink(history->semaphore_name); }
    if (history->store) munmap(history->store, sizeof(HistoryStore));
    memset(history, 0, sizeof(*history));
}

void history_add(HistoryHandle *history, const char *command)
{
    if (!history->store || !history->lock || !command || !*command) return;
    lock_history(history->lock);
    insert_command(history->store, command);
    unlock_history(history->lock);
    FILE *f = fopen(history->history_path, "a");
    if (f) { write_escaped(f, command); fclose(f); }
    if (history->store->count == TERM_HISTORY_CAP && ++history->file_trim_counter >= 100) {
        history->file_trim_counter = 0;
        rewrite_history_file(history);
    }
}

size_t history_count(HistoryHandle *history)
{
    if (!history->store || !history->lock) return 0;
    lock_history(history->lock);
    size_t count = history->store->count;
    unlock_history(history->lock);
    return count;
}

int history_latest(HistoryHandle *history, size_t back, char *buffer, size_t capacity)
{
    if (!history->store || !history->lock) return -1;
    lock_history(history->lock);
    HistoryStore *s = history->store;
    if (back >= s->count) { unlock_history(history->lock); return -1; }
    size_t index = (s->next + TERM_HISTORY_CAP - 1 - back) % TERM_HISTORY_CAP;
    snprintf(buffer, capacity, "%s", s->commands[index]);
    unlock_history(history->lock);
    return 0;
}

void history_print(HistoryStore *store, sem_t *lock, int output_fd)
{
    if (!store || !lock) return;
    lock_history(lock);
    size_t count = store->count < 1000 ? store->count : 1000;
    size_t first = (store->next + TERM_HISTORY_CAP - count) % TERM_HISTORY_CAP;
    char line[TERM_INPUT_CAP + 32];
    for (size_t i = 0; i < count; ++i) {
        size_t index = (first + i) % TERM_HISTORY_CAP;
        int n = snprintf(line, sizeof(line), "%5zu  %s\n", i + 1, store->commands[index]);
        if (n > 0) (void)write_all(output_fd, line, (size_t)n);
    }
    unlock_history(lock);
}

static size_t longest_common_substring(const char *a, const char *b)
{
    size_t alen = strlen(a), blen = strlen(b), best = 0;
    uint16_t *dp = calloc(alen + 1, sizeof(*dp));
    if (!dp) return 0;
    for (size_t i = 0; i < blen; ++i) {
        for (size_t j = alen; j > 0; --j) {
            dp[j] = a[j - 1] == b[i] ? (uint16_t)(dp[j - 1] + 1) : 0;
            if (dp[j] > best) best = dp[j];
        }
    }
    free(dp);
    return best;
}

int history_find(HistoryHandle *history, const char *term, char ***results, size_t *count)
{
    *results = NULL; *count = 0;
    if (!history->store || !history->lock || !*term) return 0;
    char **found = calloc(TERM_HISTORY_CAP, sizeof(*found));
    if (!found) return -1;
    lock_history(history->lock);
    HistoryStore *s = history->store;
    for (size_t back = 0; back < s->count; ++back) {
        size_t index = (s->next + TERM_HISTORY_CAP - 1 - back) % TERM_HISTORY_CAP;
        if (strcmp(s->commands[index], term) == 0) {
            found[0] = strdup(s->commands[index]); *count = found[0] ? 1 : 0;
            unlock_history(history->lock); *results = found; return 0;
        }
    }
    size_t best = 0;
    for (size_t i = 0; i < s->count; ++i) {
        size_t index = (s->next + TERM_HISTORY_CAP - s->count + i) % TERM_HISTORY_CAP;
        size_t score = longest_common_substring(term, s->commands[index]);
        if (score > best) {
            for (size_t k = 0; k < *count; ++k) free(found[k]);
            *count = 0; best = score;
        }
        if (score == best && score > 2) found[(*count)++] = strdup(s->commands[index]);
    }
    unlock_history(history->lock);
    if (best <= 2) { for (size_t k = 0; k < *count; ++k) free(found[k]); *count = 0; }
    *results = found;
    return 0;
}

void history_free_results(char **results, size_t count)
{
    if (!results) return;
    for (size_t i = 0; i < count; ++i) free(results[i]);
    free(results);
}
