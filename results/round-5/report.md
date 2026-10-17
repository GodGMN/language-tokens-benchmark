# Benchmark report — round-5

**DeepSeek V4.1 Flash — one-shot (spec v2, sample 4)**

Model: **DeepSeek V4.1 Flash** (`deepseek/deepseek-flash`, DeepSeek-V4.1-Flash-1017) · 763B total, ~16B active (8B active during prefill) · thinking=high

Tests per implementation: 32 public + 82 holdout = 114 scored

| lang | status | scored | public | holdout | future-spec | output tok | total tok | cost $ | tool calls | src files | src lines | gen eff (k) | tot eff (k) | rel cost |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| python | ok | 114/114 | 32/32 | 82/82 | — | 154,250 | 11,614,289 | 0.2661 | 107 | 2 | 1548 | 0.74 | 0.01 | 4.57 |
| javascript | ok | 114/114 | 32/32 | 82/82 | — | 93,405 | 2,785,938 | 0.1373 | 39 | 1 | 1603 | 1.22 | 0.04 | 1.1 |
| go | ok | 114/114 | 32/32 | 82/82 | — | 101,824 | 2,538,895 | 0.1432 | 36 | 4 | 2271 | 1.12 | 0.04 | 1.0 |
| rust | failing | 112/114 | 32/32 | 80/82 | — | 140,846 | 6,020,679 | 0.2184 | 57 | 2 | 2933 | 0.8 | 0.02 | 2.37 |
| c | failing | 112/114 | 32/32 | 80/82 | — | 121,115 | 6,223,051 | 0.2048 | 58 | 2 | 3289 | 0.92 | 0.02 | 2.45 |
| oracle | reference | 114/114 | 32/32 | 82/82 | — | — | — | — | — | 1 | 904 | — | — | — |

**Metrics** — `output/total tok`: provider-reported usage summed from the pi session. `gen eff (k)` = passed / output_tokens × 1000. `tot eff (k)` = passed / total_tokens × 1000. `rel cost` = total_tokens ÷ best conforming total. `holdout` = edge-case suite scored for this round's spec version. `future-spec` = holdout cases for rules added after this round (informational).

## Leaderboard (conforming, by total tokens)

1. **go** — 2,538,895 tokens, 101,824 output, 36 tool calls, $0.1432
2. **javascript** — 2,785,938 tokens, 93,405 output, 39 tool calls, $0.1373
3. **python** — 11,614,289 tokens, 154,250 output, 107 tool calls, $0.2661

## Non-conforming (by pass rate)

1. **rust** — 98.2% (112/114)
2. **c** — 98.2% (112/114)
