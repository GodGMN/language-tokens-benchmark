# benchmark/ — evaluation harness

Language-agnostic conformance harness. Pure Python 3 stdlib for build/runner/collect/report (`jsonschema` optional).

## Layout

- `adapters/*.json` — how each language is built and invoked.
- `fixtures/<category>/*.json` — conformance cases: `{name, input, expected_exit_code, expected_output?, expected_error_code?}`.
- `tests/` — (later) pytest suite wrapping the same fixtures.
- `schemas/` — JSON schemas for the input document and the response.
- `rounds/<round-id>/` — everything per experiment round:
  - `round.json` — model + mapping of language → pi session (id or path).
  - `REQUIREMENTS.template.md` + `make_prompts.py` → `prompt-<language>.md` (the text you paste into each pi instance).
- Entry points:
  - `build.py --all | --language X`
  - `runner.py --all | --language X [--category C] [--timeout N]`
  - `collect.py --round R [--session LANG=path-or-id]` — parses pi session JSONL for exact token usage.
  - `report.py --round R | --latest` — writes `results/R/report.md`.

## Token metrics — what pi gives us

pi writes every session to JSONL:

```
~/.pi/agent/sessions/--<cwd with / replaced by ->--/<timestamp>_<session-id>.jsonl
```

`collect.py` reads those files and sums, per session:

| Field | Source | Notes |
|---|---|---|
| `input` | `message.usage.input` | tokens sent to the provider |
| `output` | `message.usage.output` | tokens generated |
| `cacheRead` / `cacheWrite` | `message.usage.cache*` | prompt-cache hits/writes |
| `reasoning` | `message.usage.reasoning` | only when the provider reports it |
| `totalTokens` | `message.usage.totalTokens` | provider total |
| `cost` | `message.usage.cost` | provider pricing, if known |
| `turns` / `tool_calls` | message roles | `user` messages / `toolResult` messages |
| `compactions` | `compaction` entries | context compactions (their usage is counted too) |

Standalone `usage` entries (e.g. cache warming) are included; `compaction`
entries contribute their summarization usage. Reasoning tokens are **not**
added twice: they are reported separately and already included in
`totalTokens`.

Caveat: if a provider does not report usage during streaming, `input`/`output`
can be 0 for that turn. `collect.py` sums what the provider reports and never
estimates. Metrics are marked as estimates nowhere — they are raw provider
numbers, or absent.

## Running a round (pi-based)

1. Generate prompts: `python3 benchmark/rounds/round-1/make_prompts.py`
2. For each language, start a pi instance **inside its folder** with a fixed session id:
   ```bash
   cd cli-go && pi --session-id round1-go
   ```
3. Paste the content of `benchmark/rounds/round-1/prompt-go.md` into it. Let it finish.
4. Fill `model`/`provider` in `rounds/round-1/round.json` (sessions are also matched by the ids already there; override with `--session go=<path>` if needed).
5. Run the pipeline: `./scripts/benchmark.sh round-1`

Pi stores sessions as JSONL under `~/.pi/agent/sessions/--<cwd>--/<timestamp>_<session-id>.jsonl`. Each assistant message carries provider-exact `usage` (`input`, `output`, `cacheRead`, `cacheWrite`, `reasoning?`, `totalTokens`, `cost`); `collect.py` sums them (plus standalone `usage` and `compaction` entries) into `results/<round>/metrics.json`.
