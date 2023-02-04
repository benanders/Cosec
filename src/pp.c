
#include <stdlib.h>
#include <string.h>

#include "pp.h"
#include "parse.h"
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

static Token * ZERO_TK = &(Token) { .k = TK_NUM, .num = "0" };
static Token * ONE_TK  = &(Token) { .k = TK_NUM, .num = "1" };

static void def_built_ins(PP *pp);
static void def_default_include_paths(PP *pp);

PP * new_pp(Lexer *l) {
    PP *pp = calloc(1, sizeof(PP));
    pp->l = l;
    pp->macros = map_new();
    pp->conds = vec_new();
    pp->include_once = map_new();
    pp->include_paths = vec_new();
    time_t now = time(NULL);
    localtime_r(&now, &pp->now);
    def_built_ins(pp);
    def_default_include_paths(pp);
    return pp;
}

Macro * new_macro(int k) {
    Macro *m = calloc(1, sizeof(Macro));
    m->k = k;
    return m;
}

static void push_lexer(PP *pp, Lexer *l) {
    l->parent = pp->l;
    pp->l = l;
}

static void pop_lexer(PP *pp) {
    pp->l = pp->l->parent;
    assert(pp->l);
}

static Token * lex_next(PP *pp) {
    Token *t = lex_tk(pp->l);
    if (pp->l->parent && t->k == TK_EOF) {
        pop_lexer(pp);
        return lex_next(pp);
    }
    return t;
}

static Token * lex_peek(PP *pp) {
    Token *t = lex_next(pp);
    undo_tk(pp->l, t);
    return t;
}

static Token * lex_expect(PP *pp, int k) {
    Token *t = lex_next(pp);
    if (t->k != k) {
        error_at(t, "expected %s, found %s", tk2pretty(k), token2pretty(t));
    }
    return t;
}


// ---- Macro Definitions -----------------------------------------------------

static Token * expand_next(PP *pp);

static Macro * parse_obj_macro(PP *pp) {
    Vec *body = vec_new();
    Token *t = lex_next(pp);
    while (t->k != TK_NEWLINE) {
        vec_push(body, t);
        t = lex_next(pp);
    }
    // TODO: check '##' doesn't appear at start or end of macro body
    Macro *m = new_macro(MACRO_OBJ);
    m->body = body;
    return m;
}

static Map * parse_params(PP *pp, int *is_vararg) {
    lex_expect(pp, '(');
    Map *params = map_new();
    int nparams = 0;
    Token *t = lex_peek(pp);
    *is_vararg = 0;
    while (t->k != ')' && t->k != TK_NEWLINE) {
        t = lex_next(pp);
        char *name;
        if (t->k == TK_IDENT) {
            name = t->ident;
            t->k = TK_MACRO_PARAM;
            t->param = nparams++;
            if (lex_peek(pp)->k == TK_ELLIPSIS) {
                lex_next(pp);
                *is_vararg = 1;
            }
        } else if (t->k == TK_ELLIPSIS) { // Vararg
            name = "__VA_ARGS__";
            t->k = TK_MACRO_PARAM;
            t->param = nparams++;
            *is_vararg = 1;
        } else {
            error_at(t, "expected identifier, found %s", token2pretty(t));
        }
        map_put(params, name, t);
        t = lex_next(pp); // Skip ','
        if (*is_vararg || t->k != ',') {
            break;
        }
    }
    undo_tk(pp->l, t);
    lex_expect(pp, ')');
    return params;
}

