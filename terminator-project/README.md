# Terminator

Terminator is a small C shell with an X11 window and multiple tabs, prepared for the Computing Lab project. The GUI is intended to run with X11; on this Mac it uses XQuartz.

## Implemented scope

- Required Tasks 1–8: X11 terminal window and tabs, shell commands and built-ins, command parsing and expansion, pipelines/redirection, editor controls and configurable bindings, `multiWatch`, job control/signals, and both worker modes.
- Both bonus targets selected: Task 9 command history and Task 10 filename completion.
- Task 11 (custom Bash script language) is not implemented. The mandatory `source FILE` built-in runs ordinary supported command lines from a file; it does not interpret variables, loops, conditionals, or positional script arguments.

## Build and run on macOS with XQuartz

Install XQuartz and command line build tools (including `make` and a C compiler). Start XQuartz, open its X server, and make sure the shell has a valid `DISPLAY` value. Then, from this directory:

```sh
make clean
make
./terminator
```

The default mode uses a pthread worker per tab. Run the second required mode with:

```sh
./terminator useProcess=True
```

On Debian/Ubuntu with an X11 desktop, install `gcc make libx11-dev`, then use the same `make` and launch commands. `make clean` removes the generated executable.

## Interaction

- Enter a command with Return. Use Ctrl+Shift+T to open a tab and Ctrl+Shift+W to close the active tab.
- Tab completes the command name at the start of a command, or a filename in an argument/path. Multiple matches appear as a vertical numbered list; press a listed digit (or enter a number for more than nine matches), then Return. Escape dismisses the list.
- Up/Down browse history; Ctrl+R searches saved history. Ctrl+L clears the visible scrollback. Ctrl+D closes the active tab when the input line is empty; foreground children currently receive noninteractive standard input, which is already at EOF.
- Ctrl+C interrupts and Ctrl+Z suspends the active tab's foreground job. `jobs`, `fg [job]`, and `bg [job]` manage jobs.
- Ctrl+Shift+V pastes from the X11 clipboard; on this Mac, Command+Shift+V is also supported. The first column of `~/.terminatorrc` can override supported key bindings (use `Cmd+Shift+V` for the macOS Command key). The default is used for a missing, invalid, or conflicting binding. History is persisted in `~/.terminator_history`.

Built-ins include `cd`, `exit`, `history`, `jobs`, `fg`, `bg`, `source`, and `multiWatch`. Examples of supported shell syntax include quotes, `$NAME`, `$?`, `~`, `$(command)`, backticks, semicolon-separated commands, `|`, `<`, `>`, `>>`, wildcard expansion, and `&` for background jobs.

`multiWatch ["command one", "command two"]` reruns each command every two seconds and labels output with a timestamp. Ctrl+C stops the watch.

## Known limits

This is a course shell, not a full POSIX shell or terminal emulator. It has fixed input/output, argument, tab, pipeline, job, and completion limits. It does not implement `&&`/`||`, full interactive terminal/TTY behavior for child programs, full ANSI terminal rendering, or Task 11's script language. The GUI currently edits the command line and draws text; it is not a general-purpose terminal emulator.

## Project notes

- [Architecture](docs/ARCHITECTURE.md): how the GUI, workers, pipes, and shared state fit together.
- [Learning notes](LEARNING_NOTES.md): a guided explanation and walkthrough prompts for understanding the implementation.
- [Project plan](PROJECT_PLAN.md): original course schedule and scope decision.
