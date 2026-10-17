# workflow — C implementation

A complete implementation of the declarative workflow engine described in
[`../SPEC.md`](../SPEC.md), written in C11.

## Build

Requires a C11 compiler (`cc`/`gcc`/`clang`) and `make`. There are **no
third-party dependencies**: JSON parsing/serialization is implemented in
`src/json.c`; the engine only uses the C standard library (plus `libm`, which is
part of the C runtime).

```bash
cd cli-c
make
```

This produces the executable `./workflow`.

## Run

```bash
./workflow run myworkflow.json
./workflow run -              # read the workflow document from stdin
```

* stdout: exactly one JSON document (the response), nothing else.
* stderr: unused.
* Exit codes: `0` completed, `1` runtime workflow failure, `2`
  input/validation error, `3` internal error.

Example:

```bash
$ ./workflow run selftest/ex_a.json
{"status":"completed","results":{"n":[1,2,3,4,5],"evens":[2,4],"total":6},"execution_order":["n","evens","total"],"skipped":[],"error":null}
```

## Self-tests

```bash
make selftest
# or
sh selftest/run.sh
```

`selftest/run.sh` runs every workflow in `selftest/` plus a set of inline cases
and compares the **complete JSON response and the exit code** against the
expected values. It covers:

| File / case | Covers |
|---|---|
| `selftest/ex_a.json`, `ex_b.json`, `ex_c.json` | the three worked examples from SPEC §8 |
| `selftest/math_pipeline.json` | `range`, `map`, `filter`, `sum`, `subtract`, `divide`, `mod` |
| `selftest/string_pipeline.json` | `constant`, `trim`, `uppercase`, `split`, `join`, `length`, `replace`, `lowercase` |
| `selftest/object_ops.json` | `keys`, `values`, `set`, `merge`, `get`, `pick`, `omit`, nested references |
| `selftest/skipped_step.json` | `when` skipping, `not` condition, results/`skipped`/order |
| `selftest/retry.json` | `fail_until` + `retry.max_attempts` |
| `selftest/arrays.json` | `map`, `filter`, `reduce`, `sort`, `unique`, `slice`, `flatten`, `reverse`, mixed-type ordering, `stringify`/`type` transforms |
| `selftest/conditions_compound.json` | `all`, `any`, `not`, comparisons |
| `selftest/refs_nested.json` | dotted object paths and array indices in `$ref` |
| `selftest/unicode.json` | code-point `length`, ASCII-only case folding, `split("")`, UTF-8 passthrough |
| `selftest/error_overflow.json` | `ARITHMETIC_OVERFLOW`, partial results, exit 1 |
| inline cases | `STEP_FAILED`, `SKIPPED_REFERENCE`, `DIVISION_BY_ZERO`, `INVALID_ARG`, `INVALID_WORKFLOW`, `UNKNOWN_DEPENDENCY`, `CYCLIC_DEPENDENCY`, `EMPTY_ARRAY`, `INVALID_REFERENCE`, malformed JSON, stdin input |

Expected outputs were checked by hand against SPEC §8 and the semantics in
SPEC §3–§7.

## Layout

```
cli-c/
├── Makefile
├── README.md
├── workflow          # built executable
├── src/
│   ├── json.h        # JSON value model, parser, serializer API
│   ├── json.c        # strict JSON parser (i64-preserving), compact serializer,
│   │                 # structural equality and total ordering
│   └── engine.c      # CLI, validation, scheduling, refs, conditions, ops, retries
└── selftest/
    ├── run.sh        # self-test runner (POSIX sh)
    └── *.json        # hand-written workflows
```

## Implementation notes

* **Numbers.** Integer JSON literals that fit are stored as exact `i64`.
  Literals with a fraction/exponent, or integers outside `i64`, are stored as
  doubles so they can be recognized and rejected with `INVALID_ARG` when used
  as numeric parameters. All math uses `i64` with `__builtin_*_overflow`
  checks; division truncates toward zero and `%` follows the dividend's sign.
* **Deterministic scheduling.** Structural validation first (document shape,
  ids, unique ids, dependencies, cycles, retry ranges, `when` shape), then a
  topological loop that always picks the earliest ready step in `steps` order.
* **References.** `$ref` is recognized only for an object with exactly one key
  `$ref` whose value is a string. Targets must be direct/transitive
  `depends_on` dependencies; path lookup supports object keys and non-negative
  decimal array indices. Skipped targets produce `SKIPPED_REFERENCE`.
* **Skipped steps.** Result `null`, included in `results`, `execution_order`
  and `skipped`; a failed step is excluded from `results`/`execution_order`.
* **Retries.** Only `TRANSIENT_FAILURE` (from `fail_until`) is retried;
  exhausted attempts report `STEP_FAILED`.
* **JSON output.** Compact, valid UTF-8; control characters are escaped.
  `stringify` uses the same compact serializer.
* **`type_is` predicate.** SPEC §4 shows the target under a repeated `type`
  key, so the implementation accepts `{"type":"type_is","value":"..."}`,
  the `type_name`/`of`/`target` spellings, a raw document with a duplicate
  `type` key, and the collapsed `{"type":"number"}` form.

## Dependencies / pinning

None. The project intentionally ships its own small JSON layer
(`src/json.c`, `src/json.h`), so there is no package manager, lockfile or
vendored third-party version to pin.
