
#ifndef COSEC_COMPILE_H
#define COSEC_COMPILE_H

#include "parse.h"

enum {
    // Constants, globals, and functions
    IR_IMM,
    IR_FP,
    IR_GLOBAL,

    // Memory access
    IR_FARG,  // Only at START of entry BB; references 'n'th argument
    IR_ALLOC, // Allocates stack space similar to LLVM's 'alloca'
    IR_LOAD,
    IR_STORE,
    IR_ELEM, // Pointer offset calculations similar to LLVM's 'getelementptr'

    // Arithmetic
    IR_ADD,
    IR_SUB,
    IR_MUL,
    IR_DIV,
    IR_MOD,
    IR_BIT_AND,
    IR_BIT_OR,
    IR_BIT_XOR,
    IR_SHL,
    IR_SHR,

    // Comparisons
    IR_EQ,
    IR_NEQ,
    IR_LT,
    IR_LE,
    IR_GT,
    IR_GE,

    // Conversions
    IR_TRUNC,
    IR_EXT,
    IR_FP2I,    // Floating point -> integer
    IR_I2FP,    // Integer -> floating point
    IR_PTR2I,   // Pointer -> integer
    IR_I2PTR,   // Integer -> pointer
    IR_BITCAST, // Pointer -> another pointer

    // Control flow
    IR_PHI,
    IR_BR,     // Unconditional branch
    IR_CONDBR, // Conditional branch
    IR_CALL,
    IR_CARG,   // Immediately after IR_CALL
    IR_RET,

    IR_LAST, // For tables indexed by opcode
};

typedef struct IrIns {
    struct IrIns *next, *prev;
    struct IrBB *bb;
    int op;
    Type *t;
    union {
        // Constants and globals
        uint64_t imm; // IR_IMM
        struct { double fp; size_t fp_idx; }; // IR_FP
        struct Global *g; // IR_GLOBAL

        // Memory access
        size_t arg_num; // IR_FARG
        int stack_slot; // IR_ALLOC (for assembler)
        struct { struct IrIns *src, *dst; };     // IR_LOAD, IR_STORE
        struct { struct IrIns *base, *offset; }; // IR_ELEM

        // Unary and binary operations
        struct { struct IrIns *l, *r; };

        // Control flow
        struct { // IR_PHI
            Vec *preds; // of 'IrBB *'; predecessors
            Vec *defs;  // of 'IrIns *'; definitions, one for each predecessor
        };
        struct IrBB *br;  // IR_BR
        struct {   // IR_CONDBR
            struct IrIns *cond;
            struct IrBB *true, *false;
            Vec *true_chain, *false_chain; // of 'IrBB **'
        };
        struct IrIns *fn;  // IR_CALL
        struct IrIns *arg; // IR_CARG
        struct IrIns *val; // IR_RET
    };
    int vreg; // For the assembler
    int idx;  // For printing
} IrIns;

typedef struct IrBB {
    struct IrBB *next, *prev;
    IrIns *head, *last;
    int idx; // For printing
} IrBB;

typedef struct {
    IrBB *entry, *last;
} IrFn;

enum {
    K_INT,
    K_FP,
    K_STR,   // UTF-8
    K_STR16, // UTF-16
    K_STR32, // UTF-32
    K_PTR,
    K_FN_DEF,
};

typedef struct Global {
    Type *t;
    char *label;
    int k;
    union {
        int64_t i; // K_INT
        double f;  // K_FP
        struct {   // K_STR, K_STR16, K_STR32
            union { char *s; uint16_t *s16; uint32_t *s32; };
            size_t len;
        };
        struct { struct Global *ptr; int64_t offset; }; // K_PTR
        IrFn *fn; // K_FN_DEF
    };
} Global;

Vec * compile(Node *ast);

#endif
