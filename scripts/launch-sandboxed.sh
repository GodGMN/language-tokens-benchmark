#!/usr/bin/env bash
# Launch a round with each agent sandboxed to a scratch root that contains
# ONLY SPEC.md, PRD.md and its own (empty) submission directory. No holdout
# suite, no oracle, no sibling submissions.
#
# After each agent exits, its produced files are copied back into
# submissions/<round>/<lang>/ and a .done marker is written.
#
# Usage: ./scripts/launch-sandboxed.sh <round> [--dry-run]
set -uo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"
ROUND="${1:?usage: launch-sandboxed.sh <round> [--dry-run]}"
DRY=0; [[ "${2:-}" == "--dry-run" ]] && DRY=1
TIMEOUT="${AGENT_TIMEOUT:-7200}"
SANDBOX_ROOT="${SANDBOX_ROOT:-/tmp/ltb-$ROUND}"

RJSON="benchmark/rounds/$ROUND/round.json"
[[ -f "$RJSON" ]] || { echo "missing $RJSON" >&2; exit 1; }

read -r SUBMISSIONS R_PROVIDER R_MODEL R_THINKING R_TASK LANGS < <(python3 - "$RJSON" <<'PY'
import json, sys
d = json.load(open(sys.argv[1])); m = d.get("model") or {}
print(d["submissions"], m.get("provider","-"), m.get("name","-"), m.get("thinking","-"),
      d.get("task","workflow"), " ".join(d["languages"]))
PY
)
LANGS_ARR=($LANGS)

session_for() {
  python3 - "$RJSON" "$1" <<'PY'
import json, sys
d = json.load(open(sys.argv[1])); e = d["languages"][sys.argv[2]]
print(e if isinstance(e, str) else e.get("session",""))
PY
}

MODEL_ARGS=()
[[ "$R_PROVIDER" != "-" ]] && MODEL_ARGS+=(--provider "$R_PROVIDER")
[[ "$R_MODEL" != "-" ]] && MODEL_ARGS+=(--model "$R_MODEL")
[[ "$R_THINKING" != "-" ]] && MODEL_ARGS+=(--thinking "$R_THINKING")

if [[ "$DRY" == 1 ]]; then
  echo "round:    $ROUND"
  echo "model:    ${MODEL_ARGS[*]}"
  echo "sandbox:  $SANDBOX_ROOT/<lang>"
  echo "timeout:  ${TIMEOUT}s"
  for lang in "${LANGS_ARR[@]}"; do
    echo "  $SANDBOX_ROOT/$lang/submissions/$ROUND/$lang  session=$(session_for "$lang")"
  done
  exit 0
fi

mkdir -p "results/$ROUND"
for lang in "${LANGS_ARR[@]}"; do
  sid=$(session_for "$lang")
  sb="$SANDBOX_ROOT/$lang"
  dest="$ROOT/$SUBMISSIONS/$lang"
  logdir="$ROOT/results/$ROUND/$lang"
  mkdir -p "$logdir" "$dest"
  rm -f "$logdir/.done"

  # Fresh sandbox: only SPEC, PRD and the empty submission dir.
  rm -rf "$sb"
  mkdir -p "$sb/submissions/$ROUND/$lang"
  if [[ "$R_TASK" == "workflow" ]]; then
    cp "$ROOT/SPEC.md" "$sb/SPEC.md"
  else
    mkdir -p "$sb/tasks/$R_TASK"
    cp "$ROOT/tasks/$R_TASK/SPEC.md" "$sb/tasks/$R_TASK/SPEC.md"
  fi
  cp "$ROOT/PRD.md" "$sb/PRD.md"
  cp "$ROOT/benchmark/rounds/$ROUND/prompt-$lang.md" "$sb/prompt.md"

  # Wrapper: run the agent, then copy its output back and mark done.
  setsid nohup bash -c '
    cd "'"$sb"'/submissions/'"$ROUND"'/'"$lang"'" || exit 1
    env -u PI_SESSION_FILE -u PI_SESSION_ID -u PI_PROVIDER -u PI_MODEL -u PI_REASONING_LEVEL \
      timeout "'"$TIMEOUT"'" pi --print '"${MODEL_ARGS[*]}"' --session-id "'"$sid"'" \
      "$(cat "'"$sb"'/prompt.md")" > "'"$logdir"'/agent.log" 2>&1
    rc=$?
    # copy everything the agent produced back into the repo
    cp -a "'"$sb"'/submissions/'"$ROUND"'/'"$lang"'/." "'"$dest"'/" 2>/dev/null
    echo "$rc" > "'"$logdir"'/.done"
  ' >/dev/null 2>&1 < /dev/null &
  printf 'launched %-11s session=%-16s sandbox=%s\n' "$lang" "$sid" "$sb"
done

echo
echo "${#LANGS_ARR[@]} agents launched (sandboxed). Monitor: ./scripts/round-status.sh $ROUND"
