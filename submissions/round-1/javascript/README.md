# workflow — JavaScript implementation

Declarative workflow engine implementing [`../SPEC.md`](../SPEC.md). Node.js,
standard library only — **no dependencies, no build step, no `eval`**.

## Requirements

- Node.js 18+ (tested with Node 24). Uses `BigInt`, `String.prototype.replaceAll`
  and `Buffer.compare`, all of which are long-standing stdlib features.

## Build

There is no build step (interpreted language). Nothing to install.

## Run

From this `cli-javascript/` directory:

```bash
node workflow.js run myworkflow.json
node workflow.js run - < myworkflow.json
```

The `run` keyword is optional, so this also works:

```bash
node workflow.js myworkflow.json
cat myworkflow.json | node workflow.js -
```

A thin launcher is provided as well:

```bash
./workflow run myworkflow.json
```

- **stdout**: exactly one JSON document (the response), nothing else.
- **stderr**: unused on normal operation.
- **exit codes**: `0` completed, `1` runtime workflow failure, `2`
  input/argument/validation error, `3` internal error.

## Self-test

```bash
node selftest/run.js
```

The runner executes every `selftest/*.json` workflow through the CLI, checks the
exit code, and compares the full response (`status`, `results`,
`execution_order`, `skipped`, `error.code`, `error.step`). It also checks stdin
mode and the missing-argument exit code. The suite covers:

| File | Covers |
|---|---|
| `math.json` | `range`, `add`, `subtract`, `multiply`, `divide`, `mod`, `sum`, `min`, `max`, `reduce` |
| `strings.json` | `split`, `join`, `replace`, `trim`, `length`, `concat`, `uppercase`, `lowercase` (incl. empty separator) |
| `objects.json` | `constant`, `get`, `set`, `keys`, `values`, `merge`, `pick`, `omit` |
| `arrays.json` | `sort` (mixed types), `unique`, `reverse`, `slice`, `flatten`, `map`, `filter`, `type_is` |
| `skipped.json` | `when` with `gt`/`all`/`any`/`not`, skipped results and order |
| `retry.json` | `fail_until` retried successfully (SPEC example C) |
| `error_overflow.json` | `ARITHMETIC_OVERFLOW` |
| `error_divzero.json` | `DIVISION_BY_ZERO` |
| `error_empty.json` | `EMPTY_ARRAY` |
| `error_skipped_ref.json` | `SKIPPED_REFERENCE` |
| `error_condition.json` | `INVALID_CONDITION` |
| `error_invalid_arg.json` | `INVALID_ARG` |
| `invalid_workflow.json` | `INVALID_WORKFLOW` (unknown field) |
| `unknown_dependency.json` | `UNKNOWN_DEPENDENCY` |
| `cycle.json` | `CYCLIC_DEPENDENCY` |

The three worked examples from SPEC §8 are covered by `math.json`/`arrays.json`
(pipeline), `skipped.json` (skipped step) and `retry.json` (retry) and were also
verified manually.

## Implementation notes

- **Custom JSON parser** (`parseJSONText`) keeps integer literals as `BigInt`, so
  i64 values near `±2^63` are parsed and emitted exactly (no `Number` rounding).
  Fractional/exponent literals become `number`; math parameters must be integral
  i64 or the op fails with `INVALID_ARG`.
- **i64 arithmetic** is done with `BigInt` plus explicit range checks. Division
  truncates toward zero (`BigInt` semantics); `%` takes the dividend's sign.
  Results outside `[-2^63, 2^63-1]` raise `ARITHMETIC_OVERFLOW`.
- **Custom serializer** emits `BigInt` as a JSON number and is used for stdout
  and for `map`'s `stringify` transform (compact JSON).
- **Execution** is topological with the required tie-break: among ready steps,
  the earliest in the `steps` array runs first. Conditions are evaluated before
  argument resolution; skipped steps store `null` and are recorded in both
  `execution_order` and `skipped`.
- **References** resolve recursively inside `args` and condition operands, may
  use dotted paths and array indices, and are restricted to transitive
  dependencies. References to skipped steps raise `SKIPPED_REFERENCE`.
- **Retries**: only `TRANSIENT_FAILURE` (from `fail_until`) is retried, up to
  `retry.max_attempts`. If attempts run out the final code is `STEP_FAILED`.

## Dependency list

None. Only the Node.js standard library (`fs`).
