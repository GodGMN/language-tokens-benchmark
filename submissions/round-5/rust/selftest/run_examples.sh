#!/bin/sh
# Run every hand-written workflow example and print its JSON response.
# Usage: sh selftest/run_examples.sh [path-to-binary]
set -u
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BIN=${1:-"$HERE/../target/release/workflow"}
for f in "$HERE"/*.json; do
  echo "=== $(basename "$f")"
  "$BIN" run "$f"
  echo "exit=$?"
done
