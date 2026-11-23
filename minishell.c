#define _POSIX_C_SOURCE 200809L
#include "minishell.h"

Job *job_list = NULL;
int next_job_id = 1;
volatile sig_atomic_t child_changed = 0;
pid_t shell_pgid = -1;
int shell_terminal = STDIN_FILENO;
int shell_interactive = 0;
int last_status = 0;
char shell_path[PATH_MAX] = "./msh";
char shell_prompt[128] = "msh> ";
static char active_command[MAX_INPUT_SIZE];

/* read(2) is used instead of stdio fgets() so SIGCHLD can interrupt an
 * idle prompt; the main loop can then reap completed background jobs. */
static ssize_t read_command_line(char *buffer, size_t capacity)
{
    size_t used = 0;
    while (1) {
        char ch;
        ssize_t n = read(STDIN_FILENO, &ch, 1);
        if (n == 1) {
            if (ch == '\n') {
                buffer[used] = '\0';
                return (ssize_t)used;
            }
            if (used + 1 < capacity)
                buffer[used++] = ch;
            else {
                /* Drain an overlong line so its tail is not parsed as a command. */
                do {
                    n = read(STDIN_FILENO, &ch, 1);
                } while (n == 1 && ch != '\n');
                buffer[0] = '\0';
                return -4;
            }
        } else if (n == 0) {
            if (used == 0)
                return -3;
            break;
        } else if (errno == EINTR) {
            if (used == 0)
                return -1;
            continue;
        } else {
            return -2;
        }
    }
    buffer[used] = '\0';
    return (ssize_t)used;
}

static int is_prompt_assignment(const char *s)
{
    /* Only the exact PS1= form is a shell prompt assignment. */
    return strncmp(s, "PS1=", 4) == 0;
}

static void update_prompt(const char *input)
{
    const char *value = input + 4;
    if (*value == '\0')
        value = "msh> ";
    snprintf(shell_prompt, sizeof(shell_prompt), "%s", value);
    setenv("PS1", value, 1);
}

int main(void)
{
    init_shell();
    main_loop();
    return 0;
}

void init_shell(void)
{
    ssize_t n;

    n = readlink("/proc/self/exe", shell_path, sizeof(shell_path) - 1);
    if (n >= 0)
        shell_path[n] = '\0';

    shell_interactive = isatty(shell_terminal);
    if (shell_interactive) {
        /* Wait until this shell is running in the foreground. */
        while (tcgetpgrp(shell_terminal) != (shell_pgid = getpgrp()))
            kill(-shell_pgid, SIGTTIN);

        shell_pgid = getpid();
        if (setpgid(shell_pgid, shell_pgid) < 0 && errno != EACCES && errno != EPERM)
            perror("setpgid");
        tcsetpgrp(shell_terminal, shell_pgid);
    } else {
        shell_pgid = getpid();
    }

    init_signal_handlers();

    /* Start with the assignment's required prompt, independent of the
       parent shell's PS1. The user can change it with PS1=NEW_PROMPT. */
    setenv("PS1", shell_prompt, 1);
}

void main_loop(void)
{
    char input[MAX_INPUT_SIZE];
    char *args[MAX_ARGS];

    while (1) {
        reap_background_jobs();
        printf("%s", shell_prompt);
        fflush(stdout);

        ssize_t input_length = read_command_line(input, sizeof(input));
        if (input_length == -3) {
            putchar('\n');
            break;
        }
        if (input_length == 0)
            continue;
        if (input_length == -1) {
            /* Usually SIGCHLD: reap jobs, then print a fresh prompt. */
            reap_background_jobs();
            continue;
        }
        if (input_length == -2) {
            perror("read");
            continue;
        }
        if (input_length == -4) {
            fprintf(stderr, "msh: command line too long (maximum %d characters)\n", MAX_INPUT_SIZE - 1);
            continue;
        }
        if (input[0] == '\0')
            continue;

        /* PS1=VALUE is handled by the shell. "PS1 = VALUE" is not. */
        if (is_prompt_assignment(input)) {
            update_prompt(input);
            continue;
        }

        parse_command(input, args);
        if (args[0] != NULL)
            last_status = execute_command(args);
    }
}

/* Tokenizer supports whitespace-separated words, | and a trailing &.
 * Quoting, globbing, variable expansion other than echo's special variables,
 * and command substitution are intentionally outside this minimal project. */
