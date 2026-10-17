#!/bin/sh
# Self-test suite for the C workflow engine.
# Usage: sh selftest/run.sh   (or: make test)
cd "$(dirname "$0")/.." || exit 1
BIN=./workflow

if [ ! -x "$BIN" ]; then
    echo "error: $BIN not found; run 'make' first" >&2
    exit 1
fi

pass=0
fail=0

check_json() {
    # $1 = json text; validates it parses as exactly one JSON document
    if command -v python3 >/dev/null 2>&1; then
        printf '%s' "$1" | python3 -c 'import json,sys; json.load(sys.stdin)' >/dev/null 2>&1
        return $?
    fi
    return 0
}

check() {
    file=$1
    want_rc=$2
    needle=$3
    out=$("$BIN" run "$file" 2>/dev/null)
    rc=$?
    if [ "$rc" -ne "$want_rc" ]; then
        echo "FAIL $file: exit $rc (expected $want_rc)"
        fail=$((fail + 1))
        return
    fi
    case "$out" in
        *"$needle"*) ;;
        *)
            echo "FAIL $file: output does not contain: $needle"
            echo "  got: $out"
            fail=$((fail + 1))
            return
            ;;
    esac
    if ! check_json "$out"; then
        echo "FAIL $file: stdout is not valid JSON"
        fail=$((fail + 1))
        return
    fi
    pass=$((pass + 1))
    echo "ok   $file (exit $rc)"
}

# SPEC worked examples
check selftest/example_a.json 0 '"total":6'
check selftest/example_b.json 0 '"skipped":["y"]'
check selftest/example_c.json 0 '"flaky":"ok"'

# Functional pipelines
check selftest/01_math_pipeline.json 0 '"rem":4'
check selftest/02_string_pipeline.json 0 '"len":15'
check selftest/03_object_ops.json 0 '"picked":{"a":1,"c":3,"nope":null}'
check selftest/04_skipped_and_conditions.json 0 '"skipped":["too_small"]'
check selftest/05_retry_success.json 0 '"after":"ok!"'
check selftest/05b_retry_exhausted.json 1 '"code":"STEP_FAILED"'
check selftest/06_arrays.json 0 '"sorted":[1,3,"a","b",false,true,null,[1],{"a":1}]'
check selftest/07_references.json 0 '"transitive":"ann"'
check selftest/10_utilities.json 0 '"ty":"number"'

# Runtime error codes (exit 1)
check selftest/08a_overflow.json 1 '"code":"ARITHMETIC_OVERFLOW"'
check selftest/08b_division_by_zero.json 1 '"code":"DIVISION_BY_ZERO"'
check selftest/08c_empty_array.json 1 '"code":"EMPTY_ARRAY"'
check selftest/08d_invalid_arg.json 1 '"code":"INVALID_ARG"'
check selftest/08e_bad_reference.json 1 '"code":"INVALID_REFERENCE"'
check selftest/08f_skipped_reference.json 1 '"code":"SKIPPED_REFERENCE"'

# Validation error codes (exit 2)
check selftest/09a_cycle.json 2 '"code":"CYCLIC_DEPENDENCY"'
check selftest/09b_unknown_dependency.json 2 '"code":"UNKNOWN_DEPENDENCY"'
check selftest/09c_bad_version.json 2 '"code":"INVALID_WORKFLOW"'

# stdin mode
out=$(cat selftest/example_a.json | "$BIN" run - 2>/dev/null)
rc=$?
if [ "$rc" -eq 0 ] && check_json "$out"; then
    case "$out" in
        *'"total":6'*)
            pass=$((pass + 1))
            echo "ok   stdin mode (exit $rc)"
            ;;
        *)
            fail=$((fail + 1))
            echo "FAIL stdin mode: unexpected output"
            ;;
    esac
else
    fail=$((fail + 1))
    echo "FAIL stdin mode: exit $rc or invalid JSON"
fi

echo "----------------------------------------"
echo "passed: $pass  failed: $fail"
[ "$fail" -eq 0 ]
