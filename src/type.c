
#include <stdlib.h>

#include "type.h"

Type * t_new() {
    Type *t = calloc(1, sizeof(Type));
    return t;
}

Type * t_copy(Type *t) {
    Type *t2 = t_new();
    *t2 = *t;
    return t2;
}

Type * t_num(int type, int is_unsigned) {
    Type *t = t_new();
    t->t = type;
    t->is_unsigned = is_unsigned;
    switch (type) {
        case T_VOID:  t->size = t->align = 0; break;
        case T_CHAR:  t->size = t->align = 1; break;
        case T_SHORT: t->size = t->align = 2; break;
        case T_INT: case T_LONG: case T_FLOAT: t->size = t->align = 4; break;
        case T_LLONG: case T_DOUBLE: case T_LDOUBLE: t->size = t->align = 8; break;
        default: UNREACHABLE();
    }
    return t;
}

Type * t_ptr(Type *base) {
    Type *t = t_new();
    t->t = T_PTR;
    t->ptr = base;
    t->size = t->align = 8;
    return t;
}

Type * t_arr(Type *base, uint64_t len) {
    Type *t = t_new();
    t->t = T_ARR;
    t->ptr = base;
    t->len = len;
    t->size = t->align = 8;
    return t;
}

Type * t_fn(Type *ret, Type **args, int nargs) {
    Type *t = t_new();
    t->t = T_FN;
    t->ret = ret;
    t->args = args;
    t->nargs = nargs;
    t->size = t->align = 8;
    return t;
}

int is_int(Type *t) {
    return t->t >= T_CHAR && t->t <= T_LLONG;
}

int is_fp(Type *t) {
    return t->t >= T_FLOAT && t->t <= T_LDOUBLE;
}

int is_arith(Type *t) {
    return is_int(t) || is_fp(t);
}

int is_void_ptr(Type *t) {
    return t->t == T_PTR && t->ptr->t == T_VOID;
}

int are_equal(Type *a, Type *b) {
    switch (a->t) {
    case T_PTR: return are_equal(a->ptr, b->ptr);
    case T_ARR: return a->len == b->len && are_equal(a->ptr, b->ptr);
    case T_FN:
        if (a->nargs != b->nargs) return 0;
        for (int i = 0; i < a->nargs; i++) {
            if (!are_equal(a->args[i], b->args[i])) return 0;
        }
        return are_equal(a->ret, b->ret);
    default: return a->t == b->t && a->is_unsigned == b->is_unsigned;
    }
}
