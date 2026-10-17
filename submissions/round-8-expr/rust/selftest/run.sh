#!/usr/bin/env bash
# Run the hand-checked conformance cases through the CLI.
# Each case is executed twice: once with a request file and once via stdin.

set -u

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
bin="${EXPR_BIN:-$root/target/release/expr}"
cases="$here/cases.tsv"

if [ ! -x "$bin" ]; then
    echo "error: binary not found at $bin (run: cargo build --release)" >&2
    exit 1
fi

tmp="$(mktemp)"
trap 'rm -f "$tmp"' EXIT

pass=0
fail=0
line_no=0

check() {
    local name="$1" out="$2" status="$3" expected="$4" exp_status="$5" mode="$6"
    local ok=1 reason=""

    if [ "$status" -ne "$exp_status" ]; then
        ok=0
        reason="exit $status != $exp_status"
    elif [[ "$expected" == ERR\ * ]]; then
        local want_code want_pos got_code got_pos
        want_code="$(printf '%s' "$expected" | awk '{print $2}')"
        want_pos="$(printf '%s' "$expected" | awk '{print $3}')"
        got_code="$(printf '%s' "$out" | sed -n 's/.*"code":"\([A-Z_]*\)".*/\1/p')"
        got_pos="$(printf '%s' "$out" | sed -n 's/.*"position":\([^,]*\),.*/\1/p')"
        if [ -z "$got_code" ] || [ -z "$got_pos" ]; then
            ok=0
            reason="not an error document: $out"
        elif [ "$got_code" != "$want_code" ] || [ "$got_pos" != "$want_pos" ]; then
            ok=0
            reason="got $got_code@$got_pos, want $want_code@$want_pos"
        fi
    else
        if [ "$out" != "$expected" ]; then
            ok=0
            reason="got $out, want $expected"
        fi
    fi

    if [ "$ok" -eq 1 ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "FAIL [$name] ($mode): $reason" >&2
    fi
}

while IFS=$'\t' read -r name request expected exp_status; do
    line_no=$((line_no + 1))
    [ -z "$name" ] && continue
    case "$name" in \#*) continue ;; esac

    printf '%s' "$request" > "$tmp"
    out_file="$("$bin" eval "$tmp")"
    status_file=$?
    out_stdin="$(printf '%s' "$request" | "$bin" eval -)"
    status_stdin=$?

    check "$name" "$out_file" "$status_file" "$expected" "$exp_status" "file"
    check "$name" "$out_stdin" "$status_stdin" "$expected" "$exp_status" "stdin"
done < "$cases"

echo "selftest: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
