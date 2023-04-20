
#ifndef COSEC_FILE_H
#define COSEC_FILE_H

#include <stdio.h>

#include "util.h"

typedef struct {
    FILE *fp;
    char *name; // Not the full path to the file (e.g., 'stdlib.h')
    int line, col;
    Buf *buf;
    int prev_ch;
} File;

File * new_file(FILE *fp, char *path);
int next_ch(File *f);
int peek_ch(File *f);
int peek2_ch(File *f);
int next_ch_is(File *f, int c);

// Used by the preprocessor when gluing tokens together with '##'. CANNOT
// contain newlines (can't reliably update 'f->col' for errors)
void undo_chs(File *f, char *s, size_t len);

#endif
