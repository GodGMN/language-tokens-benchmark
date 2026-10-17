#!/usr/bin/env bash
# Full pipeline: build -> test -> collect pi token metrics -> report.
# Test failures do not abort the run; the report is always produced.
#
# Usage: ./scripts/benchmark.sh [round-id]
set -uo pipefail
cd "$(dirname "$0")/.."
round="${1:-round-1}"

echo "== build =="
python3 benchmark/build.py --all

echo
echo "== conformance suite =="
python3 benchmark/runner.py --all --round "$round"
tests_rc=$?

echo
echo "== pi token metrics =="
python3 benchmark/collect.py --round "$round"

echo
echo "== report =="
python3 benchmark/report.py --round "$round"

echo
echo "report: results/$round/report.md"
exit $tests_rc
