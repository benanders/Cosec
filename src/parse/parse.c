
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "parse.h"
#include "../err.h"

enum {
    SCOPE_FILE,
    SCOPE_BLOCK,
    SCOPE_LOOP,
    SCOPE_SWITCH,
};

typedef struct Scope {
    struct Scope *outer;
    int k;
    Lexer *l;
    Map *vars;
    Node *fn; // for SCOPE_BLOCK
} Scope;

static Node * node(int k, Token *tk) {
    Node *n = calloc(1, sizeof(Node));
    n->k = k;
    n->tk = tk;
    return n;
}

static void enter_scope(Scope *inner, Scope *outer, int k) {
    *inner = (Scope) {0};
    inner->k = k;
    inner->l = outer->l;
    inner->vars = map_new();
    inner->fn = outer->fn;
    inner->outer = outer;
}

static Node * find_var(Scope *s, char *name) {
    while (s) {
        Node *var = map_get(s->vars, name);
        if (var) {
            return var;
        }
        s = s->outer;
    }
    return NULL;
}

static void ensure_not_redef(Scope *s, Node *n) {
    Type *a = n->t;
    Node *v;
    if (n->t->linkage == L_EXTERN) { // extern needs type checking across scopes
        v = find_var(s, n->var_name);
        if (v && !are_equal(n->t, v->t)) goto err_types;
    }
    v = map_get(s->vars, n->var_name);
    if (!v) return;
    Type *b = v->t;
    if (n->k != v->k) goto err_symbols;
    if (!are_equal(a, b)) goto err_types;
    if (s->k == SCOPE_FILE) { // File scope
        // ALLOWED: [int a; extern int a;]
        // ALLOWED: [static int a; extern int a;]
        // ALLOWED: [extern int a; int a;]
        if (a->linkage == L_STATIC && b->linkage == L_NONE)   goto err_static1;
        if (a->linkage == L_NONE   && b->linkage == L_STATIC) goto err_static2;
        if (a->linkage == L_EXTERN && b->linkage == L_STATIC) goto err_static2;
    } else { // Block scope
        // ALLOWED: [extern int a; extern int a]
        if (!(n->t->linkage == L_EXTERN && v->t->linkage == L_EXTERN)) goto err_redef;
    }
err_redef:
    error_at(n->tk, "redefinition of '%s'", n->var_name);
err_symbols:
    error_at(n->tk, "redefinition of '%s' as a different kind of symbol", n->var_name);
err_types:
    error_at(n->tk, "redefinition of '%s' with a different type", n->var_name);
err_static1:
    error_at(n->tk, "non-static declaration of '%s' follows static declaration", n->var_name);
err_static2:
    error_at(n->tk, "static declaration of '%s' follows non-static declaration", n->var_name);
}

static void adjust_linkage(Scope *s, Token *name, Type *t) {
    if (t->k == T_FN && t->linkage == L_NONE) {
        t->linkage = L_EXTERN; // Functions are 'extern' if unspecified
    }
    if (t->k == T_FN && s->k != SCOPE_FILE && t->linkage == L_STATIC) {
        error_at(name, "function declared in block scope cannot have 'static' storage class");
    }
}

static void def_symbol(Scope *s, Node *n) {
    ensure_not_redef(s, n);
    map_put(s->vars, n->var_name, n);
}

static Node * def_var(Scope *s, Token *name, Type *t) {
    if (t->k == T_VOID) {
        error_at(name, "variable cannot have type 'void'");
    }
    adjust_linkage(s, name, t);
    Node *n = node(N_VAR, name);
    n->t = t;
    n->var_name = name->s;
    def_symbol(s, n);
    return n;
}

static Node * def_typedef(Scope *s, Token *name, Type *t) {
    assert(t->linkage == L_NONE);
    Node *n = node(N_TYPEDEF, name);
    n->t = t;
    n->var_name = name->s;
    def_symbol(s, n);
    return n;
}


// ---- Literals --------------------------------------------------------------

