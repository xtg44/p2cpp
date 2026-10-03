@echo off
rem compare.bat — differential harness for the --runtime backend (Windows).
rem
rem Mirrors tests/compare.sh: CPython reference vs p2cpp --runtime output.
rem
rem Usage:  tests\compare.bat [file.py ...]
setlocal enabledelayedexpansion

cd /d "%~dp0\.."

set "P2CPP=build\p2cpp.exe"
set "CXX=c++"
set "TMPDIR=%TEMP%\p2cpp_runtime_%RANDOM%"
if exist "%TMPDIR%" rmdir /s /q "%TMPDIR%"
mkdir "%TMPDIR%"

set /a pass=0
set /a fail=0
set "failed_names="

if "%~1"=="" (
    set "files="
    for %%f in (examples\*.py) do set "files=!files! %%f"
) else (
    set "files=%*"
)

for %%p in (%files%) do (
    set "name=%%~np"
    set "py=%%~dpnxp"
    set "exp=!TMPDIR!\!name!.expected"
    set "act=!TMPDIR!\!name!.actual"
    set "cpp=!TMPDIR!\!name!.cpp"
    set "bin=!TMPDIR!\!name!.exe"

    python "!py!" > "!exp!" 2>nul
    if !errorlevel! neq 0 (
        echo SKIP !py!  ^(CPython exited non-zero^)
        goto :next
    )

    rem Skip examples that import a native-only module (requests/re/json/csv).
    findstr /r /c:"import requests" /c:"import re" /c:"import json" /c:"import csv" /c:"from requests" /c:"from re" /c:"from json" /c:"from csv" "!py!" >nul 2>nul
    if !errorlevel!==0 (
        echo SKIP !py!  ^(uses a native-only module^)
        goto :next
    )

    "!P2CPP!" "!py!" --runtime -o "!cpp!" >nul 2>"!TMPDIR!\!name!.tperr"
    if !errorlevel! neq 0 (
        echo FAIL !py!  ^(transpile error^)
        type "!TMPDIR!\!name!.tperr" 2>nul
        set /a fail+=1
        set "failed_names=!failed_names! !py! (transpile)"
        goto :next
    )

    !CXX! -std=c++20 -O2 -o "!bin!" "!cpp!" 2>"!TMPDIR!\!name!.cerr"
    if !errorlevel! neq 0 (
        echo FAIL !py!  ^(C++ compile error^)
        type "!TMPDIR!\!name!.cerr" 2>nul
        set /a fail+=1
        set "failed_names=!failed_names! !py! (compile)"
        goto :next
    )

    "!bin!" > "!act!" 2>&1
    if !errorlevel! neq 0 (
        echo FAIL !py!  ^(runtime non-zero exit^)
        type "!act!" 2>nul
        set /a fail+=1
        set "failed_names=!failed_names! !py! (runtime)"
        goto :next
    )

    fc /w "!exp!" "!act!" >nul 2>nul
    if !errorlevel!==0 (
        echo PASS !py!
        set /a pass+=1
    ) else (
        echo FAIL !py!  ^(output mismatch^)
        fc /n "!exp!" "!act!" 2>nul
        set /a fail+=1
        set "failed_names=!failed_names! !py! (mismatch)"
    )

    :next
)

echo.
echo ==========================================
echo   passed: !pass!   failed: !fail!
if !fail! gtr 0 (
    echo   failures:
    for %%n in (!failed_names!) do echo     - %%n
    rmdir /s /q "%TMPDIR%"
    exit /b 1
)
rmdir /s /q "%TMPDIR%"
exit /b 0
