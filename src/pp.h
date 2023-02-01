
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
} Macro;

typedef struct { // C pre-processor
    Lexer *l;
    Map *macros;
} PP;

PP * new_pp(Lexer *l);

Token * next_tk(PP *p);
Token * next_tk_is(PP *p, int k);
Token * peek_tk(PP *p);
Token * peek_tk_is(PP *p, int k);
Token * peek2_tk(PP *p);
Token * peek2_tk_is(PP *p, int k);
Token * expect_tk(PP *p, int k);

#endif
