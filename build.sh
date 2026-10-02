#!/usr/bin/env bash
# Build the p2cpp transpiler without CMake.
set -e
cd "$(dirname "$0")"
CXX=${CXX:-c++}
mkdir -p build
$CXX -std=c++20 -O2 -o build/p2cpp \
    src/main.cpp src/lexer.cpp src/parser.cpp src/codegen.cpp src/native.cpp
echo "built build/p2cpp"
