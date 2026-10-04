#define _POSIX_C_SOURCE 200809L
#include "executor.h"

#define DEFAULT_CAPACITY 32

static int g_last_status = 0; // $? статус завершения последней задачи
static pid_t g_last_pid = 0; // $! пид последней запущенной задачи 
static pid_t g_shell_pid = 0; // $$ пид шелла 
static int g_in_background = 0; // флаг того что мы запустились в фоне
static int g_interactive_shell = 0; // флаг isatty
static int g_terminal_fd = STDIN_FILENO; // фд управляющего терминала в данный момент
static pid_t g_shell_pgid_local = 0;  // pgid шелла, группе которого вернется терминал

typedef int (*builtin_fn)(char **argv, size_t argc, ASTNode *cmd);

static int builtin_cd(char **argv, size_t argc, ASTNode *cmd);
static int builtin_help(char **argv, size_t argc, ASTNode *cmd);
static int builtin_jobs(char **argv, size_t argc, ASTNode *cmd);
static int builtin_fg(char **argv, size_t argc, ASTNode *cmd);
static int builtin_bg(char **argv, size_t argc, ASTNode *cmd);
static int builtin_kill(char **argv, size_t argc, ASTNode *cmd);
static int builtin_exit(char **argv, size_t argc, ASTNode *cmd);
static int builtin_pwd(char **argv, size_t argc, ASTNode *cmd);
static int builtin_echo(char **argv, size_t argc, ASTNode *cmd);
static int execute_builtin(char **argv, size_t argc, ASTNode *cmd, int *is_builtin);
static void build_cmd_arr(ASTNode *root, ASTNode ***cmds, int *count, int *capacity);


static int with_redirects(ASTNode *cmd, builtin_fn fn, char **argv, size_t argc);
static int strcat_expand(char **dst, int *cap, int *len, const char *src);
static char *cmd_to_str(ASTNode *node);
static const char *type_to_op(ASTType type);
static char *node_to_str(ASTNode *node);
static int parse_signal_arg(const char *arg, int *sig_out);


void executor_set_interactive(int fd, pid_t pgid) { // для вызова в my_shell
    g_terminal_fd = fd; // фд обновляем терминала
    g_shell_pgid_local = pgid; // пид группы шела сохраняем
    g_interactive_shell = isatty(fd); // привязан ли фд к терминалу
    jobs_init(fd, pgid); // передаем данные в job control чтобы потом знали кому вернуть терминал после fg
}

int execute(ASTNode *root) {
    if (!root) return -1;
    if (g_shell_pid == 0) {
        g_shell_pid = getpid();
    }
    int rc;
    switch(root->type) {
        case AST_COMMAND:
            rc = exec_cmd(root);
            return rc;
            break;
        case AST_PIPELINE:
            rc = exec_pipe(root);
            return rc;
            break;
        case AST_PIPEBOTH:
            rc = exec_pipe(root);
            return rc;
            break;
        case AST_OR:
            rc = execute(root->data.binary.left);
            if (rc != 0) {
                if (root->data.binary.right) {
                    rc = execute(root->data.binary.right);
                }
            } else {
                return rc;
            }
            break;
        case AST_AND:
            rc = execute(root->data.binary.left);
            if (rc == 0) {
                if (root->data.binary.right) {
                    rc = execute(root->data.binary.right);
                }
            } else {
                return rc;
            }
            break;
        case AST_SEQ:
            rc = execute(root->data.binary.left);
            if (root->data.binary.right) {
                rc = execute(root->data.binary.right);
            }
            return rc;
            break;
        case AST_BACK:
            rc = execute_background(root->data.binary.left);
            if (root->data.binary.right) {
                return execute(root->data.binary.right);
            }
            return rc;
        default:
            break;
    }
    return rc;
}
int execute_background(ASTNode *node) {
    if (!node) return 0;
    if (node->type == AST_COMMAND || node->type == AST_PIPELINE || node->type == AST_PIPEBOTH) {
        int prev = g_in_background; 
        g_in_background = 1; //ставим флаг фона для исполнения чтобы потом вернуть предыдущий флаг
        int rc = execute(node);
        g_in_background = prev;
        return rc;
    }
    char *repr = node_to_str(node); //строка для jobs
    Job *job = jobs_create(repr, 1); //создаем job с флагом фона
    if (repr) free(repr);
    if (!job) {
        fprintf(stderr, "jobs: allocation error\n");
        return -1;
    }
    jobs_attach(job); //добавляем его в список задач
    pid_t pid = fork(); //форкаемся
    if (pid < 0) {
        perror("fork");
        jobs_detach(job);
        jobs_free(job);
        return -1;
    }
    if (pid == 0) { //дочерний процесс - тот, который в фоне еще
        pid_t pgid = getpid(); //пид группы процессов - пид первого процесса
        if (setpgid(0, pgid) == -1) { //создаем новую группу процессов
            perror("setpgid");
            _exit(1);
        }
        signal(SIGINT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL); //чтобы сигналы действовали не на шелл, пока в ребенке восстанавливаем
        signal(SIGQUIT, SIG_DFL);
        g_in_background = 0; //внутри job все fg
        int rc = execute(node); //исполнение
        if (rc == -2) rc = 0;
        _exit(rc < 0 ? 1 : rc);
    }
    jobs_set_pgid(job, pid); //пока в родителе - ставим пид группы
    jobs_add_process(job, pid); //добавляем процесс в список
    g_last_pid = job->pgid; //$!
    jobs_print_start(job); //печать
    g_last_status = 0;
    return 0;
}

