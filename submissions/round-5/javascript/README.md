# `workflow` — Declarative Workflow Engine (JavaScript)

A dependency-free Node.js implementation of the workflow engine described in
`../../../SPEC.md`.

## Requirements

- Node.js with `BigInt` support (Node 10.4 or newer; tested with Node 24).
- No third-party packages and no build step.

## Run

From this directory (`submissions/round-5/javascript`):

```bash
node workflow.js run myworkflow.json
node workflow.js run -          # read the workflow JSON from stdin
```

Exit codes follow the SPEC:

| Code | Meaning |
|---|---|
| 0 | workflow completed |
| 1 | runtime workflow failure |
| 2 | input/validation error (`INVALID_WORKFLOW`, `UNKNOWN_DEPENDENCY`, `CYCLIC_DEPENDENCY`) |
| 3 | internal error |

stdout contains exactly one JSON response document; diagnostics are not printed
to stdout.

## Self-test

Run all hand-written fixtures:

```bash
bash selftest/run.sh
```

or individually:

```bash
for f in selftest/*.json; do node workflow.js run "$f"; done
```

The fixtures cover:

| File | Covers |
|---|---|
| `selftest/a_math_pipeline.json` | range → filter → sum (SPEC worked example A) |
| `selftest/b_skipped.json` | `when` false → skipped step (worked example B) |
| `selftest/c_retry.json` | `fail_until` + successful retry (worked example C) |
| `selftest/d_string_pipeline.json` | trim/split/join/replace/length/uppercase/lowercase/concat |
| `selftest/e_object_ops.json` | set/get/keys/values/merge/pick/omit |
| `selftest/f_array_ops.json` | map/filter/reduce/sort/reverse/unique/slice/flatten/min/max/sum and math |
| `selftest/g_utilities.json` | equals/compare/and/or/not/type |
| `selftest/h_nested_refs.json` | dotted and array-index `$ref` paths |
| `selftest/i_compound_conditions.json` | `all` / `any` / `not` conditions |
| `selftest/j_retry_exhausted.json` | exhausted retries → `STEP_FAILED`, exit 1 |

## Implementation notes

- **Exact i64 arithmetic.** JSON integer literals are parsed locally into
  `BigInt`, so the full `[-2^63, 2^63-1]` range is handled exactly. Integer
  literals are distinguished from float/exponent literals (`1` vs `1.0`),
  which is required for `version` and numeric-argument validation. Arithmetic
  overflow is detected with `BigInt` and reported as `ARITHMETIC_OVERFLOW`.
- **Deterministic.** No randomness, timestamps, network or filesystem writes.
  The topological scheduler always picks the earliest ready step in document
  order.
- **References** are resolved recursively inside `args` and inside condition
  `left`/`right`; only objects whose sole key is a string `$ref` are treated as
  references. Target/dependency/skipped checks follow SPEC §6.
- **Ordering/equality** implement the SPEC's structural equality and total
  order (including UTF-8 byte ordering for strings and object keys).
- **`type_is` predicate**: the target type may be supplied in any extra string
  field (`value`, `target`, `type_name`, ...), and the SPEC's literal example
  with a duplicated `type` key is also recovered.

## Dependencies

None (Node.js standard library only: `fs`).
