
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>

#include "util.h"


// ---- Vector ----------------------------------------------------------------

Vec * vec_new() {
    Vec *v = malloc(sizeof(Vec));
    v->len = 0;
    v->max = 8;
    v->data = malloc(sizeof(void *) * v->max);
    return v;
}

static Vec * vec_copy(Vec *v) {
    Vec *copy = malloc(sizeof(Vec));
    copy->len = v->len;
    copy->max = v->max;
    copy->data = malloc(sizeof(void *) * v->max);
    memcpy(copy->data, v->data, v->len * sizeof(void *));
    return v;
}

static void vec_resize(Vec *v, size_t by) {
    if (v->len + by >= v->max) {
        while (v->max <= v->len + by) {
            v->max *= 2;
        }
        v->data = realloc(v->data, sizeof(void *) * v->max);
    }
}

void vec_push(Vec *v, void *elem) {
    if (!v) return;
    vec_resize(v, 1);
    v->data[v->len++] = elem;
}

void * vec_pop(Vec *v) {
    if (v && v->len > 0) {
        return v->data[--v->len];
    }
    return NULL;
}

void vec_remove(Vec *v, size_t i) {
    memcpy(&v->data[i], &v->data[i + 1], sizeof(void *) * (v->len - i - 1));
    v->len--;
}

size_t vec_len(Vec *v) {
    if (!v) return 0;
    return v->len;
}

void * vec_get(Vec *v, size_t i) {
    if (!v) return NULL;
    return v->data[i];
}

static void vec_empty(Vec *v) {
    v->len = 0;
}


// ---- String Buffer ---------------------------------------------------------

Buf * buf_new() {
    Buf *b = malloc(sizeof(Buf));
    b->len = 0;
    b->max = 8;
    b->data = malloc(sizeof(char) * b->max);
    return b;
}

void buf_resize(Buf *b, size_t by) {
    if (b->len + by >= b->max) {
        while (b->max <= b->len + by) {
            b->max *= 2;
        }
        b->data = realloc(b->data, sizeof(char) * b->max);
    }
}

void buf_push(Buf *b, char c) {
    buf_resize(b, 1);
    b->data[b->len++] = c;
}

void buf_print(Buf *b, char *s) {
    size_t len = strlen(s);
    buf_resize(b, len);
    strncpy(&b->data[b->len], s, len);
    b->len += len;
}

void buf_printf(Buf *b, char *fmt, ...) {
    va_list args;
    while (1) {
        size_t avail = b->max - b->len;
        va_start(args, fmt);
        size_t written = vsnprintf(b->data + b->len, avail, fmt, args);
        va_end(args);
        if (avail <= written) {
            buf_resize(b, written);
            continue;
        }
        b->len += written;
        break;
    }
}


// ---- Map -------------------------------------------------------------------

#define MAP_TOMBSTONE ((void *) (-1))

static uint32_t hash(char *p) { // FNV hash
    uint32_t r = 2166136261;
    for (; *p; p++) {
        r ^= *p;
        r *= 16777619;
    }
    return r;
}

Map * map_new() {
    Map *m = malloc(sizeof(Map));
    m->size = 16;
    m->num = 0;
    m->used = 0; // Includes 'TOMBSTONE' values
    m->k = calloc(m->size, sizeof(char *));
    m->v = calloc(m->size, sizeof(void *));
    return m;
}

static void map_rehash(Map *m) {
    if (m->used < (uint64_t) (m->size * 0.7)) {
        return;
    }
    size_t new_size = (m->num < m->size * 0.35) ? m->size : m->size * 2;
    char **k = calloc(new_size, sizeof(char *));
    void **v = calloc(new_size, sizeof(void *));
    size_t mask = new_size - 1;
    for (size_t i = 0; i < m->size; i++) {
        if (!m->k[i] || m->k[i] == MAP_TOMBSTONE) {
            continue;
        }
        uint32_t h = hash(m->k[i]) & mask;
        while (k[h]) {
            h = (h + 1) & mask;
        }
        k[h] = m->k[i];
        v[h] = m->v[i];
    }
    free(m->k);
    free(m->v);
    m->k = k;
    m->v = v;
    m->size = new_size;
    m->used = m->num; // Removed all 'TOMBSTONE' values
}