static void free_argv(char **argv, size_t count) {
    if (!argv) return;
    for (size_t i = 0; i < count; ++i) {
        free(argv[i]);
    }
    free(argv);
}

static char **build_argv(ASTNode *cmd) { // массив команд 
    int n = cmd->data.commands.count;
    char **argv = malloc(sizeof(char*) * (n + 1));
    if (!argv) {
        fprintf(stderr, "build_argv: malloc error\n");
        return NULL;
    }
    for (int i = 0; i < n; ++i) {
        argv[i] = token_to_str(cmd->data.commands.words[i]);
        if (!argv[i]) {
            free_argv(argv, i);
            return NULL;
        }
    }
    if (n == 0) {
        free(argv);
        return NULL;
    }
    argv[n] = NULL;
    return argv;
}

int open_redirects(ASTNode *cmd) {
    ASTRedirection *tmp = cmd->data.commands.redirects;
    while (tmp) {
        char *path = token_to_str(tmp->target); // подстановка переменных
        if (!path) {
            return -1;
        }
        int io = tmp->io_num;
        int fd = -1;
        switch(tmp->type) {
            case OP_ROUT: {
                fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd < 0) {
                    perror(path);
                    free(path);
                    return -1;
                }
                if (dup2(fd, (io >= 0) ? io : STDOUT_FILENO)==-1) {
                    perror("dup2");
                    close(fd);
                    free(path);
                    return -1;
                }
                close(fd);
                break;
            }
            case OP_APPEND: {
                fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
                if (fd < 0) {
                    perror(path);
                    free(path);
                    return -1;
                }
                if (dup2(fd, (io >= 0) ? io : STDOUT_FILENO)==-1) {
                    perror("dup2");
                    close(fd);
                    free(path);
                    return -1;
                }
                close(fd);
                break;
            }
            case OP_RIN: {
                fd = open(path, O_RDONLY);
                if (fd < 0) {
                    perror(path);
                    free(path);
                    return -1;
                }
                if (dup2(fd, (io >= 0) ? io : STDIN_FILENO)==-1) {
                    perror("dup2");
                    close(fd);
                    free(path);
                    return -1;
                }
                close(fd);
                break;
            }
            case OP_RDBOTH: {
                fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd < 0) {
                    perror(path);
                    free(path);
                    return -1;
                }
                if (dup2(fd, (io >= 0) ? io : STDOUT_FILENO)==-1) {
                    perror("dup2");
                    close(fd);
                    free(path);
                    return -1;
                }
                if (dup2(fd, (io >= 0) ? io : STDERR_FILENO)==-1) {
                    perror("dup2");
                    close(fd);
                    free(path);
                    return -1;
                }
                close(fd);
                break;
            }
            case OP_RDBOTH_APP: {
                fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
                if (fd < 0) {
                    perror(path);
                    free(path);
                    return -1;
                }
                if (dup2(fd, (io >= 0) ? io : STDOUT_FILENO)==-1) {
                    perror("dup2");
                    close(fd);
                    free(path);
                    return -1;
                }
                if (dup2(fd, (io >= 0) ? io : STDERR_FILENO)==-1) {
                    perror("dup2");
                    close(fd);
                    free(path);
                    return -1;
                }
                close(fd);
                break;
            }
            default:
                break;
        }
        free(path);
        tmp = tmp->next;
    }
    return 0;
}

