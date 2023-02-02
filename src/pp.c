
#include <stdlib.h>
#include <string.h>

#include "pp.h"
#include "err.h"

// Preprocessor macro expansion uses Dave Prosser's algorithm, described here:
// https://www.spinellis.gr/blog/20060626/cpp.algo.pdf
// Read https://www.math.utah.edu/docs/info/cpp_1.html beforehand to make sure
// you understand the macro expansion process
// Read https://gcc.gnu.org/onlinedocs/cpp/Variadic-Macros.html for an
// explanation around variadic function-like macros

#define FIRST_KEYWORD TK_VOID

static char *KEYWORDS[] = {
    "void", "char", "short", "int", "long", "float", "double",
    "signed", "unsigned",
    "struct", "union", "enum", "typedef",
    "auto", "static", "extern", "register", "inline",
    "const", "restrict", "volatile",
    "sizeof", "if", "else", "while", "do", "for", "switch", "case", "default",
    "break", "continue", "goto", "return",
    NULL,
};

static void def_built_ins(PP *pp);

PP * new_pp(Lexer *l) {
    PP *pp = calloc(1, sizeof(PP));
    pp->l = l;
    pp->macros = map_new();
    time_t now = time(NULL);
    localtime_r(&now, &pp->now);
    def_built_ins(pp);
    return pp;
}

Macro * new_macro(int k) {
    Macro *m = calloc(1, sizeof(Macro));
    m->k = k;
    return m;
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
    Macro *m = new_macro(MACRO_OBJ);
    m->body = body;
    return m;
}

static Map * parse_params(PP *pp, int *is_vararg) {
    lex_expect(pp->l, '(');
    Map *params = map_new();
    int nparams = 0;
    Token *t = lex_peek(pp->l);
    *is_vararg = 0;
    while (t->k != ')' && t->k != TK_NEWLINE && t->k != TK_EOF) {
        t = lex_next(pp->l);
        char *name;
        if (t->k == TK_IDENT) {
            name = t->s;
            t->k = TK_MACRO_PARAM;
            t->param = nparams++;
            if (lex_peek(pp->l)->k == TK_ELLIPSIS) {
                lex_next(pp->l);
                t->is_vararg = *is_vararg = 1;
            }
        } else if (t->k == TK_ELLIPSIS) { // Vararg
            name = "__VA_ARGS__";
            t->k = TK_MACRO_PARAM;
            t->param = nparams++;
            t->is_vararg = *is_vararg = 1;
        } else {
            error_at(t, "expected identifier, found %s", token2pretty(t));
        }
        map_put(params, name, t);
        t = lex_next(pp->l); // Skip ','
        if (*is_vararg || t->k != ',') {
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
                t->param = param->param;
            }
        }
        vec_push(body, t);
        t = lex_next(pp->l);
    }
    // TODO: check '##' doesn't appear at start or end of macro body
    return body;
}

