cmake_minimum_required(VERSION 3.23)
project(Cosec C)

set(CMAKE_C_STANDARD 99)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra")
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -O0 -g")
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O3")

add_executable(Cosec
        src/main.c
        src/type.c src/type.h
        src/parse/file.c src/parse/file.h
        src/parse/lex.c src/parse/lex.h
        src/parse/parse.c src/parse/parse.h
        src/err.c src/err.h
        src/util.c src/util.h src/parse/debug.c src/parse/debug.h)
