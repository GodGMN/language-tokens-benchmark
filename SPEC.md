# SPEC: Declarative Workflow Engine

Functional contract for the `workflow` CLI. Every implementation must behave exactly as described here. This document is the single source of truth.

## 1. CLI

Command:

```bash
workflow run <workflow.json>
workflow run -            # read the document from stdin
```

Behavior:

- stdout: exactly one valid JSON document (the response). No other stdout output is allowed.
- stderr: free for diagnostics; not checked.
- No interactive prompts. No network. No reads outside the input document (and stdin).

### Exit codes

| Code | Meaning |
|---|---|
| 0 | Workflow executed; response `status` is `completed` |
| 1 | Workflow failed at runtime; response `status` is `failed` |
| 2 | Input/argument/validation error; response `status` is `failed` |
| 3 | Internal program error (bug, panic); best-effort JSON on stdout |

### Response format

Always this shape (fields may be null/empty, but all are present):

```json
{
  "status": "completed",
  "results": { "<step_id>": <json value> },
  "execution_order": ["<step_id>", "..."],
  "skipped": ["<step_id>"],
  "error": null
}
```

- `status`: `"completed"` | `"failed"`.
- `results`: map step id -> final value. Skipped steps map to `null`. Only finished steps appear.
- `execution_order`: ids in the order steps finished (skipped steps count, failed step does not).
- `skipped`: ids whose `when` condition was false. Always present.
- `error`: `null` on success, otherwise:

```json
{
  "code": "<ERROR_CODE>",
  "step": "<step_id or null>",
  "message": "<human readable, not checked>"
}
```

## 2. Workflow document

```json
{
  "version": 1,
  "steps": [ ... ]
}
```

- `version`: required, must be the integer literal `1` (`1.0` is rejected with `INVALID_WORKFLOW`).
- `steps`: required array, may be empty (valid workflow, no steps).
- Unknown top-level fields: reject (`INVALID_WORKFLOW`).

### Step

```json
{
  "id": "my_step",
  "op": "add",
  "args": { "a": 1, "b": 2 },
  "depends_on": ["other"],
  "when": { "op": "gt", "left": {"$ref": "other"}, "right": 5 },
  "retry": { "max_attempts": 3 }
}
```

| Field | Type | Default | Rules |
|---|---|---|---|
| `id` | string | required | matches `[A-Za-z_][A-Za-z0-9_]*`, unique per workflow |
| `op` | string | required | one of the operation names in §4 |
| `args` | object | `{}` | operation-specific; unknown keys rejected |
| `depends_on` | array of id | `[]` | must reference existing step ids; no cycles |
| `when` | condition | none (always run) | §5 |
| `retry` | object | `{max_attempts: 1}` | `max_attempts` integer 1..5 |

Unknown step fields: reject (`INVALID_WORKFLOW`).

## 3. Values, types and arithmetic

### JSON values

Standard JSON: `null`, `boolean`, `number`, `string`, `array`, `object`.

### Numbers

- All numeric parameters and math results are **signed 64-bit integers** (`i64`).
- A numeric parameter must be a JSON **integer literal** (`123`, `-4`) that fits in i64. Literals with a fraction or exponent (`1.0`, `1e3`) are rejected with `INVALID_ARG`, even when the value is integral.
- `type` op reports a JSON number as `"number"` regardless.

### i64 arithmetic

- `+ - * / %` wrap-check: if the true mathematical result is outside `[2^63, 2^63-1]`, the operation fails with `ARITHMETIC_OVERFLOW`.
- `INT64_MIN / -1` and `INT64_MIN % -1` are `ARITHMETIC_OVERFLOW` (this avoids the trap/UB they cause in C and Rust).
- `divide`: truncates toward zero. `7 / 2 = 3`, `-7 / 2 = -3`, `-7 / -2 = 3`.
- `mod`: sign of the **dividend** (C99 style). `7 % 2 = 1`, `-7 % 2 = -1`, `7 % -2 = 1`, `-7 % -2 = -1`.
- Division by zero: `DIVISION_BY_ZERO` (an operation error, not retriable — see §7).

### Strings

- Strings are UTF-8. Invalid UTF-8 should not occur (JSON input), but implementations may reject with `INVALID_WORKFLOW`.
- "Character" means **Unicode code point**. `length` counts code points.
- Uppercase/lowercase are **ASCII-only**: only A–Z / a–z transform; all other code points pass through unchanged.
- String ordering (for `sort`, etc.): byte-wise lexicographic order of the UTF-8 encoding.

