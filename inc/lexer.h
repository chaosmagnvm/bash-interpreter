#pragma once

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

#include "token.h"

#define DEF_SIZE 32
#define OPERATORS_COUNT 11

char *copy_range(const char*, size_t);
Token scan_operator(char*, int*);
Token scan_io_number(char*, int*);
Token scan_word(char*, int*);
Token scan(char*, int*);
Token *create_tokens(char*);
void free_tokens(Token*);
