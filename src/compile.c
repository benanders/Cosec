
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "compile.h"
#include "error.h"

#define GLOBAL_PREFIX "_G."

enum {
    SCOPE_FILE   = 0b0001,
    SCOPE_BLOCK  = 0b0010,
    SCOPE_LOOP   = 0b0100,
    SCOPE_SWITCH = 0b1000,
};

typedef struct {
    char *label;
    BB **br;
    Token *err;
} Goto;

typedef struct Scope {
    struct Scope *outer;
    int k;
    Vec *globals;
    Fn *fn;
    Map *vars;      // Block: 'IrIns *' k = IR_ALLOC; file: 'Global *'
    Vec *breaks;    // SCOPE_LOOP and SCOPE_SWITCH 'break' jump list
    Vec *continues; // SCOPE_LOOP; 'continue' jump list
    Map *labels; // of 'BB *'
    Vec *gotos;  // of 'Goto *'
} Scope;


// ---- Scopes, Globals, Functions, Basic Blocks, and Instructions ------------

static Scope enter_scope(Scope *outer, int k) {
    Scope s = {0};
    s.outer = outer;
    s.k = k;
    s.globals = outer->globals;
    s.fn = outer->fn;
    s.vars = map_new();
    if (k == SCOPE_LOOP) {
        s.breaks = vec_new();
        s.continues = vec_new();
    } else if (k == SCOPE_SWITCH) {
        s.breaks = vec_new();
    }
    s.labels = outer->labels;
    s.gotos = outer->gotos;
    return s;
}

static Scope * find_scope(Scope *s, int k) {
    while (s && !(s->k & k)) {
        s = s->outer;
    }
    return s;
}

static Global * new_global(char *label, IrType *t, int linkage) {
    Global *g = calloc(1, sizeof(Global));
    g->k = G_NONE;
    g->label = label;
    g->t = t;
    g->linkage = linkage;
    return g;
}

static InitElem * new_init_elem(uint64_t offset, Global *val) {
    InitElem *elem = malloc(sizeof(InitElem));
    elem->offset = offset;
    elem->val = val;
    return elem;
}

static BB * new_bb() {
    BB *bb = malloc(sizeof(BB));
    bb->next = bb->prev = NULL;
    bb->ir_head = bb->ir_last = NULL;
    bb->asm_head = bb->asm_last = NULL;
    bb->n = 0;

    // For assembler
    bb->pred = vec_new();
    bb->succ = vec_new();
    bb->live_in = NULL;
    return bb;
}

static Fn * new_fn() {
    Fn *fn = malloc(sizeof(Fn));
    fn->entry = fn->last = new_bb();

    // For assembler
    fn->f32s = vec_new();
    fn->f64s = vec_new();
    fn->num_gprs = fn->num_sse = 0;
    return fn;
}

static IrIns * new_ins(int op, IrType *t) {
    IrIns *ins = calloc(1, sizeof(IrIns));
    ins->op = op;
    ins->t = t;
    return ins;
}

static BB * emit_bb(Scope *s) {
    assert(s->fn); // Not top level
    if (!s->fn->last->ir_last) {
        return s->fn->last; // Current BB is empty, use that
    }
    BB *bb = new_bb();
    bb->prev = s->fn->last;
    if (s->fn->last) {
        s->fn->last->next = bb;
    } else {
        s->fn->entry = bb;
    }
    s->fn->last = bb;
    return bb;
}

static IrIns * emit_to_bb(BB *bb, IrIns *ins) {
    ins->bb = bb;
    ins->prev = bb->ir_last;
    ins->next = NULL; // Just in case
    if (bb->ir_last) {
        bb->ir_last->next = ins;
    } else {
        bb->ir_head = ins;
    }
    bb->ir_last = ins;
    return ins;
}

static IrIns * emit(Scope *s, int op, IrType *t) {
    assert(s->fn->last);
    IrIns *ins = new_ins(op, t);
    if (op == IR_CONDBR) {
        ins->true_chain = vec_new();
        ins->false_chain = vec_new();
    }
    if (op == IR_PHI) {
        ins->preds = vec_new();
        ins->defs = vec_new();
    }
    emit_to_bb(s->fn->last, ins);
    return ins;
}

static void delete_ir(IrIns *ins) {
    if (ins->prev) {
        ins->prev->next = ins->next;
    } else { // Head of the linked list
        ins->bb->ir_head = ins->next;
    }
    if (ins->next) {
        ins->next->prev = ins->prev;
    }
    if (ins->bb->ir_last == ins) {
        ins->bb->ir_last = ins->prev;
    }
}


// ---- IR Types --------------------------------------------------------------

static IrType * irt_new(int k) {
    IrType *t = calloc(1, sizeof(IrType));
    t->k = k;
    switch (k) {
    case IRT_I8:  t->size = t->align = 1; break;
    case IRT_I16: t->size = t->align = 2; break;
    case IRT_I32: case IRT_F32: t->size = t->align = 4; break;
    case IRT_I64: case IRT_F64: case IRT_PTR:
        t->size = t->align = 8;
        break;
    case IRT_ARR: t->align = 8; break;
    default: break;
    }
    return t;
}

static IrField * irt_field(IrType *t, size_t offset) {
    IrField *f = malloc(sizeof(IrField));
    f->t = t;
    f->offset = offset;
    return f;
}

static IrType * irt_conv(AstType *t) {
    switch (t->k) {
    case T_VOID:  return irt_new(IRT_VOID);
    case T_CHAR:  return irt_new(IRT_I8);
    case T_SHORT: return irt_new(IRT_I16);
    case T_INT: case T_LONG: return irt_new(IRT_I32);
    case T_LLONG: return irt_new(IRT_I64);
    case T_FLOAT: return irt_new(IRT_F32);
    case T_DOUBLE: case T_LDOUBLE: return irt_new(IRT_F64);
    case T_PTR: case T_FN: return irt_new(IRT_PTR);
    case T_ARR:
        assert(t->len->k == N_IMM); // Not VLA
        IrType *arr = irt_new(IRT_ARR);
        arr->elem = irt_conv(t->elem);
        arr->len = t->len->imm;
        arr->size = t->size;
        return arr;
    case T_STRUCT:
        assert(t->fields);
        IrType *obj = irt_new(IRT_STRUCT);
        obj->fields = vec_new();
        for (size_t i = 0; i < vec_len(t->fields); i++) {
            Field *f = vec_get(t->fields, i);
            vec_push(obj->fields, irt_field(irt_conv(f->t), f->offset));
        }
        obj->size = t->size;
        obj->align = t->align;
        return obj;
    case T_UNION:
        assert(t->fields);
        IrType *max = NULL;
        for (size_t i = 0; i < vec_len(t->fields); i++) {
            AstType *ft = ((Field *) vec_get(t->fields, i))->t;
            IrType *v = irt_conv(ft);
            max = (!max || v->size > max->size) ? v : max;
        }
        assert(t->size == max->size && t->align == max->align);
        return max;
    case T_ENUM: return irt_conv(t->num_t);
    default: UNREACHABLE();
    }
}

static int is_int(IrType *t) {
    return t->k >= IRT_I8 && t->k <= IRT_I64;
}

