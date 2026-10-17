# `workflow` — declarative workflow engine (Rust)

A complete implementation of the declarative workflow engine described in
`SPEC.md`. The CLI reads a workflow JSON document, validates it, executes the
declarative steps (references, conditions, retries, all SPEC §4 operations),
and writes exactly one JSON response document to stdout.

## Build

From this directory (`submissions/round-7/rust`):

```bash
cargo build --release
```

The binary is written to `target/release/workflow`.

## Run

```bash
target/release/workflow run path/to/workflow.json
target/release/workflow run -            # read the document from stdin
```

Examples:

```bash
target/release/workflow run selftest/math_pipeline.json
cat selftest/retry.json | target/release/workflow run -
```

stdout contains only the response JSON document. Diagnostics go to stderr.

Exit codes:

| Code | Meaning |
|---|---|
| 0 | workflow completed (`status: "completed"`) |
| 1 | runtime workflow failure (`status: "failed"`) |
| 2 | input / argument / structural validation failure |
| 3 | internal error (best-effort JSON on stdout) |

## Self-test

The `selftest/` folder contains hand-written workflows covering a math
pipeline, string pipeline, object operations, array operations, utilities,
nested references, skipped steps (`when`), compound conditions, a successful
retry, and an intentionally exhausted retry.

```bash
cargo build --release
bash selftest/run.sh
```

`run.sh` runs every fixture through the binary and prints the JSON plus the
exit code. `retry_exhausted.json` intentionally exits with code 1; all other
fixtures complete successfully.

## Dependencies

- `serde_json` **1.0.151** (pinned in `Cargo.toml` and `Cargo.lock`).
  Used only for JSON parsing/serialization.
- No workflow, graph, DAG, evaluation, or code-interpreter libraries.
- All workflow semantics (validation, topological scheduling, references,
  conditions, arithmetic, operations, retries) are implemented in
  `src/main.rs`.

Dependencies are vendored in `vendor/` and `.cargo/config.toml` points Cargo
at the vendored sources, so `cargo build --release` also works without network
access.

## Notes on semantics

- Numbers in math parameters/results are checked `i64` values; overflow,
  division by zero and `INT64_MIN / -1` / `% -1` are reported per SPEC §7.
- Structural validation happens before any step executes.
- Ready steps are executed in `steps` array order (deterministic tie-break).
- References only resolve to direct/transitive dependencies; skipped targets
  produce `SKIPPED_REFERENCE`.
- Only `TRANSIENT_FAILURE` from `fail_until` is retried; exhausted retries
  report `STEP_FAILED`.
- No randomness, timestamps, network access, or filesystem writes (other than
  reading the input document/stdin) are used.