static Type * parse_int_suffix(char *s) {
    if (strcasecmp(s, "u") == 0) {
        return t_num(T_INT, 1);
    } else if (strcasecmp(s, "l") == 0) {
        return t_num(T_LONG, 0);
    } else if (strcasecmp(s, "ul") == 0 || strcasecmp(s, "lu") == 0) {
        return t_num(T_LONG, 1);
    } else if (strcasecmp(s, "ll") == 0) {
        return t_num(T_LLONG, 0);
    } else if (strcasecmp(s, "ull") == 0 || strcasecmp(s, "llu") == 0) {
        return t_num(T_LLONG, 1);
    } else {
        return NULL;
    }
}

static Type * smallest_type_for_int(uint64_t num, int is_base_10) {
    if (is_base_10) { // Decimal constant is either int, long, or long long
        if (num <= INT_MAX)       return t_num(T_INT, 0);
        else if (num <= LONG_MAX) return t_num(T_LONG, 0);
        else                      return t_num(T_LLONG, 0);
    } else { // Octal/hex constants may be unsigned
        if (num <= INT_MAX)        return t_num(T_INT, 0);
        else if (num <= UINT_MAX)  return t_num(T_INT, 1);
        else if (num <= LONG_MAX)  return t_num(T_LONG, 0);
        else if (num <= ULONG_MAX) return t_num(T_LONG, 1);
        else if (num <= LLONG_MAX) return t_num(T_LLONG, 0);
        else                       return t_num(T_LLONG, 1);
    }
}

static Node * parse_int(Token *tk) {
    char *suffix;
    uint64_t num;
    if (strncasecmp(tk->s, "0b", 2) == 0) { // strotoul can't read '0b' prefix
        num = strtoul(tk->s + 2, &suffix, 2);
    } else { // strtoul can read '0x' and '0' prefix
        num = strtoul(tk->s, &suffix, 0);
    }
    Type *t;
    if (*suffix == '\0') { // No suffix; select type based on how large 'num' is
        int is_base_10 = (*tk->s != '0');
        t = smallest_type_for_int(num, is_base_10);
    } else { // Type specified by suffix
        t = parse_int_suffix(suffix);
        if (!t) {
            error_at(tk, "invalid integer suffix '%s'", suffix);
        }
        uint64_t invalid_bits = ~((1 << t->size) - 1);
        if ((num & invalid_bits) != 0) {
            warning_at(tk, "integer '%s' too large for specified type", tk->s);
        }
    }
    Node *n = node(N_IMM, tk);
    n->t = t;
    n->imm = num;
    return n;
}

static Type * parse_float_suffix(char *s) {
    if (strcasecmp(s, "l") == 0) {
        return t_num(T_LDOUBLE, 0);
    } else if (strcasecmp(s, "f") == 0) {
        return t_num(T_FLOAT, 0);
    } else {
        return NULL;
    }
}

static Node * parse_float(Token *tk) {
    char *suffix;
    double num = strtod(tk->s, &suffix);
    Type *t;
    if (*suffix == '\0') { // No suffix; always a double
        t = t_num(T_DOUBLE, 0);
    } else { // Type specified by suffix
        t = parse_float_suffix(suffix);
        if (!t) {
            error_at(tk, "invalid floating point suffix '%s'", suffix);
        }
    }
    Node *n = node(N_FP, tk);
    n->t = t;
    n->fp = num;
    return n;
}

static Node * parse_num(Token *tk) {
    if (strpbrk(tk->s, ".pP") ||
            (strncasecmp(tk->s, "0x", 2) != 0 && strpbrk(tk->s, "eE"))) {
        return parse_float(tk);
    } else {
        return parse_int(tk);
    }
}


// ---- Declarators -----------------------------------------------------------

static Type * parse_declarator_tail(Scope *s, Type *base, Vec *param_names);
static Type * parse_declarator(Scope *s, Type *base, Token **name, Vec *param_names);

static Type * find_typedef(Scope *s, char *name) {
    Node *n = find_var(s, name);
    if (n && n->k == N_TYPEDEF) {
        return n->t;
    }
    return NULL;
}

static int is_type(Scope *s, Token *t) {
    if (t->k == TK_IDENT) {
        return find_typedef(s, t->s) != NULL;
    } else {
        return t->k >= TK_VOID && t->k <= TK_VOLATILE;
    }
}

