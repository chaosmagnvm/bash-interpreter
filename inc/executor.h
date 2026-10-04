#pragma once

#include "lexer.h"
#include "parser.h"
#include "astnode.h"
#include "jobs.h"
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>


int exec_cmd(ASTNode *);
int exec_pipe(ASTNode *);
int execute_background(ASTNode *);
int execute(ASTNode*);
int open_redirects(ASTNode *);
void executor_set_interactive(int, pid_t);
char *token_to_str(const Token *);
