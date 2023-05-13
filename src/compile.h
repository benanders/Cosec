
#ifndef COSEC_COMPILE_H
#define COSEC_COMPILE_H

// TODO: move all constant IR_ALLOCs to the start of the function so only
// dynamically sized allocations (that modify rsp) come after. So allocating
// a static-sized variable after a dynamically-sized one doesn't require
// modification of the stack pointer

#include "parse.h"

typedef struct {
    struct IrType *t;
    size_t offset;
} IrField;

enum {
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
        Vec *fields; // IRT_STRUCT; of 'IrField *'
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
    IR_COPY,
    IR_ZERO,
    IR_IDX,   // Pointer addition (offset in bytes)

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
        struct { double fp; size_t fp_idx; /* for assembler */ }; // IR_FP
        struct Global *g; // IR_GLOBAL

        // Memory access
        size_t arg_num; // IR_FARG
        struct { // IR_ALLOC
            IrType *alloc_t;
            struct IrIns *count;
            size_t stack_slot; // For assembler
        };
        struct { struct IrIns *src, *dst, *len; }; // IR_LOAD, IR_STORE, IR_COPY
        struct { struct IrIns *ptr, *size; };      // IR_ZERO
        struct { struct IrIns *base, *offset; };   // IR_IDX

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
        struct IrIns *ret; // IR_RET
    };
    int vreg; // For assembler
    int n;    // For printing
} IrIns;

typedef struct IrBB {
    struct IrBB *next, *prev;
    IrIns *head, *last;
    int n; // For printing
} IrBB;

typedef struct {
    IrBB *entry, *last;
} IrFn;

typedef struct Global {
    char *label;
    AstNode *val; // NULL if fn def; or N_IMM, N_FP, N_STR, N_INIT, N_KPTR
    IrFn *ir_fn;
    struct AsmFn *asm_fn;
} Global;

Vec * compile(AstNode *n);

#endif