static Type * parse_enum_def(Scope *s) {
    TODO();
}

static Type * parse_union_def(Scope *s) {
    TODO();
}

static Type * parse_struct_def(Scope *s) {
    TODO();
}

static Type * parse_decl_specs(Scope *s, int *sclass) {
    if (!is_type(s, peek_tk(s->l))) {
        error_at(peek_tk(s->l), "expected type name");
    }

    int sc = 0, tq = 0, fs = 0;
    enum { tnone, tvoid, tchar, tint, tfloat, tdouble } kind = 0;
    enum { tlong = 1, tllong, tshort } size = 0;
    enum { tsigned = 1, tunsigned } sign = 0;
    Type *t = NULL;
    Token *tk;
    while (1) {
        tk = next_tk(s->l);
        switch (tk->k) {
        case TK_TYPEDEF:  if (sc) { goto sc_err; } sc = S_TYPEDEF; break;
        case TK_AUTO:     if (sc) { goto sc_err; } sc = S_AUTO; break;
        case TK_STATIC:   if (sc) { goto sc_err; } sc = S_STATIC; break;
        case TK_EXTERN:   if (sc) { goto sc_err; } sc = S_EXTERN; break;
        case TK_REGISTER: if (sc) { goto sc_err; } sc = S_REGISTER; break;
        case TK_INLINE:   if (fs) { goto fs_err; } fs = F_INLINE; break;
        case TK_CONST:    tq |= T_CONST; break;
        case TK_RESTRICT: tq |= T_RESTRICT; break;
        case TK_VOLATILE: tq |= T_VOLATILE; break;
        case TK_VOID:     if (kind) { goto t_err; } kind = tvoid; break;
        case TK_CHAR:     if (kind) { goto t_err; } kind = tchar; break;
        case TK_INT:      if (kind) { goto t_err; } kind = tint; break;
        case TK_FLOAT:    if (kind) { goto t_err; } kind = tfloat; break;
        case TK_DOUBLE:   if (kind) { goto t_err; } kind = tdouble; break;
        case TK_SHORT:    if (size) { goto t_err; } size = tshort; break;
        case TK_LONG:     if (size > tlong) { goto t_err; } size++; break;
        case TK_SIGNED:   if (sign) { goto t_err; } sign = tsigned; break;
        case TK_UNSIGNED: if (sign) { goto t_err; } sign = tunsigned; break;
        case TK_STRUCT:   if (t) { goto t_err; } t = parse_struct_def(s); break;
        case TK_UNION:    if (t) { goto t_err; } t = parse_union_def(s); break;
        case TK_ENUM:     if (t) { goto t_err; } t = parse_enum_def(s); break;
        case TK_IDENT:
            if (!find_typedef(s, tk->s)) goto done;
            if (t) goto t_err;
            t = find_typedef(s, tk->s);
            break;
        default: goto done;
        }
        if (size == tshort && !(kind == tnone || kind == tint)) goto t_err;
        if (size == tlong && !(kind == tnone || kind == tint || kind == tdouble)) goto t_err;
        if (sign && !(kind == tnone || kind == tchar || kind == tint)) goto t_err;
        if (t && (kind || size || sign)) goto t_err;
    }
done:
    (void) tq; // unused
    undo_tk(s->l, tk);
    if (sclass) *sclass = sc;
    if (t) {
        return t;
    }
    switch (kind) {
        case tvoid:   return t_num(T_VOID, 0);
        case tchar:   return t_num(T_CHAR, sign == tunsigned);
        case tfloat:  return t_num(T_FLOAT, 0);
        case tdouble: return t_num(size == tlong ? T_LDOUBLE : T_DOUBLE, 0);
        default: break;
    }
    switch (size) {
        case tshort: return t_num(T_SHORT, sign == tunsigned);
        case tlong:  return t_num(T_LONG, sign == tunsigned);
        case tllong: return t_num(T_LLONG, sign == tunsigned);
        default:     return t_num(T_INT, sign == tunsigned);
    }
sc_err:
    error_at(tk, "can't have more than one storage class specifier");
fs_err:
    error_at(tk, "can't have more than one function specifier");
t_err:
    error_at(tk, "invalid combination of type specifiers");
}

