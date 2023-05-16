
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "compile.h"

#define GLOBAL_PREFIX "_G."

enum {
    SCOPE_FILE   = 0b0001,
    SCOPE_BLOCK  = 0b0010,
    SCOPE_LOOP   = 0b0100,
    SCOPE_SWITCH = 0b1000,
};

typedef struct Scope {
    struct Scope *outer;
    int k;
    Vec *globals;
    IrFn *fn;
    Map *vars;   // Block: 'IrIns *' k = IR_ALLOC; file: 'Global *'
    Vec *breaks; // SCOPE_LOOP and SCOPE_SWITCH 'break' jump list
    union {
        Vec *continues; // SCOPE_LOOP; 'continue' jump list
        Vec *cases;     // SCOPE_SWITCH; list of 'IrBB *'
    };
} Scope;


// ---- Scopes, Globals, Functions, Basic Blocks, and Instructions ------------

static void enter_scope(Scope *inner, Scope *outer, int k) {
    *inner = (Scope) {0};
    inner->outer = outer;
    inner->k = k;
    inner->globals = outer->globals;
    inner->fn = outer->fn;
    inner->vars = map_new();
    if (k == SCOPE_LOOP) {
        inner->breaks = vec_new();
        inner->continues = vec_new();
    } else if (k == SCOPE_SWITCH) {
        inner->breaks = vec_new();
        inner->cases = vec_new();
    }
}

static Scope * find_scope(Scope *s, int k) {
    while (s && !(s->k & k)) {
        s = s->outer;
    }
    return s;
}

static Global * new_global(char *label) {
    Global *g = malloc(sizeof(Global));
    g->label = label;
    g->val = NULL;
    g->ir_fn = NULL;
    return g;
}

static IrBB * new_bb() {
    IrBB *bb = malloc(sizeof(IrBB));
    bb->next = bb->prev = NULL;
    bb->head = bb->last = NULL;
    return bb;
}

static IrFn * new_fn() {
    IrFn *fn = malloc(sizeof(IrFn));
    fn->entry = fn->last = new_bb();
    return fn;
}

static IrIns * new_ins(int op, IrType *t) {
    IrIns *ins = calloc(1, sizeof(IrIns));
    ins->op = op;
    ins->t = t;
    ins->vreg = -1;
    return ins;
}

static IrBB * emit_bb(Scope *s) {
    assert(s->fn); // Not top level
    if (!s->fn->last->last) {
        return s->fn->last; // Current BB is empty, use that
    }
    IrBB *bb = new_bb();
    bb->prev = s->fn->last;
    if (s->fn->last) {
        s->fn->last->next = bb;
    } else {
        s->fn->entry = bb;
    }
    s->fn->last = bb;
    return bb;
}