void parse_command(char *input, char **args)
{
    int count = 0;
    char *p = input;

    while (*p && count < MAX_ARGS - 1) {
        while (isspace((unsigned char)*p))
            ++p;
        if (*p == '\0')
            break;

        if (*p == '|') {
            args[count++] = "|";
            ++p;
            continue;
        }
        if (*p == '&') {
            args[count++] = "&";
            ++p;
            continue;
        }

        char *start = p;
        while (*p && !isspace((unsigned char)*p) && *p != '|' && *p != '&')
            ++p;

        if (*p) {
            char delimiter = *p;
            *p = '\0';
            args[count++] = start;
            ++p;
            if (delimiter == '|' || delimiter == '&') {
                args[count++] = (delimiter == '|') ? "|" : "&";
                if (count >= MAX_ARGS - 1)
                    break;
            }
        } else {
            args[count++] = start;
        }
    }
    args[count] = NULL;
}

static void builtin_echo(char **args)
{
    int i = 1;
    int newline = 1;

    if (args[i] && strcmp(args[i], "-n") == 0) {
        newline = 0;
        ++i;
    }

    for (; args[i]; ++i) {
        const char *value = args[i];
        if (strcmp(value, "$?") == 0) {
            printf("%d", last_status);
        } else if (strcmp(value, "$$") == 0) {
            printf("%ld", (long)shell_pgid);
        } else if (strcmp(value, "$SHELL") == 0) {
            printf("%s", shell_path);
        } else {
            /* Basic support for a few common environment variables. */
            if (value[0] == '$' && value[1] != '\0') {
                const char *env = getenv(value + 1);
                if (env)
                    fputs(env, stdout);
            } else {
                fputs(value, stdout);
            }
        }
        if (args[i + 1])
            putchar(' ');
    }
    if (newline)
        putchar('\n');
}

int is_builtin_command(char **args)
{
    if (!args || !args[0])
        return 0;

    return strcmp(args[0], "cd") == 0 ||
           strcmp(args[0], "pwd") == 0 ||
           strcmp(args[0], "echo") == 0 ||
           strcmp(args[0], "exit") == 0 ||
           strcmp(args[0], "jobs") == 0 ||
           strcmp(args[0], "fg") == 0 ||
           strcmp(args[0], "bg") == 0;
}

void handle_builtin_command(char **args)
{
    if (strcmp(args[0], "cd") == 0) {
        const char *target = args[1] ? args[1] : getenv("HOME");
        if (!target || chdir(target) < 0) {
            perror("cd");
            last_status = 1;
        } else {
            last_status = 0;
        }
    } else if (strcmp(args[0], "pwd") == 0) {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd))) {
            puts(cwd);
            last_status = 0;
        } else {
            perror("pwd");
            last_status = 1;
        }
    } else if (strcmp(args[0], "echo") == 0) {
        builtin_echo(args);
        last_status = 0;
    } else if (strcmp(args[0], "exit") == 0) {
        /* Restore terminal ownership before leaving an interactive shell. */
        if (shell_interactive)
            tcsetpgrp(shell_terminal, shell_pgid);
        exit(last_status);
    } else if (strcmp(args[0], "jobs") == 0) {
        reap_background_jobs();
        list_jobs();
        last_status = 0;
    } else if (strcmp(args[0], "fg") == 0) {
        int id = args[1] ? atoi(args[1]) : -1;
        last_status = bring_job_to_foreground(id);
    } else if (strcmp(args[0], "bg") == 0) {
        int id = args[1] ? atoi(args[1]) : -1;
        last_status = send_job_to_background(id);
    }
}

int execute_command(char **args)
{
    if (!args || !args[0])
        return 0;

    /* A standalone built-in must run in the shell process (not a child). */
    int has_control_token = 0;
    for (int i = 1; args[i]; ++i) {
        if (strcmp(args[i], "|") == 0 || strcmp(args[i], "&") == 0) {
            has_control_token = 1;
            break;
        }
    }
    if (is_builtin_command(args) && !has_control_token) {
        handle_builtin_command(args);
        return last_status;
    }

    handle_redirection_and_piping(args);
    return last_status;
}

static void child_reset_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTSTP, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
    sigaction(SIGCHLD, &sa, NULL);
    sigaction(SIGTTIN, &sa, NULL);
    sigaction(SIGTTOU, &sa, NULL);
}

