
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
    IR_PTRADD,   // Pointer addition (offset in bytes)

    // Arithmetic
    IR_ADD,
    IR_SUB,
    IR_MUL,
    IR_SDIV, // Signed division
    IR_UDIV, // Unsigned division
    IR_FDIV, // Floating point division
    IR_SMOD, // Signed modulo
    IR_UMOD, // Unsigned modulo
    IR_BIT_AND,
    IR_BIT_OR,
    IR_BIT_XOR,
    IR_SHL,
    IR_SAR, // Arithmetic right shift (for signed ints, fill with sign bit)
    IR_SHR, // Logical right shift (for unsigned ints, fill with zero)

    // Comparisons
    IR_EQ, IR_NEQ,
    IR_SLT, IR_SLE, IR_SGT, IR_SGE, // Signed comparison (for signed ints)
    IR_ULT, IR_ULE, IR_UGT, IR_UGE, // Unsigned comparison (for unsigned ints)
    IR_FLT, IR_FLE, IR_FGT, IR_FGE, // Floating point comparison

    // Conversions
    IR_TRUNC,
    IR_SEXT,    // Sign extend (for signed ints)
    IR_ZEXT,    // Zero extend (for unsigned ints)
    IR_PTR2I,   // Pointer -> integer
    IR_I2PTR,   // Integer -> pointer
    IR_BITCAST, // Pointer -> another pointer

    IR_FTRUNC, // Floating point truncation
    IR_FEXT,   // Floating point extension
    IR_FP2I,   // Floating point -> integer
    IR_I2FP,   // Integer -> floating point

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
    struct BB **bb;
    struct IrIns *ins; // Needed to generate PHIs
} BrChain;

typedef struct IrIns {
    struct IrIns *next, *prev;
    struct BB *bb;
    int op;
    IrType *t;
    union {
        // Constants and globals
        uint64_t imm; // IR_IMM
        struct { double fp; size_t fp_idx; /* for assembler */ }; // IR_FP
        struct Global *g; // IR_GLOBAL

        // Memory access
        size_t arg_idx; // IR_FARG
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
            Vec *preds; // of 'BB *'; predecessors
            Vec *defs;  // of 'IrIns *'; definitions, one for each predecessor
        };
        struct BB *br; // IR_BR
        struct { // IR_CONDBR
            struct IrIns *cond;
            struct BB *true, *false;
            Vec *true_chain, *false_chain; // of 'BrChain *'
        };
        struct IrIns *fn;  // IR_CALL
        struct IrIns *arg; // IR_CARG
        struct IrIns *ret; // IR_RET
    };
    int vreg; // For assembler
    size_t n; // For printing
} IrIns;

typedef struct BB {
    struct BB *next, *prev;
    IrIns *ir_head, *ir_last;
    struct AsmIns *asm_head, *asm_last;
    size_t n; // For printing

    // For assembler
    Vec *pred, *succ; // Control flow graph analysis
    int *live_in;     // Liveness analysis (all regs live-in at the BB's start)
} BB;

typedef struct {
    BB *entry, *last;

    // For assembler
    Vec *f32s, *f64s; // Per-function floating point constants
    int num_gprs, num_sse;
} Fn;

typedef struct {
    uint64_t offset;
    struct Global *val; // with k = G_IMM, G_FP
} InitElem;

enum {
    G_NONE,
    G_IMM,
    G_FP,
    G_INIT,
    G_PTR,
    G_FN_DEF,
};

typedef struct Global {
    int k;
    char *label;
    IrType *t;
    int linkage;
    union {
        uint64_t imm; // G_IMM
        double fp;    // G_FP
        Vec *elems;   // G_INIT; of 'InitElem *'
        struct { struct Global *g; int64_t offset; }; // G_PTR
        Fn *fn;       // G_FN
    };
} Global;

Vec * compile(AstNode *n); // of 'Global *'

#endif
