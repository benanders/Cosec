
#ifndef COSEC_FILE_H
#define COSEC_FILE_H

#include <stdio.h>

#include "util.h"

typedef struct {
    FILE *f;
    char *name;
    int line, col;
    Buf *buf;
    int last;
} File;

File * new_file(FILE *fp, char *name);

int next_ch(File *f);
void undo_ch(File *f, int c);
void undo_chs(File *f, char *s, size_t len);
int next_ch_is(File *f, int c);
int peek_ch(File *f);
int peek2_ch(File *f);

#endif
