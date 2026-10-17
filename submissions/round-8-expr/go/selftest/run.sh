#!/usr/bin/env bash
# Self-test runner: builds the CLI and checks every request in selftest/cases.
set -u
cd "$(dirname "$0")/.."

go build -o expr . || exit 1

pass=0
fail=0
for req in selftest/cases/*.json; do
    name="${req%.json}"
    expected="$(cat "$name.expected")"
    want_exit="$(cat "$name.exit")"
    actual="$(./expr eval "$req" 2>/dev/null)"
    actual_exit=$?
    if [ "$actual" = "$expected" ] && [ "$actual_exit" = "$want_exit" ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "FAIL $(basename "$req")"
        echo "  want exit $want_exit: $expected"
        echo "  got  exit $actual_exit: $actual"
    fi
done

# stdin mode smoke test
stdin_out="$(printf '%s' '{"source":"1 + 2"}' | ./expr eval -)"
if [ "$stdin_out" = '{"status":"ok","value":3}' ]; then
    pass=$((pass + 1))
else
    fail=$((fail + 1))
    echo "FAIL stdin mode"
    echo "  got: $stdin_out"
fi

echo "passed $pass, failed $fail"
[ "$fail" -eq 0 ]
