# `workflow` — Declarative Workflow Engine (Python)

A complete implementation of the `workflow` CLI specified in [`SPEC.md`](../../../SPEC.md).

- Language: Python 3 (standard library only)
- Dependencies: **none** (no third-party packages, no build step)
- Single source file: `workflow.py`
- Self-tests: `selftest/` (10 workflow fixtures + runner)

## Build

None — interpreted language:

```bash
cd submissions/round-6/python
# (interpreted language — no build step)
```

## Run

```bash
cd submissions/round-6/python
python3 workflow.py run myworkflow.json     # workflow document from a file
python3 workflow.py run -                   # workflow document on stdin
```

- **stdout**: exactly one JSON response document (nothing else).
- **stderr**: diagnostics only (the implementation prints nothing).
- Exit codes: `0` completed, `1` runtime workflow failure, `2` validation/input
  error, `3` internal error.

Response shape (always all fields):

```json
{
  "status": "completed",
  "results": { "step": "value" },
  "execution_order": ["step"],
  "skipped": [],
  "error": null
}
```

## Self-test

```bash
cd submissions/round-6/python
python3 selftest/run_selftest.py
```

The runner executes every file in `selftest/cases/` through the CLI (via a file
path and, for one case, via stdin), compares the parsed JSON with the
hand-computed expectations in `selftest/expected/` (ignoring `error.message`,
which the SPEC says is not compared) and checks the exit code.

Covered fixtures:

| File | Covers |
|---|---|
| `01_math_pipeline.json` | `range`, `filter`, `sum`, `divide`, `mod` (negative operands), `max`, `multiply`, empty `sum` |
| `02_string_pipeline.json` | `trim`, `uppercase`, `split` (literal + empty separator), `join`, `replace`, `length` (code points), ASCII-only `lowercase` |
| `03_object_ops.json` | `keys`, `values`, `merge`, `pick` (missing keys → null), `omit`, `set` (non-mutating), `get` |
| `04_skipped_when.json` | `when` false → skipped step (`null` result, in `execution_order`/`skipped`), downstream step still runs |
| `05_retry.json` | `fail_until` + `retry.max_attempts` (2 transient failures, 3rd attempt succeeds) |
| `06_refs_nested.json` | Dotted `$ref` paths into objects and arrays, transitive references |
| `07_array_ops.json` | `sort` (stable, mixed types), `reverse`, `unique`, `slice` (negative indices), `flatten`, `map`, `filter`, `reduce` |
| `08_conditions_compound.json` | `all`/`any`/`not`, `$ref` operands, mixed-type comparison |
| `09_error_overflow.json` | `ARITHMETIC_OVERFLOW` → exit 1, partial results and execution order |
| `10_validation_bad_version.json` | `version: 1.0` → `INVALID_WORKFLOW` → exit 2 |

## Implementation notes

- **Numbers**: JSON integer literals are mapped to Python `int` and validated
  against the signed 64-bit range wherever a numeric parameter is required.
  Literals with a fraction or exponent are kept verbatim in a small `RawNum`
  wrapper so they can be echoed back; they are rejected with `INVALID_ARG` when
  a numeric parameter requires an i64. They still compare numerically for
  `equals`/`sort`/`compare` and report type `"number"`.
- **Arithmetic**: overflow-checked i64 `+ - * / %`; division truncates toward
  zero, `%` follows the dividend's sign, and `INT64_MIN / -1` /
  `INT64_MIN % -1` raise `ARITHMETIC_OVERFLOW`.
- **Validation**: the whole document is validated structurally before any step
  runs (`INVALID_WORKFLOW`, `UNKNOWN_DEPENDENCY`, `CYCLIC_DEPENDENCY`, exit 2).
- **Execution**: deterministic topological order; among ready steps the one
  earliest in `steps` wins. `when` is evaluated before `$ref` resolution.
  Only `TRANSIENT_FAILURE` (from `fail_until`) is retried; when attempts are
  exhausted the final code is `STEP_FAILED`.
- **References**: `$ref` with dotted object/array paths, resolved recursively
  inside `args` and condition operands; only direct/transitive dependencies are
  legal, and skipped targets raise `SKIPPED_REFERENCE`.
- **Ordering**: strings compare by UTF-8 bytes; `sort` uses the total order
  number < string < boolean < null < array < object and is stable.

## Files

```
workflow.py                 # the CLI / engine (stdlib only)
README.md                   # this file
selftest/run_selftest.py    # self-test runner
selftest/cases/*.json       # 10 hand-written workflow documents
selftest/expected/*.json    # hand-computed expected responses + exit codes
```
