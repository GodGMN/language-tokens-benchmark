#!/bin/sh
# Build (if needed) and run the self-tests.
set -e
cd "$(dirname "$0")/.."
cargo build --release
python3 selftest/run.py
