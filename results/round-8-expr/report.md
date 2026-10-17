# Benchmark report — round-8-expr

**DeepSeek V4.1 Flash — expression evaluator (spec v1)**  ·  task `expr`

Model: **DeepSeek V4.1 Flash** (`deepseek/deepseek-flash`, DeepSeek-V4.1-Flash-1017) · 763B total, ~16B active (8B active during prefill) · thinking=high

Tests per implementation: 26 public + 30 holdout = 56 scored

| lang | status | scored | public | holdout | future-spec | output tok | total tok | cost $ | tool calls | src files | src lines | gen eff (k) | tot eff (k) | rel cost |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| python | ok | 56/56 | 26/26 | 30/30 | — | 73,983 | 2,524,500 | 0.1122 | 38 | 2 | 1340 | 0.76 | 0.02 | 2.37 |
| javascript | ok | 56/56 | 26/26 | 30/30 | — | 60,022 | 1,241,698 | 0.0835 | 24 | 3 | 1716 | 0.93 | 0.05 | 1.16 |
| go | ok | 56/56 | 26/26 | 30/30 | — | 60,130 | 1,408,634 | 0.0861 | 36 | 6 | 1506 | 0.93 | 0.04 | 1.32 |
| rust | ok | 56/56 | 26/26 | 30/30 | — | 59,463 | 1,067,224 | 0.0802 | 23 | 1 | 1748 | 0.94 | 0.05 | 1.0 |
| c | ok | 56/56 | 26/26 | 30/30 | — | 71,378 | 1,887,134 | 0.1026 | 34 | 2 | 2882 | 0.78 | 0.03 | 1.77 |
| oracle | reference | 56/56 | 26/26 | 30/30 | — | — | — | — | — | 1 | 527 | — | — | — |

**Metrics** — `output/total tok`: provider-reported usage summed from the pi session. `gen eff (k)` = passed / output_tokens × 1000. `tot eff (k)` = passed / total_tokens × 1000. `rel cost` = total_tokens ÷ best conforming total. `holdout` = edge-case suite scored for this round's spec version. `future-spec` = holdout cases for rules added after this round (informational).

## Leaderboard (conforming, by total tokens)

1. **rust** — 1,067,224 tokens, 59,463 output, 23 tool calls, $0.0802
2. **javascript** — 1,241,698 tokens, 60,022 output, 24 tool calls, $0.0835
3. **go** — 1,408,634 tokens, 60,130 output, 36 tool calls, $0.0861
4. **c** — 1,887,134 tokens, 71,378 output, 34 tool calls, $0.1026
5. **python** — 2,524,500 tokens, 73,983 output, 38 tool calls, $0.1122
