
#ifndef COSEC_CPP_H
#define COSEC_CPP_H

#include "lex.h"

Token * next_tk(Lexer *l);
Token * next_tk_is(Lexer *l, int k);
Token * peek_tk(Lexer *l);
Token * peek_tk_is(Lexer *l, int k);
Token * peek2_tk(Lexer *l);
Token * peek2_tk_is(Lexer *l, int k);
Token * expect_tk(Lexer *l, int k);

#endif
