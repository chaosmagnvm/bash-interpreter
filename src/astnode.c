#include "astnode.h"

//число - редирект - цель, то есть редирект цепляет все именно так
ASTRedirection *create_redir(int io_num, OpType type, const Token *target) { // создается редирект
    ASTRedirection *node = (ASTRedirection *)malloc(sizeof(ASTRedirection));
    node->io_num = io_num;
    node->target = target;
    node->type = type;
    node->next = NULL;
    return node;
}

ASTNode *create_comm(ASTRedirection *redirs, const Token **words, size_t count) { // создается команда
    ASTNode *node = (ASTNode*)malloc(sizeof(ASTNode));
    node->data.commands.count = count;
    node->data.commands.capacity = count;
    node->data.commands.redirects = redirs;
    node->data.commands.words = words;
    node->type = AST_COMMAND;
    return node;
}

 ASTNode *create_binary(ASTNode *left, ASTNode *right, ASTType type) {
    ASTNode *node = (ASTNode*)malloc(sizeof(ASTNode));
    node->type = type;
    node->data.binary.left = left;
    node->data.binary.right = right;
    return node;
}

void print_list(ASTRedirection *node) {
    while(node) {
        //logic
        printf("redirection: io = %d, redirect(num in enum) = %d, target = %s", node->io_num, node->type, node->target->value);
        node = node->next;
    }
    putchar('\n');
}

void print_command(ASTCommands *comm) {
    printf("commands: ");
    for (size_t i=0; i<comm->count; ++i) {
        printf("%s ", comm->words[i]->value);
    }
    print_list(comm->redirects);
}

void print_ast(ASTNode* root) {
    if (!root) return;
    if (root->type == AST_COMMAND) return;
    if (root->type) {
        if (root->data.binary.left->type == AST_COMMAND) {
            print_command(&(root->data.binary.left->data.commands));
        } else {
            print_ast(root->data.binary.left);
        }
        printf("OPERATOR IN ENUM: %d\n", root->type); 
        if (root->data.binary.right->type == AST_COMMAND) {
            print_command(&(root->data.binary.right->data.commands));
        } else {
            print_ast(root->data.binary.right);
        }
    }
}

static void free_redirect_list(ASTRedirection *node) {
    while (node) {
        ASTRedirection *next = node->next;
        free(node);
        node = next;
    }
}

void free_ast(ASTNode *root) {
    if (!root) return;
    if (root->type == AST_COMMAND) {
        free_redirect_list(root->data.commands.redirects);
        free(root->data.commands.words);
        free(root);
        return;
    }
    free_ast(root->data.binary.left);
    free_ast(root->data.binary.right);
    free(root);
}