static Vec * parse_body(PP *pp, Map *params) {
    Vec *body = vec_new();
    Token *t = lex_next(pp);
    while (t->k != TK_NEWLINE) {
        if (t->k == TK_IDENT) {
            Token *param = map_get(params, t->ident);
            if (param) {
                t->k = TK_MACRO_PARAM;
                t->param = param->param;
            }
        }
        vec_push(body, t);
        t = lex_next(pp);
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
    Token *name = lex_expect(pp, TK_IDENT);
    Macro *m;
    Token *t = lex_peek(pp);
    if (t->k == '(' && !t->has_preceding_space) {
        m = parse_fn_macro(pp);
    } else {
        m = parse_obj_macro(pp);
    }
    map_put(pp->macros, name->ident, m);
}

static void parse_undef(PP *pp) {
    Token *name = lex_expect(pp, TK_IDENT);
    lex_expect(pp, TK_NEWLINE);
    map_remove(pp->macros, name->ident);
}


// ---- Includes --------------------------------------------------------------

static char * concat_tks(Vec *tks) {
    Buf *b = buf_new();
    for (size_t i = 0; i < vec_len(tks); i++) {
        Token *t = vec_get(tks, i);
        buf_print(b, token2str(t));
    }
    return b->data;
}

static char * parse_include_path(PP *pp, int *search_local) {
    char *path = lex_include_path(pp->l, search_local);
    if (path) {
        return path;
    }
    Token *t = expand_next(pp); // Otherwise, might be a macro expansion
    if (t->k == TK_STR) {
        *search_local = 1;
        return str_ncopy(t->str, t->len);
    } else if (t->k == '<') {
        *search_local = 0;
        Vec *tks = vec_new();
        while (t->k != '>' && t->k != TK_NEWLINE) {
            vec_push(tks, t);
            t = expand_next(pp);
        }
        if (t->k != '>') {
            error_at(t, "premature end of '#include' path");
        }
        return concat_tks(tks);
    } else {
        error_at(t, "expected string or '<', found %s", token2pretty(t));
    }
}

static int include(PP *pp, char *dir, char *file, int include_once) {
    char *path = full_path(concat_paths(dir, file));
    if (map_get(pp->include_once, path)) return 1;
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return 0;
    }
    File *f = new_file(fp, file);
    Lexer *l = new_lexer(f);
    push_lexer(pp, l);
    if (include_once) {
        map_put(pp->include_once, path, (void *) 1);
    }
    return 1;
}

static void parse_include(PP *pp, Token *t) {
    int is_import = strcmp(t->ident, "import") == 0;
    int search_local;
    char *path = parse_include_path(pp, &search_local);
    lex_expect(pp, TK_NEWLINE);
    if (path[0] == '/') { // Absolute path
        if (include(pp, "/", path, is_import)) return;
    } else { // Relative path
        if (search_local) {
            char *local_dir = pp->l->f->name ? get_dir(pp->l->f->name) : ".";
            if (include(pp, local_dir, path, is_import)) return;
        }
        for (size_t i = 0; i < vec_len(pp->include_paths); i++) {
            char *dir = vec_get(pp->include_paths, i);
            if (include(pp, dir, path, is_import)) return;
        }
    }
    error_at(t, "cannot find file '%s'", path);
}

static void def_default_include_paths(PP *pp) {
    // [ $ cpp -v ] gives the list of GCC's default include paths
    vec_push(pp->include_paths, "/usr/local/include");
    vec_push(pp->include_paths, "/Library/Developer/CommandLineTools/usr/include");
    vec_push(pp->include_paths, "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include");
    vec_push(pp->include_paths, "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/System/Library/Frameworks");
}


// ---- Conditionals ----------------------------------------------------------

static int is_skip_end(Token *t) {
    return strcmp(t->ident, "elif") == 0 || strcmp(t->ident, "else") == 0 ||
           strcmp(t->ident, "endif") == 0;
}

static int is_level_start(Token *t) {
    return strcmp(t->ident, "if") == 0 || strcmp(t->ident, "ifdef") == 0 ||
           strcmp(t->ident, "ifndef") == 0;
}

static int is_level_end(Token *t) {
    return strcmp(t->ident, "endif") == 0;
}

static void skip_cond_incl(PP *pp) {
    int level = 0;
    Token *t = lex_peek(pp);
    while (t->k != TK_EOF) {
        Token *hash = t = lex_next(pp);
        if (!(t->k == '#' && t->col == 1)) continue; // Not a directive
        t = lex_next(pp);
        if (t->k != TK_IDENT) continue;
        if (level == 0 && is_skip_end(t)) {
            undo_tk(pp->l, t);
            undo_tk(pp->l, hash);
            break;
        }
        if (is_level_start(t)) level++;
        if (is_level_end(t) && level > 0) level--;
    }
}

static Token * parse_defined(PP *pp) {
    Token *t = lex_next(pp);
    if (t->k == '(') {
        t = lex_next(pp);
        lex_expect(pp, ')');
    }
    if (t->k != TK_IDENT) {
        error_at(t, "expected identifier, found %s", token2pretty(t));
    }
    return map_get(pp->macros, t->ident) ? ONE_TK : ZERO_TK;
}

