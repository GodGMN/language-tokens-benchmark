# Benchmark report — round-1

**DeepSeek V4.1 Flash — one-shot**

Model: **DeepSeek V4.1 Flash** (`deepseek/deepseek-flash`, DeepSeek-V4.1-Flash-1017) · 763B total, ~16B active (8B active during prefill) · thinking=high

Tests per implementation: 32 public + 75 holdout = 107 scored (+7 future-spec, not scored)

| lang | status | scored | public | holdout | future-spec | output tok | total tok | cost $ | tool calls | src files | src lines | gen eff (k) | tot eff (k) | rel cost |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| python | ok | 107/107 | 32/32 | 75/75 | 1/7 | 125,031 | 6,222,745 | 0.1999 | 76 | 2 | 1688 | 0.86 | 0.02 | 2.1 |
| javascript | ok | 107/107 | 32/32 | 75/75 | 1/7 | 90,736 | 2,959,548 | 0.1338 | 57 | 2 | 1987 | 1.18 | 0.04 | 1.0 |
| go | ok | 107/107 | 32/32 | 75/75 | 1/7 | 113,306 | 3,365,415 | 0.1624 | 46 | 6 | 2437 | 0.94 | 0.03 | 1.14 |
| rust | ok | 107/107 | 32/32 | 75/75 | 2/7 | 156,063 | 9,405,161 | 0.2549 | 85 | 7 | 2281 | 0.69 | 0.01 | 3.18 |
| c | ok | 107/107 | 32/32 | 75/75 | 5/7 | 121,998 | 5,224,057 | 0.1907 | 68 | 3 | 3477 | 0.88 | 0.02 | 1.77 |
| oracle | reference | 107/107 | 32/32 | 75/75 | 7/7 | — | — | — | — | 1 | 904 | — | — | — |

**Metrics** — `output/total tok`: provider-reported usage summed from the pi session. `gen eff (k)` = passed / output_tokens × 1000. `tot eff (k)` = passed / total_tokens × 1000. `rel cost` = total_tokens ÷ best conforming total. `holdout` = edge-case suite scored for this round's spec version. `future-spec` = holdout cases for rules added after this round (informational).

## Leaderboard (conforming, by total tokens)

1. **javascript** — 2,959,548 tokens, 90,736 output, 57 tool calls, $0.1338
2. **go** — 3,365,415 tokens, 113,306 output, 46 tool calls, $0.1624
3. **c** — 5,224,057 tokens, 121,998 output, 68 tool calls, $0.1907
4. **python** — 6,222,745 tokens, 125,031 output, 76 tool calls, $0.1999
5. **rust** — 9,405,161 tokens, 156,063 output, 85 tool calls, $0.2549
