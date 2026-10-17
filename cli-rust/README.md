# workflow — Rust

A complete implementation of the declarative workflow engine from `../SPEC.md`.

## Requirements

- Rust (edition 2021; tested with Rust 1.100).
- No system libraries beyond the Rust standard library.

## Build

```bash
cd cli-rust
cargo build --release
```

The binary is `target/release/workflow`.

## Run

```bash
target/release/workflow run myworkflow.json
target/release/workflow run -          # read the JSON document from stdin
```

- **stdout** is exactly one JSON document.
- **stderr** is used only for diagnostics.
- Exit codes: `0` completed, `1` workflow runtime failure, `2` input/validation
  error, `3` internal error.

Example:

```bash
echo '{"version":1,"steps":[{"id":"n","op":"range","args":{"start":1,"end":5}}]}' \
  | target/release/workflow run -
```

## Self-test

The `selftest/` directory contains ten hand-written workflows plus expected
responses, covering math, strings, arrays, objects, references, conditions,
skipped steps, retries, retry exhaustion, validation errors and division by
zero.

After building, run:

```bash
cd cli-rust
python3 selftest/run.py
```

The runner executes every `selftest/*.json` fixture via stdin, compares exit
code and parsed JSON against the matching `*.expected.json`, and also checks the
file-path invocation form. It prints `all ... self-tests passed` on success.

## Dependencies

- [`serde_json`](https://crates.io/crates/serde_json) `=1.0.151`, with the
  `arbitrary_precision` feature. Used only for JSON parsing/serialization.
  Arbitrary precision preserves the exact decimal text of JSON numbers, which
  lets the engine validate i64 parameters and compare numbers without float
  rounding.
- Versions are pinned in `Cargo.toml` and `Cargo.lock`.
- All crate sources are vendored under `vendor/`; `.cargo/config.toml` maps
  crates.io to that directory and sets Cargo offline, so the exact build command
  works without network access.

No workflow engine, graph library, evaluator, or user-code execution is used.

## Implementation notes

- `src/main.rs` — CLI argument handling, stdin/file input, panic-to-exit-3
  safety net.
- `src/workflow.rs` — structural validation (version, ids, fields, ops,
  dependencies, cycles, retry ranges, condition shapes).
- `src/engine.rs` — deterministic topological execution, `$ref` resolution,
  conditions, skips, retries, response construction.
- `src/ops.rs` — all operations and declarative map/filter forms.
- `src/value_util.rs` — JSON types, structural equality, total ordering,
  truthiness.
- `src/decimal.rs` — exact JSON-number handling for i64 conversion and numeric
  comparison.

All arithmetic uses checked signed 64-bit operations; division truncates toward
zero, remainder follows the dividend's sign, and `i64::MIN % -1` is handled as
the mathematically correct `0`. Output is deterministic: object keys are sorted,
`sort` is stable, and no randomness or wall-clock data is used.
