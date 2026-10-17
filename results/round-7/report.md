# Benchmark report — round-7

**DeepSeek V4.1 Flash — one-shot (spec v2, sample 6)**

Model: **DeepSeek V4.1 Flash** (`deepseek/deepseek-flash`, DeepSeek-V4.1-Flash-1017) · 763B total, ~16B active (8B active during prefill) · thinking=high

Tests per implementation: 32 public + 80 holdout = 112 scored (+2 future-spec, not scored)

| lang | status | scored | public | holdout | future-spec | output tok | total tok | cost $ | tool calls | src files | src lines | gen eff (k) | tot eff (k) | rel cost |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| python | ok | 112/112 | 32/32 | 80/80 | 2/2 | 123,874 | 6,737,225 | 0.2003 | 78 | 2 | 1704 | 0.9 | 0.02 | 2.24 |
| javascript | ok | 112/112 | 32/32 | 80/80 | 2/2 | 87,820 | 3,007,457 | 0.1322 | 41 | 2 | 1609 | 1.28 | 0.04 | 1.0 |
| go | ok | 112/112 | 32/32 | 80/80 | 1/2 | 78,537 | 4,149,141 | 0.1277 | 66 | 7 | 2385 | 1.43 | 0.03 | 1.38 |
| rust | ok | 112/112 | 32/32 | 80/80 | 2/2 | 131,824 | 7,532,337 | 0.2146 | 86 | 1 | 1883 | 0.85 | 0.01 | 2.5 |
| c | ok | 112/112 | 32/32 | 80/80 | 1/2 | 154,778 | 12,365,110 | 0.2866 | 97 | 1 | 3137 | 0.72 | 0.01 | 4.11 |
| oracle | reference | 112/112 | 32/32 | 80/80 | 2/2 | — | — | — | — | 1 | 904 | — | — | — |

**Metrics** — `output/total tok`: provider-reported usage summed from the pi session. `gen eff (k)` = passed / output_tokens × 1000. `tot eff (k)` = passed / total_tokens × 1000. `rel cost` = total_tokens ÷ best conforming total. `holdout` = edge-case suite scored for this round's spec version. `future-spec` = holdout cases for rules added after this round (informational).

## Leaderboard (conforming, by total tokens)

1. **javascript** — 3,007,457 tokens, 87,820 output, 41 tool calls, $0.1322
2. **go** — 4,149,141 tokens, 78,537 output, 66 tool calls, $0.1277
3. **python** — 6,737,225 tokens, 123,874 output, 78 tool calls, $0.2003
4. **rust** — 7,532,337 tokens, 131,824 output, 86 tool calls, $0.2146
5. **c** — 12,365,110 tokens, 154,778 output, 97 tool calls, $0.2866
