# workflow — Go implementation

Declarative workflow engine implementing `../../SPEC.md`.

## Requirements

- Go 1.22 or newer.
- Standard library only. There are no external modules; `go.mod` has no
  `require` entries.

## Build

```bash
cd cli-go
go build -o workflow .
```

## Run

```bash
./workflow run myworkflow.json   # read a workflow document from a file
./workflow run -                 # read the workflow document from stdin
```

- stdout contains exactly one JSON response document.
- stderr is reserved for diagnostics (currently unused).
- Exit codes: `0` completed, `1` runtime workflow failure, `2`
  input/validation error, `3` internal error.

## Self-test

Hand-written workflows live in `selftest/cases/`; each `*.json` fixture has a
recorded `*.expected.json` stdout and a `*.exit` code. The runner executes
every fixture (including one through stdin) and compares both:

```bash
cd cli-go
go build -o workflow .
./selftest/run.sh
```

Unit/behavioural tests covering every operation, all error codes, references,
conditions and retries:

```bash
cd cli-go
go test ./...
```

The three SPEC §8 worked examples are included as cases `01`-`03`.

## Dependencies

Only the Go standard library:

- `encoding/json` for JSON decoding/encoding (`json.Number` preserves numbers)
- `math/big` for exact numeric comparison and strict i64 parameter parsing
- `math`, `sort`, `strings`, `strconv`, `unicode/utf8`, `regexp`
- `os`, `io`, `bytes`, `fmt`

No workflow, graph, eval or code-generation libraries are used.

## Implementation notes

- JSON values are represented with `json.Number` for numbers, `bool`, `string`,
  `[]interface{}`, `map[string]interface{}` and `nil`. Integer parameters are
  validated exactly with `math/big.Rat` (so `1` and `1.0` are accepted, `1.5`
  and out-of-i64 literals are `INVALID_ARG`), and all arithmetic is checked
  signed 64-bit arithmetic.
- Structural equality compares numbers by numeric value, ignores object key
  order, and never equates different JSON types. `sort` and comparison
  operators use the total order from SPEC §3 and stable sorting.
- References (`$ref` with dotted paths / array indices) are resolved
  recursively inside `args` and condition operands, only against declared
  direct/transitive dependencies. Missing paths are `INVALID_REFERENCE` and
  skipped targets are `SKIPPED_REFERENCE`.
- Nested `when` conditions (`eq ne gt gte lt lte`, `all`, `any`, `not`) are
  validated structurally and then evaluated before argument resolution.
  A `$ref` used directly as a condition must resolve to a strict boolean.
- Execution is sequential with the spec's deterministic tie-break: the
  earliest ready step in document order. On failure the response contains only
  already-finished steps (skipped steps as `null`), and the failed step is
  excluded from `results` and `execution_order`.
- `fail_until` keeps a per-step attempt counter; only its
  `TRANSIENT_FAILURE` is retried, and exhausting `max_attempts` produces
  `STEP_FAILED`.
- Validation happens before any step executes, with `INVALID_WORKFLOW`,
  `UNKNOWN_DEPENDENCY` and `CYCLIC_DEPENDENCY` returning exit code `2`.
