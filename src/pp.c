
#include <stdlib.h>
#include <string.h>

#include "pp.h"
#include "err.h"

PP * new_pp(Lexer *l) {
    PP *pp = calloc(1, sizeof(PP));
    pp->l = l;
    pp->macros = map_new();
    return pp;
}

Macro * new_macro(int k, Vec *body) {
    Macro *m = calloc(1, sizeof(Macro));
    m->k = k;
    m->body = body;
    return m;
}


// ---- '#pragma' -------------------------------------------------------------

static void parse_pragma(PP *pp) {
    Token *t = lex_tk(pp->l);
    if (t->k != TK_IDENT) {
        error_at(t, "expected identifier after '#pragma'");
    }
    error_at(t, "unsupported pragma directive '%s'", t->s);
}


// ---- '#define' -------------------------------------------------------------

static void parse_obj_macro(PP *pp, Token *name) {
    Vec *body = vec_new();
    Token *t = lex_tk(pp->l);
    while (t->k != TK_NEWLINE) {
        vec_push(body, t);
        t = lex_tk(pp->l);
    }
    // TODO: check '##' doesn't appear at start or end of macro
    map_put(pp->macros, name->s, new_macro(MACRO_OBJ, body));
}

static void parse_fn_macro(PP *pp, Token *name) {
    TODO();
}

static void parse_define(PP *pp) {
    Token *name = lex_tk(pp->l);
    if (name->k != TK_IDENT) {
        error_at(name, "expected identifier after '#define'");
    }
    Token *t = lex_tk(pp->l);
    if (t->k == '(' && !t->has_preceding_space) {
        parse_fn_macro(pp, name);
    } else {
        undo_tk(pp->l, t);
        parse_obj_macro(pp, name);
    }
}


// ---- Macro Expansion -------------------------------------------------------

// Copy file, line, column info from [from] to every token in [tks] (for error
// messages from expanded macros); set [has_preceding_space] for the first
// token in [tks]
static void copy_tk_pos_info(Vec *tks, Token *from) {
    for (size_t i = 0; i < vec_len(tks); i++) {
        Token *to = vec_get(tks, i);
        to->f = from->f;
        to->line = from->line;
        to->col = from->col;
        if (i == 0) {
            to->has_preceding_space = from->has_preceding_space;
        }
    }
}

static void add_to_hide_sets(Vec *tks, Set *hide_set) {
    for (size_t i = 0; i < vec_len(tks); i++) {
        Token *t = vec_get(tks, i);
        set_union(&t->hide_set, hide_set);
    }
}

static Vec * substitute(Macro *m, Set *hide_set) {
    Vec *tks = vec_new();
    for (size_t i = 0; i < vec_len(m->body); i++) {
        Token *t = copy_tk(vec_get(m->body, i));
        vec_push(tks, t);
    }
    add_to_hide_sets(tks, hide_set);
    return tks;
}

static Token * expand_next(PP *pp) {
    Token *t = lex_tk(pp->l);
    if (t->k != TK_IDENT) {
        return t;
    }
    Macro *macro = map_get(pp->macros, t->s);
    if (!macro || set_has(t->hide_set, t->s)) {
        return t; // No macro, or macro self-reference
    }
    switch (macro->k) {
    case MACRO_OBJ:
        set_put(&t->hide_set, t->s);
        Vec *tks = substitute(macro, t->hide_set);
        copy_tk_pos_info(tks, t); // For error messages
        undo_tks(pp->l, tks);
        break;
    case MACRO_FN: TODO();
    case MACRO_PREDEF: TODO();
    }
    return expand_next(pp);
}


// ---- Tokens ----------------------------------------------------------------

static void parse_directive(PP *pp) {
    Token *t = lex_tk(pp->l);
    if (t->k != TK_IDENT) {
        goto err;
    }
    if (strcmp(t->s, "define") == 0)      parse_define(pp);
    else if (strcmp(t->s, "pragma") == 0) parse_pragma(pp);
    else goto err;
    return;
err:
    error_at(t, "unsupported preprocessor directive %s", token2str(t));
}

Token * next_tk(PP *pp) {
    while (1) {
        Token *t = expand_next(pp);
        if (t->k == TK_NEWLINE) { // Ignore newlines
            continue;
        }
        if (t->k == '#' && t->col == 1 && !t->hide_set) { // '#' at line start
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
    }
    return t;
}
