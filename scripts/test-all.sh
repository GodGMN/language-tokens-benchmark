#!/usr/bin/env bash
# Build + run the conformance suite for all languages.
set -euo pipefail
cd "$(dirname "$0")/.."
python3 benchmark/build.py --all
python3 benchmark/runner.py --all
