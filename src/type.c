
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
    t->k = type;
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
    t->k = T_PTR;
    t->ptr = base;
    t->size = t->align = 8;
    return t;
}

Type * t_arr(Type *base, uint64_t len) {
    Type *t = t_new();
    t->k = T_ARR;
    t->elem = base;
    t->len = len;
    t->size = t->elem->size * len;
    t->align = 8;
    return t;
}

Type * t_fn(Type *ret, Vec *params) {
    Type *t = t_new();
    t->k = T_FN;
    t->ret = ret;
    t->params = params;
    t->size = t->align = 8;
    return t;
}

Type * t_struct() {
    Type *t = t_new();
    t->k = T_STRUCT;
    t->fields = vec_new();
    return t;
}

Type * t_union() {
    Type *t = t_new();
    t->k = T_UNION;
    t->fields = vec_new();
    return t;
}

Type * t_enum() {
    Type *t = t_new();
    t->k = T_ENUM;
    return t;
}

Field * new_field(Type *t, char *name, size_t offset) {
    Field *f = malloc(sizeof(Field));
    f->t = t;
    f->name = name;
    f->offset = offset;
    return f;
}

int is_int(Type *t) {
    return t->k >= T_CHAR && t->k <= T_LLONG;
}

int is_fp(Type *t) {
    return t->k >= T_FLOAT && t->k <= T_LDOUBLE;
}

int is_arith(Type *t) {
    return is_int(t) || is_fp(t);
}

int is_void_ptr(Type *t) {
    return t->k == T_PTR && t->ptr->k == T_VOID;
}

int is_char_arr(Type *t) {
    return t->k == T_ARR && t->elem->k == T_CHAR;
}

int are_equal(Type *a, Type *b) {
    if (!a && !b) return 1;
    if (!a || !b) return 0;
    switch (a->k) {
    case T_PTR: return are_equal(a->ptr, b->ptr);
    case T_ARR: return a->len == b->len && are_equal(a->ptr, b->ptr);
    case T_FN:
        if (vec_len(a->params) != vec_len(b->params)) return 0;
        for (size_t i = 0; i < vec_len(a->params); i++) {
            if (!are_equal(vec_get(a->params, i), vec_get(b->params, i))) return 0;
        }
        return are_equal(a->ret, b->ret);
    case T_STRUCT: case T_ENUM: case T_UNION:
        TODO();
    default: return a->k == b->k && a->is_unsigned == b->is_unsigned;
    }
}
