# Benchmark report — round-2

**DeepSeek V4.1 Flash — one-shot (spec v2)**

Model: **DeepSeek V4.1 Flash** (`deepseek/deepseek-flash`, DeepSeek-V4.1-Flash-1017) · 763B total, ~16B active (8B active during prefill) · thinking=high

Tests per implementation: 32 public + 80 holdout = 112 scored (+2 future-spec, not scored)

| lang | status | scored | public | holdout | future-spec | output tok | total tok | cost $ | tool calls | src files | src lines | gen eff (k) | tot eff (k) | rel cost |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| python | ok | 112/112 | 32/32 | 80/80 | 0/2 | 82,123 | 3,323,523 | 0.1278 | 50 | 2 | 1297 | 1.36 | 0.03 | 1.0 |
| javascript | ok | 112/112 | 32/32 | 80/80 | 2/2 | 119,392 | 4,913,756 | 0.1804 | 62 | 2 | 1694 | 0.94 | 0.02 | 1.48 |
| go | ok | 112/112 | 32/32 | 80/80 | 2/2 | 110,406 | 3,455,929 | 0.1630 | 38 | 6 | 2416 | 1.01 | 0.03 | 1.04 |
| rust | ok | 112/112 | 32/32 | 80/80 | 2/2 | 104,408 | 4,194,694 | 0.1579 | 56 | 2 | 1792 | 1.07 | 0.03 | 1.26 |
| c | ok | 112/112 | 32/32 | 80/80 | 2/2 | 118,885 | 6,107,267 | 0.1916 | 61 | 3 | 3286 | 0.94 | 0.02 | 1.84 |
| oracle | reference | 112/112 | 32/32 | 80/80 | 2/2 | — | — | — | — | 1 | 904 | — | — | — |

**Metrics** — `output/total tok`: provider-reported usage summed from the pi session. `gen eff (k)` = passed / output_tokens × 1000. `tot eff (k)` = passed / total_tokens × 1000. `rel cost` = total_tokens ÷ best conforming total. `holdout` = edge-case suite scored for this round's spec version. `future-spec` = holdout cases for rules added after this round (informational).

## Leaderboard (conforming, by total tokens)

1. **python** — 3,323,523 tokens, 82,123 output, 50 tool calls, $0.1278
2. **go** — 3,455,929 tokens, 110,406 output, 38 tool calls, $0.1630
3. **rust** — 4,194,694 tokens, 104,408 output, 56 tool calls, $0.1579
4. **javascript** — 4,913,756 tokens, 119,392 output, 62 tool calls, $0.1804
5. **c** — 6,107,267 tokens, 118,885 output, 61 tool calls, $0.1916
