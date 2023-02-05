
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <limits.h>
#include <unistd.h>
#include <errno.h>
#include <stddef.h>

#include "util.h"
#include "err.h"


// ---- Vector ----------------------------------------------------------------

Vec * vec_new() {
    Vec *v = malloc(sizeof(Vec));
    v->len = 0;
    v->max = 8;
    v->data = malloc(sizeof(void *) * v->max);
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

void vec_push_all(Vec *v, Vec *to_append) {
    for (size_t i = 0; i < vec_len(to_append); i++) {
        void *elem = vec_get(to_append, i);
        vec_push(v, elem);
    }
}

void * vec_pop(Vec *v) {
    if (v && v->len > 0) {
        return v->data[--v->len];
    }
    return NULL;
}

void * vec_remove(Vec *v, size_t i) {
    void *elem = v->data[i];
    memcpy(&v->data[i], &v->data[i + 1], sizeof(void *) * (v->len - i - 1));
    v->len--;
    return elem;
}

size_t vec_len(Vec *v) {
    if (!v) return 0;
    return v->len;
}

void * vec_get(Vec *v, size_t i) {
    if (!v || i >= v->len) return NULL;
    return v->data[i];
}

void * vec_head(Vec *v) {
    if (!v || v->len == 0) return NULL;
    return vec_get(v, 0);
}

void * vec_tail(Vec *v) {
    if (!v || v->len == 0) return NULL;
    return vec_get(v, v->len - 1);
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

void buf_push_utf8(Buf *b, uint32_t c) {
    if (c < 0x80) {
        buf_push(b, (int8_t) c);
    } else if (c < 0x800) {
        buf_push(b, (int8_t) (0xc0 | (c >> 6)));
        buf_push(b, (int8_t) (0x80 | (c & 0x3f)));
    } else if (c < 0x10000) {
        buf_push(b, (int8_t) (0xe0 | (c >> 12)));
        buf_push(b, (int8_t) (0x80 | ((c >> 6) & 0x3f)));
        buf_push(b, (int8_t) (0x80 | (c & 0x3f)));
    } else if (c < 0x200000) {
        buf_push(b, (int8_t) (0xf0 | (c >> 18)));
        buf_push(b, (int8_t) (0x80 | ((c >> 12) & 0x3f)));
        buf_push(b, (int8_t) (0x80 | ((c >> 6) & 0x3f)));
        buf_push(b, (int8_t) (0x80 | (c & 0x3f)));
    } else {
        UNREACHABLE();
    }
}

char buf_pop(Buf *b) {
    assert(b->len > 0);
    return b->data[--b->len];
}

void buf_print(Buf *b, char *s) {
    size_t len = strlen(s);
    buf_nprint(b, s, len);
}

void buf_nprint(Buf *b, char *s, size_t len) {
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
    if (m->used < m->size * 0.7) {
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

void map_remove(Map *m, char *k) {
    uint32_t h = map_idx(m, k);
    m->k[h] = MAP_TOMBSTONE;
    m->v[h] = NULL;
    m->num--;
}

void * map_get(Map *m, char *k) {
    uint32_t h = map_idx(m, k);
    if (m->k[h]) {
        return m->v[h];
    } else {
        return NULL;
    }
}

size_t map_len(Map *m) {
    return m->num;
}


// ---- Set -------------------------------------------------------------------

Set * set_new() {
    return vec_new();
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
    if (!dst || !*dst) return;
    if (!src) { vec_empty(*dst); return; }
    for (size_t i = 0; i < vec_len(*dst); i++) {
        char *v = vec_get(*dst, i);
        if (!set_has(src, v)) {
            vec_remove(*dst, i);
        }
    }
}


// ---- String Manipulation ---------------------------------------------------

char * str_copy(char *s) {
    size_t len = strlen(s);
    return str_ncopy(s, len);
}

char * str_ncopy(char *s, size_t len) {
    char *r = malloc(sizeof(char) * (len + 1));
    strncpy(r, s, len);
    s[len] = '\0';
    return r;
}

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

static int count_leading_ones(char c) {
    for (int i = 7; i >= 0; i--) {
        if ((c & (1 << i)) == 0) {
            return 7 - i;
        }
    }
    return 8;
}

static int read_rune(char *s, const char *end, uint32_t *rune) {
    int ones = count_leading_ones(s[0]);
    if (ones == 0) {
        *rune = (uint32_t) s[0];
        return 1;
    }
    if (s + ones >= end) {
        return -1;
    }
    for (int i = 1; i < ones; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            return -1;
        }
    }
    switch (ones) {
    case 2:
        *rune = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F);
        return 2;
    case 3:
        *rune = ((s[0] & 0xF) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        return 3;
    case 4:
        *rune = ((s[0] & 0x7) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        return 4;
    default: return -1;
    }
}

uint16_t * utf8_to_utf16(char *s, size_t len, size_t *buf_len) {
    uint16_t *b = malloc(sizeof(uint16_t) * len);
    size_t n = 0;
    char *p = s, *end = s + len;
    while (p < end) {
        uint32_t rune;
        int bytes = read_rune(p, end, &rune);
        if (bytes < 0) {
            return NULL;
        }
        p += bytes;
        if (rune < 0x10000) {
            b[n++] = rune;
        } else {
            b[n++] = (rune >> 10) + 0xd7c0;
            b[n++] = (rune & 0x3ff) + 0xdc00;
        }
    }
    *buf_len = n;
    return b;
}

uint32_t * utf8_to_utf32(char *s, size_t len, size_t *buf_len) {
    uint32_t *b = malloc(sizeof(uint32_t) * len);
    size_t n = 0;
    char *p = s, *end = s + len;
    while (p < end) {
        uint32_t rune;
        int bytes = read_rune(p, end, &rune);
        if (bytes < 0) {
            return NULL;
        }
        p += bytes;
        b[n++] = rune;
    }
    *buf_len = n;
    return b;
}


// ---- Path Manipulation -----------------------------------------------------

char * concat_paths(char *dir, char *file) {
    size_t dir_len = strlen(dir), file_len = strlen(file);
    char *concat = malloc(sizeof(char) * (dir_len + file_len + 2));
    strcpy(concat, dir);
    concat[dir_len] = '/';
    strcpy(&concat[dir_len + 1], file);
    return concat;
}

char * get_dir(char *path) {
    size_t len = strlen(path);
    if (len == 0 || strcmp(path, "/") == 0) return path;
    if (path[len - 1] == '/') len--;
    size_t last = 0;
    for (; last < len && path[last] != '/'; last++);
    if (last == len) return "."; // No '/'
    return str_ncopy(path, last);
}

static char * simplify_path(char *p) {
    assert(*p == '/');
    char *s = malloc(sizeof(char) * (strlen(p) + 1));
    char *q = s;
    *q++ = *p++; // '/'
    while (*p) {
        if (*p == '/') {
            p++;
        } else if (memcmp(p, "./", 2) == 0) {
            p += 2;
        } else if (memcmp(p, "../", 3) == 0) {
            p += 3;
            if (q == s + 1) continue;
            for (q--; q[-1] != '/'; q--);
        } else {
            while (*p && *p != '/') {
                *q++ = *p++;
            }
            if (*p == '/') {
                *q++ = *p++;
            }
        }
    }
    *q = '\0';
    return s;
}

char * full_path(char *path) {
    static char cwd[PATH_MAX];
    if (path[0] == '/') {
        return simplify_path(path);
    }
    if (!*cwd && !getcwd(cwd, PATH_MAX)) {
        error("cannot get current working directory: %s", strerror(errno));
    }
    return simplify_path(concat_paths(cwd, path));
}
