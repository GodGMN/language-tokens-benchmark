# Go implementation — declarative `workflow` CLI

A from-scratch implementation of the workflow engine described in [`SPEC.md`](../../../SPEC.md).
It uses **only the Go standard library** (no third-party modules, no workflow/DAG/eval libraries).

## Build

```bash
cd submissions/round-4/go
go build -o workflow .
```

`go.mod` declares the module with `go 1.21` and has no `require` entries.

## Run

```bash
./workflow run myworkflow.json
./workflow run -          # read the JSON document from stdin
```

- stdout: exactly one JSON response document (plus a trailing newline).
- stderr: diagnostics only.
- exit codes: `0` completed, `1` runtime workflow failure, `2` validation/input error,
  `3` internal error.

Examples from SPEC §8:

```bash
./workflow run selftest/cases/01_math_pipeline.json
./workflow run selftest/cases/04_skipped.json
./workflow run selftest/cases/05_retry.json
```

## Self-test

Ten hand-written workflows live in `selftest/cases/`:

| file | covers |
|---|---|
| `01_math_pipeline.json` | `range`, `filter`, `map`, `sum`, refs, dependencies |
| `02_string_pipeline.json` | `constant`, `trim`, `split`, `join`, `uppercase`, `lowercase`, `length` |
| `03_objects.json` | `constant`, `keys`, `values`, `merge`, `pick`, `set`, `omit` |
| `04_skipped.json` | false `when`, skipped result `null`, dependent execution |
| `05_retry.json` | `fail_until` with `retry.max_attempts` |
| `06_conditions.json` | `all`, `any`, `not`, comparison conditions, refs |
| `07_arrays.json` | `sort`, `unique`, `reverse`, `slice`, `flatten`, `reduce`, `map`, `filter`, `stringify` |
| `08_error_overflow.json` | `ARITHMETIC_OVERFLOW`, exit `1`, partial results |
| `09_validation_cycle.json` | `CYCLIC_DEPENDENCY`, exit `2` |
| `10_utilities.json` | dotted refs, `equals`, `compare`, `and`, `or`, `not`, `type` |

Run them and compare against hand-derived expected responses:

```bash
cd submissions/round-4/go
go build -o workflow .
python3 selftest/check.py
```

To simply eyeball the raw JSON for one case:

```bash
./workflow run selftest/cases/07_arrays.json
```

## Notes on semantics

- JSON integers that fit in `i64` are decoded as `int64`; other numeric literals are preserved
  verbatim (so `1.0` in a value position is not silently rewritten). Numeric *parameters*
  require an integer literal and return `INVALID_ARG` for fractional/exponent literals.
- All arithmetic is checked `int64` arithmetic. Division truncates toward zero and `mod` follows
  the dividend's sign. `INT64_MIN / -1` and `INT64_MIN % -1` report `ARITHMETIC_OVERFLOW`.
- Structural validation (`INVALID_WORKFLOW`, `UNKNOWN_DEPENDENCY`, `CYCLIC_DEPENDENCY`) happens
  before any step runs. A malformed top-level `when` object/missing `op` is structural; deeper
  condition problems are reported at runtime as `INVALID_CONDITION`.
- Steps execute in deterministic topological order; among ready steps the one earliest in the
  `steps` array wins. Retries only apply to `TRANSIENT_FAILURE` from `fail_until`; exhaustion is
  reported as `STEP_FAILED`.
- `stringify` emits compact JSON with object keys sorted, so the result is deterministic across
  languages. The `type_is` filter predicate accepts its target type under `value` (also
  `type_name` / `target`), since the SPEC's inline example uses the discriminator key for both
  roles.

## Dependency list

- Go standard library only: `encoding/json`, `math/big`, `sort`, `strings`, `strconv`,
  `regexp`, `unicode/utf8`, `fmt`, `io`, `os`, `bytes`, `errors`.
- External direct dependencies: **none**.