int exec_cmd(ASTNode* cmd) {
    if (cmd->type != AST_COMMAND) return -1;
    size_t argc = cmd->data.commands.count; // количество аргументов
    char **argv = build_argv(cmd); // массив команд
    if (!argv || !argv[0]) {
        free_argv(argv, argc);
        g_last_status = 1;
        return -1;
    }
    int is_builtin = 0; // команда - builtin
    int rc_builtin = execute_builtin(argv, argc, cmd, &is_builtin);
    if (is_builtin) { 
        free_argv(argv, argc);
        return rc_builtin;
    }
    char *repr = node_to_str(cmd);
    Job *job = jobs_create(repr, g_in_background);
    if (repr) free(repr);
    if (!job) {
        fprintf(stderr, "jobs: allocation error\n");
        free_argv(argv, argc);
        g_last_status = 1;
        return -1;
    }
    jobs_attach(job); // добавляем в список джобов
    pid_t pid = fork(); // форкаемся
    if (pid < 0) {
        perror("fork");
        jobs_detach(job);
        jobs_free(job);
        free_argv(argv, argc);
        g_last_status = 1;
        return -1;
    }
    if (pid == 0) {
        pid_t pgid = job->pgid ? job->pgid : getpid(); // смотрим уже в группе или нет
        if (setpgid(0, pgid) == -1) { // пид процесса - 0, поместили его в группы процессов pgid
            perror("setpgid");
            _exit(1);
        }
        signal(SIGINT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL); // ставим дефолтные сигналы в ребенке чтобы не снести баш
        signal(SIGQUIT, SIG_DFL); // и иметь возможность посылать процессу сигнал
        if (open_redirects(cmd)==-1) {
            free_argv(argv, argc);
            _exit(1);
        }
        execvp(argv[0], argv);
        perror(argv[0]);
        free_argv(argv, argc);
        _exit(1);
    }
    if (job->pgid == 0) {
        jobs_set_pgid(job, pid); // ставим pgid, ребенок - лидер процесса
    }
    if (setpgid(pid, job->pgid) == -1 && errno != EACCES) { //добавляет ребенка в список процессов
        perror("setpgid");
    }
    jobs_add_process(job, pid);
    g_last_pid = pid;
    free_argv(argv, argc);
    if (g_in_background) {
        jobs_print_start(job);
        g_last_pid = job->pgid;
        g_last_status = 0;
        return 0;
    }
    int rc = 0;
    jobs_wait_foreground(job, &rc);
    g_last_status = rc;
    return rc;
}

