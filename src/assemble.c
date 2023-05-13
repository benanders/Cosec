
#include <stdlib.h>

#include "assemble.h"

// macOS requires stack to be 16-byte aligned before calls
#define STACK_ALIGN 16

typedef struct { // Per-function assembler
    AsmFn *fn;
    int next_gpr, next_sse;
    size_t next_stack;
    Vec *patch_with_stack_size; // of 'AsmIns *'
} Assembler;


// ---- Assembler, Functions, and Basic Blocks --------------------------------

static Assembler * new_asm(AsmFn *fn) {
    Assembler *a = malloc(sizeof(Assembler));
    a->fn = fn;
    a->next_gpr = LAST_GPR;
    a->next_sse = LAST_XMM;
    a->next_stack = 0;
    a->patch_with_stack_size = vec_new();
    return a;
}

static AsmBB * new_bb() {
    AsmBB *bb = malloc(sizeof(AsmBB));
    bb->next = bb->prev = NULL;
    bb->head = bb->last = NULL;
    bb->n = 0;
    bb->pred = bb->succ = NULL;
    bb->live_in = NULL;
    return bb;
}

static AsmFn * new_fn() {
    AsmFn *fn = malloc(sizeof(AsmFn));
    fn->entry = fn->last = new_bb();
    fn->f32s = vec_new();
    fn->f64s = vec_new();
    return fn;
}