static Type * parse_array_declarator(Scope *s, Type *base) {
    expect_tk(s->l, '[');
    uint64_t len;
    if (next_tk_is(s->l, ']')) {
        len = -1;
    } else {
        Token *num = expect_tk(s->l, TK_NUM);
        len = parse_int(num)->imm;
        expect_tk(s->l, ']');
    }
    Token *err = peek_tk(s->l);
    Type *t = parse_declarator_tail(s, base, NULL);
    if (t->k == T_ARR) {
        error_at(err, "cannot have an array of functions");
    }
    return t_arr(t, len);
}

static Type * parse_fn_declarator_param(Scope *s, Token **name) {
    Token *err = peek_tk(s->l);
    Type *base = t_num(T_INT, 0); // Parameter types default to 'int'
    if (is_type(s, peek_tk(s->l))) {
        base = parse_decl_specs(s, NULL);
    }
    Type *t = parse_declarator(s, base, name, NULL);
    if (t->k == T_ARR) { // Array of T is adjusted to pointer to T
        t = t_ptr(t->arr);
    } else if (t->k == T_FN) { // Function is adjusted to pointer to function
        t = t_ptr(t);
    }
    if (t->k == T_VOID) {
        error_at(err, "parameter cannot have 'void' type");
    }
    return t;
}

static Type * parse_fn_declarator(Scope *s, Type *ret, Vec *param_names) {
    if (ret->k == T_FN) {
        error_at(peek_tk(s->l), "function cannot return a function");
    } else if (ret->k == T_ARR) {
        error_at(peek_tk(s->l), "function cannot return an array");
    }
    expect_tk(s->l, '(');
    if (peek_tk(s->l)->k == TK_VOID && peek2_tk(s->l)->k == ')') {
        next_tk(s->l); next_tk(s->l); // 'void' ')'
        return t_fn(ret, vec_new());
    }
    Vec *param_types = vec_new();
    while (peek_tk(s->l)->k != ')' && peek_tk(s->l)->k != TK_EOF) {
        Token *name;
        Type *param = parse_fn_declarator_param(s, &name);
        vec_push(param_types, param);
        if (param_names) {
            vec_push(param_names, name); // Name may be NULL
        }
        if (!next_tk_is(s->l, ',')) {
            break;
        }
    }
    expect_tk(s->l, ')');
    return t_fn(ret, param_types);
}

static Type * parse_declarator_tail(Scope *s, Type *base, Vec *param_names) {
    if (peek_tk_is(s->l, '[')) {
        return parse_array_declarator(s, base);
    } else if (peek_tk_is(s->l, '(')) {
        return parse_fn_declarator(s, base, param_names);
    } else {
        return base;
    }
}

static void skip_type_quals(Scope *s) {
    while (next_tk_is(s->l, TK_CONST) ||
           next_tk_is(s->l, TK_RESTRICT) ||
           next_tk_is(s->l, TK_VOLATILE));
}

static Type * parse_declarator(Scope *s, Type *base, Token **name, Vec *param_names) {
    if (next_tk_is(s->l, '*')) {
        skip_type_quals(s);
        return parse_declarator(s, t_ptr(base), name, param_names);
    }
    if (next_tk_is(s->l, '(')) { // Either sub-declarator or fn parameters
        if (is_type(s, peek_tk(s->l)) || peek_tk_is(s->l, ')')) { // Function
            // An empty '()' is a function pointer, not a no-op sub-declarator
            return parse_fn_declarator(s, base, param_names);
        } else { // Sub-declarator
            Type *inner = t_new();
            Type *decl = parse_declarator(s, inner, name, param_names);
            expect_tk(s->l, ')');
            *inner = *parse_declarator_tail(s, base, param_names);
            return decl;
        }
    }
    Token *t = peek_tk(s->l);
    if (t->k == TK_IDENT) {
        *name = t;
        next_tk(s->l);
    }
    return parse_declarator_tail(s, base, param_names);
}

static Type * parse_named_declarator(Scope *s, Type *base, Token **name, Vec *param_names) {
    Token *name_copy = NULL;
    Token *err = peek_tk(s->l);
    Type *t = parse_declarator(s, base, &name_copy, param_names);
    if (!name_copy) {
        error_at(err, "expected named declarator");
    }
    if (name) {
        *name = name_copy;
    }
    return t;
}

