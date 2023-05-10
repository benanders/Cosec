
#ifndef COSEC_PP_H
#define COSEC_PP_H

#include <time.h>

#include "lex.h"
#include "util.h"

typedef struct { // C pre-processor
    Lexer *l;
    Map *macros;
    Vec *conds; // For nested '#if's
    Map *include_once;
    Vec *include_paths;
    struct tm now;
} PP;

typedef void (*BuiltIn)(PP *pp, Token *t);

enum {
    MACRO_OBJ,
    MACRO_FN,
    MACRO_BUILT_IN,
};

typedef struct {
    int k;
    Vec *body; // of 'Token *'
    union {
        struct { size_t num_params; int is_vararg; }; // MACRO_FN
        BuiltIn built_in; // MACRO_BUILT_IN
    };
} Macro;

enum {
    COND_IF,
    COND_ELIF,
    COND_ELSE,
};

typedef struct {
    int k;
    int is_true;
} Cond;

PP * new_pp(Lexer *l);
Token * next_tk(PP *pp);
Token * next_tk_is(PP *pp, int k);
Token * peek_tk(PP *pp);
Token * peek_tk_is(PP *pp, int k);
Token * peek2_tk(PP *pp);
Token * peek2_tk_is(PP *pp, int k);
Token * expect_tk(PP *pp, int k);

#endif
