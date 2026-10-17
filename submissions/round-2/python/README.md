# `workflow` CLI — Python implementation

A declarative workflow engine implementing `SPEC.md`. Pure Python 3, standard
library only, no build step.

## Build

Nothing to build (interpreted language):

```bash
cd submissions/round-2/python
python3 --version    # Python 3.8+ is sufficient
```

## Run

```bash
cd submissions/round-2/python

python3 workflow.py run myworkflow.json     # from a file
python3 workflow.py run -                   # read the JSON document from stdin
```

An equivalent executable wrapper is provided (`./workflow run <file>`), but the
canonical invocation is the `python3 workflow.py ...` form above.

- **stdout**: exactly one JSON response document (plus a trailing newline).
- **stderr**: unused except for Python's own diagnostics; never required.
- Exit codes: `0` completed, `1` runtime workflow failure, `2` input/validation
  error, `3` internal error.

## Self-test

```bash
cd submissions/round-2/python

python3 selftest/check.py     # compares every case with selftest/expected/*.json
sh selftest/run.sh            # runs every case and prints the raw JSON + exit code
```

The `selftest/` folder contains hand-written workflows covering:

| file | covers |
|---|---|
| `01_math_pipeline.json` | `range`/`map`/`sum`/`divide`/`mod`, refs and dotted refs |
| `02_string_pipeline.json` | `trim`/`lowercase`/`split`/`join`/`replace`/`uppercase`/`length` |
| `03_object_ops.json` | `set`/`keys`/`values`/`pick`/`omit`/`merge`/`get`, sorted keys |
| `04_skipped_step.json` | `when` (single and compound) and the `skipped` array |
| `05_retry.json` | `fail_until` + `retry.max_attempts` |
| `06_arrays.json` | `sort`/`flatten`/`unique`/`filter`/`slice`/`reduce`/`reverse` |
| `07_utilities.json` | `equals`/`compare`/`type`/`and`/`not`, numeric equality |
| `08_error_skipped_ref.json` | `SKIPPED_REFERENCE` (exit 1, partial results) |
| `09_error_overflow.json` | `ARITHMETIC_OVERFLOW` (exit 1, partial results) |

## Dependencies

None beyond the Python standard library (`json`, `re`, `sys`, `functools`).
There is no third-party package, no lockfile, and no network or filesystem
access apart from reading the input document.

## Implementation notes

- **Numbers.** JSON number literals containing `.` or an exponent are parsed
  into a marker subclass of `float`, so numeric operation parameters can be
  required to be *integer literals* (`1.0` and `1e3` are `INVALID_ARG`, as
  required by SPEC §3). Arithmetic is performed on Python integers with
  explicit i64 range checks (`ARITHMETIC_OVERFLOW`), C99 truncating division
  and dividend-signed modulo, and the `INT64_MIN / -1` traps handled.
- **Structural equality / ordering.** A hand-written structural equality and a
  deterministic total order (number < string < boolean < null < array <
  object) are used by `equals`, `compare`, `unique`, `sort`, `when` and
  `filter`. Object keys are ordered byte-wise on their UTF-8 encoding.
- **References.** `{"$ref": "step.path.0"}` objects are resolved recursively
  anywhere inside `args` and `when`; targets must be transitive dependencies of
  the referencing step (checked at runtime, per SPEC §6). References to
  skipped steps raise `SKIPPED_REFERENCE`.
- **Execution.** Kahn's algorithm validates the dependency graph; execution
  repeatedly picks the earliest step in the `steps` array whose dependencies
  have all finished (completed or skipped).
- **Retries.** Only `TRANSIENT_FAILURE` (from `fail_until`) is retried; when
  attempts are exhausted the reported code is `STEP_FAILED`. `fail_until` keeps
  a per-step attempt counter that starts at zero for every workflow run.
- **Determinism.** No randomness, timestamps or environment dependence; output
  JSON is compact (`separators=(",", ":")`) and ASCII-escaped.

### Interpretation choices where the SPEC is terse

- `when` must be a condition object (shape checked before execution →
  `INVALID_WORKFLOW`). An *unknown condition operator* is evaluated at runtime
  and reported as `INVALID_CONDITION`, which is also used when a (nested)
  condition does not produce a strict boolean.
- `retry: {}` is accepted and means `max_attempts: 1`; `max_attempts` outside
  `1..5` or a non-integer is `INVALID_WORKFLOW`.
- `filter.predicate` `type_is` compares against the type name carried in
  `value` (also accepted under `type_name`/`name`/`target`), because the SPEC's
  literal `{"type": "type_is", "type": "<type>"}` would repeat the
  discriminator key.
- `map.transform` accepts both the generic `{"type": "op", "op": ..., "args":
  ...}` form (with `$self` standing for the element) and the fixed
  `constant`/`add`/`subtract`/`multiply`/`divide`/`mod`/`negate`/`abs`/
  `stringify`/`type` forms.
- Exponents that overflow IEEE-754 (e.g. `1e400`) are rejected as invalid JSON
  input rather than producing a non-finite value.

## Layout

```
submissions/round-2/python/
├── workflow.py            # the whole engine + CLI (stdlib only)
├── workflow               # optional executable wrapper
├── README.md              # this file
└── selftest/
    ├── check.py           # automated comparison against expected outputs
    ├── run.sh             # run every case, print raw responses
    ├── 0N_*.json          # hand-written workflows
    └── expected/0N_*.json # expected responses (checked by check.py)
```
