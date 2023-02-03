
#ifndef COSEC_FILE_H
#define COSEC_FILE_H

#include <stdio.h>

#define MAX_FILE_PEEK 3

typedef struct {
    FILE *f;
    char *name;
    int line, col;
    int buf[MAX_FILE_PEEK];
    int buf_len;
    int last;
} File;

File * new_file(FILE *fp, char *name);

int next_ch(File *f);
void undo_ch(File *f, int c);
int next_ch_is(File *f, int c);
int peek_ch(File *f);
int peek2_ch(File *f);

#endif
