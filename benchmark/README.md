# benchmark/ — evaluation harness

Language-agnostic conformance harness. Python 3 stdlib only.

## Layout

- `adapters/<lang>.json` — how each language is built and invoked. `build` may be `null`; `requires` lists files that mean "implemented"; `run_stdin` is the stdin variant.
- `fixtures/<category>/*.json` — **public** suite.
- `holdout/<category>/*.json` — **scored edge-case** suite. Cases tagged `"since": N` only count once a round's `spec_version >= N`.
- `oracle/workflow.py` — independent reference implementation used to validate fixtures and the harness. Not a submission.
- `rounds/<round>/` — `round.json` (model, spec version, sessions, submissions dir) + `REQUIREMENTS.template.md` + generated `prompt-<lang>.md`.
- `common.py` — round/adapter/project resolution shared by the entry points.
- Entry points: `build.py`, `runner.py`, `collect.py`, `report.py`, `site.py`.

## Test format

See [`../docs/test-format.md`](../docs/test-format.md).

## Running

```bash
python3 benchmark/build.py --all                       # default round
python3 benchmark/runner.py --all                      # public + holdout
python3 benchmark/runner.py --all --public             # public only
python3 benchmark/runner.py --language go --category numeric
python3 benchmark/collect.py --round round-1           # pi token metrics
python3 benchmark/report.py  --round round-1           # report.md + summary.json
python3 benchmark/site.py                              # results/index.json
```

Or `./scripts/benchmark.sh round-1` for the whole chain.

## Scoring buckets

Each implementation is scored against the spec version of its round:

| bucket | counted? | meaning |
|---|---|---|
| `public` | yes | `fixtures/` |
| `holdout` | yes | `holdout/` cases with `since <= spec_version` |
| `future` | no | `holdout/` cases for newer spec rules (reported only) |

This is why `results/<round>/report.md` shows both a scored pass count and a
`future-spec` count.

## Token metrics

`collect.py` reads pi session JSONL and sums provider-reported usage per
session: `input`, `output`, `cacheRead`, `cacheWrite`, `reasoning`,
`totalTokens`, `cost`, plus turn/tool-call/compaction counts. See
[`../docs/methodology.md`](../docs/methodology.md).

The session for a language is read from `rounds/<round>/round.json`:

```json
"python": { "path": "python", "session": "round1-python" }
```

`session` may be an id, a `.jsonl` path, or a cwd. Override ad hoc with
`--session python=/path/to/session.jsonl`.
