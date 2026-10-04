#pragma once

#include <stdlib.h>
#include <stdio.h>

#include "token.h"

typedef enum {
    AST_COMMAND, // COMMAND
    AST_PIPELINE, // |
    AST_PIPEBOTH, // |&
    AST_AND, // &&
    AST_OR, // ||
    AST_SEQ, // ;
    AST_BACK // &
} ASTType;

typedef struct ASTNode ASTNode;

typedef struct ASTRedirection {
    int io_num;
    OpType type;
    const Token *target;
    struct ASTRedirection *next;
} ASTRedirection;

typedef struct ASTCommands {
    const Token **words; // массив указателей на команды и аргументы
    size_t count;
    size_t capacity;
    ASTRedirection *redirects; //список редир
} ASTCommands;

typedef struct ASTBinary {
            ASTNode *left, *right;
        } ASTBinary;

typedef struct ASTNode {
    ASTType type;
    union {
        ASTCommands commands;
        ASTBinary binary;
    } data;
} ASTNode;

ASTRedirection *create_redir(int, OpType, const Token *);
ASTNode *create_comm(ASTRedirection *, const Token **, size_t);
ASTNode *create_binary(ASTNode *, ASTNode *, ASTType);
void print_list(ASTRedirection *);
void print_command(ASTCommands *);
void print_ast(ASTNode*);
void free_ast(ASTNode*);
