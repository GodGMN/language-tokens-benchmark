# JSON Patch Engine — JavaScript

A dependency-free Node.js implementation of the JSON Patch engine described in
[`../../../tasks/patch/SPEC.md`](../../../tasks/patch/SPEC.md).

## Requirements

- Node.js 18 or newer. (Node 21+ preserves the exact lexeme of numbers through
  the `JSON.parse` reviver `context.source` API, which keeps integers beyond
  `2^53` exact. On older Node versions the implementation still works but huge
  integers fall back to JavaScript `Number` precision.)

No npm packages are used, and there is no build step.

## Run

From this directory:

```bash
node patch.js apply request.json
node patch.js apply -            # request on stdin
```

`stdout` contains exactly one JSON document and nothing else. Diagnostics, if
any, go to `stderr`.

```bash
# Success
{"status":"ok","document":{"a":1,"b":2}}

# Patch error (exit 1)
{"status":"error","code":"PATH_NOT_FOUND","index":0,"message":"..."}

# Invalid request (exit 2)
{"status":"error","code":"INVALID_REQUEST","index":null,"message":"..."}
```

Exit codes: `0` ok, `1` patch error, `2` invalid request, `3` internal error.
The patch is atomic: on any error no document is returned.

## Self-test

```bash
node selftest/run.js
```

The self-test spawns the real CLI (both stdin and file-argument modes) and
covers:

- `add` to objects and arrays, including `-`, index `0`, index `len`, and root;
- `remove` / `replace` on objects, arrays, empty-string keys and root;
- `move` to objects/arrays, to the root, onto itself and into descendants;
- `copy` including copying into a descendant and copying the root;
- `test` with nested/structural equality and mismatches;
- pointer escapes `~0`, `~1`, `~01`, invalid escapes and pointers without `/`;
- nested paths through mixed objects and arrays;
- every error code (`INVALID_REQUEST`, `INVALID_PATH`, `PATH_NOT_FOUND`,
  `INDEX_ERROR`, `UNKNOWN_OP`, `TEST_FAILED`, `INVALID_MOVE`) and non-zero
  operation indices;
- number equality by value (`1` vs `1.0` vs `1e0`) and large-integer
  preservation;
- `__proto__` as an ordinary object key.

Expected values and indices were checked by hand against the SPEC.

## Dependencies

None. Only the Node.js standard library (`fs`). No JSON Patch or JSON Pointer
library is used; pointer parsing, escaping, array insertion and error
classification are implemented in `patch.js`.

## Design notes

- JSON objects are represented internally as `Map`, so keys such as
  `__proto__` behave as ordinary members and insertion order is deterministic.
- JSON numbers are represented as `Num`, which keeps the original JSON lexeme.
  Structural equality compares their mathematical value (so `1`, `1.0` and
  `1e0` are equal), while output preserves the original representation.
- Pointers are validated before use: a `~` not followed by `0` or `1`, or a
  non-empty pointer not beginning with `/`, is `INVALID_PATH`.
- Array index syntax is `0` or `[1-9][0-9]*`; `-` is accepted only as the final
  segment of `add` (and therefore of `move`/`copy`), where it appends.
- Errors are raised as `PatchError(code, message)`. Patch errors and
  per-operation request-shape errors (`INVALID_REQUEST`, e.g. an unexpected
  field) are reported with their 0-based patch index; errors in the top-level
  envelope are reported with `index: null` and exit code 2.
