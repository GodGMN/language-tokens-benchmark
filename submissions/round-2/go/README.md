# `workflow` — declarative workflow engine (Go)

A self-contained Go implementation of the declarative workflow engine described in
`../../../SPEC.md`. It reads one workflow JSON document, executes the steps
deterministically, and writes exactly one JSON response document to stdout.

## Dependencies

* Go **1.18+** (tested with Go 1.25)
* **Standard library only** — `encoding/json`, `math/big`, `sort`, `strings`,
  `strconv`, etc. There are no third-party modules, no `go.sum`, and no network
  access at build or run time. `go.mod` pins the module to the local package.

## Build

From this directory (`submissions/round-2/go`):

```bash
go build -o workflow .
```

## Run

```bash
./workflow run myworkflow.json
./workflow run -                  # read the workflow JSON from stdin
```

* stdout contains exactly one JSON document.
* stderr is unused (free for diagnostics).
* Exit codes: `0` completed, `1` runtime workflow failure, `2` validation/input
  error, `3` internal error.

Example:

```bash
$ cat > demo.json <<'JSON'
{"version":1,"steps":[
  {"id":"n","op":"range","args":{"start":1,"end":5}},
  {"id":"evens","op":"filter","depends_on":["n"],
   "args":{"input":{"$ref":"n"},"predicate":{"type":"even"}}},
  {"id":"total","op":"sum","depends_on":["evens"],
   "args":{"input":{"$ref":"evens"}}}
]}
JSON
$ ./workflow run demo.json
{"status":"completed","results":{"evens":[2,4],"n":[1,2,3,4,5],"total":6},"execution_order":["n","evens","total"],"skipped":[],"error":null}
```

## Self-test

Hand-written workflows live in `selftest/` (11 documents). Run them all through
the CLI with:

```bash
./selftest/run.sh
```

The script builds `./workflow` if it is missing and then prints the response and
exit code for every file. It can also be run from the repository root as
`bash submissions/round-2/go/selftest/run.sh`.

| File | Exercises | Expected |
|---|---|---|
| `01_math_pipeline.json` | `range`, `filter`, `sum`, `reduce`, `min`, `max`, `divide`, `mod` | completed, exit 0 |
| `02_string_pipeline.json` | `trim`, `uppercase`, `lowercase`, `split` (incl. empty separator), `length`, `join`, `replace`, `concat` | completed, exit 0 |
| `03_array_pipeline.json` | `map` transforms, `sort` (mixed types), `unique` (structural), `reverse`, `slice`, `flatten`, `stringify`, `type` | completed, exit 0 |
| `04_object_ops.json` | `constant`, `set`, `get`, `keys`, `values`, `merge`, `pick`, `omit` | completed, exit 0 |
| `05_skipped_step.json` | `when` false → null result, `skipped`, dependent still runs (SPEC example B) | completed, exit 0 |
| `06_retry_success.json` | `fail_until` + retry succeeds on attempt 3 (SPEC example C) | completed, exit 0 |
| `07_retry_exhausted.json` | retries exhausted → `STEP_FAILED`, later steps not run | failed, exit 1 |
| `08_refs_and_conditions.json` | dotted/indexed `$ref`s, transitive deps, `all`/`not` conditions | completed, exit 0 |
| `09_utilities.json` | `equals` (numeric/structural), `compare` (mixed types), `and`, `or`, `not`, `type`, `truthy`/`falsy` filters | completed, exit 0 |
| `10_runtime_error_overflow.json` | i64 overflow → `ARITHMETIC_OVERFLOW`, partial results kept | failed, exit 1 |
| `11_validation_error.json` | unknown dependency → `UNKNOWN_DEPENDENCY`, nothing executed | failed, exit 2 |

The successful documents mirror SPEC §8's worked examples and the operation
tables; eyeball the printed JSON against the spec.

## Implementation layout

| File | Responsibility |
|---|---|
| `main.go` | CLI parsing, input reading, response encoding, exit codes |
| `value.go` | JSON value model, exact number handling (`big.Rat` for comparison), JSON encode/decode, structural equality and total ordering |
| `validate.go` | Structural validation (version, ids, fields, ops, deps, cycles, retry, `when` shape) |
| `engine.go` | Deterministic topological execution, `$ref` resolution, retries, skipped steps |
| `condition.go` | `when` evaluation (`eq/ne/gt/gte/lt/lte`, `all`, `any`, `not`) |
| `ops.go` | All operations from SPEC §4, checked i64 arithmetic, transforms and predicates |

Numbers are parsed with `json.Number`, so integer literals can be distinguished
from `1.0`/`1e3` (which are rejected wherever an i64 is required). Arithmetic is
done with checked 64-bit operations (`math/big` for overflow detection);
division truncates toward zero and `mod` takes the dividend's sign. Everything
is deterministic: no randomness, clocks, filesystem writes, or network.
