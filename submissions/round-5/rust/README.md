# `workflow` — declarative workflow engine (Rust)

A complete implementation of the declarative workflow engine described in
[`SPEC.md`](../../../SPEC.md). It is a single-binary CLI that reads one JSON
workflow document, executes it deterministically, and writes exactly one JSON
response document to stdout.

## Build

From this directory (`submissions/round-5/rust`):

```bash
cargo build --release
```

The build is fully offline: all dependencies are vendored under `vendor/` and
`.cargo/config.toml` redirects crates.io to that directory. `Cargo.lock` pins
every version.

## Run

```bash
target/release/workflow run myworkflow.json
target/release/workflow run -          # read the JSON document from stdin
```

- stdout: exactly one JSON response document (nothing else).
- stderr: diagnostics only (normally empty).
- Exit codes:
  - `0` — workflow completed (`status: "completed"`)
  - `1` — workflow failed at runtime (`status: "failed"`)
  - `2` — input / argument / structural validation error
  - `3` — internal program error (best-effort JSON response)

Example:

```bash
printf '%s' '{"version":1,"steps":[{"id":"a","op":"add","args":{"a":2,"b":3}}]}' \
  | target/release/workflow run -
# {"error":null,"execution_order":["a"],"results":{"a":5},"skipped":[],"status":"completed"}
```

## Self-test

Two complementary checks live in `selftest/`:

1. **Hand-written workflow examples** (`selftest/*.json`) covering a math
   pipeline, a string pipeline, object operations, array operations, a skipped
   step (`when`), a retry (`fail_until`), nested references, compound
   conditions and an arithmetic-overflow failure. Run them all and inspect the
   JSON / exit codes:

   ```bash
   sh selftest/run_examples.sh
   # or a single one:
   target/release/workflow run selftest/05_retry.json
   ```

2. **Exact-output conformance harness** (`selftest/run_selftest.py`). It
   contains 80 cases with the full expected response (status, results,
   execution order, skipped list, error code and error step) for every
   operation family plus validation, references, conditions, retries and error
   paths:

   ```bash
   python3 selftest/run_selftest.py
   # optionally pass a binary path:
   python3 selftest/run_selftest.py ./target/release/workflow
   ```

   Expected final line: `80/80 passed`.

The three worked examples from SPEC §8 are included as
`selftest/a_pipeline.json`, `selftest/b_skipped.json` and
`selftest/c_retry.json`.

## Dependencies

Runtime dependencies are only the Rust standard library plus one JSON library:

| Crate | Version | Purpose |
|---|---|---|
| `serde_json` | 1.0.151 (pinned exact) | JSON parsing and serialization |

Transitively: `serde` / `serde_core` 1.0.229, `itoa` 1.0.18, `memchr` 2.8.3,
`zmij` 1.0.23 (and unused-by-feature build-time crates present in the lock
file). All of them are vendored in `vendor/`; no network access is required to
build or run.

No workflow engine, graph/DAG library, `eval`, or user-code execution is used.
The execution engine, topological ordering, reference resolution, conditions
and retries are implemented directly in `src/main.rs`.

## Implementation notes

- **Numbers** are i64. Only JSON integer literals are accepted where the spec
  requires an integer; `1.0` / `1e3` are rejected with `INVALID_ARG`. Arithmetic
  uses checked operations, so overflow yields `ARITHMETIC_OVERFLOW`.
  `divide` truncates toward zero and `mod` takes the dividend's sign.
- **Values/ordering** implement the spec's structural equality (so `1 == 1.0`)
  and total order (`number < string < boolean < null < array < object`), with
  stable sorting.
- **Execution** validates the whole document first, then runs a deterministic
  topological order (the earliest ready step in the `steps` array wins). A
  step's `when` is evaluated before resolving its argument references; skipped
  steps record `null`, appear in `execution_order` and in `skipped`.
- **References** (`{"$ref": "step.path.0"}`) resolve recursively through
  arguments, are limited to direct/transitive `depends_on` targets, and
  distinguish a skipped target (`SKIPPED_REFERENCE`) from a bad path
  (`INVALID_REFERENCE`).
- **Retries** only apply to `TRANSIENT_FAILURE` from `fail_until`; exhausted
  attempts report `STEP_FAILED`. The `fail_until` counter is per step and per
  workflow run.
- **Output determinism**: no randomness, timestamps, network or filesystem
  writes. Object keys in generated values are kept in byte-sorted order, which
  keeps `keys`, `values`, `pick` and `sort` deterministic.
