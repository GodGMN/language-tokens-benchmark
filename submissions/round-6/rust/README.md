# `workflow` — declarative workflow engine (Rust)

Implementation of the workflow CLI described in `SPEC.md`.

## Build

```bash
cd submissions/round-6/rust
cargo build --release
```

The binary is written to `target/release/workflow`.

## Run

```bash
# from a file
target/release/workflow run myworkflow.json

# or read the JSON document from stdin
target/release/workflow run - < myworkflow.json
```

The CLI prints exactly one JSON response document on stdout. Diagnostics (if any)
go to stderr.

Exit codes:

| Code | Meaning |
|---|---|
| 0 | workflow completed (`status: "completed"`) |
| 1 | runtime workflow failure (`status: "failed"`) |
| 2 | input / argument / structural validation error |
| 3 | internal error / panic |

## Self-test

The `selftest/` directory contains ten hand-written workflows with expected
responses, including math, strings, objects, arrays, skipped steps, conditions,
references and retries.

```bash
# run the fixtures through the binary (reads each file)
python3 selftest/run_selftest.py

# same fixtures, but piped through `workflow run -` (stdin mode)
python3 selftest/run_selftest.py --stdin
```

The script exits 0 when all fixtures match. The expected file for a fixture
`NN_name.json` is `NN_name.expected.json`. Expected `error.message` is not
compared (messages are diagnostic only); `error.code` and `error.step` are.

You can also run individual fixtures directly:

```bash
target/release/workflow run selftest/01_math_pipeline.json
target/release/workflow run selftest/06_retry_failure.json
```

## Implementation notes

- JSON parsing/serialization: [`serde_json`](https://crates.io/crates/serde_json),
  pinned to `=1.0.151` with the `preserve_order` feature (which brings in
  `indexmap` transitively); the resolved dependency graph is locked in
  `Cargo.lock`. There are no other direct dependencies.
- All arithmetic uses checked `i64` operations. Division/modulo use Rust's
  truncating semantics (remainder takes the dividend's sign), with explicit
  `INT64_MIN / -1` and `INT64_MIN % -1` overflow handling.
- Structural equality treats `1` and `1.0` as equal; objects compare without key
  order. Values use the total order from SPEC §3 for `sort`/`compare`.
- Steps are validated structurally before execution, then run in deterministic
  topological order (earliest ready step in the `steps` array wins).
- `$ref` resolution is recursive inside args, checks the dependency closure,
  detects skipped targets, and distinguishes bad paths from skipped references.
- Only `TRANSIENT_FAILURE` from `fail_until` is retried; exhaustion reports
  `STEP_FAILED`. No user code is executed and there is no randomness,
  wall-clock dependence or filesystem side effect at runtime.

## Source layout

```
Cargo.toml        package manifest (serde_json pinned)
Cargo.lock        locked dependency graph
src/main.rs       CLI, validation, engine, operations
selftest/         10 workflows + expected responses + runner script
```
