#ifndef MINISHELL_H
#define MINISHELL_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <limits.h>
#include <ctype.h>
#include <termios.h>
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MAX_INPUT_SIZE 1024
#define MAX_ARGS       128
#define MAX_JOBS       64
#define MAX_PIPE_CMDS  32
#define JOB_RUNNING    1
#define JOB_STOPPED    2

/* A job represents one command or an entire pipeline (one process group). */
typedef struct Job {
    int job_id;
    pid_t pid;                 /* Process-group leader PID */
    pid_t pgid;
    int state;
    int live_count;            /* Number of child processes not yet reaped */
    int stopped_count;
    pid_t last_pid;            /* Last command in a pipeline */
    int last_status;           /* Exit status of the last command */
    int child_count;
    pid_t child_pids[MAX_PIPE_CMDS];
    char command[MAX_INPUT_SIZE];
    struct Job *next;
} Job;

extern Job *job_list;
extern int next_job_id;
extern volatile sig_atomic_t child_changed;
extern pid_t shell_pgid;
extern int shell_terminal;
extern int shell_interactive;
extern int last_status;
extern char shell_path[PATH_MAX];
extern char shell_prompt[128];

void init_shell(void);
void main_loop(void);
void parse_command(char *input, char **args);
int execute_command(char **args);
int is_builtin_command(char **args);
void handle_builtin_command(char **args);
void handle_redirection_and_piping(char **args);
void signal_handler(int sig);
void init_signal_handlers(void);

void add_job(pid_t pid, char *command);
void remove_job(pid_t pid);
void list_jobs(void);
int bring_job_to_foreground(int job_id);
int send_job_to_background(int job_id);
Job *find_job(int job_id_or_pid);
void reap_background_jobs(void);
void wait_for_foreground(pid_t pgid, int existing_job_id, int process_count, pid_t *child_pids, pid_t last_pid);

#endif
