# `workflow` — Declarative Workflow Engine (Python)

A complete implementation of the declarative workflow engine described in
[`../../../SPEC.md`](../../../SPEC.md).  The CLI reads a workflow JSON document,
executes its steps deterministically, and prints exactly one JSON response to
stdout.

## Requirements

- Python 3.8+ (developed and tested with Python 3.12)
- No third-party packages, no build step.

Dependency list: **Python standard library only** (`json`, `re`, `sys`,
`decimal`, `functools`).

## Build

Interpreted language — there is no build step.

```bash
cd submissions/round-5/python
python3 -m py_compile workflow.py   # optional syntax check
```

## Run

```bash
cd submissions/round-5/python

# from a file
python3 workflow.py run myworkflow.json

# from stdin
python3 workflow.py run - < myworkflow.json
```

Stdout is always exactly one JSON document.  Diagnostics, if any, go to
stderr.  Exit codes:

| Code | Meaning |
|---|---|
| 0 | workflow completed |
| 1 | runtime workflow failure |
| 2 | validation / input / argument error |
| 3 | internal error |

## Self-test

Ten hand-written workflows live in `selftest/`, covering:

1. `01_math.json` — range/sum/min/max and all binary arithmetic ops
2. `02_strings.json` — trim/case/replace/split/join/length/concat
3. `03_objects.json` — get/set/keys/values/merge/pick/omit
4. `04_skipped.json` — `when` conditions (comparison + `all` + `not`)
5. `05_retry.json` — `fail_until` retried up to `max_attempts`
6. `06_arrays.json` — map/filter/reduce/sort/reverse/unique/slice/flatten/stringify
7. `07_refs.json` — dotted `$ref` paths into objects and arrays
8. `08_error_divzero.json` — `DIVISION_BY_ZERO` runtime failure
9. `09_error_validation.json` — `INVALID_WORKFLOW` (bad version)
10. `10_error_skipped_ref.json` — `SKIPPED_REFERENCE`

Run them all (file input plus one stdin invocation):

```bash
cd submissions/round-5/python
python3 selftest/run_selftest.py
```

Expected output ends with `all self-tests passed`.

You can also inspect any single response directly:

```bash
python3 workflow.py run selftest/06_arrays.json
```

## Implementation notes

- All math is signed 64-bit integer math with explicit overflow checks;
  division truncates toward zero and modulo follows the dividend's sign.
- JSON float literals (`1.0`, `1e3`) are preserved as `Decimal` values so that
  they can appear inside arbitrary JSON data, but they are rejected whenever an
  operation requires an i64 integer parameter (`INVALID_ARG`).
- References (`{"$ref": "step.path.0"}`) are resolved recursively anywhere in
  `args` (and in condition operands) and may only target direct or transitive
  `depends_on` ancestors.
- Step scheduling is a deterministic topological order: among ready steps, the
  one appearing earliest in the document's `steps` array runs next.
- Only `TRANSIENT_FAILURE` from `fail_until` is retried; exhausting retries
  produces `STEP_FAILED`.  All other operation errors are final on the first
  attempt.
- `stringify` emits compact JSON with no spaces and preserves non-ASCII UTF-8.

## Files

- `workflow.py` — the complete CLI implementation
- `selftest/` — workflow fixtures and the self-test runner