int exec_pipe(ASTNode *node) {
    if (node->type != AST_PIPEBOTH && node->type != AST_PIPELINE) return -1;
    int pipeboth_flag = 0;
    if (node->type == AST_PIPEBOTH) pipeboth_flag = 1;
    int capacity = DEFAULT_WORDS_COUNT;
    int n = 0;
    ASTNode **cmds = malloc(capacity*sizeof(ASTNode*)); 
    if (!cmds) {
        fprintf(stderr, "exec pipe: malloc error\n");
        g_last_status = 1;
        return -1;
    }
    build_cmd_arr(node, &cmds, &n, &capacity); // масств команд
    if (n==0) {
        free(cmds); return 0;
    }
    if (n==1) {
        int rc = exec_cmd(cmds[0]); // если 1 команда
        free(cmds); return rc;
    }
    char *repr = node_to_str(node);
    Job *job = jobs_create(repr, g_in_background);
    if (repr) free(repr);
    if (!job) {
        fprintf(stderr, "jobs: allocation error\n");
        free(cmds);
        g_last_status = 1;
        return -1;
    }
    jobs_attach(job);
    int pipes[n-1][2]; // массив - n-1 пайпов
    for (int i = 0; i<n-1; ++i) { // открываем нужные пайпы
        if (pipe(pipes[i])==-1) {
            perror("pipe");
            for (int j=0; j<i; ++j) { 
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            jobs_detach(job);
            jobs_free(job);
            free(cmds);
            g_last_status = 1;
            return -1;
        }
    }
    pid_t pids[n]; // массив пидов
    for (int i = 0; i<n; ++i) {
        pids[i]=fork();
        if (pids[i]<0) {
            perror("fork");
            for (int j=0; j<n-1; ++j) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            jobs_detach(job);
            jobs_free(job);
            free(cmds);
            g_last_status = 1;
            return -1;
        }
        if (pids[i]==0) {
            pid_t pgid = job->pgid;
            if (pgid == 0) pgid = getpid(); 
            if (setpgid(0, pgid) == -1) { // добавляем пид в группу
                perror("setpgid"); _exit(1);
            }
            signal(SIGINT, SIG_DFL); //сигналы не убивают шелл, но должны убивать джобы
            signal(SIGTSTP, SIG_DFL); //поэтому сбрасываем sig_ign
            signal(SIGQUIT, SIG_DFL);
            if (i>0) {
                close(pipes[i-1][1]); // закрываем ненужный конец, в данном случае выход
                if (dup2(pipes[i-1][0], STDIN_FILENO)==-1) { // перенаправляем ввод в пайп
                    perror("dup2"); _exit(1);
                }
            }
            if (i<n-1) {
                close(pipes[i][0]); // закрываем вход
                if (dup2(pipes[i][1], STDOUT_FILENO) == -1) {
                    perror("dup2"); _exit(1);
                }
                if (pipeboth_flag) {
                    if (dup2(STDOUT_FILENO, STDERR_FILENO)==-1) {
                        perror("dup2"); _exit(1);
                    }
                }
            }
            for (int j=0; j<n-1; ++j) { // ненужное закрыли
                close(pipes[j][0]); close(pipes[j][1]);
            }
            size_t argc_i = cmds[i]->data.commands.count; // n 
            char **argv = build_argv(cmds[i]);
            if (!argv || !argv[0]) _exit(1);
            int open = open_redirects(cmds[i]);
            if (open==-1) {
                free_argv(argv, argc_i); _exit(1);
            }
            execvp(argv[0], argv); // уже с перенаправленным вводом/выводом
            perror(argv[0]);
            free_argv(argv, argc_i);
            _exit(1); 
        } else {
            if (i>0) close(pipes[i-1][0]); 
            if (job->pgid == 0) {
                jobs_set_pgid(job, pids[i]); // делаем ребенка лидером группы
            }
            if (setpgid(pids[i], job->pgid) == -1 && errno != EACCES) { // добавляем ребенка в группу
                perror("setpgid");
            }
            jobs_add_process(job, pids[i]);
        }
    }
    for (int i = 0; i<n-1; ++i) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }
    g_last_pid = (n > 0) ? pids[n-1] : 0;
    if (g_in_background) {
        jobs_print_start(job);
        g_last_pid = job->pgid;
        free(cmds);
        g_last_status = 0;
        return 0;
    }
    int rc = 0;
    jobs_wait_foreground(job, &rc);
    free(cmds);
    g_last_status = rc;
    return rc;
}

static void build_cmd_arr(ASTNode *root, ASTNode ***cmds, int *count, int *capacity) {
    if (!root) return;
    if (root->type == AST_COMMAND) {
        if (*count == *capacity -1) {
            (*capacity) *= 2;
            ASTNode **tmp = realloc(*cmds, (*capacity)*sizeof(ASTNode*));
            if (!tmp) {
                fprintf(stderr, "exec_pipe: realloc error\n");
                return;
            }
            *cmds = tmp;
        }
        (*cmds)[(*count)++] = root;
        return;
    }
    if (root->data.binary.left) build_cmd_arr(root->data.binary.left, cmds, count, capacity);
    if (root->data.binary.right) build_cmd_arr(root->data.binary.right, cmds, count, capacity);
}

