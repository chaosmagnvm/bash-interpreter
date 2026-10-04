#define _POSIX_C_SOURCE 200809L
#include "jobs.h"

static Job *g_jobs = NULL;
static int g_next_job_id = 1;
static int g_terminal_fd = STDIN_FILENO;
static pid_t g_shell_pgid = 0;
static struct termios g_shell_modes;
static int g_modes_saved = 0;
static volatile sig_atomic_t g_sigchld_flag = 0; 

JobProcess *create_process(pid_t pid) {
    JobProcess *proc = (JobProcess*)malloc(sizeof(JobProcess));
    if (!proc) return NULL;
    proc->pid = pid;
    proc->completed = 0;
    proc->stopped = 0;
    proc->status = 0;
    proc->next = NULL;
    return proc;
}

void jobs_init(int terminal_fd, pid_t shell_pgid) { 
    g_terminal_fd = terminal_fd;
    g_shell_pgid = shell_pgid;
    if (isatty(g_terminal_fd)) {
        if (tcgetattr(g_terminal_fd, &g_shell_modes) == 0) {
            g_modes_saved = 1;
        }
    }
}

Job *jobs_create(const char *command, int background) {
    Job *job = (Job*)malloc(sizeof(Job));
    if (!job) return NULL;
    job->id = g_next_job_id++;
    job->pgid = 0;
    job->background = background;
    job->notified = 0;
    job->command = command ? strdup(command) : NULL;
    job->state = JOB_RUNNING;
    job->processes = NULL;
    job->next = NULL;
    return job;
}

void jobs_free(Job *job) {
    if (!job) return;
    JobProcess *proc = job->processes;
    while (proc) {
        JobProcess *next = proc->next;
        free(proc);
        proc = next;
    }
    if (job->command) free(job->command);
    free(job);
}

void jobs_attach(Job *job) {
    if (!job) return;
    job->next = g_jobs;
    g_jobs = job;
}

void jobs_detach(Job *job) {
    if (!job) return;
    Job **ptr = &g_jobs;
    while (*ptr) {
        if (*ptr == job) {
            *ptr = job->next;
            job->next = NULL;
            return;
        }
        ptr = &((*ptr)->next);
    }
}

void jobs_set_pgid(Job *job, pid_t pgid) {
    if (!job) return;
    job->pgid = pgid;
}

void jobs_add_process(Job *job, pid_t pid) {
    if (!job) return;
    JobProcess *proc = create_process(pid);
    if (!proc) return;
    if (!job->processes) {
        job->processes = proc;
    } else {
        JobProcess *tmp = job->processes;
        while (tmp->next) tmp = tmp->next;
        tmp->next = proc;
    }
}

int job_stopped(Job *job) {
    JobProcess *proc = job->processes;
    if (!proc) return 0;
    while (proc) {
        if (!proc->completed && !proc->stopped) return 0;
        proc = proc->next;
    }
    return 1;
}

int job_completed(Job *job) {
    JobProcess *proc = job->processes;
    if (!proc) return 0;
    while (proc) {
        if (!proc->completed) return 0;
        proc = proc->next;
    }
    return 1;
}

int job_terminated(Job *job) {
    JobProcess *proc = job->processes;
    if (!proc) return 0;
    while (proc) {
        if (!proc->completed) return 0;
        if (!WIFSIGNALED(proc->status)) return 0;
        proc = proc->next;
    }
    return 1;
}

JobProcess *find_proc(Job *job, pid_t pid) {
    JobProcess *proc = job->processes;
    while (proc) {
        if (proc->pid == pid) return proc;
        proc = proc->next;
    }
    return NULL;
}

