#define _POSIX_C_SOURCE 200809L
#include "terminator.h"

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

typedef enum {
    BIND_LEFT, BIND_RIGHT, BIND_LINE_START, BIND_LINE_END,
    BIND_HISTORY_UP, BIND_HISTORY_DOWN, BIND_DELETE_START, BIND_DELETE_END,
    BIND_DELETE_WORD, BIND_CLEAR, BIND_EOF, BIND_COPY, BIND_PASTE,
    BIND_HISTORY_SEARCH, BIND_COUNT
} BindingId;

typedef struct { const char *name; KeySym key; unsigned modifiers; } KeyBinding;

typedef struct {
    Display *display;
    Window window;
    GC gc;
    XFontStruct *font;
    Atom wm_delete;
    Atom clipboard_atom;
    Atom utf8_atom;
    Atom targets_atom;
    Atom paste_property;
    char clipboard[TERM_INPUT_CAP];
    KeyBinding bindings[BIND_COUNT];
    char config_message[2048];
    HistoryHandle history;
    Tab tabs[TERM_MAX_TABS];
    int tab_count;
    int active;
    int process_mode;
    int running;
    int width;
    int height;
} App;

static const char *binding_names[BIND_COUNT] = {
    "cursor_left", "cursor_right", "line_start", "line_end",
    "history_up", "history_down", "delete_to_start", "delete_to_end",
    "delete_word", "clear_screen", "close_tab", "copy", "paste",
    "history_search"
};

static void config_warning(App *app, const char *message)
{
    size_t used = strlen(app->config_message);
    if (used + strlen(message) + 2 < sizeof(app->config_message)) {
        snprintf(app->config_message + used, sizeof(app->config_message) - used,
                 "%s\n", message);
    }
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) ++s;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
    return s;
}

static KeySym normalize_keysym(KeySym key)
{
    if (key >= XK_A && key <= XK_Z) return key + (XK_a - XK_A);
    return key;
}

static int parse_binding(const char *value, KeySym *key, unsigned *modifiers)
{
    char copy[128]; snprintf(copy, sizeof(copy), "%s", value);
    char *save = NULL, *part = strtok_r(copy, "+", &save);
    *modifiers = 0; *key = NoSymbol; int keys = 0;
    while (part) {
        if (!strcasecmp(part, "ctrl") || !strcasecmp(part, "control")) *modifiers |= ControlMask;
        else if (!strcasecmp(part, "shift")) *modifiers |= ShiftMask;
        else if (!strcasecmp(part, "alt") || !strcasecmp(part, "option")) *modifiers |= Mod1Mask;
        else if (!strcasecmp(part, "cmd") || !strcasecmp(part, "command")) *modifiers |= Mod2Mask;
        else { *key = normalize_keysym(XStringToKeysym(part)); ++keys; }
        part = strtok_r(NULL, "+", &save);
    }
    return keys == 1 && *key != NoSymbol;
}

static int binding_conflict(App *app, BindingId self, KeySym key, unsigned modifiers)
{
    for (int i = 0; i < BIND_COUNT; ++i)
        if (i != (int)self && app->bindings[i].key == key && app->bindings[i].modifiers == modifiers)
            return 1;
    return 0;
}

static void load_bindings(App *app)
{
    static const char *defaults[BIND_COUNT] = {
        "Left", "Right", "Ctrl+A", "Ctrl+E", "Up", "Down", "Ctrl+U",
        "Ctrl+K", "Ctrl+W", "Ctrl+L", "Ctrl+D", "Ctrl+Shift+C",
        "Ctrl+Shift+V", "Ctrl+R"
    };
    for (int i = 0; i < BIND_COUNT; ++i) {
        app->bindings[i].name = binding_names[i];
        (void)parse_binding(defaults[i], &app->bindings[i].key, &app->bindings[i].modifiers);
    }
    const char *home = getenv("HOME"); if (!home) return;
    char path[4096]; snprintf(path, sizeof(path), "%s/.terminatorrc", home);
    FILE *file = fopen(path, "r"); if (!file) return;
    char *line = NULL; size_t capacity = 0;
    while (getline(&line, &capacity, file) >= 0) {
        char *content = trim(line);
        if (!*content || *content == '#') continue;
        char *equals = strchr(content, '=');
        if (!equals) { config_warning(app, "terminator: invalid .terminatorrc line; using default binding"); continue; }
        *equals++ = '\0';
        char *name = trim(content), *value = trim(equals);
        int id = -1;
        for (int i = 0; i < BIND_COUNT; ++i) if (!strcmp(name, binding_names[i])) id = i;
        KeySym key; unsigned mods;
        if (id < 0 || !parse_binding(value, &key, &mods) || binding_conflict(app, (BindingId)id, key, mods)) {
            config_warning(app, "terminator: invalid or conflicting binding in .terminatorrc; using default");
            continue;
        }
        app->bindings[id].key = key; app->bindings[id].modifiers = mods;
    }
    free(line); fclose(file);
}

