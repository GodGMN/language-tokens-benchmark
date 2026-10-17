# Aggregated results

Samples are grouped by **model group and spec version**; rounds that differ in either are not pooled.

## deepseek-v4.1-flash-1017 · spec v1

Samples: 1 round(s) — round-1  
Version(s): DeepSeek-V4.1-Flash-1017

> Fewer than 3 samples: treat these numbers as indicative only.

| language | samples | conforming | output tok (median [min–max]) | total tok (median [min–max]) | turns (median) |
|---|---|---|---|---|---|
| python | 1 | 1/1 | 125,031 [125,031–125,031] | 6,222,745 [6,222,745–6,222,745] | 65 |
| javascript | 1 | 1/1 | 90,736 [90,736–90,736] | 2,959,548 [2,959,548–2,959,548] | 40 |
| go | 1 | 1/1 | 113,306 [113,306–113,306] | 3,365,415 [3,365,415–3,365,415] | 40 |
| rust | 1 | 1/1 | 156,063 [156,063–156,063] | 9,405,161 [9,405,161–9,405,161] | 85 |
| c | 1 | 1/1 | 121,998 [121,998–121,998] | 5,224,057 [5,224,057–5,224,057] | 55 |

## deepseek-v4.1-flash-1017 · spec v2

Samples: 1 round(s) — round-2  
Version(s): DeepSeek-V4.1-Flash-1017

> Fewer than 3 samples: treat these numbers as indicative only.

| language | samples | conforming | output tok (median [min–max]) | total tok (median [min–max]) | turns (median) |
|---|---|---|---|---|---|
| python | 1 | 0/1 | — | — | 49 |
| javascript | 1 | 1/1 | 119,392 [119,392–119,392] | 4,913,756 [4,913,756–4,913,756] | 53 |
| go | 1 | 1/1 | 110,406 [110,406–110,406] | 3,455,929 [3,455,929–3,455,929] | 37 |
| rust | 1 | 1/1 | 104,408 [104,408–104,408] | 4,194,694 [4,194,694–4,194,694] | 54 |
| c | 1 | 1/1 | 118,885 [118,885–118,885] | 6,107,267 [6,107,267–6,107,267] | 60 |

## Method notes

- Only **conforming** samples contribute to the token medians; a sample that
  fails the suite is recorded in the conformance column but never averaged in.
- Report the median with the min–max range while the sample count is small.
- **Do not pool rounds that differ in model version or spec version.** Both are
  different tasks; group by both. If the provider exposes no version, record it
  manually per round.
- Total tokens ≈ turns × context size, so it is far noisier than output
  tokens. Prefer output tokens for 'effort' and total tokens for 'cost'.
