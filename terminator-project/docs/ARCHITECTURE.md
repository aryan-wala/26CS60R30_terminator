# Architecture

## Components

`src/main.c` owns X11, the event loop, tab UI, command-line editor, history navigation, and filename completion. It alone calls Xlib. `src/worker.c` creates and shuts down tab workers and transports requests/output. `src/shell.c` owns each tab's shell context and parses/executes commands. `src/history.c` implements the shared, persistent history store. `src/terminator.h` defines the shared data structures and interfaces.

## GUI and workers

The GUI uses `poll()` on the X connection and worker output descriptors so it can react to keyboard events and output without spinning. Each tab has a request pipe toward its worker and an output pipe back. Workers never draw or access the X display; the GUI drains output and appends it to that tab's scrollback.

```text
XQuartz / X server <--> GUI event loop <--> tab request pipe --> worker
                                  ^                         <-- output pipe
                                  |                              |
                                  +------ redraw tab buffer <----+
```

The launch switch selects the worker implementation while preserving the same shell and pipe protocol:

- `./terminator`: a pthread runs each tab worker.
- `./terminator useProcess=True`: a forked child process runs each tab worker.

Commands that are not built-ins run in further forked children and use `execvp()`. Pipelines connect those children with pipes and `dup2()`. The worker waits for foreground commands and records background process groups in that tab's job table.

## Tab-local shell state

Each `ShellContext` stores its own current directory, last exit status, job table, and foreground process group. In thread mode, calling `chdir()` would change the directory for the whole application, so `cd` resolves and stores a path in the tab context. External command children call `chdir()` to that stored path just before execution. This keeps `cd` independent between tabs in either mode.

Ctrl+C and Ctrl+Z are sent as worker control requests. The worker forwards the corresponding signal to the active foreground process group. `multiWatch` supervises one child command per list item, drains their pipes with `poll()`, reruns them every two seconds, and stops them when interrupted.

## History and completion

History uses an anonymous shared `mmap` region so both thread and forked workers can access the same fixed-capacity store. A named POSIX semaphore protects concurrent access. Commands are also persisted in `~/.terminator_history`; the UI shows recent commands and Ctrl+R performs the project-defined history search. The ring stores up to 10,000 entries and the `history` command prints up to the latest 1,000.

Filename completion is performed by the GUI against the active tab's current directory. A unique prefix is inserted directly; a shared prefix is inserted when possible, otherwise the GUI displays numbered matches for selection.

## Project boundaries

`source FILE` reads lines and runs them through the ordinary command path; it is not a custom scripting language. The implementation supports a practical subset of shell syntax (quotes, environment/status expansion, command substitution, pipelines, basic redirection, semicolon lists, wildcards, and background jobs), with fixed limits declared in `terminator.h`. It does not claim full Bash/POSIX syntax, complete TTY job control, or full terminal emulation. The optional Task 11 interpreter is out of scope.