### Equality (structural)

`equals`, `eq`, and dedup (`unique`) all use structural equality:

- Numbers: `1 == 1.0` (numeric value equality of the JSON numbers).
- Objects: same keys, structurally equal values; key order irrelevant.
- Arrays: same length, element-wise equal.
- `null` equals only `null`. Different types are never equal.

### Ordering of objects' keys

Wherever an operation **returns keys or values** (`keys`, `values`), the result array is sorted by key in byte-wise lexicographic UTF-8 order.

### Ordering of values (total order used by `sort` and `compare`)

All values are totally ordered so that `sort` is deterministic:

1. First by type: `number` < `string` < `boolean` < `null` < `array` < `object`.
2. Within `number`: by numeric value.
3. Within `string`: by UTF-8 bytes.
4. Within `boolean`: `false` < `true`.
5. Within `null`: all equal.
6. Within `array`: element-wise using this same order; if one is a prefix of the other, the shorter one is smaller.
7. Within `object`: compare the sorted key lists first (element-wise, byte order); if equal, compare the values in that sorted-key order.

`sort` is **stable**: elements that compare equal keep their original relative order.

## 4. Operations

Argument names are exact. Missing required arg: `INVALID_ARG`. Wrong type: `INVALID_ARG`. Unknown argument name: `INVALID_ARG`.
Every op takes its `args` after `$ref` resolution.

### Math (2-number ops use args `a`, `b`)

| op | args | result |
|---|---|---|
| `add` | `a`, `b` | `a + b` |
| `subtract` | `a`, `b` | `a - b` |
| `multiply` | `a`, `b` | `a * b` |
| `divide` | `a`, `b` | `a / b` (trunc toward zero); `b=0` -> `DIVISION_BY_ZERO` |
| `mod` | `a`, `b` | `a % b`; `b=0` -> `DIVISION_BY_ZERO` |
| `sum` | `input` (array of i64) | sum; empty array -> `0` |
| `min` | `input` (array of i64) | minimum; empty array -> `min_empty` error `EMPTY_ARRAY` |
| `max` | `input` (array of i64) | maximum; empty array -> `EMPTY_ARRAY` |
| `range` | `start`, `end` | inclusive `[start..end]` ascending; `start > end` -> `[]` |

### Strings

| op | args | result |
|---|---|---|
| `concat` | `a`, `b` (strings) | concatenation |
| `split` | `input` (string), `sep` (string, literal) | array of strings; empty `sep` -> one char (code point) per element |
| `join` | `input` (array of strings), `sep` (string) | joined; empty array -> `""` |
| `uppercase` | `input` (string) | ASCII upper |
| `lowercase` | `input` (string) | ASCII lower |
| `trim` | `input` (string) | remove ASCII whitespace (`space`, `\t`, `\n`, `\r`, `\f`, `\v`) from both ends |
| `replace` | `input`, `search`, `replace` (all strings) | replace all literal, non-overlapping occurrences (left to right); empty `search` -> `INVALID_ARG` |
| `length` | `input` (string) | number of Unicode code points (i64) |

### Arrays

| op | args | result |
|---|---|---|
| `map` | `input` (array), `transform` (declared op, below) | transform applied to each element |
| `filter` | `input` (array), `predicate` (declared predicate, below) | elements satisfying the predicate, order preserved |
| `reduce` | `input` (array), `op` (`add`\|`subtract`\|`multiply`), `initial` (optional i64) | left fold with the accumulator starting at `initial`. If `initial` is omitted it defaults to the operator's identity: `0` for `add`/`subtract`, `1` for `multiply`. An empty array yields the accumulator unchanged (`initial` or the identity). |
| `sort` | `input` (array) | ascending by value type; mixed types ordered: number < string < boolean < null < array < object; stable for equal keys (original order kept) |
| `reverse` | `input` (array) | reversed |
| `unique` | `input` (array) | dedup by structural equality, first occurrence kept |
| `slice` | `input` (array), `start` (i64), `end` (optional i64) | like Python slice on indices: negatives count from end, clamped; `end` omitted -> end; `start > end` -> `[]` |
| `flatten` | `input` (array) | one level: arrays' elements are spliced in place; non-arrays kept as elements |

