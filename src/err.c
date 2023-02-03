
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

#if !defined(_WIN32) && !defined(_WIN64)
#include <unistd.h>
#endif

#include "err.h"

#define COLOUR_CLEAR  0
#define COLOUR_BOLD   1
#define COLOUR_RED    31
#define COLOUR_GREEN  32
#define COLOUR_YELLOW 33
#define COLOUR_BLUE   34
#define COLOUR_WHITE  37

static int supports_color() {
#if defined(_WIN32) || defined(_WIN64)
    return 0; // Don't bother on Windows
#else
    // Output in color if stdout is a terminal; proper way would be to check
    // TERM or use terminfo database
    static int supported = -1;
    if (supported < 0) {
        supported = isatty(fileno(stdout));
    }
    return supported;
#endif
}

static void print_colour(int colour) {
    if (!supports_color()) {
        return;
    }
    // Changing the text formatting attributes involves printing a special
    // terminal escape sequence (`\033[`), and then a command (`%dm`)
    printf("\033[%dm", colour);
}

static void print_error_header() {
    print_colour(COLOUR_RED);
    print_colour(COLOUR_BOLD);
    printf("error: ");
    print_colour(COLOUR_WHITE);
}

void error(char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    print_error_header();
    vprintf(fmt, args);
    print_colour(COLOUR_CLEAR);
    printf("\n");
    va_end(args);
    exit(1);
}

static void print_tk(Token *tk) {
    print_colour(COLOUR_BLUE);
    printf(" --> ");
    print_colour(COLOUR_CLEAR);
    if (tk->f && tk->f->name) {
        printf("%s", tk->f->name);
    } else {
        printf("<unknown>");
    }
    if (tk->line > 0) {
        printf(":%d", tk->line);
    }
    if (tk->col > 0) {
        printf(":%d", tk->col);
    }
    printf("\n");
}

void error_at(Token *tk, char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    print_error_header();
    vprintf(fmt, args);
    print_colour(COLOUR_CLEAR);
    printf("\n");
    print_tk(tk);
    va_end(args);
    exit(1);
}

static void print_warning_header() {
    print_colour(COLOUR_YELLOW);
    print_colour(COLOUR_BOLD);
    printf("warning: ");
    print_colour(COLOUR_WHITE);
}

void warning_at(Token *t, char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    print_warning_header();
    vprintf(fmt, args);
    print_colour(COLOUR_CLEAR);
    printf("\n");
    print_tk(t);
    va_end(args);
}
