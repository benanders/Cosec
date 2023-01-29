
#ifndef COSEC_TYPE_H
#define COSEC_TYPE_H

#include <stdint.h>

#include "util.h"

#define NO_ARR_LEN ((uint64_t) -1)

enum { // Storage classes
    S_NONE,
    S_TYPEDEF,
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

enum { // Linkage
    L_NONE,
    L_STATIC,
    L_EXTERN,
};

typedef struct {
    struct Type *t;
    char *name;
    size_t offset; // 0 for unions
} Field;

typedef struct Type {
    int k;
    size_t size, align;
    int linkage;
    union {
        int is_unsigned;  // T_CHAR to T_LLONG
        struct Type *ptr; // T_PTR
        struct { struct Type *elem; uint64_t len; }; // T_ARR
        struct { struct Type *ret; Vec *params; /* of 'Type *' */ }; // T_FN
        Vec *fields; // T_STRUCT, T_UNION
    };
} Type;

Type * t_new();
Type * t_copy(Type *t);
Type * t_num(int t, int is_unsigned);
Type * t_ptr(Type *base);
Type * t_arr(Type *base, uint64_t len);
Type * t_fn(Type *ret, Vec *args);
Type * t_struct();
Type * t_union();
Type * t_enum();

Field * new_field(Type *t, char *name, size_t offset);

int is_int(Type *t);
int is_fp(Type *t);
int is_arith(Type *t);
int is_void_ptr(Type *t);
int is_char_arr(Type *t);
int are_equal(Type *a, Type *b);

#endif