**`map.transform`** (one of):
- `{"type": "op", "op": "add", "args": {"a": {"$self": true}, "b": 2}}` — apply a 2-number op with the element.
- Simpler, fixed forms are **the only** supported transformations:
  - `{"type": "constant"}` -> each element replaced by the same value `value`.
  - `{"type": "add", "value": i64}` / `{"type": "subtract", "value": i64}` / `{"type": "multiply", "value": i64}` / `{"type": "divide", "value": i64}` / `{"type": "mod", "value": i64}` — binary op with the element as left operand.
  - `{"type": "negate"}` — `-element`.
  - `{"type": "abs"}` — absolute value.
  - `{"type": "stringify"}` — element -> JSON string (compact, no spaces).
  - `{"type": "type"}` — element -> its type name string (§6).

**`filter.predicate`** (one of):
- `{"type": "even"}` / `{"type": "odd"}` — element % 2.
- `{"type": "gt", "value": i64}` / `gte` / `lt` / `lte` / `eq` / `ne` — compare element to value.
- `{"type": "truthy"}` / `{"type": "falsy"}` — JSON truthiness: `false`, `null`, `""`, `0`, `[]`, `{}` are falsy.
- `{"type": "type_is", "type": "<type>"}` — element's type name equals.
- `{"type": "string_contains", "value": string}` — literal substring.

### Objects (objects are JSON objects)

| op | args | result |
|---|---|---|
| `get` | `input` (object), `key` (string) | the value, or `null` if missing (missing and `null` indistinguishable here by design) |
| `set` | `input` (object), `key` (string), `value` (any) | new object with key set (input not mutated) |
| `keys` | `input` (object) | array of keys, sorted (byte-wise) |
| `values` | `input` (object) | array of values ordered by their key (byte-wise) |
| `merge` | `a`, `b` (objects) | union; on conflict, `b` wins |
| `pick` | `input` (object), `keys` (array of strings) | subset (sorted keys); missing keys included as `null` |
| `omit` | `input` (object), `keys` (array of strings) | everything except listed keys |

### Utilities

| op | args | result |
|---|---|---|
| `constant` | `value` (any JSON) | the value |
| `equals` | `a`, `b` | structural equality (true/false) |
| `compare` | `a`, `b`, `op` (`gt`\|`gte`\|`lt`\|`lte`) | booleans; numbers compare numerically, strings byte-wise; mixed types: order number < string < boolean < null < array < object |
| `and` | `a`, `b` | JSON booleans: `a && b` |
| `or` | `a`, `b` | `a \|\| b` |
| `not` | `a` | `!a` |
| `type` | `value` | one of `"null"`, `"boolean"`, `"number"`, `"string"`, `"array"`, `"object"` |

### Test operation (special)

| op | args | result |
|---|---|---|
| `fail_until` | `failures` (i64, 0..10), `value` (any) | fails with `TRANSIENT_FAILURE` on its first `failures` attempts of this workflow run; then returns `value` |

`fail_until` is the only operation designed to fail. The attempt counter resets each workflow execution and is local to the step.

## 5. Conditions (`when`)

```json
{ "op": "gt", "left": <value|ref>, "right": <value|ref> }
```

- Operators: `eq`, `ne`, `gt`, `gte`, `lt`, `lte` (semantics of `equals`/`compare` from §4).
- Compound:
  - `{ "op": "all", "conditions": [ ... ] }` — logical AND.
  - `{ "op": "any", "conditions": [ ... ] }` — logical OR.
  - `{ "op": "not", "condition": { ... } }` — negation.
- `left`/`right` may be `$ref`s or literals.
- Result must be a strict boolean; non-boolean final value -> `INVALID_CONDITION`.

## 6. `$ref` references

- `{"$ref": "step_id"}` and dotted paths: `{"$ref": "step_id.user.name"}`, array indices: `{"$ref": "step_id.items.0"}`.
- Array indices are **non-negative decimal integers**. A negative (`items.-1`) or non-numeric (`items.x`) segment is `INVALID_REFERENCE`.
- An object is a reference **only** if it has exactly one key, `$ref`, whose value is a string.
- References resolve recursively anywhere inside `args` (including inside arrays and nested objects).
- A reference may only target a step that is a direct or transitive `depends_on` of the referencing step.
- Path resolution: object key lookup or array index. Missing key/index -> `INVALID_REFERENCE`.
- If the referenced step was **skipped** (result `null` because `when` was false): referencing it -> `SKIPPED_REFERENCE`.
- Self-reference or reference to a non-dependency -> `INVALID_REFERENCE` at resolution time (structural pre-checks cover the common cases; runtime is authoritative).

## 7. Execution model

