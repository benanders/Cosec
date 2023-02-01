
#include <stdlib.h>

#include "pp.h"
#include "err.h"

PP * new_pp(Lexer *l) {
    PP *pp = calloc(1, sizeof(PP));
    pp->l = l;
    pp->macros = map_new();
    return pp;
}


// ---- Macros ----------------------------------------------------------------

static Vec * add_to_hide_sets(Vec *tks, Set *hide_set) {
    Vec *v = vec_new();
    for (size_t i = 0; i < vec_len(tks); i++) {
        Token *t = copy_tk(vec_get(tks, i));
        t->hide_set = set_union(t->hide_set, hide_set);
        vec_push(v, t);
    }
    return v;
}

static Vec * substitute(Macro *m, Set *hide_set) {
    Vec *tks = vec_new();
    for (size_t i = 0; i < vec_len(m->body); i++) {
        Token *t = vec_get(m->body, i);
        vec_push(tks, t);
    }
    return add_to_hide_sets(tks, hide_set);
}

static Token * expand_next(PP *pp) {
    Token *t = lex_tk(pp->l);
    if (t->k != TK_IDENT) {
        return t;
    }
    Macro *macro = map_get(pp->macros, t->s);
    if (!macro || set_has(t->hide_set, t->s)) {
        return t; // Macro self-reference
    }
    switch (macro->k) {
    case MACRO_OBJ:
        set_put(t->hide_set, t->s);
        Vec *tks = substitute(macro, t->hide_set);
        // TODO: propagate space
        undo_tks(pp->l, tks);
        break;
    case MACRO_FN: TODO();
    case MACRO_PREDEF: TODO();
    }
    return expand_next(pp);
}


// ---- Tokens ----------------------------------------------------------------

static void parse_directive(PP *pp) {

}

Token * next_tk(PP *pp) {
    while (1) {
        Token *t = expand_next(pp);
        if (t->k == TK_NEWLINE) { // Ignore newlines
            continue;
        }
        if (t->col == 1 && t->k == '#' && !t->hide_set) { // '#' at line start
            parse_directive(pp);
            continue;
        }
        return t;
    }
}

Token * next_tk_is(PP *pp, int k) {
    Token *t = next_tk(pp);
    if (t->k == k) {
        return t;
    }
    undo_tk(pp->l, t);
    return NULL;
}

Token * peek_tk(PP *pp) {
    Token *t = next_tk(pp);
    undo_tk(pp->l, t);
    return t;
}

Token * peek_tk_is(PP *pp, int k) {
    Token *t = peek_tk(pp);
    return t->k == k ? t : NULL;
}

Token * peek2_tk(PP *pp) {
    Token *t = next_tk(pp);
    Token *t2 = peek_tk(pp);
    undo_tk(pp->l, t);
    return t2;
}

Token * peek2_tk_is(PP *pp, int k) {
    Token *t = peek2_tk(pp);
    return t->k == k ? t : NULL;
}

Token * expect_tk(PP *pp, int k) {
    Token *t = next_tk(pp);
    if (t->k != k) {
        error_at(t, "expected %s, found %s", tk2str(k), token2str(t));
    } else {
        return t;
    }
}