static int binding_matches(App *app, XKeyEvent *event, BindingId id)
{
    KeySym key = normalize_keysym(XLookupKeysym(event, 0));
    unsigned mods = event->state & (ControlMask | ShiftMask | Mod1Mask | Mod2Mask | Mod4Mask);
    return key == app->bindings[id].key && mods == app->bindings[id].modifiers;
}

static int command_paste_matches(XKeyEvent *event)
{
    KeySym key = normalize_keysym(XLookupKeysym(event, 0));
    unsigned mods = event->state & (ControlMask | ShiftMask | Mod1Mask | Mod2Mask | Mod4Mask);
    return key == XK_v && mods == (ShiftMask | Mod2Mask);
}

static void append_text(TabView *view, const char *s)
{
    tab_append_output(view, s, strlen(s));
}

static int text_width(App *app, const char *text, int length)
{
    if (length <= 0) return 0;
    return app->font ? XTextWidth(app->font, text, length) : length * 8;
}

/* Return a visual line break that fits the available pixels, preferring the
   last space before the edge. A single long word is split when necessary. */
static size_t wrapped_end(App *app, const char *text, size_t start, size_t length,
                          int available)
{
    size_t end = start, space = start;
    while (end < length && text_width(app, text + start, (int)(end + 1 - start)) <= available) {
        if (isspace((unsigned char)text[end])) space = end;
        ++end;
    }
    if (end == length) return end;
    if (end == start) return start + 1;
    if (space > start) return space + 1;
    return end;
}

static void draw_tab_bar(App *app)
{
    int x = 12;
    for (int i = 0; i < app->tab_count; ++i) {
        char label[128];
        snprintf(label, sizeof(label), i == app->active ? "[%d: Tab %d]" : "[ %d ]", i + 1, i + 1);
        XDrawString(app->display, app->window, app->gc, x, 22, label, (int)strlen(label));
        x += 112;
    }
    XDrawString(app->display, app->window, app->gc, x, 22, "[ + ]", 5);
    XDrawString(app->display, app->window, app->gc, 12, app->height - 14,
                app->process_mode ? "process mode  |  Ctrl+Shift+T new tab  |  Ctrl+Shift+W close tab"
                                  : "thread mode  |  Ctrl+Shift+T new tab  |  Ctrl+Shift+W close tab",
                app->process_mode ? 58 : 56);
}

static void draw_app(App *app)
{
    XClearWindow(app->display, app->window);
    draw_tab_bar(app);
    TabView *v = &app->tabs[app->active].view;
    int line_height = 18;
    size_t starts[TERM_INPUT_CAP]; size_t input_lines = 1;
    starts[0] = 0;
    for (size_t i = 0; i < v->length && input_lines < TERM_INPUT_CAP; ++i)
        if (v->input[i] == '\n') starts[input_lines++] = i + 1;

    /* The editor is rendered after scrollback, like a real shell prompt. If
       output ended mid-line, put the next prompt on the following line. */
    size_t partial_output_length = strlen(v->lines[v->line_count]);
    size_t input_base = v->line_count + (partial_output_length != 0);
    size_t total = input_base + input_lines;
    int content_rows = (app->height - 82) / line_height;
    if (content_rows < 1) content_rows = 1;
    size_t end = total > v->scroll_offset ? total - v->scroll_offset : 0;
    size_t first = end > (size_t)content_rows ? end - (size_t)content_rows : 0;
    int selecting_completion = v->completion_pending && v->completion_count > 9;
    size_t visual_extra = 0;
    size_t cursor_line = 0;
    if (!selecting_completion)
        for (size_t i = 0; i < v->cursor; ++i) if (v->input[i] == '\n') ++cursor_line;
    size_t cursor_line_start = starts[cursor_line];
    size_t cursor_column = selecting_completion ? strlen(v->completion_number) : v->cursor - cursor_line_start;
    int cursor_x = 12, cursor_y = 44;
    for (size_t row = first; row < end; ++row) {
        int y = 44 + (int)(row - first + visual_extra) * line_height;
        if (row < v->line_count) {
            const char *line = v->lines[row];
            size_t length = strlen(line), offset = 0;
            while (offset < length) {
                size_t finish = wrapped_end(app, line, offset, length, app->width - 24);
                if (finish <= offset) break;
                while (finish > offset && isspace((unsigned char)line[finish - 1])) --finish;
                XDrawString(app->display, app->window, app->gc, 12, y, line + offset, (int)(finish - offset));
                offset = finish;
                while (offset < length && isspace((unsigned char)line[offset])) ++offset;
                if (offset < length) { ++visual_extra; y += line_height; }
            }
            continue;
        }
        if (partial_output_length && row == v->line_count) {
            const char *line = v->lines[v->line_count];
            size_t offset = 0;
            while (offset < partial_output_length) {
                size_t finish = wrapped_end(app, line, offset, partial_output_length, app->width - 24);
                if (finish <= offset) break;
                while (finish > offset && isspace((unsigned char)line[finish - 1])) --finish;
                XDrawString(app->display, app->window, app->gc, 12, y, line + offset, (int)(finish - offset));
                offset = finish;
                while (offset < partial_output_length && isspace((unsigned char)line[offset])) ++offset;
                if (offset < partial_output_length) { ++visual_extra; y += line_height; }
            }
            continue;
        }

        size_t line = row - input_base;
        size_t begin = starts[line];
        size_t finish = line + 1 < input_lines ? starts[line + 1] - 1 : v->length;
        const char *prompt;
        if (line == 0 && v->search_mode) prompt = "Enter search term: ";
        else if (line == 0 && v->completion_pending && v->completion_count > 9) prompt = "Select completion number: ";
        else if (line == 0) prompt = app->tabs[app->active].busy ? "running> " : "user@terminator> ";
        else prompt = "> ";
        int prompt_len = (int)strlen(prompt);
        int prompt_width = text_width(app, prompt, prompt_len);
        int text_x = 12 + prompt_width;
        int available_width = app->width - text_x - 12;
        if (available_width < 1) available_width = 1;
        XDrawString(app->display, app->window, app->gc, 12, y, prompt, prompt_len);

        const char *line_text = selecting_completion ? v->completion_number : v->input + begin;
        size_t line_text_length = selecting_completion ? strlen(v->completion_number) : finish - begin;
        size_t visible_start = 0;
        size_t line_cursor = line == cursor_line ? cursor_column : line_text_length;
        size_t offset = visible_start;
        int cursor_drawn = 0;
        while (offset < line_text_length) {
            size_t finish = wrapped_end(app, line_text, offset, line_text_length, available_width);
            if (finish <= offset) break;
            XDrawString(app->display, app->window, app->gc,
                        offset == visible_start ? text_x : 12, y,
                        line_text + offset, (int)(finish - offset));
            if (line == cursor_line && cursor_column >= offset && cursor_column <= finish) {
                cursor_x = (offset == visible_start ? text_x : 12) +
                           text_width(app, line_text + offset, (int)(cursor_column - offset));
                cursor_y = y;
                cursor_drawn = 1;
            }
            offset = finish;
            while (offset < line_text_length && isspace((unsigned char)line_text[offset])) ++offset;
            if (offset < line_text_length) { ++visual_extra; y += line_height; }
        }
        if (line == cursor_line && !cursor_drawn) {
            if (line_cursor == line_text_length)
                cursor_x = (offset == visible_start ? text_x : 12) + text_width(app, line_text + offset,
                    (int)(line_cursor - offset));
            cursor_y = y;
        }
    }
    XDrawLine(app->display, app->window, app->gc, cursor_x, cursor_y - 14, cursor_x, cursor_y + 4);
}