void jobs_update_process(pid_t pid, int status) {
    Job *job = g_jobs; 
    //int term_flag = 0;
    while (job) {
        JobProcess *proc = find_proc(job, pid);
        if (proc) {
            proc->status = status;
            if (WIFSTOPPED(status)) { //помечаем остановку только процесса
                proc->stopped = 1;
                proc->completed = 0;
                job->notified = 0;
                job->state = job_stopped(job) ? JOB_STOPPED : JOB_RUNNING; // только если все стоп
            } else if (WIFCONTINUED(status)) { //если возобновленный, снимаем стоп
                proc->stopped = 0;
                job->state = JOB_RUNNING;
                job->notified = 0;
            } else {
                if (WIFSIGNALED(status)) { //если завершился сигналои
                    proc->completed = 1;
                    proc->stopped = 0;
                    //term_flag = 1;
                }
                if (WIFEXITED(status)) {
                    proc->completed = 1;
                    proc->stopped = 0;
                }
                int all_completed = job_completed(job);
                int all_stopped = job_stopped(job);
                int all_terminated = job_terminated(job);
                if (all_terminated) {
                    job->state = JOB_TERMINATED;
                } else if (all_completed) {
                    job->state = JOB_DONE;
                } else if (all_stopped) {
                    job->state = JOB_STOPPED;
                }
                job->notified = 0;
            }
            return;
        }
        job = job->next;
    }
}


void jobs_mark_running(Job *job) { // 
    if (!job) return;
    JobProcess *proc = job->processes;
    while (proc) {
        if (!proc->completed) {
            proc->stopped = 0; // снимаем стоп только с живых
        }
        proc = proc->next;
    }
    job->state = JOB_RUNNING;
    job->notified = 0;
}

void jobs_mark_stopped(Job *job) {
    if (!job) return;
    job->state = JOB_STOPPED;
    job->notified = 0;
}

void jobs_mark_done(Job *job) {
    if (!job) return;
    job->state = JOB_DONE;
    job->notified = 0;
}

static void print_job_status(Job *job, const char *status) {
    if (!job) return;
    if (!job->command) {
        printf("[%d] %s\n", job->id, status);
    } else {
        printf("[%d] %s\t%s\n", job->id, status, job->command);
    }
}

void jobs_notify(void) {
    Job *job = g_jobs;
    while (job) {
        Job *next = job->next;
        if (!job->notified) {
            if (job->state == JOB_DONE) {
                print_job_status(job, "Done");
                job->notified = 1;
                jobs_detach(job);
                jobs_free(job);
            } else if (job->state == JOB_STOPPED) {
                print_job_status(job, "Stopped");
                job->notified = 1;
            } else if (job->state == JOB_TERMINATED) {
                print_job_status(job, "Terminated");
                job->notified = 1;
                jobs_detach(job);
                jobs_free(job);
            }
        }
        job = next;
    }
}

void jobs_print_start(Job *job) {
    if (!job) return;
    if (job->command) {
        printf("[%d] %d\t%s\n", job->id, (int)job->pgid, job->command);
    } else {
        printf("[%d] %d\n", job->id, (int)job->pgid);
    }
}

void jobs_print(void) {
    Job *job = g_jobs;
    while (job) {
        if (job->state != JOB_DONE) {
            if (!job->background && job->state == JOB_RUNNING) {
                job = job->next;
                continue;
            }
            const char *status = "Running";
            if (job->state == JOB_STOPPED) status = "Stopped";
            if (job->state != JOB_TERMINATED) printf("[%d] %s\t%s\n", job->id, status, job->command ? job->command : "");
        }
        job = job->next;
    }
}

Job *jobs_find_id(int id) { //ищет по айди
    Job *job = g_jobs;
    while (job) {
        if (job->id == id) return job;
        job = job->next;
    }
    return NULL;
}

Job *jobs_find_recent(int stopped_only) { //1 тот который остановился, 0 - просто последний
    Job *job = g_jobs; 
    Job *last = NULL;
    int max_id = 0;
    while (job) {
        if (job->state == JOB_DONE) {
            job = job->next;
            continue;
        }
        if (stopped_only && job->state != JOB_STOPPED) {
            job = job->next;
            continue;
        }
        if (job->id > max_id) {
            max_id = job->id;
            last = job;
        }
        job = job->next;
    }
    return last;
}

Job *jobs_find_pgid(pid_t pgid) { //ищет по пиду группы
    Job *job = g_jobs;
    while (job) {
        if (job->pgid == pgid) return job;
        job = job->next;
    }
    return NULL;
}

int jobs_continue(Job *job, int foreground) { //отправляет сигнал продолжения всей группе
    if (!job) return -1;
    if (kill(-job->pgid, SIGCONT) < 0) {
        perror("kill(SIGCONT)");
        return -1;
    }
    jobs_mark_running(job);
    if (foreground) {
        int status = 0;
        jobs_wait_foreground(job, &status);
        return status;
    }
    job->background = 1;
    jobs_print_start(job);
    return 0;
}

