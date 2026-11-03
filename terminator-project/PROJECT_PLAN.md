# Terminator project plan

## Current status — Oct 30, 2026

The implementation was assembled in one accelerated pass because the user moved the completion target to today. It now includes the required Tasks 1–8 and both tab worker modes, plus the selected History and Autocomplete bonus features. Task 11's Bash script interpreter remains out of scope. The project builds on this Mac against XQuartz; both worker modes have been launched and their GUI/worker processes observed. Command-by-command interaction has not yet been manually checked in the XQuartz window, so the user should run the short walkthrough in the README before submission. The older five-day schedule below is the original study plan and no longer describes the remaining implementation work.

## Scope decision: required work, bonus work, and out of scope

The clarified rubric takes precedence over the original brief's any-10-of-11 wording:

- **Required:** Tasks 1-8, plus both execution modes (tabs as threads and tabs as processes).
- **Bonus target:** Task 9 (History) and Task 10 (Autocomplete).
- **Out of scope:** Task 11 (Bash Scripts/custom interpreter). Do not spend implementation time on variables, positional script arguments, conditionals, or script syntax validation.

Task 2 still requires a `source` built-in. Implement that required built-in as simple command-file input using the supported command parser; it does not imply implementing the optional Bash script language from Task 11. Document the exact supported `source` behavior and limitations.

The dual-mode requirement is architectural, not a final add-on. Start with an interface that keeps X11 calls in the GUI thread/process and allows tab workers to communicate with it; use the same command behavior in either mode. Develop and integrate both modes throughout the schedule. History and Autocomplete come after the mandatory features are working.

## How we'll work so you learn it

For each feature, use the same loop:

1. **Understand:** explain the system calls and draw the process/data flow before coding.
2. **Implement together:** make one small change at a time; you write or predict the key lines before I fill gaps.
3. **Trace:** follow one example command through input, parsing, fork/exec, pipes and output.
4. **Verify:** compile and try the feature on Linux, then explain what failed and why.
5. **Record:** add the final behavior, design choice and limitation to the docs.

Keep a short `LEARNING_NOTES.md` with concepts you can explain in your own words. For each coding session, ask for a walkthrough before moving on if any part feels opaque.

## Five-day schedule

### Progress — Thu Oct 29

- [x] Created the new project folder, starter X11 window and Makefile.
- [x] Wrote the first architecture notes for the GUI/worker pipe contract and both launch modes.
- [x] Identified the thread-mode `cd` constraint: keep a per-tab cwd because process `chdir()` is shared across threads.
- [x] Build the starter with `make clean && make` on macOS using the XQuartz paths now added to the Makefile.
- [ ] Launch the starter window under XQuartz or Linux and do the first visual/input walkthrough.
- [ ] Walk through the starter source and sketch the tab/worker structures before implementing them.

### Thu Oct 29 — project setup and dual-mode architecture

- Confirm Linux/X11 build environment and dependencies (`gcc`, `make`, `libx11-dev`). The current workspace host is macOS, while the brief requires Linux; use a Linux lab machine or Linux VM for the real build/run checks.
- Inspect and discuss the existing X11 prototypes, especially window events, drawing, tabs and `poll()`.
- Agree on data structures, tab ownership, command lifecycle, GUI-to-tab messages, shared history synchronization, and how the GUI remains the sole X11 caller.
- Sketch both variants before coding: one thread per tab in thread mode; one forked worker per tab in process mode. Identify which state is private to a tab and which state is shared.
- Create the build skeleton and a minimal X11 window/event loop.
- **Checkpoint:** architecture notes show how both modes will work; `make` builds; window opens; key input appears; no console I/O is needed for normal interaction.

### Fri Oct 30 — Tasks 1-2 and both-mode prototype

- Finish tab creation, switching, closing, per-tab buffers and a responsive event loop.
- Implement prompt/input, built-ins (`cd`, `exit`, `history`, `jobs`, `fg`, `bg`, `source`, `multiWatch` entry point), external `fork()`/`execvp()`, output capture and `$?`.
- Keep `cd` and other shell-state changes in the relevant tab's shell context.
- Bring up a minimal tab worker in each mode and confirm the GUI remains responsive while it runs.
- **Checkpoint:** multiple tabs remain usable; external commands and `cd` work in the right tab; unknown commands leave the tab alive; both launch modes start.

