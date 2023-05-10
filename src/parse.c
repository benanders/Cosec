
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <inttypes.h>

#include "parse.h"
#include "pp.h"
#include "error.h"

typedef enum {
    SCOPE_FILE,
    SCOPE_BLOCK,
    SCOPE_LOOP,
    SCOPE_SWITCH,
} ScopeT;

typedef struct Scope {
    struct Scope *outer;
    ScopeT k;
    PP *pp;
    Map *vars;  // of 'Node *' with k = N_LOCAL, N_GLOBAL, or N_TYPEDEF
    Map *tags;  // of 'Type *'
    AstNode *fn;   // NULL in file scope

    // For SCOPE_SWITCH
    Vec *cases;   // of 'Node *' with k = N_CASE or N_DEFAULT
    AstType *cond_t; // Type of the condition
} Scope;

static AstNode * node(AstNodeT k, Token *tk) {
    AstNode *n = calloc(1, sizeof(AstNode));
    n->k = k;
    n->tk = tk;
    return n;
}


// ---- Scope -----------------------------------------------------------------

static Scope new_scope(ScopeT k, PP *pp) {
    Scope s = {0};
    s.k = k;
    s.pp = pp;
    s.vars = map_new();
    s.tags = map_new();
    return s;
}

static void enter_scope(Scope *inner, Scope *outer, ScopeT k) {
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

static Scope * find_scope(Scope *s, ScopeT k) {
    while (s && s->k != k) {
        s = s->outer;
    }
    return s;
}


// ---- Types -----------------------------------------------------------------

#define NOT_FOUND ((size_t) -1)

static AstType * t_new(AstTypeT k) {
    AstType *t = calloc(1, sizeof(AstType));
    t->k = k;
    switch (t->k) {
    case T_CHAR:  t->size = t->align = 1; break;
    case T_SHORT: t->size = t->align = 2; break;
    case T_INT: case T_LONG: case T_FLOAT: t->size = t->align = 4; break;
    case T_LLONG: case T_DOUBLE: case T_LDOUBLE: case T_PTR: case T_FN:
        t->size = t->align = 8;
        break;
    case T_ARR: t->align = 8; break;
    default: break;
    }
    return t;
}

static AstType * t_num(AstTypeT k, int is_unsigned) {
    AstType *t = t_new(k);
    t->is_unsigned = is_unsigned;
    return t;
}

static AstType * t_ptr(AstType *base) {
    AstType *t = t_new(T_PTR);
    t->ptr = base;
    return t;
}

static void set_arr_len(AstType *t, AstNode *len) {
    t->len = len;
    if (len && len->k == N_IMM) {
        t->size = t->elem->size * len->imm;
    }
}

static AstType * t_arr(AstType *elem, AstNode *len) {
    AstType *t = t_new(T_ARR);
    t->elem = elem;
    set_arr_len(t, len);
    return t;
}

static AstType * t_fn(AstType *ret, Vec *params, int is_vararg) {
    AstType *t = t_new(T_FN);
    t->ret = ret;
    t->params = params;
    t->is_vararg = is_vararg;
    return t;
}

static void set_struct_fields(AstType *t, Vec *fields) {
    t->fields = fields;
    for (size_t i = 0; i < vec_len(fields); i++) { // Pick largest align
        Field *f = vec_get(fields, i);
        t->size += pad(t->size, f->t->align);
        f->offset = t->size;
        t->size += f->t->size;
        t->align = f->t->align > t->align ? f->t->align : t->align;
    }
}

static void set_union_fields(AstType *t, Vec *fields) {
    t->fields = fields;
    for (size_t i = 0; i < vec_len(fields); i++) { // Pick largest align
        Field *f = vec_get(fields, i);
        f->offset = 0;
        if (f->t->size > t->size) {
            t->size = f->t->size;
            t->align = f->t->align;
        }
    }
}

static void set_enum_consts(AstType *t, Vec *consts, AstType *num_t) {
    t->consts = consts;
    t->num_t = num_t;
    t->size = num_t->size;
    t->align = num_t->align;
}

static Field * new_field(AstType *t, char *name) {
    Field *f = malloc(sizeof(Field));
    f->t = t;
    f->name = name;
    f->offset = 0;
    return f;
}

static EnumConst * new_enum_const(char *name, uint64_t val) {
    EnumConst *k = malloc(sizeof(EnumConst));
    k->name = name;
    k->val = val;
    return k;
}

static size_t find_field(AstType *t, char *name) {
    assert(t->k == T_STRUCT || t->k == T_UNION);
    if (!t->fields) {
        return NOT_FOUND; // Incomplete type
    }
    for (size_t i = 0; i < vec_len(t->fields); i++) {
        Field *f = vec_get(t->fields, i);
        if (strcmp(f->name, name) == 0) {
            return i;
        }
    }
    return NOT_FOUND;
}

static int is_int(AstType *t) {
    return t->k >= T_CHAR && t->k <= T_LLONG;
}

static int is_fp(AstType *t) {
    return t->k >= T_FLOAT && t->k <= T_LDOUBLE;
}

static int is_num(AstType *t) {
    return is_int(t) || is_fp(t);
}

static int is_void_ptr(AstType *t) {
    return t->k == T_PTR && t->ptr->k == T_VOID;
}

static int is_null_ptr(AstNode *n) {
    while (n->k == N_CONV) n = n->l; // Skip over conversions
    return n->k == N_IMM && n->imm == 0;
}

static int is_string_type(AstType *t) {
    return t->k == T_ARR &&
           ((t->elem->k == T_CHAR && !t->elem->is_unsigned) ||
            (t->elem->k == T_SHORT && t->elem->is_unsigned) ||
            (t->elem->k == T_INT && t->elem->is_unsigned));
}

static int is_vla(AstType *t) {
    while (t->k == T_ARR) {
        if (t->len && t->len->k != N_IMM) {
            return 1;
        }
        t = t->elem;
    }
    return 0;
}

static int is_incomplete(AstType *t) {
    switch (t->k) {
    case T_VOID: return 1;
    case T_ARR:
        if (!t->len) return 1;
        return is_incomplete(t->elem);
    case T_STRUCT: case T_UNION:
        if (!t->fields) return 1;
        for (size_t i = 0; i < vec_len(t->fields); i++) {
            AstType *ft = ((Field *) vec_get(t->fields, i))->t;
            if (is_incomplete(ft)) {
                return 1;
            }
        }
        return 0;
    case T_ENUM: return !t->consts;
    default: return 0;
    }
}

static int are_equal(AstType *a, AstType *b) {
    if (!a && !b) return 1;
    if (!a || !b) return 0;
    if (a->k != b->k) return 0;
    switch (a->k) {
    case T_PTR: return are_equal(a->ptr, b->ptr);
    case T_ARR:
        if (a->len && b->len && a->len->k == N_IMM && b->len->k == N_IMM &&
                a->len->k != b->len->k) {
            return 0;
        }
        return are_equal(a->elem, b->elem);
    case T_FN:
        if (vec_len(a->params) != vec_len(b->params)) return 0;
        for (size_t i = 0; i < vec_len(a->params); i++) {
            AstType *ta = vec_get(a->params, i);
            AstType *tb = vec_get(b->params, i);
            if (!are_equal(ta, tb)) {
                return 0;
            }
        }
        return are_equal(a->ret, b->ret);
    case T_STRUCT: case T_UNION:
        if (!a->fields || !b->fields) return 0;
        if (vec_len(a->fields) != vec_len(b->fields)) return 0;
        for (size_t i = 0; i < vec_len(a->fields); i++) {
            Field *fa = vec_get(a->fields, i);
            Field *fb = vec_get(b->fields, i);
            if (strcmp(fa->name, fb->name) != 0 || !are_equal(fa->t, fb->t)) {
                return 0;
            }
        }
        return 1;
    case T_ENUM:
        if (!a->consts || !b->consts) return 0;
        if (vec_len(a->consts) != vec_len(b->consts)) return 0;
        if (!are_equal(a->num_t, b->num_t)) return 0;
        for (size_t i = 0; i < vec_len(a->consts); i++) {
            EnumConst *ka = vec_get(a->consts, i);
            EnumConst *kb = vec_get(b->consts, i);
            if (strcmp(ka->name, kb->name) != 0 || ka->val != kb->val) {
                return 0;
            }
        }
        return 1;
    default: return a->is_unsigned == b->is_unsigned;
    }
}

static void expect_val(AstNode *n) {
    if (n->t->k == T_STRUCT || n->t->k == T_UNION) {
        error_at(n->tk, "expected pointer or arithmetic type");
    }
}

static void expect_num(AstNode *n) {
    if (!is_num(n->t)) {
        error_at(n->tk, "expected arithmetic type");
    }
}

static void expect_int(AstNode *n) {
    if (!is_int(n->t)) {
        error_at(n->tk, "expected integer type");
    }
}

static void expect_ptr(AstNode *n) {
    if (n->t->k != T_PTR) {
        error_at(n->tk, "expected pointer type");
    }
}

static void expect_lval(AstNode *n) {
    if (n->k != N_LOCAL && n->k != N_GLOBAL &&
            n->k != N_DEREF && n->k != N_IDX && n->k != N_FIELD) {
        error_at(n->tk, "expression is not an lvalue");
    }
    if (n->t->k == T_ARR)  error_at(n->tk, "array type is not an lvalue");
    if (n->t->k == T_VOID) error_at(n->tk, "'void' type is not an lvalue");
}

static void expect_assignable(AstNode *n) {
    expect_lval(n);
    if (n->t->k == T_FN) error_at(n->tk, "function type is not assignable");
}


// ---- Variables, Typedefs, and Tags -----------------------------------------

static AstNode * find_var(Scope *s, char *name) {
    while (s) {
        AstNode *var = map_get(s->vars, name);
        if (var) return var;
        s = s->outer;
    }
    return NULL;
}

static AstType * find_typedef(Scope *s, char *name) {
    AstNode *n = find_var(s, name);
    if (n && n->k == N_TYPEDEF) {
        return n->t;
    }
    return NULL;
}

static AstType * find_tag(Scope *s, char *tag) {
    while (s) {
        AstType *t = map_get(s->tags, tag);
        if (t) return t;
        s = s->outer;
    }
    return NULL;
}

static void def_symbol(Scope *s, AstNode *n) {
    AstNode *v;
    if (n->t->linkage == L_EXTERN) { // extern needs type checking across scopes
        v = find_var(s, n->var_name);
        if (v && !are_equal(n->t, v->t)) goto err_type;
    }
    v = map_get(s->vars, n->var_name);
    if (!v) goto okay; // No previous definition
    if (n->k != v->k) goto err_symbol;
    if (!are_equal(n->t, v->t)) goto err_type;
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
    goto okay;
err_redef:
    error_at(n->tk, "redefinition of '%s'", n->var_name);
err_symbol:
    error_at(n->tk, "redefinition of '%s' as a different kind of symbol", n->var_name);
err_type:
    error_at(n->tk, "redefinition of '%s' with a different type", n->var_name);
err_static1:
    error_at(n->tk, "non-static declaration of '%s' follows static declaration", n->var_name);
err_static2:
    error_at(n->tk, "static declaration of '%s' follows non-static declaration", n->var_name);
okay:
    map_put(s->vars, n->var_name, n);
}

static AstNode * def_var(Scope *s, Token *name, AstType *t) {
    if (t->k == T_FN && s->k != SCOPE_FILE && t->linkage == L_STATIC) {
        error_at(name, "function declared in block scope cannot have 'static' storage class");
    }
    if (t->k == T_FN && t->linkage == L_NONE) {
        t->linkage = L_EXTERN; // Functions are 'extern' if unspecified
    }
    int is_global = s->k == SCOPE_FILE || t->linkage == L_STATIC || t->linkage == L_EXTERN;
    AstNode *n = node(is_global ? N_GLOBAL : N_LOCAL, name);
    n->t = t;
    n->var_name = name->ident;
    def_symbol(s, n);
    return n;
}

static AstNode * def_typedef(Scope *s, Token *name, AstType *t) {
    assert(t->linkage == L_NONE);
    AstNode *n = node(N_TYPEDEF, name);
    n->t = t;
    n->var_name = name->ident;
    def_symbol(s, n);
    return n;
}

static void def_enum_const(Scope *s, Token *name, AstType *t, int64_t val) {
    AstNode *v = map_get(s->vars, name->ident);
    if (v && v->k != N_IMM) {
        error_at(name, "redefinition of '%s' as a different kind of symbol", name->ident);
    } else if (v) {
        error_at(name, "redefinition of enum constant '%s'", name->ident);
    }
    AstNode *n = node(N_IMM, name);
    n->t = t;
    n->imm = val;
    map_put(s->vars, name->ident, n);
}


// ---- Literals --------------------------------------------------------------

static AstType * parse_int_suffix(char *s) {
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

static AstType * smallest_type_for_int(uint64_t num, int signed_only) {
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

static AstNode * parse_int(Token *tk) {
    char *suffix;
    uint64_t num;
    if (strncasecmp(tk->num, "0b", 2) == 0) { // strotoul can't read '0b' prefix
        num = strtoul(tk->num + 2, &suffix, 2);
    } else { // strtoul can read '0x' and '0' prefix
        num = strtoul(tk->num, &suffix, 0);
    }
    AstType *t;
    if (*suffix == '\0') { // No suffix; select type based on how large 'num' is
        int is_base_10 = (*tk->num != '0');
        t = smallest_type_for_int(num, is_base_10);
    } else { // Type specified by suffix
        t = parse_int_suffix(suffix);
        if (!t) {
            error_at(tk, "invalid integer suffix '%s'", suffix);
        }
        size_t bits = t->size * 8;
        uint64_t invalid_bits = bits >= 64 ? 0 : ~((1 << bits) - 1);
        if ((num & invalid_bits) != 0) {
            warning_at(tk, "integer '%s' too large for specified type", tk->num);
        }
    }
    AstNode *n = node(N_IMM, tk);
    n->t = t;
    n->imm = num;
    return n;
}

static AstType * parse_float_suffix(char *s) {
    if (strcasecmp(s, "l") == 0) {
        return t_num(T_LDOUBLE, 0);
    } else if (strcasecmp(s, "f") == 0) {
        return t_num(T_FLOAT, 0);
    } else {
        return NULL;
    }
}

static AstNode * parse_float(Token *tk) {
    char *suffix;
    double num = strtod(tk->num, &suffix);
    AstType *t;
    if (*suffix == '\0') { // No suffix; always a double
        t = t_num(T_DOUBLE, 0);
    } else { // Type specified by suffix
        t = parse_float_suffix(suffix);
        if (!t) {
            error_at(tk, "invalid floating point suffix '%s'", suffix);
        }
    }
    AstNode *n = node(N_FP, tk);
    n->t = t;
    n->fp = num;
    return n;
}

static AstNode * parse_num(Scope *s) {
    Token *tk = expect_tk(s->pp, TK_NUM);
    if (strpbrk(tk->num, ".pP") ||
            (strncasecmp(tk->num, "0x", 2) != 0 && strpbrk(tk->num, "eE"))) {
        return parse_float(tk);
    } else {
        return parse_int(tk);
    }
}

static Token * concat_strs(Scope *s) {
    Buf *buf = buf_new();
    Token *t = peek_tk(s->pp);
    assert(t->k == TK_STR);
    Token *str = copy_tk(t);
    while (t->k == TK_STR) {
        t = next_tk(s->pp);
        if (t->enc > str->enc) {
            str->enc = t->enc;
        }
        buf_nprint(buf, t->str, t->len);
        t = peek_tk(s->pp);
    }
    buf_push(buf, '\0');
    str->str = buf->data;
    str->len = buf->len;
    return str;
}

static AstNode * parse_str(Scope *s) {
    Token *tk = concat_strs(s);
    AstNode *len = node(N_IMM, tk);
    len->t = t_num(T_LLONG, 1);
    AstNode *n = node(N_STR, tk);
    n->enc = tk->enc;
    switch (tk->enc) {
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

static AstNode * parse_ch(Scope *s) {
    Token *tk = next_tk(s->pp);
    AstNode *n = node(N_IMM, tk);
    switch (tk->enc) {
        case ENC_NONE: n->t = t_num(T_CHAR, 0); break;
        case ENC_CHAR16: n->t = t_num(T_SHORT, 1); break;
        case ENC_CHAR32: case ENC_WCHAR: n->t = t_num(T_INT, 1); break;
    }
    n->imm = tk->ch;
    return n;
}


// ---- Declaration Specifiers ------------------------------------------------

static AstType * parse_decl_specs(Scope *s, StorageClass *sclass);
static AstType * parse_declarator(Scope *s, AstType *base, Token **name, Vec *param_names);

static AstNode * parse_expr_no_commas(Scope *s);
static int64_t calc_int_expr(AstNode *e);

static int is_type(Scope *s, Token *t) {
    if (t->k == TK_IDENT) {
        return find_typedef(s, t->ident) != NULL;
    } else {
        return t->k >= TK_VOID && t->k <= TK_VOLATILE;
    }
}

static void parse_aggr_fields(Scope *s, AstType *t) {
    expect_tk(s->pp, '{');
    Vec *fields = vec_new();
    while (!peek_tk_is(s->pp, '}') && !peek_tk_is(s->pp, TK_EOF)) {
        Token *tk = peek_tk(s->pp);
        StorageClass sclass;
        AstType *base = parse_decl_specs(s, &sclass);
        if (sclass != S_NONE) {
            error_at(tk, "illegal storage class specifier in %s field",
                     t->k == T_STRUCT ? "struct" : "union");
        }
        if (peek_tk_is(s->pp, ';')) {
            vec_push(fields, new_field(base, NULL));
        }
        while (!peek_tk_is(s->pp, ';') && !peek_tk_is(s->pp, TK_EOF)) {
            Token *name;
            AstType *ft = parse_declarator(s, base, &name, NULL);
            if (is_incomplete(ft)) {
                error_at(name, "%s field cannot have incomplete type",
                         t->k == T_STRUCT ? "struct" : "union");
            }
            if (is_vla(ft)) {
                error_at(name, "%s field must have constant size",
                         t->k == T_STRUCT ? "struct" : "union");
            }
            if (find_field(t, name->ident) != NOT_FOUND) {
                error_at(name, "duplicate field '%s' in %s", name->ident,
                         t->k == T_STRUCT ? "struct" : "union");
            }
            vec_push(fields, new_field(ft, name->ident));
            if (!next_tk_is(s->pp, ',')) {
                break;
            }
        }
        expect_tk(s->pp, ';');
    }
    expect_tk(s->pp, '}');
    if (t->k == T_STRUCT) {
        set_struct_fields(t, fields);
    } else { // T_UNION
        set_union_fields(t, fields);
    }
}

static void parse_enum_consts(Scope *s, AstType *t) {
    expect_tk(s->pp, '{');
    AstType *num_t = t_num(T_INT, 0);
    Vec *consts = vec_new();
    int64_t val = 0;
    while (!peek_tk_is(s->pp, '}') && !peek_tk_is(s->pp, TK_EOF)) {
        Token *name = expect_tk(s->pp, TK_IDENT);
        if (next_tk_is(s->pp, '=')) {
            AstNode *e = parse_expr_no_commas(s);
            val = calc_int_expr(e);
        }
        AstType *min = smallest_type_for_int(val < 0 ? -val : val, val < 0);
        if (min->k > num_t->k || (min->k == num_t->k && min->is_unsigned)) {
            *num_t = *min;
        }
        EnumConst *k = new_enum_const(name->ident, val);
        vec_push(consts, k);
        def_enum_const(s, name, num_t, val);
        val++;
        if (!next_tk_is(s->pp, ',')) {
            break;
        }
    }
    expect_tk(s->pp, '}');
    set_enum_consts(t, consts, num_t);
}

static void parse_aggr_def(Scope *s, AstType *t) {
    if (t->k == T_STRUCT || t->k == T_UNION) {
        parse_aggr_fields(s, t);
    } else { // T_ENUM
        parse_enum_consts(s, t);
    }
}

static AstType * parse_aggr(Scope *s, AstTypeT k) {
    if (!peek_tk_is(s->pp, TK_IDENT)) { // Anonymous
        AstType *t = t_new(k);
        parse_aggr_def(s, t);
        return t;
    }
    Token *tag = next_tk(s->pp);
    if (peek_tk_is(s->pp, '{')) { // Definition
        AstType *prev = map_get(s->tags, tag->ident);
        if (prev && prev->k != k) {
            error_at(tag, "use of tag '%s' does not match previous declaration",
                     tag->ident);
        } else if (prev && !is_incomplete(prev)) {
            error_at(tag, "redefinition of %s tag '%s'",
                     k == T_STRUCT ? "struct" : k == T_UNION ? "union" : "enum",
                     tag->ident);
        }
        AstType *t = prev ? prev : t_new(k);
        map_put(s->tags, tag->ident, t);
        parse_aggr_def(s, t);
        return t;
    } else { // Declaration/use
        AstType *prev = find_tag(s, tag->ident);
        if (prev && prev->k != k) {
            error_at(tag, "use of tag '%s' does not match previous declaration",
                     tag->ident);
        } else if (prev) { // Use of previous declaration
            return prev;
        } else { // New declaration
            AstType *t = t_new(k);
            map_put(s->tags, tag->ident, t);
            return t;
        }
    }
}

static AstType * parse_decl_specs(Scope *s, StorageClass *sclass) {
    if (!is_type(s, peek_tk(s->pp))) {
        error_at(peek_tk(s->pp), "expected type name");
    }
    int sc = 0, tq = 0, fs = 0;
    enum { tnone, tvoid, tchar, tint, tfloat, tdouble } kind = 0;
    enum { tlong = 1, tllong, tshort } size = 0;
    enum { tsigned = 1, tunsigned } sign = 0;
    AstType *t = NULL;
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
        case TK_STRUCT:   if (t) { goto t_err; } t = parse_aggr(s, T_STRUCT); break;
        case TK_UNION:    if (t) { goto t_err; } t = parse_aggr(s, T_UNION); break;
        case TK_ENUM:     if (t) { goto t_err; } t = parse_aggr(s, T_ENUM); break;
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
    if (sclass) {
        *sclass = sc;
    }
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

static AstType * parse_declarator_tail(Scope *s, AstType *base, Vec *param_names);
static AstNode * parse_expr(Scope *s);
static int try_calc_int_expr(AstNode *e, int64_t *val);
static AstNode * conv_to(AstNode *l, AstType *t);

static AstType * parse_array_declarator(Scope *s, AstType *base) {
    expect_tk(s->pp, '[');
    AstNode *len = NULL;
    if (!next_tk_is(s->pp, ']')) {
        AstNode *n = parse_expr(s);
        int64_t i;
        if (!try_calc_int_expr(n, &i)) {
            len = conv_to(n, t_num(T_LLONG, 1)); // VLA
        } else if (i < 0) {
            error_at(n->tk, "cannot have array with negative size ('%" PRId64 "')", i);
        } else {
            len = node(N_IMM, n->tk);
            len->t = t_num(T_LLONG, 1);
            len->imm = i;
        }
        expect_tk(s->pp, ']');
    }
    Token *err = peek_tk(s->pp);
    AstType *t = parse_declarator_tail(s, base, NULL);
    if (t->k == T_FN) {
        error_at(err, "cannot have array of functions");
    }
    if (is_incomplete(t)) {
        error_at(err, "cannot have array of elements with incomplete type");
    }
    return t_arr(t, len);
}

static AstType * parse_fn_declarator_param(Scope *s, Token **name) {
    Token *err = peek_tk(s->pp);
    AstType *base = t_num(T_INT, 0); // Parameter types default to 'int'
    if (is_type(s, peek_tk(s->pp))) {
        base = parse_decl_specs(s, NULL);
    }
    AstType *t = parse_declarator(s, base, name, NULL);
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

static AstType * parse_fn_declarator(Scope *s, AstType *ret, Vec *param_names) {
    if (ret->k == T_FN) {
        error_at(peek_tk(s->pp), "function cannot return a function");
    } else if (ret->k == T_ARR) {
        error_at(peek_tk(s->pp), "function cannot return an array");
    }
    expect_tk(s->pp, '(');
    if (peek_tk_is(s->pp, TK_VOID) && peek2_tk_is(s->pp, ')')) {
        next_tk(s->pp); next_tk(s->pp); // Skip 'void' ')'
        return t_fn(ret, vec_new(), 0);
    }
    Vec *param_types = vec_new();
    while (!peek_tk_is(s->pp, ')') && !peek_tk_is(s->pp, TK_ELLIPSIS) &&
           !peek_tk_is(s->pp, TK_EOF)) {
        Token *name;
        AstType *param = parse_fn_declarator_param(s, &name);
        vec_push(param_types, param);
        if (param_names) {
            vec_push(param_names, name); // Name may be NULL
        }
        if (!next_tk_is(s->pp, ',')) {
            break;
        }
    }
    int is_vararg = 0;
    Token *ellipsis = next_tk_is(s->pp, TK_ELLIPSIS);
    if (ellipsis) {
        if (vec_len(param_types) == 0) {
            error_at(ellipsis, "expected at least one parameter before '...'");
        }
        is_vararg = 1;
    }
    expect_tk(s->pp, ')');
    return t_fn(ret, param_types, is_vararg);
}

static AstType * parse_declarator_tail(Scope *s, AstType *base, Vec *param_names) {
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

static AstType * parse_declarator(Scope *s, AstType *base, Token **name, Vec *param_names) {
    if (next_tk_is(s->pp, '*')) {
        skip_type_quals(s);
        return parse_declarator(s, t_ptr(base), name, param_names);
    }
    if (next_tk_is(s->pp, '(')) { // Either sub-declarator or fn parameters
        if (is_type(s, peek_tk(s->pp)) || peek_tk_is(s->pp, ')')) { // Function
            // An empty '()' is a function pointer, not a no-op sub-declarator
            return parse_fn_declarator(s, base, param_names);
        } else { // Sub-declarator
            AstType *inner = t_new(T_VOID);
            AstType *decl = parse_declarator(s, inner, name, param_names);
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

static AstType * parse_named_declarator(Scope *s, AstType *base, Token **name, Vec *param_names) {
    Token *name_copy = NULL;
    Token *err = peek_tk(s->pp);
    AstType *t = parse_declarator(s, base, &name_copy, param_names);
    if (!name_copy) {
        error_at(err, "expected named declarator");
    }
    if (name) {
        *name = name_copy;
    }
    return t;
}

static AstType * parse_abstract_declarator(Scope *s, AstType *base) {
    Token *name = NULL;
    AstType *t = parse_declarator(s, base, &name, NULL);
    if (name) {
        error_at(name, "expected abstract declarator");
    }
    return t;
}


// ---- Expressions -----------------------------------------------------------

typedef enum {
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
} Prec;

static Prec BINOP_PREC[TK_LAST] = {
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

static AstNode * parse_subexpr(Scope *s, Prec min_prec);
static AstNode * parse_decl_init(Scope *s, AstType *t);

static AstNode * conv_to(AstNode *l, AstType *t) {
    AstNode *n = l;
    if (!are_equal(l->t, t)) {
        n = node(N_CONV, l->tk);
        n->t = t;
        n->l = l;
    }
    return n;
}

static AstNode * discharge(AstNode *l) {
    AstNode *n;
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

static AstNode * parse_operand(Scope *s) {
    AstNode *n;
    Token *tk = peek_tk(s->pp);
    switch (tk->k) {
    case TK_NUM: n = parse_num(s); break;
    case TK_CH:  n = parse_ch(s); break;
    case TK_STR: n = parse_str(s); break;
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

static AstNode * parse_array_access(Scope *s, AstNode *l) {
    Token *op = expect_tk(s->pp, '[');
    if (l->t->k != T_ARR) {
        l = discharge(l);
    }
    if (l->t->k != T_ARR && l->t->k != T_PTR) {
        error_at(op, "expected pointer or array type");
    }
    AstNode *idx = parse_subexpr(s, PREC_MIN);
    expect_int(idx);
    idx = conv_to(idx, t_num(T_LLONG, 0));
    expect_tk(s->pp, ']');
    AstNode *n = node(N_IDX, op);
    n->t = l->t->elem;
    n->l = l;
    n->r = idx;
    return n;
}

static AstNode * parse_call(Scope *s, AstNode *l) {
    Token *op = expect_tk(s->pp, '(');
    l = discharge(l);
    if (l->t->k != T_PTR || l->t->ptr->k != T_FN) {
        error_at(l->tk, "expected function type");
    }
    AstType *fn_t = l->t->ptr;
    Vec *args = vec_new();
    while (!peek_tk_is(s->pp, ')') && !peek_tk_is(s->pp, TK_EOF)) {
        AstNode *arg = parse_subexpr(s, PREC_COMMA);
        arg = discharge(arg);
        if (vec_len(args) >= vec_len(fn_t->params)) {
            if (!fn_t->is_vararg) {
                error_at(arg->tk, "too many arguments to function call");
            }
        } else { // Emit conversions for non-varargs
            AstType *expected = vec_get(fn_t->params, vec_len(args));
            arg = conv_to(arg, expected);
        }
        vec_push(args, arg);
        if (!next_tk_is(s->pp, ',')) {
            break;
        }
    }
    if (vec_len(args) < vec_len(fn_t->params)) {
        error_at(peek_tk(s->pp), "too few arguments to function call");
    }
    expect_tk(s->pp, ')');
    AstNode *n = node(N_CALL, op);
    n->t = fn_t->ret;
    n->fn = l;
    n->args = args;
    return n;
}

static AstNode * parse_struct_field_access(Scope *s, AstNode *l) {
    Token *op = next_tk(s->pp);
    if (l->t->k != T_STRUCT && l->t->k != T_UNION) {
        error_at(op, "expected struct or union type");
    }
    if (is_incomplete(l->t)) {
        error_at(op, "incomplete definition of %s",
                 l->t->k == T_STRUCT ? "struct" : "union");
    }
    Token *name = expect_tk(s->pp, TK_IDENT);
    size_t f_idx = find_field(l->t, name->ident);
    if (f_idx == NOT_FOUND) {
        error_at(name, "no field named '%s' in %s", name->ident,
                 l->t->k == T_STRUCT ? "struct" : "union");
    }
    Field *f = vec_get(l->t->fields, f_idx);
    AstNode *n = node(N_FIELD, op);
    n->t = f->t;
    n->obj = l;
    n->field_idx = f_idx;
    return n;
}

static AstNode * parse_struct_field_deref(Scope *s, AstNode *l) {
    expect_ptr(l);
    AstNode *n = node(N_DEREF, peek_tk(s->pp));
    n->t = l->t->ptr;
    n->l = l;
    return parse_struct_field_access(s, n);
}

static AstNode * parse_post_inc_dec(Scope *s, AstNode *l) {
    Token *op = next_tk(s->pp);
    expect_assignable(l);
    expect_val(l);
    l = discharge(l);
    AstNode *n = node(op->k == TK_INC ? N_POST_INC : N_POST_DEC, op);
    n->t = l->t;
    n->l = l;
    return n;
}

static AstNode * parse_postfix(Scope *s, AstNode *l) {
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

static AstNode * parse_neg(Scope *s) {
    Token *op = expect_tk(s->pp, '-');
    AstNode *l = parse_subexpr(s, PREC_UNARY);
    expect_num(l);
    l = discharge(l);
    AstNode *unop = node(N_NEG, op);
    unop->t = l->t;
    unop->l = l;
    return unop;
}

static AstNode * parse_plus(Scope *s) {
    expect_tk(s->pp, '+');
    AstNode *l = parse_subexpr(s, PREC_UNARY);
    expect_num(l);
    return discharge(l); // Type promotion
}

static AstNode * parse_bit_not(Scope *s) {
    Token *op = expect_tk(s->pp, '~');
    AstNode *l = parse_subexpr(s, PREC_UNARY);
    expect_int(l);
    l = discharge(l);
    AstNode *unop = node(N_BIT_NOT, op);
    unop->t = l->t;
    unop->l = l;
    return unop;
}

static AstNode * parse_log_not(Scope *s) {
    Token *op = expect_tk(s->pp, '!');
    AstNode *l = parse_subexpr(s, PREC_UNARY);
    expect_val(l);
    l = discharge(l);
    AstNode *unop = node(N_LOG_NOT, op);
    unop->t = t_num(T_INT, 0);
    unop->l = l;
    return unop;
}

static AstNode * parse_pre_inc_dec(Scope *s) {
    Token *op = next_tk(s->pp);
    AstNode *l = parse_subexpr(s, PREC_UNARY);
    expect_assignable(l);
    expect_val(l);
    l = discharge(l);
    AstNode *unop = node(op->k == TK_INC ? N_PRE_INC : N_PRE_DEC, op);
    unop->t = l->t;
    unop->l = l;
    return unop;
}

static AstNode * parse_deref(Scope *s) {
    Token *op = expect_tk(s->pp, '*');
    AstNode *l = parse_subexpr(s, PREC_UNARY);
    l = discharge(l);
    expect_ptr(l);
    if (l->t->ptr->k == T_FN) return l; // Don't dereference fn ptrs
    AstNode *unop = node(N_DEREF, op);
    unop->t = l->t->ptr;
    unop->l = l;
    return unop;
}

static AstNode * parse_addr(Scope *s) {
    Token *op = expect_tk(s->pp, '&');
    AstNode *l = parse_subexpr(s, PREC_UNARY);
    expect_lval(l);
    AstNode *unop = node(N_ADDR, op);
    unop->t = t_ptr(l->t);
    unop->l = l;
    return unop;
}

static AstNode * parse_sizeof(Scope *s) {
    Token *op = expect_tk(s->pp, TK_SIZEOF);
    AstType *t;
    if (peek_tk_is(s->pp, '(') && is_type(s, peek2_tk(s->pp))) {
        next_tk(s->pp);
        t = parse_decl_specs(s, NULL);
        t = parse_abstract_declarator(s, t);
        expect_tk(s->pp, ')');
    } else {
        AstNode *l = parse_subexpr(s, PREC_UNARY);
        t = l->t;
    }
    AstNode *n = node(N_IMM, op);
    n->t = t_num(T_LONG, 1);
    n->imm = t->size;
    return n;
}

static AstNode * parse_cast(Scope *s) {
    expect_tk(s->pp, '(');
    AstType *t = parse_decl_specs(s, NULL);
    t = parse_abstract_declarator(s, t);
    expect_tk(s->pp, ')');
    if (peek_tk_is(s->pp, '{')) { // Compound literal
        return parse_decl_init(s, t);
    } else {
        AstNode *l = parse_subexpr(s, PREC_UNARY);
        return conv_to(l, t);
    }
}

static AstNode * parse_unop(Scope *s) {
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
    AstNode *l = parse_operand(s);
    return parse_postfix(s, l);
}

static AstType * promote(AstType *l, AstType *r) { // Implicit arithmetic conversions
    assert(is_num(l));
    assert(is_num(r));
    if (l->k < r->k) { // Make 'l' the larger type
        AstType *tmp = l; l = r; r = tmp;
    }
    if (is_fp(l)) { // If one is a float, pick the largest float type
        return l;
    }
    assert(is_int(l) && l->k >= T_INT); // Both integers bigger than 'int'
    assert(is_int(r) && r->k >= T_INT); // Handled by 'discharge'
    if (l->k > r->k) { // Pick the larger
        return l;
    }
    assert(l->k == r->k); // Both the same type
    return l->is_unsigned ? l : r; // Pick the unsigned type
}

static AstNode * emit_binop(AstNodeT op, AstNode *l, AstNode *r, Token *tk) {
    l = discharge(l);
    r = discharge(r);
    AstType *t;
    if (l->t->k == T_PTR && r->t->k == T_PTR) { // Both pointers
        if (op != N_SUB && op != N_TERNARY && !(op >= N_EQ && op <= N_LOG_OR)) {
            error_at(tk, "invalid operands to binary operation");
        }
        t = is_void_ptr(l->t) || is_null_ptr(l) ? r->t : l->t;
    } else if ((op == N_ADD || op == N_SUB) &&
            (l->t->k == T_PTR || r->t->k == T_PTR)) { // Pointer arithmetic
        if (op == N_SUB && r->t->k == T_PTR) { // <int> - <ptr> invalid
            error_at(tk, "invalid operands to binary operation");
        }
        t = l->t->k == T_PTR ? l->t : r->t;
        AstNode **num = l->t->k == T_PTR ? &r : &l;
        *num = conv_to(*num, t_num(T_LLONG, 1));
    } else if (l->t->k == T_PTR || r->t->k == T_PTR) { // Pointer comparison
        t = l->t->k == T_PTR ? l->t : r->t;
        l = conv_to(l, t);
        r = conv_to(r, t);
    } else { // No pointers
        assert(is_num(l->t) && is_num(r->t));
        t = promote(l->t, r->t);
        l = conv_to(l, t);
        r = conv_to(r, t);
    }
    AstType *ret = t;
    if (l->t->k == T_PTR && r->t->k == T_PTR && op == N_SUB) {
        ret = t_num(T_LLONG, 0); // Pointer difference
    } else if (op >= N_EQ && op <= N_LOG_OR) {
        ret = t_num(T_INT, 0); // Comparison
    }
    if (!((op == N_SUB || op == N_ADD) && ((l->t->k == T_PTR) ^ (r->t->k == T_PTR)))) {
        l = conv_to(l, t); // Don't convert if we have <ptr> +/- <int>
        r = conv_to(r, t);
    }
    AstNode *n = node(op, tk);
    n->l = l;
    n->r = r;
    n->t = ret;
    return n;
}

static AstNode * parse_binop(Scope *s, Token *op, AstNode *l) {
    AstNode *r = parse_subexpr(s, BINOP_PREC[op->k] + IS_RASSOC[op->k]);
    AstNode *n;
    switch (op->k) {
    case '+':    expect_val(l); expect_val(r); return emit_binop(N_ADD, l, r, op);
    case '-':    expect_val(l); expect_val(r); return emit_binop(N_SUB, l, r, op);
    case '*':    expect_num(l); expect_num(r); return emit_binop(N_MUL, l, r, op);
    case '/':    expect_num(l); expect_num(r); return emit_binop(N_DIV, l, r, op);
    case '%':    expect_int(l); expect_int(r); return emit_binop(N_MOD, l, r, op);
    case '&':    expect_int(l); expect_int(r); return emit_binop(N_BIT_AND, l, r, op);
    case '|':    expect_int(l); expect_int(r); return emit_binop(N_BIT_OR, l, r, op);
    case '^':    expect_int(l); expect_int(r); return emit_binop(N_BIT_XOR, l, r, op);
    case TK_SHL: expect_int(l); expect_int(r); return emit_binop(N_SHL, l, r, op);
    case TK_SHR: expect_int(l); expect_int(r); return emit_binop(N_SHR, l, r, op);

    case TK_EQ:      expect_val(l); expect_val(r); return emit_binop(N_EQ, l, r, op);
    case TK_NEQ:     expect_val(l); expect_val(r); return emit_binop(N_NEQ, l, r, op);
    case '<':        expect_val(l); expect_val(r); return emit_binop(N_LT, l, r, op);
    case TK_LE:      expect_val(l); expect_val(r); return emit_binop(N_LE, l, r, op);
    case '>':        expect_val(l); expect_val(r); return emit_binop(N_GT, l, r, op);
    case TK_GE:      expect_val(l); expect_val(r); return emit_binop(N_GE, l, r, op);
    case TK_LOG_AND: expect_val(l); expect_val(r); return emit_binop(N_LOG_AND, l, r, op);
    case TK_LOG_OR:  expect_val(l); expect_val(r); return emit_binop(N_LOG_OR, l, r, op);

    case TK_A_ADD:     expect_val(l); expect_val(r); return emit_binop(N_A_ADD, l, r, op);
    case TK_A_SUB:     expect_val(l); expect_val(r); return emit_binop(N_A_SUB, l, r, op);
    case TK_A_MUL:     expect_num(l); expect_num(r); return emit_binop(N_A_MUL, l, r, op);
    case TK_A_DIV:     expect_num(l); expect_num(r); return emit_binop(N_A_DIV, l, r, op);
    case TK_A_MOD:     expect_int(l); expect_int(r); return emit_binop(N_A_MOD, l, r, op);
    case TK_A_BIT_AND: expect_int(l); expect_int(r); return emit_binop(N_A_BIT_AND, l, r, op);
    case TK_A_BIT_OR:  expect_int(l); expect_int(r); return emit_binop(N_A_BIT_OR, l, r, op);
    case TK_A_BIT_XOR: expect_int(l); expect_int(r); return emit_binop(N_A_BIT_XOR, l, r, op);
    case TK_A_SHL:     expect_int(l); expect_int(r); return emit_binop(N_A_SHL, l, r, op);
    case TK_A_SHR:     expect_int(l); expect_int(r); return emit_binop(N_A_SHR, l, r, op);

    case '=':
        expect_assignable(l);
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
        AstNode *els = parse_subexpr(s, PREC_TERNARY + IS_RASSOC['?']);
        AstNode *binop = emit_binop(N_TERNARY, r, els, op);
        n = node(N_TERNARY, op);
        n->t = binop->t;
        n->cond = l;
        n->body = n->l;
        n->els = n->r;
        return n;
    default: UNREACHABLE();
    }
}

static AstNode * parse_subexpr(Scope *s, Prec min_prec) {
    AstNode *l = parse_unop(s);
    while (BINOP_PREC[peek_tk(s->pp)->k] > min_prec) {
        Token *op = next_tk(s->pp);
        l = parse_binop(s, op, l);
    }
    return l;
}

static AstNode * parse_expr(Scope *s) {
    return parse_subexpr(s, PREC_MIN);
}

static AstNode * parse_expr_no_commas(Scope *s) {
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

#define CALC_UNOP_INT_FP(op)        \
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

#define CALC_BINOP_INT_FP(op)                  \
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

#define CALC_BINOP_INT_FP_PTR(op)                                           \
    l = eval_const_expr(e->l, err);                                         \
    if (!l) goto err;                                                       \
    r = eval_const_expr(e->r, err);                                         \
    if (!r) goto err;                                                       \
    if (l->k == N_IMM && r->k == N_IMM) {                                   \
        n->k = N_IMM;                                                       \
        n->imm = l->imm op r->imm;                                          \
    } else if (l->k == N_FP && r->k == N_FP) {                              \
        n->k = N_FP;                                                        \
        n->fp = l->fp op r->fp;                                             \
    } else if ((l->k == N_KPTR && r->k == N_IMM) ||                         \
               (l->k == N_IMM && r->k == N_KPTR)) {                         \
        AstNode *ptr = (l->k == N_KPTR) ? l : r, *imm = (ptr == l) ? r : l; \
        n->k = N_KPTR;                                                      \
        n->g = ptr->g;                                                      \
        n->offset = ptr->offset op (int64_t) imm->imm * ptr->t->ptr->size;  \
    } else if (e->k == N_SUB && l->k == N_KPTR && r->k == N_KPTR &&         \
               globals_are_equal(l->g, r->g)) {                             \
        n->k = N_IMM;                                                       \
        n->imm = (l->offset op r->offset) / l->t->ptr->size;                \
    } else {                                                                \
        goto err;                                                           \
    }                                                                       \
    break;

#define CALC_BINOP_EQ(op)                                                      \
    l = eval_const_expr(e->l, err);                                            \
    if (!l) goto err;                                                          \
    r = eval_const_expr(e->r, err);                                            \
    if (!r) goto err;                                                          \
    if (l->k == N_IMM && r->k == N_IMM) {                                      \
        n->k = N_IMM;                                                          \
        n->imm = op (l->imm == r->imm);                                        \
    } else if (l->k == N_FP && r->k == N_FP) {                                 \
        n->k = N_IMM;                                                          \
        n->imm = op (l->fp == r->fp);                                          \
    } else if (l->k == N_KPTR && r->k == N_KPTR) {                             \
        n->k = N_IMM;                                                          \
        n->imm = op (globals_are_equal(l->g, r->g) && l->offset == r->offset); \
    } else if ((l->k == N_KPTR && r->k == N_IMM) ||                            \
               (l->k == N_IMM && r->k == N_KPTR)) {                            \
        AstNode *ptr = (l->k == N_KPTR) ? l : r, *imm = (ptr == l) ? r : l;    \
        n->k = N_IMM;                                                          \
        n->imm = op (ptr->g == NULL && ptr->offset == (int64_t) imm->imm);     \
    } else {                                                                   \
        goto err;                                                              \
    }                                                                          \
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

static int globals_are_equal(AstNode *g1, AstNode *g2) {
    return (g1 == NULL && g2 == NULL) ||
           (g1 && g2 && strcmp(g1->var_name, g2->var_name) == 0);
}

// Only allowable 'Node' types are N_IMM, N_FP, N_STR, N_INIT, N_SIZEOF,
// N_KVAL, N_KPTR
static AstNode * eval_const_expr(AstNode *e, Token **err) {
    AstNode *cond, *l, *r;
    AstNode *n = node(e->k, e->tk);
    switch (e->k) {
        // Constants
    case N_IMM: case N_FP: case N_STR: *n = *e; break;
    case N_INIT:
        n->elems = vec_new();
        for (size_t i = 0; i < vec_len(e->elems); i++) {
            AstNode *v = vec_get(e->elems, i);
            if (!v) {
                vec_push(n->elems, NULL);
                continue;
            }
            v = eval_const_expr(v, err);
            if (!v || v->k == N_KVAL) goto err;
            vec_push(n->elems, v);
        }
        break;
    case N_GLOBAL:
        n->k = N_KVAL;
        n->g = e;
        break;

        // Binary operations
    case N_ADD:     CALC_BINOP_INT_FP_PTR(+)
    case N_SUB:     CALC_BINOP_INT_FP_PTR(-)
    case N_MUL:     CALC_BINOP_INT_FP(*)
    case N_DIV:     CALC_BINOP_INT_FP(/)
    case N_MOD:     CALC_BINOP_INT(%)
    case N_SHL:     CALC_BINOP_INT(<<)
    case N_SHR:     CALC_BINOP_INT(>>)
    case N_BIT_AND: CALC_BINOP_INT(&)
    case N_BIT_OR:  CALC_BINOP_INT(|)
    case N_BIT_XOR: CALC_BINOP_INT(^)
    case N_EQ:      CALC_BINOP_EQ()
    case N_NEQ:     CALC_BINOP_EQ(!)
    case N_LT:      CALC_BINOP_REL(<)
    case N_LE:      CALC_BINOP_REL(<=)
    case N_GT:      CALC_BINOP_REL(>)
    case N_GE:      CALC_BINOP_REL(>=)
    case N_LOG_AND: CALC_BINOP_INT(&&)
    case N_LOG_OR:  CALC_BINOP_INT(||)
    case N_COMMA:
        r = eval_const_expr(e->r, err);
        if (!r) goto err;
        *n = *r; // Ignore LHS
        break;

        // Ternary operation
    case N_TERNARY:
        cond = eval_const_expr(e->cond, err);
        if (!cond) goto err;
        if (cond->k != N_IMM) goto err;
        l = eval_const_expr(e->body, err);
        if (!l) goto err;
        r = eval_const_expr(e->els, err);
        if (!r) goto err;
        *n = cond->imm ? *l : *r;
        break;

        // Unary operations
    case N_NEG:     CALC_UNOP_INT_FP(-)
    case N_BIT_NOT: CALC_UNOP_INT(~)
    case N_LOG_NOT: CALC_UNOP_INT(!)
    case N_ADDR:
        l = eval_const_expr(e->l, err);
        if (!l || l->k != N_KVAL) goto err;
        *n = *l;
        n->k = N_KPTR;
        break;
    case N_DEREF:
        l = eval_const_expr(e->l, err);
        if (!l || l->k != N_KPTR) goto err;
        *n = *l;
        n->k = N_KVAL;
        break;
    case N_CONV:
        l = eval_const_expr(e->l, err);
        if (!l) goto err;
        if (is_fp(e->t) && l->k == N_IMM) { // int -> float
            n->k = N_FP;
            n->fp = (double) l->imm;
        } else if (is_int(e->t) && l->k == N_FP) { // float -> int
            n->k = N_IMM;
            n->imm = (int64_t) l->fp;
        } else if (is_int(e->t) && l->k == N_IMM) { // int -> int
            n->k = N_IMM;
            uint64_t bits = e->t->size * 8; // Bits
            uint64_t mask = bits >= 64 ? -1 : ((uint64_t) 1 << bits) - 1;
            n->imm = l->imm & mask; // Truncate
            if (!l->t->is_unsigned && (n->imm & ((int64_t) 1 << (bits - 1)))) {
                n->imm |= ~(((int64_t) 1 << bits) - 1); // Sign extend
            }
        } else if (e->t->k == T_PTR && l->k == N_IMM) { // int -> ptr
            n->k = N_KPTR;
            n->g = NULL;
            n->offset = (int64_t) l->imm;
        } else if (is_int(e->t) && l->k == N_KPTR) { // ptr -> int
            if (l->g) goto err;
            n->k = N_IMM;
            n->imm = l->offset;
        } else { // Direct conversion
            *n = *l;
        }
        break;

        // Postfix operations
    case N_IDX:
        l = eval_const_expr(e->l, err);
        if (!l || (l->k != N_KPTR && l->k != N_KVAL)) goto err;
        r = eval_const_expr(e->r, err);
        if (!r || r->k != N_IMM) goto err;
        *n = *l;
        n->offset += (int64_t) (r->imm * l->t->elem->size);
        break;
    case N_FIELD:
        l = eval_const_expr(e->obj, err);
        if (!l || l->k != N_KVAL) goto err;
        *n = *l;
        Field *f = vec_get(l->t->fields, e->field_idx);
        n->offset += (int64_t) f->offset;
        break;
    default: goto err;
    }
    n->t = e->t;
    n->tk = e->tk;
    return n;
err:
    if (err && !*err) *err = e->tk;
    return NULL;
}

static AstNode * calc_const_expr(AstNode *e) {
    Token *err = NULL;
    AstNode *n = eval_const_expr(e, &err);
    if (!n) {
        error_at(err, "expected constant expression");
    }
    if (n->k == N_KVAL) {
        error_at(n->tk, "expected constant expression");
    }
    return n;
}

static int64_t calc_int_expr(AstNode *e) {
    AstNode *n = calc_const_expr(e);
    if (n->k != N_IMM) {
        error_at(n->tk, "expected constant integer expression");
    }
    return (int64_t) n->imm;
}

static int try_calc_int_expr(AstNode *e, int64_t *val) {
    AstNode *n = eval_const_expr(e, NULL);
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
    AstNode *e = parse_expr(&s);
    return calc_int_expr(e);
}


// ---- Statements ------------------------------------------------------------

static AstNode * parse_decl(Scope *s);
static AstNode * parse_block(Scope *s);
static AstNode * parse_stmt(Scope *s);

static AstNode * parse_if(Scope *s) {
    Token *if_tk = expect_tk(s->pp, TK_IF);
    expect_tk(s->pp, '(');
    AstNode *cond = parse_expr(s);
    expect_tk(s->pp, ')');
    AstNode *body = parse_stmt(s);
    AstNode *els = NULL;
    if (peek_tk_is(s->pp, TK_ELSE)) {
        Token *else_tk = next_tk(s->pp);
        if (peek_tk_is(s->pp, TK_IF)) { // else if
            els = parse_stmt(s);
        } else { // else
            AstNode *else_body = parse_stmt(s);
            els = node(N_IF, else_tk);
            els->cond = NULL;
            els->body = else_body;
            els->els = NULL;
        }
    }
    AstNode *n = node(N_IF, if_tk);
    n->cond = cond;
    n->body = body;
    n->els = els;
    return n;
}

static AstNode * parse_while(Scope *s) {
    Token *while_tk = expect_tk(s->pp, TK_WHILE);
    expect_tk(s->pp, '(');
    AstNode *cond = parse_expr(s);
    expect_tk(s->pp, ')');
    Scope loop;
    enter_scope(&loop, s, SCOPE_LOOP);
    AstNode *body = parse_stmt(&loop);
    AstNode *n = node(N_WHILE, while_tk);
    n->cond = cond;
    n->body = body;
    return n;
}

static AstNode * parse_do_while(Scope *s) {
    Token *do_tk = expect_tk(s->pp, TK_DO);
    Scope loop;
    enter_scope(&loop, s, SCOPE_LOOP);
    AstNode *body = parse_stmt(&loop);
    expect_tk(s->pp, TK_WHILE);
    expect_tk(s->pp, '(');
    AstNode *cond = parse_expr(s);
    expect_tk(s->pp, ')');
    expect_tk(s->pp, ';');
    AstNode *n = node(N_DO_WHILE, do_tk);
    n->cond = cond;
    n->body = body;
    return n;
}

static AstNode * parse_for(Scope *s) {
    Token *for_tk = expect_tk(s->pp, TK_FOR);
    expect_tk(s->pp, '(');
    Scope loop;
    enter_scope(&loop, s, SCOPE_LOOP);

    AstNode *init = NULL;
    if (is_type(&loop, peek_tk(s->pp))) {
        init = parse_decl(&loop);
    } else if (!peek_tk_is(s->pp, ';')) {
        init = parse_expr(&loop);
        expect_tk(s->pp, ';');
    }

    AstNode *cond = NULL;
    if (!peek_tk_is(s->pp, ';')) {
        cond = parse_expr(&loop);
    }
    expect_tk(s->pp, ';');

    AstNode *inc = NULL;
    if (!peek_tk_is(s->pp, ')')) {
        inc = parse_expr(&loop);
    }
    expect_tk(s->pp, ')');

    AstNode *body = parse_stmt(&loop);
    AstNode *n = node(N_FOR, for_tk);
    n->init = init;
    n->cond = cond;
    n->inc = inc;
    n->body = body;
    return n;
}

static AstNode * parse_switch(Scope *s) {
    Token *switch_tk = expect_tk(s->pp, TK_SWITCH);
    expect_tk(s->pp, '(');
    AstNode *cond = parse_expr(s);
    expect_tk(s->pp, ')');

    Scope switch_s;
    enter_scope(&switch_s, s, SCOPE_SWITCH);
    switch_s.cond_t = cond->t;
    AstNode *body = parse_stmt(&switch_s);
    AstNode *n = node(N_SWITCH, switch_tk);
    n->cond = cond;
    n->body = body;
    n->cases = switch_s.cases;
    return n;
}

static AstNode * parse_case(Scope *s) {
    Token *case_tk = expect_tk(s->pp, TK_CASE);
    Scope *switch_s = find_scope(s, SCOPE_SWITCH);
    if (!switch_s) {
        error_at(case_tk, "'case' not allowed here");
    }
    AstNode *cond_expr = conv_to(parse_expr(s), switch_s->cond_t);
    int64_t cond = calc_int_expr(cond_expr);
    for (size_t i = 0; i < vec_len(switch_s->cases); i++) {
        AstNode *c = vec_get(switch_s->cases, i);
        if (c && c->imm == (uint64_t) cond) {
            error_at(cond_expr->tk, "duplicate case value '%ll'", cond);
        }
    }
    expect_tk(s->pp, ':');
    AstNode *body = parse_stmt(s);
    AstNode *imm = node(N_IMM, cond_expr->tk);
    imm->t = switch_s->cond_t;
    imm->imm = cond;
    AstNode *n = node(N_CASE, case_tk);
    n->cond = imm;
    n->body = body;
    vec_push(switch_s->cases, n);
    return n;
}

static AstNode * parse_default(Scope *s) {
    Token *default_tk = expect_tk(s->pp, TK_DEFAULT);
    Scope *switch_s = find_scope(s, SCOPE_SWITCH);
    if (!switch_s) {
        error_at(default_tk, "'default' not allowed here");
    }
    for (size_t i = 0; i < vec_len(switch_s->cases); i++) {
        AstNode *c = vec_get(switch_s->cases, i);
        if (c->k == N_DEFAULT) {
            error_at(default_tk, "cannot have more than one 'default' in a switch");
        }
    }
    expect_tk(s->pp, ':');
    AstNode *body = parse_stmt(s);
    AstNode *n = node(N_DEFAULT, default_tk);
    n->body = body;
    vec_push(switch_s->cases, n);
    return n;
}

static AstNode * parse_break(Scope *s) {
    Token *break_tk = expect_tk(s->pp, TK_BREAK);
    if (!find_scope(s, SCOPE_LOOP) && !find_scope(s, SCOPE_SWITCH)) {
        error_at(break_tk, "'break' not allowed here");
    }
    expect_tk(s->pp, ';');
    AstNode *n = node(N_BREAK, break_tk);
    return n;
}

static AstNode * parse_continue(Scope *s) {
    Token *continue_tk = expect_tk(s->pp, TK_CONTINUE);
    if (!find_scope(s, SCOPE_LOOP)) {
        error_at(continue_tk, "'break' not allowed here");
    }
    expect_tk(s->pp, ';');
    AstNode *n = node(N_CONTINUE, continue_tk);
    return n;
}

static AstNode * parse_goto(Scope *s) {
    Token *goto_tk = expect_tk(s->pp, TK_GOTO);
    Token *label = expect_tk(s->pp, TK_IDENT);
    expect_tk(s->pp, ';');
    AstNode *n = node(N_GOTO, goto_tk);
    n->label = label->ident;
    return n;
}

static AstNode * parse_label(Scope *s) {
    Token *label = expect_tk(s->pp, TK_IDENT);
    expect_tk(s->pp, ':');
    AstNode *body = parse_stmt(s);
    AstNode *n = node(N_LABEL, label);
    n->label = label->ident;
    n->body = body;
    return n;
}

static AstNode * parse_ret(Scope *s) {
    Token *ret_tk = expect_tk(s->pp, TK_RETURN);
    AstNode *val = NULL;
    if (!peek_tk_is(s->pp, ';')) {
        if (s->fn->t->ret->k == T_VOID) {
            error_at(peek_tk(s->pp), "cannot return value from void function");
        }
        val = parse_expr(s);
        val = conv_to(val, s->fn->t->ret);
    }
    expect_tk(s->pp, ';');
    AstNode *n = node(N_RET, ret_tk);
    n->ret = val;
    return n;
}

static AstNode * parse_expr_stmt(Scope *s) {
    AstNode *n = parse_expr(s);
    expect_tk(s->pp, ';');
    return n;
}

static AstNode * parse_stmt(Scope *s) {
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

static AstNode * parse_stmt_or_decl(Scope *s) {
    if (is_type(s, peek_tk(s->pp))) {
        return parse_decl(s);
    } else {
        return parse_stmt(s);
    }
}

static AstNode * parse_block(Scope *s) {
    expect_tk(s->pp, '{');
    Scope block;
    enter_scope(&block, s, SCOPE_BLOCK);
    AstNode *head = NULL;
    AstNode **cur = &head;
    while (!peek_tk_is(s->pp, '}') && !peek_tk_is(s->pp, TK_EOF)) {
        *cur = parse_stmt_or_decl(&block);
        while (*cur) cur = &(*cur)->next;
    }
    expect_tk(s->pp, '}');
    return head;
}


// ---- Declarations ----------------------------------------------------------

static AstNode * parse_fn_def(Scope *s, AstType *t, Token *name, Vec *param_names) {
    if (t->k != T_FN) {
        error_at(name, "expected function type");
    }
    def_var(s, name, t);
    AstNode *fn = node(N_FN_DEF, name);
    fn->t = t;
    fn->fn_name = name->ident;
    fn->param_names = param_names;
    Scope fn_scope;
    enter_scope(&fn_scope, s, SCOPE_BLOCK);
    fn->body = parse_block(&fn_scope);
    return fn;
}

static AstNode * parse_string_init(Scope *s, AstType *t) {
    assert(is_string_type(t));
    assert(!is_vla(t));
    if (!peek_tk_is(s->pp, TK_STR)) {
        return NULL; // Parse as normal array
    }
    AstNode *str = parse_str(s);
    if (!are_equal(t->elem, str->t->elem)) {
        warning_at(str->tk, "initializing string with literal of different type");
    }
    if (!t->len) {
        assert(str->t->len->k == N_IMM);
        set_arr_len(t, str->t->len);
    }
    if (t->len->imm < str->len) {
        warning_at(str->tk, "initializer string is too long");
    }
    return str;
}

static AstNode * parse_init_list_raw(Scope *s, AstType *t, int designated);

static AstNode * parse_init_elem(Scope *s, AstType *t, int designated) {
    AstNode *n;
    int has_brace = peek_tk_is(s->pp, '{') != NULL;
    if (t && (t->k == T_ARR || t->k == T_STRUCT || t->k == T_UNION || has_brace)) {
        n = parse_init_list_raw(s, t, designated);
        if (has_brace && !peek_tk_is(s->pp, '}')) {
            expect_tk(s->pp, ',');
        }
    } else {
        n = parse_expr_no_commas(s);
        if (t) {
            n = conv_to(n, t);
        }
        if (!peek_tk_is(s->pp, '}')) {
            expect_tk(s->pp, ',');
        }
    }
    return n;
}

static size_t parse_array_designator(Scope *s, AstType *t) {
    expect_tk(s->pp, '[');
    AstNode *e = parse_expr(s);
    int64_t desg = calc_int_expr(e);
    if (desg < 0 || (t->len && (uint64_t) desg >= t->len->imm)) {
        error_at(e->tk, "designator index '%" PRId64 "' exceeds array bounds", desg);
    }
    expect_tk(s->pp, ']');
    expect_tk(s->pp, '=');
    return desg;
}

static AstNode * parse_array_init(Scope *s, AstType *t, int designated) {
    assert(t->k == T_ARR);
    assert(!is_vla(t));
    AstNode *n = node(N_INIT, peek_tk(s->pp));
    n->t = t;
    n->elems = vec_new();
    int has_brace = next_tk_is(s->pp, '{') != NULL;
    size_t idx = 0;
    while (!peek_tk_is(s->pp, '}') && !peek_tk_is(s->pp, TK_EOF)) {
        if (!has_brace && t->len && idx >= t->len->imm) {
            break;
        }
        if (peek_tk_is(s->pp, '[') && !has_brace && !designated) {
            break; // e.g., int a[3][3] = {3, /* RETURN HERE */ [2] = 1}
        }
        if (peek_tk_is(s->pp, '[')) {
            idx = parse_array_designator(s, t);
            designated = 1;
        }
        if (t->len && idx >= t->len->imm) {
            warning_at(peek_tk(s->pp), "excess elements in array initializer");
        }
        AstNode *elem = parse_init_elem(s, t->elem, designated);
        vec_put(n->elems, idx, elem);
        idx++;
        designated = 0;
    }
    if (has_brace) {
        expect_tk(s->pp, '}');
    }
    if (!t->len) {
        AstNode *len = node(N_IMM, NULL);
        len->t = t_num(T_LLONG, 1);
        len->imm = idx;
        set_arr_len(t, len);
    }
    if (idx < t->len->imm) {
        vec_put(n->elems, t->len->imm - 1, NULL);
    }
    return n;
}

static size_t parse_struct_designator(Scope *s, AstType *t) {
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

static AstNode * parse_struct_init(Scope *s, AstType *t, int designated) {
    assert(t->k == T_STRUCT || t->k == T_UNION);
    AstNode *n = node(N_INIT, peek_tk(s->pp));
    n->t = t;
    n->elems = vec_new();
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
        AstType *ft = NULL;
        if (idx >= vec_len(t->fields)) {
            warning_at(peek_tk(s->pp), "excess elements in %s initializer",
                       t->k == T_STRUCT ? "struct" : "union");
        } else {
            ft = ((Field *) vec_get(t->fields, idx))->t;
        }
        AstNode *elem = parse_init_elem(s, ft, designated);
        vec_put(n->elems, idx, elem);
        idx++;
        designated = 0;
    }
    if (has_brace) {
        expect_tk(s->pp, '}');
    }
    return n;
}

static AstNode * parse_init_list_raw(Scope *s, AstType *t, int designated) {
    if (is_string_type(t)) {
        AstNode *n = parse_string_init(s, t);
        if (n) return n; // Fall through if no string literal...
    }
    if (t->k == T_ARR) {
        return parse_array_init(s, t, designated);
    } else if (t->k == T_STRUCT || t->k == T_UNION) {
        return parse_struct_init(s, t, designated);
    } else { // Everything else, e.g., int a = {3}
        AstNode *len = node(N_IMM, peek_tk(s->pp));
        len->t = t_num(T_LLONG, 1);
        len->imm = 1;
        AstType *arr_t = t_arr(t, len);
        return parse_array_init(s, arr_t, designated);
    }
}

static AstNode * parse_init_list(Scope *s, AstType *t) {
    return parse_init_list_raw(s, t, 0);
}

static AstNode * parse_decl_init(Scope *s, AstType *t) {
    Token *err = peek_tk(s->pp);
    if (t->linkage == L_EXTERN || t->k == T_FN) {
        error_at(err, "illegal initializer");
    }
    if (is_vla(t)) {
        error_at(err, "cannot initialize variable-length array");
    }
    AstNode *val;
    if (peek_tk_is(s->pp, '{') || is_string_type(t)) {
        val = parse_init_list(s, t);
    } else {
        val = parse_expr_no_commas(s);
    }
    if (t->k == T_ARR && val->t->k != T_ARR) {
        error_at(err, "array initializer must be an initializer list or string literal");
    }
    if (t->k == T_ARR && !t->len) { // Complete an incomplete array type
        set_arr_len(t, val->t->len);
    }
    val = conv_to(val, t);
    if (t->linkage == L_STATIC || s->k == SCOPE_FILE) {
        val = calc_const_expr(val);
    }
    return val;
}

static AstNode * parse_decl_var(Scope *s, AstType *t, Token *name) {
    AstNode *var = def_var(s, name, t);
    AstNode *val = NULL;
    if (next_tk_is(s->pp, '=')) {
        val = parse_decl_init(s, t);
    }
    if (is_incomplete(t)) {
        // Check this after parsing the initializer because arrays without a
        // specified length may have their type completed
        // e.g., int a[] /* INCOMPLETE HERE */ = {1, 2, 3}; /* COMPLETE HERE */
        error_at(name, "variable cannot have incomplete type");
    }
    AstNode *decl = node(N_DECL, name);
    decl->var = var;
    decl->val = val;
    return decl;
}

static AstNode * parse_init_decl(Scope *s, AstType *base, StorageClass sclass) {
    Token *name = NULL;
    Vec *param_names = vec_new();
    AstType *t = parse_named_declarator(s, base, &name, param_names);
    switch (sclass) {
    case S_TYPEDEF: return def_typedef(s, name, t);
    case S_EXTERN:  t->linkage = L_EXTERN; break;
    case S_STATIC:  t->linkage = L_STATIC; break;
    case S_AUTO: case S_REGISTER:
        if (s->k == SCOPE_FILE) {
            error_at(name, "illegal storage class specifier in file scope");
        }
        break;
    case S_NONE: break;
    }
    if (s->k == SCOPE_FILE && peek_tk_is(s->pp, '{')) {
        return parse_fn_def(s, t, name, param_names);
    }
    return parse_decl_var(s, t, name);
}

static AstNode * parse_decl(Scope *s) {
    StorageClass sclass;
    AstType *base = parse_decl_specs(s, &sclass);
    if (next_tk_is(s->pp, ';')) {
        return NULL;
    }
    AstNode *head = NULL;
    AstNode **cur = &head;
    while (1) {
        *cur = parse_init_decl(s, base, sclass);
        if ((*cur)->k == N_FN_DEF) {
            return head;
        }
        while (*cur) {
            cur = &(*cur)->next;
        }
        if (!next_tk_is(s->pp, ',')) {
            break;
        }
    }
    expect_tk(s->pp, ';');
    return head;
}

AstNode * parse(File *f) {
    Lexer *l = new_lexer(f);
    PP *pp = new_pp(l);
    Scope file_scope = new_scope(SCOPE_FILE, pp);
    AstNode *head = NULL;
    AstNode **cur = &head;
    while (!next_tk_is(pp, TK_EOF)) {
        *cur = parse_decl(&file_scope);
        while (*cur) {
            cur = &(*cur)->next;
        }
    }
    return head;
}
