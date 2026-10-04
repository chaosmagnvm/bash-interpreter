#pragma once

#include <stdio.h>
#include <stdlib.h>

#include "token.h"
#include "astnode.h"

#define DEFAULT_WORDS_COUNT 16

ASTNode *parse(const Token *tokens);
ASTNode *parse_list(const Token *tokens, size_t *index);
ASTNode *parse_and_or(const Token *tokens, size_t *index);
ASTNode *parse_pipeline(const Token *tokens, size_t *index);
ASTNode *parse_command(const Token *tokens, size_t *index);
void free_redirection_list(ASTRedirection *list);
int check(OpType op, const Token *tokens, size_t *index);
