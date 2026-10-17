# `workflow` — declarative workflow engine (C)

A standalone C11 implementation of the declarative workflow engine described in
[`SPEC.md`](../../../SPEC.md). It reads a workflow JSON document, validates it
structurally, executes its steps in a deterministic topological order, resolves
`$ref` references, evaluates `when` conditions and applies retries.

## Build

From this directory (`submissions/round-7/c`):

```bash
make
```

This produces `./workflow`. There are no third-party dependencies; the JSON
parser and serializer are implemented in `workflow.c`. Only the C standard
library and `libm` are linked (`-lm`).

Requirements: a C11 compiler (tested with `cc`/GCC/Clang) and `make`.

## Run

```bash
./workflow run myworkflow.json     # read the workflow from a file
./workflow run -                   # read the workflow from stdin
```

- **stdout**: exactly one JSON response document, nothing else.
- **stderr**: unused (diagnostics only; nothing is printed there normally).
- **Exit codes**:
  - `0` — completed
  - `1` — runtime workflow failure (`INVALID_ARG`, `INVALID_REFERENCE`,
    `SKIPPED_REFERENCE`, `INVALID_CONDITION`, `ARITHMETIC_OVERFLOW`,
    `DIVISION_BY_ZERO`, `EMPTY_ARRAY`, `STEP_FAILED`)
  - `2` — validation/input error (`INVALID_WORKFLOW`, `UNKNOWN_DEPENDENCY`,
    `CYCLIC_DEPENDENCY`, malformed JSON, bad CLI usage, unreadable input)
  - `3` — internal error (best-effort JSON response)

Example:

```bash
$ ./workflow run selftest/example_a.json
{"status":"completed","results":{"n":[1,2,3,4,5],"evens":[2,4],"total":6},"execution_order":["n","evens","total"],"skipped":[],"error":null}
```

## Self-test

```bash
make test            # equivalent to: sh selftest/run.sh
```

The self-test runs the hand-written workflows in `selftest/` and checks the exit
code, the presence of expected result/error fields, and that stdout is a single
valid JSON document (when `python3` is available for the JSON parse check; the
engine itself has no Python dependency).

`selftest/` covers:

| File | Purpose |
|---|---|
| `example_a.json`, `example_b.json`, `example_c.json` | SPEC §8 worked examples |
| `01_math_pipeline.json` | `range`, `sum`, `min`, `max`, arithmetic |
| `02_string_pipeline.json` | `split`, `join`, `uppercase`, `replace`, `length`, `trim`, `lowercase`, `concat` |
| `03_object_ops.json` | `get`, `keys`, `values`, `set`, `merge`, `pick`, `omit` |
| `04_skipped_and_conditions.json` | `when`, `all`, `any`, `not`, skipped results |
| `05_retry_success.json`, `05b_retry_exhausted.json` | `fail_until` + `retry.max_attempts`, `STEP_FAILED` |
| `06_arrays.json` | `map`, `filter`, `reduce`, `sort`, `reverse`, `unique`, `slice`, `flatten` |
| `07_references.json` | dotted paths, array indices, transitive dependencies |
| `08*.json` | runtime error codes |
| `09*.json` | structural validation error codes |
| `10_utilities.json` | `equals`, `compare`, `and`, `or`, `not`, `type` |

## Implementation notes

- **Numbers**: parsed numbers track whether they came from an integer literal
  that fits in `i64`. Numeric op parameters accept only such values; literals
  with a fraction or exponent (`1.0`, `1e3`) are rejected with `INVALID_ARG`.
  All arithmetic is checked signed 64-bit arithmetic; overflow is
  `ARITHMETIC_OVERFLOW`. Division truncates toward zero and `mod` takes the
  dividend's sign (C99 semantics).
- **JSON**: a small recursive-descent parser/serializer bundled in `workflow.c`
  preserves the integer-literal distinction, validates UTF-8, and uses UTF-8
  byte ordering for keys and strings. Integer literals outside the `i64` range
  are preserved exactly when passed through (for example by `constant`), while
  still being rejected with `INVALID_ARG` when used as a numeric parameter.
- **References**: `{"$ref": "step.path.0"}` objects are recognized only when the
  object has exactly one key, `$ref`, with a string value. Resolution is
  recursive through arrays/objects, restricted to the referencing step's
  transitive dependencies, and distinguishes skipped steps from legitimate
  `null` results.
- **Execution**: structural validation happens before any step runs. Runtime
  scheduling repeatedly picks the earliest declared step whose dependencies
  have finished; skipped steps count as finished and appear in
  `execution_order`/`skipped` with a `null` result.
- **Retries**: only `TRANSIENT_FAILURE` (produced by `fail_until`) is retried;
  when attempts are exhausted the reported code is `STEP_FAILED`.

## Layout

```
submissions/round-7/c/
├── Makefile
├── README.md
├── workflow.c
└── selftest/
    ├── run.sh
    └── *.json
```
