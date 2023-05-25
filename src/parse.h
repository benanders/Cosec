
#ifndef COSEC_PARSE_H
#define COSEC_PARSE_H

#include "pp.h"

enum { // Storage classes
    SC_NONE,
    SC_TYPEDEF,
    SC_EXTERN,
    SC_STATIC,
    SC_AUTO,
    SC_REGISTER,
};

enum { // Type qualifiers
    TQ_CONST    = 0b001,
    TQ_RESTRICT = 0b010,
    TQ_VOLATILE = 0b100,
};

enum { // Function specifiers
    FS_INLINE = 1,
};

enum { // Linkage
    LINK_NONE,
    LINK_STATIC,
    LINK_EXTERN,
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
            struct AstNode *len;   // VLA if len->k != N_IMM
            struct IrIns *vla_len; // Length of VLA at init (for compiler)
        };
        struct { // T_FN
            struct AstType *ret;
            Vec *params; // of 'AstType *'
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
    N_INIT_ELEM,
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
        Vec *elems; // N_INIT; of 'AstNode *' with k = N_INIT_ELEM
        struct {    // N_INIT_ELEM
            uint64_t desg; // Byte offset into initializer
            struct AstNode *elem;
        };
        char *var_name; // N_LOCAL, N_GLOBAL, N_TYPEDEF
        struct {        // N_KVAL, N_KPTR
            struct AstNode *g; // with k = N_GLOBAL
            int64_t offset;
        };

        // Operations
        struct { struct AstNode *l, *r; }; // Unary and binary operations
        struct { // N_CALL
            struct AstNode *fn;
            Vec *args; // of 'AstNode *'
        };
        struct { // N_FIELD
            struct AstNode *obj;
            size_t field_idx;
        };

        // Statements
        struct { // N_FN_DEF
            char *fn_name;
            Vec *param_names; // of 'Token *'
            struct AstNode *fn_body;
        };
        struct { // N_DECL
            struct AstNode *var; // with k = N_LOCAL, N_GLOBAL
            struct AstNode *val;
        };
        struct { // N_IF, N_TERNARY
            struct AstNode *if_cond, *if_body;
            struct AstNode *if_else; // For else-ifs; with k = N_IF
        };
        struct { // N_WHILE, N_DO_WHILE
            struct AstNode *loop_cond, *loop_body;
        };
        struct { // N_FOR
            struct AstNode *for_init, *for_cond, *for_inc, *for_body;
        };
        struct { // N_SWITCH
            struct AstNode *switch_cond, *switch_body;
            Vec *cases; // of 'AstNode *' with k = N_CASE
            struct AstNode *default_n; // with k = N_DEFAULT
        };
        struct { // N_CASE, N_DEFAULT
            struct AstNode *case_cond; // NULL if k = N_DEFAULT
            struct AstNode *case_body;
            struct BB **case_br; // For the compiler
        };
        struct { char *goto_label; }; // N_GOTO
        struct { // N_LABEL
            char *label;
            struct AstNode *label_body;
        };
        struct AstNode *ret; // N_RET
    };
} AstNode;

AstNode * parse(File *f);

// Used by the compiler to handle VLAs separately
int is_vla(AstType *t);

// Used by the preprocessor for '#if' directives
int64_t parse_const_int_expr(PP *pp);

#endif
