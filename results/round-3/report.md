# Benchmark report — round-3

**DeepSeek V4.1 Flash — one-shot (spec v2, sample 2)**

Model: **DeepSeek V4.1 Flash** (`deepseek/deepseek-flash`, DeepSeek-V4.1-Flash-1017) · 763B total, ~16B active (8B active during prefill) · thinking=high

Tests per implementation: 32 public + 82 holdout = 114 scored

| lang | status | scored | public | holdout | future-spec | output tok | total tok | cost $ | tool calls | src files | src lines | gen eff (k) | tot eff (k) | rel cost |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| python | ok | 114/114 | 32/32 | 82/82 | — | 106,168 | 5,585,811 | 0.1709 | 66 | 2 | 1620 | 1.07 | 0.02 | 2.95 |
| javascript | failing | 112/114 | 32/32 | 80/82 | — | 67,714 | 1,351,236 | 0.0945 | 28 | 2 | 1772 | 1.65 | 0.08 | 0.71 |
| go | ok | 114/114 | 32/32 | 82/82 | — | 69,835 | 1,891,864 | 0.1012 | 37 | 6 | 2455 | 1.63 | 0.06 | 1.0 |
| rust | ok | 114/114 | 32/32 | 82/82 | — | 112,031 | 4,635,085 | 0.1718 | 60 | 8 | 2538 | 1.02 | 0.02 | 2.45 |
| c | ok | 114/114 | 32/32 | 82/82 | — | 142,225 | 6,068,702 | 0.2152 | 60 | 2 | 3574 | 0.8 | 0.02 | 3.21 |
| oracle | reference | 114/114 | 32/32 | 82/82 | — | — | — | — | — | 1 | 904 | — | — | — |

**Metrics** — `output/total tok`: provider-reported usage summed from the pi session. `gen eff (k)` = passed / output_tokens × 1000. `tot eff (k)` = passed / total_tokens × 1000. `rel cost` = total_tokens ÷ best conforming total. `holdout` = edge-case suite scored for this round's spec version. `future-spec` = holdout cases for rules added after this round (informational).

## Leaderboard (conforming, by total tokens)

1. **go** — 1,891,864 tokens, 69,835 output, 37 tool calls, $0.1012
2. **rust** — 4,635,085 tokens, 112,031 output, 60 tool calls, $0.1718
3. **python** — 5,585,811 tokens, 106,168 output, 66 tool calls, $0.1709
4. **c** — 6,068,702 tokens, 142,225 output, 60 tool calls, $0.2152

## Non-conforming (by pass rate)

1. **javascript** — 98.2% (112/114)