static IrIns * emit_to_bb(IrBB *bb, IrIns *ins) {
    ins->bb = bb;
    ins->prev = bb->last;
    ins->next = NULL; // Just in case
    if (bb->last) {
        bb->last->next = ins;
    } else {
        bb->head = ins;
    }
    bb->last = ins;
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
        ins->bb->head = ins->next;
    }
    if (ins->next) {
        ins->next->prev = ins->prev;
    }
    if (ins->bb->last == ins) {
        ins->bb->last = ins->prev;
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
    case T_VOID:  UNREACHABLE();
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

static int is_num(IrType *t) {
    return t->k >= IRT_I8 && t->k <= IRT_F64;
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
        assert(strcmp(g->label, g2->label) != 0); // No duplicate labels
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

static Global * def_const_global(Scope *s, AstNode *n) {
    char *label = next_global_label(s);
    Global *g = new_global(label);
    g->val = n;
    def_global(s, NULL, g);
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
    [IR_LT] = IR_GE, [IR_LE] = IR_GT, [IR_GT] = IR_LE, [IR_GE] = IR_LT,
};

static IrIns * compile_expr(Scope *s, AstNode *n);

static void add_to_branch_chain(Vec *bcs, IrBB **bb, IrIns *ins) {
    BrChain *bc = malloc(sizeof(BrChain));
    bc->bb = bb;
    bc->ins = ins;
    vec_push(bcs, bc);
}

static void patch_branch_chain(Vec *bcs, IrBB *target) {
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

static void add_phi(IrIns *phi, IrBB *pred, IrIns *def) {
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
        if (((BrChain *) vec_get(br->false_chain, 0))->bb == &br->false) {
            cond->op = INVERT_COND[cond->op]; // Negate
        }
        delete_ir(br);
        return cond;
    }
    IrBB *bb = emit_bb(s);
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
    if (cond->op < IR_EQ || cond->op > IR_GE) { // Not a comparison
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

static IrIns * emit_conv(Scope *s, IrIns *src, IrType *dt) {
    int op;
    IrType *st = src->t;
    if (is_int(st) && is_fp(dt)) {
        op = IR_I2FP;
    } else if (is_fp(st) && is_int(dt)) {
        op = IR_FP2I;
    } else if (is_num(st) && is_num(dt)) {
        op = dt->size < st->size ? IR_TRUNC : IR_EXT;
    } else if (is_int(st) && (dt->k == IRT_PTR || dt->k == IRT_ARR)) {
        op = IR_I2PTR;
    } else if ((st->k == IRT_PTR || st->k == IRT_ARR) && is_int(dt)) {
        op = IR_PTR2I;
    } else if (st->k == IRT_ARR && dt->k == IRT_PTR) {
        IrIns *zero = emit(s, IR_IMM, irt_new(IRT_I64));
        zero->imm = 0;
        IrIns *idx = emit(s, IR_IDX, dt);
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
    if (is_vla(t) || t->k == T_STRUCT || t->k == T_UNION || t->k == T_FN) {
        return src; // Aggregates loaded on field access
    } else if (t->k == T_ARR) { // Not VLA
        IrIns *zero = emit(s, IR_IMM, irt_new(IRT_I64));
        zero->imm = 0;
        IrIns *arr = emit(s, IR_IDX, irt_conv(t));
        arr->base = src;
        arr->offset = zero;
        return arr;
    } else { // Base types
        IrIns *load = emit(s, IR_LOAD, irt_conv(t));
        load->src = src;
        return load;
    }
}

static void compile_init_elem(Scope *s, AstNode *n, AstType *t, IrIns *elem);

static void compile_array_init_raw(Scope *s, AstNode *n, IrIns *elem) {
    assert(n->t->k == T_ARR);
    IrIns *zero = emit(s, IR_IMM, irt_new(IRT_I64));
    zero->imm = 0;
    for (size_t i = 0; i < vec_len(n->elems); i++) {
        if (i == 0) {
            IrIns *first = emit(s, IR_IDX, irt_new(IRT_PTR));
            first->base = elem;
            first->offset = zero;
            elem = first;
        } else {
            IrIns *one = emit(s, IR_IMM, irt_new(IRT_I64));
            one->imm = 1;
            IrIns *offset = emit(s, IR_IMM, irt_new(IRT_I64));
            offset->imm = n->t->elem->size;
            IrIns *next = emit(s, IR_IDX, irt_new(IRT_PTR));
            next->base = elem;
            next->offset = offset;
            elem = next;
        }
        AstNode *v = vec_get(n->elems, i);
        compile_init_elem(s, v, n->t->elem, elem);
    }
}

static void compile_struct_init_raw(Scope *s, AstNode *n, IrIns *obj) {
    assert(n->t->k == T_STRUCT || n->t->k == T_UNION);
    for (size_t i = 0; i < vec_len(n->elems); i++) {
        Field *f = vec_get(n->t->fields, i);
        IrIns *offset = emit(s, IR_IMM, irt_new(IRT_I64));
        offset->imm = f->offset;
        IrIns *idx = emit(s, IR_IDX, irt_new(IRT_PTR));
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

static int is_const_init(AstNode *n) {
    assert(n->k == N_INIT);
    for (size_t i = 0; i < vec_len(n->elems); i++) {
        AstNode *e = vec_get(n->elems, i);
        if (!(!e || e->k == N_IMM || e->k == N_FP || e->k == N_STR ||
                e->k == N_KPTR || (e->k == N_INIT && is_const_init(e)))) {
            return 0;
        }
    }
    return 1;
}

static IrIns * compile_const_init(Scope *s, AstNode *n) {
    Global *g = def_const_global(s, n);
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
    if (is_const_init(n)) {
        return compile_const_init(s, n);
    }
    IrIns *alloc = emit(s, IR_ALLOC, irt_new(IRT_PTR));
    alloc->alloc_t = irt_conv(n->t);
    IrIns *zero = emit(s, IR_IMM, irt_new(IRT_I64));
    zero->imm = 0;
    IrIns *arr = emit(s, IR_IDX, alloc->alloc_t);
    arr->base = alloc;
    arr->offset = zero;
    compile_init_elem(s, n, n->t, arr);
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
    l = emit_conv(s, l, irt_conv(n->t)); // Convert to i64
    r = emit_conv(s, r, irt_conv(n->t));
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
    IrIns *idx = emit(s, IR_IDX, irt_new(IRT_PTR));
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
        emit_conv(s, binop, ir_target);
    }
    emit_store(s, lvalue->src, binop, n->t);
    return binop; // Assignment evaluates to its right operand
}

static IrIns * compile_and(Scope *s, AstNode *n) {
    IrIns *l = to_cond(s, compile_expr(s, n->l));
    IrBB *r_bb = emit_bb(s);
    patch_branch_chain(l->true_chain, r_bb);

    IrIns *r = to_cond(s, compile_expr(s, n->r));
    merge_branch_chains(r->false_chain, l->false_chain);
    return r;
}

static IrIns * compile_or(Scope *s, AstNode *n) {
    IrIns *l = to_cond(s, compile_expr(s, n->l));
    IrBB *r_bb = emit_bb(s);
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
    IrIns *cond = to_cond(s, compile_expr(s, n->cond));

    IrBB *true_bb = emit_bb(s);
    patch_branch_chain(cond->true_chain, true_bb);
    IrIns *true = discharge(s, compile_expr(s, n->body));
    IrIns *true_br = emit(s, IR_BR, NULL);

    IrBB *false_bb = emit_bb(s);
    patch_branch_chain(cond->false_chain, false_bb);
    IrIns *false = discharge(s, compile_expr(s, n->els));
    IrIns *false_br = emit(s, IR_BR, NULL);

    IrBB *after = emit_bb(s);
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
    return emit_conv(s, l, irt_conv(n->t));
}

static IrIns * compile_array_access(Scope *s, AstNode *n) {
    // TODO: VLA access (via multiplication)
    IrIns *ptr = compile_ptr_arith(s, n);
    return emit_load(s, ptr, n->t);
}

static IrIns * compile_struct_field_access(Scope *s, AstNode *n) {
    IrIns *ptr = discharge(s, compile_expr(s, n->obj));
    Field *f = vec_get(n->obj->t->fields, n->field_idx);
    IrIns *zero = emit(s, IR_IMM, irt_new(IRT_I64));
    zero->imm = 0;
    IrIns *obj = emit(s, IR_IDX, irt_conv(n->obj->t)); // Struct itself
    obj->base = ptr;
    obj->offset = zero;
    IrIns *offset = emit(s, IR_IMM, irt_new(IRT_I64));
    offset->imm = f->offset;
    IrIns *idx = emit(s, IR_IDX, irt_new(IRT_PTR)); // Pointer to field in struct
    idx->base = obj;
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
    IrIns *fn = compile_expr(s, n->fn);
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
        if (n->t->k >= T_FLOAT && n->t->k <= T_LDOUBLE) {
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
    case N_SHR:       return compile_binop(s, n, IR_SHR);
    case N_EQ:        return compile_binop(s, n, IR_EQ);
    case N_NEQ:       return compile_binop(s, n, IR_NEQ);
    case N_LT:        return compile_binop(s, n, IR_LT);
    case N_LE:        return compile_binop(s, n, IR_LE);
    case N_GT:        return compile_binop(s, n, IR_GT);
    case N_GE:        return compile_binop(s, n, IR_GE);
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
    case N_A_SHR:     return compile_arith_assign(s, n, IR_SHR);
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
    while (n && n->cond) { // 'if' and 'else if's
        IrIns *cond = to_cond(s, compile_expr(s, n->cond));
        IrBB *body = emit_bb(s);
        patch_branch_chain(cond->true_chain, body);
        compile_block(s, n->body);

        IrIns *end_br = emit(s, IR_BR, NULL);
        add_to_branch_chain(brs, &end_br->br, end_br);

        IrBB *after = emit_bb(s);
        patch_branch_chain(cond->false_chain, after);
        n = n->els;
    }
    if (n) { // else
        assert(!n->cond);
        assert(!n->els);
        compile_block(s, n->body);
        IrIns *end_br = emit(s, IR_BR, NULL);
        add_to_branch_chain(brs, &end_br->br, end_br);
        emit_bb(s);
    }
    patch_branch_chain(brs, s->fn->last);
}

static void compile_while(Scope *s, AstNode *n) {
    IrIns *before_br = emit(s, IR_BR, NULL);
    IrBB *cond_bb = emit_bb(s);
    before_br->bb = cond_bb;
    IrIns *cond = to_cond(s, compile_expr(s, n->cond));

    Scope loop;
    enter_scope(&loop, s, SCOPE_LOOP);
    IrBB *body_bb = emit_bb(s);
    patch_branch_chain(cond->true_chain, body_bb);
    compile_block(&loop, n->body);
    IrIns *end_br = emit(s, IR_BR, NULL);
    end_br->br = cond_bb;

    IrBB *after_bb = emit_bb(s);
    patch_branch_chain(cond->false_chain, after_bb);
    patch_branch_chain(loop.breaks, after_bb);
    patch_branch_chain(loop.continues, cond_bb);
}

static void compile_do_while(Scope *s, AstNode *n) {
    IrIns *before_br = emit(s, IR_BR, NULL);
    Scope loop;
    enter_scope(&loop, s, SCOPE_LOOP);
    IrBB *body_bb = emit_bb(s);
    before_br->br = body_bb;
    compile_block(&loop, n->body);
    IrIns *body_br = emit(s, IR_BR, NULL);

    IrBB *cond_bb = emit_bb(s);
    body_br->br = cond_bb;
    IrIns *cond = to_cond(s, compile_expr(s, n->cond));
    patch_branch_chain(cond->true_chain, body_bb);

    IrBB *after_bb = emit_bb(s);
    patch_branch_chain(cond->false_chain, after_bb);
    patch_branch_chain(loop.breaks, after_bb);
    patch_branch_chain(loop.continues, cond_bb);
}

static void compile_for(Scope *s, AstNode *n) {
    if (n->init) {
        compile_stmt(s, n->init);
    }

    IrIns *before_br = emit(s, IR_BR, NULL);
    IrBB *start_bb = NULL;
    IrIns *cond = NULL;
    if (n->cond) {
        start_bb = emit_bb(s);
        before_br->bb = start_bb;
        cond = to_cond(s, compile_expr(s, n->cond));
    }

    Scope loop;
    enter_scope(&loop, s, SCOPE_LOOP);
    IrBB *body = emit_bb(s);
    if (cond) {
        patch_branch_chain(cond->true_chain, body);
    } else {
        start_bb = body;
        before_br->bb = body;
    }
    compile_block(&loop, n->body);
    IrIns *end_br = emit(s, IR_BR, NULL);

    IrBB *continue_bb = NULL;
    if (n->inc) {
        IrBB *inc_bb = emit_bb(s);
        end_br->br = inc_bb;
        compile_expr(s, n->inc);
        IrIns *inc_br = emit(s, IR_BR, NULL);
        inc_br->br = start_bb;
        continue_bb = inc_bb;
    } else {
        end_br->br = start_bb;
        continue_bb = start_bb;
    }

    IrBB *after = emit_bb(s);
    if (cond) {
        patch_branch_chain(cond->false_chain, after);
    }
    patch_branch_chain(loop.breaks, after);
    patch_branch_chain(loop.continues, continue_bb);
}

static void compile_switch(Scope *s, AstNode *n) {
    IrIns *cond = compile_expr(s, n->cond);
    Vec *brs = vec_new(); // of 'IrBB **'
    IrBB **false_br;
    for (size_t i = 0; i < vec_len(n->cases); i++) {
        AstNode *case_n = vec_get(n->cases, i);
        if (case_n->k == N_CASE) {
            IrIns *val = compile_expr(s, case_n->cond);
            IrIns *cmp = emit(s, IR_EQ, irt_new(IRT_I32));
            cmp->l = cond;
            cmp->r = val;
            IrIns *br = emit(s, IR_CONDBR, NULL);
            br->cond = cmp;
            vec_push(brs, &br->true);
            false_br = &br->false;
            IrBB *next = emit_bb(s);
            *false_br = next;
        } else { // N_DEFAULT
            vec_push(brs, NULL);
        }
    }

    Scope swtch;
    enter_scope(&swtch, s, SCOPE_SWITCH);
    compile_block(&swtch, n->body);

    IrIns *end_br = emit(s, IR_BR, NULL);
    IrBB *after = emit_bb(s);
    end_br->br = after;
    patch_branch_chain(swtch.breaks, after);
    *false_br = after; // For if there's no default

    assert(vec_len(brs) == vec_len(swtch.cases));
    for (size_t i = 0; i < vec_len(brs); i++) { // Patch initial branches
        IrBB **br = vec_get(brs, i);
        IrBB *dst = vec_get(swtch.cases, i);
        if (br) { // Case
            *br = dst;
        } else { // Default
            *false_br = dst;
        }
    }
}

static void compile_case(Scope *s, AstNode *n) {
    Scope *swtch = find_scope(s, SCOPE_SWITCH);
    assert(swtch); // Checked by parser
    IrIns *end_br = emit(s, IR_BR, NULL);
    IrBB *bb = emit_bb(s);
    end_br->br = bb;
    compile_stmt(s, n->body);
    vec_push(swtch->cases, bb);
}

static void compile_break(Scope *s) {
    Scope *swtch_loop = find_scope(s, SCOPE_SWITCH | SCOPE_LOOP);
    assert(swtch_loop); // Checked by the parser
    IrIns *br = emit(s, IR_BR, NULL);
    add_to_branch_chain(swtch_loop->breaks, &br->br, br);
}

static void compile_continue(Scope *s) {
    Scope *loop = find_scope(s, SCOPE_LOOP);
    assert(loop); // Checked by the parser
    IrIns *br = emit(s, IR_BR, NULL);
    add_to_branch_chain(loop->continues, &br->br, br);
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
        case N_CASE: case N_DEFAULT: compile_case(s, n); break;
        case N_BREAK:    compile_break(s); break;
        case N_CONTINUE: compile_continue(s); break;
        case N_RET:      compile_ret(s, n); break;
        default:         discharge(s, compile_expr(s, n)); break;
    }
}

static void compile_block(Scope *s, AstNode *n) {
    Scope block;
    enter_scope(&block, s, SCOPE_BLOCK);
    while (n) {
        compile_stmt(&block, n);
        n = n->next;
    }
}


// ---- Globals ---------------------------------------------------------------

static void compile_fn_args(Scope *s, AstNode *n) {
    if (n->t->is_vararg) {
        TODO(); // TODO: vararg fns
    }
    IrIns *fargs[vec_len(n->param_names)];
    for (size_t i = 0; i < vec_len(n->param_names); i++) { // Emit IR_FARGs
        AstType *t = vec_get(n->t->params, i);
        IrIns *ins = emit(s, IR_FARG, irt_conv(t));
        ins->arg_num = i;
        fargs[i] = ins;
    }
    for (size_t i = 0; i < vec_len(n->param_names); i++) { // Emit IR_ALLOCs
        char *name = vec_get(n->param_names, i);
        AstType *t = vec_get(n->t->params, i);
        IrIns *alloc = emit(s, IR_ALLOC, irt_new(IRT_PTR));
        alloc->alloc_t = irt_conv(t);
        emit_store(s, alloc, fargs[i], t);
        def_local(s, name, alloc);
    }
}

static void compile_fn_def(Scope *s, AstNode *n) {
    Global *g = new_global(prepend_underscore(n->fn_name));
    g->ir_fn = new_fn();
    g->ir_fn->linkage = n->t->linkage;
    def_global(s, n->fn_name, g);
    Scope body;
    enter_scope(&body, s, SCOPE_BLOCK);
    body.fn = g->ir_fn;
    compile_fn_args(&body, n);
    compile_block(&body, n->body);
    if (!body.fn->last->last || body.fn->last->last->op != IR_RET) {
        emit(&body, IR_RET, NULL);
    }
}

static void compile_global_decl(Scope *s, AstNode *n) {
    char *label = prepend_underscore(n->var->var_name);
    Global *g = new_global(label);
    def_global(s, n->var->var_name, g);
    g->val = n->val; // May be NULL
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
    Scope top_level = {0};
    top_level.k = SCOPE_FILE;
    top_level.globals = vec_new();
    top_level.vars = map_new();
    while (n) {
        compile_top_level(&top_level, n);
        n = n->next;
    }
    return top_level.globals;
}
