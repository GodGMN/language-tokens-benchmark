#!/bin/sh
# Self-test runner: executes every workflow in this directory and compares
# stdout (byte for byte) and the exit code with the recorded expectations in
# expected/.
#
# Usage:  sh selftest/run.sh        (or: make selftest)
set -u

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/.." && pwd)
bin="$root/workflow"

if [ ! -x "$bin" ]; then
    echo "workflow binary not found; run 'make' first" >&2
    exit 1
fi

pass=0
fail=0
for input in "$here"/*.json; do
    name=$(basename "$input" .json)
    expected="$here/expected/$name.json"
    expected_exit_file="$here/expected/$name.exit"
    [ -f "$expected" ] || { echo "SKIP $name (no expectation)"; continue; }
    expected_exit=$(cat "$expected_exit_file")

    actual=$("$bin" run "$input")
    actual_exit=$?

    # Also exercise the stdin entry point on every workflow.
    stdin_actual=$(cat "$input" | "$bin" run -)
    stdin_exit=$?

    ok=1
    [ "$actual" = "$(cat "$expected")" ] || ok=0
    [ "$actual_exit" = "$expected_exit" ] || ok=0
    [ "$stdin_actual" = "$actual" ] || ok=0
    [ "$stdin_exit" = "$actual_exit" ] || ok=0

    if [ "$ok" = 1 ]; then
        echo "PASS $name (exit $actual_exit)"
        pass=$((pass + 1))
    else
        echo "FAIL $name (exit $actual_exit, expected $expected_exit)"
        echo "  expected: $(cat "$expected")"
        echo "  actual  : $actual"
        fail=$((fail + 1))
    fi
done

echo "----"
echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
