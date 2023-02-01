
#ifndef COSEC_LEX_H
#define COSEC_LEX_H

#include "file.h"
#include "util.h"

enum {
    // Symbols
    TK_SHL = 256, // First 256 characters are for ASCII
    TK_SHR,

    TK_EQ,
    TK_NEQ,
    TK_LE,
    TK_GE,

    TK_LOG_AND,
    TK_LOG_OR,

    TK_A_ADD,
    TK_A_SUB,
    TK_A_MUL,
    TK_A_DIV,
    TK_A_MOD,
    TK_A_BIT_AND,
    TK_A_BIT_OR,
    TK_A_BIT_XOR,
    TK_A_SHL,
    TK_A_SHR,

    TK_INC,
    TK_DEC,

    TK_ARROW,

    // Types
    TK_VOID,
    TK_CHAR,
    TK_SHORT,
    TK_INT,
    TK_LONG,
    TK_FLOAT,
    TK_DOUBLE,
    TK_SIGNED,
    TK_UNSIGNED,
    TK_STRUCT,
    TK_UNION,
    TK_ENUM,

    TK_TYPEDEF,
    TK_AUTO,
    TK_STATIC,
    TK_EXTERN,
    TK_REGISTER,

    TK_INLINE,

    TK_CONST,
    TK_RESTRICT,
    TK_VOLATILE,

    // Statements
    TK_SIZEOF,
    TK_IF,
    TK_ELSE,
    TK_WHILE,
    TK_DO,
    TK_FOR,
    TK_SWITCH,
    TK_CASE,
    TK_DEFAULT,
    TK_BREAK,
    TK_CONTINUE,
    TK_GOTO,
    TK_RETURN,

    // Values
    TK_NUM,
    TK_CH,
    TK_STR,
    TK_IDENT,
    TK_EOF,
    TK_SPACE,
    TK_NEWLINE,

    TK_LAST, // For tables indexed by token
};

typedef struct {
    int k;
    File *f;
    int line, col;
    int is_line_start, has_space;
    union {
        struct { char *s; size_t len; }; // TK_IDENT, TK_STR, TK_NUM
        int ch; // TK_CH
    };
} Token;

typedef struct {
    File *f;
    Vec *buf;
} Lexer;

Lexer * new_lexer(File *f);
Token * lex_tk(Lexer *l);
void    undo_tk(Lexer *l, Token *t);

char * tk2str(int t);
char * token2str(Token *t);

#endif
