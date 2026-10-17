# Declarative Workflow Engine — C implementation

A standalone CLI implementing the workflow contract in `SPEC.md` (round 5,
language: C).

```
workflow run <workflow.json>
workflow run -            # read the JSON document from stdin
```

- **stdout**: exactly one JSON document (the response).
- **stderr**: diagnostics only (the program normally prints nothing there).
- **Exit codes**: `0` completed, `1` runtime workflow failure,
  `2` validation/input error, `3` internal error.

## Build

```bash
cd submissions/round-5/c
make
```

Requirements: a C11 compiler (`cc`/`gcc`/`clang`) and GNU/POSIX `make`.
No network access and no third-party packages are needed.

Clean up with:

```bash
make clean
```

## Run

```bash
./workflow run myworkflow.json
```

or, reading the document from stdin:

```bash
./workflow run -
```

Example:

```bash
./workflow run selftest/a_pipeline.json
# {"status":"completed","results":{"n":[1,2,3,4,5],"evens":[2,4],"total":6},
#  "execution_order":["n","evens","total"],"skipped":[],"error":null}
```

## Self-test

Two layers of self-testing are provided.

1. **Hand-written example workflows** (in `selftest/`):

   | File | Covers |
   |---|---|
   | `a_pipeline.json`  | `range` → `filter` → `sum` (SPEC example A) |
   | `b_skip.json`      | skipped step via `when` (SPEC example B) |
   | `c_retry.json`     | `fail_until` + `retry` (SPEC example C) |
   | `d_math.json`      | chained math ops, `sum`/`min`/`max`, negative operands |
   | `e_strings.json`   | `trim`, `split`, `filter`, `join`, `uppercase`, `replace`, `length` |
   | `f_objects.json`   | `set`, `merge`, `pick`, `omit`, `keys`, `values`, `get` |
   | `g_refs.json`      | dotted/indexed `$ref`s, `map`, `sort`, `slice`, `unique` |
   | `h_conditions.json`| `all` / `any` / `not` conditions and skip propagation |

   Run them all:

   ```bash
   ./selftest/run_examples.sh
   ```

2. **Exhaustive assertion suite** (needs `python3`, development aid only):
   it runs ~170 cases through `./workflow run -` and compares the parsed
   response and the exit code, covering every operation, every error code,
   references/paths, conditions, retries and structural validation.

   ```bash
   make
   python3 selftest/run_tests.py
   # ... 168/168 passed
   ```

## Dependencies

- C standard library only (`libc`).
- **No external JSON library**: a small, strict JSON parser/serializer is
  written from scratch in `workflow.c` (single file, ~2.6k lines).
- No workflow/DAG/graph engine, no interpreter, no `eval`, no user code.

## Implementation notes

- `workflow.c` is a single translation unit. The `Makefile` builds the
  `workflow` binary from it.
- JSON numbers keep their original literal text so non-integer literals
  (e.g. `1.0`) round-trip unchanged in results, while operations that need
  integers require a true JSON integer literal that fits in `i64`
  (`1.0`, `1e3` → `INVALID_ARG`).
- All arithmetic uses signed 64-bit integers with `__builtin_*_overflow`
  checks; division truncates toward zero, `%` takes the dividend's sign,
  and `INT64_MIN / -1`, `INT64_MIN % -1` are `ARITHMETIC_OVERFLOW`.
- Strings are UTF-8; `length` counts code points, case conversion is
  ASCII-only, and ordering is byte-wise (`memcmp`).
- Values are immutable once built, so `$ref` resolution can share results
  and every object-producing op (`set`, `merge`, `pick`, `omit`, …)
  creates a fresh object.
- `unique` uses a structural hash table, so large inputs stay fast
  (100k elements dedup in ~0.1 s).
- The engine is deterministic: no randomness, no clocks, no environment
  lookups, no filesystem writes.
- Execution follows SPEC §7: full structural validation first, then a
  topological sweep that always picks the earliest ready step in the
  `steps` array; `when` is evaluated once per step; only
  `TRANSIENT_FAILURE` from `fail_until` is retried (`max_attempts` 1..5),
  and exhausting retries reports `STEP_FAILED`.
- Memory is intentionally never freed; the process is short-lived.

## File layout

```
submissions/round-5/c/
├── Makefile
├── README.md
├── workflow.c              # entire implementation
└── selftest/
    ├── run_examples.sh     # runs the hand-written workflows
    ├── run_tests.py        # assertion suite (dev aid)
    └── *.json              # hand-written example workflows
```
