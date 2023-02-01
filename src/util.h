
#ifndef COSEC_UTIL_H
#define COSEC_UTIL_H

#include <stddef.h>
#include <assert.h>

#define UNREACHABLE() (assert(0))
#define TODO() (assert(0))

typedef struct {
    void **data;
    size_t len, max;
} Vec;

Vec *  vec_new();
void   vec_push(Vec *v, void *elem);
void * vec_pop(Vec *v);
size_t vec_len(Vec *v);
void * vec_get(Vec *v, size_t i);

typedef struct {
    char *data;
    size_t len, max;
} Buf;

Buf * buf_new();
void  buf_push(Buf *b, char c);
void  buf_print(Buf *b, char *s);
void  buf_printf(Buf *b, char *fmt, ...);

typedef struct {
    char **k;
    void **v;
    size_t num, used, size;
} Map;

Map *  map_new();
void   map_put(Map *m, char *k, void *v);
void * map_get(Map *m, char *k);
void   map_remove(Map *m, char *k);

typedef Vec Set;

Set * set_new();
int   set_has(Set *s, char *v);
void  set_put(Set *s, char *v);
Set * set_union(Set *a, Set *b);
Set * set_intersection(Set *a, Set *b);

char * quote_ch(char ch);
char * quote_str(char *s, size_t len);

#endif