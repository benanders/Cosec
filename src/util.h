
#ifndef COSEC_UTIL_H
#define COSEC_UTIL_H

#include <assert.h>

#define UNREACHABLE() (assert(0))

typedef struct {
    void **data;
    int len, max;
} Vec;

Vec *  vec_new();
void   vec_push(Vec *v, void *elem);
void * vec_pop(Vec *v);
int    vec_len(Vec *v);
void * vec_get(Vec *v, int i);
void   vec_empty(Vec *v);

typedef struct {
    char *data;
    int len, max;
} Buf;

Buf * buf_new();
void  buf_push(Buf *b, char c);
void  buf_print(Buf *b, char *s);
void  buf_printf(Buf *b, char *fmt, ...);

char * quote_ch(int ch);
char * quote_str(char *s, int len);

#endif