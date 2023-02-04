
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <inttypes.h>

#include "parse.h"
#include "pp.h"
#include "err.h"

enum {
    SCOPE_FILE,
    SCOPE_BLOCK,
    SCOPE_LOOP,
    SCOPE_SWITCH,
};

typedef struct Scope {
    struct Scope *outer;
    int k;
    PP *pp;
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

static Scope new_scope(int k, PP *pp) {
    Scope s = {0};
    s.k = k;
    s.pp = pp;
    s.vars = map_new();
    s.tags = map_new();
    return s;
}

static void enter_scope(Scope *inner, Scope *outer, int k) {
    *inner = (Scope) {0};
    inner->k = k;
    inner->pp = outer->pp;
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
    if (t->k == T_FN && s->k != SCOPE_FILE && t->linkage == L_STATIC) {
        error_at(name, "function declared in block scope cannot have 'static' storage class");
    }
    if (t->k == T_FN && t->linkage == L_NONE) {
        t->linkage = L_EXTERN; // Functions are 'extern' if unspecified
    }
    int is_global = s->k == SCOPE_FILE || t->linkage == L_STATIC || t->linkage == L_EXTERN;
    Node *n = node(is_global ? N_GLOBAL : N_LOCAL, name);
    n->t = t;
    n->var_name = name->ident;
    def_symbol(s, n);
    return n;
}

static Node * def_typedef(Scope *s, Token *name, Type *t) {
    assert(t->linkage == L_NONE);
    Node *n = node(N_TYPEDEF, name);
    n->t = t;
    n->var_name = name->ident;
    def_symbol(s, n);
    return n;
}

static void def_tag(Scope *s, Token *tag, Type *t) {
    Type *v = map_get(s->tags, tag->ident);
    if (v && v->fields) {
        char *name = t->k == T_STRUCT ? "struct" : t->k == T_UNION ? "union" : "enum";
        error_at(tag, "redefinition of %s '%s'", name, tag->ident);
    }
    map_put(s->tags, tag->ident, t);
}

static void def_enum_const(Scope *s, Token *name, Type *t, int64_t val) {
    Node *v = map_get(s->vars, name->ident);
    if (v && v->k != t->k) {
        error_at(name, "redefinition of '%s' as a different kind of symbol", name->ident);
    } else if (v) {
        error_at(name, "redefinition of enum tag '%s'", name->ident);
    }
    Node *n = node(N_IMM, name);
    n->t = t;
    n->imm = val;
    map_put(s->vars, name->ident, n);
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

static Type * smallest_type_for_int(uint64_t num, int signed_only) {
    if (signed_only) { // Decimal constant is either int, long, or long long
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
    if (strncasecmp(tk->num, "0b", 2) == 0) { // strotoul can't read '0b' prefix
        num = strtoul(tk->num + 2, &suffix, 2);
    } else { // strtoul can read '0x' and '0' prefix
        num = strtoul(tk->num, &suffix, 0);
    }
    Type *t;
    if (*suffix == '\0') { // No suffix; select type based on how large 'num' is
        int is_base_10 = (*tk->num != '0');
        t = smallest_type_for_int(num, is_base_10);
    } else { // Type specified by suffix
        t = parse_int_suffix(suffix);
        if (!t) {
            error_at(tk, "invalid integer suffix '%s'", suffix);
        }
        uint64_t invalid_bits = ~((1 << t->size) - 1);
        if ((num & invalid_bits) != 0) {
            warning_at(tk, "integer '%s' too large for specified type", tk->num);
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
    double num = strtod(tk->num, &suffix);
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

static Node * parse_num(Scope *s) {
    Token *tk = expect_tk(s->pp, TK_NUM);
    if (strpbrk(tk->num, ".pP") ||
            (strncasecmp(tk->num, "0x", 2) != 0 && strpbrk(tk->num, "eE"))) {
        return parse_float(tk);
    } else {
        return parse_int(tk);
    }
}

static Token * parse_str_concat(Scope *s) {
    Buf *buf = buf_new();
    Token *t = peek_tk(s->pp);
    assert(t->k == TK_STR);
    Token *str = copy_tk(t);
    while (t->k == TK_STR) {
        t = next_tk(s->pp);
        if (t->str_enc != str->str_enc) {
            error_at(t, "cannot concatenate strings with different encodings");
        }
        buf_nprint(buf, t->str, t->len);
        t = peek_tk(s->pp);
    }
    buf_push(buf, '\0');
    str->str = buf->data;
    str->len = buf->len;
    return str;
}

static Node * parse_str(Scope *s) {
    Token *tk = parse_str_concat(s);
    Node *len = node(N_IMM, tk);
    len->t = t_num(T_LLONG, 1);
    Node *n = node(N_STR, tk);
    n->enc = tk->str_enc;
    switch (tk->str_enc) {
    case ENC_NONE:
        n->len = tk->len;
        n->str = tk->str;
        n->t = t_arr(t_num(T_CHAR, 0), len);
        break;
    case ENC_CHAR16:
        n->str16 = utf8_to_utf16(tk->str, tk->len, &n->len);
        n->t = t_arr(t_num(T_SHORT, 1), len);
        break;
    case ENC_CHAR32: case ENC_WCHAR:
        n->str32 = utf8_to_utf32(tk->str, tk->len, &n->len);
        n->t = t_arr(t_num(T_INT, 1), len);
        break;
    }
    if (!n->str) {
        error_at(tk, "invalid UTF-8 string");
    }
    len->imm = n->len;
    return n;
}


// ---- Declaration Specifiers ------------------------------------------------

static Type * parse_decl_specs(Scope *s, int *sclass);
static Type * parse_declarator(Scope *s, Type *base, Token **name, Vec *param_names);

static Node * parse_expr_no_commas(Scope *s);
static int64_t calc_int_expr(Node *e);

static int is_type(Scope *s, Token *t) {
    if (t->k == TK_IDENT) {
        return find_typedef(s, t->ident) != NULL;
    } else {
        return t->k >= TK_VOID && t->k <= TK_VOLATILE;
    }
}

static size_t pad(size_t offset, size_t align) {
    return offset % align == 0 ? offset : offset + align - (offset % align);
}

static void parse_struct_fields(Scope *s, Type *t) {
    expect_tk(s->pp, '{');
    size_t align = 0;
    size_t offset = 0;
    Vec *fields = vec_new();
    while (!peek_tk_is(s->pp, '}') && !peek_tk_is(s->pp, TK_EOF)) {
        Token *tk = peek_tk(s->pp);
        int sclass;
        Type *base = parse_decl_specs(s, &sclass);
        if (sclass != S_NONE) {
            error_at(tk, "illegal storage class specifier in %s field",
                     t->k == T_STRUCT ? "struct" : "union");
        }
        if (peek_tk_is(s->pp, ';')) {
            if (t->k == T_STRUCT) offset = pad(offset, base->align);
            vec_push(fields, new_field(base, NULL, offset));
            if (t->k == T_STRUCT) offset += base->size;
        }
        while (!peek_tk_is(s->pp, ';') && !peek_tk_is(s->pp, TK_EOF)) {
            Token *name;
            Type *ft = parse_declarator(s, base, &name, NULL);
            if (is_incomplete(ft)) {
                error_at(name, "%s field cannot have incomplete type",
                         t->k == T_STRUCT ? "struct" : "union");
            }
            if (find_field(t, name->ident) != NOT_FOUND) {
                error_at(name, "duplicate field '%s' in %s", name->ident,
                         t->k == T_STRUCT ? "struct" : "union");
            }
            align = ft->align > align ? ft->align : align;
            if (t->k == T_STRUCT) offset = pad(offset, ft->align);
            vec_push(fields, new_field(ft, name->ident, offset));
            if (t->k == T_STRUCT) offset += ft->size;
            if (!next_tk_is(s->pp, ',')) {
                break;
            }
        }
        expect_tk(s->pp, ';');
    }
    expect_tk(s->pp, '}');
    t->align = align;
    t->size = pad(offset, align);
    t->fields = fields;
}

static void parse_enum_consts(Scope *s, Type *enum_t) {
    expect_tk(s->pp, '{');
    Type *t = t_num(T_INT, 0);
    Vec *fields = vec_new();
    int64_t val = 0;
    while (!peek_tk_is(s->pp, '}') && !peek_tk_is(s->pp, TK_EOF)) {
        Token *name = expect_tk(s->pp, TK_IDENT);
        if (next_tk_is(s->pp, '=')) {
            Node *e = parse_expr_no_commas(s);
            val = calc_int_expr(e);
        }
        Type *min = smallest_type_for_int(val < 0 ? -val : val, val < 0);
        if (min->k > t->k || (min->k == t->k && min->is_unsigned)) {
            *t = *min;
        }
        vec_push(fields, new_field(t, name->ident, val));
        def_enum_const(s, name, t, val);
        val++;
        if (!next_tk_is(s->pp, ',')) {
            break;
        }
    }
    expect_tk(s->pp, '}');
    enum_t->fields = fields;
    enum_t->size = t->size;
    enum_t->align = t->align;
}

static void parse_fields(Scope *s, Type *t) {
    if (t->k == T_STRUCT || t->k == T_UNION) {
        parse_struct_fields(s, t);
    } else { // T_ENUM
        parse_enum_consts(s, t);
    }
}

static void parse_struct_union_enum_def(Scope *s, Type *t) {
    if (!peek_tk_is(s->pp, TK_IDENT)) { // Anonymous
        parse_fields(s, t);
        return;
    }
    Token *tag = next_tk(s->pp);
    Type *tt = find_tag(s, tag->ident);
    if (peek_tk_is(s->pp, '{')) { // Definition
        def_tag(s, tag, t);
        parse_fields(s, t);
    } else if (tt && t->k != tt->k) {
        error_at(tag, "use of tag '%s' does not match previous declaration", tag->ident);
    } else if (tt) { // Use
        t->fields = tt->fields;
    } else { // Forward declaration
        def_tag(s, tag, t);
    }
}

static Type * parse_struct_def(Scope *s) {
    Type *t = t_struct();
    parse_struct_union_enum_def(s, t);
    return t;
}

static Type * parse_union_def(Scope *s) {
    Type *t = t_union();
    parse_struct_union_enum_def(s, t);
    return t;
}

static Type * parse_enum_def(Scope *s) {
    Type *t = t_enum();
    parse_struct_union_enum_def(s, t);
    return t;
}

static Type * parse_decl_specs(Scope *s, int *sclass) {
    if (!is_type(s, peek_tk(s->pp))) {
        error_at(peek_tk(s->pp), "expected type name");
    }
    int sc = 0, tq = 0, fs = 0;
    enum { tnone, tvoid, tchar, tint, tfloat, tdouble } kind = 0;
    enum { tlong = 1, tllong, tshort } size = 0;
    enum { tsigned = 1, tunsigned } sign = 0;
    Type *t = NULL;
    Token *tk;
    while (1) {
        tk = next_tk(s->pp);
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
            if (!find_typedef(s, tk->ident)) goto done;
            if (t) goto t_err;
            t = find_typedef(s, tk->ident);
            break;
        default: goto done;
        }
        if (size == tshort && !(kind == tnone || kind == tint)) goto t_err;
        if (size == tlong && !(kind == tnone || kind == tint || kind == tdouble)) goto t_err;
        if (sign && !(kind == tnone || kind == tchar || kind == tint)) goto t_err;
        if (t && (kind || size || sign)) goto t_err;
    }
done:
    (void) tq; // Unused
    undo_tk(s->pp->l, tk);
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
static Node * parse_expr(Scope *s);
static int try_calc_int_expr(Node *e, int64_t *val);

static Type * parse_array_declarator(Scope *s, Type *base) {
    expect_tk(s->pp, '[');
    Node *len = NULL;
    if (!next_tk_is(s->pp, ']')) {
        Node *n = parse_expr(s);
        int64_t i;
        if (!try_calc_int_expr(n, &i)) {
            len = n; // VLA
        } else if (i < 0) {
            error_at(n->tk, "cannot have array with negative size ('" PRId64 "')", i);
        } else {
            len = node(N_IMM, n->tk);
            len->t = t_num(T_LLONG, 1);
            len->imm = i;
        }
        expect_tk(s->pp, ']');
    }
    Token *err = peek_tk(s->pp);
    Type *t = parse_declarator_tail(s, base, NULL);
    if (t->k == T_FN) {
        error_at(err, "cannot have array of functions");
    }
    if (is_incomplete(t)) {
        error_at(err, "cannot have array of elements with incomplete type");
    }
    return t_arr(t, len);
}

static Type * parse_fn_declarator_param(Scope *s, Token **name) {
    Token *err = peek_tk(s->pp);
    Type *base = t_num(T_INT, 0); // Parameter types default to 'int'
    if (is_type(s, peek_tk(s->pp))) {
        base = parse_decl_specs(s, NULL);
    }
    Type *t = parse_declarator(s, base, name, NULL);
    if (t->k == T_ARR) { // Array of T is adjusted to pointer to T
        t = t_ptr(t->elem);
    } else if (t->k == T_FN) { // Function is adjusted to pointer to function
        t = t_ptr(t);
    }
    if (t->k == T_VOID) {
        error_at(err, "parameter cannot have type 'void'");
    }
    return t;
}

static Type * parse_fn_declarator(Scope *s, Type *ret, Vec *param_names) {
    if (ret->k == T_FN) {
        error_at(peek_tk(s->pp), "function cannot return a function");
    } else if (ret->k == T_ARR) {
        error_at(peek_tk(s->pp), "function cannot return an array");
    }
    expect_tk(s->pp, '(');
    if (peek_tk_is(s->pp, TK_VOID) && peek2_tk_is(s->pp, ')')) {
        next_tk(s->pp); next_tk(s->pp); // 'void' ')'
        return t_fn(ret, vec_new());
    }
    Vec *param_types = vec_new();
    while (!peek_tk_is(s->pp, ')') && !peek_tk_is(s->pp, TK_EOF)) {
        Token *name;
        Type *param = parse_fn_declarator_param(s, &name);
        vec_push(param_types, param);
        if (param_names) {
            vec_push(param_names, name); // Name may be NULL
        }
        if (!next_tk_is(s->pp, ',')) {
            break;
        }
    }
    expect_tk(s->pp, ')');
    return t_fn(ret, param_types);
}

static Type * parse_declarator_tail(Scope *s, Type *base, Vec *param_names) {
    if (peek_tk_is(s->pp, '[')) {
        return parse_array_declarator(s, base);
    } else if (peek_tk_is(s->pp, '(')) {
        return parse_fn_declarator(s, base, param_names);
    } else {
        return base;
    }
}

static void skip_type_quals(Scope *s) {
    while (next_tk_is(s->pp, TK_CONST) ||
           next_tk_is(s->pp, TK_RESTRICT) ||
           next_tk_is(s->pp, TK_VOLATILE));
}

static Type * parse_declarator(Scope *s, Type *base, Token **name, Vec *param_names) {
    if (next_tk_is(s->pp, '*')) {
        skip_type_quals(s);
        return parse_declarator(s, t_ptr(base), name, param_names);
    }
    if (next_tk_is(s->pp, '(')) { // Either sub-declarator or fn parameters
        if (is_type(s, peek_tk(s->pp)) || peek_tk_is(s->pp, ')')) { // Function
            // An empty '()' is a function pointer, not a no-op sub-declarator
            return parse_fn_declarator(s, base, param_names);
        } else { // Sub-declarator
            Type *inner = t_new();
            Type *decl = parse_declarator(s, inner, name, param_names);
            expect_tk(s->pp, ')');
            *inner = *parse_declarator_tail(s, base, param_names);
            return decl;
        }
    }
    Token *t = peek_tk(s->pp);
    if (t->k == TK_IDENT) {
        *name = t;
        next_tk(s->pp);
    }
    return parse_declarator_tail(s, base, param_names);
}

static Type * parse_named_declarator(Scope *s, Type *base, Token **name, Vec *param_names) {
    Token *name_copy = NULL;
    Token *err = peek_tk(s->pp);
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
    Token *tk = peek_tk(s->pp);
    switch (tk->k) {
    case TK_NUM:
        n = parse_num(s);
        break;
    case TK_CH:
        next_tk(s->pp);
        n = node(N_IMM, tk);
        n->t = t_num(T_CHAR, 0);
        n->imm = tk->ch;
        break;
    case TK_STR:
        n = parse_str(s);
        break;
    case TK_IDENT:
        next_tk(s->pp);
        n = find_var(s, tk->ident);
        if (!n) error_at(tk, "undeclared identifier '%s'", tk->ident);
        break;
    case '(':
        next_tk(s->pp);
        n = parse_subexpr(s, PREC_MIN);
        expect_tk(s->pp, ')');
        break;
    default: error_at(tk, "expected expression");
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
    if (n->k != N_LOCAL && n->k != N_GLOBAL &&
            n->k != N_DEREF && n->k != N_IDX && n->k != N_FIELD)
        error_at(n->tk, "expression is not assignable");
    if (n->t->k == T_ARR)  error_at(n->tk, "array type is not assignable");
    if (n->t->k == T_FN)   error_at(n->tk, "function type is not assignable");
    if (n->t->k == T_VOID) error_at(n->tk, "'void' type is not assignable");
}

static int is_null_ptr(Node *n) {
    while (n->k == N_CONV) n = n->l; // Skip over the conversions
    return n->k == N_IMM && n->imm == 0;
}

static Node * parse_array_access(Scope *s, Node *l) {
    Token *op = expect_tk(s->pp, '[');
    l = discharge(l);
    if (l->t->k != T_PTR) {
        error_at(op, "expected pointer or array type");
    }
    Node *idx = parse_subexpr(s, PREC_MIN);
    ensure_int(idx);
    idx = conv_to(idx, t_num(T_LLONG, 1));
    expect_tk(s->pp, ']');
    Node *n = node(N_IDX, op);
    n->t = l->t->elem;
    n->arr = l;
    n->idx = idx;
    return n;
}

static Node * parse_call(Scope *s, Node *l) {
    Token *op = expect_tk(s->pp, '(');
    l = discharge(l);
    if (l->t->k != T_PTR || l->t->ptr->k != T_FN) {
        error_at(l->tk, "expected function type");
    }
    Type *fn_t = l->t->ptr;
    Vec *args = vec_new();
    while (!peek_tk_is(s->pp, ')') && !peek_tk_is(s->pp, TK_EOF)) {
        Node *arg = parse_subexpr(s, PREC_COMMA);
        arg = discharge(arg);
        if (vec_len(args) >= vec_len(fn_t->params)) {
            error_at(arg->tk, "too many arguments to function call");
        }
        Type *expected = vec_get(fn_t->params, vec_len(args));
        arg = conv_to(arg, expected);
        vec_push(args, arg);
        if (!next_tk_is(s->pp, ',')) {
            break;
        }
    }
    if (vec_len(args) < vec_len(fn_t->params)) {
        error_at(peek_tk(s->pp), "too few arguments to function call");
    }
    expect_tk(s->pp, ')');
    Node *n = node(N_CALL, op);
    n->t = fn_t->ret;
    n->fn = l;
    n->args = args;
    return n;
}

static Node * parse_struct_field_access(Scope *s, Node *l) {
    Token *op = next_tk(s->pp);
    if (l->t->k != T_STRUCT && l->t->k != T_UNION) {
        error_at(op, "expected struct or union type");
    }
    Token *name = expect_tk(s->pp, TK_IDENT);
    size_t f_idx = find_field(l->t, name->ident);
    if (f_idx == NOT_FOUND) {
        error_at(name, "no field named '%s' in %s", name->ident,
                 l->t->k == T_STRUCT ? "struct" : "union");
    }
    Field *f = vec_get(l->t->fields, f_idx);
    Node *n = node(N_FIELD, op);
    n->t = f->t;
    n->strct = l;
    n->field_name = name->ident;
    return n;
}

static Node * parse_struct_field_deref(Scope *s, Node *l) {
    ensure_ptr(l);
    Node *n = node(N_DEREF, peek_tk(s->pp));
    n->t = l->t->ptr;
    n->l = l;
    return parse_struct_field_access(s, n);
}

static Node * parse_post_inc_dec(Scope *s, Node *l) {
    Token *op = next_tk(s->pp);
    ensure_lvalue(l);
    l = discharge(l);
    Node *n = node(op->k == TK_INC ? N_POST_INC : N_POST_DEC, op);
    n->t = l->t;
    n->l = l;
    return n;
}

static Node * parse_postfix(Scope *s, Node *l) {
    while (1) {
        switch (peek_tk(s->pp)->k) {
        case '[': l = parse_array_access(s, l); break;
        case '(': l = parse_call(s, l); break;
        case '.': l = parse_struct_field_access(s, l); break;
        case TK_ARROW: l = parse_struct_field_deref(s, l); break;
        case TK_INC: case TK_DEC: l = parse_post_inc_dec(s, l); break;
        default: return l;
        }
    }
}

static Node * parse_neg(Scope *s) {
    Token *op = expect_tk(s->pp, '-');
    Node *l = parse_subexpr(s, PREC_UNARY);
    ensure_arith(l);
    l = discharge(l);
    Node *unop = node(N_NEG, op);
    unop->t = l->t;
    unop->l = l;
    return unop;
}

static Node * parse_plus(Scope *s) {
    expect_tk(s->pp, '+');
    Node *l = parse_subexpr(s, PREC_UNARY);
    return discharge(l); // Type promotion
}

static Node * parse_bit_not(Scope *s) {
    Token *op = expect_tk(s->pp, '~');
    Node *l = parse_subexpr(s, PREC_UNARY);
    ensure_int(l);
    l = discharge(l);
    Node *unop = node(N_BIT_NOT, op);
    unop->t = l->t;
    unop->l = l;
    return unop;
}

static Node * parse_log_not(Scope *s) {
    Token *op = expect_tk(s->pp, '!');
    Node *l = parse_subexpr(s, PREC_UNARY);
    l = discharge(l);
    Node *unop = node(N_LOG_NOT, op);
    unop->t = t_num(T_INT, 0);
    unop->l = l;
    return unop;
}

static Node * parse_pre_inc_dec(Scope *s) {
    Token *op = next_tk(s->pp);
    Node *l = parse_subexpr(s, PREC_UNARY);
    ensure_lvalue(l);
    l = discharge(l);
    Node *unop = node(op->k == TK_INC ? N_PRE_INC : N_PRE_DEC, op);
    unop->t = l->t;
    unop->l = l;
    return unop;
}

static Node * parse_deref(Scope *s) {
    Token *op = expect_tk(s->pp, '*');
    Node *l = parse_subexpr(s, PREC_UNARY);
    l = discharge(l);
    ensure_ptr(l);
    if (l->t->ptr->k == T_FN) return l; // Don't dereference fn ptrs
    Node *unop = node(N_DEREF, op);
    unop->t = l->t->ptr;
    unop->l = l;
    return unop;
}

static Node * parse_addr(Scope *s) {
    Token *op = expect_tk(s->pp, '&');
    Node *l = parse_subexpr(s, PREC_UNARY);
    ensure_lvalue(l);
    Node *unop = node(N_ADDR, op);
    unop->t = t_ptr(l->t);
    unop->l = l;
    return unop;
}

static Node * parse_sizeof(Scope *s) {
    Token *op = expect_tk(s->pp, TK_SIZEOF);
    Type *t;
    if (peek_tk_is(s->pp, '(') && is_type(s, peek2_tk(s->pp))) {
        next_tk(s->pp);
        t = parse_decl_specs(s, NULL);
        t = parse_abstract_declarator(s, t);
        expect_tk(s->pp, ')');
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
    expect_tk(s->pp, '(');
    Type *t = parse_decl_specs(s, NULL);
    t = parse_abstract_declarator(s, t);
    expect_tk(s->pp, ')');
    if (peek_tk_is(s->pp, '{')) { // Compound literal
        return parse_decl_init(s, t);
    } else {
        Node *l = parse_subexpr(s, PREC_UNARY);
        return conv_to(l, t);
    }
}

static Node * parse_unop(Scope *s) {
    switch (peek_tk(s->pp)->k) {
    case '-': return parse_neg(s);
    case '+': return parse_plus(s);
    case '~': return parse_bit_not(s);
    case '!': return parse_log_not(s);
    case TK_INC: case TK_DEC: return parse_pre_inc_dec(s);
    case '*': return parse_deref(s);
    case '&': return parse_addr(s);
    case TK_SIZEOF: return parse_sizeof(s);
    case '(':
        if (is_type(s, peek2_tk(s->pp))) {
            return parse_cast(s);
        } // Fall through otherwise...
    default: break;
    }
    Node *l = parse_operand(s);
    return parse_postfix(s, l);
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
        expect_tk(s->pp, ':');
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
    while (BINOP_PREC[peek_tk(s->pp)->k] > min_prec) {
        Token *op = next_tk(s->pp);
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

#define CALC_UNOP_INT(op)           \
    l = eval_const_expr(e->l, err); \
    if (!l) goto err;               \
    if (l->k == N_IMM) {            \
        n->k = N_IMM;               \
        n->imm = op l->imm;         \
    } else {                        \
        goto err;                   \
    }                               \
    break;

#define CALC_UNOP_ARITH(op)         \
    l = eval_const_expr(e->l, err); \
    if (!l) goto err;               \
    if (l->k == N_IMM) {            \
        n->k = N_IMM;               \
        n->imm = op l->imm;         \
    } else if (l->k == N_FP) {      \
        n->k = N_FP;                \
        n->fp = op l->fp;           \
    } else {                        \
        goto err;                   \
    }                               \
    break;

#define CALC_BINOP_INT(op)                \
    l = eval_const_expr(e->l, err);       \
    if (!l) goto err;                     \
    r = eval_const_expr(e->r, err);       \
    if (!r) goto err;                     \
    if (l->k == N_IMM && r->k == N_IMM) { \
        n->k = N_IMM;                     \
        n->imm = l->imm op r->imm;        \
    } else {                              \
        goto err;                         \
    }                                     \
    break;

#define CALC_BINOP_ARITH(op)                   \
    l = eval_const_expr(e->l, err);            \
    if (!l) goto err;                          \
    r = eval_const_expr(e->r, err);            \
    if (!r) goto err;                          \
    if (l->k == N_IMM && r->k == N_IMM) {      \
        n->k = N_IMM;                          \
        n->imm = l->imm op r->imm;             \
    } else if (l->k == N_FP && r->k == N_FP) { \
        n->k = N_FP;                           \
        n->fp = l->fp op r->fp;                \
    } else {                                   \
        goto err;                              \
    }                                          \
    break;

#define CALC_BINOP_ARITH_PTR(op)                                       \
    l = eval_const_expr(e->l, err);                                    \
    if (!l) goto err;                                                  \
    r = eval_const_expr(e->r, err);                                    \
    if (!r) goto err;                                                  \
    if (l->k == N_IMM && r->k == N_IMM) {                              \
        n->k = N_IMM;                                                  \
        n->imm = l->imm op r->imm;                                     \
    } else if (l->k == N_FP && r->k == N_FP) {                         \
        n->k = N_FP;                                                   \
        n->fp = l->fp op r->fp;                                        \
    } else if (l->k == N_KPTR && r->k == N_IMM) {                      \
        n->k = N_KPTR;                                                 \
        n->global = l->global;                                         \
        n->kptr_offset = l->kptr_offset op (r->imm * e->t->ptr->size); \
    } else if (l->k == N_IMM && r->k == N_KPTR) {                      \
        n->k = N_KPTR;                                                 \
        n->global = r->global;                                         \
        n->kptr_offset = r->kptr_offset op (l->imm * e->t->ptr->size); \
    } else {                                                           \
        goto err;                                                      \
    }                                                                  \
    break;

#define CALC_BINOP_EQ(op)                                          \
    l = eval_const_expr(e->l, err);                                \
    if (!l) goto err;                                              \
    r = eval_const_expr(e->r, err);                                \
    if (!r) goto err;                                              \
    if (l->k == N_IMM && r->k == N_IMM) {                          \
        n->k = N_IMM;                                              \
        n->imm = op (l->imm == r->imm);                            \
    } else if (l->k == N_FP && r->k == N_FP) {                     \
        n->k = N_IMM;                                              \
        n->imm = op (l->fp == r->fp);                              \
    } else if (l->k == N_KPTR && r->k == N_KPTR) {                 \
        n->k = N_IMM;                                              \
        n->imm = op (l->global == r->global &&                     \
                     l->kptr_offset == r->kptr_offset);            \
    } else if ((l->k == N_KPTR && r->k == N_IMM && r->imm == 0) || \
               (r->k == N_KPTR && l->k == N_IMM && l->imm == 0)) { \
        n->k = N_IMM;                                              \
        n->imm = op 0;                                             \
    } else {                                                       \
        goto err;                                                  \
    }                                                              \
    break;

#define CALC_BINOP_REL(op)                     \
    l = eval_const_expr(e->l, err);            \
    if (!l) goto err;                          \
    r = eval_const_expr(e->r, err);            \
    if (!r) goto err;                          \
    if (l->k == N_IMM && r->k == N_IMM) {      \
        n->k = N_IMM;                          \
        n->imm = l->imm op r->imm;             \
    } else if (l->k == N_FP && r->k == N_FP) { \
        n->k = N_IMM;                          \
        n->imm = l->fp op r->fp;               \
    } else {                                   \
        goto err;                              \
    }                                          \
    break;

static Node * eval_const_expr(Node *e, Token **err) {
    Node *cond, *l, *r;
    Node *n = node(e->k, e->tk);
    n->t = e->t;
    switch (e->k) {
        // Constants
    case N_IMM: case N_FP: case N_KPTR: *n = *e; break;
    case N_ARR:
        n->inits = vec_new();
        for (size_t i = 0; i < vec_len(e->inits); i++) {
            Node *v = vec_get(e->inits, i);
            assert(v->k == N_INIT);
            Node *k = eval_const_expr(v->init_val, err);
            if (!k) goto err;
            if (k->k == N_KVAL) goto err;
            Node *init = node(N_INIT, v->tk);
            init->init_offset = v->init_offset;
            init->init_val = k;
            vec_push(n->inits, init);
        }
        break;
    case N_GLOBAL:
        n->k = N_KVAL;
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
        l = eval_const_expr(e->l, err);
        if (!l) goto err;
        r = eval_const_expr(e->r, err);
        if (!r) goto err;
        n = r;
        break;

        // Ternary operation
    case N_TERNARY:
        cond = eval_const_expr(e->if_cond, err);
        if (!cond) goto err;
        if (cond->k != N_IMM) goto err;
        l = eval_const_expr(e->if_body, err);
        if (!l) goto err;
        r = eval_const_expr(e->if_else, err);
        if (!r) goto err;
        n = cond->imm ? l : r;
        break;

        // Unary operations
    case N_NEG:     CALC_UNOP_ARITH(-)
    case N_BIT_NOT: CALC_UNOP_INT(~)
    case N_LOG_NOT:
        CALC_UNOP_INT(!)
    case N_ADDR:
        l = eval_const_expr(e->l, err);
        if (!l) goto err;
        if (l->k != N_KVAL) goto err;
        n = l;
        n->k = N_KPTR;
        break;
    case N_DEREF:
        l = eval_const_expr(e->l, err);
        if (!l) goto err;
        if (l->k != N_KPTR) goto err;
        n = l;
        n->k = N_KVAL;
        break;
    case N_CONV:
        l = eval_const_expr(e->l, err);
        if (!l) goto err;
        if (is_fp(n->t) && l->k == N_IMM) { // int -> float
            n->k = N_FP;
            n->fp = (double) l->imm;
        } else if (is_int(n->t) && l->k == N_FP) { // float -> int
            n->k = N_IMM;
            n->imm = (int64_t) l->fp;
        } else if (is_int(n->t) && l->k == N_IMM) { // int -> int
            n->k = N_IMM;
            size_t b = n->t->size * 8; // Bits
            n->imm = l->imm & (((int64_t) 1 << b) - 1); // Truncate
            if (!l->t->is_unsigned && (n->imm & ((int64_t) 1 << (b - 1)))) {
                n->imm |= ~(((int64_t) 1 << b) - 1); // Sign extend
            }
        } else { // Direct conversion
            *n = *l;
            n->t = e->t;
        }
        break;

        // Postfix operations
    case N_IDX:
        l = eval_const_expr(e->arr, err);
        if (!l) goto err;
        if (l->k != N_KVAL && l->k != N_KPTR) goto err;
        r = eval_const_expr(e->idx, err);
        if (!r) goto err;
        if (r->k != N_IMM) goto err;
        n = l;
        n->kptr_offset += (int64_t) (r->imm * n->t->size);
        break;
    case N_FIELD:
        l = eval_const_expr(e->strct, err);
        if (!l) goto err;
        if (l->k != N_KVAL) goto err;
        size_t f_idx = find_field(l->t, e->field_name);
        assert(f_idx != NOT_FOUND);
        Field *f = vec_get(l->t->fields, f_idx);
        e = l;
        e->kptr_offset += (int64_t) f->offset;
        break;
    default: goto err;
    }
    return n;
err:
    if (err && !*err) *err = e->tk;
    return NULL;
}

static Node * calc_const_expr(Node *e) {
    Token *err;
    Node *n = eval_const_expr(e, &err);
    if (!n) {
        error_at(err, "expected constant expression");
    }
    if (n->k == N_KVAL) {
        error_at(n->tk, "expected constant expression");
    }
    return n;
}

static int64_t calc_int_expr(Node *e) {
    Node *n = calc_const_expr(e);
    if (n->k != N_IMM) {
        error_at(n->tk, "expected constant integer expression");
    }
    return (int64_t) n->imm;
}

static int try_calc_int_expr(Node *e, int64_t *val) {
    Node *n = eval_const_expr(e, NULL);
    if (n && n->k != N_IMM) {
        error_at(n->tk, "expected constant integer expression");
    }
    if (n) {
        *val = (int64_t) n->imm;
    }
    return n != NULL;
}

int64_t parse_const_int_expr(PP *pp) {
    Scope s = new_scope(SCOPE_FILE, pp);
    Node *e = parse_expr(&s);
    return calc_int_expr(e);
}


// ---- Statements ------------------------------------------------------------

static Node * parse_decl(Scope *s);
static Node * parse_block(Scope *s);
static Node * parse_stmt(Scope *s);

static Node * parse_if(Scope *s) {
    Token *if_tk = expect_tk(s->pp, TK_IF);
    expect_tk(s->pp, '(');
    Node *cond = parse_expr(s);
    expect_tk(s->pp, ')');
    Node *body = parse_stmt(s);
    Node *els = NULL;
    if (peek_tk_is(s->pp, TK_ELSE)) {
        Token *else_tk = next_tk(s->pp);
        if (peek_tk_is(s->pp, TK_IF)) { // else if
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
    Token *while_tk = expect_tk(s->pp, TK_WHILE);
    expect_tk(s->pp, '(');
    Node *cond = parse_expr(s);
    expect_tk(s->pp, ')');
    Scope loop;
    enter_scope(&loop, s, SCOPE_LOOP);
    Node *body = parse_stmt(&loop);
    Node *n = node(N_WHILE, while_tk);
    n->loop_cond = cond;
    n->loop_body = body;
    return n;
}

static Node * parse_do_while(Scope *s) {
    Token *do_tk = expect_tk(s->pp, TK_DO);
    Scope loop;
    enter_scope(&loop, s, SCOPE_LOOP);
    Node *body = parse_stmt(&loop);
    expect_tk(s->pp, TK_WHILE);
    expect_tk(s->pp, '(');
    Node *cond = parse_expr(s);
    expect_tk(s->pp, ')');
    expect_tk(s->pp, ';');
    Node *n = node(N_DO_WHILE, do_tk);
    n->loop_cond = cond;
    n->loop_body = body;
    return n;
}

static Node * parse_for(Scope *s) {
    Token *for_tk = expect_tk(s->pp, TK_FOR);
    expect_tk(s->pp, '(');
    Scope loop;
    enter_scope(&loop, s, SCOPE_LOOP);

    Node *init = NULL;
    if (is_type(&loop, peek_tk(s->pp))) {
        init = parse_decl(&loop);
    } else if (!peek_tk_is(s->pp, ';')) {
        init = parse_expr(&loop);
        expect_tk(s->pp, ';');
    }

    Node *cond = NULL;
    if (!peek_tk_is(s->pp, ';')) {
        cond = parse_expr(&loop);
    }
    expect_tk(s->pp, ';');

    Node *inc = NULL;
    if (!peek_tk_is(s->pp, ')')) {
        inc = parse_expr(&loop);
    }
    expect_tk(s->pp, ')');

    Node *body = parse_stmt(&loop);
    Node *n = node(N_FOR, for_tk);
    n->for_init = init;
    n->for_cond = cond;
    n->for_inc = inc;
    n->for_body = body;
    return n;
}

static Node * parse_switch(Scope *s) {
    Token *switch_tk = expect_tk(s->pp, TK_SWITCH);
    expect_tk(s->pp, '(');
    Node *cond = parse_expr(s);
    expect_tk(s->pp, ')');

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
    Token *case_tk = expect_tk(s->pp, TK_CASE);
    Scope *switch_s = find_scope(s, SCOPE_SWITCH);
    if (!switch_s) {
        error_at(case_tk, "'case' not allowed here");
    }
    Node *cond = parse_expr(s);
    expect_tk(s->pp, ':');
    Node *body = parse_stmt(s);
    Node *n = node(N_CASE, case_tk);
    n->case_cond = cond;
    n->case_body = body;
    vec_push(switch_s->cases, n);
    return n;
}

static Node * parse_default(Scope *s) {
    Token *default_tk = expect_tk(s->pp, TK_DEFAULT);
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
    expect_tk(s->pp, ':');
    Node *body = parse_stmt(s);
    Node *n = node(N_DEFAULT, default_tk);
    n->case_body = body;
    vec_push(switch_s->cases, n);
    return n;
}

static Node * parse_break(Scope *s) {
    Token *break_tk = expect_tk(s->pp, TK_BREAK);
    if (!find_scope(s, SCOPE_LOOP) && !find_scope(s, SCOPE_SWITCH)) {
        error_at(break_tk, "'break' not allowed here");
    }
    expect_tk(s->pp, ';');
    Node *n = node(N_BREAK, break_tk);
    return n;
}

static Node * parse_continue(Scope *s) {
    Token *continue_tk = expect_tk(s->pp, TK_CONTINUE);
    if (!find_scope(s, SCOPE_LOOP)) {
        error_at(continue_tk, "'break' not allowed here");
    }
    expect_tk(s->pp, ';');
    Node *n = node(N_CONTINUE, continue_tk);
    return n;
}

static Node * parse_goto(Scope *s) {
    Token *goto_tk = expect_tk(s->pp, TK_GOTO);
    Token *label = expect_tk(s->pp, TK_IDENT);
    expect_tk(s->pp, ';');
    Node *n = node(N_GOTO, goto_tk);
    n->label = label->ident;
    return n;
}

static Node * parse_label(Scope *s) {
    Token *label = expect_tk(s->pp, TK_IDENT);
    expect_tk(s->pp, ':');
    Node *body = parse_stmt(s);
    Node *n = node(N_LABEL, label);
    n->label = label->ident;
    n->label_body = body;
    return n;
}

static Node * parse_ret(Scope *s) {
    Token *ret_tk = expect_tk(s->pp, TK_RETURN);
    Node *val = NULL;
    if (!peek_tk_is(s->pp, ';')) {
        if (s->fn->t->ret->k == T_VOID) {
            error_at(peek_tk(s->pp), "cannot return value from void function");
        }
        val = parse_expr(s);
        val = conv_to(val, s->fn->t->ret);
    }
    expect_tk(s->pp, ';');
    Node *n = node(N_RET, ret_tk);
    n->ret_val = val;
    return n;
}

static Node * parse_expr_stmt(Scope *s) {
    Node *n = parse_expr(s);
    expect_tk(s->pp, ';');
    return n;
}

static Node * parse_stmt(Scope *s) {
    Token *t = peek_tk(s->pp);
    switch (t->k) {
    case ';':         next_tk(s->pp); return NULL;
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
        if (peek2_tk_is(s->pp, ':')) {
            return parse_label(s);
        } // Fall through...
    default: return parse_expr_stmt(s);
    }
}

static Node * parse_stmt_or_decl(Scope *s) {
    if (is_type(s, peek_tk(s->pp))) {
        return parse_decl(s);
    } else {
        return parse_stmt(s);
    }
}

static Node * parse_block(Scope *s) {
    expect_tk(s->pp, '{');
    Scope block;
    enter_scope(&block, s, SCOPE_BLOCK);
    Node *head = NULL;
    Node **cur = &head;
    while (!peek_tk_is(s->pp, '}') && !peek_tk_is(s->pp, TK_EOF)) {
        *cur = parse_stmt_or_decl(&block);
        while (*cur) cur = &(*cur)->next;
    }
    expect_tk(s->pp, '}');
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
    fn->fn_name = name->ident;
    fn->param_names = param_names;
    Scope fn_scope;
    enter_scope(&fn_scope, s, SCOPE_BLOCK);
    fn->fn_body = parse_block(&fn_scope);
    return fn;
}

static void parse_string_init(Scope *s, Vec *inits, Type *t, size_t offset) {
    assert(is_string_type(t));
    assert(!is_vla(t));
    Node *str;
    if (peek_tk_is(s->pp, TK_STR)) {
        str = parse_str(s);
    } else if (peek_tk_is(s->pp, '{') && peek2_tk_is(s->pp, TK_STR)) {
        next_tk(s->pp); // Skip '{'
        str = parse_str(s);
        expect_tk(s->pp, '}');
    } else {
        return; // Parse as normal array
    }
    if (!are_equal(t->elem, str->t->elem)) {
        warning_at(str->tk, "initializing string with literal of different type");
    }
    if (!t->len) {
        t->len = str->t->len;
        t->size = t->len->imm * t->elem->size;
    }
    if (t->len->imm < str->len) {
        warning_at(str->tk, "initializer string is too long");
    }
    for (size_t i = 0; i < t->len->imm || i < str->len; i++) {
        Node *ch = node(N_IMM, str->tk);
        ch->t = t->elem;
        if (i < str->len) {
            switch (str->enc) {
                case ENC_NONE: ch->imm = (uint64_t) str->str[i]; break;
                case ENC_CHAR16: ch->imm = str->str16[i]; break;
                case ENC_CHAR32: case ENC_WCHAR: ch->imm = str->str32[i]; break;
            }
        } else {
            ch->imm = 0;
        }
        Node *n = node(N_INIT, str->tk);
        n->init_offset = offset + i * t->elem->size;
        n->init_val = ch;
        vec_push(inits, n);
    }
}

static void parse_init_list_raw(Scope *s, Vec *inits, Type *t, size_t offset, int designated);

static void parse_init_elem(Scope *s, Vec *inits, Type *t, size_t offset, int designated) {
    if (t->k == T_ARR || t->k == T_STRUCT || t->k == T_UNION || peek_tk_is(s->pp, '{')) {
        parse_init_list_raw(s, inits, t, offset, designated);
    } else {
        Node *e = parse_expr_no_commas(s);
        e = conv_to(e, t);
        Node *n = node(N_INIT, e->tk);
        n->init_offset = offset;
        n->init_val = e;
        vec_push(inits, n);
    }
    if (!peek_tk_is(s->pp, '}')) {
        expect_tk(s->pp, ',');
    }
}

static size_t parse_array_designator(Scope *s, Type *t) {
    expect_tk(s->pp, '[');
    Node *e = parse_expr(s);
    int64_t desg = calc_int_expr(e);
    if (desg < 0 || (t->len && (uint64_t) desg >= t->len->imm)) {
        error_at(e->tk, "array designator index '" PRId64 "' exceeds array bounds", desg);
    }
    expect_tk(s->pp, ']');
    expect_tk(s->pp, '=');
    return desg;
}

static void parse_array_init(Scope *s, Vec *inits, Type *t, size_t offset, int designated) {
    assert(t->k == T_ARR);
    assert(!is_vla(t));
    int has_brace = next_tk_is(s->pp, '{') != NULL;
    size_t idx = 0;
    while (!peek_tk_is(s->pp, '}') && !peek_tk_is(s->pp, TK_EOF)) {
        if (!has_brace && t->len && idx >= t->len->imm) {
            break;
        }
        if (peek_tk_is(s->pp, '[') && !has_brace && !designated) {
            break; // e.g., int a[3][3] = {3 /* RETURN HERE */, [2] = 1}
        }
        if (peek_tk_is(s->pp, '[')) {
            idx = parse_array_designator(s, t);
            designated = 1;
        }
        int excess = t->len && idx >= t->len->imm;
        if (excess) {
            warning_at(peek_tk(s->pp), "excess elements in array initializer");
        }
        size_t elem_offset = offset + idx * t->elem->size;
        parse_init_elem(s, excess ? NULL : inits, t->elem, elem_offset, designated);
        idx++;
        designated = 0;
    }
    if (has_brace) {
        expect_tk(s->pp, '}');
    }
    if (!t->len) {
        t->len = node(N_IMM, NULL);
        t->len->t = t_num(T_LLONG, 1);
        t->len->imm = idx;
        t->size = t->len->imm * t->elem->size;
    }
}

static size_t parse_struct_designator(Scope *s, Type *t) {
    expect_tk(s->pp, '.');
    Token *name = expect_tk(s->pp, TK_IDENT);
    size_t f_idx = find_field(t, name->ident);
    if (f_idx == NOT_FOUND) {
        error_at(name, "designator '%s' does not refer to any field in the %s",
                 name->ident, t->k == T_STRUCT ? "struct" : "union");
    }
    expect_tk(s->pp, '=');
    return f_idx;
}

static void parse_struct_init(Scope *s, Vec *inits, Type *t, size_t offset, int designated) {
    assert(t->k == T_STRUCT || t->k == T_UNION);
    int has_brace = next_tk_is(s->pp, '{') != NULL;
    size_t idx = 0;
    while (!peek_tk_is(s->pp, '}') && !peek_tk_is(s->pp, TK_EOF)) {
        if (!has_brace && idx >= vec_len(t->fields)) {
            break;
        }
        if (peek_tk_is(s->pp, '.') && !has_brace && !designated) {
            break;
        }
        if (peek_tk_is(s->pp, '.')) {
            idx = parse_struct_designator(s, t);
            designated = 1;
        }
        int excess = idx >= vec_len(t->fields);
        if (excess) {
            warning_at(peek_tk(s->pp), "excess elements in %s initializer",
                       t->k == T_STRUCT ? "struct" : "union");
        }
        Field *f = excess ? vec_last(t->fields) : vec_get(t->fields, idx);
        size_t field_offset = offset + f->offset;
        parse_init_elem(s, excess ? NULL : inits, f->t, field_offset, designated);
        idx++;
        designated = 0;
    }
    if (has_brace) {
        expect_tk(s->pp, '}');
    }
}

static void parse_init_list_raw(Scope *s, Vec *inits, Type *t, size_t offset, int designated) {
    if (is_string_type(t)) {
        parse_string_init(s, inits, t, offset);
        return;
    } // Fall through if no string literal...
    if (t->k == T_ARR) {
        parse_array_init(s, inits, t, offset, designated);
    } else if (t->k == T_STRUCT || t->k == T_UNION) {
        parse_struct_init(s, inits, t, offset, designated);
    } else { // Everything else, e.g., int a = {3}
        Node *len = node(N_IMM, peek_tk(s->pp));
        len->t = t_num(T_LLONG, 1);
        len->imm = 1;
        Type *arr_t = t_arr(t, len);
        parse_array_init(s, inits, arr_t, offset, designated);
    }
}

static Node * parse_init_list(Scope *s, Type *t) {
    Node *n = node(N_ARR, peek_tk(s->pp));
    n->t = t;
    n->inits = vec_new();
    parse_init_list_raw(s, n->inits, t, 0, 0);
    return n;
}

static Node * parse_decl_init(Scope *s, Type *t) {
    if (t->linkage == L_EXTERN || t->k == T_FN) {
        error_at(peek_tk(s->pp), "illegal initializer");
    }
    if (is_vla(t)) {
        error_at(peek_tk(s->pp), "cannot initialize variable-length array");
    }
    Node *init;
    if (peek_tk_is(s->pp, '{') || is_string_type(t)) {
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
    if (next_tk_is(s->pp, '=')) {
        init = parse_decl_init(s, t);
    }
    if (is_incomplete(t)) {
        // Check this after parsing the initializer because arrays without a
        // specified length may have their type completed
        // e.g., int a[] /* INCOMPLETE HERE */ = {1, 2, 3}; /* COMPLETE HERE */
        error_at(name, "variable cannot have incomplete type");
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
    if (s->k == SCOPE_FILE && peek_tk_is(s->pp, '{')) {
        return parse_fn_def(s, t, name, param_names);
    }
    return parse_decl_var(s, t, name);
}

static Node * parse_decl(Scope *s) {
    int sclass;
    Type *base = parse_decl_specs(s, &sclass);
    if (next_tk_is(s->pp, ';')) {
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
        if (!next_tk_is(s->pp, ',')) {
            break;
        }
    }
    expect_tk(s->pp, ';');
    return head;
}

Node * parse(File *f) {
    Lexer *l = new_lexer(f);
    PP *pp = new_pp(l);
    Scope file_scope = new_scope(SCOPE_FILE, pp);
    Node *head = NULL;
    Node **cur = &head;
    while (!next_tk_is(pp, TK_EOF)) {
        *cur = parse_decl(&file_scope);
        while (*cur) cur = &(*cur)->next;
    }
    return head;
}
