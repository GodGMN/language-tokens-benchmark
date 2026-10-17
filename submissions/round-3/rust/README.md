# `workflow` — declarative workflow engine (Rust)

A complete implementation of the workflow engine described in `../../../SPEC.md`.

## Build

```bash
cd submissions/round-3/rust
cargo build --release
```

The binary is written to `target/release/workflow`.

## Run

```bash
target/release/workflow run myworkflow.json
target/release/workflow run -              # read the workflow document from stdin
```

- stdout contains exactly one JSON response document (plus a trailing newline).
- stderr is unused for normal operation.
- Exit codes: `0` completed, `1` runtime workflow failure, `2` validation/input
  error, `3` internal error.

## Dependencies

- `serde_json = 1.0.151` (pinned, with the `arbitrary_precision` feature so
  `-0` can be recognised as an integer literal while `1.0` / `1e3` are
  rejected as numeric parameters).
- `serde_json`'s own transitive dependencies.

All crates are **vendored** under `vendor/`, and `.cargo/config.toml` points
Cargo at them, so `cargo build --release` works fully offline. `Cargo.lock` is
committed. No workflow/DAG/graph/eval libraries are used; the engine, ordering,
reference resolution, conditions and all operations are implemented in `src/`.

## Self-test

Two layers:

```bash
# 1. Hand-written workflows from the SPEC examples and each operation family.
#    Prints each response and its exit code.
./selftest/run_examples.sh

# 2. Assertion harness (124 cases covering every operation, references,
#    conditions, retries, validation and error codes).
python3 selftest/run_tests.py
```

`run_tests.py` defaults to `target/release/workflow`; pass a path as the first
argument to test another build. The hand-written files in `selftest/` are:

| file | covers |
|---|---|
| `01_pipeline.json` | SPEC example A (range -> filter -> sum) |
| `02_skipped.json` | SPEC example B (`when` false) |
| `03_retry.json` | SPEC example C (`fail_until` + retry) |
| `04_string_pipeline.json` | trim/uppercase/split/join/replace/length |
| `05_object_ops.json` | set/keys/values/pick/merge/omit |
| `06_condition_nested.json` | `all` / `any` / `not` compound conditions |
| `07_retry_exhausted.json` | retries exhausted -> `STEP_FAILED`, exit 1 |
| `08_refs_nested.json` | nested object/array `$ref` paths |
| `09_math_pipeline.json` | range/map/filter/reduce |
| `10_error_cycle.json` | `CYCLIC_DEPENDENCY`, exit 2 |

## Implementation notes

- `src/validate.rs` performs all structural validation before execution
  (version, step shapes, ids, unknown fields, `depends_on`, cycles, `retry`,
  `when` shape) and computes a deterministic topological order plus transitive
  dependency sets.
- `src/engine.rs` executes ready steps in document order, evaluates `when`,
  applies retries, and accumulates partial results/order on failure.
- `src/refs.rs` resolves recursive `$ref` values and dotted paths with
  non-negative array indices, enforcing dependency, self-reference and
  skipped-step rules.
- `src/ops.rs` implements the closed operation set, `map` transforms and
  `filter` predicates with i64 checked arithmetic.
- `src/value.rs` implements structural equality, JSON truthiness and the global
  total order used by `sort`/`compare`/conditions.
- Numbers are checked i64 arithmetic; overflow, division by zero and
  `INT64_MIN / -1` / `INT64_MIN % -1` map to the specified error codes.
  Division truncates toward zero and `%` takes the dividend's sign.
- No randomness, clock, network or filesystem side effects: identical input
  always produces identical output.
