
#ifndef COSEC_TYPE_H
#define COSEC_TYPE_H

#include <stdint.h>

#include "util.h"

enum { // Storage classes
    S_TYPEDEF = 1,
    S_EXTERN,
    S_STATIC,
    S_AUTO,
    S_REGISTER,
};

enum { // Type qualifiers
    T_CONST    = 0b001,
    T_RESTRICT = 0b010,
    T_VOLATILE = 0b100,
};

enum { // Function specifiers
    F_INLINE = 1,
};

enum { // Types
    T_VOID = 1,
    T_CHAR,
    T_SHORT,
    T_INT,
    T_LONG,
    T_LLONG,
    T_FLOAT,
    T_DOUBLE,
    T_LDOUBLE,

    T_PTR,
    T_ARR,
    T_FN,
    T_STRUCT,
    T_UNION,
    T_ENUM,
};

typedef struct {
    struct Type *t;
    char *name;
} Field;

typedef struct Type {
    int t;
    int size, align;
    int is_unsigned;  // for T_CHAR to T_LLONG
    int is_static;    // The only type specifier we care about

    struct Type *ptr; // for T_PTR
    uint64_t len;     // for T_ARR

    struct Type *ret; // for T_FN
    struct Type **args;
    int nargs;

    Field *fields; // for T_STRUCT and T_UNION
    int nfields;
} Type;

Type * t_new();
Type * t_copy(Type *t);
Type * t_num(int t, int is_unsigned);
Type * t_ptr(Type *base);
Type * t_arr(Type *base, uint64_t len);
Type * t_fn(Type *ret, Type **args, int nargs);

int is_int(Type *t);
int is_fp(Type *t);
int is_arith(Type *t);
int is_void_ptr(Type *t);
int are_equal(Type *a, Type *b);

#endif
