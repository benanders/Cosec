
#ifndef COSEC_PP_H
#define COSEC_PP_H

#include "lex.h"
#include "util.h"

enum {
    MACRO_OBJ,
    MACRO_FN,
    MACRO_PREDEF,
};

typedef struct {
    int k;
    Vec *body; // of 'Token *'
    size_t nparams; // for MACRO_FN
    int is_vararg;  // for MACRO_FN
} Macro;

typedef struct { // C pre-processor
    Lexer *l;
    Map *macros;
} PP;

PP * new_pp(Lexer *l);

Token * next_tk(PP *pp);
Token * next_tk_is(PP *pp, int k);
Token * peek_tk(PP *pp);
Token * peek_tk_is(PP *pp, int k);
Token * peek2_tk(PP *pp);
Token * peek2_tk_is(PP *pp, int k);
Token * expect_tk(PP *pp, int k);

#endif
