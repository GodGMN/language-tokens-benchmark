#!/bin/sh
# Run every hand-written workflow in this directory and print its response.
# Exit codes: 0 completed, 1 runtime failure, 2 validation error, 3 internal.
set -u
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BIN="$DIR/../target/release/workflow"
if [ ! -x "$BIN" ]; then
  echo "build first: cargo build --release" >&2
  exit 1
fi
for f in "$DIR"/*.json; do
  case "$f" in
    *.expected.json) continue ;;
  esac
  echo "=== $(basename "$f")"
  "$BIN" run "$f"
  echo "--- exit=$?"
done
