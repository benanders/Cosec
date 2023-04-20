
#include <stdlib.h>
#include <ctype.h>
#include <string.h>

#include "lex.h"
#include "error.h"

#define TK_FIRST TK_SHL

static Token *SPACE_TK   = &(Token) { .k = TK_SPACE };
static Token *NEWLINE_TK = &(Token) { .k = TK_NEWLINE };
static Token *EOF_TK     = &(Token) { .k = TK_EOF };

Lexer * new_lexer(File *f) {
    Lexer *l = calloc(1, sizeof(Lexer));
    l->f = f;
    l->buf = vec_new();
    return l;
}

static Token * new_tk(Lexer *l, int k) {
    Token *t = calloc(1, sizeof(Token));
    t->k = k;
    t->f = l->f;
    t->line = l->f->line;
    t->col = l->f->col;
    return t;
}

Token * copy_tk(Token *t) {
    Token *copy = malloc(sizeof(Token));
    *copy = *t;
    return copy;
}


// ---- Comments and Spaces ---------------------------------------------------

static void skip_line_comment(Lexer *l) {
    while (peek_ch(l->f) != EOF && peek_ch(l->f) != '\n') {
        next_ch(l->f);
    }
}

static void skip_block_comment(Lexer *l) {
    Token *err = new_tk(l, -1);
    int c = next_ch(l->f);
    while (c != EOF && !(c == '*' && peek_ch(l->f) == '/')) {
        c = next_ch(l->f);
    }
    if (c == EOF) {
        error_at(err, "unterminated block comment");
    }
    next_ch(l->f); // Skip '/' of closing '*/'
}

static int skip_space(Lexer *l) {
    int c = peek_ch(l->f);
    if (isspace(c) && c != '\n') { // Handle newlines separately
        next_ch(l->f);
        return 1;
    } else if (c == '/' && peek2_ch(l->f) == '/') {
        skip_line_comment(l);
        return 1;
    } else if (c == '/' && peek2_ch(l->f) == '*') {
        skip_block_comment(l);
        return 1;
    }
    return 0;
}

static int skip_spaces(Lexer *l) {
    int r = skip_space(l);
    while (skip_space(l));
    return r;
}


// ---- Values and Symbols ----------------------------------------------------

static Token * lex_ident(Lexer *l) {
    Token *t = new_tk(l, TK_IDENT);
    Buf *b = buf_new();
    while (isalnum(peek_ch(l->f)) || peek_ch(l->f) == '_') {
        int c = next_ch(l->f);
        buf_push(b, (char) c);
    }
    buf_push(b, '\0');
    t->ident = b->data;
    return t;
}

static Token * lex_num(Lexer *l) {
    Token *t = new_tk(l, TK_NUM);
    Buf *b = buf_new();
    int last = EOF;
    while (isalnum(peek_ch(l->f)) || peek_ch(l->f) == '.' ||
            (strchr("eEpP", last) && strchr("+-", peek_ch(l->f)))) {
        int c = next_ch(l->f);
        buf_push(b, (char) c);
        last = c;
    }
    buf_push(b, '\0');
    t->num = b->data;
    return t;
}

static int lex_hex_esc_seq(Lexer *l) {
    Token *err = new_tk(l, -1);
    Buf *b = buf_new();
    while (isxdigit(peek_ch(l->f))) {
        int c = next_ch(l->f);
        buf_push(b, (char) c);
    }
    if (b->len == 0) {
        error_at(err, "expected hexadecimal digit in '\\x' escape sequence");
    }
    buf_push(b, '\0');
    return (int) strtol(b->data, NULL, 16);
}

static int lex_oct_esc_seq(Lexer *l) {
    Token *err = new_tk(l, -1);
    Buf *b = buf_new();
    int i = 0;
    while (peek_ch(l->f) >= '0' && peek_ch(l->f) <= '7' && i < 3) { // Max 3 digits
        int c = next_ch(l->f);
        buf_push(b, (char) c);
        i++;
    }
    if (b->len == 0) {
        error_at(err, "expected octal digit in '\\0' escape sequence");
    }
    buf_push(b, '\0');
    return (int) strtol(b->data, NULL, 8);
}

