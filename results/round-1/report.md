# Benchmark report — round-1

Model: deepseek-flash

| language | status | tests | % | conforming | in | out | total | reasoning | cache R/W | cost $ | turns | tool calls | src files | src lines | gen eff (k) | tot eff (k) | rel cost |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| python | ok | 32/32 | 100.0 | yes | 45106 | 125031 | 6222745 | 91213 | 6052608/0 | 0.1999 | 1 | 76 | 2 | 1688 | 0.26 | 0.01 | 2.1 |
| javascript | ok | 32/32 | 100.0 | yes | 26188 | 90736 | 2959548 | 59475 | 2842624/0 | 0.1338 | 1 | 57 | 2 | 1987 | 0.35 | 0.01 | 1.0 |
| go | ok | 32/32 | 100.0 | yes | 23565 | 113306 | 3365415 | 78740 | 3228544/0 | 0.1624 | 1 | 46 | 6 | 2437 | 0.28 | 0.01 | 1.14 |
| rust | ok | 32/32 | 100.0 | yes | 41418 | 156063 | 9405161 | 114295 | 9207680/0 | 0.2549 | 1 | 85 | 7 | 2281 | 0.21 | 0.0 | 3.18 |
| c | ok | 32/32 | 100.0 | yes | 46699 | 121998 | 5224057 | 63902 | 5055360/0 | 0.1907 | 1 | 68 | 3 | 3477 | 0.26 | 0.01 | 1.77 |
| oracle | reference | 32/32 | 100.0 | yes | — | — | — | — | —/— | — | — | — | 1 | 862 | — | — | — |

**Metrics** — `in/out/total/reasoning/cache`: provider-reported tokens summed from the pi session (`message.usage` on every assistant turn). `gen eff (k)` = passed_tests / output_tokens * 1000. `tot eff (k)` = passed_tests / total_tokens * 1000. `rel cost` = total_tokens / min(total_tokens of conforming implementations).

**Ranking** (conforming implementations, by lowest total tokens):
1. **javascript** — 2959548 total tokens, 1 turns, 57 tool calls
2. **go** — 3365415 total tokens, 1 turns, 46 tool calls
3. **c** — 5224057 total tokens, 1 turns, 68 tool calls
4. **python** — 6222745 total tokens, 1 turns, 76 tool calls
5. **rust** — 9405161 total tokens, 1 turns, 85 tool calls

