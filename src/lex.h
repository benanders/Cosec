
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
    TK_ELLIPSIS,
    TK_CONCAT, // '##'; for preprocessor only

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

    // For preprocessor only
    TK_SPACE,
    TK_NEWLINE,
    TK_MACRO_PARAM,

    TK_LAST, // For tables indexed by token
};

enum {
    ENC_NONE,   // (UTF-8)
    ENC_WCHAR,  // L"..."
    ENC_CHAR16, // u"..."
    ENC_CHAR32, // U"..."
};

typedef struct {
    int k;
    File *f;
    int line, col;
    int has_preceding_space;
    union {
        char *ident; // TK_IDENT
        char *num;   // TK_NUM
        struct { char *str; size_t len; int str_enc; }; // TK_STR
        struct { int ch; int ch_enc; }; // TK_CH
        int param;   // TK_MACRO_PARAM
    };
    Set *hide_set; // For the preprocessor
} Token;

typedef struct Lexer {
    struct Lexer *parent; // For '#include's in the preprocessor
    File *f;
    Vec *buf;
} Lexer;

Lexer * new_lexer(File *f);
Token * copy_tk(Token *t);

Token * lex_tk(Lexer *l);
void undo_tk(Lexer *l, Token *t);
void undo_tks(Lexer *l, Vec *tks);

char * lex_rest_of_line(Lexer *l);
char * lex_include_path(Lexer *l, int *search_local);
Token * glue_tks(Lexer *l, Token *t, Token *u);

char * tk2str(int t);
char * token2str(Token *t);
char * tk2pretty(int t);
char * token2pretty(Token *t);

#endif
