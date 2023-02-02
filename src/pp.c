
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

// Copy file, line, column info from [from] to every token in [tks] for error
// messages from expanded macros; set [has_preceding_space] for the first
// token in [tks]
static void copy_pos_info_to_tks(Vec *tks, Token *from) {
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


// ---- '#pragma' -------------------------------------------------------------

static void parse_pragma(PP *pp) {
    Token *t = lex_expect(pp->l, TK_IDENT);
    error_at(t, "unsupported pragma directive '%s'", t->s);
}


// ---- '#define' -------------------------------------------------------------

static Macro * parse_obj_macro(PP *pp) {
    Vec *body = vec_new();
    Token *t = lex_next(pp->l);
    while (t->k != TK_NEWLINE && t->k != TK_EOF) {
        vec_push(body, t);
        t = lex_next(pp->l);
    }
    // TODO: check '##' doesn't appear at start or end of macro body
    return new_macro(MACRO_OBJ, body);
}

static Map * parse_params(PP *pp) {
    lex_expect(pp->l, '(');
    Map *params = map_new();
    int nparams = 0;
    Token *t = lex_peek(pp->l);
    while (t->k != ')' && t->k != TK_NEWLINE && t->k != TK_EOF) {
        t = lex_expect(pp->l, TK_IDENT);
        char *name = t->s;
        t->k = TK_MACRO_PARAM;
        t->param_pos = nparams++;
        map_put(params, name, t);
        t = lex_next(pp->l); // Skip ','
        if (t->k != ',') {
            break;
        }
    }
    undo_tk(pp->l, t);
    lex_expect(pp->l, ')');
    return params;
}

static Vec * parse_body(PP *pp, Map *params) {
    Vec *body = vec_new();
    Token *t = lex_next(pp->l);
    while (t->k != TK_NEWLINE && t->k != TK_EOF) {
        if (t->k == TK_IDENT) {
            Token *param = map_get(params, t->s);
            if (param) {
                t->k = TK_MACRO_PARAM;
                t->param_pos = param->param_pos;
            }
        }
        vec_push(body, t);
        t = lex_next(pp->l);
    }
    // TODO: check '##' doesn't appear at start or end of macro body
    return body;
}

static Macro * parse_fn_macro(PP *pp) {
    Map *params = parse_params(pp);
    Vec *body = parse_body(pp, params);
    Macro *m = new_macro(MACRO_FN, body);
    m->nparams = map_len(params);
    return m;
}

static void parse_define(PP *pp) {
    Token *name = lex_expect(pp->l, TK_IDENT);
    Macro *m;
    Token *t = lex_peek(pp->l);
    if (t->k == '(' && !t->has_preceding_space) {
        m = parse_fn_macro(pp);
    } else {
        m = parse_obj_macro(pp);
    }
    map_put(pp->macros, name->s, m);
}


// ---- Macro Expansion -------------------------------------------------------

static void parse_directive(PP *pp);
static Token * expand_next(PP *pp);

static Vec * pre_expand_arg(PP *pp, Vec *arg) {
    Lexer *prev = pp->l; // New temporary lexer containing the arg
    pp->l = new_lexer(NULL);
    undo_tks(pp->l, arg);
    Vec *expanded = vec_new();
    while (1) {
        Token *t = expand_next(pp);
        if (t->k == TK_EOF) {
            break;
        }
        vec_push(expanded, t);
    }
    pp->l = prev;
    return expanded;
}

static void add_to_hide_sets(Vec *tks, Set *hide_set) {
    for (size_t i = 0; i < vec_len(tks); i++) {
        Token *t = vec_get(tks, i);
        set_union(&t->hide_set, hide_set);
    }
}

static Vec * substitute(PP *pp, Macro *m, Vec *args, Set *hide_set) {
    Vec *tks = vec_new();
    for (size_t i = 0; i < vec_len(m->body); i++) {
        Token *t = copy_tk(vec_get(m->body, i));
        if (t->k == TK_MACRO_PARAM) {
            Vec *arg = vec_get(args, t->param_pos);
            vec_push_all(tks, pre_expand_arg(pp, arg));
        } else {
            vec_push(tks, t);
        }
    }
    add_to_hide_sets(tks, hide_set);
    return tks;
}

static Vec * parse_args(PP *pp, Macro *m) {
    lex_expect(pp->l, '(');
    Vec *args = vec_new();
    if (m->nparams == 1 && lex_peek(pp->l)->k == ')') {
        vec_push(args, vec_new());
        return args; // Empty single argument
    }
    Token *t = lex_peek(pp->l);
    while (t->k != ')' && t->k != TK_EOF) {
        Vec *arg = vec_new();
        int level = 0;
        while (1) {
            t = lex_next(pp->l);
            if (t->k == TK_EOF) break;
            if (t->k == TK_NEWLINE) continue;
            if (t->k == '#' && t->col == 1) {
                parse_directive(pp);
                continue;
            }
            if ((t->k == ')' || t->k == ',') && level == 0) {
                if (t->k == ')') undo_tk(pp->l, t);
                break;
            }
            if (t->k == '(') level++;
            if (t->k == ')') level--;
            vec_push(arg, t);
        }
        vec_push(args, arg);
        t = lex_peek(pp->l);
    }
    return args;
}

static Token * expand_next(PP *pp) {
    Token *t = lex_next(pp->l);
    while (t->k == TK_NEWLINE) { // Ignore newlines
        t = lex_next(pp->l);
        t->has_preceding_space = 1;
    }
    if (t->k != TK_IDENT) {
        return t;
    }
    Macro *m = map_get(pp->macros, t->s);
    if (!m || set_has(t->hide_set, t->s)) {
        return t; // No macro, or macro self-reference
    }
    Vec *tks;
    switch (m->k) {
    case MACRO_OBJ:
        set_put(&t->hide_set, t->s);
        tks = substitute(pp, m, NULL, t->hide_set);
        break;
    case MACRO_FN:
        if (lex_peek(pp->l)->k != '(') return t;
        Vec *args = parse_args(pp, m);
        if (vec_len(args) != m->nparams) {
            error_at(t, "incorrect number of arguments provided to "
                        "function-like macro (have %zu, expected %zu)",
                        vec_len(args), m->nparams);
        }
        Token *rparen = lex_expect(pp->l, ')');
        set_intersection(&t->hide_set, rparen->hide_set);
        set_put(&t->hide_set, t->s);
        tks = substitute(pp, m, args, t->hide_set);
        break;
    case MACRO_PREDEF: TODO();
    }
    copy_pos_info_to_tks(tks, t); // For error messages
    undo_tks(pp->l, tks);
    return expand_next(pp);
}


// ---- Tokens and Directives -------------------------------------------------

static void parse_directive(PP *pp) {
    Token *t = lex_next(pp->l);
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
        error_at(t, "expected %s, found %s", tk2pretty_str(k), token2pretty_str(t));
    }
    return t;
}
