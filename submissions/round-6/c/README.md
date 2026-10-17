# `workflow` — Declarative Workflow Engine (C)

A complete implementation of the declarative workflow engine described in
`../../../SPEC.md`, written in C11 with no third-party dependencies.

## Build

```bash
cd submissions/round-6/c
make
```

This produces the `./workflow` executable. `make clean` removes the binary and
object files, `make test` builds and runs the self-tests.

## Run

```bash
./workflow run myworkflow.json
./workflow run -            # read the JSON document from stdin
```

- **stdout**: exactly one JSON document (nothing else).
- **stderr**: diagnostics only (not used in normal operation).
- **exit codes**: `0` completed, `1` runtime workflow failure,
  `2` validation/input error, `3` internal error.

Examples (see `selftest/` for the full set and `SPEC.md` §8 for the expected
outputs of the first three):

```bash
./workflow run selftest/01_pipeline.json     # range -> filter -> sum
./workflow run selftest/02_skip.json         # `when` false -> skipped step
./workflow run selftest/03_retry.json        # fail_until + retries
./workflow run selftest/09_runtime_error.json  # exit 1, DIVISION_BY_ZERO
./workflow run selftest/10_invalid_workflow.json  # exit 2, UNKNOWN_DEPENDENCY
```

## Self-test

```bash
cd submissions/round-6/c
make            # build first
./selftest/run.sh
```

`selftest/run.sh` executes the ten hand-written workflow documents in
`selftest/*.json` and then an exhaustive Python check-suite
(`selftest/run_tests.py`, 155 checks) that covers every operation, reference
form, condition form, retry behaviour, the error codes and the structural
validation rules. `python3` is only needed for the check-suite; the engine
itself does not depend on Python.

Hand-written workflows:

| File | Covers |
|---|---|
| `01_pipeline.json` | math pipeline: `range` → `filter` → `sum` (SPEC example A) |
| `02_skip.json` | `when` false → skipped step (SPEC example B) |
| `03_retry.json` | `fail_until` with `retry` (SPEC example C) |
| `04_strings.json` | string pipeline: concat/trim/upper/split/join/replace/length |
| `05_objects.json` | object ops: merge/pick/omit/get/set/keys/values |
| `06_refs.json` | dotted refs and array-index paths |
| `07_conditions.json` | `all` / `any` / `not` compound conditions |
| `08_arrays.json` | map/filter/reduce/sort/reverse/unique/flatten |
| `09_runtime_error.json` | runtime failure (exit 1) with partial results |
| `10_invalid_workflow.json` | structural failure (exit 2) |

## Dependencies

None beyond the C standard library (`libc`) and the C compiler. The JSON
parser/serializer (`json.c`, `json.h`) is part of this submission; there is no
vendored or external library, no workflow/graph library and no code
interpreter.

Build flags: `cc -O2 -std=gnu11 -Wall -Wextra` (see `Makefile`). `-std=gnu11` is
used for `__int128` and `__builtin_*_overflow`. GNU Make is used.

## Implementation notes

- **Values** (`json.h`/`json.c`): a small JSON tree that keeps both the exact
  numeric lexeme and its numeric value, so `1` and `1.0` are equal for
  equality/ordering while `1.0`/`1e3` are still rejected as numeric
  parameters (which require a JSON integer literal fitting in `i64`).
  Object keys are byte strings with explicit lengths.
- **Arithmetic**: signed 64-bit with `__builtin_{add,sub,mul}_overflow`;
  division truncates toward zero and `%` follows the dividend sign (C99).
  `INT64_MIN / -1` and `INT64_MIN % -1` report `ARITHMETIC_OVERFLOW`.
- **Validation** (`build_engine`): version, top-level/step field sets, id
  syntax and uniqueness, op names, `args` shape, `depends_on` syntax and
  existence (`UNKNOWN_DEPENDENCY`), cycle detection (`CYCLIC_DEPENDENCY`),
  `retry.max_attempts` range, and `when` shape all happen before any step
  executes.
- **Execution**: repeatedly picks the first not-yet-finished step in document
  order whose dependencies are all finished/skipped. Per-step attempt counters
  implement `fail_until`; only `TRANSIENT_FAILURE` is retried, and an
  exhausted transient failure is reported as `STEP_FAILED`.
- **References**: resolved recursively inside `args` and inside `when`
  operands; only a single-key object whose `$ref` value is a string counts as
  a reference. Targets must be transitive dependencies (reachability matrix),
  and skipped targets report `SKIPPED_REFERENCE`.
- **Condition evaluation**: comparison operators share the total-order
  comparator used by `compare`/`sort`; `all`/`any`/`not` compose conditions.
  A non-boolean result (e.g. a raw number or a reference to a non-boolean)
  reports `INVALID_CONDITION`.
- **JSON output**: compact, deterministic, one line, with an inserted trailing
  newline. No timestamps, randomness, network or filesystem side effects.
