# Mini Shell (`msh`)

A small interactive Unix shell written in C, supporting command execution, pipelines, background processes, and job control.

## Project structure

```text
MinishellSkeleton/
├── minishell.c     # Prompt, input parsing, built-ins, process and pipe execution
├── minishell.h     # Shared structures, constants, globals, and prototypes
├── job_control.c   # Job list, jobs, fg, and bg implementation
├── Makefile        # Build rules
└── README.md
```

## Build and run

```sh
make
./msh
```

To remove generated files:

```sh
make clean
```

The shell should be run directly in an interactive terminal when testing Ctrl+C, Ctrl+Z, `fg`, and `bg`.

## Features

- Default prompt: `msh>`.
- Prompt customization using the exact `PS1=VALUE` form. For example, `PS1=myshell>`. An input such as `PS1 = myshell` is treated as a normal command, not an assignment.
- External command execution using `fork()`, `execvp()`, and `waitpid()`.
- Built-ins: `cd`, `pwd`, `echo`, `exit`, `jobs`, `fg`, and `bg`.
- Special values: `echo $?` (last foreground command status), `echo $$` (shell PID), and `echo $SHELL` (this minishell executable path).
- Foreground process groups. Ctrl+C interrupts a foreground command; Ctrl+Z stops it and records the job. Ctrl+C/Ctrl+Z at an idle prompt leave the shell running and redisplay the prompt.
- Background execution with a trailing `&`, such as `sleep 10 &`.
- `jobs` lists tracked background and stopped jobs. `fg` resumes the most recently added job, or a specified job ID/PID; `bg` resumes a stopped job in the background.
- SIGCHLD-driven cleanup using `waitpid()` in the main execution flow. Completed background jobs report their exit status.
- Pipelines with multiple commands, such as `ls -l /dev | grep tty | wc -l`.
- Empty input lines are ignored. Overlong input lines are rejected.

## Example session

```text
msh> pwd
msh> PS1=work>
work> echo $$
work> echo $SHELL
work> false
work> echo $?
1
work> sleep 30 &
work> jobs
work> fg
# Ctrl+Z stops the foreground job; Ctrl+C interrupts a foreground job.
work> bg
work> ls -l /dev | grep tty | wc -l
work> exit
```

## Implementation notes

- Each pipeline command is forked into the same process group. The parent closes unused pipe descriptors and waits for the foreground process group.
- The shell transfers terminal foreground ownership to a foreground job with `tcsetpgrp()` and restores ownership to itself when the job exits or stops.
- Signal handlers perform only minimal signal-safe work. Child reaping and job-list updates happen in normal program flow.
- For a pipeline, the reported foreground exit status is taken from the final command in the pipeline.
- Background jobs are tracked as process groups, with the child PIDs retained so each pipeline member can be reaped.

## Scope and limitations

This is a teaching minishell, not a full Bash replacement. It does not implement quoting/escaping, wildcard expansion, command substitution, I/O redirection (`<`, `>`, `>>`), shell scripts, or terminal-mode preservation for programs that modify terminal settings. Commands are tokenized on whitespace; `|` and `&` are recognized even when adjacent to command words. The maximum input line, argument count, job count, and pipeline length are bounded by constants in `minishell.h`.
