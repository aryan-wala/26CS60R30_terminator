# Learning notes and guided walkthrough

Use the questions below to check that you can explain the implementation. The goal is to be able to trace a command and justify what each process/thread owns.

## 1. Start at the GUI (`src/main.c`)

1. `main()` opens the X display, creates the window, opens history, and starts the first tab. Where does `useProcess=True` change the startup path?
2. Find the `poll()` loop. Which descriptors wake it, and why does that let output arrive without freezing keyboard handling?
3. Follow Return from `handle_key()` into `submit_input()`. What request is sent, and how does the GUI associate returned output with a tab?
4. How do `TabView` and `Tab` differ? Which one contains display/input state, and which one contains worker communication state?

## 2. Trace a command (`src/worker.c`, `src/shell.c`)

For `printf 'hello\\n' | wc -c > count.txt`, trace the data in this order:

1. GUI encodes a request header plus command bytes into the tab's request pipe.
2. Worker reads the complete request and calls the shell command handler.
3. Tokenization preserves quoted spaces and recognizes `|` and `>`.
4. The shell creates a pipeline pipe and forks each stage.
5. `dup2()` connects the first command's stdout to the next command's stdin; output redirection connects the last stdout to `count.txt`.
6. Children call `execvp()` and the worker waits for the foreground pipeline.

Questions: why must unused pipe ends be closed? Which process sends the completion marker that updates the prompt/cwd in the GUI?

## 3. Understand the two modes

- In thread mode, tab workers share the GUI process, so their descriptors share a process table. Why must the GUI keep worker-owned pipe ends open in this mode?
- In process mode, `fork()` duplicates the descriptor table. Which ends should parent and child close after startup?
- Why does the shell store `cwd` as text and defer `chdir()` until it forks an external command?
- Xlib calls stay in the GUI thread/process. What could go wrong if a worker thread also drew into the X window?

Draw this for yourself:

```text
GUI -- request pipe --> tab worker -- fork/exec --> command
GUI <-- output pipe -- tab worker <-- stdout/stderr -- command
```

In process mode, “tab worker” is a separate process; in thread mode it is a pthread.

## 4. State and optional features

- `HistoryStore` is in shared memory; the named semaphore protects it. Why is a normal process-local mutex insufficient for process mode?
- How is history persisted across application runs, and what do Up/Down and Ctrl+R do in the GUI?
- Why can filename completion use the tab's stored `cwd` without changing the GUI process directory?
- Follow Ctrl+C from the key event to the foreground process group. How does this differ from terminating the GUI?
- What does `multiWatch` supervise, and how does `poll()` let it collect output from multiple commands?

## 5. Scope to explain during submission

`source FILE` executes supported command lines from a file. It is not Task 11's custom script interpreter. This shell has explicit size limits, supports a subset of shell grammar, and does not provide full interactive TTY behavior or full ANSI terminal emulation. Be prepared to state these limits accurately.

## Suggested 10-minute code walkthrough

1. Open `src/terminator.h` and identify `Tab`, `TabView`, and `ShellContext`.
2. Show `main()` and the event loop in `src/main.c`.
3. Follow one input command through `submit_input()`, `tab_send_request()`, `shell_worker_loop()`, and `shell_run_request()`.
4. Explain `run_pipeline()` using `pipe()`, `fork()`, `dup2()`, `execvp()`, and `waitpid()`.
5. Compare `tab_start()` in `src/worker.c` for pthread and process modes.
6. Show the shared history lock and persistence in `src/history.c`.
