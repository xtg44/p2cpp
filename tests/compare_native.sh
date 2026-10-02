#!/usr/bin/env bash
# compare_native.sh — differential harness for the --native backend.
#
# Same idea as compare.sh, but the generated program must compile on its own
# with no runtime header at all, which is the point of --native.
#
# Usage:  ./tests/compare_native.sh [file.py ...]
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
unsupported=0
failed_names=()

for py in "${files[@]}"; do
    name=$(basename "$py" .py)
    exp="$TMP/$name.expected"
    act="$TMP/$name.actual"
    cpp="$TMP/$name.cpp"
    bin="$TMP/$name.bin"

    if ! python3 "$py" > "$exp" 2>&1; then
        echo "SKIP $py  (CPython exited non-zero)"
        continue
    fi

    if ! "$P2CPP" "$py" --native -o "$cpp" > /dev/null 2> "$TMP/$name.tperr"; then
        echo "N/A  $py  (not translatable by --native)"
        sed 's/^/     /' "$TMP/$name.tperr" | grep -E "error:" | head -3
        unsupported=$((unsupported + 1))
        continue
    fi

    if ! "$CXX" -std=c++20 -O2 -o "$bin" "$cpp" 2> "$TMP/$name.cerr"; then
        echo "FAIL $py  (generated C++ does not compile)"
        sed 's/^/     /' "$TMP/$name.cerr" | head -12
        fail=$((fail + 1)); failed_names+=("$py (compile)")
        continue
    fi

    if ! "$bin" > "$act" 2>&1; then
        echo "FAIL $py  (runtime non-zero exit)"
        sed 's/^/     /' "$act" | head -12
        fail=$((fail + 1)); failed_names+=("$py (runtime)")
        continue
    fi

    if diff -u "$exp" "$act" > "$TMP/$name.diff"; then
        echo "PASS $py"
        pass=$((pass + 1))
    else
        echo "FAIL $py  (output mismatch)"
        sed 's/^/     /' "$TMP/$name.diff" | head -30
        fail=$((fail + 1)); failed_names+=("$py (mismatch)")
    fi
done

echo
echo "=========================================="
echo "  passed: $pass   failed: $fail   not translatable: $unsupported"
if [ $fail -gt 0 ]; then
    printf '  failures:\n'
    for n in "${failed_names[@]}"; do printf '    - %s\n' "$n"; done
    exit 1
fi
exit 0
