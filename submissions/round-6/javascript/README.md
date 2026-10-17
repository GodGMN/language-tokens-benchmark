# `workflow` CLI — JavaScript implementation

Declarative workflow engine implementing `SPEC.md` (round 6). Pure Node.js,
zero runtime dependencies (standard library only), no build step.

## Requirements

- Node.js 18+ (tested with Node 24). Uses only `fs` and `buffer`.

## Build

No build step (interpreted language):

```bash
cd submissions/round-6/javascript
```

## Run

```bash
node workflow.js run myworkflow.json
node workflow.js run -            # read the JSON workflow from stdin
```

- **stdout**: exactly one JSON document (the response).
- **stderr**: unused for normal runs.
- **Exit codes**: `0` completed, `1` runtime workflow failure, `2`
  validation/input error, `3` internal error.

Examples:

```bash
# worked example A from the spec
node workflow.js run selftest/math_ops.json

# stdin
cat selftest/retry.json | node workflow.js run -
```

## Self-test

Ten hand-written fixtures live in `selftest/*.json`, with the expected response
documents in `selftest/expected/*.json`. The runner feeds every workflow to the
CLI over stdin and compares the parsed JSON (object key order ignored, array
order respected) and the exit code:

```bash
node selftest/run.js
```

Coverage of the fixtures:

| fixture | what it exercises |
|---|---|
| `math_ops.json` | add, subtract, multiply, divide (trunc toward zero), mod (sign of dividend), sum, min, max, range, empty range |
| `string_pipeline.json` | constant, trim, lowercase, replace, split (incl. empty separator = code points), join, uppercase, length (code points) |
| `array_ops.json` | map, filter, reduce, sort (mixed types), reverse, unique, slice (negative indices), flatten |
| `object_ops.json` | get (missing → null), set (non-mutating), keys/values (byte-sorted), merge, pick, omit |
| `references.json` | `$ref` with dotted paths and array indices, nested refs, refs inside arrays |
| `transforms.json` | all `map.transform` forms (arith, negate, abs, stringify, type, constant, `type: op` with `$self`) and filter predicates |
| `skipped_step.json` | `when` false → `null` result, `skipped` list, compound `all`/`not` conditions |
| `retry.json` | `fail_until` with `max_attempts` (worked example C) |
| `div_zero_failure.json` | runtime failure: `DIVISION_BY_ZERO`, `error.step`, partial `results`, dependent step not run |
| `utilities.json` | equals (structural), compare, and/or/not, type |

## Dependencies

None beyond the Node.js standard library (`fs`, `buffer`). No workflow engine,
graph library, or code interpreter is used; operations are a fixed closed set.
No `eval`, no network, no filesystem writes.

## Implementation notes

- **Custom strict JSON parser.** It distinguishes integer literals from
  fraction/exponent literals (needed because numeric op parameters must be
  integer literals), keeps integers as `BigInt` (exact i64 checks), and records
  duplicate object keys so the `type_is` predicate works even when written with
  the duplicate key shown in the spec (`{"type": "type_is", "type": "string"}`);
  `value`/`type_name` are accepted too.
- **Value model.** `null`, booleans, strings, JS arrays, `Map` for JSON objects
  (safe for any key, including `__proto__`), and tagged `I64`/`F64` numbers.
  A custom serializer emits BigInts exactly.
- **i64 arithmetic** uses `BigInt` with explicit range checks; overflow is
  `ARITHMETIC_OVERFLOW`; `INT64_MIN / -1` and `INT64_MIN % -1` are
  `ARITHMETIC_OVERFLOW`; division truncates toward zero and modulo takes the
  dividend's sign.
- **Strings** are UTF-8; case conversion is ASCII-only; `length` and empty
  separator `split` work on Unicode code points; ordering uses UTF-8 byte
  comparison.
- **Validation** happens before execution (version, ids, duplicates, unknown
  fields, op names, retry range, `when` shape, dependency existence, cycles).
  Op argument checks happen at execution time (`INVALID_ARG`, exit 1).
- **Execution** is a deterministic topological walk: among ready steps the one
  earliest in `steps` is chosen. `when` is evaluated before args are resolved.
  Only `TRANSIENT_FAILURE` is retried; exhausting attempts reports
  `STEP_FAILED`.

## Source layout

```
workflow.js          the whole CLI (single file, ~1300 lines)
README.md            this file
selftest/            workflow fixtures + expected responses
selftest/run.js      self-test runner (no dependencies)
```
