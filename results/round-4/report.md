# Benchmark report — round-4

**DeepSeek V4.1 Flash — one-shot (spec v2, sample 3)**

Model: **DeepSeek V4.1 Flash** (`deepseek/deepseek-flash`, DeepSeek-V4.1-Flash-1017) · 763B total, ~16B active (8B active during prefill) · thinking=high

Tests per implementation: 32 public + 82 holdout = 114 scored

| lang | status | scored | public | holdout | future-spec | output tok | total tok | cost $ | tool calls | src files | src lines | gen eff (k) | tot eff (k) | rel cost |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| python | ok | 114/114 | 32/32 | 82/82 | — | 78,385 | 3,121,625 | 0.1212 | 63 | 2 | 1356 | 1.45 | 0.04 | 1.0 |
| javascript | ok | 114/114 | 32/32 | 82/82 | — | 157,602 | 9,721,332 | 0.2605 | 79 | 2 | 1715 | 0.72 | 0.01 | 3.11 |
| go | failing | 112/114 | 32/32 | 80/82 | — | 170,119 | 6,404,923 | 0.2514 | 59 | 5 | 2809 | 0.66 | 0.02 | 2.05 |
| rust | ok | 114/114 | 32/32 | 82/82 | — | 125,886 | 8,193,632 | 0.2108 | 87 | 6 | 1889 | 0.91 | 0.01 | 2.62 |
| c | failing | 113/114 | 32/32 | 81/82 | — | 117,365 | 6,633,406 | 0.1984 | 66 | 1 | 2529 | 0.96 | 0.02 | 2.12 |
| oracle | reference | 114/114 | 32/32 | 82/82 | — | — | — | — | — | 1 | 904 | — | — | — |

**Metrics** — `output/total tok`: provider-reported usage summed from the pi session. `gen eff (k)` = passed / output_tokens × 1000. `tot eff (k)` = passed / total_tokens × 1000. `rel cost` = total_tokens ÷ best conforming total. `holdout` = edge-case suite scored for this round's spec version. `future-spec` = holdout cases for rules added after this round (informational).

## Leaderboard (conforming, by total tokens)

1. **python** — 3,121,625 tokens, 78,385 output, 63 tool calls, $0.1212
2. **rust** — 8,193,632 tokens, 125,886 output, 87 tool calls, $0.2108
3. **javascript** — 9,721,332 tokens, 157,602 output, 79 tool calls, $0.2605

## Non-conforming (by pass rate)

1. **c** — 99.1% (113/114)
2. **go** — 98.2% (112/114)
