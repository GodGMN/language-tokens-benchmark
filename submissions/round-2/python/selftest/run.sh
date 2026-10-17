#!/bin/sh
# Run every self-test workflow and print its raw JSON response + exit code.
cd "$(dirname "$0")/.." || exit 1
for f in selftest/[0-9][0-9]_*.json; do
    printf '== %s\n' "$f"
    python3 workflow.py run "$f"
    printf '   exit=%s\n' "$?"
done
