
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

typedef enum {
    MACRO_OBJ,
    MACRO_FN,
    MACRO_BUILT_IN,
} MacroT;

typedef struct {
    MacroT k;
    Vec *body; // of 'Token *'
    union {
        struct { size_t num_params; int is_vararg; }; // MACRO_FN
        BuiltIn built_in; // MACRO_BUILT_IN
    };
} Macro;

typedef enum {
    COND_IF,
    COND_ELIF,
    COND_ELSE,
} CondT;

typedef struct {
    CondT k;
    int is_true;
} Cond;

PP * new_pp(Lexer *l);
Token * next_tk(PP *pp);
Token * next_tk_is(PP *pp, TokenT k);
Token * peek_tk(PP *pp);
Token * peek_tk_is(PP *pp, TokenT k);
Token * peek2_tk(PP *pp);
Token * peek2_tk_is(PP *pp, TokenT k);
Token * expect_tk(PP *pp, TokenT k);

#endif