static int builtin_cd(char **argv, size_t argc, ASTNode *cmd) {
    (void)argc; (void)cmd;
    char oldpwd[4096];
    if (!getcwd(oldpwd, sizeof(oldpwd))) {
        strcpy(oldpwd, "");
    }
    const char *path = argv[1];
    int rc = 0;
    if (!path) {
        fprintf(stderr, "cd: no path specified\n");
        rc = 1;
    } else if (chdir(path)==-1) {
        perror("cd");
        rc = 1;
    }
    g_last_status = rc;
    return rc;
}

static int builtin_help(char **argv, size_t argc, ASTNode *cmd) {
    (void)argv; (void)argc; (void)cmd;
    const char *msg =
        "Builtins: cd, pwd, echo, help, exit, jobs, fg, bg, kill\n"
        "Operators: ; && || & | |&\n"
        "Redirections: > >> < &> &>>\n";
    write(STDOUT_FILENO, msg, strlen(msg));
    g_last_status = 0;
    return 0;
}

static int builtin_jobs(char **argv, size_t argc, ASTNode *cmd) {
    (void)argv; (void)argc; (void)cmd;
    jobs_handle_sigchld();
    jobs_print();
    g_last_status = 0;
    return 0;
}

static int builtin_fg(char **argv, size_t argc, ASTNode *cmd) {
    (void)argc; (void)cmd;
    jobs_handle_sigchld();
    int rc = jobs_builtin_fg(argv[1]);
    g_last_status = rc;
    return rc;
}

static int builtin_bg(char **argv, size_t argc, ASTNode *cmd) {
    (void)argc; (void)cmd;
    jobs_handle_sigchld();
    int rc = jobs_builtin_bg(argv[1]);
    g_last_status = rc;
    return rc;
}

static int builtin_kill(char **argv, size_t argc, ASTNode *cmd) {
    (void)argc; (void)cmd;
    int rc = 1;
    int sig = SIGTERM; // сигнал по умолчанию
    int arg_idx = 1; // начало индексации сигнала
    int target_end = -1; // если сигнал задан после pid, берем только первый pid
    if (!argv[1]) {
        fprintf(stderr, "kill: missing operand\n");
        g_last_status = 1;
        return 1;
    }
    // вариант: kill -SIGSTOP pid
    if (argv[1][0] == '-' && argv[2]) {
        int tmp_sig = 0;
        if (parse_signal_arg(argv[1] + 1, &tmp_sig) == 0) {
            sig = tmp_sig;
            arg_idx = 2;
        }
    } else if (argv[2]) {
        // вариант: kill pid SIGSTOP
        int tmp_sig = 0;
        if (parse_signal_arg(argv[2], &tmp_sig) == 0) {
            sig = tmp_sig;
            target_end = 1; // используем только первый аргумент как цель
        }
    }
    for (int i = arg_idx; argv[i]; ++i) {
        if (target_end != -1 && i > target_end) break;
        if (argv[i][0] == '%') {
            int res = jobs_builtin_kill(argv[i]);
            if (res != 0) rc = 1;
            continue;
        }
        char *end = NULL;
        long val = strtol(argv[i], &end, 10);
        if (!end || *end != '\0' || val <= 0) {
            fprintf(stderr, "kill: invalid pid\n");
            rc = 1;
            continue;
        }
        if (kill((pid_t)val, sig) == -1) {
            perror("kill");
            rc = 1;
        } else {
            rc = 0;
        }
    }
    g_last_status = rc;
    return rc;
}

static int builtin_exit(char **argv, size_t argc, ASTNode *cmd) {
    (void)argv; (void)argc; (void)cmd;
    g_last_status = 0;
    return -2;
}

static int builtin_pwd(char **argv, size_t argc, ASTNode *cmd) {
    (void)argv; (void)argc; (void)cmd;
    char cwd[4096];
    int rc = 0;
    if (!getcwd(cwd, sizeof(cwd))) {
        strcpy(cwd, "?");
        rc = 1;
    }
    int n = strlen(cwd);
    cwd[n]='\n'; ++n; cwd[n]='\0';
    write(STDOUT_FILENO, cwd, n);
    g_last_status = rc;
    return rc;
}

