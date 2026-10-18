# Aggregated results

Samples are grouped by **model group, task and spec version**; rounds that differ in any of them are not pooled.

## expr · deepseek-v4.1-flash-1017 · spec v1

Samples: 1 round(s) — round-8-expr  
Version(s): DeepSeek-V4.1-Flash-1017

> Fewer than 3 samples: treat these numbers as indicative only.

| language | samples | conforming | output tok (median [min–max]) | total tok (median [min–max]) | turns (median) |
|---|---|---|---|---|---|
| python | 1 | 1/1 | 73,983 [73,983–73,983] | 2,524,500 [2,524,500–2,524,500] | 38 |
| javascript | 1 | 1/1 | 60,022 [60,022–60,022] | 1,241,698 [1,241,698–1,241,698] | 22 |
| go | 1 | 1/1 | 60,130 [60,130–60,130] | 1,408,634 [1,408,634–1,408,634] | 30 |
| rust | 1 | 1/1 | 59,463 [59,463–59,463] | 1,067,224 [1,067,224–1,067,224] | 23 |
| c | 1 | 1/1 | 71,378 [71,378–71,378] | 1,887,134 [1,887,134–1,887,134] | 29 |

## patch · deepseek-v4.1-flash-1017 · spec v1

Samples: 1 round(s) — round-9-patch  
Version(s): DeepSeek-V4.1-Flash-1017

> Fewer than 3 samples: treat these numbers as indicative only.

| language | samples | conforming | output tok (median [min–max]) | total tok (median [min–max]) | turns (median) |
|---|---|---|---|---|---|
| python | 1 | 1/1 | 37,514 [37,514–37,514] | 700,543 [700,543–700,543] | 20 |
| javascript | 1 | 1/1 | 66,718 [66,718–66,718] | 1,562,150 [1,562,150–1,562,150] | 29 |
| go | 1 | 1/1 | 42,905 [42,905–42,905] | 709,941 [709,941–709,941] | 21 |
| rust | 1 | 1/1 | 44,765 [44,765–44,765] | 925,327 [925,327–925,327] | 28 |
| c | 1 | 1/1 | 77,186 [77,186–77,186] | 2,355,515 [2,355,515–2,355,515] | 41 |

## workflow · deepseek-v4.1-flash-1017 · spec v1

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

## workflow · deepseek-v4.1-flash-1017 · spec v2

Samples: 6 round(s) — round-2, round-3, round-4, round-5, round-6, round-7  
Version(s): DeepSeek-V4.1-Flash-1017

| language | samples | conforming | output tok (median [min–max]) | total tok (median [min–max]) | turns (median) |
|---|---|---|---|---|---|
| python | 6 | 6/6 | 96,132 [78,385–154,250] | 4,626,750 [3,121,625–11,614,289] | 56 |
| javascript | 6 | 6/6 | 95,022 [67,714–157,602] | 3,760,233 [1,351,236–9,721,332] | 47 |
| go | 6 | 6/6 | 102,106 [69,835–170,119] | 3,802,535 [1,891,864–6,404,923] | 47 |
| rust | 6 | 6/6 | 128,855 [104,408–140,846] | 6,776,508 [4,194,694–8,987,971] | 71 |
| c | 6 | 6/6 | 120,000 [108,581–154,778] | 6,165,159 [3,997,438–12,365,110] | 58 |

## Method notes

- Only **conforming** samples contribute to the token medians; a sample that
  fails the suite is recorded in the conformance column but never averaged in.
- Report the median with the min–max range while the sample count is small.
- **Do not pool rounds that differ in model version or spec version.** Both are
  different tasks; group by both. If the provider exposes no version, record it
  manually per round.
- Total tokens ≈ turns × context size, so it is far noisier than output
  tokens. Prefer output tokens for 'effort' and total tokens for 'cost'.
