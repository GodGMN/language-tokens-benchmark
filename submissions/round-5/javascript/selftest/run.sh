#!/usr/bin/env bash
# Run every hand-written self-test workflow through the CLI.
set -u
cd "$(dirname "$0")/.." || exit 1
fail=0
for f in selftest/*.json; do
  echo "=== $f"
  node workflow.js run "$f"
  code=$?
  echo "exit=$code"
  case "$f" in
    *j_retry_exhausted.json) [ "$code" -eq 1 ] || fail=1 ;;
    *)                       [ "$code" -eq 0 ] || fail=1 ;;
  esac
done
if [ "$fail" -ne 0 ]; then
  echo "SELFTEST FAILED" >&2
  exit 1
fi
echo "SELFTEST OK"
