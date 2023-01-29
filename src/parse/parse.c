
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
    Map *vars;  // of 'Node *' with k = N_LOCAL, N_GLOBAL, or N_TYPEDEF
    Map *tags;  // of 'Type *'
    Node *fn;   // NULL in file scope
    Vec *cases; // for SCOPE_SWITCH
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
    inner->tags = map_new();
    inner->fn = outer->fn;
    inner->outer = outer;
    if (k == SCOPE_SWITCH) {
        inner->cases = vec_new();
    }
}

static Scope * find_scope(Scope *s, int k) {
    while (s && s->k != k) s = s->outer;
    return s;
}

static Node * find_var(Scope *s, char *name) {
    while (s) {
        Node *var = map_get(s->vars, name);
        if (var) return var;
        s = s->outer;
    }
    return NULL;
}

static Type * find_typedef(Scope *s, char *name) {
    Node *n = find_var(s, name);
    if (n && n->k == N_TYPEDEF) {
        return n->t;
    }
    return NULL;
}

static Type * find_tag(Scope *s, char *tag) {
    while (s) {
        Type *t = map_get(s->tags, tag);
        if (t) return t;
        s = s->outer;
    }
    return NULL;
}

static void ensure_not_redef(Scope *s, Node *n) {
    Node *v;
    if (n->t->linkage == L_EXTERN) { // extern needs type checking across scopes
        v = find_var(s, n->var_name);
        if (v && !are_equal(n->t, v->t)) goto err_types;
    }
    v = map_get(s->vars, n->var_name);
    if (!v) return;
    if (n->k != v->k) goto err_symbols;
    if (!are_equal(n->t, v->t)) goto err_types;
    if (s->k == SCOPE_FILE) { // File scope
        // ALLOWED: [int a; extern int a;]
        // ALLOWED: [static int a; extern int a;]
        // ALLOWED: [extern int a; int a;]
        if (n->t->linkage == L_STATIC && v->t->linkage == L_NONE)   goto err_static1;
        if (n->t->linkage == L_NONE   && v->t->linkage == L_STATIC) goto err_static2;
        if (n->t->linkage == L_EXTERN && v->t->linkage == L_STATIC) goto err_static2;
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

static void def_symbol(Scope *s, Node *n) {
    ensure_not_redef(s, n);
    map_put(s->vars, n->var_name, n);
}

static Node * def_var(Scope *s, Token *name, Type *t) {
    if (is_incomplete(t)) {
        error_at(name, "variable cannot have incomplete type");
    }
    if (t->k == T_FN && s->k != SCOPE_FILE && t->linkage == L_STATIC) {
        error_at(name, "function declared in block scope cannot have 'static' storage class");
    }
    if (t->k == T_FN && t->linkage == L_NONE) {
        t->linkage = L_EXTERN; // Functions are 'extern' if unspecified
    }
    Node *n = node((s->k == SCOPE_FILE ||
                    t->linkage == L_STATIC ||
                    t->linkage == L_EXTERN) ? N_GLOBAL : N_LOCAL, name);
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

static void def_tag(Scope *s, Token *tag, Type *t) {
    assert(!map_get(s->tags, tag->s));
    map_put(s->tags, tag->s, t);
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


// ---- Declaration Specifiers ------------------------------------------------

static Type * parse_decl_specs(Scope *s, int *sclass);
static Type * parse_declarator(Scope *s, Type *base, Token **name, Vec *param_names);

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

static size_t pad(size_t offset, size_t align) {
    return offset % align == 0 ? offset : offset + align - (offset % align);
}

static void parse_fields(Scope *s, Type *t, int is_struct) {
    expect_tk(s->l, '{');
    size_t align = 0;
    size_t offset = 0;
    Vec *fields = vec_new();
    while (!peek_tk_is(s->l, '}') && !peek_tk_is(s->l, TK_EOF)) {
        Token *tk = peek_tk(s->l);
        int sclass;
        Type *base = parse_decl_specs(s, &sclass);
        if (sclass != S_NONE) {
            error_at(tk, "illegal storage class specifier in %s field",
                     is_struct ? "struct" : "union");
        }
        if (peek_tk_is(s->l, ';')) {
            if (is_struct) offset = pad(offset, base->align);
            vec_push(fields, new_field(base, NULL, offset));
            if (is_struct) offset += base->size;
        }
        while (!peek_tk_is(s->l, ';') && !peek_tk_is(s->l, TK_EOF)) {
            Token *name;
            Type *ft = parse_declarator(s, base, &name, NULL);
            if (is_incomplete(ft)) {
                error_at(name, "%s field cannot have incomplete type",
                         is_struct ? "struct" : "union");
            }
            if (find_field(t, name->s) != NOT_FOUND) {
                error_at(name, "duplicate field '%s' in %s", name->s,
                         is_struct ? "struct" : "union");
            }
            align = ft->align > align ? ft->align : align;
            if (is_struct) offset = pad(offset, ft->align);
            vec_push(fields, new_field(ft, name->s, offset));
            if (is_struct) offset += ft->size;
            if (!next_tk_opt(s->l, ',')) {
                break;
            }
        }
        expect_tk(s->l, ';');
    }
    expect_tk(s->l, '}');
    t->align = align;
    t->size = pad(offset, align);
    t->fields = fields;
}

static void parse_struct_union_def(Scope *s, Type *t, int is_struct) {
    if (!peek_tk_is(s->l, TK_IDENT)) { // Anonymous struct
        parse_fields(s, t, is_struct);
        return;
    }
    Token *tag = next_tk(s->l);
    if (peek_tk_is(s->l, '{')) { // Definition
        Type *tt = map_get(s->tags, tag->s);
        if (tt && tt->fields) { // Redefinition in same scope
            error_at(tag, "redefinition of %s '%s'",
                     is_struct ? "struct" : "union", tag->s);
        } else if (!tt) { // No previous declaration
            def_tag(s, tag, t);
        }
        parse_fields(s, t, is_struct);
        if (tt) {
            tt->fields = t->fields;
        }
    } else {
        Type *tt = find_tag(s, tag->s);
        if (!tt) { // Declaration
            def_tag(s, tag, t);
        } else { // Use
            if (is_struct ? tt->k != T_STRUCT : tt->k != T_UNION) {
                error_at(tag, "use of %s tag '%s' that does not match previous declaration",
                         is_struct ? "struct" : "union", tag->s);
            }
            t->fields = tt->fields;
        }
    }
}

static Type * parse_union_def(Scope *s) {
    Type *t = t_union();
    parse_struct_union_def(s, t, 0);
    return t;
}

static Type * parse_struct_def(Scope *s) {
    Type *t = t_struct();
    parse_struct_union_def(s, t, 1);
    return t;
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


// ---- Declarators -----------------------------------------------------------

static Type * parse_declarator_tail(Scope *s, Type *base, Vec *param_names);

static Node *  parse_expr(Scope *s);
static int64_t calc_int_expr(Node *e);

static Type * parse_array_declarator(Scope *s, Type *base) {
    expect_tk(s->l, '[');
    uint64_t len = NO_ARR_LEN;
    if (!next_tk_opt(s->l, ']')) {
        Node *num = parse_expr(s);
        len = calc_int_expr(num);
        expect_tk(s->l, ']');
    }
    Token *err = peek_tk(s->l);
    Type *t = parse_declarator_tail(s, base, NULL);
    if (t->k == T_FN) {
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
        t = t_ptr(t->elem);
    } else if (t->k == T_FN) { // Function is adjusted to pointer to function
        t = t_ptr(t);
    }
    if (is_incomplete(t)) {
        error_at(err, "parameter cannot have incomplete type");
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
    if (peek_tk_is(s->l, TK_VOID) && peek2_tk_is(s->l, ')')) {
        next_tk(s->l); next_tk(s->l); // 'void' ')'
        return t_fn(ret, vec_new());
    }
    Vec *param_types = vec_new();
    while (!peek_tk_is(s->l, ')') && !peek_tk_is(s->l, TK_EOF)) {
        Token *name;
        Type *param = parse_fn_declarator_param(s, &name);
        vec_push(param_types, param);
        if (param_names) {
            vec_push(param_names, name); // Name may be NULL
        }
        if (!next_tk_opt(s->l, ',')) {
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
    while (next_tk_opt(s->l, TK_CONST) ||
           next_tk_opt(s->l, TK_RESTRICT) ||
           next_tk_opt(s->l, TK_VOLATILE));
}

static Type * parse_declarator(Scope *s, Type *base, Token **name, Vec *param_names) {
    if (next_tk_opt(s->l, '*')) {
        skip_type_quals(s);
        return parse_declarator(s, t_ptr(base), name, param_names);
    }
    if (next_tk_opt(s->l, '(')) { // Either sub-declarator or fn parameters
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

enum { // Operator precedence
    PREC_MIN,
    PREC_COMMA,   // ,
    PREC_ASSIGN,  // =, +=, -=, *=, /=, %=, &=, |=, ^=, >>=, <<=
    PREC_TERNARY, // ?
    PREC_LOG_OR,  // ||
    PREC_LOG_AND, // &&
    PREC_BIT_OR,  // |
    PREC_BIT_XOR, // ^
    PREC_BIT_AND, // &
    PREC_EQ,      // ==, !=
    PREC_REL,     // <, >, <=, >=
    PREC_SHIFT,   // >>, <<
    PREC_ADD,     // +, -
    PREC_MUL,     // *, /, %
    PREC_UNARY,   // ++, -- (prefix), -, + (unary), !, ~, casts, *, &, sizeof
};

static int BINOP_PREC[TK_LAST] = {
    ['+'] = PREC_ADD, ['-'] = PREC_ADD,
    ['*'] = PREC_MUL, ['/'] = PREC_MUL, ['%'] = PREC_MUL,
    ['&'] = PREC_BIT_AND, ['|'] = PREC_BIT_OR, ['^'] = PREC_BIT_XOR,
    [TK_SHL] = PREC_SHIFT, [TK_SHR] = PREC_SHIFT,
    [TK_EQ] = PREC_EQ, [TK_NEQ] = PREC_EQ,
    ['<'] = PREC_REL, [TK_LE] = PREC_REL, ['>'] = PREC_REL, [TK_GE] = PREC_REL,
    [TK_LOG_AND] = PREC_LOG_AND, [TK_LOG_OR] = PREC_LOG_OR,
    ['='] = PREC_ASSIGN,
    [TK_A_ADD] = PREC_ASSIGN, [TK_A_SUB] = PREC_ASSIGN,
    [TK_A_MUL] = PREC_ASSIGN, [TK_A_DIV] = PREC_ASSIGN,
    [TK_A_MOD] = PREC_ASSIGN,
    [TK_A_BIT_AND] = PREC_ASSIGN,
    [TK_A_BIT_OR] = PREC_ASSIGN, [TK_A_BIT_XOR] = PREC_ASSIGN,
    [TK_A_SHL] = PREC_ASSIGN, [TK_A_SHR] = PREC_ASSIGN,
    [','] = PREC_COMMA, ['?'] = PREC_TERNARY,
};

static int IS_RASSOC[TK_LAST] = {
    ['?'] = 1, ['='] = 1,
    [TK_A_ADD] = 1, [TK_A_SUB] = 1,
    [TK_A_MUL] = 1, [TK_A_DIV] = 1, [TK_A_MOD] = 1,
    [TK_A_BIT_AND] = 1, [TK_A_BIT_OR] = 1, [TK_A_BIT_XOR] = 1,
    [TK_A_SHL] = 1, [TK_A_SHR] = 1,
};

static Node * parse_subexpr(Scope *s, int min_prec);
static Node * parse_decl_init(Scope *s, Type *t);

static Node * conv_to(Node *l, Type *t) {
    Node *n = l;
    if (!are_equal(l->t, t)) {
        n = node(N_CONV, l->tk);
        n->t = t;
        n->l = l;
    }
    return n;
}

static Node * discharge(Node *l) {
    Node *n;
    switch (l->t->k) {
    case T_CHAR: case T_SHORT: // Small types are converted to int
        n = conv_to(l, t_num(T_INT, 0));
        break;
    case T_ARR: // Arrays are converted to pointers
        n = conv_to(l, t_ptr(l->t->elem));
        break;
    case T_FN: // Functions are converted to pointer to functions
        n = node(N_ADDR, l->tk);
        n->t = t_ptr(l->t);
        n->l = l;
        break;
    default: n = l; break;
    }
    return n;
}

static Node * parse_operand(Scope *s) {
    Node *n;
    Token *t = next_tk(s->l);
    switch (t->k) {
    case TK_NUM:
        n = parse_num(t);
        break;
    case TK_CH:
        n = node(N_IMM, t);
        n->t = t_num(T_CHAR, 0);
        n->imm = t->ch;
        break;
    case TK_STR:
        n = node(N_STR, t);
        n->t = t_arr(t_num(T_CHAR, 0), t->len);
        n->str = t->s;
        n->len = t->len;
        break;
    case TK_IDENT:
        n = find_var(s, t->s);
        if (!n) error_at(t, "undeclared identifier '%s'", t->s);
        break;
    case '(':
        n = parse_subexpr(s, PREC_MIN);
        expect_tk(s->l, ')');
        break;
    default: error_at(t, "expected expression");
    }
    return n;
}

static void ensure_arith(Node *n) {
    if (!is_arith(n->t)) error_at(n->tk, "expected arithmetic type");
}

static void ensure_int(Node *n) {
    if (!is_int(n->t)) error_at(n->tk, "expected integer type");
}

static void ensure_ptr(Node *n) {
    if (n->t->k != T_PTR) error_at(n->tk, "expected pointer type");
}

static void ensure_lvalue(Node *n) {
    if (n->k != N_LOCAL && n->k != N_GLOBAL && n->k != N_DEREF && n->k != N_IDX)
        error_at(n->tk, "expression is not assignable");
    if (n->t->k == T_ARR)  error_at(n->tk, "array type is not assignable");
    if (n->t->k == T_FN)   error_at(n->tk, "function type is not assignable");
    if (n->t->k == T_VOID) error_at(n->tk, "'void' type is not assignable");
}

static int is_null_ptr(Node *n) {
    while (n->k == N_CONV) n = n->l; // Skip over the conversions
    return n->k == N_IMM && n->imm == 0;
}

static Node * parse_array_access(Scope *s, Node *l, Token *op) {
    l = discharge(l);
    if (l->t->k != T_PTR) {
        error_at(op, "expected pointer or array type");
    }
    Node *idx = parse_subexpr(s, PREC_MIN);
    ensure_int(idx);
    idx = conv_to(idx, t_num(T_LLONG, 1));
    expect_tk(s->l, ']');
    Node *n = node(N_IDX, op);
    n->t = l->t->elem;
    n->arr = l;
    n->idx = idx;
    return n;
}

static Node * parse_call(Scope *s, Node *l, Token *op) {
    l = discharge(l);
    if (l->t->k != T_PTR || l->t->ptr->k != T_FN) {
        error_at(l->tk, "expected function type");
    }
    Type *fn_t = l->t->ptr;
    Vec *args = vec_new();
    while (!peek_tk_is(s->l, ')') && !peek_tk_is(s->l, TK_EOF)) {
        Node *arg = parse_subexpr(s, PREC_COMMA);
        arg = discharge(arg);
        if (vec_len(args) >= vec_len(fn_t->params)) {
            error_at(arg->tk, "too many arguments to function call");
        }
        Type *expected = vec_get(fn_t->params, vec_len(args));
        arg = conv_to(arg, expected);
        vec_push(args, arg);
        if (!next_tk_opt(s->l, ',')) {
            break;
        }
    }
    if (vec_len(args) < vec_len(fn_t->params)) {
        error_at(peek_tk(s->l), "too few arguments to function call");
    }
    expect_tk(s->l, ')');
    Node *n = node(N_CALL, op);
    n->t = fn_t->ret;
    n->fn = l;
    n->args = args;
    return n;
}

static Node * parse_struct_field_access(Scope *s, Node *l, Token *op) {
    if (l->t->k != T_STRUCT && l->t->k != T_UNION) {
        error_at(op, "expected struct or union type");
    }
    Token *name = expect_tk(s->l, TK_IDENT);
    size_t f_idx = find_field(l->t, name->s);
    if (f_idx == NOT_FOUND) {
        error_at(name, "no field named '%s' in %s", name->s,
                 l->t->k == T_STRUCT ? "struct" : "union");
    }
    Field *f = vec_get(l->t->fields, f_idx);
    Node *n = node(N_DOT, op);
    n->t = f->t;
    n->strct = l;
    n->field_name = name->s;
    return n;
}

static Node * parse_struct_field_deref(Scope *s, Node *l, Token *op) {
    ensure_ptr(l);
    Node *n = node(N_DEREF, op);
    n->t = l->t->ptr;
    n->l = l;
    return parse_struct_field_access(s, n, op);
}

static Node * parse_post_inc_dec(Node *l, Token *op) {
    ensure_lvalue(l);
    l = discharge(l);
    Node *n = node(op->k == TK_INC ? N_POST_INC : N_POST_DEC, op);
    n->t = l->t;
    n->l = l;
    return n;
}

static Node * parse_postfix(Scope *s, Node *l) {
    while (1) {
        Token *op = next_tk(s->l);
        switch (op->k) {
        case '[': l = parse_array_access(s, l, op); break;
        case '(': l = parse_call(s, l, op); break;
        case '.': l = parse_struct_field_access(s, l, op); break;
        case TK_ARROW: l = parse_struct_field_deref(s, l, op); break;
        case TK_INC: case TK_DEC: l = parse_post_inc_dec(l, op); break;
        default:
            undo_tk(s->l, op);
            return l;
        }
    }
}

static Node * parse_neg(Scope *s, Token *op) {
    Node *l = parse_subexpr(s, PREC_UNARY);
    ensure_arith(l);
    l = discharge(l);
    Node *unop = node(N_NEG, op);
    unop->t = l->t;
    unop->l = l;
    return unop;
}

static Node * parse_plus(Scope *s) {
    Node *l = parse_subexpr(s, PREC_UNARY);
    return discharge(l); // Type promotion
}

static Node * parse_bit_not(Scope *s, Token *op) {
    Node *l = parse_subexpr(s, PREC_UNARY);
    ensure_int(l);
    l = discharge(l);
    Node *unop = node(N_BIT_NOT, op);
    unop->t = l->t;
    unop->l = l;
    return unop;
}

static Node * parse_log_not(Scope *s, Token *op) {
    Node *l = parse_subexpr(s, PREC_UNARY);
    l = discharge(l);
    Node *unop = node(N_LOG_NOT, op);
    unop->t = t_num(T_INT, 0);
    unop->l = l;
    return unop;
}

static Node * parse_pre_inc_dec(Scope *s, Token *op) {
    Node *l = parse_subexpr(s, PREC_UNARY);
    ensure_lvalue(l);
    l = discharge(l);
    Node *unop = node(op->k == TK_INC ? N_PRE_INC : N_PRE_DEC, op);
    unop->t = l->t;
    unop->l = l;
    return unop;
}

static Node * parse_deref(Scope *s, Token *op) {
    Node *l = parse_subexpr(s, PREC_UNARY);
    l = discharge(l);
    ensure_ptr(l);
    if (l->t->ptr->k == T_FN) return l; // Don't dereference fn ptrs
    Node *unop = node(N_DEREF, op);
    unop->t = l->t->ptr;
    unop->l = l;
    return unop;
}

static Node * parse_addr(Scope *s, Token *op) {
    Node *l = parse_subexpr(s, PREC_UNARY);
    ensure_lvalue(l);
    Node *unop = node(N_ADDR, op);
    unop->t = t_ptr(l->t);
    unop->l = l;
    return unop;
}

static Node * parse_sizeof(Scope *s, Token *op) {
    Type *t;
    if (peek_tk_is(s->l, '(') && is_type(s, peek2_tk(s->l))) {
        next_tk(s->l);
        t = parse_decl_specs(s, NULL);
        t = parse_abstract_declarator(s, t);
        expect_tk(s->l, ')');
    } else {
        Node *l = parse_subexpr(s, PREC_UNARY);
        t = l->t;
    }
    Node *n = node(N_IMM, op);
    n->t = t_num(T_LONG, 1);
    n->imm = t->size;
    return n;
}

static Node * parse_cast(Scope *s) {
    Type *t = parse_decl_specs(s, NULL);
    t = parse_abstract_declarator(s, t);
    expect_tk(s->l, ')');
    if (peek_tk_is(s->l, '{')) { // Compound literal
        return parse_decl_init(s, t);
    } else {
        Node *l = parse_subexpr(s, PREC_UNARY);
        return conv_to(l, t);
    }
}

static Node * parse_unop(Scope *s) {
    Token *op = next_tk(s->l);
    switch (op->k) {
    case '-': return parse_neg(s, op);
    case '+': return parse_plus(s);
    case '~': return parse_bit_not(s, op);
    case '!': return parse_log_not(s, op);
    case TK_INC: case TK_DEC: return parse_pre_inc_dec(s, op);
    case '*': return parse_deref(s, op);
    case '&': return parse_addr(s, op);
    case TK_SIZEOF: return parse_sizeof(s, op);
    case '(':
        if (is_type(s, peek_tk(s->l))) {
            return parse_cast(s);
        } // Fall through otherwise...
    default:
        undo_tk(s->l, op);
        Node *l = parse_operand(s);
        return parse_postfix(s, l);
    }
}

static Type * promote(Type *l, Type *r) { // Implicit arithmetic conversions
    assert(is_arith(l));
    assert(is_arith(r));
    if (l->k < r->k) { // Make 'l' the larger type
        Type *tmp = l; l = r; r = tmp;
    }
    if (is_fp(l)) { // If one is a float, pick the largest float type
        return l;
    }
    assert(is_int(l) && l->size >= 4); // Both integers bigger than 'int'
    assert(is_int(r) && r->size >= 4);
    if (l->size > r->size) { // Pick the larger
        return l;
    }
    assert(l->size == r->size); // Both the same size
    return l->is_unsigned ? l : r; // Pick the unsigned type
}

static Node * emit_binop(int op, Node *l, Node *r, Token *tk) {
    l = discharge(l);
    r = discharge(r);
    Type *t;
    if (l->t->k == T_PTR && r->t->k == T_PTR) { // Both pointers
        if (op != N_SUB && op != N_TERNARY && !(op >= N_EQ && op <= N_LOG_OR)) {
            error_at(tk, "invalid operands to binary operation");
        }
        t = is_void_ptr(l->t) || is_null_ptr(l) ? r->t : l->t;
    } else if (l->t->k == T_PTR || r->t->k == T_PTR) { // One pointer
        t = l->t->k == T_PTR ? l->t : r->t;
    } else { // No pointers
        assert(is_arith(l->t) && is_arith(r->t));
        t = promote(l->t, r->t);
    }
    Type *ret = t;
    if (l->t->k == T_PTR && r->t->k == T_PTR && op == N_SUB) {
        ret = t_num(T_LLONG, 0); // Pointer diff
    } else if (op >= N_EQ && op <= N_LOG_OR) {
        ret = t_num(T_INT, 0); // Comparison
    }
    Node *n = node(op, tk);
    n->l = conv_to(l, t);
    n->r = conv_to(r, t);
    n->t = ret;
    return n;
}

static Node * parse_binop(Scope *s, Token *op, Node *l) {
    Node *r = parse_subexpr(s, BINOP_PREC[op->k] + IS_RASSOC[op->k]);
    Node *n;
    switch (op->k) {
    case '+':    return emit_binop(N_ADD, l, r, op);
    case '-':    return emit_binop(N_SUB, l, r, op);
    case '*':    ensure_arith(l); ensure_arith(r); return emit_binop(N_MUL, l, r, op);
    case '/':    ensure_arith(l); ensure_arith(r); return emit_binop(N_DIV, l, r, op);
    case '%':    ensure_int(l); ensure_int(r); return emit_binop(N_MOD, l, r, op);
    case '&':    ensure_int(l); ensure_int(r); return emit_binop(N_BIT_AND, l, r, op);
    case '|':    ensure_int(l); ensure_int(r); return emit_binop(N_BIT_OR, l, r, op);
    case '^':    ensure_int(l); ensure_int(r); return emit_binop(N_BIT_XOR, l, r, op);
    case TK_SHL: ensure_int(l); ensure_int(r); return emit_binop(N_SHL, l, r, op);
    case TK_SHR: ensure_int(l); ensure_int(r); return emit_binop(N_SHR, l, r, op);

    case TK_EQ:      return emit_binop(N_EQ, l, r, op);
    case TK_NEQ:     return emit_binop(N_NEQ, l, r, op);
    case '<':        return emit_binop(N_LT, l, r, op);
    case TK_LE:      return emit_binop(N_LE, l, r, op);
    case '>':        return emit_binop(N_GT, l, r, op);
    case TK_GE:      return emit_binop(N_GE, l, r, op);
    case TK_LOG_AND: return emit_binop(N_LOG_AND, l, r, op);
    case TK_LOG_OR:  return emit_binop(N_LOG_OR, l, r, op);

    case TK_A_ADD:     return emit_binop(N_A_ADD, l, r, op);
    case TK_A_SUB:     return emit_binop(N_A_SUB, l, r, op);
    case TK_A_MUL:     ensure_arith(l); ensure_arith(r); return emit_binop(N_A_MUL, l, r, op);
    case TK_A_DIV:     ensure_arith(l); ensure_arith(r); return emit_binop(N_A_DIV, l, r, op);
    case TK_A_MOD:     ensure_int(l); ensure_int(r); return emit_binop(N_A_MOD, l, r, op);
    case TK_A_BIT_AND: ensure_int(l); ensure_int(r); return emit_binop(N_A_BIT_AND, l, r, op);
    case TK_A_BIT_OR:  ensure_int(l); ensure_int(r); return emit_binop(N_A_BIT_OR, l, r, op);
    case TK_A_BIT_XOR: ensure_int(l); ensure_int(r); return emit_binop(N_A_BIT_XOR, l, r, op);
    case TK_A_SHL:     ensure_int(l); ensure_int(r); return emit_binop(N_A_SHL, l, r, op);
    case TK_A_SHR:     ensure_int(l); ensure_int(r); return emit_binop(N_A_SHR, l, r, op);

    case '=':
        ensure_lvalue(l);
        n = node(N_ASSIGN, op);
        n->t = l->t;
        n->l = l;
        n->r = conv_to(r, l->t);
        return n;
    case ',':
        n = emit_binop(N_COMMA, l, r, op);
        n->t = n->r->t;
        return n;
    case '?':
        expect_tk(s->l, ':');
        Node *els = parse_subexpr(s, PREC_TERNARY + IS_RASSOC['?']);
        Node *binop = emit_binop(N_TERNARY, r, els, op);
        n = node(N_TERNARY, op);
        n->t = binop->t;
        n->if_cond = l;
        n->if_body = n->l;
        n->if_else = n->r;
        return n;
    default: UNREACHABLE();
    }
}

static Node * parse_subexpr(Scope *s, int min_prec) {
    Node *l = parse_unop(s);
    while (BINOP_PREC[peek_tk(s->l)->k] > min_prec) {
        Token *op = next_tk(s->l);
        l = parse_binop(s, op, l);
    }
    return l;
}

static Node * parse_expr(Scope *s) {
    return parse_subexpr(s, PREC_MIN);
}

static Node * parse_expr_no_commas(Scope *s) {
    return parse_subexpr(s, PREC_COMMA);
}


// ---- Constant Expressions --------------------------------------------------

#define CALC_UNOP_INT(op)          \
    l = calc_const_expr_raw(e->l); \
    if (l->k == N_IMM) {           \
        n = node(N_IMM, e->tk);    \
        n->t = e->t;               \
        n->imm = op l->imm;        \
    } else {                       \
        goto err;                  \
    }                              \
    break;

#define CALC_UNOP_ARITH(op)        \
    l = calc_const_expr_raw(e->l); \
    if (l->k == N_IMM) {           \
        n = node(N_IMM, e->tk);    \
        n->t = e->t;               \
        n->imm = op l->imm;        \
    } else if (l->k == N_FP) {     \
        n = node(N_FP, e->tk);     \
        n->t = e->t;               \
        n->fp = op l->fp;          \
    } else {                       \
        goto err;                  \
    }                              \
    break;

#define CALC_BINOP_INT(op)                                        \
    l = calc_const_expr_raw(e->l), r = calc_const_expr_raw(e->r); \
    if (l->k == N_IMM && r->k == N_IMM) {                         \
        n = node(N_IMM, e->tk);                                   \
        n->t = e->t;                                              \
        n->imm = l->imm op r->imm;                                \
    } else {                                                      \
        goto err;                                                 \
    }                                                             \
    break;

#define CALC_BINOP_ARITH(op)                                      \
    l = calc_const_expr_raw(e->l), r = calc_const_expr_raw(e->r); \
    if (l->k == N_IMM && r->k == N_IMM) {                         \
        n = node(N_IMM, e->tk);                                   \
        n->t = e->t;                                              \
        n->imm = l->imm op r->imm;                                \
    } else if (l->k == N_FP && r->k == N_FP) {                    \
        n = node(N_FP, e->tk);                                    \
        n->t = e->t;                                              \
        n->fp = l->fp op r->fp;                                   \
    } else {                                                      \
        goto err;                                                 \
    }                                                             \
    break;

#define CALC_BINOP_ARITH_PTR(op)                                  \
    l = calc_const_expr_raw(e->l), r = calc_const_expr_raw(e->r); \
    if (l->k == N_IMM && r->k == N_IMM) {                         \
        n = node(N_IMM, e->tk);                                   \
        n->t = e->t;                                              \
        n->imm = l->imm op r->imm;                                \
    } else if (l->k == N_FP && r->k == N_FP) {                    \
        n = node(N_FP, e->tk);                                    \
        n->t = e->t;                                              \
        n->fp = l->fp op r->fp;                                   \
    } else if (l->k == N_KPTR && r->k == N_IMM) {                 \
        n = node(N_KPTR, e->tk);                                  \
        n->t = e->t;                                              \
        n->global = l->global;                                    \
        n->offset = l->offset op (r->imm * n->t->ptr->size);      \
    } else if (l->k == N_IMM && r->k == N_KPTR) {                 \
        n = node(N_KPTR, e->tk);                                  \
        n->t = e->t;                                              \
        n->global = r->global;                                    \
        n->offset = r->offset op (l->imm * n->t->ptr->size);      \
    } else {                                                      \
        goto err;                                                 \
    }                                                             \
    break;

#define CALC_BINOP_EQ(op)                                              \
    l = calc_const_expr_raw(e->l), r = calc_const_expr_raw(e->r);      \
    if (l->k == N_IMM && r->k == N_IMM) {                              \
        n = node(N_IMM, e->tk);                                        \
        n->t = e->t;                                                   \
        n->imm = op (l->imm == r->imm);                                \
    } else if (l->k == N_FP && r->k == N_FP) {                         \
        n = node(N_FP, e->tk);                                         \
        n->t = e->t;                                                   \
        n->fp = op (l->fp == r->fp);                                   \
    } else if (l->k == N_KPTR && r->k == N_KPTR) {                     \
        n = node(N_IMM, e->tk);                                        \
        n->t = e->t;                                                   \
        n->fp = op (l->global == r->global && l->offset == r->offset); \
    } else if ((l->k == N_KPTR && r->k == N_IMM && r->imm == 0) ||     \
               (r->k == N_KPTR && l->k == N_IMM && l->imm == 0)) {     \
        n = node(N_IMM, e->tk);                                        \
        n->t = e->t;                                                   \
        n->imm = op 0;                                                 \
    } else {                                                           \
        goto err;                                                      \
    }                                                                  \
    break;

#define CALC_BINOP_REL(op)                                        \
    l = calc_const_expr_raw(e->l), r = calc_const_expr_raw(e->r); \
    if (l->k == N_IMM && r->k == N_IMM) {                         \
        n = node(N_IMM, e->tk);                                   \
        n->t = e->t;                                              \
        n->imm = l->imm op r->imm;                                \
    } else if (l->k == N_FP && r->k == N_FP) {                    \
        n = node(N_IMM, e->tk);                                   \
        n->t = e->t;                                              \
        n->imm = l->fp op r->fp;                                  \
    } else {                                                      \
        goto err;                                                 \
    }                                                             \
    break;

static Node * calc_const_expr_raw(Node *e) {
    Node *n, *cond, *l, *r;
    switch (e->k) {
        // Constants
    case N_IMM: case N_FP: case N_KPTR: n = e; break;
    case N_ARR: TODO(); // TODO: make new N_ARR with calc_const_expr on each init var
    case N_GLOBAL:
        n = node(N_KVAL, e->tk);
        n->t = e->t;
        n->global = e;
        break;

        // Binary operations
    case N_ADD: CALC_BINOP_ARITH_PTR(+)
    case N_SUB: CALC_BINOP_ARITH_PTR(-)
    case N_MUL: CALC_BINOP_ARITH(*)
    case N_DIV: CALC_BINOP_ARITH(/)
    case N_MOD: CALC_BINOP_INT(%)
    case N_SHL: CALC_BINOP_INT(<<)
    case N_SHR: CALC_BINOP_INT(>>)
    case N_BIT_AND: CALC_BINOP_INT(&)
    case N_BIT_OR:  CALC_BINOP_INT(|)
    case N_BIT_XOR: CALC_BINOP_INT(^)
    case N_EQ:  CALC_BINOP_EQ()
    case N_NEQ: CALC_BINOP_EQ(!)
    case N_LT:  CALC_BINOP_REL(<)
    case N_LE:  CALC_BINOP_REL(<=)
    case N_GT:  CALC_BINOP_REL(>)
    case N_GE:  CALC_BINOP_REL(>=)
    case N_LOG_AND: CALC_BINOP_INT(&&)
    case N_LOG_OR:  CALC_BINOP_INT(||)
    case N_COMMA:
        l = calc_const_expr_raw(e->l), r = calc_const_expr_raw(e->r);
        n = r;
        break;

        // Ternary operation
    case N_TERNARY:
        cond = calc_const_expr_raw(e->if_cond);
        l = calc_const_expr_raw(e->if_body);
        r = calc_const_expr_raw(e->if_else);
        if (cond->k != N_IMM) goto err;
        n = cond->imm ? l : r;
        break;

        // Unary operations
    case N_NEG:     CALC_UNOP_ARITH(-)
    case N_BIT_NOT: CALC_UNOP_INT(~)
    case N_LOG_NOT: CALC_UNOP_INT(!)
    case N_ADDR:
        l = calc_const_expr_raw(e->l);
        if (l->k != N_KVAL) goto err;
        n = node(N_KPTR, e->tk);
        n->t = e->t;
        n->global = l->global;
        break;
    case N_DEREF:
        l = calc_const_expr_raw(e->l);
        if (l->k != N_KPTR) goto err;
        n = node(N_KVAL, e->tk);
        n->t = e->t;
        n->global = l->global;
        n->offset = l->offset;
        break;
    case N_CONV:
        l = calc_const_expr_raw(e->l);
        n = node(l->k, e->tk);
        *n = *l;
        n->t = e->t;
        if (is_fp(n->t) && l->k == N_IMM) { // int -> float
            n->k = N_FP;
            n->fp = (double) l->imm;
        } else if (is_int(n->t) && l->k == N_FP) { // float -> int
            n->k = N_IMM;
            n->imm = (int64_t) l->fp;
        } else if (is_int(n->t) && l->k == N_IMM) { // int -> int
            n->k = N_IMM;
            size_t b = n->t->size * 8; // Bits
            n->imm = l->imm & (((int64_t) 1 << b) - 1); // Truncate to 'dst'
            if (!l->t->is_unsigned && (n->imm & ((int64_t) 1 << (b - 1)))) {
                n->imm |= ~(((int64_t) 1 << b) - 1); // Sext if sign bit set
            }
        }
        break;

        // Postfix operations
    case N_IDX:
        l = calc_const_expr_raw(e->arr), r = calc_const_expr_raw(e->idx);
        if (r->k != N_IMM) goto err;
        if (l->k != N_KVAL && l->k != N_KPTR) goto err;
        n = node(N_KVAL, e->tk);
        n->t = e->t;
        n->global = l->global;
        n->offset = l->offset + (int64_t) (r->imm * n->t->size);
        break;
    case N_DOT:
        l = calc_const_expr_raw(e->strct);
        if (l->k != N_KVAL) goto err;
        size_t f_idx = find_field(l->t, e->field_name);
        assert(f_idx != NOT_FOUND);
        Field *f = vec_get(l->t->fields, f_idx);
        n = node(N_KVAL, e->tk);
        n->t = e->t;
        n->global = l->global;
        n->offset = l->offset + (int64_t) f->offset;
        break;
    default: goto err;
    }
    return n;
err:
    error_at(e->tk, "expected constant expression");
}

static Node * calc_const_expr(Node *e) {
    Node *n = calc_const_expr_raw(e);
    if (n->k == N_KVAL) {
        error_at(e->tk, "expected constant expression");
    }
    return n;
}

static int64_t calc_int_expr(Node *e) {
    Node *n = calc_const_expr_raw(e);
    if (n->k != N_IMM) {
        error_at(e->tk, "expected constant integer expression");
    }
    return (int64_t) n->imm;
}


// ---- Statements ------------------------------------------------------------

static Node * parse_decl(Scope *s);
static Node * parse_block(Scope *s);
static Node * parse_stmt(Scope *s);

static Node * parse_if(Scope *s) {
    Token *if_tk = expect_tk(s->l, TK_IF);
    expect_tk(s->l, '(');
    Node *cond = parse_expr(s);
    expect_tk(s->l, ')');
    Node *body = parse_stmt(s);
    Node *els = NULL;
    if (peek_tk_is(s->l, TK_ELSE)) {
        Token *else_tk = next_tk(s->l);
        if (peek_tk_is(s->l, TK_IF)) { // else if
            els = parse_stmt(s);
        } else { // else
            Node *else_body = parse_stmt(s);
            els = node(N_IF, else_tk);
            els->if_cond = NULL;
            els->if_body = else_body;
            els->if_else = NULL;
        }
    }
    Node *n = node(N_IF, if_tk);
    n->if_cond = cond;
    n->if_body = body;
    n->if_else = els;
    return n;
}

static Node * parse_while(Scope *s) {
    Token *while_tk = expect_tk(s->l, TK_WHILE);
    expect_tk(s->l, '(');
    Node *cond = parse_expr(s);
    expect_tk(s->l, ')');
    Scope loop;
    enter_scope(&loop, s, SCOPE_LOOP);
    Node *body = parse_stmt(&loop);
    Node *n = node(N_WHILE, while_tk);
    n->loop_cond = cond;
    n->loop_body = body;
    return n;
}

static Node * parse_do_while(Scope *s) {
    Token *do_tk = expect_tk(s->l, TK_DO);
    Scope loop;
    enter_scope(&loop, s, SCOPE_LOOP);
    Node *body = parse_stmt(&loop);
    expect_tk(s->l, TK_WHILE);
    expect_tk(s->l, '(');
    Node *cond = parse_expr(s);
    expect_tk(s->l, ')');
    expect_tk(s->l, ';');
    Node *n = node(N_DO_WHILE, do_tk);
    n->loop_cond = cond;
    n->loop_body = body;
    return n;
}

static Node * parse_for(Scope *s) {
    Token *for_tk = expect_tk(s->l, TK_FOR);
    expect_tk(s->l, '(');
    Scope loop;
    enter_scope(&loop, s, SCOPE_LOOP);

    Node *init = NULL;
    if (is_type(&loop, peek_tk(s->l))) {
        init = parse_decl(&loop);
    } else if (!peek_tk_is(s->l, ';')) {
        init = parse_expr(&loop);
        expect_tk(s->l, ';');
    }

    Node *cond = NULL;
    if (!peek_tk_is(s->l, ';')) {
        cond = parse_expr(&loop);
    }
    expect_tk(s->l, ';');

    Node *inc = NULL;
    if (!peek_tk_is(s->l, ')')) {
        inc = parse_expr(&loop);
    }
    expect_tk(s->l, ')');

    Node *body = parse_stmt(&loop);
    Node *n = node(N_FOR, for_tk);
    n->for_init = init;
    n->for_cond = cond;
    n->for_inc = inc;
    n->for_body = body;
    return n;
}

static Node * parse_switch(Scope *s) {
    Token *switch_tk = expect_tk(s->l, TK_SWITCH);
    expect_tk(s->l, '(');
    Node *cond = parse_expr(s);
    expect_tk(s->l, ')');

    Scope switch_s;
    enter_scope(&switch_s, s, SCOPE_SWITCH);
    Node *body = parse_stmt(s);
    Node *n = node(N_SWITCH, switch_tk);
    n->switch_cond = cond;
    n->switch_body = body;
    n->cases = switch_s.cases;
    return n;
}

static Node * parse_case(Scope *s) {
    Token *case_tk = expect_tk(s->l, TK_CASE);
    Scope *switch_s = find_scope(s, SCOPE_SWITCH);
    if (!switch_s) {
        error_at(case_tk, "'case' not allowed here");
    }
    Node *cond = parse_expr(s);
    expect_tk(s->l, ':');
    Node *body = parse_stmt(s);
    Node *n = node(N_CASE, case_tk);
    n->case_cond = cond;
    n->case_body = body;
    vec_push(switch_s->cases, n);
    return n;
}

static Node * parse_default(Scope *s) {
    Token *default_tk = expect_tk(s->l, TK_DEFAULT);
    Scope *switch_s = find_scope(s, SCOPE_SWITCH);
    if (!switch_s) {
        error_at(default_tk, "'default' not allowed here");
    }
    for (size_t i = 0; i < vec_len(switch_s->cases); i++) {
        Node *cas = vec_get(switch_s->cases, i);
        if (cas->k == N_DEFAULT) {
            error_at(default_tk, "cannot have more than one 'default' in a switch");
        }
    }
    expect_tk(s->l, ':');
    Node *body = parse_stmt(s);
    Node *n = node(N_DEFAULT, default_tk);
    n->case_body = body;
    vec_push(switch_s->cases, n);
    return n;
}

static Node * parse_break(Scope *s) {
    Token *break_tk = expect_tk(s->l, TK_BREAK);
    if (!find_scope(s, SCOPE_LOOP) && !find_scope(s, SCOPE_SWITCH)) {
        error_at(break_tk, "'break' not allowed here");
    }
    expect_tk(s->l, ';');
    Node *n = node(N_BREAK, break_tk);
    return n;
}

static Node * parse_continue(Scope *s) {
    Token *continue_tk = expect_tk(s->l, TK_CONTINUE);
    if (!find_scope(s, SCOPE_LOOP)) {
        error_at(continue_tk, "'break' not allowed here");
    }
    expect_tk(s->l, ';');
    Node *n = node(N_CONTINUE, continue_tk);
    return n;
}

static Node * parse_goto(Scope *s) {
    Token *goto_tk = expect_tk(s->l, TK_GOTO);
    Token *label = expect_tk(s->l, TK_IDENT);
    expect_tk(s->l, ';');
    Node *n = node(N_GOTO, goto_tk);
    n->label = label->s;
    return n;
}

static Node * parse_label(Scope *s) {
    Token *label = expect_tk(s->l, TK_IDENT);
    expect_tk(s->l, ':');
    Node *body = parse_stmt(s);
    Node *n = node(N_LABEL, label);
    n->label = label->s;
    n->label_body = body;
    return n;
}

static Node * parse_ret(Scope *s) {
    Token *ret_tk = expect_tk(s->l, TK_RETURN);
    Node *val = NULL;
    if (!peek_tk_is(s->l, ';')) {
        if (s->fn->t->ret->k == T_VOID) {
            error_at(peek_tk(s->l), "cannot return value from void function");
        }
        val = parse_expr(s);
        val = conv_to(val, s->fn->t->ret);
    }
    expect_tk(s->l, ';');
    Node *n = node(N_RET, ret_tk);
    n->ret_val = val;
    return n;
}

static Node * parse_expr_stmt(Scope *s) {
    Node *n = parse_expr(s);
    expect_tk(s->l, ';');
    return n;
}

static Node * parse_stmt(Scope *s) {
    Token *t = peek_tk(s->l);
    switch (t->k) {
    case ';':         next_tk(s->l); return NULL;
    case '{':         return parse_block(s);
    case TK_IF:       return parse_if(s);
    case TK_WHILE:    return parse_while(s);
    case TK_DO:       return parse_do_while(s);
    case TK_FOR:      return parse_for(s);
    case TK_SWITCH:   return parse_switch(s);
    case TK_CASE:     return parse_case(s);
    case TK_DEFAULT:  return parse_default(s);
    case TK_BREAK:    return parse_break(s);
    case TK_CONTINUE: return parse_continue(s);
    case TK_GOTO:     return parse_goto(s);
    case TK_RETURN:   return parse_ret(s);
    case TK_IDENT:
        if (peek2_tk_is(s->l, ':')) {
            return parse_label(s);
        } // Fall through...
    default: return parse_expr_stmt(s);
    }
}

static Node * parse_stmt_or_decl(Scope *s) {
    if (is_type(s, peek_tk(s->l))) {
        return parse_decl(s);
    } else {
        return parse_stmt(s);
    }
}

static Node * parse_block(Scope *s) {
    expect_tk(s->l, '{');
    Scope block;
    enter_scope(&block, s, SCOPE_BLOCK);
    Node *head = NULL;
    Node **cur = &head;
    while (!peek_tk_is(s->l, '}') && !peek_tk_is(s->l, TK_EOF)) {
        *cur = parse_stmt_or_decl(&block);
        while (*cur) cur = &(*cur)->next;
    }
    expect_tk(s->l, '}');
    return head;
}


// ---- Declarations ----------------------------------------------------------

static Node * parse_fn_def(Scope *s, Type *t, Token *name, Vec *param_names) {
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

static void parse_string_init(Scope *s, Vec *inits, Type *t, size_t offset) {
    assert(is_char_arr(t));
    Token *str = next_tk_opt(s->l, TK_STR);
    if (!str && peek_tk_is(s->l, '{') && peek2_tk_is(s->l, TK_STR)) {
        next_tk(s->l);
        str = next_tk(s->l);
        expect_tk(s->l, '}');
    }
    if (!str) {
        return; // Parse as normal array
    }
    if (t->len == NO_ARR_LEN) {
        t->len = str->len;
        t->size = t->len * t->elem->size;
    }
    if (t->len < str->len) {
        warning_at(str, "initializer string is too long");
    }
    for (size_t i = 0; i < t->len || i < str->len; i++) {
        Node *ch = node(N_IMM, str);
        ch->t = t->elem;
        ch->imm = i < str->len ? (uint64_t) str->s[i] : 0;
        Node *n = node(N_INIT, str);
        n->init_offset = offset + i;
        n->init_val = ch;
        vec_push(inits, n);
    }
}

static void parse_init_list_raw(Scope *s, Vec *inits, Type *t, size_t offset, int designated);

static void parse_init_elem(Scope *s, Vec *inits, Type *t, size_t offset, int designated) {
    if (t->k == T_ARR || t->k == T_STRUCT || t->k == T_UNION || peek_tk_is(s->l, '{')) {
        parse_init_list_raw(s, inits, t, offset, designated);
    } else {
        Node *e = parse_expr_no_commas(s);
        e = conv_to(e, t);
        Node *n = node(N_INIT, e->tk);
        n->init_offset = offset;
        n->init_val = e;
        vec_push(inits, n);
    }
    if (!peek_tk_is(s->l, '}')) {
        expect_tk(s->l, ',');
    }
}

static size_t parse_array_designator(Scope *s, Type *t) {
    expect_tk(s->l, '[');
    Node *e = parse_expr(s);
    int64_t desg = calc_int_expr(e);
    if (desg < 0 || (uint64_t) desg >= t->len) {
        error_at(e->tk, "array designator index '%llu' exceeds array bounds", desg);
    }
    expect_tk(s->l, ']');
    expect_tk(s->l, '=');
    return desg;
}

static void parse_array_init(Scope *s, Vec *inits, Type *t, size_t offset, int designated) {
    assert(t->k == T_ARR);
    int has_brace = next_tk_opt(s->l, '{') != NULL;
    size_t idx = 0;
    while (!peek_tk_is(s->l, '}') && !peek_tk_is(s->l, TK_EOF)) {
        if (!has_brace && t->len != NO_ARR_LEN && idx >= t->len) {
            break;
        }
        if (peek_tk_is(s->l, '[') && !has_brace && !designated) {
            break; // e.g., int a[3][3] = {3 /* RETURN HERE */, [2] = 1}
        }
        if (peek_tk_is(s->l, '[')) {
            idx = parse_array_designator(s, t);
            designated = 1;
        }
        int excess = t->len != NO_ARR_LEN && idx >= t->len;
        if (excess) {
            warning_at(peek_tk(s->l), "excess elements in array initializer");
        }
        size_t elem_offset = offset + idx * t->elem->size;
        parse_init_elem(s, excess ? NULL : inits, t->elem, elem_offset, designated);
        idx++;
        designated = 0;
    }
    if (has_brace) {
        expect_tk(s->l, '}');
    }
    if (t->len == NO_ARR_LEN) {
        t->len = idx;
        t->size = t->len * t->elem->size;
    }
}

static size_t parse_struct_designator(Scope *s, Type *t) {
    expect_tk(s->l, '.');
    Token *name = expect_tk(s->l, TK_IDENT);
    size_t f_idx = find_field(t, name->s);
    if (f_idx == NOT_FOUND) {
        error_at(name, "designator '%s' does not refer to any field in the %s",
                 name->s, t->k == T_STRUCT ? "struct" : "union");
    }
    expect_tk(s->l, '=');
    return f_idx;
}

static void parse_struct_init(Scope *s, Vec *inits, Type *t, size_t offset, int designated) {
    assert(t->k == T_STRUCT || t->k == T_UNION);
    int has_brace = next_tk_opt(s->l, '{') != NULL;
    size_t idx = 0;
    while (!peek_tk_is(s->l, '}') && !peek_tk_is(s->l, TK_EOF)) {
        if (!has_brace && idx >= vec_len(t->fields)) {
            break;
        }
        if (peek_tk_is(s->l, '.') && !has_brace && !designated) {
            break;
        }
        if (peek_tk_is(s->l, '.')) {
            idx = parse_struct_designator(s, t);
            designated = 1;
        }
        int excess = idx >= vec_len(t->fields);
        if (excess) {
            warning_at(peek_tk(s->l), "excess elements in %s initializer",
                       t->k == T_STRUCT ? "struct" : "union");
        }
        Field *f = vec_get(t->fields, excess ? vec_len(t->fields) - 1 : idx);
        size_t field_offset = offset + f->offset;
        parse_init_elem(s, excess ? NULL : inits, f->t, field_offset, designated);
        idx++;
        designated = 0;
    }
    if (has_brace) {
        expect_tk(s->l, '}');
    }
}

static void parse_init_list_raw(Scope *s, Vec *inits, Type *t, size_t offset, int designated) {
    if (is_char_arr(t)) {
        parse_string_init(s, inits, t, offset);
        return;
    } // Fall through if no string literal...
    if (t->k == T_ARR) {
        parse_array_init(s, inits, t, offset, designated);
    } else if (t->k == T_STRUCT || t->k == T_UNION) {
        parse_struct_init(s, inits, t, offset, designated);
    } else { // Everything else, e.g., int a = {3}
        Type *arr_t = t_arr(t, 1);
        parse_array_init(s, inits, arr_t, offset, designated);
    }
}

static Node * parse_init_list(Scope *s, Type *t) {
    Node *n = node(N_ARR, peek_tk(s->l));
    n->t = t;
    n->inits = vec_new();
    parse_init_list_raw(s, n->inits, t, 0, 0);
    return n;
}

static Node * parse_decl_init(Scope *s, Type *t) {
    if (t->linkage == L_EXTERN || t->k == T_FN) {
        error_at(peek_tk(s->l), "illegal initializer");
    }
    Node *init;
    if (peek_tk_is(s->l, '{') || is_char_arr(t)) {
        init = parse_init_list(s, t);
    } else {
        init = parse_expr_no_commas(s);
    }
    init = conv_to(init, t);
    if (t->linkage == L_STATIC || s->k == SCOPE_FILE) {
        init = calc_const_expr(init);
    }
    return init;
}

static Node * parse_decl_var(Scope *s, Type *t, Token *name) {
    Node *var = def_var(s, name, t);
    Node *init = NULL;
    if (next_tk_opt(s->l, '=')) {
        init = parse_decl_init(s, t);
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
    if (next_tk_opt(s->l, ';')) {
        return NULL;
    }
    Node *head = NULL;
    Node **cur = &head;
    while (1) {
        *cur = parse_init_decl(s, t_copy(base), sclass);
        if ((*cur)->k == N_FN_DEF) {
            return head;
        }
        while (*cur) cur = &(*cur)->next;
        if (!next_tk_opt(s->l, ',')) {
            break;
        }
    }
    expect_tk(s->l, ';');
    return head;
}

Node * parse(char *path) {
    File *f = new_file(path);
    Lexer *l = new_lexer(f);
    Scope file_scope = {0};
    file_scope.k = SCOPE_FILE;
    file_scope.l = l;
    file_scope.vars = map_new();
    file_scope.tags = map_new();
    Node *head = NULL;
    Node **cur = &head;
    while (!next_tk_opt(l, TK_EOF)) {
        *cur = parse_decl(&file_scope);
        while (*cur) cur = &(*cur)->next;
    }
    return head;
}
