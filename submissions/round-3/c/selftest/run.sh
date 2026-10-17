#!/bin/sh
# Run the hand-written fixture suite against ./workflow.
set -e
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec python3 "$here/check.py"
