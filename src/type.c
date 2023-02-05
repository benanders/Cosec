
#include <stdlib.h>
#include <string.h>

#include "type.h"
#include "parse.h"

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

Type * t_arr(Type *base, struct Node *len) {
    Type *t = t_new();
    t->k = T_ARR;
    t->elem = base;
    t->len = len;
    if (len && len->k == N_IMM) {
        t->size = t->elem->size * len->imm;
    }
    t->align = 8;
    return t;
}

Type * t_fn(Type *ret, Vec *params, int is_vararg) {
    Type *t = t_new();
    t->k = T_FN;
    t->ret = ret;
    t->params = params;
    t->is_vararg = is_vararg;
    t->size = t->align = 8;
    return t;
}

Type * t_struct() {
    Type *t = t_new();
    t->k = T_STRUCT;
    return t;
}

Type * t_union() {
    Type *t = t_new();
    t->k = T_UNION;
    return t;
}

Type * t_enum() {
    Type *t = t_new();
    t->k = T_ENUM;
    return t;
}

Field * new_field(Type *t, char *name, uint64_t offset) {
    Field *f = malloc(sizeof(Field));
    f->t = t;
    f->name = name;
    f->offset = offset;
    return f;
}

size_t find_field(Type *t, char *name) {
    assert(t->k == T_STRUCT || t->k == T_UNION);
    if (!t->fields) return NOT_FOUND; // Incomplete type
    for (size_t i = 0; i < vec_len(t->fields); i++) {
        Field *f = vec_get(t->fields, i);
        if (strcmp(f->name, name) == 0) {
            return i;
        }
    }
    return NOT_FOUND;
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

int is_string_type(Type *t) {
    return t->k == T_ARR && ((t->elem->k == T_CHAR && !t->elem->is_unsigned) ||
           (t->elem->k == T_SHORT && t->elem->is_unsigned) ||
           (t->elem->k == T_INT && t->elem->is_unsigned));
}

int is_vla(Type *t) {
    return t->k == T_ARR && t->len && t->len->k != N_IMM;
}

int is_incomplete(Type *t) {
    return t->k == T_VOID ||
           (t->k == T_ARR && !t->len) ||
           ((t->k == T_STRUCT || t->k == T_UNION) && !t->fields);
}

static int fields_are_equal(Field *a, Field *b) {
    return strcmp(a->name, b->name) == 0 && a->offset == b->offset &&
           are_equal(a->t, b->t);
}

int are_equal(Type *a, Type *b) {
    if (!a && !b) return 1;
    if (!a || !b) return 0;
    if (a->k != b->k) return 0;
    switch (a->k) {
    case T_PTR: return are_equal(a->ptr, b->ptr);
    case T_ARR: return a->len == b->len && are_equal(a->ptr, b->ptr);
    case T_FN:
        if (vec_len(a->params) != vec_len(b->params)) return 0;
        for (size_t i = 0; i < vec_len(a->params); i++) {
            if (!are_equal(vec_get(a->params, i), vec_get(b->params, i))) return 0;
        }
        return are_equal(a->ret, b->ret);
    case T_STRUCT: case T_UNION:
        if (!a->fields || !b->fields) return 0;
        if (vec_len(a->fields) != vec_len(b->fields)) return 0;
        for (size_t i = 0; i < vec_len(a->fields); i++) {
            if (!fields_are_equal(vec_get(a->fields, i), vec_get(b->fields, i))) return 0;
        }
        return 1;
    default: return a->is_unsigned == b->is_unsigned;
    }
}
