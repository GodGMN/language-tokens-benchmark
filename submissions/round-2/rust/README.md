# `workflow` — declarative workflow engine (Rust)

A complete implementation of the declarative workflow engine described in
`../../../SPEC.md`. The binary is called `workflow` and is built with Cargo.

## Build

From this directory (`submissions/round-2/rust`):

```bash
cargo build --release
```

This produces `target/release/workflow`. All dependencies are pinned in
`Cargo.lock`; the crates are also vendored under `vendor/` and wired up through
`.cargo/config.toml`, so the build works without network access.

## Run

```bash
target/release/workflow run myworkflow.json
target/release/workflow run -          # read the JSON document from stdin
```

* stdout contains exactly one JSON response document.
* stderr may contain diagnostics (not used by the conformance suite).
* Exit codes: `0` completed, `1` runtime workflow failure, `2` validation/input
  error, `3` internal error.

## Self-test

```bash
./selftest/run.sh
```

`selftest/run.sh` builds the release binary and runs
`selftest/check.py`, which feeds every workflow in `selftest/cases/` through the
CLI (including one `run -` stdin case) and compares the parsed JSON response
with `selftest/expected/`. The free-form `error.message` field is ignored, as
the spec says it is not compared.

You can also run individual cases directly:

```bash
target/release/workflow run selftest/cases/01_math_pipeline.json
```

The self-test cases cover:

| File | Covers |
|---|---|
| `01_math_pipeline.json` | `range`, `filter`, `sum`, `map`, `min`, `max` |
| `02_string_pipeline.json` | `trim`, `uppercase`, `split`, `join`, `replace`, `length` |
| `03_object_ops.json` | `keys`, `values`, `get`, `set`, `merge`, `pick`, `omit` |
| `04_skipped_step.json` | `when` with `all`/`any`/`not`, skipped results, dependents of skipped steps |
| `05_retry.json` | `fail_until` with `retry.max_attempts` |
| `06_references.json` | dotted `$ref` paths and array indices |
| `07_arrays.json` | `sort`, `unique`, `reverse`, `flatten`, `slice`, `reduce` |
| `08_error_partial.json` | runtime failure with partial results, skipped prefix, exit code 1 |
| `09_utilities.json` | `equals`, `compare`, `and`, `or`, `not`, `type`, compound `when` |
| `10_transforms.json` | all `map.transform` and `filter.predicate` forms |

Examples A, B and C from SPEC §8 are covered by
`01_math_pipeline.json`, `04_skipped_step.json` and `05_retry.json`
respectively.

## Dependencies

* Rust standard library (edition 2021).
* [`serde_json`](https://crates.io/crates/serde_json) `1.0.151`, pinned exactly
  in `Cargo.toml` and `Cargo.lock`, used only for JSON parsing/serialization.
  Its transitive dependencies are pinned by the lockfile and vendored.

No workflow engine, DAG/graph library, or code-interpreter/eval library is
used. The operation set is closed and hard-coded; user input is never executed.

## Implementation notes

* All numbers used by operations are signed 64-bit integers. A numeric
  parameter must be an integer literal that fits in `i64`; `1.0` / `1e3` are
  rejected with `INVALID_ARG`.
* `+ - * / %` use checked `i64` arithmetic. `INT64_MIN / -1` and
  `INT64_MIN % -1` produce `ARITHMETIC_OVERFLOW`; division truncates toward
  zero and modulo takes the dividend's sign.
* Structural equality and the total ordering used by `sort`/`compare` follow
  SPEC §3, including mixed-type ordering (`number < string < boolean < null <
  array < object`) and byte-wise string ordering.
* Validation happens before any step runs; execution uses the deterministic
  "earliest ready step in the source array" topological tie-break.
* References resolve recursively inside `args` and conditions. Only direct or
  transitive `depends_on` targets are legal; references to skipped steps yield
  `SKIPPED_REFERENCE`.
* `TRANSIENT_FAILURE` from `fail_until` is the only retriable failure; when
  attempts are exhausted the final code is `STEP_FAILED`. All other operation
  errors are returned immediately.
* Unknown argument names and wrong argument types are runtime `INVALID_ARG`
  errors (exit 1), while malformed step/workflow fields are structural
  `INVALID_WORKFLOW` errors (exit 2), as specified.
* The engine is deterministic: no randomness, timestamps, network or
  filesystem side effects beyond reading the input document/stdin.
