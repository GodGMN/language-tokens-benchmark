#!/usr/bin/env bash
# Black-box self-test: runs every selftest/cases/*.json through the CLI and
# compares stdout and the exit code with the recorded expectation.
set -u

cd "$(dirname "$0")/.." || exit 1

if [ ! -x ./workflow ]; then
    echo "error: ./workflow not found; run: go build -o workflow ." >&2
    exit 1
fi

run_file() {
    ./workflow run "$1"
}

run_stdin() {
    ./workflow run - < "$1"
}

pass=0
fail=0

check_one() {
    local label="$1" runner="$2" input="$3" want_exit="$4" expected="$5"
    local actual got_exit
    actual=$("$runner" "$input")
    got_exit=$?

    if [ "$got_exit" -ne "$want_exit" ]; then
        echo "FAIL $label: exit $got_exit, want $want_exit"
        fail=$((fail + 1))
        return
    fi
    if printf '%s\n' "$actual" | diff -u "$expected" - >/dev/null; then
        echo "ok   $label"
        pass=$((pass + 1))
    else
        echo "FAIL $label: stdout differs from $(basename "$expected")"
        printf '%s\n' "$actual" | diff -u "$expected" - | sed 's/^/     /'
        fail=$((fail + 1))
    fi
}

for input in selftest/cases/*.json; do
    case "$input" in
        *.expected.json) continue ;;
    esac
    base=${input%.json}
    name=$(basename "$base")
    want_exit=$(cat "$base.exit")
    check_one "$name" run_file "$input" "$want_exit" "$base.expected.json"
done

# stdin mode: reuse the first case
base=selftest/cases/01_math_pipeline
check_one "01_math_pipeline (stdin)" run_stdin "$base.json" \
    "$(cat "$base.exit")" "$base.expected.json"

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