static int is_valid_ucn(unsigned int c) {
    // U+D800 to U+DFFF are reserved for surrogate pairs.
    if (0xd800 <= c && c <= 0xdfff) return 0;
    // It's not allowed to encode ASCII characters using \U or \u. Some
    // characters not in the basic character set (C11 5.2.1p3) are excepted.
    return 0xa0 <= c || c == '$' || c == '@' || c == '`';
}

static int lex_universal_ch(Lexer *l, size_t len) { // [len] is 4 or 8
    Token *err = new_tk(l, -1);
    Buf *b = buf_new();
    while (isxdigit(peek_ch(l->f)) && b->len < len) {
        int c = next_ch(l->f);
        buf_push(b, (char) c);
    }
    if (b->len != len) {
        error_at(err, "expected %zu hexadecimal digits in '\\%c' escape sequence",
                 len, (len == 4 ? 'u' : 'U'));
    }
    buf_push(b, '\0');
    uint32_t r = strtol(b->data, NULL, 16);
    if (!is_valid_ucn(r)) {
        error_at(err, "invalid universal character '\\%c%0*x'",
                 (len == 4) ? 'u' : 'U', len, r);
    }
    return (int) r;
}

static int lex_esc_seq(Lexer *l, int *is_utf8) {
    *is_utf8 = 0;
    Token *err = new_tk(l, -1);
    int c = peek_ch(l->f);
    switch (c) {
        case '\'': case '"': case '?': case '\\': next_ch(l->f); return c;
        case 'a': next_ch(l->f); return '\a';
        case 'b': next_ch(l->f); return '\b';
        case 'f': next_ch(l->f); return '\f';
        case 'n': next_ch(l->f); return '\n';
        case 'r': next_ch(l->f); return '\r';
        case 't': next_ch(l->f); return '\t';
        case 'v': next_ch(l->f); return '\v';
        case 'u': *is_utf8 = 1; next_ch(l->f); return lex_universal_ch(l, 4);
        case 'U': *is_utf8 = 1; next_ch(l->f); return lex_universal_ch(l, 8);
        case 'x': next_ch(l->f); return lex_hex_esc_seq(l);
        case '0' ... '7': return lex_oct_esc_seq(l);
        default: error_at(err, "unknown escape sequence");
    }
}

static int lex_enc(Lexer *l) {
    switch (peek_ch(l->f)) {
        case 'L': next_ch(l->f); return ENC_WCHAR;
        case 'u': next_ch(l->f); return ENC_CHAR16;
        case 'U': next_ch(l->f); return ENC_CHAR32;
        default: return ENC_NONE;
    }
}

static Token * lex_ch(Lexer *l) {
    int enc = lex_enc(l);
    Token *t = new_tk(l, TK_CH);
    next_ch(l->f); // Skip '
    t->ch = next_ch(l->f);
    if (t->ch == '\\') {
        int is_utf8;
        t->ch = lex_esc_seq(l, &is_utf8);
    }
    if (!next_ch_is(l->f, '\'')) {
        error_at(t, "unterminated character literal");
    }
    t->enc = enc;
    return t;
}

static Token * lex_str(Lexer *l) {
    int enc = lex_enc(l);
    Token *t = new_tk(l, TK_STR);
    next_ch(l->f); // Skip "
    Buf *b = buf_new();
    int c = next_ch(l->f);
    while (c != EOF && c != '\"') {
        int is_utf8 = 0;
        if (c == '\\') {
            c = lex_esc_seq(l, &is_utf8);
        }
        if (is_utf8) {
            buf_push_utf8(b, c);
        } else {
            buf_push(b, (char) c);
        }
        c = next_ch(l->f);
    }
    if (c == EOF) {
        error_at(t, "unterminated string literal");
    }
    t->str = b->data;
    t->len = b->len; // NOT null terminated
    t->enc = enc;
    return t;
}

