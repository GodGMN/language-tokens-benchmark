# `patch` — JSON Patch engine (Rust)

A CLI implementation of the JSON Patch engine described in
`tasks/patch/SPEC.md`.

## Build

From this directory:

```bash
cargo build --release
```

The binary is written to `target/release/patch`.

## Run

```bash
target/release/patch apply request.json
target/release/patch apply -          # read the request from stdin
```

`stdout` contains exactly one JSON document:

```json
{"status":"ok","document":{...}}
{"status":"error","code":"PATH_NOT_FOUND","index":0,"message":"..."}
```

Exit codes: `0` success, `1` patch error, `2` invalid request, `3` internal
error. Diagnostics (if any) go to stderr; messages in error responses are
free-form and are not part of the compared contract.

## Self-test

```bash
bash selftest/run.sh
```

This builds the release binary and runs `selftest/cases.json` through both the
stdin and file invocation modes. The suite covers object/array add (including
`-` and insertion at `len`), remove, replace, move (including descendant and
same-path errors), copy (including into a descendant), test/equality, pointer
escapes `~0`/`~1`, nested and empty-key paths, operation ordering, atomicity,
and every error code (`INVALID_REQUEST`, `INVALID_PATH`, `PATH_NOT_FOUND`,
`INDEX_ERROR`, `UNKNOWN_OP`, `TEST_FAILED`, `INVALID_MOVE`).

The self-test runner uses Python 3 to compare JSON responses and exit codes.
Python is not needed to build or run the engine itself.

## Dependencies

- Rust standard library.
- `serde_json` **1.0.151**, pinned exactly in `Cargo.toml`, with the
  `preserve_order` feature (which uses `indexmap`). `Cargo.lock` pins the full
  dependency graph so builds are reproducible.

`serde_json` is used only to parse and serialize JSON. JSON Pointer parsing,
escaping/unescaping, array-index validation, all six patch operations,
structural equality and error classification are implemented directly in
`src/main.rs`. No JSON Patch/JSON Pointer library is used.

## Implementation notes

- A patch is applied to a copy of the input document; on error no document is
  returned.
- Object member order is preserved. Removing a key shifts the remaining keys
  (no swap-remove), and adding a new key appends it, matching normal insertion
  order semantics.
- Array indices must be `0` or `[1-9][0-9]*`; `-` is accepted only where `add`
  semantics apply (including the add step of `move`/`copy`).
- Numbers compare by numeric value; other JSON types compare structurally.
- Operation-shape failures are `INVALID_REQUEST` with the offending operation's
  0-based index. Envelope failures (bad JSON, missing/unknown top-level fields,
  non-array patch, CLI/file errors) use `index: null`.
