# `patch` — JSON Patch engine (Python)

A dependency-free Python implementation of the JSON Patch engine defined in
`../../../tasks/patch/SPEC.md` (spec version 1). It implements RFC 6902-style
operations over RFC 6901 JSON Pointers with the exact request/response
envelope, error classification and exit codes required by the SPEC.

## Requirements

- Python 3.8 or newer (uses only the standard library).
- No third-party packages, no build step, no network access.

## Run

From this directory (`submissions/round-9-patch/python/`):

```bash
# request from a file
python3 patch.py apply request.json

# request from stdin
python3 patch.py apply -
```

Example:

```bash
$ echo '{"document":{"a":1},"patch":[{"op":"add","path":"/b","value":2}]}' \
    | python3 patch.py apply -
{"status":"ok","document":{"a":1,"b":2}}
```

Stdout contains exactly one JSON document (compact, one line) and nothing
else. Any diagnostic text goes to stderr.

Exit codes:

| exit | meaning |
|---|---|
| 0 | `status: "ok"` |
| 1 | patch error (`INVALID_PATH`, `PATH_NOT_FOUND`, `INDEX_ERROR`, `UNKNOWN_OP`, `TEST_FAILED`, `INVALID_MOVE`) |
| 2 | `INVALID_REQUEST` (bad CLI usage, unreadable file, malformed JSON, bad envelope or bad operation shape) |
| 3 | internal error |

## Implementation notes

- **Pointer parsing** is hand-written: `""` is the root, non-empty paths must
  start with `/`, and `~1`/`~0` are unescaped left-to-right (so `~01` is the
  literal `~1`). A `~` not followed by `0` or `1` is `INVALID_PATH`.
- **Array indices** must be `0` or `[1-9][0-9]*`. `-` means "append" and is
  accepted only where `add` semantics apply (`add`, and the target of
  `move`/`copy`). Any other index token, a leading zero, a sign, or an
  out-of-range index produces `INDEX_ERROR`.
- **Traversal:** missing object keys and traversal into scalars are
  `PATH_NOT_FOUND`; malformed/out-of-range array indices are `INDEX_ERROR`.
- **`add`**: replaces the root for `""`; otherwise inserts/overwrites. Array
  indices `0..len` insert before that position, `-` appends.
- **`remove`/`replace`**: the root cannot be removed (`INVALID_PATH`); object
  members must exist; array indices must be `0..len-1`.
- **`move`**: checks that `from` exists, then rejects same-path and strict
  descendant targets with `INVALID_MOVE`, then performs remove-then-add. This
  means moving within an array follows the shifted-index semantics of
  remove-then-add. (Consequently moving the root is always `INVALID_MOVE`,
  since every other pointer is a descendant of `""`.)
- **`copy`**: deep-copies the source, then applies `add` semantics; copying
  into a descendant of the source is allowed.
- **`test`**: structural equality where numbers compare by value (`1` equals
  `1.0`), booleans are distinct from numbers, arrays are ordered, objects are
  key-order insensitive.
- **Atomicity:** operations are applied in order; on the first failure no
  document is returned and `index` names the failing operation. Envelope
  errors use `index: null`.
- JSON parsing is strict: malformed JSON, trailing data, non-standard
  `NaN`/`Infinity` constants and out-of-range floats are `INVALID_REQUEST`.

## Self-test

The self-test invokes the real CLI as a subprocess (both stdin and file
input), parses stdout, and checks the full response plus exit code against
hand-written expectations. It covers every operation, `~0`/`~1` escapes,
nested paths, array insertion (including `-`), move-into-descendant, copy,
test equality, and at least one case of every error code:

```bash
python3 selftest/run_selftest.py
```

Expected output:

```text
OK: 106 self-test cases passed
```

## Files

```text
patch.py                    implementation and CLI
selftest/run_selftest.py    black-box self-verification suite
README.md                   this file
```