static Token * lex_sym(Lexer *l) {
    Token *t = new_tk(l, -1);
    int c = next_ch(l->f);
    t->k = c;
    switch (c) {
    case '<':
        if (next_ch_is(l->f, '=')) { t->k = TK_LE; break; }
        if (next_ch_is(l->f, '<')) {
            if (next_ch_is(l->f, '=')) { t->k = TK_A_SHL; break; }
            t->k = TK_SHL; break;
        }
        break;
    case '>':
        if (next_ch_is(l->f, '=')) { t->k = TK_GE; break; }
        if (next_ch_is(l->f, '>')) {
            if (next_ch_is(l->f, '=')) { t->k = TK_A_SHR; break; }
            t->k = TK_SHR; break;
        }
        break;
    case '=': if (next_ch_is(l->f, '=')) { t->k = TK_EQ; }  break;
    case '!': if (next_ch_is(l->f, '=')) { t->k = TK_NEQ; } break;
    case '&':
        if (next_ch_is(l->f, '&')) { t->k = TK_LOG_AND;   break; }
        if (next_ch_is(l->f, '=')) { t->k = TK_A_BIT_AND; break; }
        break;
    case '|':
        if (next_ch_is(l->f, '|')) { t->k = TK_LOG_OR;   break; }
        if (next_ch_is(l->f, '=')) { t->k = TK_A_BIT_OR; break; }
        break;
    case '^': if (next_ch_is(l->f, '=')) { t->k = TK_A_BIT_XOR; } break;
    case '+':
        if (next_ch_is(l->f, '=')) { t->k = TK_A_ADD; break; }
        if (next_ch_is(l->f, '+')) { t->k = TK_INC;   break; }
        break;
    case '-':
        if (next_ch_is(l->f, '=')) { t->k = TK_A_SUB; break; }
        if (next_ch_is(l->f, '-')) { t->k = TK_DEC;   break; }
        if (next_ch_is(l->f, '>')) { t->k = TK_ARROW; break; }
        break;
    case '*': if (next_ch_is(l->f, '=')) { t->k = TK_A_MUL; } break;
    case '/': if (next_ch_is(l->f, '=')) { t->k = TK_A_DIV; } break;
    case '%': if (next_ch_is(l->f, '=')) { t->k = TK_A_MOD; } break;
    case '.':
        if (peek_ch(l->f) == '.' && peek2_ch(l->f) == '.') {
            next_ch(l->f); next_ch(l->f);
            t->k = TK_ELLIPSIS;
        }
        break;
    case '#': if (next_ch_is(l->f, '#')) { t->k = TK_CONCAT; } break;
    default: break;
    }
    return t;
}


// ---- Tokens ----------------------------------------------------------------

static Token * lex_raw(Lexer *l) {
    if (!l->f) {
        return EOF_TK;
    }
    if (skip_spaces(l)) {
        return SPACE_TK;
    }
    int c = peek_ch(l->f);
    if (c == EOF) {
        return new_tk(l, TK_EOF);
    } else if (c == '\n') {
        next_ch(l->f);
        return NEWLINE_TK;
    } else if (isdigit(c) || (c == '.' && isdigit(peek2_ch(l->f)))) {
        return lex_num(l);
    } else if (c == '\'' ||
               ((c == 'L' || c == 'u' || c == 'U') && peek2_ch(l->f) == '\'')) {
        return lex_ch(l);
    } else if (c == '"' ||
               ((c == 'L' || c == 'u' || c == 'U') && peek2_ch(l->f) == '"')) {
        return lex_str(l);
    } else if (isalpha(c) || c == '_') {
        return lex_ident(l);
    } else {
        return lex_sym(l);
    }
}

Token * lex_tk(Lexer *l) {
    Token *t;
    if (vec_len(l->buf) > 0) {
        t = vec_pop(l->buf);
    } else {
        t = lex_raw(l);
        while (t->k == TK_SPACE) {
            t = lex_raw(l);
            t->has_preceding_space = 1;
        }
    }
    return t;
}

void undo_tk(Lexer *l, Token *t) {
    if (t->k == TK_EOF) {
        return;
    }
    vec_push(l->buf, t);
}

void undo_tks(Lexer *l, Vec *tks) {
    for (size_t i = 0; i < vec_len(tks); i++) {
        undo_tk(l, vec_get(tks, vec_len(tks) - i - 1));
    }
}


// ---- Special Preprocessor Functions ----------------------------------------

char * lex_rest_of_line(Lexer *l) {
    skip_spaces(l);
    Buf *b = buf_new();
    int c = next_ch(l->f);
    while (c != '\n' && c != EOF) {
        buf_push(b, (char) c);
        c = next_ch(l->f);
    }
    buf_push(b, '\0');
    return b->data;
}