static uint32_t map_idx(Map *m, char *k) {
    size_t mask = m->size - 1;
    uint32_t h = hash(k) & mask;
    while (m->k[h]) {
        if (m->k[h] != MAP_TOMBSTONE && strcmp(k, m->k[h]) == 0) {
            break;
        }
        h = (h + 1) & mask;
    }
    return h;
}

void map_put(Map *m, char *k, void *v) {
    map_rehash(m);
    size_t mask = m->size - 1;
    uint32_t h = hash(k) & mask;
    while (m->k[h]) {
        if (m->k[h] == MAP_TOMBSTONE || strcmp(k, m->k[h]) == 0) {
            break;
        }
        h = (h + 1) & mask;
    }
    if (!m->k[h] || m->k[h] == MAP_TOMBSTONE) {
        m->v[h] = v; // Doesn't exist
        m->k[h] = k;
        m->num++;
        if (!m->k[h]) {
            m->used++;
        }
    } else {
        m->v[h] = v; // Already exists
    }
}

void * map_get(Map *m, char *k) {
    uint32_t h = map_idx(m, k);
    if (m->k[h]) {
        return m->v[h];
    } else {
        return NULL;
    }
}

void map_remove(Map *m, char *k) {
    uint32_t h = map_idx(m, k);
    m->k[h] = MAP_TOMBSTONE;
    m->v[h] = NULL;
    m->num--;
}


// ---- Set -------------------------------------------------------------------

Set * set_new() {
    return vec_new();
}

Set * set_copy(Set *s) {
    if (!s) return NULL;
    return vec_copy(s);
}

int set_has(Set *s, char *v) {
    if (!s) return 0;
    for (size_t i = 0; i < vec_len(s); i++) {
        char *vv = vec_get(s, i);
        if (strcmp(v, vv) == 0) {
            return 1;
        }
    }
    return 0;
}

void set_put(Set **s, char *v) {
    if (!s) return;
    if (!*s) *s = set_new();
    if (!set_has(*s, v)) {
        vec_push(*s, v);
    }
}

void set_union(Set **dst, Set *src) {
    if (!dst || !src) return;
    for (size_t i = 0; i < vec_len(src); i++) {
        char *v = vec_get(src, i);
        set_put(dst, v);
    }
}

void set_intersection(Set **dst, Set *src) {
    if (!dst) return;
    if (!src) { vec_empty(*dst); return; }
    if (!*dst) return;
    for (size_t i = 0; i < vec_len(*dst); i++) {
        char *v = vec_get(*dst, i);
        if (!set_has(src, v)) {
            vec_remove(*dst, i);
        }
    }
}


// ---- Character and String Output -------------------------------------------

static void quote_ch_to_buf(Buf *b, char ch) {
    switch (ch) {
    case '\\': buf_print(b, "\\\\"); break;
    case '\"': buf_print(b, "\\\""); break;
    case '\'': buf_print(b, "\\'"); break;
    case '\a': buf_print(b, "\\a"); break;
    case '\b': buf_print(b, "\\b"); break;
    case '\f': buf_print(b, "\\f"); break;
    case '\n': buf_print(b, "\\n"); break;
    case '\r': buf_print(b, "\\r"); break;
    case '\t': buf_print(b, "\\t"); break;
    case '\v': buf_print(b, "\\v"); break;
    case 0:    buf_print(b, "\\0"); break;
    default:
        if (iscntrl(ch)) {
            buf_printf(b, "\\%03o", ch);
        } else {
            buf_push(b, ch);
        }
        break;
    }
}

char * quote_ch(char ch) {
    Buf *b = buf_new();
    quote_ch_to_buf(b, ch);
    return b->data;
}

char * quote_str(char *s, size_t len) {
    Buf *b = buf_new();
    for (size_t i = 0; i < len; i++) {
        quote_ch_to_buf(b, *(s++));
    }
    buf_push(b, '\0');
    return b->data;
}
