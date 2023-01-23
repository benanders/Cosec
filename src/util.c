
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>

#include "util.h"

Vec * vec_new() {
    Vec *v = malloc(sizeof(Vec));
    v->len = 0;
    v->max = 8;
    v->data = malloc(sizeof(void *) * v->max);
    return v;
}

static void vec_resize(Vec *v, int by) {
    if (v->len + by >= v->max) {
        while (v->max <= v->len + by) {
            v->max *= 2;
        }
        v->data = realloc(v->data, sizeof(void *) * v->max);
    }
}

void vec_push(Vec *v, void *elem) {
    vec_resize(v, 1);
    v->data[v->len++] = elem;
}

void * vec_pop(Vec *v) {
    if (v->len > 0) {
        return v->data[--v->len];
    } else {
        return NULL;
    }
}

int vec_len(Vec *v) {
    return v->len;
}

void * vec_get(Vec *v, int i) {
    return v->data[i];
}

void vec_empty(Vec *v) {
    v->len = 0;
}

Buf * buf_new() {
    Buf *b = malloc(sizeof(Buf));
    b->len = 0;
    b->max = 8;
    b->data = malloc(sizeof(char) * b->max);
    return b;
}

void buf_resize(Buf *b, int by) {
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
    int len = (int) strlen(s);
    buf_resize(b, len);
    strncpy(&b->data[b->len], s, len);
    b->len += len;
}

void buf_printf(Buf *b, char *fmt, ...) {
    va_list args;
    while (1) {
        int avail = b->max - b->len;
        va_start(args, fmt);
        int written = vsnprintf(b->data + b->len, avail, fmt, args);
        va_end(args);
        if (avail <= written) {
            buf_resize(b, written);
            continue;
        }
        b->len += written;
        break;
    }
}

static void quote_ch_to_buf(Buf *b, int ch) {
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
            buf_push(b, (char) ch);
        }
        break;
    }
}

char * quote_ch(int ch) {
    Buf *b = buf_new();
    quote_ch_to_buf(b, ch);
    return b->data;
}

char * quote_str(char *s, int len) {
    Buf *b = buf_new();
    for (int i = 0; i < len; i++) {
        quote_ch_to_buf(b, *(s++));
    }
    buf_push(b, '\0');
    return b->data;
}