char * lex_include_path(Lexer *l, int *search_cwd) {
    skip_spaces(l);
    Token *err = new_tk(l, -1);
    char close;
    if (next_ch_is(l->f, '"')) {
        close = '"';
        *search_cwd = 1;
    } else if (next_ch_is(l->f, '<')) {
        close = '>';
        *search_cwd = 0;
    } else {
        return NULL; // No include path
    }
    Buf *b = buf_new();
    int c = next_ch(l->f);
    while (c != close && c != EOF && c != '\n') {
        buf_push(b, (char) c);
        c = next_ch(l->f);
    }
    if (c != close) {
        error_at(err, "premature end of '#include' path");
    }
    if (b->len == 0) {
        error_at(err, "cannot have empty '#include' path");
    }
    buf_push(b, '\0');
    return b->data;
}

Token * glue_tks(Lexer *l, Token *t, Token *u) {
    Buf *b = buf_new();
    buf_print(b, token2str(t));
    buf_print(b, token2str(u));
    buf_push(b, '\0');
    undo_chs(l->f, b->data, b->len);
    Token *glued = lex_tk(l);
    glued->has_preceding_space = t->has_preceding_space;
    if (next_ch(l->f) != '\0') {
        error_at(t, "macro token concatenation formed invalid token '%s'", b->data);
    }
    return glued;
}


// ---- Token Printing --------------------------------------------------------

static char *TK_NAMES[TK_LAST - TK_FIRST] = {
    "<<", ">>", "==", "!=", "<=", ">=", "&&", "||", "+=", "-=", "*=", "/=",
    "%=", "&=", "|=", "^=", "<<=", ">>=", "++", "--", "->", "...", "##",
    "void", "char", "short", "int", "long", "float", "double", "signed",
    "unsigned", "struct", "union", "enum", "typedef", "auto", "static",
    "extern", "register", "inline", "const", "restrict", "volatile", "sizeof",
    "if", "else", "while", "do", "for", "switch", "case", "default", "break",
    "continue", "goto", "return", "number", "character", "string",
    "identifier", "end of file", "space", "newline", "macro parameter",
};

char * tk2str(int t) {
    Buf *b = buf_new();
    if (t < 256) {
        buf_print(b, quote_ch((char) t));
    } else {
        buf_print(b, TK_NAMES[t - TK_FIRST]);
    }
    buf_push(b, '\0');
    return b->data;
}

static void write_encoding(Buf *b, int enc) {
    switch (enc) {
        case ENC_NONE: break;
        case ENC_CHAR16: buf_push(b, 'u'); break;
        case ENC_CHAR32: buf_push(b, 'U'); break;
        case ENC_WCHAR:  buf_push(b, 'L'); break;
    }
}

char * token2str(Token *t) {
    Buf *b = buf_new();
    switch (t->k) {
    case TK_NUM:   buf_printf(b, "%s", t->num); break;
    case TK_IDENT: buf_printf(b, "%s", t->ident); break;
    case TK_CH:
        write_encoding(b, t->enc);
        buf_printf(b, "'%c'", quote_ch((char) t->ch));
        break;
    case TK_STR:
        write_encoding(b, t->enc);
        buf_printf(b, "\"%s\"", quote_str(t->str, t->len));
        break;
    default: return tk2str(t->k);
    }
    return b->data;
}

char * tk2pretty(int t) {
    Buf *b = buf_new();
    if (t < TK_NUM) {
        buf_push(b, '\'');
    }
    buf_print(b, tk2str(t));
    if (t < TK_NUM) {
        buf_push(b, '\'');
    }
    buf_push(b, '\0');
    return b->data;
}

char * token2pretty(Token *t) {
    Buf *b = buf_new();
    switch (t->k) {
    case TK_NUM:   buf_printf(b, "number '%s'", t->num); break;
    case TK_IDENT: buf_printf(b, "identifier '%s'", t->ident); break;
    case TK_CH:
        buf_print(b, "character ");
        write_encoding(b, t->enc);
        buf_printf(b, "'%c'", quote_ch((char) t->ch));
        break;
    case TK_STR:
        buf_print(b, "string ");
        write_encoding(b, t->enc);
        buf_printf(b, "\"%s\"", quote_str(t->str, t->len));
        break;
    default: return tk2pretty(t->k);
    }
    return b->data;
}
