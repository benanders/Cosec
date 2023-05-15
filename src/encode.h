
#ifndef COSEC_ENCODE_H
#define COSEC_ENCODE_H

#include "assemble.h"

void encode_nasm(FILE *out, Vec *globals);

// For register allocation debugging
void encode_gpr(FILE *out, int reg, int size);
void encode_xmm(FILE *out, int reg);

#endif
