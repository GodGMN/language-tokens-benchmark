# Benchmark report — round-8-expr

**DeepSeek V4.1 Flash — expression evaluator (spec v1)**  ·  task `expr`

Model: **DeepSeek V4.1 Flash** (`deepseek/deepseek-flash`, DeepSeek-V4.1-Flash-1017) · 763B total, ~16B active (8B active during prefill) · thinking=high

Tests per implementation: 26 public + 30 holdout = 56 scored

| lang | status | scored | public | holdout | future-spec | output tok | total tok | cost $ | tool calls | src files | src lines | gen eff (k) | tot eff (k) | rel cost |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| python | failing | 55/56 | 26/26 | 29/30 | — | 73,983 | 2,524,500 | 0.1122 | 38 | 2 | 1340 | 0.74 | 0.02 | — |
| javascript | failing | 55/56 | 26/26 | 29/30 | — | 60,022 | 1,241,698 | 0.0835 | 24 | 3 | 1716 | 0.92 | 0.04 | — |
| go | failing | 54/56 | 26/26 | 28/30 | — | 60,130 | 1,408,634 | 0.0861 | 36 | 6 | 1506 | 0.9 | 0.04 | — |
| rust | failing | 55/56 | 26/26 | 29/30 | — | 59,463 | 1,067,224 | 0.0802 | 23 | 1 | 1748 | 0.92 | 0.05 | — |
| c | failing | 55/56 | 26/26 | 29/30 | — | 71,378 | 1,887,134 | 0.1026 | 34 | 2 | 2882 | 0.77 | 0.03 | — |
| oracle | reference | 56/56 | 26/26 | 30/30 | — | — | — | — | — | 1 | 527 | — | — | — |

**Metrics** — `output/total tok`: provider-reported usage summed from the pi session. `gen eff (k)` = passed / output_tokens × 1000. `tot eff (k)` = passed / total_tokens × 1000. `rel cost` = total_tokens ÷ best conforming total. `holdout` = edge-case suite scored for this round's spec version. `future-spec` = holdout cases for rules added after this round (informational).

_No conforming implementations yet._

## Non-conforming (by pass rate)

1. **python** — 98.2% (55/56)
2. **javascript** — 98.2% (55/56)
3. **rust** — 98.2% (55/56)
4. **c** — 98.2% (55/56)
5. **go** — 96.4% (54/56)
