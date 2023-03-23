
#ifndef COSEC_COMPILE_H
#define COSEC_COMPILE_H

#include "parse.h"

// TODO: consider getting rid of the concept of an array from the bytecode and
// just dealing with pointers; IR_ALLOC takes a size as an argument and returns
// a pointer
// May make array bounds checking and optimisations that depend on this
// harder...
// Could we tag pointers with size information when known? this is checked on
// emitting IR_ELEM and any pointer returned by IR_ELEM gets its size adjusted
// accordingly so we know when we're performing loads/stores to out of bound
// pointers (for alias analysis)

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
    Type *t;
    char *label;
    Node *val; // One of N_IMM, N_FP, N_STR, N_INIT, N_KPTR
    IrFn *fn;  // One of 'val' or 'fn' is NULL
} Global;

Vec * compile(Node *n);

#endif