static Macro * parse_fn_macro(PP *pp) {
    int is_vararg;
    Map *params = parse_params(pp, &is_vararg);
    Vec *body = parse_body(pp, params);
    Macro *m = new_macro(MACRO_FN);
    m->nparams = map_len(params);
    m->body = body;
    m->is_vararg = is_vararg;
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


// ---- Built-In Macros -------------------------------------------------------

static void macro_date(PP *pp, Token *t) {
    char time[20];
    strftime(time, sizeof(time), "%b %e %Y", &pp->now);
    t->k = TK_STR;
    t->len = strlen(time);
    t->s = malloc(t->len + 1);
    strcpy(t->s, time);
}

static void macro_time(PP *pp, Token *t) {
    char time[20];
    strftime(time, sizeof(time), "%T", &pp->now);
    t->k = TK_STR;
    t->len = strlen(time);
    t->s = malloc(t->len + 1);
    strcpy(t->s, time);
}

static void macro_file(PP *pp, Token *t) {
    t->k = TK_STR;
    t->len = strlen(pp->l->f->path);
    t->s = malloc(t->len + 1);
    strcpy(t->s, pp->l->f->path);
}

static void macro_line(PP *pp, Token *t) {
    (void) pp; // Unused
    t->k = TK_NUM;
    t->len = snprintf(NULL, 0, "%d", t->line);
    t->s = malloc(t->len + 1);
    sprintf(t->s, "%d", t->line);
}

static void macro_one(PP *pp, Token *t) {
    (void) pp; // Unused
    t->k = TK_NUM;
    t->len = 1;
    t->s = "1";
}

static void macro_stdc_version(PP *pp, Token *t) {
    (void) pp; // Unused
    t->k = TK_NUM;
    t->s = "199901L"; // C99 standard
    t->len = strlen(t->s);
}

static void def_built_in(PP *pp, char *name, BuiltIn fn) {
    Macro *m = new_macro(MACRO_BUILT_IN);
    m->build_in = fn;
    map_put(pp->macros, name, m);
}

static void def_built_ins(PP *pp) {
    def_built_in(pp, "__DATE__", macro_date);
    def_built_in(pp, "__TIME__", macro_time);
    def_built_in(pp, "__FILE__", macro_file);
    def_built_in(pp, "__LINE__", macro_line);
    def_built_in(pp, "__STDC__", macro_one);
    def_built_in(pp, "__STDC_VERSION__", macro_stdc_version);
    def_built_in(pp, "__STDC_HOSTED__", macro_one);
}


// ---- Macro Expansion -------------------------------------------------------

static void parse_directive(PP *pp);
static Token * expand_next(PP *pp);

static void copy_pos_info_to_tks(Vec *tks, Token *from) {
    // Copy file, line, column info from [from] to every token in [tks] for
    // error messages from expanded macros; set [has_preceding_space] for the
    // first token in [tks]
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

static Vec * pre_expand_arg(PP *pp, Vec *arg) {
    Lexer *prev = pp->l;
    pp->l = new_lexer(NULL); // Temporary lexer containing the arg tokens
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

static Vec * substitute(PP *pp, Macro *m, Vec *args, Set *hide_set) {
    Vec *tks = vec_new();
    for (size_t i = 0; i < vec_len(m->body); i++) {
        Token *t = copy_tk(vec_get(m->body, i));
        if (t->k == TK_MACRO_PARAM) {
            Vec *arg = vec_get(args, t->param);
            arg = pre_expand_arg(pp, arg);
            copy_pos_info_to_tks(arg, t); // For leading token's preceding space
            vec_push_all(tks, arg);
        } else {
            vec_push(tks, t);
        }
    }
    for (size_t i = 0; i < vec_len(tks); i++) { // Add [hide_set] to all the tks
        Token *t = vec_get(tks, i);
        set_union(&t->hide_set, hide_set);
    }
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
            if (t->k == TK_NEWLINE) continue;
            if (t->k == TK_EOF) break;
            if (t->k == '#' && t->col == 1) {
                parse_directive(pp);
                continue;
            }
            if (t->k == ')' && level == 0) {
                undo_tk(pp->l, t);
                break;
            }
            int in_vararg = (m->is_vararg && vec_len(args) == m->nparams - 1);
            if (t->k == ',' && level == 0 && !in_vararg) {
                break;
            }
            if (t->k == '(') level++;
            if (t->k == ')') level--;
            vec_push(arg, t);
        }
        vec_push(args, arg);
        t = lex_peek(pp->l);
    }
    if (m->is_vararg && vec_len(args) == m->nparams - 1) {
        // Allow not specifying the vararg parameter, e.g.
        // #define x(a, ...) [...] \\ x(3)
        vec_push(args, vec_new());
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
        copy_pos_info_to_tks(tks, t); // For error messages
        undo_tks(pp->l, tks);
        break;
    case MACRO_FN:
        if (lex_peek(pp->l)->k != '(') return t;
        Vec *args = parse_args(pp, m);
        if (vec_len(args) != m->nparams) {
            error_at(t, "incorrect number of arguments provided to function-"
                        "like macro invocation (have %zu, expected %zu)",
                        vec_len(args), m->nparams);
        }
        Token *rparen = lex_expect(pp->l, ')');
        set_intersection(&t->hide_set, rparen->hide_set);
        set_put(&t->hide_set, t->s);
        tks = substitute(pp, m, args, t->hide_set);
        copy_pos_info_to_tks(tks, t); // For error messages
        undo_tks(pp->l, tks);
        break;
    case MACRO_BUILT_IN:
        t = copy_tk(t);
        m->build_in(pp, t);
        undo_tk(pp->l, t);
        break;
    }
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
    error_at(t, "unsupported preprocessor directive '%s'", token2str(t));
}

Token * next_tk(PP *pp) {
    Token *t = expand_next(pp);
    if (t->k == '#' && t->col == 1 && !t->hide_set) { // '#' at line start
        parse_directive(pp);
        return next_tk(pp);
    }
    if (t->k == TK_IDENT) { // Check for keywords
        for (int i = 0; KEYWORDS[i]; i++) {
            if (strcmp(t->s, KEYWORDS[i]) == 0) {
                t->k = FIRST_KEYWORD + i;
                break;
            }
        }
    }
    return t;
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
        error_at(t, "expected %s, found %s", tk2pretty(k), token2pretty(t));
    }
    return t;
}