static Vec * parse_cond_line(PP *pp) {
    Vec *tks = vec_new();
    Token *t = expand_next(pp);
    while (t->k != TK_NEWLINE) {
        if (t->k == TK_IDENT && strcmp(t->ident, "defined") == 0) {
            t = parse_defined(pp);
        } else if (t->k == TK_IDENT) {
            t = ZERO_TK; // All other idents get replaced with '0'
        }
        vec_push(tks, t);
        t = expand_next(pp);
    }
    return tks;
}

static int parse_cond(PP *pp) {
    // Create a temporary lexer for the constant condition; don't use
    // 'push_lexer' because we want the 'TK_EOF' when we're done
    Vec *tks = parse_cond_line(pp);
    Lexer *prev = pp->l;
    pp->l = new_lexer(NULL);
    undo_tks(pp->l, tks);
    int64_t v = parse_const_int_expr(pp);
    pp->l = prev;
    return v != 0;
}

static void start_if(PP *pp, int is_true) {
    Cond *cond = malloc(sizeof(Cond));
    cond->k = COND_IF;
    cond->was_true = is_true;
    vec_push(pp->conds, cond);
    if (!is_true) {
        skip_cond_incl(pp);
    }
}

static void parse_if(PP *pp) {
    int is_true = parse_cond(pp);
    start_if(pp, is_true);
}

static void parse_ifdef(PP *pp) {
    Token *t = lex_expect(pp, TK_IDENT);
    lex_expect(pp, TK_NEWLINE);
    int is_true = map_get(pp->macros, t->ident) != NULL;
    start_if(pp, is_true);
}

static void parse_ifndef(PP *pp) {
    Token *t = lex_expect(pp, TK_IDENT);
    lex_expect(pp, TK_NEWLINE);
    int is_true = map_get(pp->macros, t->ident) == NULL;
    start_if(pp, is_true);
}

static void parse_elif(PP *pp, Token *t) {
    if (vec_len(pp->conds) == 0) {
        error_at(t, "'#elif' directive without preceding '#if'");
    }
    Cond *cond = vec_last(pp->conds);
    if (cond->k == COND_ELSE) {
        error_at(t, "'#elif' directive after '#else'");
    }
    cond->k = COND_ELIF;
    int is_true = parse_cond(pp);
    if (!is_true || cond->was_true) {
        skip_cond_incl(pp);
    }
    cond->was_true |= is_true;
}

static void parse_else(PP *pp, Token *t) {
    if (vec_len(pp->conds) == 0) {
        error_at(t, "'#else' directive without preceding '#if'");
    }
    Cond *cond = vec_last(pp->conds);
    cond->k = COND_ELSE;
    lex_expect(pp, TK_NEWLINE);
    if (cond->was_true) {
        skip_cond_incl(pp);
    }
}

static void parse_endif(PP *pp, Token *t) {
    if (vec_len(pp->conds) == 0) {
        error_at(t, "'#endif' directive without preceding '#if'");
    }
    vec_pop(pp->conds);
    lex_expect(pp, TK_NEWLINE);
}


// ---- Other Directives ------------------------------------------------------

static void parse_line(PP *pp) {
    Token *t = expand_next(pp);
    if (t->k != TK_NUM) {
        error_at(t, "expected number after '#line', found %s", token2pretty(t));
    }
    char *end;
    long line = strtol(t->num, &end, 10);
    if (*end != '\0' || line < 0) {
        error_at(t, "invalid line number '%s' after '#line'", token2str(t));
    }
    t = expand_next(pp);
    char *file = NULL;
    if (t->k == TK_STR) {
        file = str_ncopy(t->str, t->len); // TK_STR is not NULL terminated
        t = lex_next(pp);
    }
    if (t->k != TK_NEWLINE) {
        error_at(t, "expected newline, found %s", token2pretty(t));
    }
    pp->l->f->line = (int) line;
    if (file) {
        pp->l->f->name = file;
    }
}

static void parse_warning(PP *pp, Token *t) {
    char *msg = lex_rest_of_line(pp->l);
    warning_at(t, msg);
}

static void parse_error(PP *pp, Token *t) {
    char *msg = lex_rest_of_line(pp->l);
    error_at(t, msg);
}

static void parse_pragma_once(PP *pp) {
    char *path = full_path(pp->l->f->name);
    map_put(pp->include_once, path, (void *) 1);
    lex_expect(pp, TK_NEWLINE);
}

