# `patch` — JSON Patch Engine (Go)

A standalone CLI that applies an RFC 6902-style JSON Patch to a JSON document
using RFC 6901 JSON Pointers, as defined by `tasks/patch/SPEC.md` (spec version
1).

## Build

```bash
cd submissions/round-9-patch/go
go build -o patch .
```

Requires Go 1.21 or newer. There are **no third-party dependencies** — only the
Go standard library (`encoding/json`, `math/big`, etc.). No JSON-patch or
JSON-pointer library is used; pointer parsing, unescaping, array indexing,
insertion semantics, deep copies and structural equality are implemented here.

## Run

```bash
./patch apply request.json
./patch apply -            # request from stdin
```

Example request:

```json
{ "document": { "a": 1 }, "patch": [ { "op": "add", "path": "/b", "value": 2 } ] }
```

Example output:

```json
{"status":"ok","document":{"a":1,"b":2}}
```

stdout contains exactly one JSON document and nothing else. Diagnostics, if
any, go to stderr.

Exit codes:

| code | meaning |
|---|---|
| 0 | success (`status: "ok"`) |
| 1 | patch error (`INVALID_PATH`, `PATH_NOT_FOUND`, `INDEX_ERROR`, `UNKNOWN_OP`, `TEST_FAILED`, `INVALID_MOVE`) |
| 2 | invalid request (`INVALID_REQUEST`) |
| 3 | internal error (output failure) |

Error responses carry the zero-based index of the failing operation, or
`null` for envelope errors. The patch is atomic: a failed request never returns
a partially patched document.

## Self-test

```bash
./selftest/run.sh          # builds ./patch and runs the black-box cases
# or, after building:
python3 selftest/run.py
```

`selftest/run.py` drives the binary through the CLI, parses stdout as JSON and
checks both the response and the process exit code. It requires Python 3 only
for running the self-test, not for the implementation. The cases cover:

- `add` to objects, array insertion, `-` append, index == length, root replace
- `remove`, `replace` (objects, arrays, root)
- `move` (including same-array post-remove indexing, child-to-ancestor, to root)
- `copy` (including into a descendant of `from`)
- `test` (numbers by value, object key order independence, root, failures)
- pointer escapes `~0` / `~1`, the `~01` ordering case, nested paths, empty keys
- every error code: `INVALID_REQUEST`, `INVALID_PATH`, `PATH_NOT_FOUND`,
  `INDEX_ERROR`, `UNKNOWN_OP`, `TEST_FAILED`, `INVALID_MOVE`
- atomicity and operation indices for multi-op patches

Error `message` text is deliberately not compared, per the spec.

## Implementation notes

- **Numbers** are decoded with `json.Decoder.UseNumber()` so their original
  lexical form is preserved on output. Equality compares them exactly by value
  using `math/big.Rat` (so `1`, `1.0` and `1e0` are equal).
- **Input ordering** of object keys is not preserved by Go maps; output is a
  valid JSON document with the same semantics. Object equality is
  order-independent.
- **Pointer parsing** splits on `/`, then unescapes each segment in a single
  left-to-right pass implementing `~1` -> `/` before `~0` -> `~`; any other
  `~` sequence is `INVALID_PATH`.
- **Array indices** must be exactly `0` or `[1-9][0-9]*`. `-` is accepted only
  as the final token of `add`, where it appends. Malformed or out-of-range
  indices are `INDEX_ERROR`; missing object members and traversal through
  scalars are `PATH_NOT_FOUND`.
- **Move** verifies that `from` exists, then rejects moving onto itself or into
  a strict descendant (comparing parsed pointer segments), then removes before
  adding using normal `add` semantics.
- **Copy** deep-copies the source subtree before adding it, so later mutations
  of one copy cannot affect the other.

## Files

| path | purpose |
|---|---|
| `main.go` | CLI, request envelope parsing, operation shape validation, responses |
| `pointer.go` | RFC 6901 pointer parsing/unescaping, array index rules, descendant checks |
| `ops.go` | `add`/`remove`/`replace`/`move`/`copy`/`test` semantics |
| `value.go` | deep copy and structural JSON equality |
| `selftest/` | black-box self-test suite |
