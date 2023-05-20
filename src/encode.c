
#include <inttypes.h>

#include "encode.h"

#define BB_PREFIX  "._BB"
#define F32_PREFIX "_F"
#define F64_PREFIX "_D"

static char *X64_OPCODES[X64_LAST] = {
    "mov", "movsx", "movzx", "movss", "movsd", "lea",
    "add", "sub", "imul", "cwd", "cdq", "cqo", "idiv", "div",
    "and", "or", "xor", "shl", "shr", "sar",
    "addss", "addsd", "subss", "subsd", "mulss", "mulsd", "divss", "divsd",
    "cmp", "sete", "setne", "setl", "setle", "setg", "setge",
    "setb", "setbe", "seta", "setae",
    "ucomiss", "ucomisd",
    "cvtss2sd", "cvtsd2ss", "cvtsi2ss", "cvtsi2sd", "cvttss2si", "cvttsd2si",
    "push", "pop",
    "jmp", "je", "jne", "jl", "jle", "jg", "jge", "jb", "jbe", "ja", "jae",
    "call", "ret", "syscall",
};

static char *GPR_NAMES[][R64 + 1] = {
    { NULL, NULL,   NULL,  NULL,   NULL,   NULL,  }, // R_NONE
    { NULL, "al",   "ah",  "ax",   "eax",  "rax", },
    { NULL, "cl",   "ch",  "cx",   "ecx",  "rcx", },
    { NULL, "dl",   "dh",  "dx",   "edx",  "rdx", },
    { NULL, "bl",   "bh",  "bx",   "ebx",  "rbx", },
    { NULL, "spl",  NULL,  "sp",   "esp",  "rsp", },
    { NULL, "bpl",  NULL,  "bp",   "ebp",  "rbp", },
    { NULL, "sil",  NULL,  "si",   "esi",  "rsi", },
    { NULL, "dil",  NULL,  "di",   "edi",  "rdi", },
    { NULL, "r8b",  NULL,  "r8w",  "r8d",  "r8",  },
    { NULL, "r9b",  NULL,  "r9w",  "r9d",  "r9",  },
    { NULL, "r10b", NULL,  "r10w", "r10d", "r10", },
    { NULL, "r11b", NULL,  "r11w", "r11d", "r11", },
    { NULL, "r12b", NULL,  "r12w", "r12d", "r12", },
    { NULL, "r13b", NULL,  "r13w", "r13d", "r13", },
    { NULL, "r14b", NULL,  "r14w", "r14d", "r14", },
    { NULL, "r15b", NULL,  "r15w", "r15d", "r15", },
};

static char *XMM_NAMES[] = {
    NULL, "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
    "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15",
};

static char *REG_SIZE_SUFFIX[] = {
    [R8L] = "l", [R8H] = "h", [R16] = "w", [R32] = "d", [R64] = "q",
};

static char *NASM_MEM_PREFIX[] = {
    [1] = "byte", [2] = "word", [4] = "dword", [8] = "qword",
};

static char *NASM_CONST[] = {
    [1] = "db", [2] = "dw", [4] = "dd", [8] = "dq",
};

static void encode_mem_access(FILE *out, size_t bytes) {
    if (bytes > 0) {
        fprintf(out, "%s ", NASM_MEM_PREFIX[bytes]);
    }
}

void encode_gpr(FILE *out, int reg, int size) {
    assert(size != R0 && reg != R_NONE);
    if (reg < LAST_GPR) { // Physical
        fprintf(out, "%s", GPR_NAMES[reg][size]);
    } else { // Virtual
        fprintf(out, "%%%d%s", reg - LAST_GPR, REG_SIZE_SUFFIX[size]);
    }
}

void encode_xmm(FILE *out, int reg) {
    if (reg < LAST_XMM) { // Physical
        fprintf(out, "%s", XMM_NAMES[reg]);
    } else { // Virtual
        fprintf(out, "%%%df", reg - LAST_XMM);
    }
}

static void encode_op(FILE *out, Global *g, AsmOpr *opr) {
    switch (opr->k) {
    case OPR_IMM: fprintf(out, "%" PRIi64, opr->imm); break;
    case OPR_F32:
        encode_mem_access(out, 4);
        fprintf(out, "[rel %s." F32_PREFIX "%zu]", g->label, opr->fp);
        break;
    case OPR_F64:
        encode_mem_access(out, 8);
        fprintf(out, "[rel %s." F64_PREFIX "%zu]", g->label, opr->fp);
        break;
    case OPR_GPR: encode_gpr(out, opr->reg, opr->size); break;
    case OPR_XMM: encode_xmm(out, opr->reg); break;
    case OPR_MEM:
        encode_mem_access(out, opr->bytes);
        fprintf(out, "[");
        encode_gpr(out, opr->base, opr->base_size);
        if (opr->idx != R_NONE) {
            fprintf(out, " + ");
            encode_gpr(out, opr->idx, opr->idx_size);
            if (opr->scale > 1) {
                fprintf(out, "*%d", opr->scale);
            }
        }
        if (opr->disp > 0) {
            fprintf(out, " + %" PRIi64, opr->disp);
        } else if (opr->disp < 0) {
            fprintf(out, " - %" PRIi64, -opr->disp);
        }
        fprintf(out, "]");
        break;
    case OPR_BB:    fprintf(out, BB_PREFIX "%zu", opr->bb->n); break;
    case OPR_LABEL: fprintf(out, "%s", opr->label); break;
    case OPR_DEREF:
        encode_mem_access(out, opr->bytes);
        fprintf(out, "[%s]", opr->label);
        break;
    }
}

