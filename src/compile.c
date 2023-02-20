
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "compile.h"

#define STR_PREFIX "_S."

typedef struct Scope {
    struct Scope *outer;
    Vec *globals;
    IrFn *fn;
    Map *vars; // Block: 'IrIns *' k = IR_ALLOC; file: 'Global *'
} Scope;

static void enter_scope(Scope *inner, Scope *outer) {
    *inner = (Scope) {0};
    inner->outer = outer;
    inner->globals = outer->globals;
    inner->fn = outer->fn;
    inner->vars = map_new();
}

static Scope * find_global_scope(Scope *s) {
    while (s->outer) {
        s = s->outer;
    }
    assert(!s->fn);
    return s;
}

static Global * new_global(int k, Type *t, char *label) {
    Global *g = calloc(1, sizeof(Global));
    g->k = k;
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
        assert(strcmp(g->label, g2->label) != 0); // No duplicates
    }
    vec_push(s->globals, g);
    if (name) {
        Scope *gs = find_global_scope(s);
        assert(gs);
        map_put(gs->vars, name, g);
    }
}

static Global * def_str(Scope *s, Node *n) {
    assert(n->k == N_STR);
    size_t i = vec_len(s->globals);
    int num_digits = (i == 0) ? 1 : (int) log10((double) i) + 1;
    char *label = malloc(sizeof(char) * (num_digits + strlen(STR_PREFIX) + 1));
    sprintf(label, STR_PREFIX "%zu", i);
    Global *g = new_global(K_STR, n->t, label);
    switch (n->enc) {
        case ENC_NONE: g->str = n->str; break;
        case ENC_CHAR16: g->str16 = n->str16; break;
        case ENC_CHAR32: case ENC_WCHAR: g->str32 = n->str32; break;
    }
    g->len = n->len;
    g->enc = n->enc;
    def_global(s, NULL, g);
    return g;
}

static Global * def_arr(Scope *s, Node *n) {
    assert(n->k == N_ARR);
    TODO();
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
    Scope *gs = find_global_scope(s);
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
    if (l->t->ptr->k == T_ARR) { // Pointer to an array
        IrIns *zero = emit(s, IR_IMM, t_num(T_LLONG, 0));
        zero->imm = 0;
        IrIns *elem = emit(s, IR_ELEM, l->t->ptr);
        elem->base = l;
        elem->offset = zero;
        return elem;
    } else if (l->t->ptr->k == T_FN) { // Pointer to a function
        return l;
    } else {
        IrIns *load = emit(s, IR_LOAD, l->t->ptr);
        load->src = l;
        return load;
    }
}

static IrIns * compile_arr(Scope *s, Node *n) {
    assert(n->t->k == T_ARR);
    IrIns *alloc = emit(s, IR_ALLOC, t_ptr(n->var->t));
    IrIns *arr = emit(s, IR_ELEM, alloc->t->ptr);
    arr->base = alloc;
    arr->offset = 0;
    IrIns *elem;
//    for (size_t i = 0; i < vec_len(n->inits); i++) {
//        Node *init = vec_get(n->inits, i);
//        assert(init->k == N_INIT);
//        if (i == 0) {
//            elem = emit(s, IR_ELEM, t_ptr(arr->t->elem));
//            elem->base = arr;
//            elem->offset = 0;
//        } else {
//
//        }
//        IrIns *v = discharge(s, compile_expr(s, n));
//        IrIns *store = emit(s, IR_STORE, NULL);
//        store->dst = elem;
//        store->src = v;
//    }
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
        ins->g = def_str(s, n);
        break;
    case N_ARR:
        ins = compile_arr(s, n);
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

static IrIns * compile_assign(Scope *s, Node *n) {
    IrIns *r = discharge(s, compile_expr(s, n->r));
    IrIns *l = compile_expr(s, n->l);
    assert(l->op == IR_LOAD || l->op == IR_ELEM); // lvalue
    l->op = IR_STORE; // Change to store
    l->dst = l->src;
    l->src = r;
    l->t = NULL;
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
    IrIns *store = emit(s, IR_STORE, NULL);
    store->src = binop;
    store->dst = lvalue->src;
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
    Node one = {.k = N_IMM, .t = t, .imm = 1};
    Node fake_op = {.k = is_sub ? N_SUB : N_ADD, .t = n->t, .l = n->l, .r = &one};
    IrIns *result = compile_expr(s, &fake_op);
    IrIns *lvalue = result->l;
    assert(lvalue->op == IR_LOAD);
    IrIns *store = emit(s, IR_STORE, NULL);
    store->src = result;
    store->dst = lvalue->src;
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
    IrIns *ptr = compile_ptr_arith(s, n);
    return emit_load(s, ptr);
}

static IrIns * compile_field_access(Scope *s, Node *n) {
    IrIns *l = discharge(s, compile_expr(s, n->l));
    assert(l->op == IR_LOAD);
    delete_ir(l);
    size_t idx = find_field(n->strct->t, n->field_name);
    assert(idx != NOT_FOUND); // Checked by parser
    Field *f = vec_get(n->strct->t->fields, idx);
    IrIns *offset = emit(s, IR_IMM, t_num(T_LLONG, 0));
    offset->imm = f->offset;
    IrIns *elem = emit(s, IR_ELEM, t_ptr(f->t));
    elem->base = l->src;
    elem->offset = offset;
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
        } else {
            return compile_binop(s, n, IR_ADD);
        }
    case N_SUB:
        if (n->l->t->k == T_PTR && n->r->t->k == T_PTR) {
            return compile_ptr_sub(s, n);
        } else if (n->l->t->k == T_PTR || n->r->t->k == T_PTR) {
            return compile_ptr_arith(s, n);
        } else {
            return compile_binop(s, n, IR_SUB);
        }
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

static void compile_decl(Scope *s, Node *n) {
    if (n->var->t->k == T_FN) {
        return; // Not an object; doesn't need stack space
    }
    assert(n->var->k == N_LOCAL);
    IrIns *alloc = emit(s, IR_ALLOC, t_ptr(n->var->t));
    def_local(s, n->var->var_name, alloc);
    if (n->init) {
        IrIns *v = discharge(s, compile_expr(s, n->init));
        IrIns *store = emit(s, IR_STORE, NULL);
        store->src = v;
        store->dst = alloc;
    }
}

static void compile_stmt(Scope *s, Node *n) {
    switch (n->k) {
        case N_TYPEDEF: break;
        case N_DECL:    compile_decl(s, n); break;
        default:        discharge(s, compile_expr(s, n)); break;
    }
}

static void compile_block(Scope *s, Node *n) {
    Scope block;
    enter_scope(&block, s);
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
        IrIns *store = emit(s, IR_STORE, NULL);
        store->dst = alloc;
        store->src = fargs[i];
        def_local(s, name, alloc);
    }
}

static void compile_fn_def(Scope *s, Node *n) {
    Global *g = new_global(K_FN_DEF, n->t, prepend_underscore(n->fn_name));
    g->fn = new_fn();
    def_global(s, n->fn_name, g);
    Scope body;
    enter_scope(&body, s);
    body.fn = g->fn;
    compile_fn_args(&body, n);
    compile_block(&body, n->fn_body);
    if (!body.fn->last || body.fn->last->last->op != IR_RET) {
        emit(&body, IR_RET, NULL);
    }
}

static void compile_global_decl(Scope *s, Node *n) {
    TODO();
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
    top_level.globals = vec_new();
    top_level.vars = map_new();
    while (n) {
        compile_top_level(&top_level, n);
        n = n->next;
    }
    return top_level.globals;
}
