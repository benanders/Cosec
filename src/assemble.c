
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

static AsmIns * asm1(int op, AsmOp *l) {
    AsmIns *ins = asm0(op);
    ins->l = l;
    return ins;
}

static AsmIns * asm2(int op, AsmOp *l, AsmOp *r) {
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

static AsmOp * discharge(Assembler *a, IrIns *ir);

static AsmOp * op_new(int k) {
    AsmOp *op = calloc(1, sizeof(AsmOp));
    op->k = k;
    return op;
}

static AsmOp * op_imm(uint64_t imm) {
    AsmOp *op = op_new(OP_IMM);
    op->imm = imm;
    return op;
}

static AsmOp * op_fp(IrType *t, size_t idx) {
    AsmOp *op = op_new(t->k == IRT_F32 ? OP_F32 : OP_F64);
    op->fp = idx;
    return op;
}

static AsmOp * op_gpr(int reg, int size) {
    AsmOp *op = op_new(OP_GPR);
    op->reg = reg;
    op->size = size;
    return op;
}

static AsmOp * op_gpr_t(int reg, IrType *t) {
    int size;
    switch (t->size) {
        case 1: size = R8L; break;
        case 2: size = R16; break;
        case 4: size = R32; break;
        case 8: size = R64; break;
        default: UNREACHABLE();
    }
    return op_gpr(reg, size);
}

static AsmOp * op_xmm(int reg) {
    AsmOp *op = op_new(OP_XMM);
    op->reg = reg;
    return op;
}

static AsmOp * op_deref(char *label, int bytes) {
    AsmOp *op = op_new(OP_DEREF);
    op->label = label;
    op->bytes = bytes;
    return op;
}

// 'mem->bytes' MUST be set by the calling function if memory is to be accessed
static AsmOp * op_mem_from_ptr(Assembler *a, IrIns *ptr) {
    assert(ptr->t->k == IRT_PTR);
    AsmOp *l = discharge(a, ptr);
    assert(l->k == OP_GPR);
    assert(l->size == R64);
    AsmOp *mem = op_new(OP_MEM); // [<reg>]
    mem->base = l->reg;
    mem->base_size = R64;
    mem->scale = 1;
    return mem;
}

static AsmOp * op_mem_from_alloc(IrIns *alloc) {
    assert(alloc->op == IR_ALLOC);
    assert(alloc->t->k == IRT_PTR);
    AsmOp *mem = op_new(OP_MEM); // [rbp - <stack slot>]
    mem->base = RBP;
    mem->base_size = R64;
    mem->scale = 1;
    mem->disp = -alloc->stack_slot;
    return mem;
}

static AsmOp * op_mem_from_global(IrIns *global) {
    assert(global->op == IR_GLOBAL);
    assert(global->t->k == IRT_PTR);
    return op_deref(global->g->label, 0);
}

static AsmOp * load_ptr(Assembler *a, IrIns *to_load) {
    assert(to_load->t->k == IRT_PTR);
    switch (to_load->op) {
        case IR_ALLOC:  return op_mem_from_alloc(to_load);
        case IR_GLOBAL: return op_mem_from_global(to_load);
        default:        return op_mem_from_ptr(a, to_load);
    }
}

static AsmOp * alloc_vreg(Assembler *a, IrIns *ir) {
    if (ir->t->k == IRT_F32 || ir->t->k == IRT_F64) {
        ir->vreg = a->next_sse++;
        return op_xmm(ir->vreg);
    } else {
        ir->vreg = a->next_gpr++;
        return op_gpr_t(ir->vreg, ir->t);
    }
}

static int mov_for(IrType *t) {
    switch (t->k) {
        case IRT_F32: return X64_MOVSS;
        case IRT_F64: return X64_MOVSD;
        default: return X64_MOV;
    }
}

// Emit assembly to put the result of an instruction into a vreg and returns
// the vreg as an operand
static AsmOp * discharge(Assembler *a, IrIns *ir) {
    if (ir->vreg > 0) { // Already in a vreg
        if (ir->t->k == IRT_F32 || ir->t->k == IRT_F64) {
            return op_xmm(ir->vreg);
        } else {
            return op_gpr_t(ir->vreg, ir->t);
        }
    }
    int mov = mov_for(ir->t);
    AsmOp *op = alloc_vreg(a, ir);
    switch (ir->op) {
        case IR_IMM:    emit(a, asm2(mov, op, op_imm(ir->imm))); break;
        case IR_FP:     emit(a, asm2(mov, op, op_fp(ir->t, ir->fp_idx))); break;
        case IR_GLOBAL: emit(a, asm2(X64_LEA, op, op_deref(ir->g->label, 0))); break;
        case IR_LOAD:   emit(a, asm2(mov, op, load_ptr(a, ir->l))); break;
        case IR_ALLOC:  emit(a, asm2(X64_LEA, op, op_mem_from_alloc(ir))); break;
        // TODO: comparison operations
        default: UNREACHABLE();
    }
    return op;
}

static AsmOp * inline_imm(Assembler *a, IrIns *ir) {
    if (ir->op == IR_IMM) {
        return op_imm(ir->imm);
    }
    return discharge(a, ir);
}

static AsmOp * inline_mem(Assembler *a, IrIns *ir) {
    if (ir->op == IR_LOAD) {
        if (ir->vreg > 0) { // Already in a vreg
            return discharge(a, ir);
        }
        return load_ptr(a, ir->l);
    } else if (ir->op == IR_FP) {
        return op_fp(ir->t, ir->fp_idx);
    } else {
        return discharge(a, ir);
    }
}

static AsmOp * inline_imm_mem(Assembler *a, IrIns *ir) {
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