static int is_fp(IrType *t) {
    return t->k >= IRT_F32 && t->k <= IRT_F64;
}


// ---- Local and Global Variables --------------------------------------------

static void def_local(Scope *s, char *name, IrIns *alloc) {
    assert(alloc->op == IR_ALLOC);
    assert(s->outer); // Not top level
    map_put(s->vars, name, alloc);
}

static void def_global(Scope *s, char *name, Global *g) {
    for (size_t i = 0; i < vec_len(s->globals); i++) {
        Global *g2 = vec_get(s->globals, i);
        if (g2->k == G_NONE) { // Not a forward declaration
            continue;
        }
        assert(strcmp(g->label, g2->label) != 0); // No duplicate definitions
    }
    vec_push(s->globals, g);
    if (name) {
        Scope *gs = find_scope(s, SCOPE_FILE);
        assert(gs);
        map_put(gs->vars, name, g);
    }
}

static char * next_global_label(Scope *s) {
    size_t i = vec_len(s->globals);
    int num_digits = (i == 0) ? 1 : (int) log10((double) i) + 1;
    char *label = malloc(sizeof(char) * (num_digits + strlen(GLOBAL_PREFIX) + 1));
    sprintf(label, GLOBAL_PREFIX "%zu", i);
    return label;
}

static void compile_global(Scope *s, AstNode *n, Global *g);

static Global * def_const_global(Scope *s, AstNode *n) {
    char *label = next_global_label(s);
    Global *g = new_global(label, irt_conv(n->t), n->t->linkage);
    def_global(s, NULL, g);
    compile_global(s, n, g);
    return g;
}

static IrIns * find_local(Scope *s, char *name) {
    while (s->outer) { // Not global scope
        IrIns *ins = map_get(s->vars, name);
        if (ins) return ins;
        s = s->outer;
    }
    return NULL;
}

static Global * find_global(Scope *s, char *name) {
    Scope *gs = find_scope(s, SCOPE_FILE);
    return map_get(gs->vars, name);
}


// ---- Expressions -----------------------------------------------------------

static int INVERT_COND[IR_LAST] = {
    [IR_EQ] = IR_NEQ, [IR_NEQ] = IR_EQ,
    [IR_SLT] = IR_SGE, [IR_SLE] = IR_SGT, [IR_SGT] = IR_SLE, [IR_SGE] = IR_SLT,
    [IR_ULT] = IR_UGE, [IR_ULE] = IR_UGT, [IR_UGT] = IR_ULE, [IR_UGE] = IR_ULT,
    [IR_FLT] = IR_FGE, [IR_FLE] = IR_FGT, [IR_FGT] = IR_FLE, [IR_FGE] = IR_FLT,
};

static IrIns * compile_expr(Scope *s, AstNode *n);

static void add_to_branch_chain(Vec *bcs, BB **bb, IrIns *ins) {
    BrChain *bc = malloc(sizeof(BrChain));
    bc->bb = bb;
    bc->ins = ins;
    vec_push(bcs, bc);
}

static void patch_branch_chain(Vec *bcs, BB *target) {
    for (size_t i = 0; i < vec_len(bcs); i++) {
        BrChain *bc = vec_get(bcs, i);
        *bc->bb = target;
    }
    vec_empty(bcs);
}

static void merge_branch_chains(Vec *bcs, Vec *to_append) {
    for (size_t i = 0; i < vec_len(to_append); i++) {
        BrChain *bc = vec_get(to_append, i);
        vec_push(bcs, bc);
    }
}

static void add_phi(IrIns *phi, BB *pred, IrIns *def) {
    assert(phi->op == IR_PHI);
    vec_push(phi->preds, pred);
    vec_push(phi->defs, def);
}

static IrIns * discharge(Scope *s, IrIns *br) {
    if (br->op != IR_CONDBR) {
        return br; // Doesn't need discharging
    }
    if (vec_len(br->true_chain) == 1 && vec_len(br->false_chain) == 1) {
        IrIns *cond = br->cond;
        if (*((BrChain *) vec_get(br->false_chain, 0))->bb == br->false) {
            cond->op = INVERT_COND[cond->op]; // Negate
        }
        delete_ir(br);
        return cond;
    }
    BB *bb = emit_bb(s);
    IrIns *k_true = emit(s, IR_IMM, irt_new(IRT_I32));
    k_true->imm = 1;
    IrIns *k_false = emit(s, IR_IMM, irt_new(IRT_I32));
    k_false->imm = 0;
    IrIns *phi = emit(s, IR_PHI, br->t);
    for (size_t i = 0; i < vec_len(br->true_chain); i++) {
        BrChain *bc = vec_get(br->true_chain, i);
        if (bc->ins != br) { // Handle last condition separately
            add_phi(phi, bc->ins->bb, k_true);
        }
    }
    for (size_t i = 0; i < vec_len(br->false_chain); i++) {
        BrChain *bc = vec_get(br->false_chain, i);
        if (bc->ins != br) { // Handle last condition separately
            add_phi(phi, bc->ins->bb, k_false);
        }
    }
    patch_branch_chain(br->true_chain, bb);
    patch_branch_chain(br->false_chain, bb);
    add_phi(phi, br->bb, br->cond); // Add last condition separately
    br->op = IR_BR; // Change last condition to unconditional branch
    br->br = bb;
    return phi;
}

static IrIns * to_cond(Scope *s, IrIns *cond) {
    if (cond->op == IR_CONDBR) {
        return cond; // Already a condition
    }
    cond = discharge(s, cond);
    if (cond->op < IR_EQ || cond->op > IR_FGE) { // Not a comparison
        IrIns *zero = emit(s, IR_IMM, cond->t);
        zero->imm = 0;
        IrIns *cmp = emit(s, IR_NEQ, irt_new(IRT_I32));
        cmp->l = cond;
        cmp->r = zero;
        cond = cmp;
    }
    IrIns *br = emit(s, IR_CONDBR, NULL);
    br->cond = cond;
    add_to_branch_chain(br->true_chain, &br->true, br);
    add_to_branch_chain(br->false_chain, &br->false, br);
    return br;
}

static IrIns * emit_conv(Scope *s, IrIns *src, AstType *ast_st, IrType *dt) {
    int op;
    IrType *st = src->t;
    if (is_int(st) && is_fp(dt)) {
        op = IR_I2FP;
    } else if (is_fp(st) && is_int(dt)) {
        op = IR_FP2I;
    } else if (is_int(st) && is_int(dt)) {
        if (dt->size == st->size) return src; // No conversion needed
        op = dt->size < st->size ? IR_TRUNC : (ast_st->is_unsigned ? IR_ZEXT : IR_SEXT);
    } else if (is_fp(st) && is_fp(dt)) {
        if (dt->size == st->size) return src; // No conversion needed
        op = dt->size < st->size ? IR_FTRUNC : IR_FEXT;
    } else if (is_int(st) && (dt->k == IRT_PTR || dt->k == IRT_ARR)) {
        op = IR_I2PTR;
    } else if ((st->k == IRT_PTR || st->k == IRT_ARR) && is_int(dt)) {
        op = IR_PTR2I;
    } else if (st->k == IRT_ARR && dt->k == IRT_PTR) {
        IrIns *zero = emit(s, IR_IMM, irt_new(IRT_I64));
        zero->imm = 0;
        IrIns *idx = emit(s, IR_PTRADD, dt);
        idx->base = src;
        idx->offset = zero;
        return idx;
    } else if (st->k == IRT_PTR && dt->k == IRT_ARR) {
        op = IR_BITCAST;
    } else if ((st->k == IRT_PTR || st->k == IRT_ARR) &&
               (dt->k == IRT_PTR || dt->k == IRT_ARR)) {
        return src; // No conversion necessary
    } else {
        UNREACHABLE();
    }
    IrIns *conv = emit(s, op, dt);
    conv->l = src;
    return conv;
}

