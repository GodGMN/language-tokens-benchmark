#!/usr/bin/env bash
# Report progress of a round: per-language state + live token totals.
# Usage: ./scripts/round-status.sh [round]
set -uo pipefail
cd "$(dirname "$0")/.."
ROUND="${1:-round-2}"
RJSON="benchmark/rounds/$ROUND/round.json"
[[ -f "$RJSON" ]] || { echo "missing $RJSON" >&2; exit 1; }

read -r LANGS < <(python3 -c "import json;print(' '.join(json.load(open('$RJSON'))['languages']))")
printf '%-12s %-9s %-13s %s\n' LANGUAGE STATE TOKENS LOG
for lang in $LANGS; do
  sid=$(python3 -c "import json;e=json.load(open('$RJSON'))['languages']['$lang'];print(e if isinstance(e,str) else e.get('session',''))")
  log="results/$ROUND/$lang/agent.log"
  state="idle"
  pgrep -f -- "session-id $sid" >/dev/null 2>&1 && state="running"
  [[ -f "results/$ROUND/$lang/.done" ]] && state="done(rc=$(cat results/$ROUND/$lang/.done))"
  tokens="—"
  sf=$(python3 - "$sid" <<'PY' 2>/dev/null
import glob, os, sys
hits = glob.glob(os.path.expanduser(f"~/.pi/agent/sessions/**/*_{sys.argv[1]}.jsonl"), recursive=True)
print(max(hits, key=os.path.getmtime) if hits else "")
PY
)
  if [[ -n "$sf" ]]; then
    tokens=$(python3 - "$sf" <<'PY' 2>/dev/null
import json, sys
tot = 0
for line in open(sys.argv[1], errors="ignore"):
    try: e = json.loads(line)
    except Exception: continue
    if e.get("type") == "message" and e.get("message", {}).get("role") == "assistant":
        tot += (e["message"].get("usage") or {}).get("totalTokens", 0)
    elif e.get("type") == "usage":
        tot += (e.get("usage") or {}).get("totalTokens", 0)
print(f"{tot:,}")
PY
)
  fi
  printf '%-12s %-9s %-13s %s\n' "$lang" "$state" "$tokens" "$log"
done