static int start_tab(App *app)
{
    if (app->tab_count >= TERM_MAX_TABS) return -1;
    Tab *tab = &app->tabs[app->tab_count];
    memset(tab, 0, sizeof(*tab));
    tab->to_worker = tab->from_worker = -1;
    if (tab_start(tab, app->process_mode, ConnectionNumber(app->display),
                  app->history.store, app->history.lock) != 0) return -1;
    app->active = app->tab_count++;
    return 0;
}

static void close_tab_at(App *app, int index)
{
    if (index < 0 || index >= app->tab_count) return;
    if (app->tab_count == 1) { tab_stop(&app->tabs[0]); app->tab_count = 0; app->running = 0; return; }
    tab_stop(&app->tabs[index]);
    for (int i = index; i + 1 < app->tab_count; ++i) app->tabs[i] = app->tabs[i + 1];
    --app->tab_count;
    if (app->active > index) --app->active;
    else if (app->active == index && app->active >= app->tab_count) app->active = app->tab_count - 1;
}

static void close_active_tab(App *app)
{
    close_tab_at(app, app->active);
}

static int command_incomplete(const char *s)
{
    int single = 0, dbl = 0, escaped = 0, backtick = 0, depth = 0;
    size_t n = strlen(s);
    for (size_t i = 0; i < n; ++i) {
        char c = s[i];
        if (escaped) { escaped = 0; continue; }
        if (c == '\\' && !single) { escaped = 1; continue; }
        if (c == '`' && !single) { backtick = !backtick; continue; }
        if (c == '\'' && !dbl && !backtick) single = !single;
        else if (c == '"' && !single && !backtick) dbl = !dbl;
        else if (!single && c == '(' && i > 0 && s[i - 1] == '$') ++depth;
        else if (!single && c == ')' && depth > 0) --depth;
    }
    if (escaped || single || dbl || backtick || depth > 0) return 1;
    return n > 0 && s[n - 1] == '\\';
}

static int trailing_escape_outside_single(const char *s)
{
    int single = 0, dbl = 0, escaped = 0;
    for (size_t i = 0; s[i]; ++i) {
        char c = s[i];
        if (escaped) { escaped = 0; continue; }
        if (c == '\\' && !single) { escaped = 1; continue; }
        if (c == '\'' && !dbl) single = !single;
        else if (c == '"' && !single) dbl = !dbl;
    }
    return escaped && !single;
}