static IrIns * emit_load(Scope *s, IrIns *src, AstType *t) {
    // 't' is the type of the object to load from the pointer 'src'
    // The only valid operations on aggregate types are:
    // * T_STRUCT/T_UNION: field access (., ->), assign (=), addr (&), comma (,), ternary (?)
    // * T_ARR: member access ([]), addr (&), comma (,), ternary (?), ptr arith
    assert(src->t->k == IRT_PTR);
    if (t->k == T_ARR || t->k == T_STRUCT || t->k == T_UNION || t->k == T_FN) {
        return src; // Aggregates loaded on field access
    } else { // Base types
        IrIns *load = emit(s, IR_LOAD, irt_conv(t));
        load->src = src;
        return load;
    }
}

static void compile_init_elem(Scope *s, AstNode *n, AstType *t, IrIns *elem);

static void compile_array_init_raw(Scope *s, AstNode *n, IrIns *elem) {
    assert(n->t->k == T_ARR);
    for (size_t i = 0; i < vec_len(n->elems); i++) {
        AstNode *v = vec_get(n->elems, i);
        compile_init_elem(s, v, n->t->elem, elem);
        if (i < vec_len(n->elems) - 1) {
            IrIns *offset = emit(s, IR_IMM, irt_new(IRT_I64));
            offset->imm = n->t->elem->size;
            IrIns *next = emit(s, IR_PTRADD, irt_new(IRT_PTR));
            next->base = elem;
            next->offset = offset;
            elem = next;
        }
    }
}

static void compile_struct_init_raw(Scope *s, AstNode *n, IrIns *obj) {
    assert(n->t->k == T_STRUCT || n->t->k == T_UNION);
    for (size_t i = 0; i < vec_len(n->elems); i++) {
        Field *f = vec_get(n->t->fields, i);
        IrIns *offset = emit(s, IR_IMM, irt_new(IRT_I64));
        offset->imm = f->offset;
        IrIns *idx = emit(s, IR_PTRADD, irt_new(IRT_PTR));
        idx->base = obj;
        idx->offset = offset;
        AstNode *v = vec_get(n->elems, i);
        compile_init_elem(s, v, f->t, idx);
    }
}

static void compile_init_elem(Scope *s, AstNode *n, AstType *t, IrIns *elem) {
    if (n) {
        if (t->k == T_ARR) {
            compile_array_init_raw(s, n, elem);
        } else if (t->k == T_STRUCT || t->k == T_UNION) {
            compile_struct_init_raw(s, n, elem);
        } else {
            IrIns *ins = discharge(s, compile_expr(s, n));
            IrIns *store = emit(s, IR_STORE, NULL);
            store->dst = elem;
            store->src = ins;
        }
    } else {
        if (t->k == T_ARR || t->k == T_STRUCT || t->k == T_UNION) {
            IrIns *size = emit(s, IR_IMM, irt_new(IRT_I64));
            size->imm = t->size;
            IrIns *zero = emit(s, IR_ZERO, NULL);
            zero->ptr = elem;
            zero->size = size;
        } else {
            IrIns *zero = emit(s, IR_IMM, irt_conv(t));
            zero->imm = 0;
            IrIns *store = emit(s, IR_STORE, NULL);
            store->dst = elem;
            store->src = zero;
        }
    }
}

static IrIns * compile_const_init(Scope *s, AstNode *n) {
    AstNode *const_init = try_calc_const_expr(n);
    if (!const_init) {
        return NULL;
    }
    Global *g = def_const_global(s, const_init);
    IrIns *src = emit(s, IR_GLOBAL, irt_new(IRT_PTR));
    src->g = g;
    IrIns *dst = emit(s, IR_ALLOC, irt_new(IRT_PTR));
    dst->alloc_t = irt_conv(n->t);
    IrIns *size = emit(s, IR_IMM, irt_new(IRT_I64));
    size->imm = dst->alloc_t->size;
    IrIns *copy = emit(s, IR_COPY, NULL);
    copy->src = src;
    copy->dst = dst;
    copy->len = size;
    return dst;
}

static IrIns * compile_init(Scope *s, AstNode *n) {
    assert(n->k == N_INIT);
    IrIns *const_init = compile_const_init(s, n);
    if (const_init) {
        return const_init;
    }
    IrIns *alloc = emit(s, IR_ALLOC, irt_new(IRT_PTR));
    alloc->alloc_t = irt_conv(n->t);
    compile_init_elem(s, n, n->t, alloc);
    return alloc;
}

static IrIns * compile_kptr(Scope *s, AstNode *n) {
    assert(n->g->k == N_GLOBAL);
    Global *g = find_global(s, n->g->var_name);
    assert(g); // Checked by parser
    IrIns *ins = emit(s, IR_GLOBAL, irt_new(IRT_PTR));
    ins->g = g;
    if (n->offset != 0) {
        IrIns *offset = emit(s, IR_IMM, irt_new(IRT_I64));
        offset->imm = n->offset < 0 ? -n->offset : n->offset;
        IrIns *arith = emit(s, n->offset < 0 ? IR_SUB : IR_ADD, irt_new(IRT_PTR));
        arith->l = ins;
        arith->r = offset;
        return arith;
    }
    return ins;
}

static IrIns * compile_operand(Scope *s, AstNode *n) {
    IrIns *ins;
    Global *g;
    switch (n->k) {
    case N_IMM:
        ins = emit(s, IR_IMM, irt_conv(n->t));
        ins->imm = n->imm;
        break;
    case N_FP:
        ins = emit(s, IR_FP, irt_conv(n->t));
        ins->fp = n->fp;
        break;
    case N_STR:
        assert(n->t->k == T_PTR);
        ins = emit(s, IR_GLOBAL, irt_new(IRT_PTR));
        ins->g = def_const_global(s, n);
        break;
    case N_INIT:
        ins = compile_init(s, n);
        break;
    case N_LOCAL:
        ins = find_local(s, n->var_name); // 'IR_ALLOC' ins
        assert(ins); // Checked by parser
        ins = emit_load(s, ins, n->t);
        break;
    case N_GLOBAL:
        g = find_global(s, n->var_name);
        assert(g); // Checked by parser
        ins = emit(s, IR_GLOBAL, irt_new(IRT_PTR));
        ins->g = g;
        ins = emit_load(s, ins, n->t);
        break;
    case N_KPTR:
        ins = compile_kptr(s, n);
        break;
    default: UNREACHABLE();
    }
    return ins;
}

