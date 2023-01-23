
#include <stdlib.h>
#include <ctype.h>
#include <string.h>

#include "lex.h"
#include "err.h"

#define MAX_SYMBOL_LEN 3
#define FIRST_TK       TK_EQ
#define FIRST_SYMBOL   TK_EQ
#define FIRST_KEYWORD  TK_VOID
#define FIRST_VALUE    TK_NUM

static char *SYMBOLS[] = {
    "==", "!=", "<=", ">=",
    "&&", "||",
    "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=",
    "<<", ">>", "++", "--",
    "->",
    NULL,
};

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

static char *VALUES[] = {
    "number", "character", "string", "identifier", "end of file", "space",
    NULL,
};

static Token *SPACE_TK = &(Token) { .t = TK_SPACE };

Lexer * new_lexer(File *f) {
    Lexer *l = calloc(1, sizeof(Lexer));
    l->f = f;
    l->buf = vec_new();
    return l;
}

static Token * new_tk(Lexer *l, int t) {
    Token *tk = calloc(1, sizeof(Token));
    tk->t = t;
    tk->f = l->f;
    tk->line = l->f->line;
    tk->col = l->f->col;
    return tk;
}


// ---- Comments and Spaces ---------------------------------------------------

static void skip_line_comment(Lexer *l) {
    while (peek_ch(l->f) != EOF && peek_ch(l->f) != '\n') {
        next_ch(l->f);
    }
}

static void skip_block_comment(Lexer *l) {
    Token *err = new_tk(l, EOF);
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
    int c = next_ch(l->f);
    if (c == EOF) {
        return 0;
    } else if (isspace(c)) {
        return 1;
    } else if (c == '/') {
        if (next_ch_is(l->f, '/')) {
            skip_line_comment(l);
            return 1;
        } else if (next_ch_is(l->f, '*')) {
            skip_block_comment(l);
            return 1;
        }
    }
    undo_ch(l->f, c);
    return 0;
}

static int skip_spaces(Lexer *l) {
    int r = 0;
    while (skip_space(l)) {
        r = 1;
    }
    return r;
}


// ---- Values and Symbols ----------------------------------------------------


static Token * lex_ident(Lexer *l) {
    Token *tk = new_tk(l, TK_IDENT);
    Buf *b = buf_new();
    int c = next_ch(l->f);
    while (isalnum(c) || c == '_') {
        buf_push(b, (char) c);
        c = next_ch(l->f);
    }
    undo_ch(l->f, c);
    buf_push(b, '\0');
    for (int i = 0; KEYWORDS[i]; i++) {
        if (strcmp(b->data, KEYWORDS[i]) == 0) {
            tk->t = FIRST_KEYWORD + i;
            break;
        }
    }
    tk->s = b->data;
    return tk;
}

static Token * lex_num(Lexer *l) {
    Token *tk = new_tk(l, TK_NUM);
    Buf *b = buf_new();
    int c = next_ch(l->f), last = c;
    while (isalnum(c) || c == '.' || (strchr("eEpP", last) && strchr("+-", c))) {
        buf_push(b, (char) c);
        last = c;
        c = next_ch(l->f);
    }
    undo_ch(l->f, c);
    buf_push(b, '\0');
    tk->s = b->data;
    return tk;
}

static int lex_hex_esc_seq(Lexer *l) {
    Token *err = new_tk(l, -1);
    Buf *b = buf_new();
    int c = next_ch(l->f);
    while (isxdigit(c)) {
        buf_push(b, (char) c);
        c = next_ch(l->f);
    }
    if (b->len == 0) {
        error_at(err, "expected hexadecimal digit in escape sequence");
    }
    undo_ch(l->f, c);
    buf_push(b, '\0');
    return (int) strtol(b->data, NULL, 16);
}

static int lex_oct_esc_seq(Lexer *l) {
    Token *err = new_tk(l, -1);
    Buf *b = buf_new();
    int i = 0;
    int c = next_ch(l->f);
    while (c >= '0' && c <= '7' && i < 3) { // Max of 3 octal digits
        buf_push(b, (char) c);
        c = next_ch(l->f);
        i++;
    }
    if (b->len == 0) {
        error_at(err, "expected octal digit in escape sequence");
    }
    undo_ch(l->f, c);
    buf_push(b, '\0');
    return (int) strtol(b->data, NULL, 8);
}

static int lex_esc_seq(Lexer *l) {
    Token *err = new_tk(l, -1);
    int c = next_ch(l->f);
    switch (c) {
        case '\'': case '"': case '?': case '\\': return c;
        case 'a': return '\a';
        case 'b': return '\b';
        case 'f': return '\f';
        case 'n': return '\n';
        case 'r': return '\r';
        case 't': return '\t';
        case 'v': return '\v';
        case 'x': return lex_hex_esc_seq(l);
        case '0' ... '7': undo_ch(l->f, c); return lex_oct_esc_seq(l);
        default: error_at(err, "unknown escape sequence");
    }
}