### Sat Oct 31 — Tasks 3-5, parser and process plumbing in both modes

- Build a tokenizer/parser that tracks quotes and continuation state; add variable expansion and nested `$(...)`/backtick substitution in small steps.
- Implement `<` and `>` redirection, then arbitrary-length pipelines and combinations.
- Draw the fork/pipe/dup2/exec/wait flow and check every file descriptor is closed in the right process.
- Exercise quoting, a multiline command, input/output redirection and a 3-stage pipeline in each mode.
- **Checkpoint:** required command behavior is consistent between modes for the features completed so far.

### Sun Nov 1 — Tasks 6-8, then bonus History

- Add cursor editing, history navigation, Ctrl+L/Ctrl+D, configurable persistent bindings, and X11 selection copy/paste.
- Add foreground/background job tracking and Ctrl+C/Ctrl+Z delivery scoped to the active tab's job.
- Implement required Task 6 `multiWatch` with one pipe per command, `poll()`/`select()`, 2-second reruns, timestamped output and Ctrl+C cleanup.
- Finish built-ins and job control in both modes. Keep Ctrl+C/Ctrl+Z scoped to the active tab's jobs.
- If all mandatory features work in both modes, begin bonus Task 9: shared persistent history, last 10,000 commands, display last 1,000, and Ctrl+R search.
- **Checkpoint:** Tasks 1-8 run in both modes; interactive editing works and signals affect the intended job only. History begins only after this checkpoint.

### Mon Nov 2 — bonus Autocomplete, dual-mode integration and submission freeze

- Add bonus Task 10: filename completion, longest common prefix and numbered ambiguous-choice selection.
- Run a mandatory checklist on Linux in both modes; fix integration bugs before polishing bonus features.
- Write README/design document, capture a short demo transcript, inspect repository state and freeze by end of day.
- **Checkpoint:** clean clone builds from README instructions; Tasks 1-8 and both modes are demonstrated; bonus History and Autocomplete are described honestly as complete or incomplete; Task 11 is clearly out of scope.

### Tue Nov 3 — submission buffer only

- No planned feature work. Use the day only for a final clean build, repository link and Moodle submission before 11:59 PM.

## Daily learning checkpoints

At the end of each day, be able to explain without reading the code:

- Oct 29: X11 event loop, event-driven I/O, per-tab state, and thread/process architecture.
- Oct 30: parent/child, `fork`, `execvp`, `wait`, built-ins vs external commands, and GUI/worker communication.
- Oct 31: tokenization, quoting, `pipe`, `dup2`, redirection and descriptor lifetime in both modes.
- Nov 1: process groups/signals, job table, thread synchronization and process-shared synchronization.
- Nov 2: dual-mode consistency, bonus history/completion, integration and project limitations.

## Risk and fallback order

The schedule is exceptionally compressed. Preserve a working, documented build at every checkpoint. Mandatory acceptance is Tasks 1-8 plus both modes; do not trade any of these for bonus features. If time is short, drop or narrow History first, then Autocomplete, and leave Task 11 out. Do not claim an incomplete feature as complete. If either mode is incomplete, prioritize mode parity over bonus work.

## Submission checklist

- [ ] Tasks 1-8 work in thread mode and process mode.
- [ ] Task 9 History and Task 10 Autocomplete are clearly marked complete, partial, or not implemented for bonus evaluation.
- [ ] Task 11 Bash Scripts/custom interpreter is listed as out of scope.
- [ ] `make` builds `./terminator` on Linux with X11 flags.
- [ ] README covers dependencies, both launch modes, implemented features and limitations.
- [ ] Design document covers GUI/event loop, both modes and synchronization, jobs/signals, IPC/redirection, history and completion.
- [ ] Final clean build and demo run completed by November 2.
- [ ] GitHub repository link submitted through Moodle by November 3, 11:59 PM.