static void execute_child_builtin(char **args)
{
    if (strcmp(args[0], "echo") == 0) {
        builtin_echo(args);
        fflush(stdout);
        _exit(0);
    }
    if (strcmp(args[0], "pwd") == 0) {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd))) {
            puts(cwd);
            fflush(stdout);
            _exit(0);
        }
        _exit(1);
    }
    if (strcmp(args[0], "cd") == 0) {
        const char *target = args[1] ? args[1] : getenv("HOME");
        _exit(target && chdir(target) == 0 ? 0 : 1);
    }
    _exit(127);
}

void handle_redirection_and_piping(char **args)
{
    char *commands[MAX_PIPE_CMDS][MAX_ARGS];
    int command_count = 0, arg_count = 0;
    int background = 0;
    char command_line[MAX_INPUT_SIZE] = {0};

    /* Save a readable copy for jobs output. */
    for (int i = 0; args[i]; ++i) {
        if (i && strlen(command_line) + 2 < sizeof(command_line))
            strcat(command_line, " ");
        if (strlen(command_line) + strlen(args[i]) + 1 < sizeof(command_line))
            strcat(command_line, args[i]);
    }

    int total = 0;
    while (args[total])
        ++total;
    if (total > 0 && strcmp(args[total - 1], "&") == 0) {
        background = 1;
        args[--total] = NULL;
    }

    command_count = 1;
    for (int i = 0; i < total; ++i) {
        if (strcmp(args[i], "|") == 0) {
            if (arg_count == 0 || command_count >= MAX_PIPE_CMDS) {
                fprintf(stderr, "msh: invalid or too many pipeline commands\n");
                last_status = 2;
                return;
            }
            commands[command_count - 1][arg_count] = NULL;
            ++command_count;
            arg_count = 0;
        } else {
            if (arg_count >= MAX_ARGS - 1) {
                fprintf(stderr, "msh: too many arguments\n");
                last_status = 2;
                return;
            }
            commands[command_count - 1][arg_count++] = args[i];
        }
    }
    if (arg_count == 0) {
        fprintf(stderr, "msh: empty command in pipeline\n");
        last_status = 2;
        return;
    }
    commands[command_count - 1][arg_count] = NULL;

    int pipes[MAX_PIPE_CMDS - 1][2];
    for (int i = 0; i < command_count - 1; ++i) {
        if (pipe(pipes[i]) < 0) {
            perror("pipe");
            for (int j = 0; j < i; ++j) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            last_status = 1;
            return;
        }
    }

    pid_t pgid = 0;
    int launched = 0;
    pid_t child_pids[MAX_PIPE_CMDS] = {0};
    for (int i = 0; i < command_count; ++i) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            break;
        }
        if (pid == 0) {
            if (pgid == 0)
                setpgid(0, 0);
            else
                setpgid(0, pgid);
            child_reset_signals();

            if (i > 0)
                dup2(pipes[i - 1][0], STDIN_FILENO);
            if (i < command_count - 1)
                dup2(pipes[i][1], STDOUT_FILENO);

            for (int j = 0; j < command_count - 1; ++j) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            if (is_builtin_command(commands[i]))
                execute_child_builtin(commands[i]);

            execvp(commands[i][0], commands[i]);
            fprintf(stderr, "msh: %s: %s\n", commands[i][0], strerror(errno));
            _exit(errno == ENOENT ? 127 : 126);
        }

        if (pgid == 0)
            pgid = pid;
        setpgid(pid, pgid);
        child_pids[launched] = pid;
        ++launched;
    }

    for (int i = 0; i < command_count - 1; ++i) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }

    if (launched == 0) {
        last_status = 1;
        return;
    }

    snprintf(active_command, sizeof(active_command), "%s", command_line);

    if (background) {
        add_job(pgid, command_line);
        Job *job = find_job((int)pgid);
        if (job) {
            job->live_count = launched;
            job->child_count = launched;
            job->last_pid = child_pids[launched - 1];
            memcpy(job->child_pids, child_pids, (size_t)launched * sizeof(pid_t));
        }
        printf("[%d] %ld\n", job ? job->job_id : -1, (long)pgid);
        last_status = 0;
    } else {
        wait_for_foreground(pgid, 0, launched, child_pids, child_pids[launched - 1]);
    }
}

