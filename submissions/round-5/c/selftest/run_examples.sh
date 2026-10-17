#!/bin/sh
# Run every hand-written example workflow through the CLI.
set -u
cd "$(dirname "$0")/.." || exit 3

if [ ! -x ./workflow ]; then
    echo "building ./workflow ..." >&2
    make || exit 3
fi

rc=0
for f in selftest/*.json; do
    printf '== %s\n' "$f"
    ./workflow run "$f" || rc=1
    printf '\n'
done

printf '== stdin example\n'
printf '%s' '{"version":1,"steps":[{"id":"hello","op":"constant","args":{"value":"world"}}]}' \
    | ./workflow run - || rc=1
printf '\n'

if [ "$rc" -eq 0 ]; then
    echo "All example workflows ran (see JSON above)."
else
    echo "Some example workflows failed (see JSON above)." >&2
fi
exit "$rc"
