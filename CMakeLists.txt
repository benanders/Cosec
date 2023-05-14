cmake_minimum_required(VERSION 3.23)
project(Cosec C)

set(CMAKE_C_STANDARD 99)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra")
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -O0 -g")
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O3")

add_executable(Cosec
        src/main.c
        src/file.c src/file.h
        src/lex.c src/lex.h
        src/pp.c src/pp.h
        src/parse.c src/parse.h
        src/compile.c src/compile.h
        src/assemble.c src/assemble.h
        src/encode.c src/encode.h
        src/error.c src/error.h
        src/debug.c src/debug.h
        src/util.c src/util.h)

include(FindPython3) # Python for testing
enable_testing()
add_test(NAME test_all COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/RunTests.py
        $<TARGET_FILE:Cosec>
        ${PROJECT_SOURCE_DIR}/tests)
