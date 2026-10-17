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
| round-3 | v2 | python | 114/114 | 106,168 | 5,585,811 | 0.1709 | yes |
| round-3 | v2 | javascript | 112/114 | 67,714 | 1,351,236 | 0.0945 | no |
| round-3 | v2 | go | 114/114 | 69,835 | 1,891,864 | 0.1012 | yes |
| round-3 | v2 | rust | 114/114 | 112,031 | 4,635,085 | 0.1718 | yes |
| round-3 | v2 | c | 114/114 | 142,225 | 6,068,702 | 0.2152 | yes |
| round-4 | v2 | python | 114/114 | 78,385 | 3,121,625 | 0.1212 | yes |
| round-4 | v2 | javascript | 114/114 | 157,602 | 9,721,332 | 0.2605 | yes |
| round-4 | v2 | go | 112/114 | 170,119 | 6,404,923 | 0.2514 | no |
| round-4 | v2 | rust | 114/114 | 125,886 | 8,193,632 | 0.2108 | yes |
| round-4 | v2 | c | 113/114 | 117,365 | 6,633,406 | 0.1984 | no |
| round-5 | v2 | python | 114/114 | 154,250 | 11,614,289 | 0.2661 | yes |
| round-5 | v2 | javascript | 114/114 | 93,405 | 2,785,938 | 0.1373 | yes |
| round-5 | v2 | go | 114/114 | 101,824 | 2,538,895 | 0.1432 | yes |
| round-5 | v2 | rust | 112/114 | 140,846 | 6,020,679 | 0.2184 | no |
| round-5 | v2 | c | 112/114 | 121,115 | 6,223,051 | 0.2048 | no |
| round-6 | v2 | python | 114/114 | 86,096 | 3,667,690 | 0.1347 | yes |
| round-6 | v2 | javascript | 114/114 | 96,639 | 4,513,010 | 0.1527 | yes |
| round-6 | v2 | go | 114/114 | 102,388 | 4,517,854 | 0.1588 | yes |
| round-6 | v2 | rust | 114/114 | 135,279 | 8,987,971 | 0.2302 | yes |
| round-6 | v2 | c | 114/114 | 108,581 | 3,997,438 | 0.1626 | yes |
| round-7 | v2 | python | 114/114 | 123,874 | 6,737,225 | 0.2003 | yes |
| round-7 | v2 | javascript | 114/114 | 87,820 | 3,007,457 | 0.1322 | yes |
| round-7 | v2 | go | 113/114 | 78,537 | 4,149,141 | 0.1277 | no |
| round-7 | v2 | rust | 114/114 | 131,824 | 7,532,337 | 0.2146 | yes |
| round-7 | v2 | c | 113/114 | 154,778 | 12,365,110 | 0.2866 | no |

## Output-token stability across rounds

Output tokens are the cleaner measure: they are not inflated by cache reads.

| language | rounds | output tokens | spread |
|---|---|---|---|
| python | 6 | round-1:125,031, round-3:106,168, round-4:78,385, round-5:154,250, round-6:86,096, round-7:123,874 | 97% |
| javascript | 6 | round-1:90,736, round-2:119,392, round-4:157,602, round-5:93,405, round-6:96,639, round-7:87,820 | 79% |
| go | 5 | round-1:113,306, round-2:110,406, round-3:69,835, round-5:101,824, round-6:102,388 | 62% |
| rust | 6 | round-1:156,063, round-2:104,408, round-3:112,031, round-4:125,886, round-6:135,279, round-7:131,824 | 49% |
| c | 4 | round-1:121,998, round-2:118,885, round-3:142,225, round-6:108,581 | 31% |

## Reading this

- A single round is one sample. Rankings by **total** tokens move a lot
  between runs because session length (and therefore cache reads) varies.
- **Output** tokens are more stable and are the better signal for
  'how much did the model have to write'.
- Correctness is the gate: only conforming implementations are ranked.