int jobs_kill(Job *job, int sig) { //посыпалет сигнал всей группе
    if (!job) return -1;
    if (kill(-job->pgid, sig) < 0) {
        perror("kill");
        return -1;
    }
    return 0;
}

void restore_terminal(void) { //если настоящий терминал, то возвращает владение после fg stop/kill
    if (isatty(g_terminal_fd)) {
        tcsetpgrp(g_terminal_fd, g_shell_pgid); // передаем терминал обратно шеллу
        if (g_modes_saved) {
            tcsetattr(g_terminal_fd, TCSADRAIN, &g_shell_modes); // восстанавливаем настройки терминала
        }
    }
}

int jobs_wait_foreground(Job *job, int *status_out) {
    if (!job) return -1;
    tcsetpgrp(g_terminal_fd, job->pgid); //передаем терминал джобу
    int status;
    pid_t pid;
    int last_status = 0;
    while (!job_completed(job) && !job_stopped(job)) {
        pid = waitpid(-job->pgid, &status, WUNTRACED); //возвращает управление если процесс остановлен
        if (pid < 0) {
            if (errno == EINTR) continue;
            perror("waitpid");
            break;
        }
        jobs_update_process(pid, status);
        if (WIFEXITED(status)) {
            last_status = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            last_status = 128 + WTERMSIG(status);
        } else if (WIFSTOPPED(status)) {
            last_status = 128 + WSTOPSIG(status);
        }
    }
    JobProcess *p = job->processes;
    while (p && p->next) p = p->next; // последний процесс в конвейере
    if (p) {
        if (WIFEXITED(p->status)) {
            last_status = WEXITSTATUS(p->status);
        } else if (WIFSIGNALED(p->status)) {
            last_status = 128 + WTERMSIG(p->status);
        } else if (WIFSTOPPED(p->status)) {
            //last_status = 128 + WSTOPSIG(p->status);
        }
    }
    restore_terminal();
    if (status_out) {
        *status_out = last_status;
    }
    if (job->state == JOB_DONE) {
        jobs_detach(job);
        jobs_free(job);
    } else if (job->state == JOB_STOPPED) {
        job->background = 1;
    }
    return 0;
}

void jobs_set_sigchld_flag(void) {
    g_sigchld_flag = 1;
}

void jobs_handle_sigchld(void) { //обработчик sigchild
    if (!g_sigchld_flag) return;
    g_sigchld_flag = 0;
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED)) > 0) {
        jobs_update_process(pid, status); // обновляем статус процесса в сооветствие с флагом
    }
}

int jobs_parse_id(const char *arg, int *id_out) { // получаем id из строки
    if (!arg || !id_out) return -1;
    if (arg[0] == '%') ++arg;
    if (!*arg) return -1;
    int id = 0;
    while (*arg) {
        if (*arg < '0' || *arg > '9') return -1;
        id = id * 10 + (*arg - '0');
        ++arg;
    }
    *id_out = id;
    return 0;
}

Job *resolve_job_arg(const char *arg, int stopped_only) { // строка -> джоб
    if (!arg || !*arg) {
        return jobs_find_recent(stopped_only);
    }
    int id = 0;
    if (jobs_parse_id(arg, &id) == -1) {
        fprintf(stderr, "jobs: invalid id\n");
        return NULL;
    }
    Job *job = jobs_find_id(id);
    if (!job) {
        fprintf(stderr, "jobs: no such job\n");
    }
    return job;
}

int jobs_builtin_fg(const char *arg) {
    Job *job = resolve_job_arg(arg, 0); // получаем джоб из строки по номеру
    if (!job) return 1;
    job->background = 0;
    return jobs_continue(job, 1); // продолжаем в фг
}

int jobs_builtin_bg(const char *arg) {
    Job *job = resolve_job_arg(arg, 1); // только остановленные ищем
    if (!job) return 1; 
    return jobs_continue(job, 0); // продолжаем в фоне
}

int jobs_builtin_kill(const char *arg) {
    Job *job = resolve_job_arg(arg, 0); // ищем неостановленные
    if (!job) return 1;
    return jobs_kill(job, SIGTERM); // убиваем процесс
} 
