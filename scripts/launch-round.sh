#!/usr/bin/env bash
# Launch one pi session per language, in parallel, for a benchmark round.
#
# Each agent runs non-interactively (pi --print) with its own cwd
# (submissions/<round>/<language>/), its own session id, and the round's
# requirements prompt. Model is pinned from round.json so all agents match.
#
# Usage:
#   ./scripts/launch-round.sh [round-id] [--dry-run]
#   ./scripts/launch-round.sh --status [round-id]
#
# Environment:
#   AGENT_TIMEOUT   seconds before an agent is killed (default 3600)
#   BENCH_MODEL / BENCH_PROVIDER   ad-hoc override of the round's model
set -uo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"

TIMEOUT="${AGENT_TIMEOUT:-3600}"
MODE="launch"
ROUND="round-1"
for arg in "$@"; do
  case "$arg" in
    --dry-run) MODE="dry" ;;
    --status)  MODE="status" ;;
    -*) echo "unknown flag: $arg" >&2; exit 2 ;;
    *)  ROUND="$arg" ;;
  esac
done

PROMPT_DIR="benchmark/rounds/$ROUND"
ROUND_JSON="$PROMPT_DIR/round.json"
[[ -f "$ROUND_JSON" ]] || { echo "missing $ROUND_JSON" >&2; exit 1; }

# Read round config: languages, submissions dir, model, sessions.
read -r SUBMISSIONS R_PROVIDER R_MODEL R_THINKING LANGS < <(python3 - "$ROUND_JSON" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
m = d.get("model") or {}
print(d.get("submissions", f"submissions/{d['id']}"),
      m.get("provider") or "-", m.get("name") or "-", m.get("thinking") or "-",
      " ".join(d.get("languages", {}).keys()))
PY
)
LANGS_ARR=($LANGS)

# Map language -> session id from round.json (python one-shot per language).
session_for() {
  python3 - "$ROUND_JSON" "$1" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
e = d.get("languages", {}).get(sys.argv[2])
print(e if isinstance(e, str) else (e or {}).get("session", ""))
PY
}

MODEL_ARGS=()
[[ "$R_PROVIDER" != "-" ]] && MODEL_ARGS+=(--provider "$R_PROVIDER")
[[ "$R_MODEL" != "-" ]] && MODEL_ARGS+=(--model "$R_MODEL")
[[ "$R_THINKING" != "-" ]] && MODEL_ARGS+=(--thinking "$R_THINKING")
if [[ -n "${BENCH_MODEL:-}" ]]; then
  MODEL_ARGS=()
  [[ -n "${BENCH_PROVIDER:-}" ]] && MODEL_ARGS+=(--provider "$BENCH_PROVIDER")
  MODEL_ARGS+=(--model "$BENCH_MODEL")
fi

if [[ "$MODE" == "status" ]]; then
  printf '%-12s %-9s %-12s %s\n' LANGUAGE STATE TOKENS LOG
  for lang in "${LANGS_ARR[@]}"; do
    sid=$(session_for "$lang")
    log="results/$ROUND/$lang/agent.log"
    state="idle"
    pgrep -f -- "session-id $sid" >/dev/null 2>&1 && state="running"
    tokens="—"
    sf=$(python3 - "$sid" <<'PY' 2>/dev/null
import glob, os, sys
sid = sys.argv[1]
hits = glob.glob(os.path.expanduser(f"~/.pi/agent/sessions/**/*_{sid}.jsonl"), recursive=True)
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
      [[ "$state" == "idle" ]] && state="done"
    fi
    printf '%-12s %-9s %-12s %s\n' "$lang" "$state" "$tokens" "$log"
  done
  exit 0
fi

if [[ "$MODE" == "dry" ]]; then
  echo "round:    $ROUND"
  echo "model:    ${MODEL_ARGS[*]:-<pi default>}"
  echo "timeout:  ${TIMEOUT}s per agent"
  for lang in "${LANGS_ARR[@]}"; do
    echo "  $SUBMISSIONS/$lang/  session=$(session_for "$lang")  prompt=$PROMPT_DIR/prompt-$lang.md"
  done
  exit 0
fi

pids=()
for lang in "${LANGS_ARR[@]}"; do
  dir="$SUBMISSIONS/$lang"
  sid=$(session_for "$lang")
  prompt="$ROOT/$PROMPT_DIR/prompt-$lang.md"
  logdir="$ROOT/results/$ROUND/$lang"
  log="$logdir/agent.log"

  [[ -f "$prompt" ]] || { echo "missing prompt: $prompt" >&2; exit 1; }
  [[ -d "$ROOT/$dir" ]] || { echo "missing submission dir: $dir" >&2; exit 1; }
  [[ -n "$sid" ]] || { echo "no session id for $lang in $ROUND_JSON" >&2; exit 1; }
  mkdir -p "$logdir"

  # Strip the parent session's PI_* markers so the child creates its own session.
  setsid nohup env \
    -u PI_SESSION_FILE -u PI_SESSION_ID -u PI_PROVIDER \
    -u PI_MODEL -u PI_REASONING_LEVEL \
    timeout "$TIMEOUT" \
    pi --print "${MODEL_ARGS[@]}" --session-id "$sid" "$(cat "$prompt")" \
    > "$log" 2>&1 < /dev/null &
  printf 'launched %-11s pid=%-7s session=%-16s log=%s\n' "$lang" "$!" "$sid" "$log"
done

echo
echo "${#LANGS_ARR[@]} agents running in parallel (timeout ${TIMEOUT}s, model: ${MODEL_ARGS[*]:-<pi default>})."
echo "progress:  ./scripts/launch-round.sh --status $ROUND"
echo "metrics:   python3 benchmark/collect.py --round $ROUND"
