#pragma once

typedef enum {
    TOKEN_WORD, // команда/путь/аргумент
    TOKEN_OP, // оператор, пайп и т.д
    TOKEN_EOF, // конец команды
    TOKEN_ERROR, // ошибка
    TOKEN_INCOMPLETE_QUOTES
} TokenType;

typedef enum {
    OP_RDBOTH_APP, // &>>
    OP_APPEND, // >>
    OP_RDBOTH, // &>
    OP_OR, //  ||
    OP_AND, //  &&
    OP_PIPE_BOTH, //  |&
    OP_PIPE, //  |
    OP_AMP, //  &     
    OP_SEMI, // ;
    OP_ROUT, // >
    OP_RIN, // <
    OP_NOT_AN_OP
} OpType;

typedef struct Token {
    const char* value; // строка, ну то есть сама команда/оператор и т.д
    TokenType type; // тип токена - слово, оператор, еоф и т.д
    OpType op_type; // если оператор, то его тип - пайп, и/или и т.д
    int io_number; // число 
    int single_quotes; // ' '
    int double_quotes; // " "
    int start; int end; // полуинтервал, [start, end)
} Token;