static Token * lex_ch(Lexer *l) {
    Token *tk = new_tk(l, TK_CH);
    next_ch(l->f); // Skip '
    tk->ch = next_ch(l->f);
    if (tk->ch == '\\') {
        tk->ch = lex_esc_seq(l);
    }
    if (!next_ch_is(l->f, '\'')) {
        error_at(tk, "unterminated character literal");
    }
    return tk;
}

static Token * lex_str(Lexer *l) {
    Token *tk = new_tk(l, TK_STR);
    next_ch(l->f); // Skip "
    Buf *b = buf_new();
    int c = next_ch(l->f);
    while (c != EOF && c != '\"') {
        if (c == '\\') {
            c = lex_esc_seq(l);
        }
        buf_push(b, (char) c);
        c = next_ch(l->f);
    }
    if (c == EOF) {
        error_at(tk, "unterminated string literal");
    }
    tk->s = b->data;
    tk->len = b->len; // NOT null terminated
    return tk;
}

static Token * lex_sym(Lexer *l) {
    Buf *b = buf_new();
    int c = next_ch(l->f);
    Token *tk = new_tk(l, c);
    while (c != EOF && b->len < MAX_SYMBOL_LEN) {
        buf_push(b, (char) c);
        c = next_ch(l->f);
    }
    for (int i = 0; SYMBOLS[i]; i++) {
        char *sym = SYMBOLS[i];
        int sym_len = (int) strlen(sym);
        if (sym_len <= b->len && strncmp(b->data, sym, sym_len) == 0) {
            tk->t = FIRST_SYMBOL + i;
            tk->len = sym_len;
            break;
        }
    }
    for (int i = b->len - 1; i >= tk->len; i--) {
        undo_ch(l->f, b->data[i]);
    }
    return tk;
}

static Token * lex_tk(Lexer *l) {
    if (skip_spaces(l)) {
        return SPACE_TK;
    }
    int c = peek_ch(l->f);
    if (c == EOF) {
        return new_tk(l, TK_EOF);
    } else if (isalpha(c) || c == '_') {
        return lex_ident(l);
    } else if (isdigit(c) || (c == '.' && isdigit(peek2_ch(l->f)))) {
        return lex_num(l);
    } else if (c == '\'') {
        return lex_ch(l);
    } else if (c == '"') {
        return lex_str(l);
    } else {
        return lex_sym(l);
    }
}


// ---- Tokens ----------------------------------------------------------------

Token * next_tk(Lexer *l) {
    if (vec_len(l->buf) > 0) {
        return vec_pop(l->buf);
    } else {
        Token *tk = lex_tk(l);
        while (tk->t == TK_SPACE) {
            tk = lex_tk(l);
        }
        return tk;
    }
}

void undo_tk(Lexer *l, Token *tk) {
    if (tk->t == TK_EOF) {
        return;
    }
    vec_push(l->buf, tk);
}

Token * next_tk_is(Lexer *l, int tk) {
    Token *t = next_tk(l);
    if (t->t == tk) {
        return t;
    } else {
        undo_tk(l, t);
        return NULL;
    }
}

Token * peek_tk(Lexer *l) {
    Token *t = next_tk(l);
    undo_tk(l, t);
    return t;
}

Token * peek2_tk(Lexer *l) {
    Token *t = next_tk(l);
    Token *t2 = peek_tk(l);
    undo_tk(l, t);
    return t2;
}

Token * expect_tk(Lexer *l, int tk) {
    Token *t = next_tk(l);
    if (t->t != tk) {
        error_at(t, "expected %s, found %s", tk2str(tk), token2str(t));
    } else {
        return t;
    }
}

char * tk2str(int tk) {
    Buf *b = buf_new();
    if (tk < FIRST_VALUE) {
        buf_push(b, '\'');
    }
    if (tk < FIRST_TK) {
        buf_print(b, quote_ch(tk));
    } else if (tk < FIRST_KEYWORD) {
        buf_print(b, SYMBOLS[tk - FIRST_SYMBOL]);
    } else if (tk < FIRST_VALUE) {
        buf_print(b, KEYWORDS[tk - FIRST_KEYWORD]);
    } else {
        buf_print(b, VALUES[tk - FIRST_VALUE]);
    }
    if (tk < FIRST_VALUE) {
        buf_push(b, '\'');
    }
    buf_push(b, '\0');
    return b->data;
}

char * token2str(Token *tk) {
    Buf *b = buf_new();
    switch (tk->t) {
        case TK_NUM:   buf_printf(b, "number '%s'", tk->s); break;
        case TK_CH:    buf_printf(b, "character '%c'", quote_ch(tk->ch)); break;
        case TK_STR:   buf_printf(b, "string \"%s\"", quote_str(tk->s, tk->len)); break;
        case TK_IDENT: buf_printf(b, "identifier '%s'", tk->s); break;
        default:       return tk2str(tk->t);
    }
    return b->data;
}
