
#ifndef COSEC_UTIL_H
#define COSEC_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <assert.h>

#define UNREACHABLE() (assert(0))
#define TODO() (assert(0))

// Vector
typedef struct {
    void **data;
    size_t len, max;
} Vec;

Vec * vec_new();
void vec_push(Vec *v, void *elem);
void vec_push_all(Vec *v, Vec *to_append);
void vec_put(Vec *v, size_t i, void *elem);
void * vec_pop(Vec *v);
void * vec_remove(Vec *v, size_t i);
size_t vec_len(Vec *v);
void * vec_get(Vec *v, size_t i);
void * vec_head(Vec *v);
void * vec_tail(Vec *v);
void vec_empty(Vec *v);

// String buffer
typedef struct {
    char *data;
    size_t len, max;
} Buf;

Buf * buf_new();
void buf_push(Buf *b, char c);
void buf_push_utf8(Buf *b, uint32_t c);
char buf_pop(Buf *b);
void buf_print(Buf *b, char *s);
void buf_nprint(Buf *b, char *s, size_t len);
void buf_printf(Buf *b, char *fmt, ...);

// Map
typedef struct {
    char **k;
    void **v;
    size_t num, used, size;
} Map;

Map * map_new();
void map_put(Map *m, char *k, void *v);
void map_remove(Map *m, char *k);
void * map_get(Map *m, char *k);
size_t map_count(Map *m);

// Set
typedef Vec Set;

Set * set_new();
int set_has(Set *s, char *v);
void set_put(Set **s, char *v);
void set_union(Set **dst, Set *src);
void set_intersection(Set **dst, Set *src);

// String manipulation
char * str_copy(char *s);
char * str_ncopy(char *s, size_t len);
char * prepend_underscore(char *s);

char * quote_ch(char ch);
char * quote_str(char *s, size_t len);

uint16_t * utf8_to_utf16(char *s, size_t len, size_t *buf_len);
uint32_t * utf8_to_utf32(char *s, size_t len, size_t *buf_len);

// Path manipulation
char * concat_paths(char *dir, char *file);
char * get_dir(char *path);
char * full_path(char *path);

#endif