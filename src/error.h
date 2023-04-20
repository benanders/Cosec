
#ifndef COSEC_ERROR_H
#define COSEC_ERROR_H

#include "lex.h"

void error(char *fmt, ...) __attribute__((noreturn));
void error_at(Token *tk, char *fmt, ...) __attribute__((noreturn));
void warning_at(Token *tk, char *fmt, ...);

#endif