static void submit_input(App *app)
{
    Tab *tab = &app->tabs[app->active];
    TabView *v = &tab->view;
    if (v->search_mode) {
        char **matches = NULL; size_t count = 0;
        char term[TERM_INPUT_CAP]; snprintf(term, sizeof(term), "%s", v->input);
        char line[TERM_INPUT_CAP + 64];
        snprintf(line, sizeof(line), "Enter search term: %s\n", term); append_text(v, line);
        if (history_find(&app->history, term, &matches, &count) == 0 && count) {
            for (size_t i = 0; i < count; ++i) { append_text(v, matches[i]); append_text(v, "\n"); }
            history_free_results(matches, count);
        } else {
            append_text(v, "No match for search term in history\n");
            history_free_results(matches, count);
        }
        v->search_mode = 0; v->input[0] = '\0'; v->length = v->cursor = 0;
        return;
    }
    if (tab->busy) return;
    if (command_incomplete(v->input)) {
        size_t append_count = 1;
        int line_join = trailing_escape_outside_single(v->input);
        if (line_join) { --v->length; v->input[v->length] = '\0'; v->cursor = v->length; append_count = 0; }
        if (v->length + append_count < sizeof(v->input)) {
            if (append_count) v->input[v->length++] = '\n';
            v->input[v->length] = '\0'; v->cursor = v->length;
            v->continuation = 1;
        }
        return;
    }
    /* Clearing is a display operation owned by the UI. Record it in the
       normal shared history, then reset only this tab's scrollback. */
    const char *command = v->input;
    while (isspace((unsigned char)*command)) ++command;
    size_t command_length = strlen(command);
    while (command_length && isspace((unsigned char)command[command_length - 1]))
        --command_length;
    if (command_length == 5 && memcmp(command, "clear", 5) == 0) {
        history_add(&app->history, v->input);
        memset(v->lines, 0, sizeof(v->lines));
        v->line_count = 0;
        v->scroll_offset = 0;
        v->input[0] = '\0';
        v->length = v->cursor = 0;
        v->continuation = 0;
        v->history_browse = 0;
        v->completion_pending = 0;
        v->completion_count = 0;
        v->completion_number[0] = '\0';
        return;
    }
    char echoed[TERM_INPUT_CAP + 64];
    snprintf(echoed, sizeof(echoed), "user@terminator> %s\n", v->input);
    append_text(v, echoed);
    history_add(&app->history, v->input);
    if (tab_send_request(tab, REQUEST_RUN, v->input, v->length) == 0) tab->busy = 1;
    v->input[0] = '\0'; v->length = v->cursor = 0; v->continuation = 0; v->history_browse = 0;
}

static void insert_bytes(TabView *v, const char *bytes, size_t count)
{
    if (count == 0 || v->length + count >= sizeof(v->input)) return;
    memmove(v->input + v->cursor + count, v->input + v->cursor, v->length - v->cursor + 1);
    memcpy(v->input + v->cursor, bytes, count);
    v->length += count; v->cursor += count;
}

static void copy_input(App *app)
{
    TabView *v = &app->tabs[app->active].view;
    snprintf(app->clipboard, sizeof(app->clipboard), "%s", v->input);
    XSetSelectionOwner(app->display, XA_PRIMARY, app->window, CurrentTime);
    XSetSelectionOwner(app->display, app->clipboard_atom, app->window, CurrentTime);
}

static void clear_completion(TabView *v)
{
    v->completion_pending = 0;
    v->completion_count = 0;
    v->completion_number[0] = '\0';
}

static void replace_completed_word(TabView *v, size_t start, size_t cursor,
                                   const char *replacement)
{
    size_t repl = strlen(replacement);
    if (start > cursor || cursor > v->length ||
        start + repl + (v->length - cursor) >= sizeof(v->input)) return;
    memmove(v->input + start + repl, v->input + cursor, v->length - cursor + 1);
    memcpy(v->input + start, replacement, repl);
    v->length = start + repl + (v->length - cursor);
    v->cursor = start + repl;
}

static void choose_completion(TabView *v, size_t choice)
{
    if (!v->completion_pending || choice >= v->completion_count) return;
    char replacement[TERM_INPUT_CAP];
    snprintf(replacement, sizeof(replacement), "%s%s", v->completion_path,
             v->completion_names[choice]);
    replace_completed_word(v, v->completion_start, v->completion_cursor, replacement);
    clear_completion(v);
}