static void encode_ins(FILE *out, Global *g, AsmIns *ins) {
    fprintf(out, "%s", X64_OPCODES[ins->op]);
    if (ins->l) {
        fprintf(out, " ");
        encode_op(out, g, ins->l);
    }
    if (ins->r) {
        fprintf(out, ", ");
        encode_op(out, g, ins->r);
    }
    fprintf(out, "\n");
}

static void encode_bb(FILE *out, Global *g, BB *bb) {
    fprintf(out, BB_PREFIX "%zu:\n", bb->n);
    for (AsmIns *ins = bb->asm_head; ins; ins = ins->next) {
        fprintf(out, "\t");
        encode_ins(out, g, ins);
    }
}

static void encode_fps(FILE *out, Global *g) {
    for (size_t i = 0; i < vec_len(g->fn->f32s); i++) {
        uint32_t *fp = vec_get(g->fn->f32s, i);
        fprintf(out, "%s." F32_PREFIX "%zu: ", g->label, i);
        fprintf(out, "dd 0x%" PRIx32 " ; float %g\n", *fp, *((float *) fp));
    }
    for (size_t i = 0; i < vec_len(g->fn->f64s); i++) {
        uint64_t *fp = vec_get(g->fn->f64s, i);
        fprintf(out, "%s." F64_PREFIX "%zu: ", g->label, i);
        fprintf(out, "dq 0x%" PRIx64 " ; double %g\n", *fp, *((double *) fp));
    }
}

static void number_bbs(Fn *fn) {
    size_t i = 0;
    for (BB *bb = fn->entry; bb; bb = bb->next) {
        bb->n = i++;
    }
}

static void encode_fn(FILE *out, Global *g) {
    number_bbs(g->fn);
    if (g->fn->linkage == LINK_EXTERN) {
        fprintf(out, "global %s\n", g->label);
    }
    encode_fps(out, g);
    fprintf(out, "%s:\n", g->label);
    for (BB *bb = g->fn->entry; bb; bb = bb->next) {
        encode_bb(out, g, bb);
    }
    fprintf(out, "\n");
}

static void encode_fns(FILE *out, Vec *globals) {
    int written_header = 0;
    for (size_t i = 0; i < vec_len(globals); i++) {
        Global *g = vec_get(globals, i);
        if (!g->fn) {
            continue; // Not a function definition
        }
        if (!written_header) {
            fprintf(out, "section .text\n");
            written_header = 1;
        }
        encode_fn(out, g);
    }
}

static void encode_global(FILE *out, Global *g) {
    if (g->val->t->linkage != LINK_STATIC) {
        fprintf(out, "global %s\n", g->label);
    }
    fprintf(out, "%s: ", g->label);
    AstNode *n = g->val;
    switch (n->k) {
    case N_IMM: fprintf(out, "%s %" PRIu64, NASM_CONST[n->t->size], n->imm); break;
    case N_FP:  fprintf(out, "%s %lf", NASM_CONST[n->t->size], n->fp); break;
    case N_STR: fprintf(out, "db `%s` 0", quote_str(n->str, n->len)); break; // TODO: encoding
    case N_INIT: break; // TODO
    case N_KPTR:
        fprintf(out, "dq %s", n->g->label);
        if (n->offset > 0) {
            fprintf(out, " + %" PRIi64, n->offset);
        } else {
            fprintf(out, " - %" PRIi64, -n->offset);
        }
        break;
    default: UNREACHABLE();
    }
    fprintf(out, "\n");
}

static void encode_globals(FILE *out, Vec *globals) {
    int written_header = 0;
    for (size_t i = 0; i < vec_len(globals); i++) {
        Global *g = vec_get(globals, i);
        if (!g->val) {
            continue; // Function definition
        }
        if (!written_header) {
            fprintf(out, "section .data\n");
            written_header = 1;
        }
        encode_global(out, g);
    }
}

void encode_nasm(FILE *out, Vec *globals) {
    encode_fns(out, globals);     // .text section
    encode_globals(out, globals); // .data section
}
