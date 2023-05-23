
#include <stdlib.h>

#include "assemble.h"

// macOS requires stack to be 16-byte aligned before calls
#define STACK_ALIGN 16

typedef struct { // Per-function assembler
    Fn *fn;
    BB *bb;
    int next_gpr, next_sse;
    size_t next_stack;
    Vec *patch_with_stack_size; // of 'AsmIns *'
} Assembler;

static Assembler * new_asm(Fn *fn) {
    Assembler *a = malloc(sizeof(Assembler));
    a->fn = fn;
    a->bb = fn->entry;
    a->next_gpr = LAST_GPR;
    a->next_sse = LAST_XMM;
    a->next_stack = 0;
    a->patch_with_stack_size = vec_new();
    return a;
}


// ---- Instructions ----------------------------------------------------------

static AsmIns * asm0(int op) {
    AsmIns *ins = malloc(sizeof(AsmIns));
    ins->next = ins->prev = NULL;
    ins->bb = NULL;
    ins->op = op;
    ins->l = ins->r = NULL;
    ins->n = 0;
    return ins;
}

static AsmIns * asm1(int op, AsmOpr *l) {
    AsmIns *ins = asm0(op);
    ins->l = l;
    return ins;
}

static AsmIns * asm2(int op, AsmOpr *l, AsmOpr *r) {
    AsmIns *ins = asm0(op);
    ins->l = l;
    ins->r = r;
    return ins;
}

static AsmIns * emit_to_bb(BB *bb, AsmIns *ins) {
    ins->bb = bb;
    ins->prev = bb->asm_last;
    if (bb->asm_last) {
        bb->asm_last->next = ins;
    } else {
        bb->asm_head = ins;
    }
    bb->asm_last = ins;
    return ins;
}

static AsmIns * emit(Assembler *a, AsmIns *ins) {
    return emit_to_bb(a->bb, ins);
}

void delete_asm(AsmIns *ins) {
    if (ins->prev) {
        ins->prev->next = ins->next;
    } else { // Head of linked list
        ins->bb->asm_head = ins->next;
    }
    if (ins->next) {
        ins->next->prev = ins->prev;
    }
    if (ins->bb->asm_last == ins) {
        ins->bb->asm_last = ins->prev;
    }
}


// ---- Operands --------------------------------------------------------------

static AsmOpr * discharge(Assembler *a, IrIns *ir);

static AsmOpr * opr_new(int k) {
    AsmOpr *opr = calloc(1, sizeof(AsmOpr));
    opr->k = k;
    return opr;
}

static AsmOpr * opr_imm(uint64_t imm) {
    AsmOpr *opr = opr_new(OPR_IMM);
    opr->imm = imm;
    return opr;
}

static AsmOpr * opr_fp(IrType *t, size_t idx) {
    AsmOpr *opr = opr_new(t->k == IRT_F32 ? OPR_F32 : OPR_F64);
    opr->fp = idx;
    return opr;
}

static AsmOpr * opr_gpr(int reg, int size) {
    AsmOpr *opr = opr_new(OPR_GPR);
    opr->reg = reg;
    opr->size = size;
    return opr;
}

static AsmOpr * opr_gpr_t(int reg, IrType *t) {
    int size;
    switch (t->k) {
        case IRT_I8:  size = R8L; break;
        case IRT_I16: size = R16; break;
        case IRT_I32: size = R32; break;
        case IRT_I64: case IRT_PTR: case IRT_ARR: size = R64; break;
        default: UNREACHABLE();
    }
    return opr_gpr(reg, size);
}

static AsmOpr * opr_xmm(int reg) {
    AsmOpr *opr = opr_new(OPR_XMM);
    opr->reg = reg;
    return opr;
}

static AsmOpr * opr_bb(BB *bb) {
    AsmOpr *opr = opr_new(OPR_BB);
    opr->bb = bb;
    return opr;
}

static AsmOpr * opr_label(char *label) {
    AsmOpr *opr = opr_new(OPR_LABEL);
    opr->label = label;
    return opr;
}

static AsmOpr * opr_deref(char *label) {
    AsmOpr *opr = opr_new(OPR_DEREF);
    opr->label = label;
    return opr;
}