static int name_compare(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

static void add_completion_name(char names[TERM_COMPLETION_CHOICES][TERM_NAME_CAP],
                                size_t *count, const char *name)
{
    for (size_t i = 0; i < *count; ++i)
        if (strcmp(names[i], name) == 0) return;
    if (*count >= TERM_COMPLETION_CHOICES) return;
    snprintf(names[(*count)++], TERM_NAME_CAP, "%s", name);
}

static int collect_command_completions(const char *prefix,
                                       char names[TERM_COMPLETION_CHOICES][TERM_NAME_CAP],
                                       size_t *count)
{
    static const char *builtins[] = {
        "cd", "exit", "history", "jobs", "fg", "bg", "source", "multiWatch"
    };
    size_t prefix_length = strlen(prefix);
    *count = 0;
    for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); ++i)
        if (strncmp(builtins[i], prefix, prefix_length) == 0)
            add_completion_name(names, count, builtins[i]);

    const char *path_env = getenv("PATH");
    if (!path_env) return 0;
    char *path_copy = strdup(path_env);
    if (!path_copy) return -1;
    char *save = NULL;
    for (char *directory = strtok_r(path_copy, ":", &save); directory;
         directory = strtok_r(NULL, ":", &save)) {
        if (!*directory) directory = ".";
        DIR *dir = opendir(directory);
        if (!dir) continue;
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strncmp(entry->d_name, prefix, prefix_length) != 0) continue;
            char full_path[TERM_INPUT_CAP];
            snprintf(full_path, sizeof(full_path), "%s/%s", directory, entry->d_name);
            struct stat info;
            if (stat(full_path, &info) == 0 && S_ISREG(info.st_mode) &&
                access(full_path, X_OK) == 0)
                add_completion_name(names, count, entry->d_name);
        }
        closedir(dir);
    }
    free(path_copy);
    return 0;
}

static void complete_filename(App *app)
{
    Tab *tab = &app->tabs[app->active];
    TabView *v = &tab->view;
    if (tab->busy || v->search_mode) return;
    clear_completion(v);
    size_t start = v->cursor;
    while (start > 0 && !isspace((unsigned char)v->input[start - 1]) &&
           v->input[start - 1] != '|' && v->input[start - 1] != '>' &&
           v->input[start - 1] != '<' && v->input[start - 1] != '&') --start;
    char word[TERM_INPUT_CAP];
    size_t word_len = v->cursor - start;
    memcpy(word, v->input + start, word_len); word[word_len] = '\0';
    char *slash = strrchr(word, '/');
    int command_position = 1;
    for (size_t i = 0; i < start; ++i)
        if (!isspace((unsigned char)v->input[i])) { command_position = 0; break; }
    char path_prefix[TERM_INPUT_CAP] = "";
    const char *base = word;
    char directory[TERM_INPUT_CAP];
    char names[TERM_COMPLETION_CHOICES][TERM_NAME_CAP];
    size_t count = 0;
    int command_completion = command_position && !slash && word_len > 0;
    if (command_completion) {
        if (collect_command_completions(word, names, &count) != 0) return;
        /* Keep Task 10's current-directory filename completion available at
           the first word too, including local programs not on PATH. */
        DIR *cwd = opendir(tab->cwd);
        if (cwd) {
            struct dirent *entry;
            while ((entry = readdir(cwd)) != NULL) {
                if (strncmp(entry->d_name, word, word_len) == 0)
                    add_completion_name(names, &count, entry->d_name);
            }
            closedir(cwd);
        }
    } else {
      if (slash) {
        size_t prefix_len = (size_t)(slash - word + 1);
        memcpy(path_prefix, word, prefix_len); path_prefix[prefix_len] = '\0';
        base = slash + 1;
        if (path_prefix[0] == '/') snprintf(directory, sizeof(directory), "%s", path_prefix);
        else snprintf(directory, sizeof(directory), "%s/%s", tab->cwd, path_prefix);
      } else snprintf(directory, sizeof(directory), "%s", tab->cwd);

      size_t base_len = strlen(base);
      DIR *dir = opendir(directory);
      if (!dir) return;
      struct dirent *entry;
      while ((entry = readdir(dir)) != NULL && count < TERM_COMPLETION_CHOICES) {
          if (strncmp(entry->d_name, base, base_len) == 0) {
              snprintf(names[count++], TERM_NAME_CAP, "%s", entry->d_name);
          }
      }
      closedir(dir);
    }
    size_t base_len = strlen(base);
    if (!count) return;
    qsort(names, count, TERM_NAME_CAP, name_compare);
    if (count == 1) {
        char replacement[TERM_INPUT_CAP];
        snprintf(replacement, sizeof(replacement), "%s%s", path_prefix, names[0]);
        replace_completed_word(v, start, v->cursor, replacement);
        return;
    }
    size_t common = strlen(names[0]);
    for (size_t i = 1; i < count; ++i) {
        size_t j = 0;
        while (j < common && names[i][j] == names[0][j]) ++j;
        common = j;
    }
    if (common > base_len) {
        char replacement[TERM_INPUT_CAP];
        snprintf(replacement, sizeof(replacement), "%s%.*s", path_prefix, (int)common, names[0]);
        replace_completed_word(v, start, v->cursor, replacement);
    }
    v->completion_pending = 1;
    v->completion_count = count;
    v->completion_start = start;
    v->completion_cursor = v->cursor;
    snprintf(v->completion_path, sizeof(v->completion_path), "%s", path_prefix);
    for (size_t i = 0; i < count; ++i) snprintf(v->completion_names[i], TERM_NAME_CAP, "%s", names[i]);
    append_text(v, "\nAvailable completions:\n");
    for (size_t i = 0; i < count; ++i) {
        char choice[TERM_NAME_CAP + 32];
        snprintf(choice, sizeof(choice), "  %zu) %s\n", i + 1, names[i]);
        append_text(v, choice);
    }
}

