
# Cosec C Compiler

Cosec is a toy optimising C compiler, written to learn more about compiler theory. Cosec only generates x86-64 assembly code (my MacBook Pro's architecture) in NASM format (my preferred assembler) for the moment.

My goals for the project are:

* **Maintainable**: the source code is clear and written in a clean, modular, easily maintainable, and extensible fashion.
* **Complete**: the compiler is compliant with the C99 standard (including a few extra GNU conveniences), can compile itself, and passes the LLVM and GCC test suites.
* **Technically unique**: the compiler uses a unqiue set of algorithms for parsing, optimisation, and code generation.
* **Standalone**: the compiler doesn't have any external dependencies.

Future features will include:

* **Three levels of IR**: Cosec uses 3 levels of intermediate representation (IR) to compile C code, including a high-level abstract syntax tree (AST), intermediate-level static single-assignment (SSA) form IR, and low-level assembly code IR.
* **Complex optimisations**: the compiler performs optimisation and analysis passes on the SSA IR to try and generate more efficient assembly.
* **Register allocation**: the compiler uses a complex graph-colouring algorithm for register allocation, including support for pre-coloured nodes, register coalescing, and spilling.
* **Tests**: the compiler comes with a suite of (relatively basic) tests run using a simple Python wrapper.


## Compiler Pipeline

1. **Lexing** (`lex.c`): the C source code is read and converted into a sequence of tokens.
2. **Preprocessing** (`pp.c`): resolves things like macros, `#include`s, and `#define`s.
3. **Parsing** (`parse.c`): builds an abstract syntax tree (AST) from the preprocessed tokens.
4. **Compilation** (`compile.c`): the static single assignment (SSA) form IR is generated from a well-formed AST.
5. **Optimisation and analysis**: various SSA IR analysis and optimisation passes are interleaved to try and generate more efficient assembly.
6. **Assembling** (`assemble.c`): lowers the three-address SSA IR to the two-address target assembly language IR (only x86-64 is supported for now) using an unlimited number of virtual registers.
7. **Register allocation** (`regalloc.c`): assigns physical registers to the virtual ones produced by the assembler.
8. **Encoding** (`encode.c`): writes the final assembly code to an output file (only NASM assembly format is supported for now).


## Building and Usage

You can build Cosec with [CMake](https://cmake.org/):

```bash
$ mkdir build
$ cd build
$ cmake -DCMAKE_BUILD_TYPE=Release ..
$ make
```

You can then compile a C file with:

```bash
$ ./Cosec test.c
```

This generates the output x86-64 assembly file `out.s` in NASM format. You can assemble and link this file (on macOS) with:

```bash
$ nasm -f macho64 test.s
$ ld test.o
```

On macOS Big Sur (my MacBook Pro's operating system), you need to add the following annoying linker arguments:

```bash
$ ld -lSystem -L/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/lib test.o
```

Hopefully in the future, you'll be able to build Cosec with itself!
