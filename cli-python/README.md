# workflow — Python implementation

Declarative workflow engine implementing [`../SPEC.md`](../SPEC.md). It reads one
JSON workflow document, executes the step DAG, and writes exactly one JSON
response document to stdout.

- Language: Python 3 (tested with 3.11; uses only 3.6+ standard-library features)
- Dependencies: **standard library only** — `json`, `math`, `re`, `sys`,
  `copy`, `functools`. No third-party packages, no build step, no lockfile.
- No network, no filesystem writes, no user code execution.

## Build

Interpreted language: there is no build step.

```bash
cd cli-python
python3 -m py_compile workflow.py   # optional syntax check
```

## Run

All of these forms are supported and produce the same result:

```bash
cd cli-python

# canonical adapter form
python3 workflow.py run myworkflow.json

# bare-file form
python3 workflow.py myworkflow.json

# JSON document on stdin
python3 workflow.py run - < myworkflow.json
cat myworkflow.json | python3 workflow.py -
```

stdout contains exactly one JSON document (with a trailing newline); stderr is
unused. Exit codes:

| Code | Meaning |
|---|---|
| 0 | workflow completed |
| 1 | runtime workflow failure |
| 2 | input / argument / structural validation error |
| 3 | internal program error |

Example:

```bash
$ python3 workflow.py run selftest/01_math_pipeline.json
{"status":"completed","results":{"nums":[1,2,3,4,5,6,7,8,9,10],"evens":[2,4,6,8,10],"total":30,"doubled":60,"final":59},"execution_order":["nums","evens","total","doubled","final"],"skipped":[],"error":null}
$ echo $?
0
```

## Self-test

The `selftest/` folder contains 10 hand-written workflows plus a runner that
invokes the real CLI (`run <file>`, bare `<file>`, and `run -` with stdin) and
compares the parsed response and exit code against expected values. The runner
also exercises every runtime/structural error code inline.

```bash
cd cli-python
python3 selftest/run_selftest.py
```

Expected final line:
`All self-tests passed (10 workflows, 11 error cases).`

Coverage of the self-tests:

| File | Covers |
|---|---|
| `01_math_pipeline.json` | `range`, `filter`, `sum`, `multiply`, `subtract`, refs |
| `02_string_pipeline.json` | `trim`, `uppercase`, `split`, `join`, `replace`, `length`, `lowercase` |
| `03_object_ops.json` | `set`, `merge`, `keys`, `values`, `pick`, `omit`, `get` |
| `04_skipped_step.json` | `when` false, skipped result `null`, dependent step running |
| `05_retry_success.json` | `fail_until` retried successfully |
| `06_retry_exhausted.json` | retries exhausted -> `STEP_FAILED`, exit 1 |
| `07_array_ops.json` | `sort`, `unique`, `reverse`, `slice`, `map`, `filter`, `reduce`, `flatten` |
| `08_references_nested.json` | dotted paths, array indices, transitive references |
| `09_arithmetic_overflow.json` | i64 overflow -> `ARITHMETIC_OVERFLOW`, partial results |
| `10_conditions_compound.json` | `all` / `any` / `not`, refs in conditions, skip |

## Implementation notes

- **Values/arithmetic**: JSON values are handled structurally; numeric operands
  are coerced to i64 (integral JSON numbers only, otherwise `INVALID_ARG`).
  `add/subtract/multiply` are overflow-checked; `divide` truncates toward zero;
  `mod` follows the dividend's sign; divide/mod by zero is `DIVISION_BY_ZERO`.
- **Ordering**: a single total order (`number < string < boolean < null <
  array < object`) drives `sort`, `compare`, filter comparisons and condition
  operators. String order is UTF-8 byte order, object keys are compared after
  sorting them byte-wise. `sort` uses Python's stable sort.
- **Equality**: structural, with numeric value equality (`1 == 1.0`), key-order
  insensitive objects, and type-sensitive comparisons (`true != 1`).
- **References**: `{"$ref": "step.path.0"}` is resolved recursively inside
  `args` and `when` operands. The target must be a direct/transitive
  `depends_on` of the referencing step. Missing targets/paths are
  `INVALID_REFERENCE`; references to skipped steps are `SKIPPED_REFERENCE`.
- **Validation**: the whole document is validated before any step runs
  (version, unknown fields, ids, unique ids, op names, `depends_on` existence,
  cycles, retry range, condition shape). Unknown operation argument keys and
  wrong argument types are detected when the operation actually runs, so a
  skipped step is never executed or argument-checked.
- **Execution**: deterministic Kahn-style scheduling; among ready steps the one
  earliest in the input `steps` array runs first. Skipped steps count as
  finished for dependents and appear in `execution_order` and `skipped` with a
  `null` result.
- **Retries/`fail_until`**: only `TRANSIENT_FAILURE` is retried, up to
  `retry.max_attempts` (1..5, default 1). Exhausted attempts yield
  `STEP_FAILED`. The per-step attempt counter lives in the execution context, so
  it resets for every process run.
- **Conditions**: `eq/ne/gt/gte/lt/lte`, `all`/`any`/`not`, with lazy
  short-circuiting. A bare `{"$ref": "id"}` is also accepted as a condition; its
  resolved value must be a strict boolean, otherwise `INVALID_CONDITION`.
- **Determinism**: no randomness, timestamps, locale-dependent behavior or
  wall-clock reads; outputs depend only on the input document.

## Files

```
cli-python/
├── README.md
├── workflow.py                 # the complete engine + CLI
└── selftest/
    ├── run_selftest.py         # self-test runner (stdlib only)
    ├── 01_math_pipeline.json
    ├── 02_string_pipeline.json
    ├── 03_object_ops.json
    ├── 04_skipped_step.json
    ├── 05_retry_success.json
    ├── 06_retry_exhausted.json
    ├── 07_array_ops.json
    ├── 08_references_nested.json
    ├── 09_arithmetic_overflow.json
    └── 10_conditions_compound.json
```