static IrIns * compile_binop(Scope *s, AstNode *n, int op) {
    IrIns *l = discharge(s, compile_expr(s, n->l));
    IrIns *r = discharge(s, compile_expr(s, n->r));
    IrIns *ins = emit(s, op, irt_conv(n->t));
    ins->l = l;
    ins->r = r;
    return ins;
}

static IrIns * compile_ptr_sub(Scope *s, AstNode *n) { // <ptr> - <ptr>
    assert(n->l->t->k == T_PTR);
    assert(n->r->t->k == T_PTR);
    IrIns *l = discharge(s, compile_expr(s, n->l));
    IrIns *r = discharge(s, compile_expr(s, n->r));
    l = emit_conv(s, l, n->l->t, irt_conv(n->t)); // Convert to i64
    r = emit_conv(s, r, n->r->t, irt_conv(n->t));
    IrIns *sub = emit(s, IR_SUB, irt_conv(n->t));
    sub->l = l;
    sub->r = r;
    IrIns *size = emit(s, IR_IMM, irt_conv(n->t));
    size->imm = n->l->t->ptr->size;
    IrIns *div = emit(s, IR_UDIV, irt_conv(n->t));
    div->l = sub;
    div->r = size;
    return div;
}

static IrIns * compile_ptr_arith(Scope *s, AstNode *n) { // <ptr> +/- <int>
    IrIns *l = discharge(s, compile_expr(s, n->l));
    IrIns *r = discharge(s, compile_expr(s, n->r));
    IrIns *ptr = (n->l->t->k == T_PTR || n->l->t->k == T_ARR) ? l : r;
    IrIns *offset = (ptr == l) ? r : l;
    if (n->k == N_SUB || n->k == N_A_SUB) { // Negate the offset
        IrIns *zero = emit(s, IR_IMM, offset->t);
        zero->imm = 0;
        IrIns *sub = emit(s, IR_SUB, offset->t);
        sub->l = zero;
        sub->r = offset;
        offset = sub;
    }
    IrIns *vla = NULL;
    AstType *elem = (ptr == l) ? n->l->t->ptr : n->r->t->ptr;
    if (is_vla(elem)) {
        vla = emit_load(s, elem->vla_len, elem->len->t);
        elem = elem->elem;
        while (is_vla(elem)) {
            IrIns *len = emit_load(s, elem->vla_len, elem->len->t);
            IrIns *mul = emit(s, IR_MUL, vla->t);
            mul->l = vla;
            mul->r = len;
            vla = mul;
            elem = elem->elem;
        }
    }
    IrIns *scale = emit(s, IR_IMM, offset->t);
    scale->imm = elem->size;
    if (vla) {
        IrIns *mul = emit(s, IR_MUL, vla->t);
        mul->l = vla;
        mul->r = scale;
        scale = mul;
    }
    IrIns *mul = emit(s, IR_MUL, offset->t);
    mul->l = offset;
    mul->r = scale;
    IrIns *idx = emit(s, IR_PTRADD, irt_new(IRT_PTR));
    idx->base = ptr;
    idx->offset = mul;
    return idx;
}

static void emit_store(Scope *s, IrIns *dst, IrIns *src, AstType *t) {
    // 'src_t' is the type of the object to store into the pointer 'dst'
    assert(dst->t->k == IRT_PTR);
    assert(t->k != T_ARR); // Checked by parser
    if (t->k == T_STRUCT || t->k == T_UNION) { // Use IR_COPY for aggregates
        IrIns *size = emit(s, IR_IMM, irt_new(IRT_I64));
        size->imm = src->t->size;
        IrIns *copy = emit(s, IR_COPY, NULL);
        copy->src = src;
        copy->dst = dst;
        copy->len = size;
    } else {
        IrIns *store = emit(s, IR_STORE, NULL);
        store->dst = dst;
        store->src = src;
    }
}

static IrIns * compile_assign(Scope *s, AstNode *n) {
    IrIns *r = discharge(s, compile_expr(s, n->r));
    IrIns *l = compile_expr(s, n->l);
    IrIns *dst;
    if (l->op == IR_LOAD) { // Base types
        dst = l->src;
        delete_ir(l);
    } else { // Aggregates (T_STRUCT, T_UNION)
        dst = l;
    } // Can't assign to T_ARR
    emit_store(s, dst, r, n->r->t);
    return r; // Assignment evaluates to its right operand
}

static IrIns * compile_arith_assign(Scope *s, AstNode *n, int op) {
    IrIns *binop = compile_binop(s, n, op);

    IrIns *lvalue = binop->l;
    if (lvalue->op != IR_LOAD) {
        lvalue = lvalue->l; // IR_CONV getting in the way
    }
    assert(lvalue->op == IR_LOAD);

    AstType *target = n->l->t;
    if (n->l->k == N_CONV) {
        target = n->l->l->t; // Type conversion getting in the way
    }
    IrType *ir_target = irt_conv(target);
    // May need to emit a truncation, e.g. 'char a = 3; char *b = &a; *b += 1'
    if (binop->t->k != ir_target->k) {
        emit_conv(s, binop, n->l->t, ir_target);
    }
    emit_store(s, lvalue->src, binop, n->t);
    return binop; // Assignment evaluates to its right operand
}

static IrIns * compile_and(Scope *s, AstNode *n) {
    IrIns *l = to_cond(s, compile_expr(s, n->l));
    BB *r_bb = emit_bb(s);
    patch_branch_chain(l->true_chain, r_bb);

    IrIns *r = to_cond(s, compile_expr(s, n->r));
    merge_branch_chains(r->false_chain, l->false_chain);
    return r;
}

static IrIns * compile_or(Scope *s, AstNode *n) {
    IrIns *l = to_cond(s, compile_expr(s, n->l));
    BB *r_bb = emit_bb(s);
    patch_branch_chain(l->false_chain, r_bb);

    IrIns *r = to_cond(s, compile_expr(s, n->r));
    merge_branch_chains(r->true_chain, l->true_chain);
    return r;
}

static IrIns * compile_comma(Scope *s, AstNode *n) {
    discharge(s, compile_expr(s, n->l)); // Discard left operand
    return compile_expr(s, n->r);
}

static IrIns * compile_ternary(Scope *s, AstNode *n) {
    IrIns *cond = to_cond(s, compile_expr(s, n->if_cond));

    BB *true_bb = emit_bb(s);
    patch_branch_chain(cond->true_chain, true_bb);
    IrIns *true = discharge(s, compile_expr(s, n->if_body));
    IrIns *true_br = emit(s, IR_BR, NULL);

    BB *false_bb = emit_bb(s);
    patch_branch_chain(cond->false_chain, false_bb);
    IrIns *false = discharge(s, compile_expr(s, n->if_else));
    IrIns *false_br = emit(s, IR_BR, NULL);

    BB *after = emit_bb(s);
    true_br->br = after;
    false_br->br = after;

    IrIns *phi = emit(s, IR_PHI, irt_conv(n->t));
    add_phi(phi, true_bb, true);
    add_phi(phi, false_bb, false);
    return phi;
}