// 'mem->bytes' MUST be set by the calling function if memory is to be accessed
static AsmOpr * opr_mem_from_ptr(Assembler *a, IrIns *ptr, IrType *to_load) {
    assert(ptr->t->k == IRT_PTR);
    AsmOpr *l = discharge(a, ptr);
    assert(l->k == OPR_GPR);
    assert(l->size == R64);
    AsmOpr *mem = opr_new(OPR_MEM); // [<reg>]
    mem->base = l->reg;
    mem->base_size = R64;
    mem->scale = 1;
    if (to_load) {
        assert(to_load->size <= 8);
        mem->bytes = to_load->size;
    }
    return mem;
}

static AsmOpr * opr_mem_from_alloc(IrIns *alloc, IrType *to_load) {
    assert(alloc->op == IR_ALLOC);
    assert(alloc->t->k == IRT_PTR);
    AsmOpr *mem = opr_new(OPR_MEM); // [rbp - <stack slot>]
    mem->base = RBP;
    mem->base_size = R64;
    mem->scale = 1;
    mem->disp = -((int64_t) alloc->stack_slot);
    if (to_load) {
        assert(to_load->size <= 8);
        mem->bytes = to_load->size;
    }
    return mem;
}

static AsmOpr * opr_mem_from_global(IrIns *global, IrType *to_load) {
    assert(global->op == IR_GLOBAL);
    assert(global->t->k == IRT_PTR);
    AsmOpr *mem = opr_deref(global->g->label);
    if (to_load) {
        assert(to_load->size <= 8);
        mem->bytes = to_load->size;
    }
    return mem;
}

// 'to_load' is the type of the object pointed to by 'ptr'; gives us the number
// of bytes to read from memory (or NULL if we don't care about setting 'bytes')
static AsmOpr * load_ptr(Assembler *a, IrIns *ptr, IrType *to_load) {
    assert(ptr->t->k == IRT_PTR);
    switch (ptr->op) {
        case IR_ALLOC:  return opr_mem_from_alloc(ptr, to_load);
        case IR_GLOBAL: return opr_mem_from_global(ptr, to_load);
        // TODO: inline IR_PTRADD
        default:        return opr_mem_from_ptr(a, ptr, to_load);
    }
}

static AsmOpr * next_vreg(Assembler *a, IrType *t) {
    if (t->k == IRT_F32 || t->k == IRT_F64) {
        return opr_xmm(a->next_sse++);
    } else {
        return opr_gpr_t(a->next_gpr++, t);
    }
}

static int mov_for(IrType *t) {
    switch (t->k) {
        case IRT_F32: return X64_MOVSS;
        case IRT_F64: return X64_MOVSD;
        default:      return X64_MOV;
    }
}


// ---- Operand Discharge and Inlining ----------------------------------------

static int SET_OP[IR_LAST] = {
    [IR_EQ] = X64_SETE, [IR_NEQ] = X64_SETNE,
    [IR_SLT] = X64_SETL, [IR_SLE] = X64_SETLE, [IR_SGT] = X64_SETG, [IR_SGE] = X64_SETGE,
    [IR_ULT] = X64_SETB, [IR_ULE] = X64_SETBE, [IR_UGT] = X64_SETA, [IR_UGE] = X64_SETAE,
    [IR_FLT] = X64_SETB, [IR_FLE] = X64_SETBE, [IR_FGT] = X64_SETA, [IR_FGE] = X64_SETAE,
};

static void asm_cmp(Assembler *a, IrIns *ir);

// Emit assembly to put the result of an instruction into a vreg
static AsmOpr * discharge(Assembler *a, IrIns *ir) {
    // Always re-materialise a LEA on discharging an IR_ALLOC
    if (ir->op != IR_ALLOC && ir->vreg != R_NONE) { // Already in a vreg
        return (ir->t->k == IRT_F32 || ir->t->k == IRT_F64) ?
            opr_xmm(ir->vreg) : opr_gpr_t(ir->vreg, ir->t);
    }
    AsmOpr *dst = next_vreg(a, ir->t);
    ir->vreg = dst->reg;
    switch (ir->op) {
    case IR_IMM:    emit(a, asm2(mov_for(ir->t), dst, opr_imm(ir->imm))); break;
    case IR_FP:     emit(a, asm2(mov_for(ir->t), dst, opr_fp(ir->t, ir->fp_idx))); break;
    case IR_GLOBAL: emit(a, asm2(X64_LEA, dst, opr_deref(ir->g->label))); break;
    case IR_LOAD:   emit(a, asm2(mov_for(ir->t), dst, load_ptr(a, ir->l, ir->t))); break;
    case IR_ALLOC:  emit(a, asm2(X64_LEA, dst, opr_mem_from_alloc(ir, NULL))); break;
    case IR_EQ:  case IR_NEQ:
    case IR_SLT: case IR_SLE: case IR_SGT: case IR_SGE:
    case IR_ULT: case IR_ULE: case IR_UGT: case IR_UGE:
    case IR_FLT: case IR_FLE: case IR_FGT: case IR_FGE:
        asm_cmp(a, ir);
        emit(a, asm1(SET_OP[ir->op], opr_gpr(dst->reg, R8L))); // Set low 8 bits
        emit(a, asm2(X64_AND, dst, opr_imm(1))); // Clear rest of vreg
        break;
    default: UNREACHABLE();
    }
    return dst;
}

