# `patch` — JSON Patch engine (C)

A self-contained C implementation of the JSON Patch engine described in
`../../../tasks/patch/SPEC.md`.

## Dependencies

None beyond a C11 compiler and the C standard library. The JSON parser and
serializer (`json.c` / `json.h`) are bundled with this submission and were
written for this task. No JSON-Patch or JSON-Pointer library is used.

## Build

From this directory:

```sh
make
```

This produces `./patch`.

## Run

```sh
./patch apply request.json
./patch apply -          # request from stdin
```

stdout contains exactly one JSON response document:

```json
{"status":"ok","document":...}
{"status":"error","code":"...","index":0,"message":"..."}
```

Exit codes: `0` ok, `1` patch error, `2` invalid request, `3` internal error.
Diagnostics (for example JSON syntax errors) go to stderr.

## Self-test

The self-test suite is structural: it parses the response JSON and checks the
status, result document (or error code and index) and process exit code.

```sh
make selftest
# or
./selftest/run.sh
```

The suite needs `python3` (test only, not for building or running `patch`) and
covers:

- `add` to objects and arrays, including `-`, index 0, middle and `len`,
- `remove`, `replace` and root replacement,
- `move` (including self/descendant rejection and array reordering),
- `copy` (including copying into a descendant and copying the root),
- `test` with structural equality and numeric value equality,
- pointer escaping (`~0` and `~1`), empty segments and nested paths,
- every error code: `INVALID_REQUEST`, `INVALID_PATH`, `PATH_NOT_FOUND`,
  `INDEX_ERROR`, `UNKNOWN_OP`, `TEST_FAILED`, `INVALID_MOVE`,
- operation indexes and patch atomicity.

## Implementation notes

- RFC 6901 pointer segments are unescaped by scanning left to right, so `~0`
  and `~1` are handled without accidental double-unescaping.
- Array tokens accept only `0` or `[1-9][0-9]*`; `-` is special-cased only for
  the final segment of `add`.
- The document is modified in place while applying the patch. On any error the
  working copy is discarded, so the patch is atomic.
- Numbers retain their original lexeme for output. `test` compares numbers by
  exact decimal value (for example `1`, `1.0`, and `1e0` are equal), not by
  their textual spelling.
- String equality compares decoded Unicode code points (equivalent raw UTF-8
  and `\uXXXX` spellings compare equal). Output is JSON with non-ASCII escaped
  as `\uXXXX`.
