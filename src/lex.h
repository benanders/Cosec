
#ifndef COSEC_LEX_H
#define COSEC_LEX_H

#include "file.h"
#include "util.h"

typedef int TokenT;

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

    // Preprocessor only
    TK_SPACE,
    TK_NEWLINE,
    TK_MACRO_PARAM,

    TK_LAST, // For tables indexed by token
};

typedef enum { // In order of element size
    ENC_NONE,   // UTF-8 (default)
    ENC_CHAR16, // u"..." (UTF-16)
    ENC_WCHAR,  // L"..." (UTF-32)
    ENC_CHAR32, // U"..." (UTF-32)
} Enc;

typedef struct {
    TokenT k;
    File *f;
    int line, col;
    int has_preceding_space;
    union {
        char *ident; // TK_IDENT
        char *num;   // TK_NUM
        struct {
            Enc enc;
            union {
                int ch; // TK_CH
                struct { char *str; size_t len; }; // TK_STR
            };
        };
        size_t param_idx; // TK_MACRO_PARAM
    };
    Set *hide_set; // For macro expansion in the preprocessor
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

// Special preprocessor functions
char * lex_rest_of_line(Lexer *l); // For '#error' and '#warning'
char * lex_include_path(Lexer *l, int *search_cwd); // For '#include' and '#import'
Token * glue_tks(Lexer *l, Token *t1, Token *t2); // For '##' operator

// Token printing
char * tk2str(TokenT t);
char * token2str(Token *t);
char * tk2pretty(TokenT t);
char * token2pretty(Token *t);

#endif
