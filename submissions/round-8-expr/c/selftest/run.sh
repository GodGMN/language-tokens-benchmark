#!/bin/sh
# Run the full self-test suite.
#   1. selftest/run.py  : ~200 inline cases (precedence, every function,
#                         every error code, request validation)
#   2. selftest/cases/  : plain request/expected-output file pairs
# Usage (from the project directory):  sh selftest/run.sh
set -u

cd "$(dirname "$0")/.." || exit 3

if [ ! -x ./expr ]; then
    echo "building expr..." >&2
    make || exit 3
fi

status=0

echo "== inline cases =="
if command -v python3 >/dev/null 2>&1; then
    python3 selftest/run.py || status=1
else
    echo "python3 not found; skipping inline cases" >&2
fi

echo
echo "== file cases =="
fail=0
for req in selftest/cases/*.json; do
    base=${req%.json}
    exp="${base}.out"
    expcode=$(cat "${base}.code")
    out=$(./expr eval "$req")
    code=$?
    if [ "$out" != "$(cat "$exp")" ] || [ "$code" != "$expcode" ]; then
        echo "FAIL $req (exit $code, expected $expcode)"
        echo "  expected: $(cat "$exp")"
        echo "  actual  : $out"
        fail=$((fail + 1))
    else
        echo "ok   $req"
    fi
done
[ "$fail" -eq 0 ] || status=1

echo
if [ "$status" -eq 0 ]; then
    echo "ALL SELF-TESTS PASSED"
else
    echo "SELF-TESTS FAILED"
fi
exit "$status"
