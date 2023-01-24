
#ifndef COSEC_PARSE_H
#define COSEC_PARSE_H

#include "../type.h"
#include "lex.h"

enum { // AST nodes
    // Constants and variables
    N_IMM,
    N_FP,
    N_STR,
    N_VAR,

    // Arithmetic
    N_ADD,
    N_SUB,
    N_MUL,
    N_DIV,
    N_MOD,
    N_BIT_AND,
    N_BIT_OR,
    N_BIT_XOR,
    N_SHL,
    N_SHR,

    // Comparisons
    N_EQ,
    N_NEQ,
    N_LT,
    N_LE,
    N_GT,
    N_GE,
    N_LOG_AND,
    N_LOG_OR,

    // Assignment
    N_ASSIGN,
    N_A_ADD,
    N_A_SUB,
    N_A_MUL,
    N_A_DIV,
    N_A_MOD,
    N_A_BIT_AND,
    N_A_BIT_OR,
    N_A_BIT_XOR,
    N_A_SHL,
    N_A_SHR,

    // Other operations
    N_COMMA,
    N_TERNARY,

    // Unary operations
    N_NEG,
    N_BIT_NOT,
    N_LOG_NOT,
    N_PRE_INC,
    N_PRE_DEC,
    N_POST_INC,
    N_POST_DEC,
    N_DEREF,
    N_ADDR,
    N_CONV,
    N_SIZEOF,

    // Postfix operations
    N_IDX,
    N_CALL,
    N_DOT,
    N_ARROW,

    // Statements
    N_FN_DEF,
    N_TYPEDEF,
    N_DECL,
    N_IF,
    N_WHILE,
    N_DO_WHILE,
    N_FOR,
    N_SWITCH,
    N_CASE,
    N_DEFAULT,
    N_BREAK,
    N_CONTINUE,
    N_GOTO,
    N_LABEL,
    N_RET,

    N_LAST,
};

typedef struct Node {
    struct Node *next;
    int k;
    Type *t;
    Token *tk;
    union {
        // Constants
        uint64_t imm; // N_IMM
        double fp;    // N_FP
        struct { char *str; int len; }; // N_STR

        // Variables
        char *var_name; // N_VAR, N_TYPEDEF

        // Operations
        struct { struct Node *l, *r; };
        struct { struct Node *fn; Vec *args; /* of 'Node *' */ }; // N_CALL

        // Statements
        struct { char *fn_name; Vec *param_names; struct Node *fn_body; }; // N_FN_DEF
        struct { struct Node *var, *init; }; // N_DECL
        struct { struct Node *if_cond, *if_body, *if_els; }; // N_IF
        struct { struct Node *loop_cond, *loop_body; }; // N_WHILE, N_DO_WHILE
        struct { struct Node *for_init, *for_cond, *for_inc, *for_body; }; // N_FOR
        struct { struct Node *switch_cond, *switch_body; }; // N_SWITCH
        struct { struct Node *case_cond, *case_body; }; // N_CASE, N_DEFAULT
        struct { char *label; struct Node *label_body; }; // N_GOTO, N_LABEL
        struct Node *val; // N_RET
    };
} Node;

Node * parse(char *path);

#endif
