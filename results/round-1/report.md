# Benchmark report — round-1

Model: (see metrics.json)

| language | status | tests | % | conforming | in | out | total | reasoning | cache R/W | cost $ | turns | tool calls | src files | src lines | gen eff (k) | tot eff (k) | rel cost |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| python | not run | — | — | — | — | — | — | — | —/— | — | — | — | 0 | 0 | — | — | — |
| javascript | not run | — | — | — | — | — | — | — | —/— | — | — | — | 0 | 0 | — | — | — |
| go | not run | — | — | — | — | — | — | — | —/— | — | — | — | 0 | 0 | — | — | — |
| rust | not run | — | — | — | — | — | — | — | —/— | — | — | — | 0 | 0 | — | — | — |
| c | not run | — | — | — | — | — | — | — | —/— | — | — | — | 0 | 0 | — | — | — |
| oracle | reference | 32/32 | 100.0 | yes | — | — | — | — | —/— | — | — | — | 1 | 862 | — | — | — |

**Metrics** — `in/out/total/reasoning/cache`: provider-reported tokens summed from the pi session (`message.usage` on every assistant turn). `gen eff (k)` = passed_tests / output_tokens * 1000. `tot eff (k)` = passed_tests / total_tokens * 1000. `rel cost` = total_tokens / min(total_tokens of conforming implementations).

_No conforming implementations yet._

_Not run yet: python, javascript, go, rust, c._

