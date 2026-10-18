# Benchmark report — round-6

**DeepSeek V4.1 Flash — one-shot (spec v2, sample 5)**  ·  task `workflow`

Model: **DeepSeek V4.1 Flash** (`deepseek/deepseek-flash`, DeepSeek-V4.1-Flash-1017) · 763B total, ~16B active (8B active during prefill) · thinking=high

Tests per implementation: 32 public + 80 holdout = 112 scored (+2 future-spec, not scored)

| lang | status | scored | public | holdout | future-spec | output tok | total tok | cost $ | tool calls | src files | src lines | gen eff (k) | tot eff (k) | rel cost |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| python | ok | 112/112 | 32/32 | 80/80 | 2/2 | 86,096 | 3,667,690 | 0.1347 | 46 | 2 | 1672 | 1.3 | 0.03 | 1.0 |
| javascript | ok | 112/112 | 32/32 | 80/80 | 2/2 | 96,639 | 4,513,010 | 0.1527 | 57 | 2 | 1397 | 1.16 | 0.02 | 1.23 |
| go | ok | 112/112 | 32/32 | 80/80 | 2/2 | 102,388 | 4,517,854 | 0.1588 | 60 | 5 | 2063 | 1.09 | 0.02 | 1.23 |
| rust | ok | 112/112 | 32/32 | 80/80 | 2/2 | 135,279 | 8,987,971 | 0.2302 | 89 | 2 | 1967 | 0.83 | 0.01 | 2.45 |
| c | ok | 112/112 | 32/32 | 80/80 | 2/2 | 108,581 | 3,997,438 | 0.1626 | 46 | 4 | 3112 | 1.03 | 0.03 | 1.09 |
| oracle | reference | 112/112 | 32/32 | 80/80 | 2/2 | — | — | — | — | 1 | 904 | — | — | — |

Evaluation: `json-comparison-v2` · 2026-10-18T00:31:07.993169+00:00. Rescored committed submissions using historical usage; no new model calls. See [evaluation notes](../../docs/evaluation-notes.md).

**Metrics** — `output/total tok`: provider-reported usage summed from the pi session. `gen eff (k)` = passed / output_tokens × 1000. `tot eff (k)` = passed / total_tokens × 1000. `rel cost` = total_tokens ÷ best conforming total. `holdout` = edge-case suite scored for this round's spec version. `future-spec` = holdout cases for rules added after this round (informational).

## Leaderboard (conforming, by total tokens)

1. **python** — 3,667,690 tokens, 86,096 output, 46 tool calls, $0.1347
2. **c** — 3,997,438 tokens, 108,581 output, 46 tool calls, $0.1626
3. **javascript** — 4,513,010 tokens, 96,639 output, 57 tool calls, $0.1527
4. **go** — 4,517,854 tokens, 102,388 output, 60 tool calls, $0.1588
5. **rust** — 8,987,971 tokens, 135,279 output, 89 tool calls, $0.2302
