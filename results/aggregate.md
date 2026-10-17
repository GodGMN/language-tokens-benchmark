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

Samples: 6 round(s) — round-2, round-3, round-4, round-5, round-6, round-7  
Version(s): DeepSeek-V4.1-Flash-1017

| language | samples | conforming | output tok (median [min–max]) | total tok (median [min–max]) | turns (median) |
|---|---|---|---|---|---|
| python | 6 | 5/6 | 106,168 [78,385–154,250] | 5,585,811 [3,121,625–11,614,289] | 56 |
| javascript | 6 | 5/6 | 96,639 [87,820–157,602] | 4,513,010 [2,785,938–9,721,332] | 47 |
| go | 6 | 4/6 | 102,106 [69,835–110,406] | 2,997,412 [1,891,864–4,517,854] | 47 |
| rust | 6 | 5/6 | 125,886 [104,408–135,279] | 7,532,337 [4,194,694–8,987,971] | 71 |
| c | 6 | 3/6 | 118,885 [108,581–142,225] | 6,068,702 [3,997,438–6,107,267] | 58 |

## Method notes

- Only **conforming** samples contribute to the token medians; a sample that
  fails the suite is recorded in the conformance column but never averaged in.
- Report the median with the min–max range while the sample count is small.
- **Do not pool rounds that differ in model version or spec version.** Both are
  different tasks; group by both. If the provider exposes no version, record it
  manually per round.
- Total tokens ≈ turns × context size, so it is far noisier than output
  tokens. Prefer output tokens for 'effort' and total tokens for 'cost'.
