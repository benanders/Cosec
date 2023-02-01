
#ifndef COSEC_ERR_H
#define COSEC_ERR_H

#include "lex.h"

void error(char *fmt, ...) __attribute__((noreturn));
void error_at(Token *tk, char *fmt, ...) __attribute__((noreturn));

void warning_at(Token *t, char *fmt, ...);

#endif
