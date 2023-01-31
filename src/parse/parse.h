
#ifndef COSEC_PARSE_H
#define COSEC_PARSE_H

#include "../type.h"
#include "lex.h"

enum { // AST nodes
    // Constants and variables
    N_IMM,
    N_FP,
    N_STR,
    N_ARR,
    N_INIT,
    N_LOCAL,
    N_GLOBAL,
    N_KPTR, // Constant pointer to a 'static' variable
    N_KVAL, // Used by the constant expression calculator only

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

    // Postfix operations
    N_IDX,
    N_CALL,
    N_FIELD,

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
        // Constants and variables
        uint64_t imm; // N_IMM
        double fp;    // N_FP
        struct { char *str; size_t len; }; // N_STR
        Vec *inits; /* of N_INIT */ // N_ARR
        struct { struct Node *init_val; uint64_t init_offset; }; // N_INIT
        char *var_name; // N_LOCAL, N_GLOBAL, N_TYPEDEF
        struct { struct Node *global; /* to N_GLOBAL */ int64_t kptr_offset; }; // N_KPTR, N_KVAL

        // Operations
        struct { struct Node *l, *r; }; // Unary, binary
        struct { struct Node *arr, *idx; }; // N_IDX
        struct { struct Node *fn; Vec *args; /* of 'Node *' */ }; // N_CALL
        struct { struct Node *strct; char *field_name; }; // N_FIELD

        // Statements
        struct { char *fn_name; Vec *param_names; struct Node *fn_body; }; // N_FN_DEF
        struct { struct Node *var, *init; }; // N_DECL
        struct { struct Node *if_cond, *if_body, *if_else; }; // N_IF, N_TERNARY
        struct { struct Node *loop_cond, *loop_body; }; // N_WHILE, N_DO_WHILE
        struct { struct Node *for_init, *for_cond, *for_inc, *for_body; }; // N_FOR
        struct { struct Node *switch_cond, *switch_body; Vec *cases; /* of 'Node *' */ }; // N_SWITCH
        struct { struct Node *case_cond, *case_body; }; // N_CASE, N_DEFAULT
        struct { char *label; struct Node *label_body; }; // N_GOTO, N_LABEL
        struct Node *ret_val; // N_RET
    };
} Node;

Node * parse(char *path);

#endif