static int builtin_echo(char **argv, size_t argc, ASTNode *cmd) {
    (void)argc; (void)cmd;
    int index = 1;
    int newline = 1;
    if (argv[1] && strcmp(argv[1], "-n")==0) {
        newline = 0; ++index;
    }
    for (int i = index; argv[i]!=NULL; ++i) {
        if (i>index) write(STDOUT_FILENO, " ", 1);
        write(STDOUT_FILENO, argv[i], strlen(argv[i]));
    }
    if (newline) write(STDOUT_FILENO, "\n", 1);
    g_last_status = 0;
    return 0;
}

static builtin_fn resolve_builtin(const char *name) {
    if (!name) return NULL;
    if (strcmp(name, "cd") == 0) return builtin_cd;
    if (strcmp(name, "help") == 0) return builtin_help;
    if (strcmp(name, "jobs") == 0) return builtin_jobs;
    if (strcmp(name, "fg") == 0) return builtin_fg;
    if (strcmp(name, "bg") == 0) return builtin_bg;
    if (strcmp(name, "kill") == 0) return builtin_kill;
    if (strcmp(name, "exit") == 0) return builtin_exit;
    if (strcmp(name, "pwd") == 0) return builtin_pwd;
    if (strcmp(name, "echo") == 0) return builtin_echo;
    return NULL;
}

static int execute_builtin(char **argv, size_t argc, ASTNode *cmd, int *is_builtin) {
    builtin_fn fn = resolve_builtin(argv[0]);
    *is_builtin = (fn != NULL);
    if (!fn) return 0;
    return with_redirects(cmd, fn, argv, argc);
}

static int with_redirects(ASTNode *cmd, builtin_fn fn, char **argv, size_t argc) {
    int saved[3] = {-1, -1, -1};
    for (int i = 0; i < 3; ++i) {
        saved[i] = dup(i);
        if (saved[i] < 0) {
            perror("dup");
            for (int j = 0; j < i; ++j) {
                if (saved[j] >= 0) close(saved[j]);
            }
            g_last_status = 1;
            return -1;
        }
    }
    if (open_redirects(cmd) == -1) {
        for (int i = 0; i < 3; ++i) {
            if (saved[i] >= 0) {
                dup2(saved[i], i);
                close(saved[i]);
            }
        }
        g_last_status = 1;
        return -1;
    }
    int rc = fn(argv, argc, cmd);
    for (int i = 0; i < 3; ++i) {
        if (saved[i] >= 0) {
            dup2(saved[i], i);
            close(saved[i]);
        }
    }
    return rc;
}

static int parse_signal_arg(const char *arg, int *sig_out) {
    char *end = NULL;
    int v = strtol(arg, &end, 10);
    if (end && *end == '\0' && v > 0) { // вся строка была числом
        *sig_out = v;
        return 0;
    }
    const char *name = arg;
    if (strncmp(name, "SIG", 3) == 0) name += 3;
    struct { const char *name; int sig; } table[] = {
        {"HUP", SIGHUP}, {"INT", SIGINT}, {"QUIT", SIGQUIT}, {"ILL", SIGILL},
        {"ABRT", SIGABRT}, {"KILL", SIGKILL}, {"ALRM", SIGALRM}, {"TERM", SIGTERM},
        {"STOP", SIGSTOP}, {"TSTP", SIGTSTP}, {"CONT", SIGCONT}, {"CHLD", SIGCHLD},
        {"USR1", SIGUSR1}, {"USR2", SIGUSR2}, {"PIPE", SIGPIPE}, {"TTIN", SIGTTIN},
        {"TTOU", SIGTTOU}, {"SEGV", SIGSEGV}, {"FPE", SIGFPE}, {"BUS", SIGBUS},
        {"TRAP", SIGTRAP}
    };
    size_t count = sizeof(table)/sizeof(table[0]);
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(name, table[i].name) == 0) {
            *sig_out = table[i].sig;
            return 0;
        }
    }
    return -1;
}

