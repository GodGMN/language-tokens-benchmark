# `workflow` — declarative workflow engine (Go)

A dependency-free Go implementation of the workflow engine described in
[`SPEC.md`](../../../SPEC.md). The CLI reads one JSON workflow document and
executes its steps, writing exactly one JSON response document to stdout.

## Requirements

- Go 1.21 or newer (developed and tested with Go 1.25).
- No third-party dependencies. The module (`go.mod`) uses only the Go standard
  library: `encoding/json`, `math/big`, `sort`, `strconv`, `strings`,
  `unicode/utf8`, `regexp`, `os`, `io`, `fmt`.

## Build

```bash
cd submissions/round-5/go
go build -o workflow .
```

## Run

```bash
./workflow run myworkflow.json
./workflow run -          # read the document from stdin
```

- stdout: one JSON response document, nothing else.
- stderr: diagnostics only.
- Exit codes: `0` completed, `1` runtime workflow failure, `2` validation/input
  error, `3` internal error.

## Self-test

Hand-written workflows (math, strings, objects, skipped step, retry,
references, conditions, arrays and a runtime error) live in `selftest/`.
Build first, then run all of them:

```bash
cd submissions/round-5/go
go build -o workflow .
./selftest/run.sh
```

Or run one at a time:

```bash
./workflow run selftest/example_a.json          # SPEC example A
./workflow run selftest/example_b_skip.json     # SPEC example B
./workflow run selftest/example_c_retry.json    # SPEC example C
./workflow run selftest/math_pipeline.json
./workflow run selftest/string_pipeline.json
./workflow run selftest/object_ops.json
./workflow run selftest/skipped_step.json
./workflow run selftest/retry.json
./workflow run selftest/references.json
./workflow run selftest/conditions.json
./workflow run selftest/arrays.json
./workflow run selftest/error_divzero.json      # expected: exit 1
```

Expected highlights:

- `example_a.json` → `{"n":[1,2,3,4,5],"evens":[2,4],"total":6}`, exit 0.
- `example_b_skip.json` → `y: null`, `skipped: ["y"]`, exit 0.
- `example_c_retry.json` → `flaky: "ok"` after 2 transient failures, exit 0.
- `error_divzero.json` → `DIVISION_BY_ZERO` with `step: "boom"`, exit 1.

## Source layout

| File | Contents |
|---|---|
| `main.go` | CLI entry point, response encoding, exit codes, panic recovery |
| `value.go` | JSON value model, number-literal preservation, structural equality and the spec's total order |
| `workflow.go` | Structural validation, dependency graph/cycles, references, conditions, topological execution, retries |
| `ops.go` | All operations from SPEC §4, checked i64 arithmetic, map transforms and filter predicates |

## Implementation notes

- **Numbers.** JSON number literals are preserved verbatim (`Number.Raw`) so a
  numeric *parameter* can be required to be a plain integer literal fitting
  i64 (`1.0` / `1e3` → `INVALID_ARG`), while `constant`/`equals` still compare
  numeric values (`1 == 1.0`) and output keeps the original literal.
- **i64 arithmetic.** All math is checked; overflow yields
  `ARITHMETIC_OVERFLOW`; division truncates toward zero and `%` takes the
  dividend's sign; `INT64_MIN / -1` and `INT64_MIN % -1` overflow.
- **References.** `$ref` with dotted object paths and non-negative array
  indices resolves only against direct/transitive `depends_on` targets.
  Missing paths, non-dependency targets and self-references are
  `INVALID_REFERENCE`; a reference to a step skipped by `when` is
  `SKIPPED_REFERENCE`.
- **Execution order.** Repeatedly picks the earliest step in `steps` order
  whose dependencies have all finished (completed or skipped), as required by
  SPEC §7.2.
- **Retries.** Only `TRANSIENT_FAILURE` (from `fail_until`) is retried up to
  `max_attempts`; if attempts are exhausted the final code is `STEP_FAILED`.
  Other operation errors are never retried.
- **`type_is` predicate.** SPEC §4 writes the target type as a second `"type"`
  key, which is impossible in a JSON object (duplicate keys). The
  implementation accepts the target under `"value"` (consistent with the other
  predicates) and also tolerates `"target_type"`, `"type_name"` and `"target"`.
- **`map.transform`.** Both the fixed forms (`constant`, `add`, `subtract`,
  `multiply`, `divide`, `mod`, `negate`, `abs`, `stringify`, `type`) and the
  documented `{"type":"op", ...}` form with `{"$self": true}` are supported.
- **Determinism.** No randomness, timestamps or clock reads; all map traversals
  that can surface errors iterate keys in sorted order.
