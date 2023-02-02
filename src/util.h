
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
void   vec_push_all(Vec *v, Vec *to_append);
void * vec_pop(Vec *v);
void   vec_remove(Vec *v, size_t i);
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
void   map_remove(Map *m, char *k);
void * map_get(Map *m, char *k);
size_t map_len(Map *m);

typedef Vec Set;

Set * set_new();
Set * set_copy(Set *s);
int   set_has(Set *s, char *v);
void  set_put(Set **s, char *v);
void  set_union(Set **dst, Set *src);
void  set_intersection(Set **dst, Set *src);

char * quote_ch(char ch);
char * quote_str(char *s, size_t len);

#endif