# Declarative Workflow Engine — JavaScript implementation

Implements the `workflow` CLI described in `../../../SPEC.md`.

## Requirements

- Node.js 18 or newer (uses `BigInt` for exact signed 64-bit arithmetic).
- No third-party dependencies. Only Node's built-in `fs` module is used.
- No build step.

## Build

None (interpreted language).

```bash
cd submissions/round-2/javascript
# nothing to build
```

## Run

```bash
cd submissions/round-2/javascript

# from a file
node workflow.js run myworkflow.json

# from stdin
node workflow.js run - < myworkflow.json
```

There is also a convenience wrapper (same behaviour, no build step):

```bash
./workflow run myworkflow.json
```

### Output contract

- stdout: exactly one JSON document (plus a trailing newline).
- stderr: diagnostics only.
- Exit codes: `0` completed, `1` runtime workflow failure, `2` input/validation
  error, `3` internal error.

## Self-test

Hand-written workflows live in `selftest/`. The runner executes each one through
the real CLI and compares the parsed response (and exit code) with hand-checked
expectations. Object key order is ignored; array order is significant.

```bash
cd submissions/round-2/javascript
node selftest/run.js
```

Cases cover:

| File | Covers |
|---|---|
| `example_a.json` / `example_b.json` / `example_c.json` | the three SPEC §8 worked examples |
| `math_pipeline.json` | `range`, `map`, `filter`, `sum`, `min`, `max`, `reduce` |
| `string_pipeline.json` | `trim`, ASCII case ops, `split`, `join`, `replace`, `length`, `concat` |
| `object_ops.json` | `set`, `get`, `keys`, `values`, `merge`, `pick`, `omit` |
| `skipped_step.json` | false `when`, null result, `skipped` list |
| `retry_transient.json` | `fail_until` retried to success |
| `retry_exhausted.json` | retries exhausted -> `STEP_FAILED`, exit 1 |
| `references.json` | nested/array-index/transitive `$ref` resolution |
| `errors_overflow.json` | i64 `ARITHMETIC_OVERFLOW`, partial results |
| `conditions_compound.json` | `all` / `any` / `not` conditions |
| `validation_cycle.json` | `CYCLIC_DEPENDENCY`, exit 2 |

You can also run any file directly, e.g.:

```bash
node workflow.js run selftest/math_pipeline.json
```

## Implementation notes

- **Exact numbers.** A strict JSON parser preserves whether a number was written
  as an integer literal (stored as `BigInt`) or with a fraction/exponent
  (stored as a JS number). Integer parameters must be i64 integer literals;
  `1.0` / `1e3` are rejected with `INVALID_ARG` (and `"version": 1.0` with
  `INVALID_WORKFLOW`).
- **i64 arithmetic.** All math is done with `BigInt`, with explicit range checks.
  Division truncates toward zero; modulo takes the dividend's sign;
  `INT64_MIN / -1` and `INT64_MIN % -1` are `ARITHMETIC_OVERFLOW`.
- **Determinism.** No randomness, timestamps or wall-clock use. Steps run in a
  topological order with tie-break "earliest in the `steps` array".
- **References.** `$ref` objects resolve recursively through `args` and
  conditions. Targets must be direct/transitive dependencies; skipped targets
  produce `SKIPPED_REFERENCE`.
- **Conditions.** `all` / `any` short-circuit; comparison uses structural
  equality for `eq`/`ne` and the SPEC total order for `gt`/`gte`/`lt`/`lte`.
- **Strings.** Ordering and key sorting use UTF-8 byte comparison; `length` and
  empty-separator `split` count Unicode code points; case conversion is ASCII
  only.
- **Objects.** Object values are represented with null-prototype objects, so
  keys such as `__proto__` are treated as ordinary data.
- **Retries.** Only `TRANSIENT_FAILURE` (from `fail_until`) is retried. When
  attempts are exhausted the final error code is `STEP_FAILED`.

## Dependency list

None beyond the Node.js standard library (`fs`, plus language built-ins such as
`BigInt`). No workflow/DAG/eval libraries are used; operations are a fixed,
closed dispatch table.
