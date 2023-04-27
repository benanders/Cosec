
#ifndef COSEC_COMPILE_H
#define COSEC_COMPILE_H

// TODO: move all constant IR_ALLOCs to the start of the function so only
// dynamically sized allocations (that modify rsp) come after. So allocating
// a static-sized variable after a dynamically-sized one doesn't require
// modification of the stack pointer

#include "parse.h"

enum {
    IRT_VOID,
    IRT_I8,
    IRT_I16,
    IRT_I32,
    IRT_I64,
    IRT_F32,
    IRT_F64,
    IRT_PTR,
    IRT_ARR,
    IRT_STRUCT,
};

typedef struct IrType {
    int k;
    size_t size, align;
    union {
        struct { struct IrType *elem; size_t len; }; // IRT_ARR
        Vec *fields; // IRT_STRUCT; of 'IrType *'
    };
} IrType;

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

    // Intrinsics
    IR_ZERO,
    IR_COPY,

    IR_LAST, // For tables indexed by opcode
};

typedef struct {
    struct IrBB **bb;
    struct IrIns *ins; // Needed to generate PHIs
} BrChain;

typedef struct IrIns {
    struct IrIns *next, *prev;
    struct IrBB *bb;
    int op;
    IrType *t;
    union {
        // Constants and globals
        uint64_t imm; // IR_IMM
        struct { double fp; size_t fp_idx; }; // IR_FP
        struct Global *g; // IR_GLOBAL

        // Memory access
        size_t arg_num; // IR_FARG
        struct { struct IrIns *count; int stack_slot; /* for assembler */ }; // IR_ALLOC
        struct { struct IrIns *src, *dst; };     // IR_LOAD, IR_STORE
        struct { struct IrIns *base, *offset; }; // IR_ELEM

        // Unary and binary operations
        struct { struct IrIns *l, *r; };

        // Control flow
        struct { // IR_PHI
            Vec *preds; // of 'IrBB *'; predecessors
            Vec *defs;  // of 'IrIns *'; definitions, one for each predecessor
        };
        struct IrBB *br; // IR_BR
        struct { // IR_CONDBR
            struct IrIns *cond;
            struct IrBB *true, *false;
            Vec *true_chain, *false_chain; // of 'BrChain *'
        };
        struct IrIns *fn;  // IR_CALL
        struct IrIns *arg; // IR_CARG
        struct IrIns *val; // IR_RET

        // Intrinsics
        struct { struct IrIns *ptr, *size; }; // IR_ZERO
        struct { struct IrIns *cpy_src, *cpy_dst, *cpy_size; }; // IR_COPY
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

typedef struct Global {
    char *label;
    Node *val; // NULL if fn def; or one of N_IMM, N_FP, N_STR, N_INIT, N_KPTR
    IrFn *fn;
} Global;

Vec * compile(Node *n);

#endif