void wait_for_foreground(pid_t pgid, int existing_job_id, int process_count, pid_t *child_pids, pid_t last_pid)
{
    int status;
    int remaining = 0;
    int stopped_count = 0;
    int last_command_status = last_status;
    for (int i = 0; child_pids && i < process_count; ++i) {
        if (child_pids[i] > 0)
            ++remaining;
    }

    if (shell_interactive)
        tcsetpgrp(shell_terminal, pgid);

    /* For pipelines, wait until every live member has either exited or stopped.
       This prevents a partially-stopped pipeline from being mistaken as a job. */
    while (remaining > 0 && stopped_count < remaining) {
        pid_t w = waitpid(-pgid, &status, WUNTRACED);
        if (w > 0) {
            if (WIFSTOPPED(status)) {
                ++stopped_count;
                if (w == last_pid)
                    last_command_status = 128 + WSTOPSIG(status);
            } else if (WIFEXITED(status) || WIFSIGNALED(status)) {
                if (w == last_pid) {
                    last_command_status = WIFEXITED(status)
                        ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
                }
                --remaining;
                if (child_pids) {
                    for (int k = 0; k < process_count; ++k) {
                        if (child_pids[k] == w) {
                            child_pids[k] = 0;
                            break;
                        }
                    }
                }
            }
        } else if (w < 0 && errno == EINTR) {
            continue;
        } else if (w < 0 && errno == ECHILD) {
            remaining = 0;
            break;
        } else if (w < 0) {
            perror("waitpid");
            break;
        }
    }

    if (shell_interactive)
        tcsetpgrp(shell_terminal, shell_pgid);

    last_status = last_command_status;

    if (stopped_count > 0) {
        Job *job = find_job((int)pgid);
        if (!job) {
            add_job(pgid, active_command[0] ? active_command : "(stopped foreground job)");
            job = find_job((int)pgid);
        }
        if (job) {
            job->state = JOB_STOPPED;
            job->live_count = remaining;
            job->stopped_count = stopped_count;
            if (child_pids) {
                job->child_count = process_count;
                memcpy(job->child_pids, child_pids, (size_t)process_count * sizeof(pid_t));
                job->last_pid = last_pid;
            }
            printf("\n[%d] Stopped PID %ld\t%s\n", job->job_id,
                   (long)job->pgid, job->command);
            fflush(stdout);
        }
    } else if (existing_job_id > 0) {
        Job *job = find_job(existing_job_id);
        if (job)
            remove_job(job->pgid);
    }
}

void signal_handler(int sig)
{
    if (sig == SIGCHLD) {
        child_changed = 1;
    } else if (sig == SIGINT || sig == SIGTSTP) {
        /* At the prompt, Ctrl+C/Ctrl+Z should refresh msh, not terminate it. */
        (void)write(STDOUT_FILENO, "\n", 1);
    }
}

void init_signal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGCHLD, &sa, NULL);

    /* The shell catches Ctrl+C/Ctrl+Z only while it owns the terminal.
       Foreground children receive these signals in their own process group;
       child_reset_signals() restores the default dispositions after fork(). */
    struct sigaction prompt_sa;
    memset(&prompt_sa, 0, sizeof(prompt_sa));
    prompt_sa.sa_handler = signal_handler;
    sigemptyset(&prompt_sa.sa_mask);
    prompt_sa.sa_flags = 0;
    sigaction(SIGINT, &prompt_sa, NULL);
    sigaction(SIGTSTP, &prompt_sa, NULL);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
}

void reap_background_jobs(void)
{
    int status;
    pid_t pid;
    child_changed = 0;

    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED)) > 0) {
        Job *job = NULL;
        for (Job *cur = job_list; cur; cur = cur->next) {
            for (int k = 0; k < cur->child_count; ++k) {
                if (cur->child_pids[k] == pid) {
                    job = cur;
                    break;
                }
            }
            if (job)
                break;
        }
        if (!job)
            continue;

        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            for (int k = 0; k < job->child_count; ++k) {
                if (job->child_pids[k] == pid) {
                    job->child_pids[k] = 0;
                    break;
                }
            }
            if (job->live_count > 0)
                --job->live_count;
            if (job->live_count == 0) {
                int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
                printf("\n[%d] Done (status %d)\t%s\n", job->job_id, code, job->command);
                remove_job(job->pgid);
            }
        } else if (WIFSTOPPED(status)) {
            job->state = JOB_STOPPED;
            ++job->stopped_count;
        } else if (WIFCONTINUED(status)) {
            job->state = JOB_RUNNING;
            job->stopped_count = 0;
        }
    }
}
