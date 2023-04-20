
#include <stdlib.h>

#include "file.h"

File * new_file(FILE *fp, char *path) {
    assert(fp);
    File *f = malloc(sizeof(File));
    f->fp = fp;
    f->path = str_copy(path);
    f->line = 1;
    f->col = 1;
    f->buf = buf_new();
    f->prev_ch = 0;
    return f;
}

static int read_ch_raw(File *f) {
    int c;
    if (f->buf->len > 0) {
        c = (int) buf_pop(f->buf);
    } else {
        c = getc(f->fp);
        if (c == EOF) {
            // End the file with '\n' (for the preprocessor)
            c = (f->prev_ch == '\n' || f->prev_ch == EOF) ? EOF : '\n';
        } else if (c == '\r') { // Turn '\r' into '\n'
            int c2 = getc(f->fp);
            if (c2 != '\n') { // Turn '\r\n' into '\n'
                ungetc(c2, f->fp);
            }
            c = '\n';
        }
    }
    if (c == '\n') {
        f->line++;
        f->col = 1;
    } else if (c != EOF) {
        f->col++;
    }
    f->prev_ch = c;
    return c;
}

void undo_ch(File *f, int c) {
    if (c == EOF) {
        return;
    }
    assert(c >= 0 && c <= 255);
    buf_push(f->buf, (char) c);
}

int next_ch(File *f) {
    int c = read_ch_raw(f);
    if (c == '\\') {
        int c2 = read_ch_raw(f);
        if (c2 == '\n') {
            return next_ch(f); // Escape newlines preceded by '\'
        } else {
            undo_ch(f, c2);
            f->col--;
        }
    }
    return c;
}

int next_ch_is(File *f, int c) {
    if (peek_ch(f) == c) {
        next_ch(f);
        return 1;
    }
    return 0;
}

int peek_ch(File *f) {
    int line = f->line, col = f->col;
    int c = next_ch(f);
    undo_ch(f, c);
    f->line = line, f->col = col;
    return c;
}

int peek2_ch(File *f) {
    int line = f->line, col = f->col;
    int c1 = next_ch(f);
    int c2 = next_ch(f);
    undo_ch(f, c2);
    undo_ch(f, c1);
    f->line = line, f->col = col;
    return c2;
}

void undo_chs(File *f, char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        char ch = s[len - i - 1];
        assert(ch != '\n'); // Cannot contain newlines
        undo_ch(f, ch);
        f->col--;
    }
}
