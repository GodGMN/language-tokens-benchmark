# Benchmark report — round-3

**DeepSeek V4.1 Flash — one-shot (spec v2, sample 2)**

Model: **DeepSeek V4.1 Flash** (`deepseek/deepseek-flash`, DeepSeek-V4.1-Flash-1017) · 763B total, ~16B active (8B active during prefill) · thinking=high

Tests per implementation: 32 public + 80 holdout = 112 scored (+2 future-spec, not scored)

| lang | status | scored | public | holdout | future-spec | output tok | total tok | cost $ | tool calls | src files | src lines | gen eff (k) | tot eff (k) | rel cost |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| python | ok | 112/112 | 32/32 | 80/80 | 2/2 | 106,168 | 5,585,811 | 0.1709 | 66 | 2 | 1620 | 1.05 | 0.02 | 4.13 |
| javascript | ok | 112/112 | 32/32 | 80/80 | 0/2 | 67,714 | 1,351,236 | 0.0945 | 28 | 2 | 1772 | 1.65 | 0.08 | 1.0 |
| go | ok | 112/112 | 32/32 | 80/80 | 2/2 | 69,835 | 1,891,864 | 0.1012 | 37 | 6 | 2455 | 1.6 | 0.06 | 1.4 |
| rust | ok | 112/112 | 32/32 | 80/80 | 2/2 | 112,031 | 4,635,085 | 0.1718 | 60 | 8 | 2538 | 1.0 | 0.02 | 3.43 |
| c | ok | 112/112 | 32/32 | 80/80 | 2/2 | 142,225 | 6,068,702 | 0.2152 | 60 | 2 | 3574 | 0.79 | 0.02 | 4.49 |
| oracle | reference | 112/112 | 32/32 | 80/80 | 2/2 | — | — | — | — | 1 | 904 | — | — | — |

**Metrics** — `output/total tok`: provider-reported usage summed from the pi session. `gen eff (k)` = passed / output_tokens × 1000. `tot eff (k)` = passed / total_tokens × 1000. `rel cost` = total_tokens ÷ best conforming total. `holdout` = edge-case suite scored for this round's spec version. `future-spec` = holdout cases for rules added after this round (informational).

## Leaderboard (conforming, by total tokens)

1. **javascript** — 1,351,236 tokens, 67,714 output, 28 tool calls, $0.0945
2. **go** — 1,891,864 tokens, 69,835 output, 37 tool calls, $0.1012
3. **rust** — 4,635,085 tokens, 112,031 output, 60 tool calls, $0.1718
4. **python** — 5,585,811 tokens, 106,168 output, 66 tool calls, $0.1709
5. **c** — 6,068,702 tokens, 142,225 output, 60 tool calls, $0.2152
