#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>

#include <pwd.h>
#include <sys/utsname.h>
#include <limits.h>

#include "token.h"
#include "lexer.h"
#include "astnode.h"
#include "parser.h"
#include "executor.h"
#include "jobs.h"

#define BUF_SIZE 8192

static volatile sig_atomic_t g_got_sigint = 0;

static void sigchld_handler(int signo) {
    (void)signo;
    jobs_set_sigchld_flag();
}

static void sigint_handler(int signo) {
    (void)signo;
    g_got_sigint = 1; // ctrl+c
}

static void print_prompt(void) {
    char cwd[4096];
    struct utsname u;
    const char *user = getenv("USER");
    if (!user) {
        struct passwd *pw = getpwuid(getuid());
        user = pw ? pw->pw_name : "?";
    }
    if (uname(&u) == -1) strcpy(u.nodename, "?");
    if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, "?");
    char prompt[4200];
    int len = snprintf(prompt, sizeof(prompt), "%s@%s:%s$ ", user, u.nodename, cwd);
    write(STDOUT_FILENO, prompt, (size_t)len);
}

static void setup_terminal(int *terminal_fd, pid_t *shell_pgid) {
    if (!terminal_fd || !shell_pgid) return;
    if (isatty(*terminal_fd)) { 
        while (tcgetpgrp(*terminal_fd) != (*shell_pgid = getpgrp())) { 
            kill(-*shell_pgid, SIGTTIN); 
        }
        signal(SIGTTOU, SIG_IGN); // чтобы не убить терминал игнорируем остановку/убийство
        signal(SIGTTIN, SIG_IGN); 
        *shell_pgid = getpid();
        if (setpgid(*shell_pgid, *shell_pgid) == -1) { // делает процесс лидером своей группы
            perror("setpgid"); 
        }
        tcsetpgrp(*terminal_fd, *shell_pgid); // отдаем терминал этой группе
    }
}

static void setup_signal_handlers(void) {
    struct sigaction sa_chld; // ставим sigaction на sigchild
    memset(&sa_chld, 0, sizeof(sa_chld));
    sa_chld.sa_handler = sigchld_handler;
    sigemptyset(&sa_chld.sa_mask);
    sa_chld.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sa_chld, NULL);
    signal(SIGINT, sigint_handler); // ставим флаг при ctrl+c
    signal(SIGTSTP, SIG_IGN); // игнорируем ctrl+z
    signal(SIGQUIT, SIG_IGN); // игнорируем sigquit
}

static int repl_loop(void) {
    while (1) {
        jobs_handle_sigchld(); // смена статуса процесса в сооветствие с изменением флага
        jobs_notify(); // печать done при завершении процесса
        char buf[BUF_SIZE]; // создаем буфер
        size_t len = 0;
        buf[0] = '\0';
        const char *prompt = NULL;
        Token *tokens = NULL;

        while (1) {
            if (!prompt) {
                print_prompt();
            } else {
                write(STDOUT_FILENO, prompt, strlen(prompt)); // напечатали промпт если он есть, то есть >
            }

            if (len >= sizeof(buf) - 1) { // защита от переполнения
                len = 0;
                break;
            }

            ssize_t r = read(STDIN_FILENO, buf + len, sizeof(buf) - 1 - len); // читаем ввод
            if (r == 0) {
                write(STDOUT_FILENO, "\n", 1);
                return 0;
            }
            if (r < 0) {
                if (errno == EINTR) { // прерван сигналом
                    if (g_got_sigint) {
                        g_got_sigint = 0; // ctrl+c
                        write(STDOUT_FILENO, "\n", 1); // переход на строку
                        len = 0; 
                        buf[0] = '\0';
                        prompt = NULL;
                        break;
                    }
                    continue;
                }
                perror("read");
                return 1;
            }
            len += r; // буфер пополнила еще одна строка
            buf[len] = '\0';

            while (len > 0 && (buf[len - 1] == '\n')) { // удаляем переходы на строку в конце
                buf[--len] = '\0'; 
            }

            size_t trim = len;
            while (trim > 0 && isspace(buf[trim - 1])) trim--; // удаляем пробелы в конце
            if (trim > 0 && buf[trim - 1] == '\\') {
                len = trim - 1; // обработка многострочной команды
                buf[len] = '\0';
                prompt = "> ";
                continue;
            }

            tokens = create_tokens(buf); // массив токенов создали
            if (!tokens) {
                len = 0;
                prompt = NULL;
                break;
            }

            int need_more = 0; // незавершенный оператор или "\"
            int need_quotes = 0;
            const Token *last_token = NULL;
            for (int i = 0; tokens[i].type != TOKEN_EOF; ++i) {
                last_token = &tokens[i];
                if (tokens[i].type == TOKEN_ERROR) {
                    need_more = 1; // если у нас "\"
                    break;
                }
                if (tokens[i].type == TOKEN_INCOMPLETE_QUOTES) {
                    need_quotes = 1;
                    break;
                }
            }
            if (!need_more && !need_quotes && last_token && last_token->type == TOKEN_OP && last_token->op_type != OP_AMP) {
                need_more = 1; // оператор незавершен
            }
            if (!need_more && !need_quotes) {
                prompt = NULL;
                break;
            }

            free_tokens(tokens);
            tokens = NULL;
            prompt = "> ";
            if (!need_quotes && len < sizeof(buf) - 1 && len > 0 && !isspace(buf[len - 1])) {
                buf[len++] = ' '; // продолжаем писать в тот же буфер
                buf[len] = '\0';
            }
        }

        if (!tokens) continue;
        ASTNode *root = parse(tokens);
        if (!root) {
            free_tokens(tokens);
            continue;
        }
        int rc = execute(root);
        free_ast(root);
        free_tokens(tokens);
        jobs_handle_sigchld();
        jobs_notify();
        if (rc == -2) break; // exit || ctrl+D
    }
    return 0;
}

int main(void) {
    int terminal_fd = STDIN_FILENO; // фд управляющего терминала
    pid_t shell_pgid = getpid(); //
    setup_terminal(&terminal_fd, &shell_pgid);
    executor_set_interactive(terminal_fd, shell_pgid); 
    setup_signal_handlers();
    return repl_loop();
}
