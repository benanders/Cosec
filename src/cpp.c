
#include "cpp.h"
#include "err.h"


// ---- Tokens ----------------------------------------------------------------

Token * next_tk(Lexer *l) {
    Token *t = lex_tk(l); // Just pass through for now
    while (t->k == TK_NEWLINE) {
        t = lex_tk(l);
    }
    return t;
}

Token * next_tk_is(Lexer *l, int k) {
    Token *t = next_tk(l);
    if (t->k == k) {
        return t;
    }
    undo_tk(l, t);
    return NULL;
}

Token * peek_tk(Lexer *l) {
    Token *t = next_tk(l);
    undo_tk(l, t);
    return t;
}

Token * peek_tk_is(Lexer *l, int k) {
    Token *t = peek_tk(l);
    return t->k == k ? t : NULL;
}

Token * peek2_tk(Lexer *l) {
    Token *t = next_tk(l);
    Token *t2 = peek_tk(l);
    undo_tk(l, t);
    return t2;
}

Token * peek2_tk_is(Lexer *l, int k) {
    Token *t = peek2_tk(l);
    return t->k == k ? t : NULL;
}

Token * expect_tk(Lexer *l, int k) {
    Token *t = next_tk(l);
    if (t->k != k) {
        error_at(t, "expected %s, found %s", tk2str(k), token2str(t));
    } else {
        return t;
    }
}
