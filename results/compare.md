# Cross-round comparison

| round | task | spec | language | scored | output tok | total tok | cost $ | conforming |
|---|---|---|---|---|---|---|---|---|
| round-1 | workflow | v1 | python | 106/106 | 125,031 | 6,222,745 | 0.1999 | yes |
| round-1 | workflow | v1 | javascript | 106/106 | 90,736 | 2,959,548 | 0.1338 | yes |
| round-1 | workflow | v1 | go | 106/106 | 113,306 | 3,365,415 | 0.1624 | yes |
| round-1 | workflow | v1 | rust | 106/106 | 156,063 | 9,405,161 | 0.2549 | yes |
| round-1 | workflow | v1 | c | 106/106 | 121,998 | 5,224,057 | 0.1907 | yes |
| round-2 | workflow | v2 | python | 112/112 | 82,123 | 3,323,523 | 0.1278 | yes |
| round-2 | workflow | v2 | javascript | 112/112 | 119,392 | 4,913,756 | 0.1804 | yes |
| round-2 | workflow | v2 | go | 112/112 | 110,406 | 3,455,929 | 0.1630 | yes |
| round-2 | workflow | v2 | rust | 112/112 | 104,408 | 4,194,694 | 0.1579 | yes |
| round-2 | workflow | v2 | c | 112/112 | 118,885 | 6,107,267 | 0.1916 | yes |
| round-3 | workflow | v2 | python | 112/112 | 106,168 | 5,585,811 | 0.1709 | yes |
| round-3 | workflow | v2 | javascript | 112/112 | 67,714 | 1,351,236 | 0.0945 | yes |
| round-3 | workflow | v2 | go | 112/112 | 69,835 | 1,891,864 | 0.1012 | yes |
| round-3 | workflow | v2 | rust | 112/112 | 112,031 | 4,635,085 | 0.1718 | yes |
| round-3 | workflow | v2 | c | 112/112 | 142,225 | 6,068,702 | 0.2152 | yes |
| round-4 | workflow | v2 | python | 112/112 | 78,385 | 3,121,625 | 0.1212 | yes |
| round-4 | workflow | v2 | javascript | 112/112 | 157,602 | 9,721,332 | 0.2605 | yes |
| round-4 | workflow | v2 | go | 112/112 | 170,119 | 6,404,923 | 0.2514 | yes |
| round-4 | workflow | v2 | rust | 112/112 | 125,886 | 8,193,632 | 0.2108 | yes |
| round-4 | workflow | v2 | c | 112/112 | 117,365 | 6,633,406 | 0.1984 | yes |
| round-5 | workflow | v2 | python | 112/112 | 154,250 | 11,614,289 | 0.2661 | yes |
| round-5 | workflow | v2 | javascript | 112/112 | 93,405 | 2,785,938 | 0.1373 | yes |
| round-5 | workflow | v2 | go | 112/112 | 101,824 | 2,538,895 | 0.1432 | yes |
| round-5 | workflow | v2 | rust | 112/112 | 140,846 | 6,020,679 | 0.2184 | yes |
| round-5 | workflow | v2 | c | 112/112 | 121,115 | 6,223,051 | 0.2048 | yes |
| round-6 | workflow | v2 | python | 112/112 | 86,096 | 3,667,690 | 0.1347 | yes |
| round-6 | workflow | v2 | javascript | 112/112 | 96,639 | 4,513,010 | 0.1527 | yes |
| round-6 | workflow | v2 | go | 112/112 | 102,388 | 4,517,854 | 0.1588 | yes |
| round-6 | workflow | v2 | rust | 112/112 | 135,279 | 8,987,971 | 0.2302 | yes |
| round-6 | workflow | v2 | c | 112/112 | 108,581 | 3,997,438 | 0.1626 | yes |
| round-7 | workflow | v2 | python | 112/112 | 123,874 | 6,737,225 | 0.2003 | yes |
| round-7 | workflow | v2 | javascript | 112/112 | 87,820 | 3,007,457 | 0.1322 | yes |
| round-7 | workflow | v2 | go | 112/112 | 78,537 | 4,149,141 | 0.1277 | yes |
| round-7 | workflow | v2 | rust | 112/112 | 131,824 | 7,532,337 | 0.2146 | yes |
| round-7 | workflow | v2 | c | 112/112 | 154,778 | 12,365,110 | 0.2866 | yes |
| round-8-expr | expr | v1 | python | 56/56 | 73,983 | 2,524,500 | 0.1122 | yes |
| round-8-expr | expr | v1 | javascript | 56/56 | 60,022 | 1,241,698 | 0.0835 | yes |
| round-8-expr | expr | v1 | go | 56/56 | 60,130 | 1,408,634 | 0.0861 | yes |
| round-8-expr | expr | v1 | rust | 56/56 | 59,463 | 1,067,224 | 0.0802 | yes |
| round-8-expr | expr | v1 | c | 56/56 | 71,378 | 1,887,134 | 0.1026 | yes |
| round-9-patch | patch | v1 | python | 38/38 | 37,514 | 700,543 | 0.0539 | yes |
| round-9-patch | patch | v1 | javascript | 38/38 | 66,718 | 1,562,150 | 0.0945 | yes |
| round-9-patch | patch | v1 | go | 38/38 | 42,905 | 709,941 | 0.0590 | yes |
| round-9-patch | patch | v1 | rust | 38/38 | 44,765 | 925,327 | 0.0654 | yes |
| round-9-patch | patch | v1 | c | 38/38 | 77,186 | 2,355,515 | 0.1118 | yes |

## Output-token stability across rounds

Output tokens are the cleaner measure: they are not inflated by cache reads.

### expr · deepseek-v4.1-flash-1017 · DeepSeek-V4.1-Flash-1017 · spec v1

| language | rounds | output tokens | spread |
|---|---|---|---|
No repeated samples in this group.

### patch · deepseek-v4.1-flash-1017 · DeepSeek-V4.1-Flash-1017 · spec v1

| language | rounds | output tokens | spread |
|---|---|---|---|
No repeated samples in this group.

### workflow · deepseek-v4.1-flash-1017 · DeepSeek-V4.1-Flash-1017 · spec v1

| language | rounds | output tokens | spread |
|---|---|---|---|
No repeated samples in this group.

### workflow · deepseek-v4.1-flash-1017 · DeepSeek-V4.1-Flash-1017 · spec v2

| language | rounds | output tokens | spread |
|---|---|---|---|
| python | 6 | round-2:82,123, round-3:106,168, round-4:78,385, round-5:154,250, round-6:86,096, round-7:123,874 | 97% |
| javascript | 6 | round-2:119,392, round-3:67,714, round-4:157,602, round-5:93,405, round-6:96,639, round-7:87,820 | 133% |
| go | 6 | round-2:110,406, round-3:69,835, round-4:170,119, round-5:101,824, round-6:102,388, round-7:78,537 | 144% |
| rust | 6 | round-2:104,408, round-3:112,031, round-4:125,886, round-5:140,846, round-6:135,279, round-7:131,824 | 35% |
| c | 6 | round-2:118,885, round-3:142,225, round-4:117,365, round-5:121,115, round-6:108,581, round-7:154,778 | 43% |


## Reading this

- A single round is one sample. Rankings by **total** tokens move a lot
  between runs because session length (and therefore cache reads) varies.
- **Output** tokens are more stable and are the better signal for
  'how much did the model have to write'.
- Correctness is the gate: only conforming implementations are ranked.
