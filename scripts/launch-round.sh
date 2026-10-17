#!/usr/bin/env bash
# Launch one pi session per language, in parallel, for a benchmark round.
#
# Each agent runs non-interactively (pi --print) with its own cwd (cli-<lang>/),
# its own session id (round1-<lang>) and the round's requirements prompt.
#
# Usage:
#   ./scripts/launch-round.sh [round-id] [--dry-run]
#   ./scripts/launch-round.sh --status [round-id]
#
# Environment:
#   AGENT_TIMEOUT   seconds before an agent is killed (default 3600)
#   PI_MODEL        forwarded to the child sessions if set here
set -uo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"

LANGS=(python javascript go rust c)
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
LOG_ROOT="results/$ROUND"

# Model is pinned per round so all five agents run on exactly the same one.
MODEL_ARGS=()
if [[ -f "$PROMPT_DIR/round.json" ]]; then
  read -r R_PROVIDER R_MODEL R_THINKING < <(python3 -c "
import json
d = json.load(open('$PROMPT_DIR/round.json'))
print(d.get('provider') or '-', d.get('model') or '-', d.get('pi_thinking_level') or '-')
")
  [[ "$R_PROVIDER" != "-" ]] && MODEL_ARGS+=(--provider "$R_PROVIDER")
  [[ "$R_MODEL" != "-" ]] && MODEL_ARGS+=(--model "$R_MODEL")
  [[ "$R_THINKING" != "-" ]] && MODEL_ARGS+=(--thinking "$R_THINKING")
fi

# Ad-hoc override: BENCH_PROVIDER=... BENCH_MODEL=... ./scripts/launch-round.sh
if [[ -n "${BENCH_MODEL:-}" ]]; then
  MODEL_ARGS=()
  [[ -n "${BENCH_PROVIDER:-}" ]] && MODEL_ARGS+=(--provider "$BENCH_PROVIDER")
  MODEL_ARGS+=(--model "$BENCH_MODEL")
fi

# --- status mode: report progress of a running/finished round -----------------
if [[ "$MODE" == "status" ]]; then
  printf '%-12s %-9s %-10s %s\n' LANGUAGE SESSION TOKENS LOG
  for lang in "${LANGS[@]}"; do
    log="$LOG_ROOT/$lang/agent.log"
    running="no"
    pgrep -f "session-id round1-$lang" >/dev/null 2>&1 && running="yes"
    tokens="—"
    if session_file=$(python3 - "$lang" <<'PY' 2>/dev/null
import sys, glob, os
lang = sys.argv[1]
hits = glob.glob(os.path.expanduser(f"~/.pi/agent/sessions/**/*_round1-{lang}.jsonl"), recursive=True)
print(max(hits, key=os.path.getmtime) if hits else "")
PY
    ); then
      :
    fi
    if [[ -n "${session_file:-}" ]]; then
      tokens=$(python3 - "$session_file" <<'PY' 2>/dev/null
import json, sys
tot = 0
for line in open(sys.argv[1], errors="ignore"):
    try: e = json.loads(line)
    except Exception: continue
    if e.get("type") == "message" and e.get("message", {}).get("role") == "assistant":
        tot = (e["message"].get("usage") or {}).get("totalTokens", 0) + tot
    elif e.get("type") == "usage":
        tot += (e.get("usage") or {}).get("totalTokens", 0)
print(f"{tot:,}")
PY
    )
    fi
    state="done"
    [[ "$running" == "yes" ]] && state="running"
    printf '%-12s %-9s %-10s %s\n' "$lang" "$state" "$tokens" "$log"
  done
  exit 0
fi

# --- dry run: show what would be launched -------------------------------------
if [[ "$MODE" == "dry" ]]; then
  echo "round:    $ROUND"
  echo "timeout:  ${TIMEOUT}s per agent"
  echo "model:    ${MODEL_ARGS[*]:-<pi default>}"
  for lang in "${LANGS[@]}"; do
    echo "  cli-$lang/  session=round1-$lang  prompt=$PROMPT_DIR/prompt-$lang.md  -> $LOG_ROOT/$lang/agent.log"
  done
  exit 0
fi

# --- launch -------------------------------------------------------------------
pids=()
for lang in "${LANGS[@]}"; do
  dir="cli-$lang"
  prompt="$ROOT/$PROMPT_DIR/prompt-$lang.md"
  logdir="$ROOT/$LOG_ROOT/$lang"
  log="$logdir/agent.log"

  [[ -f "$prompt" ]] || { echo "missing prompt: $prompt" >&2; exit 1; }
  [[ -d "$ROOT/$dir" ]] || { echo "missing project dir: $dir" >&2; exit 1; }
  mkdir -p "$logdir"

  # Strip the parent session's PI_* markers so the child creates its own session.
  # Model is pinned from round.json so all five agents run on the same one.
  setsid nohup env \
    -u PI_SESSION_FILE -u PI_SESSION_ID -u PI_PROVIDER \
    -u PI_MODEL -u PI_REASONING_LEVEL \
    timeout "$TIMEOUT" \
    pi --print "${MODEL_ARGS[@]}" --session-id "round1-$lang" "$(cat "$prompt")" \
    > "$log" 2>&1 < /dev/null &
  pid=$!
  pids+=("$pid:$lang")
  printf 'launched %-11s pid=%-7s session=round1-%-11s log=%s\n' "$lang" "$pid" "$lang" "$log"
done

echo
echo "${#pids[@]} agents running in parallel (timeout ${TIMEOUT}s, model: ${MODEL_ARGS[*]:-<pi default>})."
echo "progress:  ./scripts/launch-round.sh --status $ROUND"
echo "metrics:   python3 benchmark/collect.py --round $ROUND"
