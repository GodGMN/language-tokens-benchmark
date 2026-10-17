#!/bin/sh
# Self-test entry point: runs the hand-written example workflows and the
# exhaustive Python check-suite (if python3 is available).
set -e
cd "$(dirname "$0")/.."

echo "== example workflows =="
status=0
for f in selftest/[0-9]*.json; do
    echo "-- $f"
    set +e
    ./workflow run "$f"
    rc=$?
    set -e
    echo "exit=$rc"
done

if command -v python3 >/dev/null 2>&1; then
    echo "== check suite =="
    python3 selftest/run_tests.py
else
    echo "python3 not found; skipping the check suite"
fi