static AsmOpr * inline_imm(Assembler *a, IrIns *ir) {
    if (ir->op == IR_IMM) {
        return opr_imm(ir->imm);
    }
    return discharge(a, ir);
}

static AsmOpr * inline_mem(Assembler *a, IrIns *ir) {
    if (ir->op == IR_LOAD) {
        if (ir->vreg != R_NONE) { // Already in a vreg
            return discharge(a, ir);
        }
        return load_ptr(a, ir->l, ir->t);
    } else if (ir->op == IR_FP) {
        return opr_fp(ir->t, ir->fp_idx);
    } else {
        return discharge(a, ir);
    }
}

static AsmOpr * inline_imm_mem(Assembler *a, IrIns *ir) {
    if (ir->op == IR_IMM) {
        return inline_imm(a, ir);
    } else {
        return inline_mem(a, ir);
    }
}

static AsmOpr * inline_label_mem(Assembler *a, IrIns *ir) {
    if (ir->op == IR_GLOBAL) {
        return opr_label(ir->g->label);
    } else {
        return inline_mem(a, ir);
    }
}


// ---- Immediates, Constants, and Memory Operations --------------------------

#define NUM_REG_FARGS 6
static int GPR_FARGS[] = { RDI, RSI, RDX, RCX, R8, R9, };
static int SSE_FARGS[] = { XMM0, XMM1, XMM2, XMM3, XMM4, XMM5, XMM6, XMM7, };

static void asm_farg(Assembler *a, IrIns *ir) {
    // TODO: stack arguments
    AsmOpr *dst = next_vreg(a, ir->t); // vreg for the result
    ir->vreg = dst->reg;
    AsmOpr *src;
    if (ir->t->k == IRT_F32 || ir->t->k == IRT_F64) {
        src = opr_xmm(SSE_FARGS[ir->arg_idx]); // FP args indexed separately
    } else {
        src = opr_gpr_t(GPR_FARGS[ir->arg_idx], ir->t);
    }
    emit(a, asm2(mov_for(ir->t), dst, src));
}

static void asm_fp(Assembler *a, IrIns *ir) {
    size_t idx;
    if (ir->t->k == IRT_F32) {
        idx = vec_len(a->fn->f32s);
        float *fp = malloc(sizeof(float));
        *fp = (float) ir->fp;
        vec_push(a->fn->f32s, fp);
    } else {
        idx = vec_len(a->fn->f64s);
        double *fp = malloc(sizeof(double));
        *fp = ir->fp;
        vec_push(a->fn->f64s, fp);
    }
    ir->fp_idx = idx;
}

static void asm_alloc(Assembler *a, IrIns *ir) {
    assert(ir->t->k == IRT_PTR);
    a->next_stack += + pad(a->next_stack, ir->alloc_t->align) + ir->alloc_t->size;
    ir->stack_slot = a->next_stack;
}

static void asm_load(Assembler *a, IrIns *ir) {
    // TODO: discharge IR_LOADs with more than one use here
}

static void asm_store(Assembler *a, IrIns *ir) {
    AsmOpr *l = load_ptr(a, ir->dst, ir->src->t);
    AsmOpr *r = inline_imm(a, ir->src);
    emit(a, asm2(mov_for(ir->src->t), l, r));
}