static void parse_pragma(PP *pp) {
    Token *t = lex_expect(pp, TK_IDENT);
    if (strcmp(t->ident, "once") == 0) {
        parse_pragma_once(pp);
    } else {
        error_at(t, "unsupported pragma directive '%s'", token2str(t));
    }
}


// ---- Built-In Macros -------------------------------------------------------

static void macro_date(PP *pp, Token *t) {
    char time[20];
    strftime(time, sizeof(time), "%b %e %Y", &pp->now);
    t->k = TK_STR;
    t->len = strlen(time);
    t->str = str_ncopy(time, t->len);
}

static void macro_time(PP *pp, Token *t) {
    char time[20];
    strftime(time, sizeof(time), "%T", &pp->now);
    t->k = TK_STR;
    t->len = strlen(time);
    t->str = str_ncopy(time, t->len);
}

static void macro_file(PP *pp, Token *t) {
    t->k = TK_STR;
    t->len = strlen(pp->l->f->name);
    t->str = str_ncopy(pp->l->f->name, t->len);
}

static void macro_line(PP *pp, Token *t) {
    (void) pp; // Unused
    t->k = TK_NUM;
    size_t len = snprintf(NULL, 0, "%d", t->line);
    t->num = malloc(sizeof(char) * (len + 1));
    sprintf(t->num, "%d", t->line);
}

static void macro_one(PP *pp, Token *t) {
    (void) pp; // Unused
    t->k = TK_NUM;
    t->num = "1";
}

