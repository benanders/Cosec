cmake_minimum_required(VERSION 3.23)
project(Cosec C)

set(CMAKE_C_STANDARD 99)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra")
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -O0 -g")
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O3")

add_executable(Cosec src/main.c src/file.c src/file.h src/err.c src/err.h src/lex.c src/lex.h src/util.c src/util.h src/type.c src/type.h)