static void asm_ptradd(Assembler *a, IrIns *ir) {
    AsmOpr *l = discharge(a, ir->l);
    AsmOpr *r = inline_imm(a, ir->r);
    if (r->k == OPR_IMM && r->imm == 0) {
        ir->vreg = l->reg;
        return; // Doesn't modify the pointer
    }

    AsmOpr *dst = next_vreg(a, ir->t); // vreg for the result
    ir->vreg = dst->reg;

    // TODO: incorporate PTRADD, ADD/SUB, MUL, SHL, ALLOC (rbp) into LEA
    // TODO: implement basic dead code elimination
    AsmOpr *addr = opr_new(OPR_MEM);
    addr->base = l->reg;
    assert(l->size == R64); // Pointers are always 64-bit
    addr->base_size = R64;
    addr->scale = 1;
    switch (r->k) {
    case OPR_IMM:
        addr->disp = (int64_t) r->imm;
        break;
    case OPR_GPR:
        addr->idx = r->reg;
        assert(r->size == R64); // Pointers are always 64-bit
        addr->idx_size = R64;
        break;
    default: UNREACHABLE();
    }
    emit(a, asm2(X64_LEA, dst, addr));
}


// ---- Arithmetic ------------------------------------------------------------

static int INT_OP[IR_LAST] = {
    [IR_ADD] = X64_ADD,
    [IR_SUB] = X64_SUB,
    [IR_MUL] = X64_IMUL,
    [IR_BIT_AND] = X64_AND,
    [IR_BIT_OR] = X64_OR,
    [IR_BIT_XOR] = X64_XOR,
    [IR_SHL] = X64_SHL,
    [IR_SAR] = X64_SAR,
    [IR_SHR] = X64_SHR,
};

static int F32_OP[IR_LAST] = {
    [IR_ADD]  = X64_ADDSS,
    [IR_SUB]  = X64_SUBSS,
    [IR_MUL]  = X64_MULSS,
    [IR_FDIV] = X64_DIVSS,
};

static int F64_OP[IR_LAST] = {
    [IR_ADD]  = X64_ADDSD,
    [IR_SUB]  = X64_SUBSD,
    [IR_MUL]  = X64_MULSD,
    [IR_FDIV] = X64_DIVSD,
};

static void asm_arith(Assembler *a, IrIns *ir) {
    AsmOpr *l = discharge(a, ir->l); // Left operand always a vreg
    AsmOpr *r = inline_imm_mem(a, ir->r);

    AsmOpr *dst = next_vreg(a, ir->t); // vreg for the result
    ir->vreg = dst->reg;
    emit(a, asm2(mov_for(ir->t), dst, l));

    int op;
    switch (ir->t->k) {
        case IRT_F32: op = F32_OP[ir->op]; break;
        case IRT_F64: op = F64_OP[ir->op]; break;
        default:      op = INT_OP[ir->op]; break;
    }
    assert(op != 0);
    emit(a, asm2(op, dst, r));
}

static void asm_div_mod(Assembler *a, IrIns *ir) {
    AsmOpr *dividend = discharge(a, ir->l); // Left operand always a vreg
    AsmOpr *divisor = inline_mem(a, ir->r);

    // Mov dividend into eax
    emit(a, asm2(X64_MOV, opr_gpr(RAX, dividend->size), dividend));

    // Sign extend eax into edx:eax
    int ext_op;
    switch (ir->t->size) {
        case 4:  ext_op = X64_CDQ; break;
        case 8:  ext_op = X64_CQO; break;
        default: ext_op = X64_CWD; break;
    }
    emit(a, asm0(ext_op));

    // div or idiv performs rdx:rax / <operand>
    int is_signed = (ir->op == IR_SDIV || ir->op == IR_SMOD);
    emit(a, asm1(is_signed ? X64_IDIV : X64_DIV, divisor));

    // Mov result from rax (division) or rdx (modulo) into vreg
    AsmOpr *dst = next_vreg(a, ir->t); // vreg for the result
    ir->vreg = dst->reg;
    AsmOpr *result;
    if (ir->op == IR_SDIV || ir->op == IR_UDIV) {
        result = opr_gpr_t(RAX, ir->t); // Quotient in rax
    } else { // IR_MOD
        result = opr_gpr_t(RDX, ir->t); // Remainder in rax
    }
    emit(a, asm2(X64_MOV, dst, result));
}

