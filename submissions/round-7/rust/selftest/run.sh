#!/usr/bin/env bash
# Run every self-test workflow and print its JSON response.
# Some fixtures intentionally fail (e.g. retry_exhausted), so non-zero exits
# are reported but do not abort the script.
set -u
cd "$(dirname "$0")/.."
BIN="target/release/workflow"

if [ ! -x "$BIN" ]; then
  echo "binary not found; run: cargo build --release" >&2
  exit 1
fi

for f in selftest/*.json; do
  echo "== $f"
  "$BIN" run "$f"
  code=$?
  echo "   (exit $code)"
done

echo "== stdin: selftest/math_pipeline.json"
"$BIN" run - < selftest/math_pipeline.json
echo "   (exit $?)"
