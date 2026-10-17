# Declarative Workflow Engine — JavaScript

A complete implementation of the declarative workflow engine described in
[`../../../SPEC.md`](../../../SPEC.md), exposed as the `workflow` CLI.

## Requirements

- Node.js **16 or newer** (tested with Node 24).
- No external dependencies. Only the Node.js standard library is used
  (`fs`, `process`, the global `Buffer`).

## Build

There is no build step (interpreted language):

```bash
cd submissions/round-4/javascript
# nothing to build
```

## Run

```bash
# from submissions/round-4/javascript
node workflow.js run myworkflow.json

# read the document from stdin
node workflow.js run -
```

Examples:

```bash
node workflow.js run selftest/a_pipeline.json
echo '{"version":1,"steps":[]}' | node workflow.js run -
```

### CLI contract

- **stdout**: exactly one JSON response document (plus a trailing newline).
- **stderr**: unused for normal operation.
- Exit codes:
  - `0` — workflow completed (`status: "completed"`)
  - `1` — runtime workflow failure (`status: "failed"`)
  - `2` — input/validation error (`INVALID_WORKFLOW`, `UNKNOWN_DEPENDENCY`,
    `CYCLIC_DEPENDENCY`)
  - `3` — internal error

The response always has the shape:

```json
{
  "status": "completed",
  "results": {},
  "execution_order": [],
  "skipped": [],
  "error": null
}
```

## Self-test

Hand-written workflows live in `selftest/`; each `<case>.json` has a sibling
`<case>.expected.json` containing the expected response. `error.message` is
deliberately not compared (the spec declares it free-form).

```bash
cd submissions/round-4/javascript
python3 selftest/run.py
```

The runner executes every case through the real CLI, checks the exit code,
checks that stdout is exactly one JSON document, and compares the parsed
response. Python is used **only** by the self-test runner; the engine itself
needs only Node.js.

Cases cover:

| Case | Covers |
|---|---|
| `a_pipeline` | SPEC worked example A (`range` / `filter` / `sum`) |
| `b_skip` | SPEC worked example B (`when` false → skipped step) |
| `c_retry` | SPEC worked example C (`fail_until` + retry succeeds) |
| `d_math` | `range`, `sum`, `min`, `max`, `divide`, `mod`, `subtract`, `multiply`, `add` |
| `e_strings` | `concat`, case conversion, `trim`, `split`, `join`, `replace`, `length`, code points |
| `f_objects` | `keys`, `values`, `get`, `set`, `merge`, `pick`, `omit` |
| `g_arrays` | `map`, `filter`, `reduce`, `sort`, `reverse`, `unique`, `slice`, `flatten` |
| `h_conditions` | compound `all` / `any` / `not` conditions |
| `i_retry_exhausted` | retries exhausted → `STEP_FAILED`, partial results kept |
| `j_validation` | `1.0` version rejected → `INVALID_WORKFLOW`, exit 2 |

## Implementation notes

- **Custom JSON parser.** JSON integer literals are represented as `BigInt`,
  so `1` (a valid i64 parameter) is distinguishable from `1.0`/`1e3` (which
  are rejected with `INVALID_ARG`). This also preserves exact i64 values such
  as `9223372036854775807`.
- **i64 arithmetic.** All math uses `BigInt`; results are range-checked
  against `[-2^63, 2^63-1]`. Division truncates toward zero, `mod` takes the
  dividend's sign, and `INT64_MIN / -1` / `INT64_MIN % -1` report
  `ARITHMETIC_OVERFLOW`.
- **Objects** are represented by a `Map`-backed `JObj`, preserving insertion
  order, avoiding prototype-key pitfalls, and allowing deterministic
  serialization.
- **Strings** use Unicode code points for `length` and empty-separator
  `split`; case conversion and trimming are ASCII-only; ordering is by UTF-8
  bytes (`Buffer.compare`).
- **Structural equality** and **total ordering** follow SPEC §3, including
  mixed-type ordering and sorted-key object comparison.
- **Execution** is deterministic: ready steps are chosen by earliest position
  in the `steps` array; skipped steps count as finished; references may only
  target direct/transitive dependencies.
- **Retries** retry only `TRANSIENT_FAILURE` (from `fail_until`); when
  attempts are exhausted the final error code is `STEP_FAILED`. Other
  operation/argument/reference/condition errors are never retried.

## Dependency list

Runtime dependencies: **none**.

- Node.js standard library: `fs` (file/stdin input), global `Buffer` (UTF-8
  byte ordering), `process` (argv/exit code).
- Self-test only: Python 3 standard library (`json`, `subprocess`, `os`,
  `sys`).
