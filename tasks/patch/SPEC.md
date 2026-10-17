# SPEC: JSON Patch Engine (`patch`)

Apply an RFC-6902-style patch to a JSON document, using RFC-6901 JSON Pointer
paths. Pointer escaping, array insertion semantics and error classification
are the hard parts.

**Spec version: 1.**

## 1. CLI

```bash
patch apply <request.json>
patch apply -            # request from stdin
```

Request:

```json
{ "document": { "a": 1 }, "patch": [ { "op": "add", "path": "/b", "value": 2 } ] }
```

- `document`: required, any JSON value.
- `patch`: required array of operations.
- Unknown top-level fields -> `INVALID_REQUEST`.

### Response

```json
{ "status": "ok", "document": { "a": 1, "b": 2 } }
{ "status": "error", "code": "PATH_NOT_FOUND", "index": 0, "message": "..." }
```

- `index`: 0-based index of the failing operation — including an operation
  with a bad shape — or `null` for envelope errors (bad top-level request).
- `message`: free-form, never compared.
- On error the original document is **not** returned; the patch is atomic.

### Exit codes

| code | meaning |
|---|---|
| 0 | `status: "ok"` |
| 1 | patch error (`status: "error"` with an op index) |
| 2 | invalid request (`INVALID_REQUEST`) |
| 3 | internal error |

## 2. JSON Pointer

Paths are RFC 6901 pointers:

- `""` is the whole document.
- Otherwise the path must begin with `/`; segments are separated by `/`.
- Unescaping: replace `~1` with `/`, then `~0` with `~`
  (so `/a~1b` addresses key `a/b`, `/a~0b` addresses key `a~b`).
- A `~` not followed by `0` or `1` is `INVALID_PATH`.
- A non-empty path not starting with `/` is `INVALID_PATH`.

### Array indices

An array segment is valid only if it is `0` or matches `[1-9][0-9]*`
(no leading zeros, no sign). `-` is accepted **only** by `add`, meaning
"append". Anything else is `INDEX_ERROR`.

## 3. Operations

All operations are applied in order. Each op is an object with `op` and
`path`, plus the extra fields listed below. Any field not listed for that op
is `INVALID_REQUEST`.

| op | extra fields | behaviour |
|---|---|---|
| `add` | `value` | insert/replace |
| `remove` | — | delete an existing member |
| `replace` | `value` | replace an existing member |
| `move` | `from` | remove `from`, then `add` at `path` |
| `copy` | `from` | deep-copy `from`, then `add` at `path` |
| `test` | `value` | assert equality |

Unknown `op` -> `UNKNOWN_OP`.

### add

- `path == ""` replaces the whole document with `value`.
- Parent must exist; otherwise `PATH_NOT_FOUND`.
- Object: set the key (create or overwrite).
- Array: `-` appends; an index `0..len` inserts before that position;
  anything else is `INDEX_ERROR`.
- A scalar parent -> `PATH_NOT_FOUND`.

### remove

- `path == ""` -> `INVALID_PATH` (cannot remove the root).
- Object: key must exist, else `PATH_NOT_FOUND`.
- Array: index must be `0..len-1`, else `INDEX_ERROR`.

### replace

- `path == ""` replaces the whole document.
- Object: key must exist, else `PATH_NOT_FOUND`.
- Array: index must be `0..len-1`, else `INDEX_ERROR`.

### move

- `from` must exist, else `PATH_NOT_FOUND`.
- `from` must not equal `path` -> `INVALID_MOVE`.
- `path` must not be a strict descendant of `from` -> `INVALID_MOVE`.
- Otherwise remove `from`, then `add` at `path` (with `add` semantics).

### copy

- `from` must exist, else `PATH_NOT_FOUND`.
- Deep-copies the value, then `add` at `path`.
- Copying into a descendant of `from` is allowed.

### test

- The value at `path` must exist, else `PATH_NOT_FOUND`.
- It must be structurally equal to `value`, else `TEST_FAILED`.
- `path == ""` tests the whole document.

## 4. Equality (structural)

Numbers by value, strings by code points, booleans by value, arrays
element-wise, objects by key set and value, `null` equals only `null`.
Different types are never equal.

## 5. Error codes

| code | exit | when |
|---|---|---|
| `INVALID_REQUEST` | 2 | bad envelope or op shape |
| `INVALID_PATH` | 1 | malformed pointer / root remove |
| `PATH_NOT_FOUND` | 1 | missing member or non-container parent |
| `INDEX_ERROR` | 1 | array index out of range or malformed |
| `UNKNOWN_OP` | 1 | unknown `op` |
| `TEST_FAILED` | 1 | `test` value mismatch |
| `INVALID_MOVE` | 1 | move to itself or into a descendant |

## 6. Examples

| document | patch | result |
|---|---|---|
| `{"a":1}` | `[{"op":"add","path":"/b","value":2}]` | `{"a":1,"b":2}` |
| `[1,2,3]` | `[{"op":"add","path":"/1","value":9}]` | `[1,9,2,3]` |
| `[1,2,3]` | `[{"op":"add","path":"/-","value":4}]` | `[1,2,3,4]` |
| `{"a":{"b":1}}` | `[{"op":"remove","path":"/a/b"}]` | `{"a":{}}` |
| `{"a":1}` | `[{"op":"move","from":"/a","path":"/b"}]` | `{"b":1}` |
| `{"a":1}` | `[{"op":"test","path":"/a","value":1}]` | `{"a":1}` |
| `{"a/b":1}` | `[{"op":"remove","path":"/a~1b"}]` | `{}` |
| `{"a":1}` | `[{"op":"remove","path":"/x"}]` | `PATH_NOT_FOUND`, index 0 |
| `[1]` | `[{"op":"add","path":"/5","value":0}]` | `INDEX_ERROR`, index 0 |
| `{"a":1}` | `[{"op":"move","from":"/a","path":"/a/b"}]` | `INVALID_MOVE`, index 0 |

## 7. Determinism

No randomness, time, locale or filesystem access. Same request -> same
response. stdout contains only the JSON document.
