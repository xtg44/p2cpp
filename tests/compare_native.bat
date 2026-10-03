@echo off
rem compare_native.bat — differential harness for the --native backend (Windows).
rem
rem Mirrors tests/compare_native.sh: run each example with CPython for the
rem expected output, transpile with p2cpp --native, compile the generated C++,
rem run it, and diff the two.
rem
rem Usage:  tests\compare_native.bat [file.py ...]
setlocal enabledelayedexpansion

cd /d "%~dp0\.."

set "P2CPP=build\p2cpp.exe"
set "CXX=c++"
set "TMPDIR=%TEMP%\p2cpp_native_%RANDOM%"
if exist "%TMPDIR%" rmdir /s /q "%TMPDIR%"
mkdir "%TMPDIR%"

set /a pass=0
set /a fail=0
set /a unsupported=0
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

    "!P2CPP!" "!py!" --native -o "!cpp!" >nul 2>"!TMPDIR!\!name!.tperr"
    if !errorlevel! neq 0 (
        echo N/A  !py!  ^(not translatable by --native^)
        type "!TMPDIR!\!name!.tperr" | findstr /c:"error:" 2>nul
        set /a unsupported+=1
        goto :next
    )

    !CXX! -std=c++20 -O2 -o "!bin!" "!cpp!" 2>"!TMPDIR!\!name!.cerr"
    if !errorlevel! neq 0 (
        echo FAIL !py!  ^(generated C++ does not compile^)
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
echo   passed: !pass!   failed: !fail!   not translatable: !unsupported!
if !fail! gtr 0 (
    echo   failures:
    for %%n in (!failed_names!) do echo     - %%n
    rmdir /s /q "%TMPDIR%"
    exit /b 1
)
rmdir /s /q "%TMPDIR%"
exit /b 0
