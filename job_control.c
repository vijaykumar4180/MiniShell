#include "minishell.h"

void add_job(pid_t pid, char *command)
{
    Job *job = calloc(1, sizeof(*job));
    if (!job) {
        perror("calloc");
        return;
    }
    job->job_id = next_job_id++;
    job->pid = pid;
    job->pgid = pid;
    job->state = JOB_RUNNING;
    job->live_count = 1;
    job->child_count = 1;
    job->last_pid = pid;
    job->last_status = 0;
    job->child_pids[0] = pid;
    snprintf(job->command, sizeof(job->command), "%s", command ? command : "");
    job->next = job_list;
    job_list = job;
}

Job *find_job(int job_id_or_pid)
{
    for (Job *job = job_list; job; job = job->next) {
        if (job->job_id == job_id_or_pid || job->pid == (pid_t)job_id_or_pid ||
            job->pgid == (pid_t)job_id_or_pid)
            return job;
    }
    return NULL;
}

void remove_job(pid_t pid)
{
    Job **link = &job_list;
    while (*link) {
        if ((*link)->pid == pid || (*link)->pgid == pid) {
            Job *old = *link;
            *link = old->next;
            free(old);
            return;
        }
        link = &(*link)->next;
    }
}

void list_jobs(void)
{
    for (Job *job = job_list; job; job = job->next) {
        printf("[%d] %s\tPID/PGID: %ld\t%s\n",
               job->job_id,
               job->state == JOB_STOPPED ? "Stopped" : "Running",
               (long)job->pgid,
               job->command);
    }
}

static Job *select_job(int id)
{
    if (id > 0)
        return find_job(id);
    /* No argument: choose the most recently added job. */
    return job_list;
}

int bring_job_to_foreground(int job_id)
{
    Job *job = select_job(job_id);
    if (!job) {
        fprintf(stderr, "msh: fg: no such job\n");
        return 1;
    }

    pid_t pgid = job->pgid;
    int id = job->job_id;
    int child_count = job->child_count;
    pid_t last_pid = job->last_pid;

    if (shell_interactive && tcsetpgrp(shell_terminal, pgid) < 0)
        perror("fg: tcsetpgrp");
    if (kill(-pgid, SIGCONT) < 0 && errno != ESRCH) {
        perror("fg: SIGCONT");
        if (shell_interactive)
            tcsetpgrp(shell_terminal, shell_pgid);
        return 1;
    }
    job->state = JOB_RUNNING;
    job->stopped_count = 0;
    wait_for_foreground(pgid, id, child_count, job->child_pids, last_pid);
    return last_status;
}

int send_job_to_background(int job_id)
{
    Job *job = select_job(job_id);
    if (!job) {
        fprintf(stderr, "msh: bg: no such job\n");
        return 1;
    }

    if (kill(-job->pgid, SIGCONT) < 0) {
        perror("bg: SIGCONT");
        return 1;
    }
    job->state = JOB_RUNNING;
    job->stopped_count = 0;
    printf("[%d] %s &\n", job->job_id, job->command);
    return 0;
}
