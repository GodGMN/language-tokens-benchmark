#!/usr/bin/env bash
# Build and run the workflow self-tests.
set -euo pipefail
cd "$(dirname "$0")/.."
cargo build --release
exec python3 selftest/check.py
