
#ifndef COSEC_PARSE_H
#define COSEC_PARSE_H

#include "pp.h"

enum {
    S_NONE,
    S_TYPEDEF,
    S_EXTERN,
    S_STATIC,
    S_AUTO,
    S_REGISTER,
};

enum {
    T_CONST    = 0b001,
    T_RESTRICT = 0b010,
    T_VOLATILE = 0b100,
};

enum {
    F_INLINE = 1,
};

enum {
    L_NONE,
    L_STATIC,
    L_EXTERN,
};

typedef struct {
    struct AstType *t;
    char *name;
    size_t offset;
} Field;

typedef struct {
    char *name;
    uint64_t val;
} EnumConst;

enum {
    T_VOID = 1,
    T_CHAR,
    T_SHORT,
    T_INT,
    T_LONG,
    T_LLONG,
    T_FLOAT,
    T_DOUBLE,
    T_LDOUBLE,

    T_PTR,
    T_ARR,
    T_FN,
    T_STRUCT,
    T_UNION,
    T_ENUM,
};

typedef struct AstType {
    int k;
    int linkage;
    size_t size, align;
    union {
        int is_unsigned;  // T_CHAR to T_LLONG
        struct AstType *ptr; // T_PTR
        struct { // T_ARR
            struct AstType *elem;
            struct AstNode *len; // VLA if len->k != N_IMM
        };
        struct { // T_FN
            struct AstType *ret;
            Vec *params; // of 'Type *'
            int is_vararg;
        };
        Vec *fields; // of 'Field *'; T_STRUCT, T_UNION
        struct {  // T_ENUM
            Vec *consts; // of 'EnumConst *'
            struct AstType *num_t;
        };
    };
} AstType;

enum {
    // Constants and variables
    N_IMM,
    N_FP,
    N_STR,
    N_INIT, // Array/struct/union initializer
    N_LOCAL,
    N_GLOBAL,
    N_KVAL, // Used by the constant expression parser
    N_KPTR,

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

typedef struct AstNode {
    struct AstNode *next;
    int k;
    AstType *t;
    Token *tk;
    union {
        // Constants and variables
        uint64_t imm; // N_IMM
        double fp;    // N_FP
        struct {      // N_STR
            union { char *str; uint16_t *str16; uint32_t *str32; };
            size_t len;
            int enc;
        };
        Vec *elems;     // N_INIT (array/struct initializer); of 'Node *'
        char *var_name; // N_LOCAL, N_GLOBAL, N_TYPEDEF
        struct { struct AstNode *g; /* N_GLOBAL */ int64_t offset; }; // N_KVAL, N_KPTR

        // Operations
        struct { struct AstNode *l, *r; }; // Unary and binary operations
        struct { struct AstNode *fn; Vec *args; /* of 'Node *' */ }; // N_CALL
        struct { struct AstNode *obj; size_t field_idx; }; // N_FIELD

        // Statements
        struct { struct AstNode *var, *val; }; // N_DECL
        struct {
            struct AstNode *body, *cond; // N_WHILE, N_DO_WHILE, N_CASE
            union {
                struct { char *fn_name; Vec *param_names; /* of 'Token *' */ }; // N_FN_DEF
                struct AstNode *els; // N_IF, N_TERNARY
                struct { struct AstNode *init, *inc; }; // N_FOR
                Vec *cases;  // N_SWITCH
                char *label; // N_GOTO, N_LABEL
            };
        };
        struct AstNode *ret; // N_RET
    };
} AstNode;

AstNode * parse(File *f);

// Used by the preprocessor for '#if' directives
int64_t parse_const_int_expr(PP *pp);

#endif