static Type * parse_abstract_declarator(Scope *s, Type *base) {
    Token *name = NULL;
    Type *t = parse_declarator(s, base, &name, NULL);
    if (name) {
        error_at(name, "expected abstract declarator");
    }
    return t;
}


// ---- Expressions -----------------------------------------------------------

static Node * conv_to(Node *n, Type *t) {
    TODO();
}

static Node * parse_expr_no_commas(Scope *s) {
    TODO();
}

static Node * parse_const_expr(Scope *s) {
    TODO();
}


// ---- Statements ------------------------------------------------------------

static Node * parse_block(Scope *s) {
    TODO();
}


// ---- Declarations ----------------------------------------------------------

static Node * parse_fn_def(Scope *s, Type *t, Token *name, Vec *param_names) {
    assert(s->k == SCOPE_FILE);
    if (t->k != T_FN) {
        error_at(name, "expected function type");
    }
    def_var(s, name, t);
    Node *fn = node(N_FN_DEF, name);
    fn->t = t;
    fn->fn_name = name->s;
    fn->param_names = param_names;
    Scope fn_scope;
    enter_scope(&fn_scope, s, SCOPE_BLOCK);
    fn->fn_body = parse_block(&fn_scope);
    return fn;
}

static Node * parse_initializer_list(Scope *s) {
    TODO();
}

static Node * parse_decl_var(Scope *s, Type *t, Token *name) {
    Node *var = def_var(s, name, t);
    Node *init = NULL;
    if (next_tk_is(s->l, '=')) {
        if (t->linkage == L_EXTERN || t->k == T_FN) {
            error_at(peek_tk(s->l), "illegal initializer");
        }
        if (t->linkage == L_STATIC || s->k == SCOPE_FILE) {
            init = parse_const_expr(s);
        } else { // Block scope, no linkage, object
            if (peek_tk_is(s->l, '{') || peek_tk_is(s->l, TK_STR)) {
                init = parse_initializer_list(s);
            } else {
                init = parse_expr_no_commas(s);
            }
            init = conv_to(init, t);
        }
    }
    Node *decl = node(N_DECL, name);
    decl->var = var;
    decl->init = init;
    return decl;
}

static Node * parse_init_decl(Scope *s, Type *base, int sclass) {
    Token *name = NULL;
    Vec *param_names = vec_new();
    Type *t = parse_named_declarator(s, base, &name, param_names);
    switch (sclass) {
    case S_TYPEDEF: return def_typedef(s, name, t);
    case S_EXTERN:  t->linkage = L_EXTERN; break;
    case S_STATIC:  t->linkage = L_STATIC; break;
    case S_AUTO: case S_REGISTER:
        if (s->k == SCOPE_FILE) {
            error_at(name, "illegal storage class specifier in file scope");
        }
        break;
    }
    if (s->k == SCOPE_FILE && peek_tk_is(s->l, '{')) {
        return parse_fn_def(s, t, name, param_names);
    }
    return parse_decl_var(s, t, name);
}

static Node * parse_decl(Scope *s) {
    int sclass;
    Type *base = parse_decl_specs(s, &sclass);
    if (next_tk_is(s->l, ';')) {
        return NULL;
    }
    Node *head = NULL;
    Node **cur = &head;
    while (1) {
        *cur = parse_init_decl(s, t_copy(base), sclass);
        while (*cur) cur = &(*cur)->next;
        if (!next_tk_is(s->l, ',')) {
            break;
        }
    }
    expect_tk(s->l, ';');
    return head;
}


// ---- Top Level -------------------------------------------------------------

Node * parse(char *path) {
    File *f = new_file(path);
    Lexer *l = new_lexer(f);
    Scope top_level = {0};
    top_level.k = SCOPE_FILE;
    top_level.l = l;
    top_level.vars = map_new();
    Node *head = NULL;
    Node **cur = &head;
    while (!next_tk_is(l, TK_EOF)) {
        *cur = parse_decl(&top_level);
        while (*cur) cur = &(*cur)->next;
    }
    return head;
}