static void request_paste(App *app)
{
    XConvertSelection(app->display, app->clipboard_atom, app->utf8_atom,
                      app->paste_property, app->window, CurrentTime);
}

static void handle_selection_request(App *app, XSelectionRequestEvent *request)
{
    XSelectionEvent reply;
    memset(&reply, 0, sizeof(reply));
    reply.type = SelectionNotify;
    reply.display = request->display;
    reply.requestor = request->requestor;
    reply.selection = request->selection;
    reply.target = request->target;
    reply.time = request->time;
    reply.property = None;
    Atom property = request->property == None ? request->target : request->property;
    if (request->target == app->targets_atom) {
        Atom supported[] = { app->targets_atom, app->utf8_atom, XA_STRING };
        XChangeProperty(app->display, request->requestor, property, XA_ATOM, 32,
                        PropModeReplace, (unsigned char *)supported, 3);
        reply.property = property;
    } else if (request->target == app->utf8_atom || request->target == XA_STRING) {
        XChangeProperty(app->display, request->requestor, property, request->target, 8,
                        PropModeReplace, (unsigned char *)app->clipboard,
                        (int)strlen(app->clipboard));
        reply.property = property;
    }
    XSendEvent(app->display, request->requestor, False, 0, (XEvent *)&reply);
    XFlush(app->display);
}

static void handle_selection_notify(App *app)
{
    Atom type; int format; unsigned long count, remaining;
    unsigned char *value = NULL;
    if (XGetWindowProperty(app->display, app->window, app->paste_property, 0,
                           TERM_INPUT_CAP / 4, True, AnyPropertyType, &type,
                           &format, &count, &remaining, &value) == Success &&
        value && format == 8 && (type == app->utf8_atom || type == XA_STRING)) {
        TabView *v = &app->tabs[app->active].view;
        insert_bytes(v, (const char *)value, (size_t)count);
    }
    if (value) XFree(value);
    draw_app(app);
}

static void handle_key(App *app, XKeyEvent *event)
{
    Tab *tab = &app->tabs[app->active];
    TabView *v = &tab->view;
    KeySym key;
    char text[32];
    int count = XLookupString(event, text, sizeof(text), &key, NULL);
    int left = binding_matches(app, event, BIND_LEFT);
    int right = binding_matches(app, event, BIND_RIGHT);
    int line_start = binding_matches(app, event, BIND_LINE_START);
    int line_end = binding_matches(app, event, BIND_LINE_END);
    int history_up = binding_matches(app, event, BIND_HISTORY_UP);
    int history_down = binding_matches(app, event, BIND_HISTORY_DOWN);
    if (v->completion_pending && key == XK_Return && v->completion_count > 9) {
        long number = strtol(v->completion_number, NULL, 10);
        if (number > 0 && (size_t)number <= v->completion_count) choose_completion(v, (size_t)number - 1);
        else clear_completion(v);
        draw_app(app); return;
    }
    if (v->completion_pending && key >= XK_0 && key <= XK_9 && v->completion_count > 9) {
        size_t n = strlen(v->completion_number);
        if (n + 1 < sizeof(v->completion_number)) {
            v->completion_number[n] = (char)('0' + (key - XK_0));
            v->completion_number[n + 1] = '\0';
        }
        draw_app(app); return;
    }
    if (v->completion_pending && key >= XK_1 && key <= XK_9 && v->completion_count <= 9) {
        size_t choice = (size_t)(key - XK_1);
        if (choice < v->completion_count) choose_completion(v, choice);
        draw_app(app); return;
    }
    if (v->completion_pending && key == XK_0 && v->completion_count <= 9) {
        choose_completion(v, 9); draw_app(app); return;
    }
    if (v->completion_pending && key == XK_Escape) {
        clear_completion(v); draw_app(app); return;
    }
    if (v->completion_pending && key != XK_Tab) clear_completion(v);
    if (binding_matches(app, event, BIND_HISTORY_SEARCH) && !v->search_mode) {
        v->search_mode = 1; v->input[0] = '\0'; v->length = v->cursor = 0;
        draw_app(app); return;
    }
    if (history_up && !v->search_mode) {
        size_t count_history = history_count(&app->history);
        if (v->history_browse < count_history) {
            if (v->history_browse == 0) snprintf(v->saved_input, sizeof(v->saved_input), "%s", v->input);
            ++v->history_browse;
            if (history_latest(&app->history, v->history_browse - 1, v->input, sizeof(v->input)) == 0)
                v->length = v->cursor = strlen(v->input);
        }
        draw_app(app); return;
    }
    if (history_down && !v->search_mode) {
        if (v->history_browse) {
            --v->history_browse;
            if (v->history_browse == 0) snprintf(v->input, sizeof(v->input), "%s", v->saved_input);
            else (void)history_latest(&app->history, v->history_browse - 1, v->input, sizeof(v->input));
            v->length = v->cursor = strlen(v->input);
        }
        draw_app(app); return;
    }
    if (binding_matches(app, event, BIND_COPY)) {
        copy_input(app);
    } else if (binding_matches(app, event, BIND_PASTE) || command_paste_matches(event)) {
        request_paste(app);
    } else if ((event->state & ControlMask) && (key == XK_c || key == XK_C)) {
        if (tab->busy) (void)tab_send_request(tab, REQUEST_INTERRUPT, NULL, 0);
        else { v->length = v->cursor = 0; v->input[0] = '\0'; }
    } else if ((event->state & ControlMask) && (key == XK_z || key == XK_Z)) {
        if (tab->busy) (void)tab_send_request(tab, REQUEST_SUSPEND, NULL, 0);
    } else if (line_start) v->cursor = 0;
    else if (line_end) v->cursor = v->length;
    else if (binding_matches(app, event, BIND_DELETE_START)) {
        memmove(v->input, v->input + v->cursor, v->length - v->cursor + 1);
        v->length -= v->cursor; v->cursor = 0;
    } else if (binding_matches(app, event, BIND_DELETE_END)) {
        v->input[v->cursor] = '\0'; v->length = v->cursor;
    } else if (binding_matches(app, event, BIND_DELETE_WORD)) {
        size_t old_cursor = v->cursor;
        while (v->cursor && isspace((unsigned char)v->input[v->cursor - 1])) --v->cursor;
        while (v->cursor && !isspace((unsigned char)v->input[v->cursor - 1])) --v->cursor;
        memmove(v->input + v->cursor, v->input + old_cursor, v->length - old_cursor + 1);
        v->length -= old_cursor - v->cursor;
    } else if (binding_matches(app, event, BIND_CLEAR)) {
        memset(v->lines, 0, sizeof(v->lines));
        v->line_count = 0;
        v->scroll_offset = 0;
    } else if (binding_matches(app, event, BIND_EOF)) {
        if (!v->length && !tab->busy) close_active_tab(app);
    } else if (left && v->cursor) --v->cursor;
    else if (right && v->cursor < v->length) ++v->cursor;
    else if (key == XK_Home) v->cursor = 0;
    else if (key == XK_End) v->cursor = v->length;
    else if (key == XK_BackSpace && v->cursor) {
        memmove(v->input + v->cursor - 1, v->input + v->cursor, v->length - v->cursor + 1);
        --v->cursor; --v->length;
    } else if (key == XK_Delete && v->cursor < v->length) {
        memmove(v->input + v->cursor, v->input + v->cursor + 1, v->length - v->cursor);
        --v->length;
    } else if (key == XK_Return || key == XK_KP_Enter) submit_input(app);
    else if ((event->state & ControlMask) && (event->state & ShiftMask) && key == XK_T) (void)start_tab(app);
    else if ((event->state & ControlMask) && (event->state & ShiftMask) && key == XK_W) close_active_tab(app);
    else if (key == XK_Tab) complete_filename(app);
    else if (count > 0 && !(event->state & ControlMask)) {
        insert_bytes(v, text, (size_t)count);
        if (v->history_browse) v->history_browse = 0;
    }
    draw_app(app);
}

