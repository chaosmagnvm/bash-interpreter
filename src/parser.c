#include "parser.h"

void free_redirection_list(ASTRedirection *list) {
    while (list) {
        ASTRedirection *next = list->next;
        free(list);
        list = next;
    }
}

int check(OpType op, const Token *tokens, size_t *index) {
    if (tokens[*index].type!=TOKEN_OP) return 0;
    return tokens[*index].op_type == op;
}

ASTNode *parse(const Token *tokens) {
    size_t index = 0;
    return parse_list(tokens, &index);
}

ASTNode *parse_list(const Token *tokens, size_t *index) {
    ASTNode *left = parse_and_or(tokens, index);
    if (!left) return NULL;

    while (check(OP_SEMI, tokens, index) || check(OP_AMP, tokens, index)) {
        OpType op_type = tokens[*index].op_type;
        ++(*index);

        ASTNode *right = NULL;
        if (tokens[*index].type != TOKEN_EOF) {
            right = parse_and_or(tokens, index);
            if (!right) return NULL;
        } else if (op_type != OP_AMP) {
            fprintf(stderr, "parse: unexpected EOF\n");
            free_ast(left);
            return NULL;
        }

        if (op_type == OP_SEMI) {
            left = create_binary(left, right, AST_SEQ);
        } else { // OP_AMP
            left = create_binary(left, right, AST_BACK);
        }
    }
    return left;
}



ASTNode *parse_and_or(const Token *tokens, size_t *index) {
    ASTNode* left = parse_pipeline(tokens, index);
    if (!left) return NULL;
    OpType op_type;
    while(check(OP_AND, tokens, index) || check(OP_OR, tokens, index)) {
        op_type = tokens[*index].op_type;
        ++(*index);
        ASTNode *right = parse_pipeline(tokens, index);
        if (!right) return NULL;
        if (op_type == OP_AND) left = create_binary(left, right, AST_AND);
        if (op_type == OP_OR) left = create_binary(left, right, AST_OR);
    }
    return left;
}

ASTNode *parse_pipeline(const Token *tokens, size_t *index) {
    ASTNode* left = parse_command(tokens, index);
    if (!left) return NULL;
    OpType op_type;
    while(check(OP_PIPE, tokens, index) || check(OP_PIPE_BOTH, tokens, index)) {
        op_type = tokens[*index].op_type;
        ++(*index);
        ASTNode *right = parse_command(tokens, index);
        if (!right) return NULL;
        if (op_type == OP_PIPE_BOTH) left = create_binary(left, right, AST_PIPEBOTH);
        if (op_type == OP_PIPE) left = create_binary(left, right, AST_PIPELINE);
    }
    return left;
}

ASTNode *parse_command(const Token *tokens, size_t *index) {
    const Token **commands = (const Token**)malloc(sizeof(const Token*)*DEFAULT_WORDS_COUNT);
    if (!commands) {
        fprintf(stderr, "parse_command: malloc error\n"); return NULL;
    }
    size_t count = 0, capacity = DEFAULT_WORDS_COUNT;
    ASTRedirection *redirection_list = NULL;
    ASTRedirection *temp = NULL;
    if (tokens[*index].type == TOKEN_OP) {
        OpType t = tokens[*index].op_type;
        if (t != OP_ROUT && t != OP_APPEND && t != OP_RIN && t != OP_RDBOTH && t != OP_RDBOTH_APP) {
            fprintf(stderr, "parse_command: unexpected operator\n");
            free(commands);
            return NULL;
        }
    }
    while (tokens[*index].type == TOKEN_WORD || tokens[*index].type == TOKEN_OP) {
        if (tokens[*index].type == TOKEN_WORD) {
            if (count == capacity) {
                capacity *= 2;
                const Token **tmp = (const Token **)realloc(commands, sizeof(const Token *) * capacity);
                if (!tmp) {
                    fprintf(stderr, "parse_command: realloc error\n");
                    free(commands);
                    free_redirection_list(redirection_list);
                    return NULL;
                }
                commands = tmp;
            }
            commands[count++] = &tokens[*index];
            ++(*index);
            continue;
        }
        if (tokens[*index].type == TOKEN_OP) {
            OpType redirect_flag = OP_NOT_AN_OP;
            switch (tokens[*index].op_type) {
                case OP_ROUT:
                    redirect_flag = OP_ROUT;
                    break;
                case OP_APPEND:
                    redirect_flag = OP_APPEND;
                    break;
                case OP_RIN:
                    redirect_flag = OP_RIN;
                    break;
                case OP_RDBOTH:
                    redirect_flag = OP_RDBOTH;
                    break;
                case OP_RDBOTH_APP:
                    redirect_flag = OP_RDBOTH_APP;
                    break;
                default:
                    goto finish_loop;
            }

            int io = tokens[*index].io_number;
            ++(*index);

            if (tokens[*index].type != TOKEN_WORD) {
                fprintf(stderr, "parse_command: expected redirect target\n");
                free(commands);
                free_redirection_list(redirection_list);
                return NULL;
            }
            ASTRedirection *node = create_redir(io, redirect_flag, &tokens[*index]);
            if (!node) {
                fprintf(stderr, "parse_command: create_redir error\n");
                free(commands);
                free_redirection_list(redirection_list);
                return NULL;
            }
            node->next = NULL;
            if (!redirection_list) {
                redirection_list = node;
                temp = node;
            } else {
                temp->next = node;
                temp = node;
            }

            ++(*index);
            continue;
        }
    }
    finish_loop:
    return create_comm(redirection_list, commands, count);
}
