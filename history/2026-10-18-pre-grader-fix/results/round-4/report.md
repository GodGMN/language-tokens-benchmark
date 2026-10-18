# Benchmark report — round-4

**DeepSeek V4.1 Flash — one-shot (spec v2, sample 3)**

Model: **DeepSeek V4.1 Flash** (`deepseek/deepseek-flash`, DeepSeek-V4.1-Flash-1017) · 763B total, ~16B active (8B active during prefill) · thinking=high

Tests per implementation: 32 public + 80 holdout = 112 scored (+2 future-spec, not scored)

| lang | status | scored | public | holdout | future-spec | output tok | total tok | cost $ | tool calls | src files | src lines | gen eff (k) | tot eff (k) | rel cost |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| python | ok | 112/112 | 32/32 | 80/80 | 2/2 | 78,385 | 3,121,625 | 0.1212 | 63 | 2 | 1356 | 1.43 | 0.04 | 1.0 |
| javascript | ok | 112/112 | 32/32 | 80/80 | 2/2 | 157,602 | 9,721,332 | 0.2605 | 79 | 2 | 1715 | 0.71 | 0.01 | 3.11 |
| go | ok | 112/112 | 32/32 | 80/80 | 0/2 | 170,119 | 6,404,923 | 0.2514 | 59 | 5 | 2809 | 0.66 | 0.02 | 2.05 |
| rust | ok | 112/112 | 32/32 | 80/80 | 2/2 | 125,886 | 8,193,632 | 0.2108 | 87 | 6 | 1889 | 0.89 | 0.01 | 2.62 |
| c | ok | 112/112 | 32/32 | 80/80 | 1/2 | 117,365 | 6,633,406 | 0.1984 | 66 | 1 | 2529 | 0.95 | 0.02 | 2.12 |
| oracle | reference | 112/112 | 32/32 | 80/80 | 2/2 | — | — | — | — | 1 | 904 | — | — | — |

**Metrics** — `output/total tok`: provider-reported usage summed from the pi session. `gen eff (k)` = passed / output_tokens × 1000. `tot eff (k)` = passed / total_tokens × 1000. `rel cost` = total_tokens ÷ best conforming total. `holdout` = edge-case suite scored for this round's spec version. `future-spec` = holdout cases for rules added after this round (informational).

## Leaderboard (conforming, by total tokens)

1. **python** — 3,121,625 tokens, 78,385 output, 63 tool calls, $0.1212
2. **go** — 6,404,923 tokens, 170,119 output, 59 tool calls, $0.2514
3. **c** — 6,633,406 tokens, 117,365 output, 66 tool calls, $0.1984
4. **rust** — 8,193,632 tokens, 125,886 output, 87 tool calls, $0.2108
5. **javascript** — 9,721,332 tokens, 157,602 output, 79 tool calls, $0.2605
