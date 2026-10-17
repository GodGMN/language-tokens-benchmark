# `workflow` — declarative workflow engine (JavaScript / Node.js)

A black-box-conformant implementation of the declarative workflow engine
described in [`SPEC.md`](../../../SPEC.md). It reads a workflow JSON document,
executes its fixed operation set, and prints exactly one JSON response on
stdout.

## Requirements & dependencies

- **Node.js 18 or newer** (tested on Node 24). No build step, no `npm install`.
- **Dependencies: none.** Only the Node.js standard library (`fs`,
  `path` in the self-test runner, `child_process` in the self-test runner).
- The implementation is a single file: `workflow.js`.

## Build

No build step is required (interpreted language).

```bash
cd submissions/round-7/javascript
# nothing to build
```

## Run

```bash
# read the workflow document from a file
node workflow.js run myworkflow.json

# read the workflow document from stdin
node workflow.js run -
```

The executable bit is set, so `./workflow.js run myworkflow.json` also works.

Behavior:

- stdout: exactly one JSON document (response), nothing else.
- stderr: diagnostics only (unused in normal operation).
- Exit codes: `0` completed, `1` runtime workflow failure, `2` input /
  validation error, `3` internal error.

Response shape:

```json
{
  "status": "completed",
  "results": { "step_id": 42 },
  "execution_order": ["step_id"],
  "skipped": [],
  "error": null
}
```

## Self-test

Hand-written workflows live in `selftest/workflows/` with expected snapshots in
`selftest/expected/`. The runner executes every fixture twice — once via a file
path and once via `run -` (stdin) — and compares status, results,
execution order, skipped list and `error.code` / `error.step`.

```bash
cd submissions/round-7/javascript
node selftest/run.js
```

Expected output ends with `11/11 self-tests passed` and exit code `0`.

The fixtures cover:

| Fixture | Covers |
|---|---|
| `01_math_pipeline` | `range`, `filter`, `sum`, `multiply`, `divide`, `mod`, `subtract`, `min`, `max`, empty range |
| `02_string_pipeline` | `trim`, `uppercase`, `lowercase`, `replace`, `split` (incl. empty separator), `join`, `concat`, `length` (code points) |
| `03_array_ops` | `map`, `filter`, `reduce` (with/without `initial`), `sort` (mixed types), `reverse`, `unique`, `slice`, `flatten` |
| `04_object_ops` | `get`, `set`, `keys`, `values`, `merge`, `pick`, `omit` (sorted keys) |
| `05_skipped_when` | `when` false → null result + `skipped`, compound `all` / `any` / `not` conditions |
| `06_retry_fail_until` | `fail_until` retried until success, default retry, dependent step |
| `07_nested_refs` | dotted paths, array indices, refs nested in literal arrays |
| `08_utilities` | `type`, `equals` (1 == 1.0, structural objects), `compare` (mixed types), `and`/`or`/`not` JSON truthiness, `constant` null |
| `09_error_overflow` | i64 overflow, exit 1, failed step excluded from results/order |
| `10_validation_cycle` | `CYCLIC_DEPENDENCY`, exit 2, empty results |
| `11_skipped_reference` | `SKIPPED_REFERENCE`, exit 1 |

## Implementation notes

- **Exact i64 arithmetic.** All numbers are represented internally as `BigInt`.
  A custom JSON parser distinguishes integer literals (parsed as `BigInt`) from
  fraction/exponent literals (parsed as doubles). Integer args are range-checked
  against `[-2^63, 2^63-1]`; arithmetic is overflow-checked; division truncates
  toward zero; `%` takes the dividend's sign; `INT64_MIN / -1` and
  `INT64_MIN % -1` report `ARITHMETIC_OVERFLOW`. Float literals used where an
  i64 argument is required (e.g. `{"a": 1.0}`) are `INVALID_ARG`.
- **Validation before execution.** Version, ids, unique ids, field shapes,
  unknown fields, op names, retry range, `when` shape, dependency existence and
  cycles are all checked structurally. `INVALID_WORKFLOW`, `UNKNOWN_DEPENDENCY`
  and `CYCLIC_DEPENDENCY` exit with code 2 before any step runs.
- **Deterministic scheduling.** At each point the earliest step in the `steps`
  array whose dependencies have all finished (completed or skipped) is executed.
- **References.** `$ref` objects (exactly one key, string value) resolve
  recursively inside args and condition operands. A target must be a direct or
  transitive dependency; skipped targets raise `SKIPPED_REFERENCE`; bad targets,
  self references and bad paths raise `INVALID_REFERENCE`.
- **Conditions / ordering.** Structural equality treats `1 == 1.0`; the total
  order used by `sort`/`compare`/relational conditions is
  `number < string < boolean < null < array < object`, with UTF-8 byte order for
  strings and sorted-key comparison for objects. `sort` is stable.
- **Retries.** Only `TRANSIENT_FAILURE` (from `fail_until`) is retried, up to
  `retry.max_attempts` (1..5, default 1). When attempts are exhausted the
  response uses `STEP_FAILED`. All other errors fail the step immediately.
- **No user code, no I/O.** The operation set is fixed; there is no `eval`, no
  network access and no filesystem access beyond reading the input document.
