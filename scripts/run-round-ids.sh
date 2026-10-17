#!/usr/bin/env bash
# Run an explicit list of round ids end-to-end, unattended, then aggregate+commit.
# Usage: ./scripts/run-round-ids.sh round-8-expr round-9-patch
set -uo pipefail
cd "$(dirname "$0")/.."
ROUND_TIMEOUT="${ROUND_TIMEOUT:-7200}"
mkdir -p results
exec >>results/run-rounds.log 2>&1
echo "=== run-round-ids $* $(date -u +%FT%TZ)"

wait_round() {
  local round="$1" nlangs deadline done
  nlangs=$(python3 -c "import json;print(len(json.load(open('benchmark/rounds/$round/round.json'))['languages']))")
  deadline=$(( SECONDS + ROUND_TIMEOUT ))
  while (( SECONDS < deadline )); do
    done=$(find "results/$round" -name .done 2>/dev/null | wc -l)
    (( done >= nlangs )) && { echo "[$round] $done/$nlangs done"; return 0; }
    sleep 20
  done
  echo "[$round] TIMEOUT"; return 1
}

for round in "$@"; do
  echo "--- [$round] launch $(date -u +%FT%TZ)"
  ./scripts/launch-sandboxed.sh "$round"
  wait_round "$round"
  echo "[$round] build";   python3 benchmark/build.py   --all --round "$round"
  echo "[$round] test";    python3 benchmark/runner.py  --all --round "$round"
  echo "[$round] collect"; python3 benchmark/collect.py --round "$round"
  echo "[$round] report";  python3 benchmark/report.py  --round "$round"
done

python3 benchmark/compare.py
python3 benchmark/aggregate.py
python3 benchmark/site.py

git add -A
if git commit -q -m "Add task rounds: $*"; then
  git push origin main && echo "[all] committed and pushed $(date -u +%FT%TZ)"
else
  echo "[all] nothing to commit"
fi
echo "=== finished $(date -u +%FT%TZ)"