static IrIns * compile_neg(Scope *s, AstNode *n) {
    IrIns *l = discharge(s, compile_expr(s, n->l));
    IrIns *zero = emit(s, IR_IMM, irt_conv(n->t));
    zero->imm = 0;
    IrIns *sub = emit(s, IR_SUB, irt_conv(n->t));
    sub->l = zero;
    sub->r = l;
    return sub;
}

static IrIns * compile_bit_not(Scope *s, AstNode *n) {
    IrIns *l = discharge(s, compile_expr(s, n->l));
    IrIns *neg1 = emit(s, IR_IMM, irt_conv(n->t));
    neg1->imm = -1;
    IrIns *xor = emit(s, IR_BIT_XOR, irt_conv(n->t));
    xor->l = l;
    xor->r = neg1;
    return xor;
}

static IrIns * compile_log_not(Scope *s, AstNode *n) {
    IrIns *l = to_cond(s, compile_expr(s, n->l));
    assert(l->op == IR_CONDBR);
    Vec *swap = l->true_chain; // Swap true and false chains
    l->true_chain = l->false_chain;
    l->false_chain = swap;
    return l;
}

static IrIns * compile_inc_dec(Scope *s, AstNode *n) {
    int is_sub = (n->k == N_PRE_DEC || n->k == N_POST_DEC);
    AstType ptr_t = { .k = T_LLONG, .is_unsigned = 1 };
    AstType *t = n->t->k == T_PTR ? &ptr_t : n->t;
    AstNode one = { .k = N_IMM, .t = t, .imm = 1 };
    AstNode op = { .k = is_sub ? N_SUB : N_ADD, .t = n->t, .l = n->l, .r = &one };
    IrIns *result = compile_expr(s, &op);
    IrIns *lvalue = result->l;
    assert(lvalue->op == IR_LOAD);
    emit_store(s, lvalue->src, result, n->t);
    int is_prefix = (n->k == N_PRE_INC || n->k == N_PRE_DEC);
    if (is_prefix) {
        return result;
    } else {
        return lvalue;
    }
}

static IrIns * compile_deref(Scope *s, AstNode *n) {
    IrIns *op = discharge(s, compile_expr(s, n->l));
    return emit_load(s, op, n->t);
}

static IrIns * compile_addr(Scope *s, AstNode *n) {
    IrIns *l = compile_expr(s, n->l);
    if (l->op == IR_LOAD) { // Base types
        delete_ir(l);
        return l->src;
    } else { // Aggregates (T_ARR, T_STRUCT, T_UNION) and T_FN
        return l;
    }
}

static IrIns * compile_conv(Scope *s, AstNode *n) {
    IrIns *l = discharge(s, compile_expr(s, n->l));
    return emit_conv(s, l, n->l->t, irt_conv(n->t));
}

static IrIns * compile_array_access(Scope *s, AstNode *n) {
    // TODO: VLA access (via multiplication)
    IrIns *ptr = compile_ptr_arith(s, n);
    return emit_load(s, ptr, n->t);
}

static IrIns * compile_struct_field_access(Scope *s, AstNode *n) {
    IrIns *ptr = discharge(s, compile_expr(s, n->obj));
    Field *f = vec_get(n->obj->t->fields, n->field_idx);
    IrIns *offset = emit(s, IR_IMM, irt_new(IRT_I64));
    offset->imm = f->offset;
    IrIns *idx = emit(s, IR_PTRADD, irt_new(IRT_PTR));
    idx->base = ptr;
    idx->offset = offset;
    return emit_load(s, idx, f->t);
}

static IrIns * compile_union_field_access(Scope *s, AstNode *n) {
    IrIns *obj = discharge(s, compile_expr(s, n->obj));
    AstType *ft = ((Field *) vec_get(n->obj->t->fields, n->field_idx))->t;
    return emit_load(s, obj, ft);
}

static IrIns * compile_field_access(Scope *s, AstNode *n) {
    assert(n->obj->t->k == T_STRUCT || n->obj->t->k == T_UNION);
    if (n->obj->t->k == T_STRUCT) {
        return compile_struct_field_access(s, n);
    } else { // T_UNION
        return compile_union_field_access(s, n);
    }
}

static IrIns * compile_call(Scope *s, AstNode *n) {
    AstType *fn_t = n->fn->t;
    if (fn_t->k == T_PTR) {
        fn_t = fn_t->ptr;
    }
    assert(fn_t->k == T_FN);
    IrIns *fn = discharge(s, compile_expr(s, n->fn));
    IrIns *args[vec_len(n->args)];
    for (size_t i = 0; i < vec_len(n->args); i++) {
        AstNode *arg = vec_get(n->args, i);
        args[i] = discharge(s, compile_expr(s, arg));
    }
    IrIns *call = emit(s, IR_CALL, irt_conv(n->t));
    call->fn = fn;
    for (size_t i = 0; i < vec_len(n->args); i++) {
        AstNode *arg = vec_get(n->args, i);
        IrIns *carg = emit(s, IR_CARG, irt_conv(arg->t));
        carg->arg = args[i];
    }
    return call;
}

