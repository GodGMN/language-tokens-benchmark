# `workflow` — Declarative Workflow Engine (JavaScript)

A CLI implementation of the declarative workflow engine specified in
[`../../../SPEC.md`](../../../SPEC.md): JSON workflow documents in, one JSON
response document out.

## Build

There is **no build step** — this is plain Node.js. No `npm install` is
required, no `package.json`, no external dependencies.

Requirements: Node.js **v14 or newer** (uses `BigInt`, which arrived in v10.4;
`Object.create(null)` and spread syntax are used throughout). Tested on Node 24.

## Run

From this folder (`submissions/round-3/javascript`):

```bash
node workflow.js run myworkflow.json     # run a workflow file
node workflow.js run -                   # read the workflow document from stdin
```

- **stdout**: exactly one JSON document (the response), plus a trailing newline.
- **stderr**: unused (free for diagnostics).
- **Exit codes**:
  - `0` — workflow completed (`status: "completed"`)
  - `1` — runtime workflow failure (`status: "failed"`)
  - `2` — input/argument/validation error (`status: "failed"`)
  - `3` — internal program error (best-effort JSON response)

Example:

```bash
$ echo '{"version":1,"steps":[{"id":"a","op":"add","args":{"a":1,"b":2}}]}' \
    | node workflow.js run -
{"status":"completed","results":{"a":3},"execution_order":["a"],"skipped":[],"error":null}
```

## Self-test

The `selftest/` folder contains hand-written workflow documents plus a driver
that runs each one through the CLI and compares the parsed response **and** the
exit code against the expected result.

```bash
cd submissions/round-3/javascript
node selftest/check.js        # or: sh selftest/run.sh
```

Expected output ends with `All self-tests passed`.

Cases covered:

| File | Covers |
|---|---|
| `01_math_pipeline.json` | `range`, `sum`, `divide`, `mod`, `multiply`, `min`, `max`, `subtract`; multi-step refs |
| `02_string_pipeline.json` | `trim`, `lowercase`, `replace`, `split`, `join`, `uppercase`, `length`, `concat` |
| `03_object_ops.json` | `keys`, `values`, `set`, `merge`, `get` (present/missing), `omit`, `pick` |
| `04_skipped_step.json` | `when` false → step skipped, `null` result (SPEC §8 example B) |
| `05_retry.json` | `fail_until` retried to success (SPEC §8 example C) |
| `06_arrays.json` | `sort`, `reverse`, `unique`, `flatten`, `slice`, `map`, `filter`, `reduce` |
| `07_references.json` | dotted `$ref` paths and array-index `$ref` segments |
| `08_conditions.json` | compound conditions (`all`/`any`/`not`), skipping |
| `09_error_overflow.json` | i64 overflow → `ARITHMETIC_OVERFLOW`, exit 1, failed step excluded from results/order |
| `10_retry_exhausted.json` | retries exhausted → `STEP_FAILED`, exit 1 |
| stdin check in `check.js` | SPEC §8 example A delivered via `run -` |

You can also run a single case directly, e.g.:

```bash
node workflow.js run selftest/01_math_pipeline.json
```

## Dependencies

**None.** Only the Node.js standard library is used (`fs`). The JSON parser and
serializer are implemented in `workflow.js` (see below). No workflow/graph
libraries, no `eval`, no user code execution, no filesystem writes, no network.

## Implementation notes

- **Custom JSON parser.** A stock `JSON.parse` would erase the distinction
  between `1` and `1.0`/`1e3`, which the SPEC requires for numeric parameters
  (`INVALID_ARG`), and would lose precision for integers beyond 2^53. The
  parser keeps every number's original literal (`Num`), and the serializer emits
  integers as exact decimal text.
- **i64 arithmetic.** Operations compute with `BigInt`, range-check every
  `+ - *` and unary negation/abs against `[-2^63, 2^63-1]`, and special-case
  `INT64_MIN / -1` and `INT64_MIN % -1` as `ARITHMETIC_OVERFLOW`. Division and
  modulo use BigInt semantics, which match the SPEC (truncation toward zero,
  sign of the dividend) — C99 style.
- **References** are resolved recursively through the whole `args` tree (and
  `when` operands), restricted to the step's direct/transitive `depends_on`
  closure. Array index segments must be non-negative decimal integers.
- **Conditions** support `eq/ne/gt/gte/lt/lte/all/any/not`; a non-boolean result
  is `INVALID_CONDITION`. Unknown condition op names are also
  `INVALID_CONDITION` at runtime.
- **Execution** is a topological walk with the SPEC tie-break: among ready steps
  (all dependencies finished or skipped), pick the earliest in the `steps`
  array. `when` is evaluated before argument resolution. Skipped steps appear in
  `results` (as `null`), `execution_order` and `skipped`.
- **Retries** only retry `TRANSIENT_FAILURE` (from `fail_until`, whose attempt
  counter is per step per execution of the workflow); everything else fails
  immediately. Exhausting retries reports `STEP_FAILED` for that step.
- **Validation** happens in phases: document shape/version/fields, per-step
  fields and ids, unknown dependencies, then cycle detection — with
  `error.step` attributed exactly as SPEC §7 requires.
- **Determinism.** No randomness, timestamps or wall-clock dependence; string
  and key ordering uses UTF-8 byte comparison (`Buffer.compare`), arrays are
  sorted stably.
