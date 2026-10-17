# Isolation

The benchmark only means something if each implementation is produced without
seeing the others, the public suite, or the holdout suite.

## What the agent gets

- Its own directory: `submissions/<round>/<lang>/` (its cwd).
- Read access to `SPEC.md` and `PRD.md`.
- A prompt (`benchmark/rounds/<round>/prompt-<lang>.md`) that states the
  read/write boundary explicitly.

## What it must not see

- Other `submissions/<round>/*` folders.
- `benchmark/fixtures/` (public suite).
- `benchmark/holdout/` (scored edge cases).
- `benchmark/oracle/` (reference implementation — would be cheating).

The prompt forbids these, but **a prompt is not enforcement**. A pi session with
normal tools can read any file the OS allows.

## Enforcing it

For any round you intend to publish, run the agents in a worktree that does not
contain the suite or the sibling submissions:

```bash
git worktree add ../wt-go round-base --no-checkout
cd ../wt-go && git sparse-checkout set submissions/round-2/go SPEC.md PRD.md
```

Then start the agent there and point `AGENTS` at it. The launcher reads
`submissions/<round>/<lang>` from `round.json`; if you relocate the worktree,
override the submission path per language.

A simpler alternative for a private round: copy `submissions/<round>/<lang>/`
plus `SPEC.md`/`PRD.md` into a scratch directory and run pi there. The session
id is what `collect.py` follows, so the location does not matter for metrics.

## Why the holdout is committed anyway

The holdout suite is committed so published results are reproducible. It is an
*evaluation set*, not a secret. The guarantee we make is that implementations
were produced without seeing it — enforced by the worktree/sandbox above, and
audited by checking each session's tool calls:

```bash
python3 - <<'PY'
import json, glob, os
for s in glob.glob(os.path.expanduser("~/.pi/agent/sessions/**/*_round2-*.jsonl"), recursive=True):
    for line in open(s, errors="ignore"):
        try: e = json.loads(line)
        except: continue
        m = e.get("message", {})
        if m.get("role") == "assistant":
            for b in (m.get("content") or []):
                if isinstance(b, dict) and b.get("type") == "toolCall":
                    blob = json.dumps(b.get("arguments") or {})
                    if "fixtures" in blob or "holdout" in blob or "oracle" in blob:
                        print("LEAK?", s.split('/')[-1], blob[:200])
PY
```
