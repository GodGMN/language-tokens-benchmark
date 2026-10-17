# Cross-round comparison

| round | spec | language | scored | output tok | total tok | cost $ | conforming |
|---|---|---|---|---|---|---|---|
| round-1 | v1 | python | 107/107 | 125,031 | 6,222,745 | 0.1999 | yes |
| round-1 | v1 | javascript | 107/107 | 90,736 | 2,959,548 | 0.1338 | yes |
| round-1 | v1 | go | 107/107 | 113,306 | 3,365,415 | 0.1624 | yes |
| round-1 | v1 | rust | 107/107 | 156,063 | 9,405,161 | 0.2549 | yes |
| round-1 | v1 | c | 107/107 | 121,998 | 5,224,057 | 0.1907 | yes |
| round-2 | v2 | python | 112/114 | 82,123 | 3,323,523 | 0.1278 | no |
| round-2 | v2 | javascript | 114/114 | 119,392 | 4,913,756 | 0.1804 | yes |
| round-2 | v2 | go | 114/114 | 110,406 | 3,455,929 | 0.1630 | yes |
| round-2 | v2 | rust | 114/114 | 104,408 | 4,194,694 | 0.1579 | yes |
| round-2 | v2 | c | 114/114 | 118,885 | 6,107,267 | 0.1916 | yes |

## Output-token stability across rounds

Output tokens are the cleaner measure: they are not inflated by cache reads.

| language | rounds | output tokens | spread |
|---|---|---|---|
| javascript | 2 | round-1:90,736, round-2:119,392 | 32% |
| go | 2 | round-1:113,306, round-2:110,406 | 3% |
| rust | 2 | round-1:156,063, round-2:104,408 | 49% |
| c | 2 | round-1:121,998, round-2:118,885 | 3% |

## Reading this

- A single round is one sample. Rankings by **total** tokens move a lot
  between runs because session length (and therefore cache reads) varies.
- **Output** tokens are more stable and are the better signal for
  'how much did the model have to write'.
- Correctness is the gate: only conforming implementations are ranked.
