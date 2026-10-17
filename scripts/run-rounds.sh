#!/usr/bin/env bash
# Run a range of rounds end-to-end, unattended.
#   launch (sandboxed, 5 agents in parallel) -> wait -> build -> test
#   -> collect metrics -> report
# Then aggregate all rounds and commit/push.
#
# Usage: ./scripts/run-rounds.sh [start] [end]
set -uo pipefail
cd "$(dirname "$0")/.."

START="${1:-3}"; END="${2:-7}"
ROUND_TIMEOUT="${ROUND_TIMEOUT:-7200}"
LOG="results/run-rounds.log"
mkdir -p results
exec >>"$LOG" 2>&1

echo "=============================================================="
echo "run-rounds started $(date -u +%FT%TZ)  rounds $START..$END"

wait_round() {
  local round="$1"
  local nlangs deadline done
  nlangs=$(python3 -c "import json;print(len(json.load(open('benchmark/rounds/$round/round.json'))['languages']))")
  deadline=$(( SECONDS + ROUND_TIMEOUT ))
  while (( SECONDS < deadline )); do
    done=$(find "results/$round" -name .done 2>/dev/null | wc -l)
    if (( done >= nlangs )); then echo "[$round] $done/$nlangs agents done"; return 0; fi
    sleep 20
  done
  echo "[$round] TIMEOUT waiting for agents"; return 1
}

for n in $(seq "$START" "$END"); do
  round="round-$n"
  echo "--------------------------------------------------------------"
  echo "[$round] launching $(date -u +%FT%TZ)"
  ./scripts/launch-sandboxed.sh "$round"
  wait_round "$round"
  echo "[$round] build";   python3 benchmark/build.py  --all --round "$round"
  echo "[$round] test";    python3 benchmark/runner.py --all --round "$round"
  echo "[$round] collect"; python3 benchmark/collect.py --round "$round"
  echo "[$round] report";  python3 benchmark/report.py  --round "$round"
done

echo "[all] compare / aggregate / site"
python3 benchmark/compare.py
python3 benchmark/aggregate.py
python3 benchmark/site.py

git add -A
if git commit -q -m "Add rounds $START-$END: five spec-v2 samples + aggregation

Five more one-shot samples of the frozen spec v2 on
deepseek/deepseek-flash, each run sandboxed (agents see only SPEC.md, PRD.md
and their own submission dir). Rounds share group deepseek-v4.1-flash-1017 and
spec v2, so aggregate.py reports median [min-max] per language over n samples."; then
  git push origin main && echo "[all] committed and pushed $(date -u +%FT%TZ)"
else
  echo "[all] nothing to commit"
fi
echo "run-rounds finished $(date -u +%FT%TZ)"
