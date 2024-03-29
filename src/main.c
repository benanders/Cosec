
#include <stdio.h>
#include <string.h>

#include "parse.h"
#include "compile.h"
#include "assemble.h"
#include "encode.h"
#include "error.h"
#include "debug.h"
#include "reg_alloc.h"

// Compile the generated assembly with (on my macOS machine):
//   nasm -f macho64 out.s
//   ld -L/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/lib -lSystem out.o
// The linker arguments are annoying but necessary
//
// See the equivalent LLVM IR with:
//   clang -emit-llvm -Xclang -disable-O0-optnone -S test.c
//   cat test.ll
// See the assembly code generated by clang with:
//   clang -S -masm=intel -O0 test.c
//   cat test.s
// See the result of optimisations (e.g., mem2reg) on the LLVM IR with:
//   clang -emit_ins-llvm -Xclang -disable-O0-optnone -S test.c
//   opt -S test.ll -mem2reg
//   cat test.ll
// See the assembly code generated by compiling LLVM IR (if you modify it) with:
//   llc --x86-asm-syntax=intel -O0 test.ll
//   cat test.s

static void print_version() {
    printf("cosec 0.3.0\n");
}

static void print_help() {
    printf("Usage: cosec [options] <file>\n");
    printf("\n");
    printf("Options:\n");
    printf("  --help, -h     Print this help message\n");
    printf("  --version, -v  Print the compiler version\n");
    printf("  -o <file>      Output assembly to <file>\n");
}

static void pipeline(char *in, char *out) {
    FILE *f_in = fopen(in, "r");
    if (!f_in) {
        error("can't read input file '%s'", in);
    }
    File *f = new_file(f_in, in);

    // Parser
    AstNode *ast = parse(f);
    print_ast(ast);
    printf("\n");

    // Compiler
    Vec *globals = compile(ast);
    print_ir(globals);
    printf("\n");

    // Assembler
    assemble(globals);
    encode_nasm(stdout, globals);

    // Register allocator
    reg_alloc(globals, 1);
    encode_nasm(stdout, globals);
    FILE *f_out = fopen(out, "w");
    if (!f_out) {
        error("can't open output file '%s'", out);
    }
    encode_nasm(f_out, globals);
}

int main(int argc, char *argv[]) {
    char *in = NULL, *out = "out.s";
    for (int i = 1; i < argc; i++) {
        char *arg = argv[i];
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            print_help();
            return 1;
        } else if (strcmp(arg, "--version") == 0 || strcmp(arg, "-v") == 0) {
            print_version();
            return 1;
        } else if (strcmp(arg, "-o") == 0) {
            if (i == argc - 1) {
                error("no file name after '-o'");
            }
            out = argv[++i];
        } else if (in) {
            error("multiple input files provided");
        } else {
            in = arg;
        }
    }
    if (!in) {
        error("no input files");
    }
    pipeline(in, out);
    return 0;
}
