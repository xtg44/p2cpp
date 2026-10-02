#!/usr/bin/env bash
# compare.sh — end-to-end regression harness for p2cpp's --runtime backend.
#
# For every .py file given (or every file in examples/ by default) it:
#   1. runs it with CPython  -> expected output
#   2. transpiles it with p2cpp --runtime -> .cpp (+ the sibling py_runtime.h,
#      which p2cpp writes into the same temporary directory)
#   3. compiles that .cpp
#   4. runs the binary       -> actual output
#   5. diffs the two, reporting PASS/FAIL
#
# The default (direct) backend is exercised by compare_native.sh.
#
# Usage:  ./tests/compare.sh [file.py ...]
set -u
cd "$(dirname "$0")/.."

P2CPP=./build/p2cpp
CXX=${CXX:-c++}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

files=("$@")
if [ ${#files[@]} -eq 0 ]; then
    files=(examples/*.py)
fi

pass=0
fail=0
failed_names=()

for py in "${files[@]}"; do
    name=$(basename "$py" .py)
    exp="$TMP/$name.expected"
    act="$TMP/$name.actual"
    cpp="$TMP/$name.cpp"
    bin="$TMP/$name.bin"

    # 1. CPython reference
    if ! python3 "$py" > "$exp" 2>&1; then
        echo "SKIP $py  (CPython exited non-zero)"
        continue
    fi

    # The --runtime backend has no re/json/requests/csv; those are direct-mode
    # (--native) features. Skip examples that rely on them.
    if grep -qE '^[[:space:]]*(import|from)[[:space:]]+(requests|re|json|csv)([[:space:]]|$)' "$py"; then
        echo "SKIP $py  (uses a native-only module)"
        continue
    fi

    # 2. transpile (--runtime: the maximum-fidelity py:: backend)
    if ! "$P2CPP" "$py" --runtime -o "$cpp" > /dev/null 2> "$TMP/$name.tperr"; then
        echo "FAIL $py  (transpile error)"
        sed 's/^/     /' "$TMP/$name.tperr"
        fail=$((fail + 1)); failed_names+=("$py (transpile)")
        continue
    fi

    # 3. compile
    if ! "$CXX" -std=c++20 -O2 -o "$bin" "$cpp" 2> "$TMP/$name.cerr"; then
        echo "FAIL $py  (C++ compile error)"
        sed 's/^/     /' "$TMP/$name.cerr" | head -20
        fail=$((fail + 1)); failed_names+=("$py (compile)")
        continue
    fi

    # 4. run
    if ! "$bin" > "$act" 2>&1; then
        echo "FAIL $py  (runtime non-zero exit)"
        sed 's/^/     /' "$act" | head -20
        fail=$((fail + 1)); failed_names+=("$py (runtime)")
        continue
    fi

    # 5. diff
    if diff -u "$exp" "$act" > "$TMP/$name.diff"; then
        echo "PASS $py"
        pass=$((pass + 1))
    else
        echo "FAIL $py  (output mismatch)"
        sed 's/^/     /' "$TMP/$name.diff" | head -40
        fail=$((fail + 1)); failed_names+=("$py (mismatch)")
    fi
done

echo
echo "=========================================="
echo "  passed: $pass   failed: $fail"
if [ $fail -gt 0 ]; then
    printf '  failures:\n'
    for n in "${failed_names[@]}"; do printf '    - %s\n' "$n"; done
    exit 1
fi
exit 0
