@echo off
rem build.bat — build the p2cpp transpiler on Windows.
rem
rem Prefers CMake (works with MSVC or MinGW). If CMake is unavailable it falls
rem back to compiling the five source files directly with the first compiler it
rem can find (cl / g++ / clang++).
rem
rem The resulting executable is build\p2cpp.exe (Release configuration).
setlocal enabledelayedexpansion

cd /d "%~dp0"

where cmake >nul 2>nul
if %errorlevel%==0 (
    echo [build.bat] using CMake...
    cmake -B build -DCMAKE_BUILD_TYPE=Release
    if errorlevel 1 exit /b 1
    cmake --build build --config Release
    if errorlevel 1 exit /b 1
    echo [build.bat] built build\p2cpp.exe
    exit /b 0
)

echo [build.bat] cmake not found; falling back to direct compilation...

rem ---- Direct compilation fallback ----
set "SRC=src\main.cpp src\lexer.cpp src\parser.cpp src\codegen.cpp src\native.cpp"
set "CXXFLAGS=/std:c++20 /O2 /EHsc /I src"

if not exist build mkdir build

rem Try MSVC cl first (requires a Developer Command Prompt or vcvars run first).
where cl >nul 2>nul
if %errorlevel%==0 (
    echo [build.bat] compiling with MSVC cl...
    cl %CXXFLAGS% /Fe:build\p2cpp.exe %SRC%
    if errorlevel 1 exit /b 1
    echo [build.bat] built build\p2cpp.exe
    exit /b 0
)

rem Then MinGW g++.
where g++ >nul 2>nul
if %errorlevel%==0 (
    echo [build.bat] compiling with g++...
    g++ -std=c++20 -O2 -I src -o build\p2cpp.exe %SRC%
    if errorlevel 1 exit /b 1
    echo [build.bat] built build\p2cpp.exe
    exit /b 0
)

rem Then clang++.
where clang++ >nul 2>nul
if %errorlevel%==0 (
    echo [build.bat] compiling with clang++...
    clang++ -std=c++20 -O2 -I src -o build\p2cpp.exe %SRC%
    if errorlevel 1 exit /b 1
    echo [build.bat] built build\p2cpp.exe
    exit /b 0
)

echo [build.bat] no C++ compiler found ^(cl / g++ / clang++^) and no cmake.
echo [build.bat] Install Visual Studio Build Tools, MinGW-w64, or CMake, then retry.
exit /b 1