char *token_to_str(const Token *token) {
    if (!token || !token->value) return NULL;
    int cap = DEFAULT_CAPACITY;
    char *out = malloc(cap);
    if (!out) return NULL;
    int len = 0;
    out[0] = '\0'; //строка которая подставляет подстановки

    for (int i = 0; token->value[i]; ) {
        if (token->value[i] == '$' && !token->single_quotes) {
            const char *next = token->value + i + 1; // строка начинающаяся с i+1 символа
            char tmp[2] = { '$', '\0' };
            if (!*next) { strcat_expand(&out, &cap, &len, tmp); ++i; continue; } // если $-последний, то просто добавляем
            if (*next == '$' || *next == '?' || *next == '!') { //если вся строка это $!/ $? \ $$
                char tmp[32]; // буфер для конвертации числа в строку
                const char *rep = "";
                if (*next == '$') { // пишем строку в tmp, rep = tmp
                    snprintf(tmp, sizeof(tmp), "%d", g_shell_pid ? g_shell_pid : getpid());
                    rep = tmp;
                } else if (*next == '?') {
                    snprintf(tmp, sizeof(tmp), "%d", g_last_status);
                    rep = tmp;
                } else {
                    snprintf(tmp, sizeof(tmp), "%d", g_last_pid);
                    rep = tmp;
                }
                strcat_expand(&out, &cap, &len, rep); // rep -> out
                i += 2;
                continue;
            }
        }
        char temp[2] = {token->value[i], '\0'};
        strcat_expand(&out, &cap, &len, temp);
        ++i;
    }
    return out;
}

static int strcat_expand(char **dst, int *cap, int *len, const char *src) { // strcat + realloc
    if (!src) return 0;
    if (*len + strlen(src) + 1 >= (size_t)*cap) {
        int new_cap = *cap ? *cap : DEFAULT_CAPACITY;
        while (*len + strlen(src) + 1 >= (size_t)new_cap) new_cap *= 2;
        char *tmp = realloc(*dst, new_cap);
        if (!tmp) return -1;
        *dst = tmp;
        *cap = new_cap;
    }
    memcpy(*dst + *len, src, strlen(src));
    *len += strlen(src);
    (*dst)[*len] = '\0';
    return 0;
}

static char *cmd_to_str(ASTNode *node) { // для job control
    if (!node || node->type != AST_COMMAND) return NULL;
    int cap = 64;
    int len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    buf[0] = '\0';
    for (size_t i = 0; i < node->data.commands.count; ++i) {
        const Token *token = node->data.commands.words[i];
        if (!token || !token->value) continue;
        if (len > 0) {
            if (strcat_expand(&buf, &cap, &len, " ") == -1) {
                free(buf);
                return NULL;
            }
        }
        if (strcat_expand(&buf, &cap, &len, token->value) == -1) {
            free(buf);
            return NULL;
        }
    }
    return buf;
}

static const char *type_to_op(ASTType type) {
    switch (type) {
        case AST_PIPELINE:
            return " | ";
        case AST_PIPEBOTH:
            return " |& ";
        case AST_AND:
            return " && ";
        case AST_OR:
            return " || ";
        case AST_SEQ:
            return "; ";
        case AST_BACK:
            return " & ";
        default:
            break;
    }
    return " ";
}

static char *node_to_str(ASTNode *node) { // для job control 
    if (!node) return NULL;
    if (node->type == AST_COMMAND) {
        return cmd_to_str(node);
    }
    char *left = node->data.binary.left ? node_to_str(node->data.binary.left) : NULL; //левое поддерево
    char *right = node->data.binary.right ? node_to_str(node->data.binary.right) : NULL;//правое
    const char *op = type_to_op(node->type); //оператор
    int cap = 64;
    int len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        if (left) free(left);
        if (right) free(right);
        return NULL;
    }
    buf[0] = '\0';
    if (left) {
        if (strcat_expand(&buf, &cap, &len, left) == -1) { //превращаем в строку левое поддерево
            free(buf);
            if (left) free(left);
            if (right) free(right);
            return NULL;
        }
    }
    if (strcat_expand(&buf, &cap, &len, op) == -1) { //дбавляем в строку оператор
        free(buf);
        if (left) free(left);
        if (right) free(right);
        return NULL;
    }
    if (right) {
        if (strcat_expand(&buf, &cap, &len, right) == -1) { //добавляем правое поддререво
            free(buf);
            if (left) free(left);
            if (right) free(right);
            return NULL;
        }
    }
    if (left) free(left);
    if (right) free(right);
    return buf;
}