static AsmBB * emit_bb(Assembler *a) {
    assert(a->fn);
    AsmBB *bb = new_bb();
    bb->prev = a->fn->last;
    if (a->fn->last) {
        a->fn->last->next = bb;
    } else {
        a->fn->entry = bb;
    }
    a->fn->last = bb;
    return bb;
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

static AsmIns * emit_to_bb(AsmBB *bb, AsmIns *ins) {
    ins->bb = bb;
    ins->prev = bb->last;
    if (bb->last) {
        bb->last->next = ins;
    } else {
        bb->head = ins;
    }
    bb->last = ins;
    return ins;
}

static AsmIns * emit(Assembler *a, AsmIns *ins) {
    return emit_to_bb(a->fn->last, ins);
}

void delete_asm(AsmIns *ins) {
    if (ins->prev) {
        ins->prev->next = ins->next;
    } else { // Head of linked list
        ins->bb->head = ins->next;
    }
    if (ins->next) {
        ins->next->prev = ins->prev;
    }
    if (ins->bb->last == ins) {
        ins->bb->last = ins->prev;
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
    switch (t->size) {
        case 1: size = R8L; break;
        case 2: size = R16; break;
        case 4: size = R32; break;
        case 8: size = R64; break;
        default: UNREACHABLE();
    }
    return opr_gpr(reg, size);
}

static AsmOpr * opr_xmm(int reg) {
    AsmOpr *opr = opr_new(OPR_XMM);
    opr->reg = reg;
    return opr;
}

static AsmOpr * opr_deref(char *label, int bytes) {
    AsmOpr *opr = opr_new(OPR_DEREF);
    opr->label = label;
    opr->bytes = bytes;
    return opr;
}

// 'mem->bytes' MUST be set by the calling function if memory is to be accessed
static AsmOpr * opr_mem_from_ptr(Assembler *a, IrIns *ptr) {
    assert(ptr->t->k == IRT_PTR);
    AsmOpr *l = discharge(a, ptr);
    assert(l->k == OPR_GPR);
    assert(l->size == R64);
    AsmOpr *mem = opr_new(OPR_MEM); // [<reg>]
    mem->base = l->reg;
    mem->base_size = R64;
    mem->scale = 1;
    return mem;
}

static AsmOpr * opr_mem_from_alloc(IrIns *alloc) {
    assert(alloc->op == IR_ALLOC);
    assert(alloc->t->k == IRT_PTR);
    AsmOpr *mem = opr_new(OPR_MEM); // [rbp - <stack slot>]
    mem->base = RBP;
    mem->base_size = R64;
    mem->scale = 1;
    mem->disp = -((int64_t) alloc->stack_slot);
    return mem;
}

static AsmOpr * opr_mem_from_global(IrIns *global) {
    assert(global->op == IR_GLOBAL);
    assert(global->t->k == IRT_PTR);
    return opr_deref(global->g->label, 0);
}

static AsmOpr * load_ptr(Assembler *a, IrIns *to_load) {
    assert(to_load->t->k == IRT_PTR);
    switch (to_load->op) {
        case IR_ALLOC:  return opr_mem_from_alloc(to_load);
        case IR_GLOBAL: return opr_mem_from_global(to_load);
        default:        return opr_mem_from_ptr(a, to_load);
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

// Emit assembly to put the result of an instruction into a vreg and returns
// the vreg as an operand
static AsmOpr * discharge(Assembler *a, IrIns *ir) {
    if (ir->vreg > 0) { // Already in a vreg
        if (ir->t->k == IRT_F32 || ir->t->k == IRT_F64) {
            return opr_xmm(ir->vreg);
        } else {
            return opr_gpr_t(ir->vreg, ir->t);
        }
    }
    int mov_op = mov_for(ir->t);
    AsmOpr *opr = next_vreg(a, ir->t);
    ir->vreg = opr->reg;
    switch (ir->op) {
        case IR_IMM:    emit(a, asm2(mov_op,  opr, opr_imm(ir->imm))); break;
        case IR_FP:     emit(a, asm2(mov_op,  opr, opr_fp(ir->t, ir->fp_idx))); break;
        case IR_GLOBAL: emit(a, asm2(X64_LEA, opr, opr_deref(ir->g->label, 0))); break;
        case IR_LOAD:   emit(a, asm2(mov_op,  opr, load_ptr(a, ir->l))); break;
        case IR_ALLOC:  emit(a, asm2(X64_LEA, opr, opr_mem_from_alloc(ir))); break;
        // TODO: comparison operations
        default: UNREACHABLE();
    }
    return opr;
}

static AsmOpr * inline_imm(Assembler *a, IrIns *ir) {
    if (ir->op == IR_IMM) {
        return opr_imm(ir->imm);
    }
    return discharge(a, ir);
}

static AsmOpr * inline_mem(Assembler *a, IrIns *ir) {
    if (ir->op == IR_LOAD) {
        if (ir->vreg > 0) { // Already in a vreg
            return discharge(a, ir);
        }
        return load_ptr(a, ir->l);
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


// ---- Instructions ----------------------------------------------------------

static int INT_OP[IR_LAST] = {
    [IR_ADD] = X64_ADD,
    [IR_SUB] = X64_SUB,
    [IR_MUL] = X64_IMUL,
    [IR_BIT_AND] = X64_AND,
    [IR_BIT_OR] = X64_OR,
    [IR_BIT_XOR] = X64_XOR,
};

static int F32_OP[IR_LAST] = {
    [IR_ADD] = X64_ADDSS,
    [IR_SUB] = X64_SUBSS,
    [IR_MUL] = X64_MULSS,
    [IR_DIV] = X64_DIVSS,
};

static int F64_OP[IR_LAST] = {
    [IR_ADD] = X64_ADDSD,
    [IR_SUB] = X64_SUBSD,
    [IR_MUL] = X64_MULSD,
    [IR_DIV] = X64_DIVSD,
};

static void asm_fp(Assembler *a, IrIns *ir) {
    size_t idx;
    if (ir->t->k == T_FLOAT) {
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

static size_t align_to(size_t val, size_t align) {
    if (align > 0) {
        return val % align == 0 ? val : val + align - (val % align);
    } else {
        return val;
    }
}

static void asm_alloc(Assembler *a, IrIns *ir) {
    assert(ir->t->k == IRT_PTR);
    a->next_stack = align_to(a->next_stack, ir->alloc_t->align) + ir->alloc_t->size;
    ir->stack_slot = a->next_stack;
}

static void asm_load(Assembler *a, IrIns *ir) {
    // TODO: discharge IR_LOADs with more than one use here
}

static void asm_store(Assembler *a, IrIns *ir) {
    AsmOpr *l = load_ptr(a, ir->dst);
    AsmOpr *r = inline_imm(a, ir->src);
    emit(a, asm2(mov_for(ir->src->t), l, r));
}

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
    // TODO
}

static void asm_sh(Assembler *a, IrIns *ir) {
    // TODO
}

static void asm_postamble(Assembler *a);

static void asm_ret(Assembler *a, IrIns *ir) {
    if (ir->ret) {
        AsmOpr *val = inline_imm_mem(a, ir->ret);
        if (ir->ret->t->k == IRT_F32 || ir->ret->t->k == IRT_F64) {
            emit(a, asm2(mov_for(ir->ret->t), opr_xmm(XMM0), val));
        } else {
            // Zero the rest of eax using movsx if the function returns
            // something smaller than an int
            AsmOpr *dst = opr_gpr(RAX, ir->ret->t->size == 8 ? R64 : R32);
            int mov_op = ir->ret->t->size < 4 ? X64_MOVSX : X64_MOV;
            emit(a, asm2(mov_op, dst, val));
        }
    }
    asm_postamble(a);
    emit(a, asm0(X64_RET));
}

static void asm_ins(Assembler *a, IrIns *ir) {
    switch (ir->op) {
    // Constants and globals
    case IR_IMM:    break; // Always inlined
    case IR_FP:     asm_fp(a, ir); break;
    case IR_GLOBAL: break; // Always inlined

        // Memory access
    case IR_FARG:  assert(0); // TODO
    case IR_ALLOC: asm_alloc(a, ir); break;
    case IR_LOAD:  asm_load(a, ir); break;
    case IR_STORE: asm_store(a, ir); break;
    case IR_IDX:   assert(0); // TODO

        // Arithmetic
    case IR_ADD: case IR_SUB: case IR_MUL:
    case IR_BIT_AND: case IR_BIT_OR: case IR_BIT_XOR:
        asm_arith(a, ir);
        break;
    case IR_DIV:
        if (ir->t->k == IRT_F32 || ir->t->k == IRT_F64) {
            asm_arith(a, ir);
        } else {
            asm_div_mod(a, ir);
        }
        break;
    case IR_MOD: asm_div_mod(a, ir); break;
    case IR_SHL: case IR_SHR: asm_sh(a, ir); break;

        // Comparisons
    case IR_EQ: case IR_NEQ: case IR_LT: case IR_LE: case IR_GT: case IR_GE:
        break; // Handled by CONDBR

        // Control flow
    case IR_RET: asm_ret(a, ir); break;
    default: assert(0); // TODO
    }
}

static void asm_bb(Assembler *a, IrBB *ir_bb) {
    for (IrIns *ins = ir_bb->head; ins; ins = ins->next) {
        asm_ins(a, ins);
    }
}


// ---- Functions -------------------------------------------------------------

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
    a->next_stack = align_to(a->next_stack, STACK_ALIGN);
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

static AsmFn * asm_fn(IrFn *ir_fn) {
    AsmFn *fn = new_fn();
    Assembler *a = new_asm(fn);
    asm_preamble(a);
    for (IrBB *ir_bb = ir_fn->entry; ir_bb; ir_bb = ir_bb->next) {
        emit_bb(a);
        asm_bb(a, ir_bb);
    }
    patch_stack_sizes(a);
    fn->num_gprs = a->next_gpr;
    fn->num_sse = a->next_sse;
    return fn;
}

Vec * assemble(Vec *global) {
    Vec *fns = vec_new();
    for (size_t i = 0; i < vec_len(global); i++) {
        Global *g = vec_get(global, i);
        if (g->ir_fn) {
            g->asm_fn = asm_fn(g->ir_fn);
        }
    }
    return fns;
}
