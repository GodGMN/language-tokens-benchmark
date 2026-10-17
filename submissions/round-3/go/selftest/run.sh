#!/bin/sh
# Self-test runner for the Go workflow engine.
# Usage: ./selftest/run.sh   (from submissions/round-3/go)
set -u

cd "$(dirname "$0")/.." || exit 1
BIN=./workflow

if [ ! -x "$BIN" ]; then
  echo "building $BIN ..."
  go build -o "$BIN" . || exit 1
fi

status=0
for f in selftest/*.json; do
  echo "=== $f"
  out=$("$BIN" run "$f")
  rc=$?
  echo "$out"
  echo "exit=$rc"
  # A failure fixture is expected to exit 1; everything else must exit 0.
  case "$f" in
    *failure.json)
      [ "$rc" -eq 1 ] || { echo "UNEXPECTED EXIT for $f"; status=1; }
      ;;
    *)
      [ "$rc" -eq 0 ] || { echo "UNEXPECTED EXIT for $f"; status=1; }
      ;;
  esac
  # stdout must be a single valid JSON document.
  printf '%s' "$out" | python3 -m json.tool >/dev/null || { echo "INVALID JSON for $f"; status=1; }
done

if [ "$status" -eq 0 ]; then
  echo "ALL SELFTESTS PASSED"
else
  echo "SELFTESTS FAILED"
fi
exit "$status"
