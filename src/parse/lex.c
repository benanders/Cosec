
#include <stdlib.h>
#include <ctype.h>
#include <string.h>

#include "lex.h"
#include "../err.h"

#define FIRST_KEYWORD  TK_VOID

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

static Token *SPACE_TK = &(Token) { .k = TK_SPACE };

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
    Token *t = new_tk(l, TK_IDENT);
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
            t->k = FIRST_KEYWORD + i;
            break;
        }
    }
    t->s = b->data;
    return t;
}

static Token * lex_num(Lexer *l) {
    Token *t = new_tk(l, TK_NUM);
    Buf *b = buf_new();
    int c = next_ch(l->f), last = c;
    while (isalnum(c) || c == '.' || (strchr("eEpP", last) && strchr("+-", c))) {
        buf_push(b, (char) c);
        last = c;
        c = next_ch(l->f);
    }
    undo_ch(l->f, c);
    buf_push(b, '\0');
    t->s = b->data;
    return t;
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
    Token *t = new_tk(l, TK_CH);
    next_ch(l->f); // Skip '
    t->ch = next_ch(l->f);
    if (t->ch == '\\') {
        t->ch = lex_esc_seq(l);
    }
    if (!next_ch_is(l->f, '\'')) {
        error_at(t, "unterminated character literal");
    }
    return t;
}

static Token * lex_str(Lexer *l) {
    Token *t = new_tk(l, TK_STR);
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
        error_at(t, "unterminated string literal");
    }
    t->s = b->data;
    t->len = b->len; // NOT null terminated
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
    default: break;
    }
    return t;
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
    Token *t;
    if (vec_len(l->buf) > 0) {
        t = vec_pop(l->buf);
    } else {
        t = lex_tk(l);
        while (t->k == TK_SPACE) {
            t = lex_tk(l);
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

Token * next_tk_opt(Lexer *l, int k) {
    Token *t = next_tk(l);
    if (t->k == k) {
        return t;
    }
    undo_tk(l, t);
    return NULL;
}

Token * peek_tk(Lexer *l) {
    Token *t = next_tk(l);
    undo_tk(l, t);
    return t;
}

Token * peek_tk_is(Lexer *l, int k) {
    Token *t = peek_tk(l);
    return t->k == k ? t : NULL;
}

Token * peek2_tk(Lexer *l) {
    Token *t = next_tk(l);
    Token *t2 = peek_tk(l);
    undo_tk(l, t);
    return t2;
}

Token * peek2_tk_is(Lexer *l, int k) {
    Token *t = peek2_tk(l);
    return t->k == k ? t : NULL;
}

Token * expect_tk(Lexer *l, int k) {
    Token *t = next_tk(l);
    if (t->k != k) {
        error_at(t, "expected %s, found %s", tk2str(k), token2str(t));
    } else {
        return t;
    }
}

static char *TKS[] = {
    "<<", ">>", "==", "!=", "<=", ">=", "&&", "||", "+=", "-=", "*=", "/=",
    "%=", "&=", "|=", "^=", "<<=", ">>=", "++", "--", "->",
    "void", "char", "short", "int", "long", "float", "double", "signed",
    "unsigned", "struct", "union", "enum", "typedef", "auto", "static",
    "extern", "register", "inline", "const", "restrict", "volatile", "sizeof",
    "if", "else", "while", "do", "for", "switch", "case", "default", "break",
    "continue", "goto", "return", "number", "character", "string",
    "identifier", "end of file", "newline", "space",
};


char * tk2str(int t) {
    Buf *b = buf_new();
    if (t < FIRST_KEYWORD) {
        buf_push(b, '\'');
    }
    if (t < 256) {
        buf_print(b, quote_ch((char) t));
    } else {
        buf_print(b, TKS[t - TK_SHL]);
    }
    if (t < FIRST_KEYWORD) {
        buf_push(b, '\'');
    }
    buf_push(b, '\0');
    return b->data;
}

char * token2str(Token *t) {
    Buf *b = buf_new();
    switch (t->k) {
        case TK_NUM:   buf_printf(b, "number '%s'", t->s); break;
        case TK_CH:    buf_printf(b, "character '%c'", quote_ch((char) t->ch)); break;
        case TK_STR:   buf_printf(b, "string \"%s\"", quote_str(t->s, t->len)); break;
        case TK_IDENT: buf_printf(b, "identifier '%s'", t->s); break;
        default:       return tk2str(t->k);
    }
    return b->data;
}