1. **Structural validation** (before any step runs): version, ids, unique ids, field shapes, unknown fields, op names, `depends_on` exist, no cycles, `retry` range, `when` shape. Failures: `INVALID_WORKFLOW` (exit 2).
2. **Topological execution** with deterministic tie-break: among ready steps (all deps finished/skipped), pick the one earliest in the `steps` array.
3. For each step: evaluate `when` -> if false, record `null` result, add to `execution_order` and `skipped`, continue. Else resolve `args` refs, run op.
4. **Retries**: an operation failure of type `TRANSIENT_FAILURE` (from `fail_until`) is retried up to `max_attempts` (default 1). Other operation errors are NOT retried.
5. When a step definitively fails:
   - `status`: `failed`, exit 1.
   - `error.step`: the failed step id.
   - `results`: all previously completed steps (completed only, not the failed one).
   - `execution_order`: completed steps' ids, in order (skipped included; failed step excluded).

### Error codes

| Code | Exit | Meaning |
|---|---|---|
| `INVALID_WORKFLOW` | 2 | structural validation failure |
| `UNKNOWN_DEPENDENCY` | 2 | `depends_on` names a missing id |
| `CYCLIC_DEPENDENCY` | 2 | dependency cycle |
| `INVALID_REFERENCE` | 1 | bad `$ref` target or path |
| `SKIPPED_REFERENCE` | 1 | ref to a skipped step |
| `INVALID_ARG` | 1 | wrong arg name/type for an op |
| `INVALID_CONDITION` | 1 | `when` not evaluating to a boolean |
| `ARITHMETIC_OVERFLOW` | 1 | i64 overflow |
| `DIVISION_BY_ZERO` | 1 | divide/mod by zero |
| `EMPTY_ARRAY` | 1 | `min`/`max` on empty array |
| `TRANSIENT_FAILURE` | 1 | `fail_until` exhausted its attempts (final code when retries run out is `STEP_FAILED`) |
| `STEP_FAILED` | 1 | generic: step failed after retries |

`TRANSIENT_FAILURE` is the per-attempt code; when attempts are exhausted the final response uses `STEP_FAILED`.

### `error.step`

The `step` field is the id of the step responsible for the failure, or `null` when the failure is a property of the whole document:

| Situation | `step` |
|---|---|
| Any runtime failure of a step (bad args, overflow, div-by-zero, bad/skipped reference, bad condition, retries exhausted) | that step's id |
| `UNKNOWN_DEPENDENCY` | the step whose `depends_on` is unknown |
| `INVALID_WORKFLOW` about one step's fields | that step's `id` when it is a string (even if it fails the id pattern), else `null` |
| `INVALID_WORKFLOW` about `version`/`steps`, and `CYCLIC_DEPENDENCY` | `null` |

`error.message` is free-form and is never compared. `error.code` and `error.step` are always compared.

## 8. Worked examples

### Example A — pipeline

```json
{
  "version": 1,
  "steps": [
    { "id": "n", "op": "range", "args": { "start": 1, "end": 5 } },
    { "id": "evens", "op": "filter", "depends_on": ["n"],
      "args": { "input": {"$ref": "n"}, "predicate": {"type": "even"} } },
    { "id": "total", "op": "sum", "depends_on": ["evens"],
      "args": { "input": {"$ref": "evens"} } }
  ]
}
```

```json
{
  "status": "completed",
  "results": { "n": [1,2,3,4,5], "evens": [2,4], "total": 6 },
  "execution_order": ["n", "evens", "total"],
  "skipped": [],
  "error": null
}
```

### Example B — skipped step

```json
{
  "version": 1,
  "steps": [
    { "id": "x", "op": "constant", "args": { "value": 3 } },
    { "id": "y", "op": "constant", "args": { "value": 99 },
      "depends_on": ["x"],
      "when": { "op": "gt", "left": {"$ref": "x"}, "right": 10 } }
  ]
}
```

```json
{
  "status": "completed",
  "results": { "x": 3, "y": null },
  "execution_order": ["x", "y"],
  "skipped": ["y"],
  "error": null
}
```

### Example C — retry

```json
{
  "version": 1,
  "steps": [
    { "id": "flaky", "op": "fail_until",
      "args": { "failures": 2, "value": "ok" },
      "retry": { "max_attempts": 3 } }
  ]
}
```

```json
{
  "status": "completed",
  "results": { "flaky": "ok" },
  "execution_order": ["flaky"],
  "skipped": [],
  "error": null
}
```
