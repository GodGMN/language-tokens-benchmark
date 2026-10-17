#!/usr/bin/env bash
# Self-test runner: builds the CLI and checks every fixture in selftest/.
set -u
cd "$(dirname "$0")/.."

if ! go build -o workflow .; then
  echo "build failed" >&2
  exit 1
fi

fail=0
for f in selftest/[0-9]*.json; do
  b="$(basename "$f" .json)"
  actual="$(./workflow run "$f")"
  code=$?
  want_code="$(tr -d '[:space:]' < "selftest/expected/$b.exit")"
  if [ "$code" != "$want_code" ]; then
    echo "FAIL $b: exit $code, expected $want_code"
    fail=1
    continue
  fi
  if ! python3 -c '
import json, sys
actual = json.loads(sys.argv[1])
expected = json.load(open(sys.argv[2]))
# error.message is free-form and never compared by the real suite.
if actual.get("error"):
    actual["error"].pop("message", None)
if expected.get("error"):
    expected["error"].pop("message", None)
if actual != expected:
    print("FAIL: response mismatch", file=sys.stderr)
    print("actual:  ", json.dumps(actual, sort_keys=True), file=sys.stderr)
    print("expected:", json.dumps(expected, sort_keys=True), file=sys.stderr)
    sys.exit(1)
' "$actual" "selftest/expected/$b.json"; then
    echo "FAIL $b: response mismatch"
    fail=1
    continue
  fi
  echo "ok   $b (exit $code)"
done

if [ "$fail" -ne 0 ]; then
  echo "some self-tests failed" >&2
  exit 1
fi
echo "all self-tests passed"
