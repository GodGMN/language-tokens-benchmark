# workflow (Go implementation)

A dependency-free Go implementation of the declarative workflow engine described
in `../../../SPEC.md`.

## Build

```bash
cd submissions/round-3/go
go build -o workflow .
```

## Run

```bash
./workflow run myworkflow.json
./workflow run -          # read the workflow JSON from stdin
```

* stdout contains exactly one JSON document (the response).
* diagnostics go to stderr.
* exit codes: `0` completed, `1` runtime failure, `2` validation/input error,
  `3` internal error.

## Dependencies

Only the Go standard library (`go.mod` declares the module and Go version; there
are no third-party modules and no network access is needed to build).

## Self-test

Ten hand-written workflows live in `selftest/`:

| file | covers |
|---|---|
| `math_pipeline.json` | SPEC Example A: `range` + `filter` + `sum` |
| `math_edges.json` | truncating division, C99-style modulo, min/max, empty range |
| `string_pipeline.json` | trim, uppercase, split, join, length |
| `object_ops.json` | set, get, keys, values, merge, pick, omit |
| `skipped.json` | SPEC Example B: `when` false + `skipped` list |
| `retry.json` | SPEC Example C: `fail_until` + `retry.max_attempts` |
| `conditions.json` | `all` / `any` / `not` conditions |
| `array_ops.json` | sort, unique, reverse, slice, map, reduce, flatten |
| `refs.json` | dotted paths, array indices, dependent references |
| `failure.json` | retries exhausted -> `STEP_FAILED`, exit 1 |

Run them all:

```bash
cd submissions/round-3/go
go build -o workflow .
sh selftest/run.sh
```

The runner builds the binary if needed, executes each fixture through the CLI,
checks exit codes and verifies that stdout is a single valid JSON document.

## Implementation notes

* Source files: `main.go` (CLI/response), `workflow.go` (structural
  validation), `engine.go` (topological execution, refs, conditions),
  `ops.go` (all operations), `value.go` (i64/equality/total ordering),
  `errors.go` (error codes).
* Numbers are decoded with `json.Decoder.UseNumber()` so that the distinction
  between the integer literal `1` and `1.0` is preserved; numeric *parameters*
  require a plain i64 literal.
* All arithmetic is checked i64 arithmetic; division truncates toward zero and
  modulo takes the dividend's sign.
* Execution picks, among ready steps, the earliest one in the document, which
  makes the topologic order deterministic.
* `$ref` objects are resolved recursively inside `args` (and inside `when`
  operands), with dependency-closure, skipped-step and path checks.
* Skipped steps are recorded with `null` results, included in
  `execution_order`, and listed in `skipped`.
