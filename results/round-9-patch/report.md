# Benchmark report — round-9-patch

**DeepSeek V4.1 Flash — JSON patch engine (spec v1)**  ·  task `patch`

Model: **DeepSeek V4.1 Flash** (`deepseek/deepseek-flash`, DeepSeek-V4.1-Flash-1017) · 763B total, ~16B active (8B active during prefill) · thinking=high

Tests per implementation: 14 public + 24 holdout = 38 scored

| lang | status | scored | public | holdout | future-spec | output tok | total tok | cost $ | tool calls | src files | src lines | gen eff (k) | tot eff (k) | rel cost |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| python | failing | 37/38 | 14/14 | 23/24 | — | 37,514 | 700,543 | 0.0539 | 21 | 2 | 1029 | 0.99 | 0.05 | — |
| javascript | failing | 37/38 | 14/14 | 23/24 | — | 66,718 | 1,562,150 | 0.0945 | 31 | 2 | 1366 | 0.55 | 0.02 | — |
| go | failing | 37/38 | 14/14 | 23/24 | — | 42,905 | 709,941 | 0.0590 | 24 | 5 | 1023 | 0.86 | 0.05 | — |
| rust | failing | 37/38 | 14/14 | 23/24 | — | 44,765 | 925,327 | 0.0654 | 36 | 2 | 857 | 0.83 | 0.04 | — |
| c | failing | 37/38 | 14/14 | 23/24 | — | 77,186 | 2,355,515 | 0.1118 | 42 | 4 | 2372 | 0.48 | 0.02 | — |
| oracle | reference | 38/38 | 14/14 | 24/24 | — | — | — | — | — | 1 | 224 | — | — | — |

**Metrics** — `output/total tok`: provider-reported usage summed from the pi session. `gen eff (k)` = passed / output_tokens × 1000. `tot eff (k)` = passed / total_tokens × 1000. `rel cost` = total_tokens ÷ best conforming total. `holdout` = edge-case suite scored for this round's spec version. `future-spec` = holdout cases for rules added after this round (informational).

_No conforming implementations yet._

## Non-conforming (by pass rate)

1. **python** — 97.4% (37/38)
2. **javascript** — 97.4% (37/38)
3. **go** — 97.4% (37/38)
4. **rust** — 97.4% (37/38)
5. **c** — 97.4% (37/38)
