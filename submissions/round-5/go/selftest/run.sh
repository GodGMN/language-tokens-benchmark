#!/bin/sh
# Runs every workflow in this folder through the CLI.
# Exit codes: 0 completed, 1 runtime failure, 2 validation error, 3 internal.
set -u
cd "$(dirname "$0")/.." || exit 1
BIN=./workflow
[ -x "$BIN" ] || { echo "build first: go build -o workflow ." >&2; exit 1; }
for f in selftest/*.json; do
  printf '=== %s\n' "$f"
  "$BIN" run "$f"
  printf 'exit=%s\n\n' "$?"
done