static void asm_sh(Assembler *a, IrIns *ir) {
    AsmOpr *l = discharge(a, ir->l); // Left operand always a vreg

    AsmOpr *r = inline_imm(a, ir->r); // Right either imm or vreg
    if (r->k == OPR_GPR) { // If a vreg, shift count has to be in cl
        emit(a, asm2(X64_MOV, opr_gpr(RCX, r->size), r));
        r = opr_gpr(RCX, R8L);
    }

    AsmOpr *dst = next_vreg(a, ir->t); // vreg for the result
    ir->vreg = dst->reg;
    emit(a, asm2(X64_MOV, dst, l));

    emit(a, asm2(INT_OP[ir->op], dst, r)); // shift operation
}


// ---- Conversions -----------------------------------------------------------

static void asm_trunc(Assembler *a, IrIns *ir) {
    // For conversions, we can't allocate the result to the same vreg as its
    // operand, because the source operand might still be used after the
    // conversion operation.
    // We need to maintain SSA form over the assembly output, so emit a mov
    // into a new vreg and let the coalescer deal with it.
    AsmOpr *src = inline_imm_mem(a, ir->l);
    AsmOpr *dst = next_vreg(a, ir->t); // New vreg for the result
    ir->vreg = dst->reg;
    // We can't (e.g.) do mov ax, qword [rbp-4]; we have to mov into a register
    // the same size as the SOURCE, then use the truncated register (i.e., ax)
    // in future instructions
    emit(a, asm2(X64_MOV, opr_gpr_t(dst->reg, ir->l->t), src));
}

static void asm_ext(Assembler *a, IrIns *ir, int op) {
    AsmOpr *src = inline_imm_mem(a, ir->l);
    if (src->k == OPR_IMM) {
        op = X64_MOV;
    }
    AsmOpr *dst = next_vreg(a, ir->t); // New vreg for the result
    ir->vreg = dst->reg;
    emit(a, asm2(op, dst, src));
}

static void asm_fp_trunc_ext(Assembler *a, IrIns *ir, int op) {
    // See below for why we don't use cvtxx2xx with a memory operand:
    // https://stackoverflow.com/questions/16597587/why-dont-gcc-and-clang-use-cvtss2sd-memory
    AsmOpr *src = discharge(a, ir->l);
    AsmOpr *dst = next_vreg(a, ir->t); // New vreg for the result
    ir->vreg = dst->reg;
    emit(a, asm2(op, dst, src));
}

static void asm_conv_fp_int(Assembler *a, IrIns *ir, int op) {
    AsmOpr *src = discharge(a, ir->l);
    AsmOpr *dst = next_vreg(a, ir->t); // New vreg for the result
    ir->vreg = dst->reg;
    emit(a, asm2(op, dst, src));
}

static void asm_fp_to_int(Assembler *a, IrIns *ir) {
    int op = ir->l->t->k == IRT_F32 ? X64_CVTTSS2SI : X64_CVTTSD2SI;
    asm_conv_fp_int(a, ir, op);
}

static void asm_int_to_fp(Assembler *a, IrIns *ir) {
    int op = ir->t->k == IRT_F32 ? X64_CVTSI2SS : X64_CVTSI2SD;
    asm_conv_fp_int(a, ir, op);
}


// ---- Control Flow ----------------------------------------------------------

static int JMP_OP[IR_LAST] = {
    [IR_EQ] = X64_JE,  [IR_NEQ] = X64_JNE,
    [IR_SLT] = X64_JL, [IR_SLE] = X64_JLE, [IR_SGT] = X64_JG, [IR_SGE] = X64_JGE,
    [IR_ULT] = X64_JB, [IR_ULE] = X64_JBE, [IR_UGT] = X64_JA, [IR_UGE] = X64_JAE,
    [IR_FLT] = X64_JB, [IR_FLE] = X64_JBE, [IR_FGT] = X64_JA, [IR_FGE] = X64_JAE,
};

static int INVERT_JMP[X64_LAST] = {
    [X64_JE] = X64_JNE, [X64_JNE] = X64_JE,
    [X64_JL] = X64_JGE, [X64_JLE] = X64_JG, [X64_JG] = X64_JLE, [X64_JGE] = X64_JL,
    [X64_JB] = X64_JAE, [X64_JBE] = X64_JA, [X64_JA] = X64_JBE, [X64_JAE] = X64_JB,
};

#define GPR_RET_REG RAX
#define SSE_RET_REG XMM0

static void asm_br(Assembler *a, IrIns *ir) {
    if (ir->br == ir->bb->next) {
        return; // Don't emit anything for a branch to next BB
    }
    emit(a, asm1(X64_JMP, opr_bb(ir->br)));
}

