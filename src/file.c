
#include <stdlib.h>
#include <assert.h>
#include <string.h>

#include "file.h"

File * new_file(FILE *fp, char *name) {
    assert(fp);
    File *f = malloc(sizeof(File));
    f->f = fp;
    f->name = malloc(sizeof(char) * (strlen(name) + 1));
    strcpy(f->name, name);
    f->line = 1;
    f->col = 1;
    f->buf_len = 0;
    f->last = 0;
    return f;
}

static int read_ch_raw(File *f) {
    int c;
    if (f->buf_len > 0) {
        c = f->buf[--f->buf_len];
    } else {
        c = getc(f->f);
        if (c == EOF) {
            // End the file with '\n' (for the preprocessor)
            c = (f->last == '\n' || f->last == EOF) ? EOF : '\n';
        } else if (c == '\r') { // Turn '\r' into '\n'
            int c2 = getc(f->f);
            if (c2 != '\n') { // Turn '\r\n' into '\n'
                ungetc(c2, f->f);
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
    f->last = c;
    return c;
}

int next_ch(File *f) {
    while (1) {
        int c = read_ch_raw(f);
        if (c == '\\') {
            int c2 = read_ch_raw(f);
            if (c2 == '\n') {
                continue; // Escape newlines preceded by '\'
            } else {
                undo_ch(f, c2);
            }
        }
        return c;
    }
}

void undo_ch(File *f, int c) {
    if (c == EOF) {
        return;
    }
    assert(f->buf_len < MAX_FILE_PEEK);
    f->buf[f->buf_len++] = c;
    if (c == '\n') { // Undo line and column update
        f->col = 1;
        f->line--;
    } else {
        f->col--;
    }
}

int next_ch_is(File *f, int c) {
    if (peek_ch(f) == c) {
        next_ch(f);
        return 1;
    }
    return 0;
}

int peek_ch(File *f) {
    int c = next_ch(f);
    undo_ch(f, c);
    return c;
}

int peek2_ch(File *f) {
    int c1 = next_ch(f);
    int c2 = next_ch(f);
    undo_ch(f, c2);
    undo_ch(f, c1);
    return c2;
}
