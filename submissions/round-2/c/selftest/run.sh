#!/bin/sh
# Self-test driver for the workflow CLI.
# Run from anywhere: it locates the project root relative to this script.
set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT" || exit 1

if [ ! -x ./workflow ]; then
    echo "workflow binary not found; run 'make' first" >&2
    exit 1
fi

pass=0
fail=0

check() {
    file="$1"
    want_rc="$2"
    want_sub="$3"
    out=$(./workflow run "selftest/$file")
    rc=$?
    if [ "$rc" -ne "$want_rc" ]; then
        echo "FAIL $file: exit $rc, want $want_rc"
        echo "  $out"
        fail=$((fail + 1))
    elif ! printf '%s' "$out" | grep -F -q -- "$want_sub"; then
        echo "FAIL $file: missing expected substring: $want_sub"
        echo "  $out"
        fail=$((fail + 1))
    else
        echo "ok   $file"
        pass=$((pass + 1))
    fi
}

check math_pipeline.json    0 '"total":90'
check math_pipeline.json    0 '"big":10'
check string_pipeline.json  0 '"final":"HELLO-there"'
check string_pipeline.json  0 '"len":11'
check object_ops.json       0 '"picked":{"a":9,"missing":null,"z":0}'
check object_ops.json       0 '"vs":[1,2,[3,4]]'
check skip.json             0 '"skipped":["y"]'
check skip.json             0 '"execution_order":["x","y","z"]'
check retry.json            0 '"flaky":"ok","after":"ok!"'
check refs.json             0 '"third":40'
check conditions.json       0 '"skipped":["negated"]'
check array_ops.json        0 '"sorted":[1,3,"a","b",false,true,null,[1],[2],{}]'
check array_ops.json        0 '"uniq":[1,"1",[1],{"k":1}]'
check array_ops.json        0 '"sliced":[3,"a","b",false,true,null,[1],[2]]'
check error_overflow.json   1 '"code":"ARITHMETIC_OVERFLOW"'
check error_overflow.json   1 '"results":{"ok":1}'
check invalid_workflow.json 2 '"code":"UNKNOWN_DEPENDENCY"'

echo
echo "passed: $pass  failed: $fail"
[ "$fail" -eq 0 ]