static IrIns * compile_expr(Scope *s, AstNode *n) {
    switch (n->k) {
        // Binary operations
    case N_ADD:
        if (n->l->t->k == T_PTR || n->r->t->k == T_PTR) {
            return compile_ptr_arith(s, n);
        }
        return compile_binop(s, n, IR_ADD);
    case N_SUB:
        if (n->l->t->k == T_PTR && n->r->t->k == T_PTR) {
            return compile_ptr_sub(s, n);
        } else if (n->l->t->k == T_PTR || n->r->t->k == T_PTR) {
            return compile_ptr_arith(s, n);
        }
        return compile_binop(s, n, IR_SUB);
    case N_MUL: return compile_binop(s, n, IR_MUL);
    case N_DIV:
        if (n->t->k >= T_FLOAT && n->t->k <= T_LDOUBLE) { // FP division
            return compile_binop(s, n, IR_FDIV);
        } else if (n->t->is_unsigned) { // Unsigned integer division
            return compile_binop(s, n, IR_UDIV);
        } else { // Signed integer division
            return compile_binop(s, n, IR_SDIV);
        }
    case N_MOD:
        if (n->t->is_unsigned) { // Unsigned integer modulo
            return compile_binop(s, n, IR_UMOD);
        } else { // Signed integer modulo
            return compile_binop(s, n, IR_SMOD);
        }
    case N_BIT_AND:   return compile_binop(s, n, IR_BIT_AND);
    case N_BIT_OR:    return compile_binop(s, n, IR_BIT_OR);
    case N_BIT_XOR:   return compile_binop(s, n, IR_BIT_XOR);
    case N_SHL:       return compile_binop(s, n, IR_SHL);
    case N_SHR:
        if (n->t->is_unsigned) { // Unsigned integer right shift
            return compile_binop(s, n, IR_SHR);
        } else { // Signed integer right shift
            return compile_binop(s, n, IR_SAR);
        }
    case N_EQ:        return compile_binop(s, n, IR_EQ);
    case N_NEQ:       return compile_binop(s, n, IR_NEQ);
    case N_LT:
        if (n->l->t->k >= T_FLOAT && n->l->t->k <= T_LDOUBLE) { // FP comparison
            return compile_binop(s, n, IR_FLT);
        } else if (n->l->t->is_unsigned) { // Unsigned integer comparison
            return compile_binop(s, n, IR_ULT);
        } else { // Signed integer comparison
            return compile_binop(s, n, IR_SLT);
        }
    case N_LE:
        if (n->l->t->k >= T_FLOAT && n->l->t->k <= T_LDOUBLE) { // FP comparison
            return compile_binop(s, n, IR_FLE);
        } else if (n->l->t->is_unsigned) { // Unsigned integer comparison
            return compile_binop(s, n, IR_ULE);
        } else { // Signed integer comparison
            return compile_binop(s, n, IR_SLE);
        }
    case N_GT:
        if (n->l->t->k >= T_FLOAT && n->l->t->k <= T_LDOUBLE) { // FP comparison
            return compile_binop(s, n, IR_FGT);
        } else if (n->l->t->is_unsigned) { // Unsigned integer comparison
            return compile_binop(s, n, IR_UGT);
        } else { // Signed integer comparison
            return compile_binop(s, n, IR_SGT);
        }
    case N_GE:
        if (n->l->t->k >= T_FLOAT && n->l->t->k <= T_LDOUBLE) { // FP comparison
            return compile_binop(s, n, IR_FGE);
        } else if (n->l->t->is_unsigned) { // Unsigned integer comparison
            return compile_binop(s, n, IR_UGE);
        } else { // Signed integer comparison
            return compile_binop(s, n, IR_SGE);
        }
    case N_LOG_AND:   return compile_and(s, n);
    case N_LOG_OR:    return compile_or(s, n);
    case N_ASSIGN:    return compile_assign(s, n);
    case N_A_ADD:     return compile_arith_assign(s, n, IR_ADD);
    case N_A_SUB:     return compile_arith_assign(s, n, IR_SUB);
    case N_A_MUL:     return compile_arith_assign(s, n, IR_MUL);
    case N_A_DIV:
        if (n->t->k >= T_FLOAT && n->t->k <= T_LDOUBLE) {
            return compile_arith_assign(s, n, IR_FDIV);
        } else if (n->t->is_unsigned) { // Unsigned integer division
            return compile_arith_assign(s, n, IR_UDIV);
        } else { // Signed integer division
            return compile_arith_assign(s, n, IR_SDIV);
        }
    case N_A_MOD:
        if (n->t->is_unsigned) { // Unsigned integer modulo
            return compile_arith_assign(s, n, IR_UMOD);
        } else { // Signed integer modulo
            return compile_arith_assign(s, n, IR_SMOD);
        }
    case N_A_BIT_AND: return compile_arith_assign(s, n, IR_BIT_AND);
    case N_A_BIT_OR:  return compile_arith_assign(s, n, IR_BIT_OR);
    case N_A_BIT_XOR: return compile_arith_assign(s, n, IR_BIT_XOR);
    case N_A_SHL:     return compile_arith_assign(s, n, IR_SHL);
    case N_A_SHR:
        if (n->t->is_unsigned) { // Unsigned integer right shift
            return compile_arith_assign(s, n, IR_SHR);
        } else { // Signed integer right shift
            return compile_arith_assign(s, n, IR_SAR);
        }
    case N_COMMA:     return compile_comma(s, n);
    case N_TERNARY:   return compile_ternary(s, n);

        // Unary operations
    case N_NEG:     return compile_neg(s, n);
    case N_BIT_NOT: return compile_bit_not(s, n);
    case N_LOG_NOT: return compile_log_not(s, n);
    case N_PRE_INC: case N_PRE_DEC: case N_POST_INC: case N_POST_DEC:
        return compile_inc_dec(s, n);
    case N_DEREF:   return compile_deref(s, n);
    case N_ADDR:    return compile_addr(s, n);
    case N_CONV:    return compile_conv(s, n);

        // Postfix operations
    case N_IDX:   return compile_array_access(s, n);
    case N_CALL:  return compile_call(s, n);
    case N_FIELD: return compile_field_access(s, n);

        // Operands
    default: return compile_operand(s, n);
    }
}


// ---- Statements ------------------------------------------------------------

static void compile_block(Scope *s, AstNode *n);
static void compile_stmt(Scope *s, AstNode *n);

static IrIns * compile_vla(Scope *s, AstType *t) {
    assert(is_vla(t));
    Vec *to_mul = vec_new();
    while (is_vla(t)) {
        IrIns *count = discharge(s, compile_expr(s, t->len));
        vec_push(to_mul, count);
        IrIns *len = emit(s, IR_ALLOC, irt_new(IRT_PTR));
        len->alloc_t = irt_conv(t->len->t);
        emit_store(s, len, count, t->len->t);
        t->vla_len = len;
        t = t->elem;
    }
    assert(vec_len(to_mul) > 0); // Is actually a VLA
    IrIns *total = vec_get(to_mul, 0);
    for (size_t i = 1; i < vec_len(to_mul); i++) {
        IrIns *mul = emit(s, IR_MUL, total->t);
        mul->l = total;
        mul->r = vec_get(to_mul, i);
        total = mul;
    }
    IrIns *alloc = emit(s, IR_ALLOC, irt_new(IRT_PTR));
    alloc->alloc_t = irt_conv(t);
    alloc->count = total;
    return alloc;
}

static void compile_decl(Scope *s, AstNode *n) {
    if (n->var->t->k == T_FN) {
        return; // Not an object; doesn't need stack space
    }
    assert(n->var->k == N_LOCAL);
    IrIns *alloc;
    if (n->val && n->val->k == N_INIT) { // Array/struct initializer list
        alloc = compile_init(s, n->val);
    } else if (is_vla(n->var->t)) { // VLA
        alloc = compile_vla(s, n->var->t);
    } else { // Everything else
        alloc = emit(s, IR_ALLOC, irt_new(IRT_PTR));
        alloc->alloc_t = irt_conv(n->var->t);
    }
    def_local(s, n->var->var_name, alloc);
    if (n->val && n->val->k != N_INIT) {
        IrIns *v = discharge(s, compile_expr(s, n->val));
        emit_store(s, alloc, v, n->val->t);
    }
}

static void compile_if(Scope *s, AstNode *n) {
    Vec *brs = vec_new();
    while (n && n->if_cond) { // 'if' and 'else if's
        IrIns *cond = to_cond(s, compile_expr(s, n->if_cond));
        BB *body = emit_bb(s);
        patch_branch_chain(cond->true_chain, body);
        compile_block(s, n->if_body);

        IrIns *end_br = emit(s, IR_BR, NULL);
        add_to_branch_chain(brs, &end_br->br, end_br);

        BB *after = emit_bb(s);
        patch_branch_chain(cond->false_chain, after);
        n = n->if_else;
    }
    if (n) { // else
        assert(!n->if_cond && !n->if_else);
        compile_block(s, n->if_body);
        IrIns *end_br = emit(s, IR_BR, NULL);
        add_to_branch_chain(brs, &end_br->br, end_br);
        emit_bb(s);
    }
    patch_branch_chain(brs, s->fn->last);
}

