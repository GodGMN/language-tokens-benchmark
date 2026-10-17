# Declarative Workflow Engine — Python implementation

This directory contains a complete implementation of the workflow contract in
`../../../SPEC.md`, using only the Python standard library.

## Requirements

- Python 3.8 or newer (`python3` must be on `PATH`).
- No third-party dependencies, no build step, no network access.

## Build

Interpreted language — there is no build step.

## Run

From this directory (`submissions/round-4/python`):

```bash
python3 workflow.py run myworkflow.json
python3 workflow.py run -
```

The second form reads the workflow document from stdin.  On success the process
exits with `0`; runtime workflow failures use `1`, validation/input errors use
`2`, and internal errors use `3`.  Stdout always contains exactly one JSON
document and nothing else.

## Self-test

The `selftest/` folder contains nine hand-written workflows with expected JSON
responses.  Run them all with:

```bash
python3 selftest/run_selftest.py
```

The runner checks the exit code, `status`, `results`, `execution_order`,
`skipped`, `error.code` and `error.step` (the free-form error message is
ignored, as in the external contract).  Expected: `9/9 self-tests passed`.

Cases covered:

- math pipeline (`range`, `filter`, `map`, `sum`, `subtract`, `divide`, `mod`)
- string pipeline (`trim`, case conversion, `replace`, `split`, `join`,
  `length`, Unicode code-point splitting)
- object operations (`keys`, `values`, `get`, `set`, `merge`, `pick`, `omit`)
- skipped step via `when`
- retries with `fail_until`
- arrays and mixed-type total ordering (`sort`, `unique`, `reverse`, `slice`,
  `flatten`, `reduce`)
- compound conditions (`all`, `any`, `not`, structural/numeric equality, mixed
  type ordering)
- arithmetic overflow response
- structural validation / unknown dependency response

## Dependency list

Runtime: Python standard library only.

- `json` — parsing/serialising JSON
- `math` — rejecting non-finite JSON numbers
- `re` — step-id and array-index validation
- `sys` — CLI arguments and stream I/O
- `functools.cmp_to_key` — stable ordering by the SPEC total order

The self-test runner additionally uses `pathlib` and `subprocess` from the
standard library.

## Implementation notes

- All math is performed on signed 64-bit integers with explicit overflow
  checks; division truncates toward zero and `mod` follows the dividend's sign.
- Numeric operation arguments must be JSON integer literals (not `1.0`/`1e3`)
  and must fit in i64.
- References support dotted object paths and non-negative array indices, and
  are only allowed to direct/transitive dependencies.
- Execution is deterministic: ready steps are selected by their original order
  in the `steps` array.
- Skipped steps still appear in `execution_order` and map to `null` in
  `results`.
- Only `TRANSIENT_FAILURE` from `fail_until` is retried; exhausting retries
  produces `STEP_FAILED`.
