
#include <stdlib.h>

#include "assemble.h"

typedef struct { // Per-function assembler
    AsmFn *fn;
    int next_gpr, next_sse, next_stack;
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
    mem->disp = -alloc->stack_slot;
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


// ---- Functions -------------------------------------------------------------

static AsmFn * asm_fn(IrFn *ir_fn) {
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