static void compile_while(Scope *s, AstNode *n) {
    IrIns *before_br = emit(s, IR_BR, NULL);
    BB *cond_bb = emit_bb(s);
    before_br->br = cond_bb;
    IrIns *cond = to_cond(s, compile_expr(s, n->loop_cond));

    Scope loop = enter_scope(s, SCOPE_LOOP);
    BB *body_bb = emit_bb(s);
    patch_branch_chain(cond->true_chain, body_bb);
    compile_block(&loop, n->loop_body);
    IrIns *end_br = emit(s, IR_BR, NULL);
    end_br->br = cond_bb;

    BB *after_bb = emit_bb(s);
    patch_branch_chain(cond->false_chain, after_bb);
    patch_branch_chain(loop.breaks, after_bb);
    patch_branch_chain(loop.continues, cond_bb);
}

static void compile_do_while(Scope *s, AstNode *n) {
    IrIns *before_br = emit(s, IR_BR, NULL);
    Scope loop = enter_scope(s, SCOPE_LOOP);
    BB *body_bb = emit_bb(s);
    before_br->br = body_bb;
    compile_block(&loop, n->loop_body);
    IrIns *body_br = emit(s, IR_BR, NULL);

    BB *cond_bb = emit_bb(s);
    body_br->br = cond_bb;
    IrIns *cond = to_cond(s, compile_expr(s, n->loop_cond));
    patch_branch_chain(cond->true_chain, body_bb);

    BB *after_bb = emit_bb(s);
    patch_branch_chain(cond->false_chain, after_bb);
    patch_branch_chain(loop.breaks, after_bb);
    patch_branch_chain(loop.continues, cond_bb);
}

static void compile_for(Scope *s, AstNode *n) {
    if (n->for_init) {
        compile_stmt(s, n->for_init);
    }

    IrIns *before_br = emit(s, IR_BR, NULL);
    BB *start_bb = NULL;
    IrIns *cond = NULL;
    if (n->for_cond) {
        start_bb = emit_bb(s);
        before_br->br = start_bb;
        cond = to_cond(s, compile_expr(s, n->for_cond));
    }

    Scope loop = enter_scope(s, SCOPE_LOOP);
    BB *body = emit_bb(s);
    if (cond) {
        patch_branch_chain(cond->true_chain, body);
    } else {
        start_bb = body;
        before_br->br = body;
    }
    compile_block(&loop, n->for_body);
    IrIns *end_br = emit(s, IR_BR, NULL);

    BB *continue_bb = NULL;
    if (n->for_inc) {
        BB *inc_bb = emit_bb(s);
        end_br->br = inc_bb;
        compile_expr(s, n->for_inc);
        IrIns *inc_br = emit(s, IR_BR, NULL);
        inc_br->br = start_bb;
        continue_bb = inc_bb;
    } else {
        end_br->br = start_bb;
        continue_bb = start_bb;
    }

    BB *after = emit_bb(s);
    if (cond) {
        patch_branch_chain(cond->false_chain, after);
    }
    patch_branch_chain(loop.breaks, after);
    patch_branch_chain(loop.continues, continue_bb);
}

static void compile_switch(Scope *s, AstNode *n) {
    IrIns *cond = discharge(s, compile_expr(s, n->switch_cond));
    for (size_t i = 0; i < vec_len(n->cases); i++) {
        AstNode *case_n = vec_get(n->cases, i);
        IrIns *val = compile_expr(s, case_n->case_cond);
        IrIns *cmp = emit(s, IR_EQ, irt_new(IRT_I32));
        cmp->l = cond;
        cmp->r = val;
        IrIns *br = emit(s, IR_CONDBR, NULL);
        br->cond = cmp;
        case_n->case_br = &br->true;
        BB *next = emit_bb(s);
        br->false = next;
    }
    IrIns *default_br = emit(s, IR_BR, NULL);
    emit_bb(s);
    if (n->default_n) { // If there's a default
        n->default_n->case_br = &default_br->br;
    }

    Scope switch_s = enter_scope(s, SCOPE_SWITCH);
    compile_block(&switch_s, n->switch_body);

    IrIns *end_br = emit(s, IR_BR, NULL);
    BB *after = emit_bb(s);
    end_br->br = after;
    patch_branch_chain(switch_s.breaks, after);
    if (!default_br->br) { // If there's no default
        default_br->br = after;
    }
    patch_branch_chain(switch_s.breaks, after);
}

static void compile_case_default(Scope *s, AstNode *n) {
    Scope *switch_s = find_scope(s, SCOPE_SWITCH);
    assert(switch_s); // Checked by parser
    IrIns *end_br = emit(s, IR_BR, NULL);
    BB *bb = emit_bb(s);
    end_br->br = bb;
    assert(n->case_br);
    *n->case_br = bb;
    compile_stmt(s, n->case_body);
}

static void compile_break(Scope *s) {
    Scope *switch_loop = find_scope(s, SCOPE_SWITCH | SCOPE_LOOP);
    assert(switch_loop); // Checked by parser
    IrIns *br = emit(s, IR_BR, NULL);
    add_to_branch_chain(switch_loop->breaks, &br->br, br);
}

static void compile_continue(Scope *s) {
    Scope *loop = find_scope(s, SCOPE_LOOP);
    assert(loop); // Checked by the parser
    IrIns *br = emit(s, IR_BR, NULL);
    add_to_branch_chain(loop->continues, &br->br, br);
}

static void compile_goto(Scope *s, AstNode *n) {
    IrIns *br = emit(s, IR_BR, NULL);
    emit_bb(s);
    Goto *pair = malloc(sizeof(Goto));
    pair->label = n->goto_label;
    pair->br = &br->br;
    pair->err = n->tk;
    vec_push(s->gotos, pair);
}

static void compile_label(Scope *s, AstNode *n) {
    if (map_get(s->labels, n->label)) {
        error_at(n->tk, "redefinition of label '%s'", n->label);
    }
    IrIns *end_br = emit(s, IR_BR, NULL);
    BB *bb = emit_bb(s);
    end_br->br = bb;
    map_put(s->labels, n->label, bb);
    compile_stmt(s, n->label_body);
}

static void compile_ret(Scope *s, AstNode *n) {
    IrIns *v = NULL;
    if (n->ret) {
        v = discharge(s, compile_expr(s, n->ret));
    }
    IrIns *ret = emit(s, IR_RET, NULL);
    ret->ret = v;
}

static void compile_stmt(Scope *s, AstNode *n) {
    switch (n->k) {
        case N_TYPEDEF:  break;
        case N_DECL:     compile_decl(s, n); break;
        case N_IF:       compile_if(s, n); break;
        case N_WHILE:    compile_while(s, n); break;
        case N_DO_WHILE: compile_do_while(s, n); break;
        case N_FOR:      compile_for(s, n); break;
        case N_SWITCH:   compile_switch(s, n); break;
        case N_CASE: case N_DEFAULT: compile_case_default(s, n); break;
        case N_BREAK:    compile_break(s); break;
        case N_CONTINUE: compile_continue(s); break;
        case N_GOTO:     compile_goto(s, n); break;
        case N_LABEL:    compile_label(s, n); break;
        case N_RET:      compile_ret(s, n); break;
        default:         discharge(s, compile_expr(s, n)); break;
    }
}

