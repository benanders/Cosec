
#ifndef COSEC_PP_H
#define COSEC_PP_H

#include <time.h>

#include "lex.h"
#include "util.h"

enum {
    MACRO_OBJ,
    MACRO_FN,
    MACRO_BUILT_IN,
};

struct PP;
typedef void (* BuiltIn)(struct PP *pp, Token *t);

typedef struct {
    int k;
    Vec *body; // of 'Token *'
    size_t nparams; // for MACRO_FN
    int is_vararg;  // for MACRO_FN
    BuiltIn build_in; // for MACRO_BUILT_IN
} Macro;

enum {
    COND_IF,
    COND_ELIF,
    COND_ELSE,
};

typedef struct {
    int k;
    int was_true;
} Cond;

typedef struct PP { // C pre-processor
    Lexer *l;
    Map *macros;
    Vec *conds;
    Map *include_once;
    Vec *include_paths;
    struct tm now;
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
