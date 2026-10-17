# `workflow` — declarative workflow engine (Rust)

A complete implementation of the workflow engine described in `../../../SPEC.md`.
It reads a workflow JSON document, validates it, executes the steps in a
deterministic topological order and prints exactly one JSON response document on
stdout.

## Build

```bash
cd submissions/round-4/rust
cargo build --release
```

The binary is `target/release/workflow`.

Dependencies are pinned in `Cargo.lock` and vendored in `vendor/`
(`.cargo/config.toml` points Cargo at the vendored sources), so the build works
offline.

## Run

```bash
target/release/workflow run myworkflow.json
target/release/workflow run -          # read the document from stdin
```

- stdout: one JSON response document, nothing else.
- stderr: diagnostics only.
- Exit codes:
  - `0` — workflow completed (`status: "completed"`)
  - `1` — runtime workflow failure (`status: "failed"`)
  - `2` — input/argument/validation error (`status: "failed"`)
  - `3` — internal panic (best-effort JSON on stdout)

Examples:

```bash
target/release/workflow run selftest/01-math.json
printf '%s' '{"version":1,"steps":[{"id":"x","op":"constant","args":{"value":42}}]}' \
  | target/release/workflow run -
```

## Dependencies

- Rust standard library.
- `serde_json` **1.0.151** (with the `arbitrary_precision` feature) for JSON
  parsing/serialization. This is the only third-party crate used directly.
  Its transitive dependencies (`serde`, `serde_core`, `itoa`, `memchr`, `zmij`)
  are pinned in `Cargo.lock` and vendored.

No workflow/DAG/graph/eval libraries are used. The only operations available are
the fixed set in SPEC §4; no user code is executed. The implementation performs
no network access and no filesystem writes (it only reads the input document or
stdin).

## Self-test

Hand-written workflows and their expected responses live in `selftest/`.
The runner checks exit code, status, results, execution order, skipped steps and
the `error.code` / `error.step` pair (messages are free-form and not compared).

```bash
./selftest/run.sh
# equivalently:
cargo build --release
python3 selftest/run.py
```

Current cases:

| File | Covers |
|---|---|
| `01-math.json` | `range`, `sum`, `min`, `max`, `reduce`, `subtract`, `divide`, `mod`, empty range |
| `02-strings.json` | `trim`, `uppercase`, `replace`, `split` (incl. empty separator), `join`, `length`, `concat`, ASCII-only `lowercase` |
| `03-objects.json` | `constant` object, `keys`, `values`, `set`, `get` (missing -> null), `merge`, `pick`, `omit` |
| `04-arrays.json` | `sort` (mixed types, stable), `reverse`, `unique` (structural equality), `slice` (negative/clamped), `flatten`, `reduce` |
| `05-transforms.json` | all `map` transforms (fixed forms and `$self` op form), all common `filter` predicates, truthiness |
| `06-refs.json` | dotted references and array-index references |
| `07-conditions.json` | `all` / `any` / `not`, all comparison operators, boolean-ref condition, skipped steps |
| `08-div-zero.json` | runtime `DIVISION_BY_ZERO` with partial results |
| `09-skipped-ref.json` | `SKIPPED_REFERENCE` |
| `10-retry-exhausted.json` | retries exhausted -> `STEP_FAILED` |
| `11-overflow.json` | `ARITHMETIC_OVERFLOW` |
| `12-unknown-dependency.json` | `UNKNOWN_DEPENDENCY` (exit 2) |
| `13-float-version.json` | `version: 1.0` rejected with `INVALID_WORKFLOW` |
| `14-retry-success.json` | `fail_until` succeeds on the third attempt (SPEC example C) |

## Source layout

- `src/main.rs` — CLI argument handling, stdin/file reading, JSON output, exit codes.
- `src/engine.rs` — structural validation, DAG/cycle checks, execution, references, conditions, retries, response building.
- `src/ops.rs` — all operations and the map/filter transform/predicate declarations.
- `src/value_util.rs` — structural equality, total value ordering, truthiness, type names.
- `src/error.rs` — error codes and error structures.

## Behaviour notes

- All numeric parameters are required to be JSON integer literals fitting `i64`;
  fraction/exponent literals are rejected with `INVALID_ARG`. Arithmetic uses
  checked `i64` operations. `INT64_MIN / -1` and `INT64_MIN % -1` are
  `ARITHMETIC_OVERFLOW`; division truncates toward zero and `mod` follows the
  dividend's sign.
- Values use structural equality (so `1 == 1.0`) and the total order from SPEC §3
  for `sort`/`compare`. Strings and object keys are compared by UTF-8 bytes.
- References resolve recursively in step `args`; only direct/transitive
  dependencies are targetable, and references to skipped steps produce
  `SKIPPED_REFERENCE`.
- Execution picks, among ready steps, the one earliest in the `steps` array.
  Skipped steps get `null` results and are included in `execution_order` and
  `skipped`; a definitively failed step is excluded from `execution_order`.
- Only `TRANSIENT_FAILURE` from `fail_until` is retried; when attempts run out
  the final error code is `STEP_FAILED`.
