#!/usr/bin/env bash
# Self-test: run every workflow in selftest/ through the built CLI and compare
# both the JSON response and the exit code with the expected values.
set -u
cd "$(dirname "$0")/.." || exit 1

if [ ! -x ./workflow ]; then
  echo "building workflow..." >&2
  go build -o workflow . || exit 1
fi

fail=0
for f in selftest/*.json; do
  name=$(basename "$f" .json)
  expected="selftest/expected/$name.json"
  expected_exit=$(cat "selftest/expected/$name.exit")

  out=$(./workflow run "$f")
  code=$?

  if [ "$code" != "$expected_exit" ]; then
    echo "FAIL $name: exit $code, want $expected_exit"
    fail=1
    continue
  fi

  if command -v jq >/dev/null 2>&1; then
    a=$(printf '%s' "$out" | jq -S . 2>/dev/null)
    b=$(jq -S . "$expected")
    if [ "$a" != "$b" ]; then
      echo "FAIL $name: response mismatch"
      echo "  got:      $out"
      echo "  expected: $(cat "$expected")"
      fail=1
      continue
    fi
  else
    if [ "$out" != "$(cat "$expected")" ]; then
      echo "FAIL $name: response mismatch"
      echo "  got:      $out"
      echo "  expected: $(cat "$expected")"
      fail=1
      continue
    fi
  fi
  echo "ok   $name"
done

if [ "$fail" -ne 0 ]; then
  echo "SELFTEST FAILED" >&2
  exit 1
fi
echo "all selftests passed"
