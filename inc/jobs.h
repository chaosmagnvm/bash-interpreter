#pragma once

#include <sys/types.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>

typedef enum {
    JOB_RUNNING,
    JOB_STOPPED,
    JOB_DONE,
    JOB_TERMINATED
} JobState;

typedef struct JobProcess { 
    pid_t pid; 
    int completed;
    int stopped;
    int status; //exit-status
    struct JobProcess *next;
} JobProcess;

typedef struct Job {
    int id;
    pid_t pgid;
    int background;
    int notified; 
    char *command;
    JobState state;
    JobProcess *processes;
    struct Job *next; 
} Job;

void jobs_init(int terminal_fd, pid_t shell_pgid);
Job *jobs_create(const char *command, int background);
void jobs_free(Job *job);
void jobs_attach(Job *job);
void jobs_detach(Job *job);
void jobs_set_pgid(Job *job, pid_t pgid);
void jobs_add_process(Job *job, pid_t pid);
void jobs_update_process(pid_t pid, int status);
void jobs_mark_running(Job *job);
void jobs_mark_stopped(Job *job);
void jobs_mark_done(Job *job);
void jobs_notify(void);
void jobs_print(void);
Job *jobs_find_id(int id);
Job *jobs_find_recent(int stopped_only);
Job *jobs_find_pgid(pid_t pgid);
int jobs_continue(Job *job, int foreground);
int jobs_kill(Job *job, int sig);
int jobs_wait_foreground(Job *job, int *status_out);
void jobs_set_sigchld_flag(void);
void jobs_handle_sigchld(void);
int jobs_parse_id(const char *arg, int *id_out);
void jobs_print_start(Job *job);
int jobs_builtin_fg(const char *arg);
int jobs_builtin_bg(const char *arg);
int jobs_builtin_kill(const char *arg);
