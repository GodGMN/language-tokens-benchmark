# Aggregated results

## deepseek-v4.1-flash-1017

Samples: 2 round(s) — round-1, round-2  
Version(s): DeepSeek-V4.1-Flash-1017

> Fewer than 3 samples: treat these numbers as indicative only.

| language | samples | conforming | output tok (median [min–max]) | total tok (median [min–max]) | turns (median) |
|---|---|---|---|---|---|
| python | 2 | 1/2 | 125,031 [125,031–125,031] | 6,222,745 [6,222,745–6,222,745] | 57 |
| javascript | 2 | 2/2 | 105,064 [90,736–119,392] | 3,936,652 [2,959,548–4,913,756] | 46 |
| go | 2 | 2/2 | 111,856 [110,406–113,306] | 3,410,672 [3,365,415–3,455,929] | 38 |
| rust | 2 | 2/2 | 130,235 [104,408–156,063] | 6,799,927 [4,194,694–9,405,161] | 69 |
| c | 2 | 2/2 | 120,441 [118,885–121,998] | 5,665,662 [5,224,057–6,107,267] | 57 |

## Method notes

- Only **conforming** samples contribute to the token medians; a sample that
  fails the suite is recorded in the conformance column but never averaged in.
- Report the median with the min–max range while the sample count is small.
- **Do not pool rounds from different model versions.** Group them by the
  exact version; if the provider does not expose one, record it manually.
- Total tokens ≈ turns × context size, so it is far noisier than output
  tokens. Prefer output tokens for 'effort' and total tokens for 'cost'.