static void handle_button(App *app, XButtonEvent *event)
{
    if (event->button == Button4 || event->button == Button5) {
        TabView *view = &app->tabs[app->active].view;
        if (event->button == Button4) {
            if (view->scroll_offset < view->line_count) ++view->scroll_offset;
        } else if (view->scroll_offset) --view->scroll_offset;
        draw_app(app); return;
    }
    if (event->y > 30) return;
    int slot = event->x / 112;
    if (slot < app->tab_count) app->active = slot;
    else if (slot == app->tab_count) (void)start_tab(app);
    draw_app(app);
}

static void collect_output(App *app, int index)
{
    Tab *tab = &app->tabs[index];
    char buffer[8192];
    for (;;) {
        ssize_t n = read(tab->from_worker, buffer, sizeof(buffer));
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (n < 0 && errno == EINTR) continue;
        if (n == 0) { tab->output_eof = 1; break; }
        if (n < 0) break;
        for (ssize_t i = 0; i < n; ++i) {
            unsigned char c = (unsigned char)buffer[i];
            if (tab->in_marker) {
                if (c == 0x1f) {
                    tab->marker[tab->marker_length] = '\0';
                    if (tab->marker_length > 1 && tab->marker[0] == 'P')
                        snprintf(tab->cwd, sizeof(tab->cwd), "%s", tab->marker + 1);
                    else if (tab->marker_length > 1 && tab->marker[0] == 'R') {
                        snprintf(tab->cwd, sizeof(tab->cwd), "%s", tab->marker + 1);
                        if (tab->view.lines[tab->view.line_count][0] != '\0')
                            tab_append_output(&tab->view, "\n", 1);
                        tab->busy = 0;
                    }
                    tab->marker_length = 0; tab->in_marker = 0;
                } else if (tab->marker_length + 1 < sizeof(tab->marker)) {
                    tab->marker[tab->marker_length++] = (char)c;
                }
            } else if (c == 0x1f) {
                tab->in_marker = 1; tab->marker_length = 0;
            } else {
                tab_append_output(&tab->view, (const char *)&buffer[i], 1);
            }
        }
    }
}

