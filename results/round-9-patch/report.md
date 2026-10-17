# Benchmark report — round-9-patch

**DeepSeek V4.1 Flash — JSON patch engine (spec v1)**  ·  task `patch`

Model: **DeepSeek V4.1 Flash** (`deepseek/deepseek-flash`, DeepSeek-V4.1-Flash-1017) · 763B total, ~16B active (8B active during prefill) · thinking=high

Tests per implementation: 14 public + 24 holdout = 38 scored

| lang | status | scored | public | holdout | future-spec | output tok | total tok | cost $ | tool calls | src files | src lines | gen eff (k) | tot eff (k) | rel cost |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| python | ok | 38/38 | 14/14 | 24/24 | — | 37,514 | 700,543 | 0.0539 | 21 | 2 | 1029 | 1.01 | 0.05 | 1.0 |
| javascript | ok | 38/38 | 14/14 | 24/24 | — | 66,718 | 1,562,150 | 0.0945 | 31 | 2 | 1366 | 0.57 | 0.02 | 2.23 |
| go | ok | 38/38 | 14/14 | 24/24 | — | 42,905 | 709,941 | 0.0590 | 24 | 5 | 1023 | 0.89 | 0.05 | 1.01 |
| rust | ok | 38/38 | 14/14 | 24/24 | — | 44,765 | 925,327 | 0.0654 | 36 | 2 | 857 | 0.85 | 0.04 | 1.32 |
| c | ok | 38/38 | 14/14 | 24/24 | — | 77,186 | 2,355,515 | 0.1118 | 42 | 4 | 2372 | 0.49 | 0.02 | 3.36 |
| oracle | reference | 38/38 | 14/14 | 24/24 | — | — | — | — | — | 1 | 224 | — | — | — |

**Metrics** — `output/total tok`: provider-reported usage summed from the pi session. `gen eff (k)` = passed / output_tokens × 1000. `tot eff (k)` = passed / total_tokens × 1000. `rel cost` = total_tokens ÷ best conforming total. `holdout` = edge-case suite scored for this round's spec version. `future-spec` = holdout cases for rules added after this round (informational).

## Leaderboard (conforming, by total tokens)

1. **python** — 700,543 tokens, 37,514 output, 21 tool calls, $0.0539
2. **go** — 709,941 tokens, 42,905 output, 24 tool calls, $0.0590
3. **rust** — 925,327 tokens, 44,765 output, 36 tool calls, $0.0654
4. **javascript** — 1,562,150 tokens, 66,718 output, 31 tool calls, $0.0945
5. **c** — 2,355,515 tokens, 77,186 output, 42 tool calls, $0.1118
