#include "lexer.h"

const char* operators[] = {"&>>", ">>", "&>", "||", "&&", "|&", "|", "&", ";", ">", "<"};

char *copy_range(const char *src, size_t len) {
    char *buf = (char *)malloc(len + 1);
    if (!buf) return NULL;
    for (size_t i = 0; i<len; ++i) {
        buf[i]=src[i];
    }
    buf[len] = '\0';
    return buf;
}

Token scan_operator(char* str, int *index) {
    Token token = {0};
    token.start = *index;
    token.type = TOKEN_OP;
    token.io_number = -1;
    for (int i = 0; i<OPERATORS_COUNT; ++i) {
        if (strncmp(str + *index, operators[i], strlen(operators[i]))==0) {
            token.value = copy_range(operators[i], strlen(operators[i]));
            if (!token.value) {
                token.type = TOKEN_ERROR;
                token.value = "error: memory allocation failed\n";
                return token;
            }
            token.op_type = i;
            *index += strlen(operators[i]);
            token.end = *index;
            return token;
        }
    }
    token.op_type = OP_NOT_AN_OP;
    token.end = *index;
    return token;
}

Token scan_io_number(char* str, int *index) { //осталось доработать приклеивание редиректа
    int start = *index;
    int i = *index;
    while (isdigit((unsigned char)str[i])) ++i;
    if (i == start) {
        return scan_word(str, index);
    }
    int num = 0;
    for (int k = start; k < i; ++k) {
        num = num * 10 + (str[k] - '0');
    }
    *index = i;
        while (str[*index] && isspace((unsigned char)str[*index])) ++(*index);
    Token op = scan_operator(str, index);
    if (op.type == TOKEN_OP &&
        (op.op_type == OP_ROUT || op.op_type == OP_APPEND ||
         op.op_type == OP_RIN || op.op_type == OP_RDBOTH ||
         op.op_type == OP_RDBOTH_APP)) {
        op.io_number = num;
        op.start = start;
        return op;
    }
    *index = start;
    Token token = scan_word(str, index);
    return token;
}

Token scan_word(char *str, int *index) {
    Token token = {0};
    token.start = *index;
    token.type = TOKEN_WORD;
    token.io_number = -1;

    int i = *index;
    int n = DEF_SIZE;
    int buf_i = 0;
    char *buf = malloc(n*sizeof(char));
    if (!buf) {
        token.type = TOKEN_ERROR;
        token.value = "scan_word: malloc error\n";
        return token;
    }

    if (str[i] == '"') {
        ++i;
        while (str[i] && str[i] != '"') {
            if (str[i] == '\\') {                    
                if (!str[i + 1]) {
                    free(buf);
                    token.type = TOKEN_INCOMPLETE_QUOTES;
                    token.value = NULL;
                    token.io_number = 22;
                    token.end = i;
                    *index = i;
                    return token;
                }
                if (str[i + 1] == '\n') {
                    i += 2;
                    continue;
                }
                if (str[i + 1] == '"' || str[i + 1] == '\\' || str[i + 1] == '$' || str[i + 1] == '`') {
                    if (buf_i == n - 1) {
                        n *= 2;
                        buf = realloc(buf, n);
                    }
                    buf[buf_i++] = str[i + 1];
                    i += 2;
                    continue;
                }
                if (buf_i == n - 1) {
                    n *= 2;
                    buf = realloc(buf, n);
                }
                buf[buf_i++] = '\\';
                str[i] = str[i + 1];
                ++i;
            }

            if (buf_i == n-1) {
                n *= 2;
                buf = realloc(buf, n);
            }
            buf[buf_i++] = str[i++];
        }
        if (str[i] != '"') {
            free(buf);
            token.type = TOKEN_INCOMPLETE_QUOTES;
            token.value = NULL;
            token.io_number = 22;
            token.end = i;
            *index = i;
            return token;
        }
        buf[buf_i] = '\0';
        token.value = buf;
        token.double_quotes = 1;
        token.single_quotes = 0;
        *index = i + 1;
        token.end = *index;
        return token;
    }

    if (str[i] == '\'') {
        ++i;
        while (str[i] && str[i] != '\'') {
            if (buf_i == n-1) {
                n *= 2;
                buf = realloc(buf, n);
            }
            buf[buf_i++] = str[i++];
        }
        if (str[i] != '\'') {
            free(buf);
            token.type = TOKEN_INCOMPLETE_QUOTES;
            token.value = NULL;
            token.io_number = 11;
            token.end = i;
            *index = i;
            return token;
        }
        buf[buf_i] = '\0';
        token.value = buf;
        token.single_quotes = 1;
        token.double_quotes = 0;
        *index = i + 1;
        token.end = *index;
        return token;
    }

    while (str[i] && !isspace((unsigned char)str[i])) {
        if (str[i] == '\\') {
            if (!str[i + 1] || str[i + 1] == '\n') {
                free(buf);
                token.type = TOKEN_ERROR;
                token.value = "error: no symbol after '\\'";
                return token;
            }
            ++i;
        } else {
            for (int j = 0; j < OPERATORS_COUNT; ++j) {
                size_t op_len = strlen(operators[j]);
                if (strncmp(str + i, operators[j], op_len) == 0) {
                    if (operators[j][0] == '&' && str[i+op_len] != '\0' && !isspace((unsigned char)str[i+op_len])) {
                        break;
                    }
                    if (buf_i == 0) {
                        return scan_operator(str, index);
                    }
                    buf[buf_i] = '\0';
                    token.value = buf;
                    token.end = i;
                    *index = i;
                    return token;
                }
            }
        }

        if (buf_i == n-1) {
            n *= 2;
            buf = realloc(buf, n);
        }
        buf[buf_i++] = str[i++];
    }

    buf[buf_i] = '\0';
    int digits_only = buf_i;
    for (int k = 0; k < buf_i; ++k) {
        if (!isdigit((unsigned char)buf[k])) {
            digits_only = 0;
            break;
        }
    }
    if (digits_only) {
        int ii = i;
        while (str[ii] && isspace((unsigned char)str[ii])) ++ii;
        for (int j = 0; j < OPERATORS_COUNT; ++j) {
            size_t len = strlen(operators[j]);
            if (strncmp(str + ii, operators[j], len) == 0) {
                if (operators[j][0] == '>' || operators[j][0] == '<') {
                    free(buf);
                    *index = token.start;
                    return scan_io_number(str, index);
                }
            }
        }
    }
    token.value = buf;
    token.end = i;
    *index = i;
    return token;
}