static void macro_stdc_version(PP *pp, Token *t) {
    (void) pp; // Unused
    t->k = TK_NUM;
    t->num = "199901L"; // C99 standard
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
static Token * expand_next_ignore_newlines(PP *pp);

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

static Token * stringize(Vec *tks, Token *hash) {
    Buf *b = buf_new();
    for (size_t i = 0; i < vec_len(tks); i++) {
        Token *t = vec_get(tks, i);
        if (b->len > 0 && t->has_preceding_space) {
            buf_push(b, ' ');
        }
        buf_print(b, token2str(t));
    }
    Token *str = copy_tk(hash);
    str->k = TK_STR;
    str->str = b->data;
    str->len = b->len;
    return str;
}

static void glue(PP *pp, Vec *tks, Token *t) {
    Token *last = vec_pop(tks);
    Token *glued = glue_tks(pp->l, last, t);
    vec_push(tks, glued);
}

static Vec * pre_expand_arg(PP *pp, Vec *arg) {
    // Create a temporary lexer for the arg; don't use 'push_lexer' because we
    // want the 'TK_EOF' when we're finished pre-expansion
    Lexer *prev = pp->l;
    pp->l = new_lexer(NULL);
    undo_tks(pp->l, arg);
    Vec *expanded = vec_new();
    while (1) {
        Token *t = expand_next_ignore_newlines(pp);
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
        Token *u = i < vec_len(m->body) - 1 ? vec_get(m->body, i + 1) : NULL;
        if (t->k == '#' && u && u->k == TK_MACRO_PARAM) {
            Vec *arg = vec_get(args, u->param);
            Token *str = stringize(arg, t);
            vec_push(tks, str);
            i++; // Skip 'u'
        } else if (t->k == TK_CONCAT && u && u->k == TK_MACRO_PARAM) {
            // <anything> ## <macro param>
            Vec *arg = vec_get(args, u->param);
            if (vec_len(arg) > 0) { // TODO: if 'arg' is the vararg...
                Token *first = vec_remove(arg, 0);
                glue(pp, tks, first);
                vec_push_all(tks, arg); // Don't pre-expand
            }
            i++; // Skip 'u'
        } else if (t->k == TK_CONCAT && u) {
            // <anything> ## <token>
            hide_set = u->hide_set;
            glue(pp, tks, u);
            i++; // Skip 'u'
        } else if (t->k == TK_MACRO_PARAM && u && u->k == TK_CONCAT) {
            // <macro param> ## <anything>
            Vec *arg = vec_get(args, t->param);
            if (vec_len(arg) == 0) {
                i++; // Skip '##' if nothing to glue to
            } else {
                vec_push_all(tks, arg); // Don't pre-expand
            }
        } else if (t->k == TK_MACRO_PARAM) {
            Vec *arg = vec_get(args, t->param);
            arg = pre_expand_arg(pp, arg);
            copy_pos_info_to_tks(arg, t); // For leading token's preceding space
            vec_push_all(tks, arg);
        } else {
            vec_push(tks, t);
        }
    }
    for (size_t i = 0; i < vec_len(tks); i++) { // Add 'hide_set' to all the tks
        Token *t = vec_get(tks, i);
        set_union(&t->hide_set, hide_set);
    }
    return tks;
}

static Vec * parse_args(PP *pp, Macro *m) {
    lex_expect(pp, '(');
    Vec *args = vec_new();
    Token *t = lex_peek(pp);
    if (m->nparams == 1 && t->k == ')') {
        vec_push(args, vec_new());
        return args; // Empty single argument
    }
    while (t->k != ')' && t->k != TK_EOF) {
        Vec *arg = vec_new();
        int level = 0;
        while (1) {
            t = lex_next(pp);
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
    }
    if (m->is_vararg && vec_len(args) == m->nparams - 1) {
        // Allow not specifying the vararg parameter, e.g.
        // #define x(a, ...) [...] \\ x(3)
        vec_push(args, vec_new());
    }
    return args;
}

static Token * expand_next(PP *pp) {
    Token *t = lex_next(pp);
    if (t->k != TK_IDENT) {
        return t;
    }
    Macro *m = map_get(pp->macros, t->ident);
    if (!m || set_has(t->hide_set, t->ident)) {
        return t; // No macro, or macro self-reference
    }
    Vec *tks;
    switch (m->k) {
    case MACRO_OBJ:
        set_put(&t->hide_set, t->ident);
        tks = substitute(pp, m, NULL, t->hide_set);
        copy_pos_info_to_tks(tks, t); // For error messages
        undo_tks(pp->l, tks);
        break;
    case MACRO_FN:
        if (lex_peek(pp)->k != '(') return t;
        Vec *args = parse_args(pp, m);
        if (vec_len(args) != m->nparams) {
            error_at(t, "incorrect number of arguments provided to function-"
                        "like macro invocation (have %zu, expected %zu)",
                        vec_len(args), m->nparams);
        }
        Token *rparen = lex_expect(pp, ')');
        set_intersection(&t->hide_set, rparen->hide_set);
        set_put(&t->hide_set, t->ident);
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
    return expand_next_ignore_newlines(pp);
}

static Token * expand_next_ignore_newlines(PP *pp) {
    Token *t = expand_next(pp);
    while (t->k == TK_NEWLINE) { // Ignore newlines
        t = lex_next(pp);
        t->has_preceding_space = 1;
    }
    return t;
}


// ---- Tokens and Directives -------------------------------------------------

static void parse_directive(PP *pp) {
    Token *t = lex_next(pp);
    if (t->k == TK_NEWLINE) return; // Empty directive
    if (t->k != TK_IDENT) goto err;
    if (strcmp(t->ident, "define") == 0)         parse_define(pp);
    else if (strcmp(t->ident, "undef") == 0)     parse_undef(pp);
    else if (strcmp(t->ident, "include") == 0 ||
             strcmp(t->ident, "import") == 0)    parse_include(pp, t);
    else if (strcmp(t->ident, "if") == 0)        parse_if(pp);
    else if (strcmp(t->ident, "ifdef") == 0)     parse_ifdef(pp);
    else if (strcmp(t->ident, "ifndef") == 0)    parse_ifndef(pp);
    else if (strcmp(t->ident, "elif") == 0)      parse_elif(pp, t);
    else if (strcmp(t->ident, "else") == 0)      parse_else(pp, t);
    else if (strcmp(t->ident, "endif") == 0)     parse_endif(pp, t);
    else if (strcmp(t->ident, "line") == 0)      parse_line(pp);
    else if (strcmp(t->ident, "warning") == 0)   parse_warning(pp, t);
    else if (strcmp(t->ident, "error") == 0)     parse_error(pp, t);
    else if (strcmp(t->ident, "pragma") == 0)    parse_pragma(pp);
    else goto err;
    return;
err:
    error_at(t, "unsupported preprocessor directive '%s'", token2str(t));
}

Token * next_tk(PP *pp) {
    Token *t = expand_next_ignore_newlines(pp);
    if (t->k == '#' && t->col == 1 && !t->hide_set) { // '#' at line start
        parse_directive(pp);
        return next_tk(pp);
    }
    if (t->k == TK_IDENT) { // Check for keywords
        for (int i = 0; KEYWORDS[i]; i++) {
            if (strcmp(t->ident, KEYWORDS[i]) == 0) {
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
