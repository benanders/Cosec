
#ifndef COSEC_ASSEMBLE_H
#define COSEC_ASSEMBLE_H

#include "compile.h"

enum { // General-purpose registers
    R_NONE,
    RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI,
    R8, R9, R10, R11, R12, R13, R14, R15,
    LAST_GPR, // Separates virtual and physical registers
};

enum { // SSE registers
    XMM0 = 1, XMM1, XMM2, XMM3, XMM4, XMM5, XMM6, XMM7, XMM8,
    XMM9, XMM10, XMM11, XMM12, XMM13, XMM14, XMM15,
    LAST_XMM,
};

enum { // Register size
    R0,  // Register not used
    R8L, // Lowest 8 bits (al)
    R8H, // High 8-bits of lowest 16 bits (ah)
    R16, // Lowest 16 bits (ax)
    R32, // Lowest 32 bits (eax)
    R64, // Full 64 bits (rax)
};

enum { // x86-64 opcodes
    // Memory access
    X64_MOV,
    X64_MOVSX,
    X64_MOVZX,
    X64_MOVSS,
    X64_MOVSD,
    X64_LEA,

    // Arithmetic
    X64_ADD,
    X64_SUB,
    X64_IMUL,
    X64_CWD,
    X64_CDQ,
    X64_CQO,
    X64_IDIV,
    X64_DIV,
    X64_AND,
    X64_OR,
    X64_XOR,
    X64_SHL,
    X64_SHR,
    X64_SAR,

    // Floating point arithmetic
    X64_ADDSS,
    X64_ADDSD,
    X64_SUBSS,
    X64_SUBSD,
    X64_MULSS,
    X64_MULSD,
    X64_DIVSS,
    X64_DIVSD,

    // Comparisons
    X64_CMP,
    X64_SETE,
    X64_SETNE,
    X64_SETL,
    X64_SETLE,
    X64_SETG,
    X64_SETGE,
    X64_SETB,
    X64_SETBE,
    X64_SETA,
    X64_SETAE,

    // Floating point comparisons
    X64_UCOMISS,
    X64_UCOMISD,

    // Floating point conversions
    X64_CVTSS2SD,
    X64_CVTSD2SS,
    X64_CVTSI2SS,
    X64_CVTSI2SD,
    X64_CVTTSS2SI,
    X64_CVTTSD2SI,

    // Stack manipulation
    X64_PUSH,
    X64_POP,

    // Control flow
    X64_JMP,
    X64_JE,
    X64_JNE,
    X64_JL,
    X64_JLE,
    X64_JG,
    X64_JGE,
    X64_JB,
    X64_JBE,
    X64_JA,
    X64_JAE,
    X64_CALL,
    X64_RET,
    X64_SYSCALL,

    X64_LAST, // For tables
};

enum { // Operand types
    OPR_IMM,   // Immediate
    OPR_F32,   // Per-function floating point constant (float)
    OPR_F64,   // (double or ldouble)
    OPR_GPR,   // General purpose register
    OPR_XMM,   // Floating point SSE register
    OPR_MEM,   // Memory access: [base + idx * scale + disp]
    OPR_BB,    // Label for a BB
    OPR_LABEL, // Arbitrary label
    OPR_DEREF, // Value at a label: [label]
};

typedef struct {
    int k;
    union {
        uint64_t imm; // OPR_IMM
        size_t fp;    // OPR_FP
        struct { int reg, size; }; // OPR_GPR, OPR_XMM
        struct {
            size_t bytes; // 1, 2, 4, or 8 bytes for memory access
            union {
                struct { // OPR_MEM
                    int base, base_size;
                    int idx, idx_size;
                    int scale; // 1, 2, 4, or 8
                    int64_t disp;
                };
                char *label; // OPR_LABEL, OPR_DEREF
            };
        };
        struct BB *bb; // OPR_BB
    };
} AsmOpr;

typedef struct AsmIns {
    struct AsmIns *next, *prev;
    struct BB *bb;
    int op;
    AsmOpr *l, *r;

    // For register allocator
    size_t n;
} AsmIns;

void assemble(Vec *globals);

// For register allocator to remove redundant 'mov's
void delete_asm(AsmIns *ins);

#endif
