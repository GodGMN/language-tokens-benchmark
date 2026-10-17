#!/usr/bin/env bash
# Build + run the conformance suite for one language.
# Usage: ./scripts/test.sh go [category]
set -euo pipefail
cd "$(dirname "$0")/.."
lang="${1:?usage: test.sh <language> [category]}"
shift
python3 benchmark/build.py --language "$lang"
python3 benchmark/runner.py --language "$lang" ${1:+--category "$1"}
