
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
    if (!t->fields) {
        return NOT_FOUND; // Incomplete type
    }
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
    if (t->k != T_ARR) return 0;
    if (t->len && t->len->k != N_IMM) return 1;
    return is_vla(t->elem);
}

int is_incomplete(Type *t) {
    switch (t->k) {
    case T_VOID: return 1;
    case T_ARR:
        if (!t->len) return 1;
        return is_incomplete(t->elem);
    case T_STRUCT: case T_UNION:
        if (!t->fields) return 1;
        for (size_t i = 0; i < vec_len(t->fields); i++) {
            Type *ft = ((Field *) vec_get(t->fields, i))->t;
            if (is_incomplete(ft)) {
                return 1;
            }
        }
    case T_ENUM: return !t->fields;
    default: return 0;
    }
}

int are_equal(Type *a, Type *b) {
    if (!a && !b) return 1;
    if (!a || !b) return 0;
    if (a->k != b->k) return 0;
    switch (a->k) {
    case T_PTR: return are_equal(a->ptr, b->ptr);
    case T_ARR:
        if (a->len && b->len && a->len->k == N_IMM && b->len->k == N_IMM &&
                a->len->k != b->len->k) {
            return 0;
        }
        return are_equal(a->elem, b->elem);
    case T_FN:
        if (vec_len(a->params) != vec_len(b->params)) return 0;
        for (size_t i = 0; i < vec_len(a->params); i++) {
            if (!are_equal(vec_get(a->params, i), vec_get(b->params, i))) return 0;
        }
        return are_equal(a->ret, b->ret);
    case T_STRUCT: case T_UNION: case T_ENUM:
        if (!a->fields || !b->fields) return 0;
        if (vec_len(a->fields) != vec_len(b->fields)) return 0;
        for (size_t i = 0; i < vec_len(a->fields); i++) {
            Field *fa = vec_get(a->fields, i);
            Field *fb = vec_get(b->fields, i);
            if (!(strcmp(fa->name, fb->name) == 0 && fa->offset == fb->offset &&
                   are_equal(fa->t, fb->t))) {
                return 0;
            }
        }
        return 1;
    default: return a->is_unsigned == b->is_unsigned;
    }
}
