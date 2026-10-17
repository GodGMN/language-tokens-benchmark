#!/usr/bin/env bash
# Run every self-test workflow through the CLI and print its response + exit code.
set -u
cd "$(dirname "$0")/.." || exit 1
if [ ! -x ./workflow ]; then
  echo "building ./workflow ..." >&2
  go build -o workflow . || exit 1
fi
for f in selftest/*.json; do
  echo "=== $f"
  ./workflow run "$f"
  echo "    exit=$?"
done