static void compile_block(Scope *s, AstNode *n) {
    Scope block = enter_scope(s, SCOPE_BLOCK);
    while (n) {
        compile_stmt(&block, n);
        n = n->next;
    }
}


// ---- Globals ---------------------------------------------------------------

static void ensure_ends_with_ret(Scope *s) {
    if (!s->fn->last->ir_last || s->fn->last->ir_last->op != IR_RET) {
        emit(s, IR_RET, NULL);
    }
}

static void resolve_gotos(Scope *s) {
    for (size_t i = 0; i < vec_len(s->gotos); i++) {
        Goto *pair = vec_get(s->gotos, i);
        BB *bb = map_get(s->labels, pair->label);
        if (!bb) {
            error_at(pair->err, "use of undeclared label '%s'", pair->label);
        }
        assert(pair->br);
        *pair->br = bb;
    }
}

static void compile_fn_args(Scope *s, AstNode *n) {
    if (n->t->is_vararg) {
        TODO(); // TODO: vararg fns
    }
    IrIns *fargs[vec_len(n->param_names)];
    for (size_t i = 0; i < vec_len(n->param_names); i++) { // Emit IR_FARGs
        AstType *t = vec_get(n->t->params, i);
        IrIns *ins = emit(s, IR_FARG, irt_conv(t));
        ins->arg_idx = i;
        fargs[i] = ins;
    }
    for (size_t i = 0; i < vec_len(n->param_names); i++) { // Emit IR_ALLOCs
        Token *name = vec_get(n->param_names, i);
        AstType *t = vec_get(n->t->params, i);
        IrIns *alloc = emit(s, IR_ALLOC, irt_new(IRT_PTR));
        alloc->alloc_t = irt_conv(t);
        emit_store(s, alloc, fargs[i], t);
        def_local(s, name->ident, alloc);
    }
}

static void compile_fn_def(Scope *s, AstNode *n) {
    char *label = prepend_underscore(n->fn_name);
    Global *g = new_global(label, irt_conv(n->t), n->t->linkage);
    g->k = G_FN_DEF;
    g->fn = new_fn();
    def_global(s, n->fn_name, g);
    Scope body = enter_scope(s, SCOPE_BLOCK);
    body.fn = g->fn;
    body.labels = map_new();
    body.gotos = vec_new();
    compile_fn_args(&body, n);
    compile_block(&body, n->fn_body);
    resolve_gotos(&body);
    ensure_ends_with_ret(&body);
}

static void compile_const_init_elem(Scope *s, Vec *elems, AstNode *n, uint64_t offset);

static void compile_const_arr_init(Scope *s, Vec *elems, AstNode *n, uint64_t offset) {
    assert(n->k == N_INIT);
    assert(n->t->k == T_ARR);
    for (size_t i = 0; i < vec_len(n->elems); i++) {
        AstNode *elem = vec_get(n->elems, i);
        uint64_t elem_offset = offset + i * n->t->elem->size;
        compile_const_init_elem(s, elems, elem, elem_offset);
    }
}

static void compile_const_struct_init(Scope *s, Vec *elems, AstNode *n, uint64_t offset) {
    assert(n->k == N_INIT);
    assert(n->t->k == T_STRUCT);
    for (size_t i = 0; i < vec_len(n->elems); i++) {
        AstNode *elem = vec_get(n->elems, i);
        Field *f = vec_get(n->t->fields, i);
        uint64_t field_offset = offset + f->offset;
        compile_const_init_elem(s, elems, elem, field_offset);
    }
}

static void compile_const_init_elem(Scope *s, Vec *elems, AstNode *n, uint64_t offset) {
    if (!n) return;
    if (n->k == N_INIT) {
        if (n->t->k == T_STRUCT) {
            compile_const_struct_init(s, elems, n, offset);
        } else { // T_ARR
            assert(n->t->k == T_ARR);
            compile_const_arr_init(s, elems, n, offset);
        }
    } else {
        Global *v = new_global(NULL, irt_conv(n->t), n->t->linkage);
        compile_global(s, n, v);
        vec_push(elems, new_init_elem(offset, v));
    }
}

static void compile_global(Scope *s, AstNode *n, Global *g) {
    if (!n) return;
    switch (n->k) {
    case N_IMM: g->k = G_IMM; g->imm = n->imm; break;
    case N_FP:  g->k = G_FP;  g->fp = n->fp;   break;
    case N_STR:
        g->k = G_INIT;
        g->elems = vec_new();
        assert(n->t->k == T_ARR);
        for (size_t i = 0; i < n->len; i++) {
            Global *v = new_global(NULL, irt_conv(n->t->elem), LINK_NONE);
            v->k = G_IMM;
            uint64_t size;
            switch (n->enc) {
                case ENC_NONE:   v->imm = (uint64_t) n->str[i]; size = 1; break;
                case ENC_CHAR16: v->imm = n->str16[i]; size = 2; break;
                case ENC_WCHAR:
                case ENC_CHAR32: v->imm = n->str32[i]; size = 4; break;
                default: UNREACHABLE();
            }
            vec_push(g->elems, new_init_elem(i * size, v));
        }
        break;
    case N_INIT:
        g->k = G_INIT;
        g->elems = vec_new();
        compile_const_init_elem(s, g->elems, n, 0);
        break;
    case N_KPTR:
        g->k = G_PTR;
        g->g = find_global(s, n->g->var_name);
        g->offset = n->offset;
        break;
    default: UNREACHABLE();
    }
}

static void compile_null_global(Global *g) {
    switch (g->t->k) {
    case IRT_I8 ... IRT_I64: case IRT_PTR:
        g->k = G_IMM;
        g->imm = 0;
        break;
    case IRT_F32: case IRT_F64:
        g->k = G_FP;
        g->imm = 0;
        break;
    case IRT_ARR: case IRT_STRUCT:
        g->k = G_INIT;
        g->elems = vec_new();
        break;
    default: UNREACHABLE();
    }
}

static void compile_global_decl(Scope *s, AstNode *n) {
    assert(n->var->k == N_GLOBAL);
    char *label = prepend_underscore(n->var->var_name);
    Global *g = new_global(label, irt_conv(n->var->t), n->var->t->linkage);
    def_global(s, n->var->var_name, g);
    if (n->var->t->k == T_VOID || n->var->t->k == T_FN ||
            n->var->t->linkage == LINK_EXTERN) {
        return; // Shouldn't have an initialization value -> not an object
    }
    if (n->val) {
        compile_global(s, n->val, g);
    } else {
        compile_null_global(g);
    }
}

static void compile_top_level(Scope *s, AstNode *n) {
    switch (n->k) {
        case N_DECL:    compile_global_decl(s, n); break;
        case N_FN_DEF:  compile_fn_def(s, n); break;
        case N_TYPEDEF: break; // Ignore
        default:        UNREACHABLE();
    }
}

Vec * compile(AstNode *n) {
    Scope file = {0};
    file.k = SCOPE_FILE;
    file.globals = vec_new();
    file.vars = map_new();
    while (n) {
        compile_top_level(&file, n);
        n = n->next;
    }
    return file.globals;
}
