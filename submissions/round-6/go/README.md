# workflow — declarative workflow engine (Go)

Implementation of the declarative workflow engine described in `SPEC.md`.
It reads a workflow JSON document, validates it structurally, executes the
steps in deterministic topological order, and writes exactly one JSON response
document to stdout.

## Requirements

- Go 1.21 or newer (standard library only).
- No network access, no third-party modules, no generated code.

## Build

```bash
cd submissions/round-6/go
go build -o workflow .
```

## Run

```bash
./workflow run myworkflow.json
./workflow run -                 # read the workflow document from stdin
```

`stdout` contains only the response JSON document. Diagnostics are not written
to stdout. Exit codes:

- `0` — workflow completed (`status: "completed"`)
- `1` — runtime workflow failure (`status: "failed"`)
- `2` — input/validation error (`status: "failed"`)
- `3` — internal error (best-effort JSON response)

Example:

```bash
./workflow run selftest/01_math_pipeline.json
```

## Self-test

The `selftest/` folder contains ten hand-written workflows covering a math
pipeline, strings, objects, skipped steps (`when`), retries (`fail_until`),
references, arrays, compound conditions, and two failure cases. Expected
responses and exit codes are in `selftest/expected/`.

Run everything (rebuilds the binary first):

```bash
cd submissions/round-6/go
./selftest/run.sh
```

Or run individual fixtures and inspect the JSON by hand:

```bash
./workflow run selftest/01_math_pipeline.json
./workflow run selftest/04_skipped.json
./workflow run selftest/05_retry.json
```

The self-test runner compares `status`, `results`, `execution_order`,
`skipped`, `error.code`, `error.step`, and the process exit code.
`error.message` is intentionally ignored because the specification declares it
free-form and never compared.

## Implementation notes

- Source layout:
  - `main.go` — CLI, input decoding, response writing, panic handling.
  - `validate.go` — structural validation, dependency checks, cycle detection.
  - `executor.go` — deterministic topological execution, `$ref` resolution,
    conditions, and retries.
  - `ops.go` — all operations, transforms, and filter predicates.
  - `value.go` — JSON value helpers, structural equality, total ordering,
    i64 parsing, and compact JSON serialization.
- Numbers are decoded with `json.Decoder.UseNumber`, so number literals keep
  their original JSON spelling. Operation numeric parameters are accepted only
  when the literal is an integer that fits in an i64. Generic JSON numbers keep
  arbitrary precision for structural equality and ordering (`math/big`).
- Arithmetic is checked with `math/big` and results must fit in i64; division
  truncates toward zero and mod takes the dividend's sign.
- Dependencies: standard library only.

## Dependencies

| Dependency | Version | Purpose |
|---|---|---|
| Go standard library | Go 1.21+ toolchain | JSON, CLI, arithmetic, sorting |
| Third-party modules | none | — |

`go.mod` pins the module declaration and required Go language version; there
is no `go.sum` because there are no external modules.