static void asm_cmp(Assembler *a, IrIns *ir) { // 'ir' is comparison instruction
    AsmOpr *l = discharge(a, ir->l);
    AsmOpr *r = inline_imm_mem(a, ir->r);
    int op;
    switch (ir->l->t->k) {
        case IRT_F32: op = X64_UCOMISS; break;
        case IRT_F64: op = X64_UCOMISD; break;
        default:      op = X64_CMP; break;
    }
    emit(a, asm2(op, l, r));
}

static void asm_condbr(Assembler *a, IrIns *ir) {
    // The true case or false case MUST be the next basic block
    assert(ir->true == ir->bb->next || ir->false == ir->bb->next);
    asm_cmp(a, ir->cond);
    int op = JMP_OP[ir->cond->op];
    assert(op != 0);
    if (ir->true == ir->bb->next) { // True case falls through
        op = INVERT_JMP[op]; // Invert condition
        assert(op != 0);
    }
    BB *target = (ir->true == ir->bb->next) ? ir->false : ir->true;
    emit(a, asm1(op, opr_bb(target)));
}

static void asm_call(Assembler *a, IrIns *ir) {
    // Count number of arguments
    size_t nargs = 0;
    for (IrIns *ins = ir->next; ins && ins->op == IR_CARG; ins = ins->next) {
        nargs++;
    }

    // Discharge/inline arguments
    size_t i = 0;
    AsmOpr *args[nargs];
    for (IrIns *ins = ir->next; ins && ins->op == IR_CARG; ins = ins->next) {
        args[i++] = inline_imm_mem(a, ins->l);
    }

    // Move arguments into required registers
    i = 0;
    size_t gpr_idx = 0, sse_idx = 0;
    for (IrIns *ins = ir->next; ins && ins->op == IR_CARG; ins = ins->next) {
        // TODO: pass extra arguments on stack (beyond NUM_REG_FARGS)
        AsmOpr *dst;
        if (ins->t->k == IRT_F32 || ins->t->k == IRT_F64) {
            dst = opr_xmm(SSE_FARGS[sse_idx++]);
        } else {
            dst = opr_gpr_t(GPR_FARGS[gpr_idx++], ins->t);
        }
        emit(a, asm2(mov_for(ins->t), dst, args[i++]));
    }

    // Emit call
    emit(a, asm1(X64_CALL, inline_label_mem(a, ir->l)));

    // Get return value
    if (ir->t->k != IRT_VOID) {
        AsmOpr *dst = next_vreg(a, ir->t); // New vreg for the result
        ir->vreg = dst->reg;
        AsmOpr *ret;
        if (ir->t->k == IRT_F32 || ir->t->k == IRT_F64) {
            ret = opr_xmm(SSE_RET_REG);
        } else {
            ret = opr_gpr_t(GPR_RET_REG, ir->t);
        }
        emit(a, asm2(mov_for(ir->t), dst, ret));
    }
}

static void asm_postamble(Assembler *a);

static void asm_ret(Assembler *a, IrIns *ir) {
    if (ir->ret) {
        AsmOpr *val = inline_imm_mem(a, ir->ret);
        if (ir->ret->t->k == IRT_F32 || ir->ret->t->k == IRT_F64) {
            emit(a, asm2(mov_for(ir->ret->t), opr_xmm(SSE_RET_REG), val));
        } else {
            // Zero the rest of eax using movsx if the function returns
            // something smaller than an int
            AsmOpr *dst = opr_gpr(GPR_RET_REG, ir->ret->t->size == 8 ? R64 : R32);
            int mov_op = ir->ret->t->size < 4 ? X64_MOVSX : X64_MOV;
            emit(a, asm2(mov_op, dst, val));
        }
    }
    asm_postamble(a);
    emit(a, asm0(X64_RET));
}


// ---- Functions, Basic Blocks, and Instructions -----------------------------

