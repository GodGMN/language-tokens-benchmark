# workflow — declarative workflow engine (C)

A complete implementation of the `workflow` CLI described in `../../../SPEC.md`.

## Build

```bash
cd submissions/round-2/c
make
```

Requirements: a C11 compiler (`cc`/`gcc`/`clang`) and `make`. No external
libraries, no network access, no generated dependencies.

## Run

```bash
./workflow run myworkflow.json
./workflow run -            # read the JSON document from stdin
```

* stdout: exactly one JSON response document (compact) plus a trailing newline.
* stderr: unused.
* Exit codes: `0` completed, `1` runtime workflow failure, `2` validation/input
  error, `3` internal error.
* The only file read is the input document (or stdin).

Example:

```bash
./workflow run selftest/math_pipeline.json
```

## Self-test

```bash
cd submissions/round-2/c
make
make test          # or: ./selftest/run.sh
```

`selftest/run.sh` runs every workflow in `selftest/*.json` through the binary and
checks the exit code and a few key result fragments. The fixtures cover:

| File | Covers |
|---|---|
| `math_pipeline.json` | `range`, `filter`, `map`, `sum`, `max`, retries-free pipeline |
| `string_pipeline.json` | `concat`, `uppercase`, `split`, `join`, `replace`, `length` |
| `object_ops.json` | `constant`, `set`, `keys`, `values`, `merge`, `pick`, `omit`, `get` |
| `skip.json` | `when` false -> `null` result, `skipped` list, execution order |
| `retry.json` | `fail_until` + `retry.max_attempts` |
| `refs.json` | `$ref` with dotted object paths and array indices, nested refs |
| `conditions.json` | compound `all` / `any` / `not` conditions |
| `array_ops.json` | `sort` (mixed types), `reverse`, `unique`, `slice`, `flatten` |
| `error_overflow.json` | `ARITHMETIC_OVERFLOW` (exit 1, prior results kept) |
| `invalid_workflow.json` | `UNKNOWN_DEPENDENCY` (exit 2) |

## Implementation notes

* Single-binary C11 program, sources in `src/`:
  * `src/json.c`, `src/json.h` — arena allocator, strict JSON parser and compact
    emitter, structural equality, total value ordering, truthiness.
  * `src/main.c` — CLI, structural validation, DAG execution, `$ref` resolution,
    conditions, retries, all operations, response building.
* Dependencies: **none** beyond the C standard library. The JSON parser/emitter
  is part of this submission (no vendored third-party code, no package manager).
* Numbers are signed 64-bit integers for all numeric parameters and arithmetic;
  overflow is detected with compiler builtins. Integer literals with a fraction
  or exponent are rejected for numeric parameters even when mathematically
  integral, as required by the spec.
* JSON numbers keep their original literal for pass-through output; arithmetic
  results are emitted canonically.
* Integer comparisons of out-of-i64-range literals use exact decimal
  comparison, so equality/ordering stay correct beyond 64 bits.
* Execution is deterministic: topological order with the earliest-in-`steps`
  tie-break, stable `sort`, no randomness, clocks, or external I/O.
* The parser preserves duplicate object keys (needed for the spec's literal
  `{"type":"type_is","type":"number"}` predicate form); lookups use the last
  occurrence, matching common JSON behavior.
* `map.transform` supports both the fixed forms (`constant`, `add`, `subtract`,
  `multiply`, `divide`, `mod`, `negate`, `abs`, `stringify`, `type`) and the
  documented `{"type":"op", ...}` form with `{"$self": true}` substitution.
* `filter` predicates support `even`, `odd`, `gt`, `gte`, `lt`, `lte`, `eq`,
  `ne`, `truthy`, `falsy`, `type_is` (including the repeated-key spelling), and
  `string_contains`; the PRD-style shorthand strings for `even`/`odd`/
  `truthy`/`falsy` are also accepted.

## Source layout

```
Makefile
README.md
src/json.h
src/json.c
src/main.c
selftest/*.json
selftest/run.sh
```