Token scan(char *str, int *index) {
    if (isdigit(str[*index])) { // фд файла + редирект
        int cntr = 0;
        int i = *index;
        while (isdigit(str[i++])) ++cntr;

        int next = *index + cntr;
        if (strncmp(str + next, "&>>", 3) == 0 ||
            strncmp(str + next, "&>", 2) == 0  ||
            strncmp(str + next, ">>", 2) == 0  ||
            strncmp(str + next, ">", 1) == 0   ||
            strncmp(str + next, "<", 1) == 0) {
            return scan_io_number(str, index);
        }
    }

    for (int i = 0; i < OPERATORS_COUNT; ++i) { // если попался оператор
        if (strncmp(str + *index, operators[i], strlen(operators[i])) == 0)
            return scan_operator(str, index);
    }
    return scan_word(str, index);
}


Token *create_tokens(char* str) {
    Token *tokens = (Token*)malloc(sizeof(Token)*DEF_SIZE);
    int index = 0, i = 0, n = DEF_SIZE;
    while(str[index]!='\0') {
        if (i == n-1) {
            n+=DEF_SIZE;
            tokens = realloc(tokens, sizeof(Token)*n);
        }
        while (str[index] && isspace((unsigned char)str[index])) ++index;
        if (str[index] == '\0') break;
        if (str[index]=='#') break;
        tokens[i++] = scan(str, &index);
        if (tokens[i-1].type == TOKEN_ERROR) {
            printf("%s", tokens[i-1].value);
            return NULL;
        }
    }
    Token token;
    token.type = TOKEN_EOF;
    token.op_type = OP_NOT_AN_OP;
    if (i > 0 && tokens[i-1].type==TOKEN_OP && tokens[i-1].op_type!=OP_AMP) {
        if (tokens[i-1].op_type != OP_AMP) {
            token.type = TOKEN_ERROR;
            token.value = "op";
            tokens[i++]=token;
            tokens[i].type = TOKEN_EOF;
            return tokens;
        }
    }
    if (i > 0 && tokens[i-1].type == TOKEN_INCOMPLETE_QUOTES) {
        tokens[i].type = TOKEN_EOF;
        tokens[i].op_type = OP_NOT_AN_OP;
        tokens[i].value = "NEWLINE";
        return tokens;
    }
    token.value = "NEWLINE";
    tokens[i]=token;
    return tokens;
}

void free_tokens(Token *tokens) {
    if (!tokens) return;
    for (int i = 0; tokens[i].type != TOKEN_EOF; ++i) {
        if (tokens[i].value && tokens[i].type != TOKEN_ERROR) {
            free((char*)tokens[i].value);
        }
    }
    free(tokens);
}
