# workflow — Python implementation

A declarative workflow engine CLI implementing `../../../SPEC.md` (round 3,
Python submission).

## Requirements / dependencies

* Python 3.8 or newer (tested with `python3` from the system).
* **Standard library only.** The implementation imports `json`, `re`, `sys`,
  `math` and `functools`. There are no third-party packages, no lockfile and no
  build step.

## Build

None — it is interpreted:

```bash
cd submissions/round-3/python
# no build step
```

## Run

```bash
cd submissions/round-3/python
python3 workflow.py run myworkflow.json     # read a file
python3 workflow.py run -                   # read the document from stdin
```

There is also an optional executable wrapper with the same behaviour:

```bash
./workflow run myworkflow.json
./workflow run -
```

The only output on stdout is exactly one JSON document. Diagnostics (if any)
would go to stderr. Exit codes:

| code | meaning |
|---|---|
| 0 | workflow completed |
| 1 | runtime workflow failure |
| 2 | validation / input / argument error |
| 3 | internal program error (best-effort JSON) |

## Self-test

Hand-written fixtures live in `selftest/cases/`; the runner compares the parsed
response (ignoring the free-form `message`) and the exit code, and also checks
the `run -` stdin path.

```bash
cd submissions/round-3/python
python3 selftest/run_selftest.py
```

Expected result: `23/23 checks passed` (13 fixture files plus stdin and
inline error-code checks).

Fixtures cover:

| file | covers |
|---|---|
| `01_math_pipeline.json` | `range`, `add`, `subtract`, `multiply`, `divide`, `mod`, `sum`, `min`, `max`, `reduce` |
| `02_strings.json` | `constant`, `uppercase`, `lowercase`, `trim`, `split` (empty separator too), `join`, `replace`, `length`, `concat` |
| `03_arrays.json` | `sort` (mixed types), `reverse`, `unique`, `map` (fixed and `type:"op"` forms), `filter`, `slice`, `flatten`, `stringify`, `type` transforms |
| `04_objects.json` | `keys`, `values`, `get`, `set`, `merge`, `pick`, `omit` |
| `05_utilities.json` | `type`, `equals`, `compare`, `and`, `or`, `not`, `constant` |
| `06_conditions.json` | `when` with `gt`/`eq`/`gte`/`all`/`any`/`not`, skipped steps |
| `07_retry_success.json` | `fail_until` + retry success (SPEC example C) |
| `08_retry_exhausted.json` | retries exhausted -> `STEP_FAILED`, later steps not run |
| `09_nested_refs.json` | dotted object paths, array indices, transitive dependencies, ref-looking literals |
| `10_dag_order.json` | deterministic topological tie-break (earliest in `steps`) |
| `11_errors.json` | `INVALID_REFERENCE` with partial results |
| `12_skipped_ref.json` | `SKIPPED_REFERENCE` |
| `13_invalid_arg.json` | `INVALID_ARG` (float numeric literal) |

The runner additionally exercises `run -` and inline error-code cases
(`ARITHMETIC_OVERFLOW`, `DIVISION_BY_ZERO`, `EMPTY_ARRAY`, non-retried errors,
`CYCLIC_DEPENDENCY`, `UNKNOWN_DEPENDENCY`, `INVALID_WORKFLOW`).

The SPEC §8 worked examples are `selftest/example_a.json`,
`selftest/example_b.json` and `selftest/example_c.json`; they can also be run
directly with `python3 workflow.py run selftest/example_a.json`.

## Implementation notes

* Numbers are Python ints checked against the signed 64-bit range at every
  arithmetic step; overflow is `ARITHMETIC_OVERFLOW`. Division truncates toward
  zero, modulo takes the dividend's sign, and `INT64_MIN / -1` /
  `INT64_MIN % -1` are overflow.
* JSON floats are parsed only as non-finite-checked `float`; any numeric
  *parameter* must be an integer literal in i64 range, otherwise `INVALID_ARG`.
* Structural equality is type-aware (`true` is not `1`, `1 == 1.0`), and the
  total order for `sort`/`compare` is number < string < boolean < null < array <
  object with UTF-8 byte ordering for strings and keys.
* Steps execute in deterministic topological order, picking the earliest
  ready step in the original `steps` array. `$ref` targets are limited to the
  step's transitive `depends_on` closure.
* Retries apply only to `TRANSIENT_FAILURE` (from `fail_until`); when attempts
  are exhausted the final response code is `STEP_FAILED`.
* No network, no writes, no user-supplied code: the operation set is closed.

## Files

```text
workflow.py                 implementation + CLI
workflow                    optional executable wrapper
README.md                   this file
selftest/run_selftest.py    self-test runner
selftest/cases/*.json       self-test fixtures
selftest/example_[abc].json SPEC §8 worked examples
```
