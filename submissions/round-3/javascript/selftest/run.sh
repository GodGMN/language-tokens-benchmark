#!/bin/sh
# Run the self-test suite from the submission folder.
set -e
cd "$(dirname "$0")/.."
node selftest/check.js
