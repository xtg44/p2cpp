cmake_minimum_required(VERSION 3.16)
project(python_to_cpp CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(p2cpp
    src/main.cpp
    src/lexer.cpp
    src/parser.cpp
    src/codegen.cpp
    src/native.cpp
)

target_include_directories(p2cpp PRIVATE src)

if(MSVC)
    target_compile_options(p2cpp PRIVATE /W3)
else()
    target_compile_options(p2cpp PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