int main(int argc, char **argv)
{
    static App app;
    memset(&app, 0, sizeof(app));
    app.process_mode = argc > 1 && strcmp(argv[1], "useProcess=True") == 0;
    app.width = 900; app.height = 600; app.running = 1;
    signal(SIGPIPE, SIG_IGN);
    load_bindings(&app);

    if (history_open(&app.history) != 0) {
        fprintf(stderr, "terminator: could not initialize shared history\n");
        return EXIT_FAILURE;
    }

    app.display = XOpenDisplay(NULL);
    if (!app.display) { fprintf(stderr, "terminator: cannot open X display\n"); history_close(&app.history); return EXIT_FAILURE; }
    int screen = DefaultScreen(app.display);
    app.window = XCreateSimpleWindow(app.display, RootWindow(app.display, screen),
        80, 80, (unsigned)app.width, (unsigned)app.height, 1,
        BlackPixel(app.display, screen), WhitePixel(app.display, screen));
    app.wm_delete = XInternAtom(app.display, "WM_DELETE_WINDOW", False);
    app.clipboard_atom = XInternAtom(app.display, "CLIPBOARD", False);
    app.utf8_atom = XInternAtom(app.display, "UTF8_STRING", False);
    app.targets_atom = XInternAtom(app.display, "TARGETS", False);
    app.paste_property = XInternAtom(app.display, "TERMINATOR_CLIPBOARD", False);
    XSetWMProtocols(app.display, app.window, &app.wm_delete, 1);
    XStoreName(app.display, app.window, "Terminator");
    XWMHints wm_hints;
    memset(&wm_hints, 0, sizeof(wm_hints));
    wm_hints.flags = InputHint;
    wm_hints.input = True;
    XSetWMHints(app.display, app.window, &wm_hints);
    XSelectInput(app.display, app.window, ExposureMask | KeyPressMask | ButtonPressMask |
                 StructureNotifyMask);
    XMapWindow(app.display, app.window);
    app.gc = XCreateGC(app.display, app.window, 0, NULL);
    app.font = XLoadQueryFont(app.display, "fixed");
    if (app.font) XSetFont(app.display, app.gc, app.font->fid);
    (void)fcntl(ConnectionNumber(app.display), F_SETFD, FD_CLOEXEC);
    if (start_tab(&app) != 0) { XCloseDisplay(app.display); history_close(&app.history); return EXIT_FAILURE; }
    if (app.config_message[0]) append_text(&app.tabs[0].view, app.config_message);

    while (app.running) {
        while (XPending(app.display)) {
            XEvent event; XNextEvent(app.display, &event);
            if (event.type == Expose && event.xexpose.count == 0) draw_app(&app);
            else if (event.type == KeyPress) handle_key(&app, &event.xkey);
            else if (event.type == ButtonPress) handle_button(&app, &event.xbutton);
            else if (event.type == MapNotify && event.xmap.window == app.window) {
                XWindowAttributes attributes;
                if (XGetWindowAttributes(app.display, app.window, &attributes) &&
                    attributes.map_state == IsViewable) {
                    XSetInputFocus(app.display, app.window, RevertToParent, CurrentTime);
                    XFlush(app.display);
                }
            }
            else if (event.type == SelectionRequest) handle_selection_request(&app, &event.xselectionrequest);
            else if (event.type == SelectionNotify) handle_selection_notify(&app);
            else if (event.type == ConfigureNotify) {
                app.width = event.xconfigure.width; app.height = event.xconfigure.height; draw_app(&app);
            } else if (event.type == ClientMessage && (Atom)event.xclient.data.l[0] == app.wm_delete)
                app.running = 0;
        }

        struct pollfd fds[TERM_MAX_TABS + 1];
        int map[TERM_MAX_TABS + 1];
        nfds_t nfds = 1;
        fds[0].fd = ConnectionNumber(app.display); fds[0].events = POLLIN; fds[0].revents = 0; map[0] = -1;
        for (int i = 0; i < app.tab_count; ++i) if (app.tabs[i].alive && !app.tabs[i].output_eof) {
            fds[nfds].fd = app.tabs[i].from_worker; fds[nfds].events = POLLIN | POLLHUP; fds[nfds].revents = 0;
            map[nfds++] = i;
        }
        int ready = poll(fds, nfds, -1);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) { perror("poll"); break; }
        int changed = 0;
        for (nfds_t i = 1; i < nfds; ++i) if (fds[i].revents) { collect_output(&app, map[i]); changed = 1; }
        for (int i = app.tab_count - 1; i >= 0; --i)
            if (app.tabs[i].output_eof) { close_tab_at(&app, i); changed = 1; }
        if (changed) draw_app(&app);
    }

    while (app.tab_count > 0) { tab_stop(&app.tabs[app.tab_count - 1]); --app.tab_count; }
    XFreeGC(app.display, app.gc);
    if (app.font) XFreeFont(app.display, app.font);
    XCloseDisplay(app.display);
    history_close(&app.history);
    return EXIT_SUCCESS;
}