static void asm_ins(Assembler *a, IrIns *ir) {
    switch (ir->op) {
    // Constants and globals
    case IR_IMM:    break; // Always inlined
    case IR_FP:     asm_fp(a, ir); break;
    case IR_GLOBAL: break; // Always inlined

        // Memory access
    case IR_FARG:   asm_farg(a, ir); break;
    case IR_ALLOC:  asm_alloc(a, ir); break;
    case IR_LOAD:   asm_load(a, ir); break;
    case IR_STORE:  asm_store(a, ir); break;
    case IR_PTRADD: asm_ptradd(a, ir); break;

        // Arithmetic
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_FDIV:
    case IR_BIT_AND: case IR_BIT_OR: case IR_BIT_XOR:
        asm_arith(a, ir);
        break;
    case IR_SDIV: case IR_UDIV: case IR_SMOD: case IR_UMOD:
        asm_div_mod(a, ir);
        break;
    case IR_SHL: case IR_SAR: case IR_SHR:
        asm_sh(a, ir);
        break;

        // Comparisons
    case IR_EQ:  case IR_NEQ:
    case IR_SLT: case IR_SLE: case IR_SGT: case IR_SGE:
    case IR_ULT: case IR_ULE: case IR_UGT: case IR_UGE:
    case IR_FLT: case IR_FLE: case IR_FGT: case IR_FGE:
        break; // Handled by CONDBR or 'discharge'

        // Conversions
    case IR_TRUNC:   asm_trunc(a, ir); break;
    case IR_SEXT:    asm_ext(a, ir, X64_MOVSX); break;
    case IR_ZEXT:    asm_ext(a, ir, X64_MOVZX); break;
    case IR_PTR2I:   asm_trunc(a, ir); break; // Pointers are always bigger
    case IR_I2PTR:   asm_ext(a, ir, X64_MOVZX); break;
    case IR_BITCAST: asm_ext(a, ir, X64_MOV); break;

    case IR_FTRUNC: asm_fp_trunc_ext(a, ir, X64_CVTSD2SS); break;
    case IR_FEXT:   asm_fp_trunc_ext(a, ir, X64_CVTSS2SD); break;
    case IR_FP2I:   asm_fp_to_int(a, ir); break;
    case IR_I2FP:   asm_int_to_fp(a, ir); break;

        // Control flow
    case IR_BR:     asm_br(a, ir); break;
    case IR_CONDBR: asm_condbr(a, ir); break;
    case IR_CALL:   asm_call(a, ir); break;
    case IR_CARG:   break; // Handled by IR_CALL
    case IR_RET:    asm_ret(a, ir); break;
    default: assert(0); // TODO
    }
}

static void asm_bb(Assembler *a, BB *bb) {
    for (IrIns *ins = bb->ir_head; ins; ins = ins->next) {
        asm_ins(a, ins);
    }
}

static void asm_preamble(Assembler *a) {
    emit(a, asm1(X64_PUSH, opr_gpr(RBP, R64)));                            // push rbp
    emit(a, asm2(X64_MOV, opr_gpr(RBP, R64), opr_gpr(RSP, R64)));          // mov rbp, rsp
    AsmIns *patch = emit(a, asm2(X64_SUB, opr_gpr(RSP, R64), opr_imm(0))); // sub rsp, <stack size>
    vec_push(a->patch_with_stack_size, patch);
}

static void asm_postamble(Assembler *a) {
    AsmIns *patch = emit(a, asm2(X64_ADD, opr_gpr(RSP, R64), opr_imm(0))); // add rsp, <stack size>
    emit(a, asm1(X64_POP, opr_gpr(RBP, R64)));                             // pop rbp
    vec_push(a->patch_with_stack_size, patch);
}

static void patch_stack_sizes(Assembler *a) {
    a->next_stack += pad(a->next_stack, STACK_ALIGN);
    if (a->next_stack == 0) {
        for (size_t i = 0; i < vec_len(a->patch_with_stack_size); i++) {
            AsmIns *ins = vec_get(a->patch_with_stack_size, i);
            delete_asm(ins);
        }
    } else {
        for (size_t i = 0; i < vec_len(a->patch_with_stack_size); i++) {
            AsmIns *ins = vec_get(a->patch_with_stack_size, i);
            assert(ins->r->k == OPR_IMM);
            ins->r->imm = a->next_stack;
        }
    }
}

static void asm_fn(Fn *fn) {
    Assembler *a = new_asm(fn);
    asm_preamble(a);
    for (BB *bb = fn->entry; bb; bb = bb->next) {
        a->bb = bb;
        asm_bb(a, bb);
    }
    patch_stack_sizes(a);
    fn->num_gprs = a->next_gpr;
    fn->num_sse = a->next_sse;
}

void assemble(Vec *global) {
    for (size_t i = 0; i < vec_len(global); i++) {
        Global *g = vec_get(global, i);
        if (g->fn) {
            asm_fn(g->fn);
        }
    }
}
