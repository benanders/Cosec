
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

static Global * new_global(Type *t, char *label) {
    Global *g = calloc(1, sizeof(Global));
    g->t = t;
    g->label = label;
    return g;
}

static IrBB * new_bb() {
    IrBB *bb = calloc(1, sizeof(IrBB));
    return bb;
}

static IrFn * new_fn() {
    IrFn *fn = calloc(1, sizeof(IrFn));
    fn->entry = fn->last = new_bb();
    return fn;
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

static IrIns * emit_to_bb(IrBB *bb, IrIns *i) {
    i->bb = bb;
    i->prev = bb->last;
    i->next = NULL; // Just in case
    if (bb->last) {
        bb->last->next = i;
    } else {
        bb->head = i;
    }
    bb->last = i;
    return i;
}

static IrIns * emit(Scope *s, int op, Type *t) {
    assert(s->fn->last);
    IrIns *i = calloc(1, sizeof(IrIns));
    i->op = op;
    i->t = t;
    if (op == IR_CONDBR) {
        i->true_chain = vec_new();
        i->false_chain = vec_new();
    }
    if (op == IR_PHI) {
        i->preds = vec_new();
        i->defs = vec_new();
    }
    emit_to_bb(s->fn->last, i);
    return i;
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

static Global * def_const_global(Scope *s, Node *n) {
    char *label = next_global_label(s);
    Global *g = new_global(n->t, label);
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

static IrIns * compile_expr(Scope *s, Node *n);

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
    IrIns *k_true = emit(s, IR_IMM, t_num(T_INT, 0));
    k_true->imm = 1;
    IrIns *k_false = emit(s, IR_IMM, t_num(T_INT, 0));
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
        IrIns *cmp = emit(s, IR_NEQ, t_num(T_INT, 0));
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

static IrIns * emit_conv(Scope *s, IrIns *src, Type *dt) {
    int op;
    Type *st = src->t;
    if (is_int(st) && is_fp(dt)) {
        op = IR_I2FP;
    } else if (is_fp(st) && is_int(dt)) {
        op = IR_FP2I;
    } else if (is_arith(st) && is_arith(dt)) {
        op = dt->size < st->size ? IR_TRUNC : IR_EXT;
    } else if (is_int(st) && (dt->k == T_PTR || dt->k == T_ARR || dt->k == T_FN)) {
        op = IR_I2PTR;
    } else if ((st->k == T_PTR || st->k == T_ARR || st->k == T_FN) && is_int(dt)) {
        op = IR_PTR2I;
    } else if (st->k == T_ARR && dt->k == T_PTR) {
        IrIns *zero = emit(s, IR_IMM, t_num(T_LLONG, 0));
        zero->imm = 0;
        IrIns *elem = emit(s, IR_ELEM, dt);
        elem->base = src;
        elem->offset = zero;
        return elem;
    } else if ((st->k == T_PTR || st->k == T_ARR || st->k == T_FN) &&
               (dt->k == T_PTR || dt->k == T_ARR || dt->k == T_FN)) {
        op = IR_BITCAST;
    } else {
        UNREACHABLE();
    }
    IrIns *conv = emit(s, op, dt);
    conv->l = src;
    return conv;
}

static IrIns * emit_load(Scope *s, IrIns *l) {
    assert(l->t->k == T_PTR);
    Type *t = l->t->ptr;
    if (t->k == T_ARR || t->k == T_STRUCT || t->k == T_UNION) { // Aggregate
        IrIns *zero = emit(s, IR_IMM, t_num(T_LLONG, 0));
        zero->imm = 0;
        IrIns *elem = emit(s, IR_ELEM, l->t->ptr);
        elem->base = l;
        elem->offset = zero;
        return elem;
    } else if (l->t->ptr->k == T_FN) { // Pointer to function
        return l;
    } else {
        IrIns *load = emit(s, IR_LOAD, l->t->ptr);
        load->src = l;
        return load;
    }
}

static void compile_init_elem(Scope *s, Node *n, Type *t, IrIns *elem);

static void compile_array_init_raw(Scope *s, Node *n, IrIns *elem) {
    assert(n->t->k == T_ARR);
    IrIns *zero = emit(s, IR_IMM, t_num(T_LLONG, 0));
    zero->imm = 0;
    for (size_t i = 0; i < vec_len(n->elems); i++) {
        if (i == 0) {
            IrIns *first = emit(s, IR_ELEM, t_ptr(n->t->elem));
            first->base = elem;
            first->offset = zero;
            elem = first;
        } else {
            IrIns *one = emit(s, IR_IMM, t_num(T_LLONG, 0));
            one->imm = 1;
            IrIns *next = emit(s, IR_ELEM, elem->t);
            next->base = elem;
            next->offset = one;
            elem = next;
        }
        Node *v = vec_get(n->elems, i);
        compile_init_elem(s, v, n->t->elem, elem);
    }
}

static void compile_struct_init_raw(Scope *s, Node *n, IrIns *strct) {
    assert(n->t->k == T_STRUCT || n->t->k == T_UNION);
    for (size_t i = 0; i < vec_len(n->elems); i++) {
        Type *ft = ((Field *) vec_get(n->t->fields, i))->t;
        IrIns *idx = emit(s, IR_IMM, t_num(T_LLONG, 0));
        idx->imm = i;
        IrIns *elem = emit(s, IR_ELEM, t_ptr(ft));
        elem->base = strct;
        elem->offset = idx;
        Node *v = vec_get(n->elems, i);
        compile_init_elem(s, v, ft, elem);
    }
}

static void compile_init_elem(Scope *s, Node *n, Type *t, IrIns *elem) {
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
            IrIns *size = emit(s, IR_IMM, t);
            size->imm = t->size;
            IrIns *zero = emit(s, IR_ZERO, NULL);
            zero->ptr = elem;
            zero->size = size;
        } else {
            IrIns *zero = emit(s, IR_IMM, t);
            zero->imm = 0;
            IrIns *store = emit(s, IR_STORE, NULL);
            store->dst = elem;
            store->src = zero;
        }
    }
}

static int is_const_init(Node *n) {
    assert(n->k == N_INIT);
    for (size_t i = 0; i < vec_len(n->elems); i++) {
        Node *e = vec_get(n->elems, i);
        if (!(e->k == N_IMM || e->k == N_FP || e->k == N_STR || e->k == N_KPTR ||
              (e->k == N_INIT && is_const_init(e)))) {
            return 0;
        }
    }
    return 1;
}

static IrIns * compile_const_init(Scope *s, Node *n) {
    Global *g = def_const_global(s, n);
    IrIns *src = emit(s, IR_GLOBAL, t_ptr(n->t));
    src->g = g;
    IrIns *dst = emit(s, IR_ALLOC, t_ptr(n->t));
    IrIns *size = emit(s, IR_IMM, t_num(T_LLONG, 0));
    size->imm = n->t->size;
    IrIns *cpy = emit(s, IR_COPY, NULL);
    cpy->cpy_src = src;
    cpy->cpy_dst = dst;
    cpy->cpy_size = size;
    return dst;
}

static IrIns * compile_init(Scope *s, Node *n) {
    assert(n->k == N_INIT);
    if (is_const_init(n)) {
        return compile_const_init(s, n);
    }
    IrIns *alloc = emit(s, IR_ALLOC, t_ptr(n->t));
    IrIns *zero = emit(s, IR_IMM, t_num(T_LLONG, 0));
    zero->imm = 0;
    IrIns *arr = emit(s, IR_ELEM, n->t);
    arr->base = alloc;
    arr->offset = zero;
    compile_init_elem(s, n, n->t, arr);
    return alloc;
}

static IrIns * compile_kptr(Scope *s, Node *n) {
    assert(n->global->k == N_GLOBAL);
    Global *g = find_global(s, n->global->var_name);
    assert(g); // Checked by parser
    IrIns *ins = emit(s, IR_GLOBAL, t_ptr(n->t));
    ins->g = g;
    if (n->offset != 0) {
        IrIns *offset = emit(s, IR_IMM, t_num(T_LLONG, 0));
        offset->imm = n->offset < 0 ? -n->offset : n->offset;
        IrIns *arith = emit(s, n->offset < 0 ? IR_SUB : IR_ADD, n->t);
        arith->l = ins;
        arith->r = offset;
        return arith;
    }
    return ins;
}

static IrIns * compile_operand(Scope *s, Node *n) {
    IrIns *ins;
    Global *g;
    switch (n->k) {
    case N_IMM:
        ins = emit(s, IR_IMM, n->t);
        ins->imm = n->imm;
        break;
    case N_FP:
        ins = emit(s, IR_FP, n->t);
        ins->fp = n->fp;
        break;
    case N_STR:
        ins = emit(s, IR_GLOBAL, n->t);
        ins->g = def_const_global(s, n);
        break;
    case N_INIT:
        ins = compile_init(s, n);
        break;
    case N_LOCAL:
        ins = find_local(s, n->var_name); // 'IR_ALLOC' ins
        assert(ins); // Checked by parser
        ins = emit_load(s, ins);
        break;
    case N_GLOBAL:
        g = find_global(s, n->var_name);
        assert(g); // Checked by parser
        ins = emit(s, IR_GLOBAL, t_ptr(n->t));
        ins->g = g;
        ins = emit_load(s, ins);
        break;
    case N_KPTR:
        ins = compile_kptr(s, n);
        break;
    default: UNREACHABLE();
    }
    return ins;
}

static IrIns * compile_binop(Scope *s, Node *n, int op) {
    IrIns *l = discharge(s, compile_expr(s, n->l));
    IrIns *r = discharge(s, compile_expr(s, n->r));
    IrIns *ins = emit(s, op, n->t);
    ins->l = l;
    ins->r = r;
    return ins;
}

static IrIns * compile_ptr_sub(Scope *s, Node *n) { // <ptr> - <ptr>
    assert(n->l->t->k == T_PTR);
    assert(n->r->t->k == T_PTR);
    IrIns *l = discharge(s, compile_expr(s, n->l));
    IrIns *r = discharge(s, compile_expr(s, n->r));
    l = emit_conv(s, l, n->t); // Convert to i64
    r = emit_conv(s, r, n->t);
    IrIns *sub = emit(s, IR_SUB, n->t);
    sub->l = l;
    sub->r = r;
    IrIns *size = emit(s, IR_IMM, n->t);
    size->imm = n->l->t->ptr->size;
    IrIns *div = emit(s, IR_DIV, n->t);
    div->l = sub;
    div->r = size;
    return div;
}

static IrIns * compile_ptr_arith(Scope *s, Node *n) { // <ptr> +/- <int>
    IrIns *l = discharge(s, compile_expr(s, n->l));
    IrIns *r = discharge(s, compile_expr(s, n->r));
    IrIns *ptr = n->l->t->k == T_PTR ? l : r;
    IrIns *offset = n->l->t->k == T_PTR ? r : l;
    if (n->k == N_SUB || n->k == N_A_SUB) { // Negate the offset
        IrIns *zero = emit(s, IR_IMM, offset->t);
        zero->imm = 0;
        IrIns *sub = emit(s, IR_SUB, offset->t);
        sub->l = zero;
        sub->r = offset;
        offset = sub;
    }
    IrIns *elem = emit(s, IR_ELEM, ptr->t);
    elem->base = ptr;
    elem->offset = offset;
    return elem;
}

static void emit_store(Scope *s, IrIns *dst, IrIns *src) {
    assert(src->t->k != T_ARR); // Checked by parser
    if (src->t->k == T_STRUCT || src->t->k == T_UNION) { // Use IR_COPY
        assert(src->op == IR_ELEM && src->offset->imm == 0);
        delete_ir(src->offset);
        delete_ir(src);
        IrIns *size = emit(s, IR_IMM, t_num(T_LLONG, 0));
        size->imm = src->ptr->t->size;
        IrIns *cpy = emit(s, IR_COPY, NULL);
        cpy->cpy_src = src->ptr;
        cpy->cpy_dst = dst;
        cpy->cpy_size = size;
    } else {
        IrIns *store = emit(s, IR_STORE, NULL);
        store->dst = dst;
        store->src = src;
    }
}

static IrIns * compile_assign(Scope *s, Node *n) {
    IrIns *r = discharge(s, compile_expr(s, n->r));
    IrIns *l = compile_expr(s, n->l);
    assert(l->op == IR_LOAD || l->op == IR_ELEM);
    delete_ir(l);
    if (l->op == IR_ELEM) {
        delete_ir(l->offset);
    }
    emit_store(s, l->src, r);
    return r; // Assignment evaluates to its right operand
}

static IrIns * compile_arith_assign(Scope *s, Node *n, int op) {
    IrIns *binop = compile_binop(s, n, op);

    IrIns *lvalue = binop->l;
    if (lvalue->op != IR_LOAD) {
        lvalue = lvalue->l; // IR_CONV getting in the way
    }
    assert(lvalue->op == IR_LOAD);

    Type *target = n->l->t;
    if (n->l->k == N_CONV) {
        target = n->l->l->t; // Type conversion getting in the way
    }
    // May need to emit a truncation, e.g. 'char a = 3; char *b = &a; *b += 1'
    if (!are_equal(binop->t, target)) {
        emit_conv(s, binop, target);
    }
    emit_store(s, lvalue->src, binop);
    return binop; // Assignment evaluates to its right operand
}

static IrIns * compile_and(Scope *s, Node *n) {
    IrIns *l = to_cond(s, compile_expr(s, n->l));
    IrBB *r_bb = emit_bb(s);
    patch_branch_chain(l->true_chain, r_bb);

    IrIns *r = to_cond(s, compile_expr(s, n->r));
    merge_branch_chains(r->false_chain, l->false_chain);
    return r;
}

static IrIns * compile_or(Scope *s, Node *n) {
    IrIns *l = to_cond(s, compile_expr(s, n->l));
    IrBB *r_bb = emit_bb(s);
    patch_branch_chain(l->false_chain, r_bb);

    IrIns *r = to_cond(s, compile_expr(s, n->r));
    merge_branch_chains(r->true_chain, l->true_chain);
    return r;
}

static IrIns * compile_comma(Scope *s, Node *n) {
    discharge(s, compile_expr(s, n->l)); // Discard left operand
    return compile_expr(s, n->r);
}

static IrIns * compile_ternary(Scope *s, Node *n) {
    IrIns *cond = to_cond(s, compile_expr(s, n->if_cond));

    IrBB *true_bb = emit_bb(s);
    patch_branch_chain(cond->true_chain, true_bb);
    IrIns *true = discharge(s, compile_expr(s, n->if_body));
    IrIns *true_br = emit(s, IR_BR, NULL);

    IrBB *false_bb = emit_bb(s);
    patch_branch_chain(cond->false_chain, false_bb);
    IrIns *false = discharge(s, compile_expr(s, n->if_else));
    IrIns *false_br = emit(s, IR_BR, NULL);

    IrBB *after = emit_bb(s);
    true_br->br = after;
    false_br->br = after;

    IrIns *phi = emit(s, IR_PHI, n->t);
    add_phi(phi, true_bb, true);
    add_phi(phi, false_bb, false);
    return phi;
}

static IrIns * compile_neg(Scope *s, Node *n) {
    IrIns *l = discharge(s, compile_expr(s, n->l));
    IrIns *zero = emit(s, IR_IMM, n->t);
    zero->imm = 0;
    IrIns *sub = emit(s, IR_SUB, n->t);
    sub->l = zero;
    sub->r = l;
    return sub;
}

static IrIns * compile_bit_not(Scope *s, Node *n) {
    IrIns *l = discharge(s, compile_expr(s, n->l));
    IrIns *neg1 = emit(s, IR_IMM, n->t);
    neg1->imm = -1;
    IrIns *xor = emit(s, IR_BIT_XOR, n->t);
    xor->l = l;
    xor->r = neg1;
    return xor;
}

static IrIns * compile_log_not(Scope *s, Node *n) {
    IrIns *l = to_cond(s, compile_expr(s, n->l));
    assert(l->op == IR_CONDBR);
    Vec *swap = l->true_chain; // Swap true and false chains
    l->true_chain = l->false_chain;
    l->false_chain = swap;
    return l;
}

static IrIns * compile_inc_dec(Scope *s, Node *n) {
    int is_sub = (n->k == N_PRE_DEC || n->k == N_POST_DEC);
    Type *t = n->t->k == T_PTR ? t_num(T_LLONG, 1) : n->t;
    Node one = { .k = N_IMM, .t = t, .imm = 1 };
    Node op = { .k = is_sub ? N_SUB : N_ADD, .t = n->t, .l = n->l, .r = &one };
    IrIns *result = compile_expr(s, &op);
    IrIns *lvalue = result->l;
    assert(lvalue->op == IR_LOAD);
    emit_store(s, lvalue->src, result);
    int is_prefix = (n->k == N_PRE_INC || n->k == N_PRE_DEC);
    if (is_prefix) {
        return result;
    } else {
        return lvalue;
    }
}

static IrIns * compile_deref(Scope *s, Node *n) {
    IrIns *op = discharge(s, compile_expr(s, n->l));
    return emit_load(s, op);
}

static IrIns * compile_addr(Scope *s, Node *n) {
    IrIns *result = compile_expr(s, n->l);
    if (n->l->t->k == T_FN) {
        return result;
    }
    assert(result->op == IR_LOAD || result->op == IR_ELEM); // lvalue
    IrIns *ptr = result->src;
    delete_ir(result); // Remove IR_LOAD/IR_ELEM
    return ptr;
}

static IrIns * compile_conv(Scope *s, Node *n) {
    IrIns *l = discharge(s, compile_expr(s, n->l));
    return emit_conv(s, l, n->t);
}

static IrIns * compile_array_access(Scope *s, Node *n) {
    // TODO: VLA access (via multiplication)
    IrIns *ptr = compile_ptr_arith(s, n);
    return emit_load(s, ptr);
}

static IrIns * compile_field_access(Scope *s, Node *n) {
    IrIns *l = discharge(s, compile_expr(s, n->l));
    Type *ft = ((Field *) vec_get(n->strct->t->fields, n->field_idx))->t;
    IrIns *idx = emit(s, IR_IMM, t_num(T_LLONG, 0));
    idx->imm = n->field_idx;
    IrIns *elem = emit(s, IR_ELEM, t_ptr(ft));
    elem->base = l;
    elem->offset = idx;
    return emit_load(s, elem);
}

static IrIns * compile_call(Scope *s, Node *n) {
    Type *fn_t = n->fn->t;
    if (fn_t->k == T_PTR) {
        fn_t = fn_t->ptr;
    }
    assert(fn_t->k == T_FN);
    IrIns *fn = compile_expr(s, n->fn);
    IrIns *args[vec_len(n->args)];
    for (size_t i = 0; i < vec_len(n->args); i++) {
        Node *arg = vec_get(n->args, i);
        args[i] = discharge(s, compile_expr(s, arg));
    }
    IrIns *call = emit(s, IR_CALL, n->t);
    call->fn = fn;
    for (size_t i = 0; i < vec_len(n->args); i++) {
        Node *arg = vec_get(n->args, i);
        IrIns *carg = emit(s, IR_CARG, arg->t);
        carg->arg = args[i];
    }
    return call;
}

static IrIns * compile_expr(Scope *s, Node *n) {
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
    case N_MUL:       return compile_binop(s, n, IR_MUL);
    case N_DIV:       return compile_binop(s, n, IR_DIV);
    case N_MOD:       return compile_binop(s, n, IR_MOD);
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
    case N_A_DIV:     return compile_arith_assign(s, n, IR_DIV);
    case N_A_MOD:     return compile_arith_assign(s, n, IR_MOD);
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

static void compile_block(Scope *s, Node *n);
static void compile_stmt(Scope *s, Node *n);

static void compile_decl(Scope *s, Node *n) {
    if (n->var->t->k == T_FN) {
        return; // Not an object; doesn't need stack space
    }
    assert(n->var->k == N_LOCAL);
    IrIns *alloc;
    if (n->init && n->init->k == N_INIT) { // Array/struct initializer list
        alloc = compile_init(s, n->init);
    } else if (is_vla(n->var->t)) { // VLA
        Vec *to_mul = vec_new();
        Type *t = n->var->t;
        while (is_vla(t)) {
            IrIns *count = discharge(s, compile_expr(s, t->len));
            vec_push(to_mul, count);
            t->len_ins = emit(s, IR_ALLOC, t_ptr(count->t));
            emit_store(s, t->len_ins, count);
            t = t->elem;
        }
        assert(vec_len(to_mul) > 0); // Is actually a VLA
        IrIns *total = vec_get(to_mul, 0);
        for (size_t i = 1; i < vec_len(to_mul); i++) {
            IrIns *mul = emit(s, IR_MUL, total->t);
            mul->l = vec_get(to_mul, i);
            mul->r = total;
            total = mul;
        }
        alloc = emit(s, IR_ALLOC, t_ptr(t));
        alloc->count = total;
    } else { // Everything else
        alloc = emit(s, IR_ALLOC, t_ptr(n->var->t));
    }
    def_local(s, n->var->var_name, alloc);
    if (n->init && n->init->k != N_INIT) {
        IrIns *v = discharge(s, compile_expr(s, n->init));
        emit_store(s, alloc, v);
    }
}

static void compile_if(Scope *s, Node *n) {
    Vec *brs = vec_new();
    while (n && n->if_cond) { // 'if' and 'else if's
        IrIns *cond = to_cond(s, compile_expr(s, n->if_cond));
        IrBB *body = emit_bb(s);
        patch_branch_chain(cond->true_chain, body);
        compile_block(s, n->if_body);

        IrIns *end_br = emit(s, IR_BR, NULL);
        add_to_branch_chain(brs, &end_br->br, end_br);

        IrBB *after = emit_bb(s);
        patch_branch_chain(cond->false_chain, after);
        n = n->if_else;
    }
    if (n) { // else
        assert(!n->if_cond);
        assert(!n->if_else);
        compile_block(s, n->if_body);
        IrIns *end_br = emit(s, IR_BR, NULL);
        add_to_branch_chain(brs, &end_br->br, end_br);
        emit_bb(s);
    }
    patch_branch_chain(brs, s->fn->last);
}

static void compile_while(Scope *s, Node *n) {
    IrIns *before_br = emit(s, IR_BR, NULL);
    IrBB *cond_bb = emit_bb(s);
    before_br->bb = cond_bb;
    IrIns *cond = to_cond(s, compile_expr(s, n->loop_cond));

    Scope loop;
    enter_scope(&loop, s, SCOPE_LOOP);
    IrBB *body_bb = emit_bb(s);
    patch_branch_chain(cond->true_chain, body_bb);
    compile_block(&loop, n->loop_body);
    IrIns *end_br = emit(s, IR_BR, NULL);
    end_br->br = cond_bb;

    IrBB *after_bb = emit_bb(s);
    patch_branch_chain(cond->false_chain, after_bb);
    patch_branch_chain(loop.breaks, after_bb);
    patch_branch_chain(loop.continues, cond_bb);
}

static void compile_do_while(Scope *s, Node *n) {
    IrIns *before_br = emit(s, IR_BR, NULL);
    Scope loop;
    enter_scope(&loop, s, SCOPE_LOOP);
    IrBB *body_bb = emit_bb(s);
    before_br->br = body_bb;
    compile_block(&loop, n->loop_body);
    IrIns *body_br = emit(s, IR_BR, NULL);

    IrBB *cond_bb = emit_bb(s);
    body_br->br = cond_bb;
    IrIns *cond = to_cond(s, compile_expr(s, n->loop_cond));
    patch_branch_chain(cond->true_chain, body_bb);

    IrBB *after_bb = emit_bb(s);
    patch_branch_chain(cond->false_chain, after_bb);
    patch_branch_chain(loop.breaks, after_bb);
    patch_branch_chain(loop.continues, cond_bb);
}

static void compile_for(Scope *s, Node *n) {
    if (n->init) {
        compile_stmt(s, n->init);
    }

    IrIns *before_br = emit(s, IR_BR, NULL);
    IrBB *start_bb = NULL;
    IrIns *cond = NULL;
    if (n->for_cond) {
        start_bb = emit_bb(s);
        before_br->bb = start_bb;
        cond = to_cond(s, compile_expr(s, n->for_cond));
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
    compile_block(&loop, n->for_body);
    IrIns *end_br = emit(s, IR_BR, NULL);

    IrBB *continue_bb = NULL;
    if (n->for_inc) {
        IrBB *inc_bb = emit_bb(s);
        end_br->br = inc_bb;
        compile_expr(s, n->for_inc);
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

static void compile_switch(Scope *s, Node *n) {
    IrIns *cond = compile_expr(s, n->switch_cond);
    Vec *brs = vec_new(); // of 'IrBB **'
    IrBB **false_br;
    for (size_t i = 0; i < vec_len(n->cases); i++) {
        Node *case_n = vec_get(n->cases, i);
        if (case_n->k == N_CASE) {
            IrIns *val = compile_expr(s, case_n->switch_cond);
            IrIns *cmp = emit(s, IR_EQ, t_num(T_INT, 0));
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
    compile_block(&swtch, n->switch_body);

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

static void compile_case(Scope *s, Node *n) {
    Scope *swtch = find_scope(s, SCOPE_SWITCH);
    assert(swtch); // Checked by parser
    IrIns *end_br = emit(s, IR_BR, NULL);
    IrBB *bb = emit_bb(s);
    end_br->br = bb;
    compile_stmt(s, n->case_body);
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

static void compile_ret(Scope *s, Node *n) {
    IrIns *v = NULL;
    if (n->ret_val) {
        v = discharge(s, compile_expr(s, n->ret_val));
    }
    IrIns *ret = emit(s, IR_RET, NULL);
    ret->val = v;
}

static void compile_stmt(Scope *s, Node *n) {
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

static void compile_block(Scope *s, Node *n) {
    Scope block;
    enter_scope(&block, s, SCOPE_BLOCK);
    while (n) {
        compile_stmt(&block, n);
        n = n->next;
    }
}


// ---- Globals ---------------------------------------------------------------

static void compile_fn_args(Scope *s, Node *n) {
    if (n->t->is_vararg) {
        TODO(); // TODO: vararg fns
    }
    IrIns *fargs[vec_len(n->param_names)];
    for (size_t i = 0; i < vec_len(n->param_names); i++) { // Emit IR_FARGs
        Type *t = vec_get(n->t->params, i);
        IrIns *ins = emit(s, IR_FARG, t);
        ins->arg_num = i;
        fargs[i] = ins;
    }
    for (size_t i = 0; i < vec_len(n->param_names); i++) { // Emit IR_ALLOCs
        char *name = vec_get(n->param_names, i);
        Type *t = vec_get(n->t->params, i);
        IrIns *alloc = emit(s, IR_ALLOC, t_ptr(t));
        emit_store(s, alloc, fargs[i]);
        def_local(s, name, alloc);
    }
}

static void compile_fn_def(Scope *s, Node *n) {
    Global *g = new_global(n->t, prepend_underscore(n->fn_name));
    g->fn = new_fn();
    def_global(s, n->fn_name, g);
    Scope body;
    enter_scope(&body, s, SCOPE_BLOCK);
    body.fn = g->fn;
    compile_fn_args(&body, n);
    compile_block(&body, n->fn_body);
    if (!body.fn->last->last || body.fn->last->last->op != IR_RET) {
        emit(&body, IR_RET, NULL);
    }
}

static void compile_global_decl(Scope *s, Node *n) {
    char *label = prepend_underscore(n->var->var_name);
    Global *g = new_global(n->var->t, label);
    def_global(s, n->var->var_name, g);
    g->val = n->init; // May be NULL
}

static void compile_top_level(Scope *s, Node *n) {
    switch (n->k) {
        case N_DECL:    compile_global_decl(s, n); break;
        case N_FN_DEF:  compile_fn_def(s, n); break;
        case N_TYPEDEF: break; // Ignore
        default:        UNREACHABLE();
    }
}

Vec * compile(Node *n) {
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
