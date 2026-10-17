# Declarative Workflow Engine — Python implementation

A CLI that executes the declarative JSON workflow format described in
`../../../SPEC.md`.

## Requirements / dependencies

- Python 3.7 or newer (tested with Python 3.11).
- **No external packages.** Only the Python standard library is used:
  `json`, `sys`, `math`, `re`, and `functools.cmp_to_key`. There is no
  package manager/lockfile because there are no third-party dependencies.

## Build

This is an interpreted program, so there is no build step.

## Run

From `submissions/round-7/python`:

```bash
# read a workflow from a file
python3 workflow.py run myworkflow.json

# read a workflow from stdin
python3 workflow.py run -
```

The command prints exactly one JSON response document to stdout. Diagnostics
(if any) would go to stderr. Exit codes:

| Code | Meaning |
|---:|---|
| 0 | workflow completed |
| 1 | runtime workflow failure (`status: "failed"`) |
| 2 | validation / input / argument error (`status: "failed"`) |
| 3 | internal program error |

Example:

```bash
$ python3 workflow.py run selftest/math_pipeline.json
{"status":"completed","results":{...},"execution_order":[...],"skipped":[],"error":null}
```

## Self-test

The `selftest/` directory contains 10 hand-written workflows and a runner that
executes each one through the real CLI and compares the complete response
(status, results, execution order, skipped list, error code and error step)
and exit code:

```bash
python3 selftest/run_selftest.py
```

Expected output:

```
ok   arrays.json
ok   conditions.json
...
all 10 self-tests passed
```

The workflows cover:

- `math_pipeline.json` — range/map/sum/divide pipeline.
- `string_pipeline.json` — trim/lowercase/split/join/uppercase/length.
- `objects.json` — merge/pick/omit/keys/values and sorting.
- `skipped_step.json` — a `when` condition that skips a step; a later step
  still runs because its dependency was skipped (not referencing it).
- `retry.json` — `fail_until` succeeds on the third attempt.
- `references.json` — dotted object paths and array indices.
- `conditions.json` — `all` / `any` / `not` compound conditions.
- `arrays.json` — sort/unique/reverse/slice/flatten/reduce.
- `error_overflow.json` — i64 overflow (exit 1, partial results).
- `validation_cycle.json` — dependency cycle (exit 2).

## Implementation notes

- `workflow.py` is a single self-contained file.
- Structural validation (version, ids, uniqueness, field shapes, unknown
  fields, op names, dependency existence, cycles, retry range, condition
  shapes) runs completely before any step executes.
- Topological execution picks the earliest step in the `steps` array among
  those whose dependencies are all finished/skipped.
- `$ref` resolution is recursive inside `args` and condition operands; it
  enforces the direct/transitive dependency rule, path/index validation and
  `SKIPPED_REFERENCE`.
- All integer arithmetic uses Python integers but is range-checked against
  signed 64-bit bounds on every operation. Division truncates toward zero and
  modulo takes the dividend's sign.
- JSON values are compared with the SPEC's structural equality and total
  ordering (including UTF-8 byte ordering for strings and object keys), and
  `sort` is stable.
- Only `fail_until` produces `TRANSIENT_FAILURE`; retries are limited to that
  code, and exhausting attempts yields `STEP_FAILED`. Every other operation
  error is reported as-is without retrying.
