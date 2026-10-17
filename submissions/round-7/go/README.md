# `workflow` — declarative workflow engine (Go)

A complete implementation of the workflow engine described in
[`../../../SPEC.md`](../../../SPEC.md), written in Go using only the standard
library.

## Build

```bash
cd submissions/round-7/go
go build -o workflow .
```

Requires Go 1.21 or newer (tested with Go 1.25). No network access is needed
and there are no third-party modules (`go.mod` declares no `require`s).

## Run

```bash
./workflow run myworkflow.json
./workflow run -                 # read the JSON document from stdin
```

* **stdout** — exactly one JSON document (the response).
* **stderr** — unused (diagnostics only).
* **exit codes** — `0` completed, `1` runtime failure, `2` validation/input
  error, `3` internal error.

## Self-test

Hand-written fixtures live in `selftest/`; the expected responses and exit
codes are in `selftest/expected/*.json` and `selftest/expected/*.exit`.

```bash
cd submissions/round-7/go
go build -o workflow .
./selftest/run.sh                # runs the CLI against every fixture
# or, without jq installed:
go test ./...                    # same fixtures through the engine
```

`selftest/run.sh` compares both the JSON body and the process exit code (it
canonicalises with `jq` when available). Cases covered:

| Fixture | Covers |
|---|---|
| `math_pipeline.json` | SPEC §8 example A (range → filter → sum) |
| `skipped_step.json` | SPEC §8 example B (`when` false) |
| `retry.json` | SPEC §8 example C (`fail_until` + `retry`) |
| `retry_exhausted.json` | retries exhausted → `STEP_FAILED`, dependencies not run |
| `math_ops.json` | divide/mod signs, min/max/sum, empty `sum`, `range` start>end, `reduce` |
| `string_ops.json` | concat, ASCII case, trim, replace, length (code points), split (incl. empty sep), join |
| `object_ops.json` | get/set/keys/values/merge/pick/omit, missing keys → `null` |
| `array_ops.json` | sort (mixed types), reverse, unique (structural), slice (negatives/clamping), flatten, map (all transform forms), filter (all predicate forms) |
| `conditions.json` | `all`/`any`/`not`, compare/equals, `type`, `and`/`or`/`not` |
| `conditions_ref.json` | boolean `$ref` used directly as a condition |
| `execution_order.json` | deterministic tie-break: earliest ready step in document order |
| `ref_paths.json` | dotted refs, array-index refs, refs nested in arrays/objects |
| `err_overflow.json` | `ARITHMETIC_OVERFLOW` |
| `err_divzero.json` | `DIVISION_BY_ZERO` |
| `err_min_empty.json` | `EMPTY_ARRAY` |
| `err_arg_type.json` | non-integer numeric literal → `INVALID_ARG` |
| `err_bad_ref_path.json` | negative array index segment → `INVALID_REFERENCE` |
| `err_skipped_ref.json` | reference to skipped step → `SKIPPED_REFERENCE` |
| `err_unknown_dep.json` | `UNKNOWN_DEPENDENCY` |
| `err_cycle.json` | `CYCLIC_DEPENDENCY` |
| `err_version.json` | `1.0` version → `INVALID_WORKFLOW` |
| `err_unknown_field.json` | unknown step field → `INVALID_WORKFLOW` |
| `err_bad_cond.json` | malformed `when` → `INVALID_WORKFLOW` |
| `err_invalid_condition.json` | condition evaluating to a non-boolean → `INVALID_CONDITION` |

## Dependencies

Standard library only:

* `encoding/json` (with `UseNumber`, so JSON numbers keep their literal form
  and integer literals can be distinguished from `1.0` / `1e3`)
* `math/big` (exact i64 overflow checks for `+ - *`, `sum`, `reduce`,
  `negate`, `abs`)
* `sort`, `strings`, `strconv`, `unicode/utf8`, `regexp`, `bytes`, `os`, `io`

No workflow/DAG/graph/eval libraries are used; operations are a fixed closed
set in `ops.go`.

## Layout

| File | Contents |
|---|---|
| `main.go` | CLI, stdin/file input, response shape, exit codes, panic guard |
| `value.go` | JSON value helpers: structural equality, total order, truthiness, type names, i64/i64-exact number handling |
| `validate.go` | Structural validation (version, steps, ids, fields, deps, cycles, `retry`, `when` shape) |
| `refs.go` | `$ref` resolution (dotted paths, array indices, dependency closure, skipped refs) and `when` evaluation |
| `ops.go` | All SPEC §4 operations, `map` transforms and `filter` predicates, i64 arithmetic |
| `engine.go` | Deterministic topological execution, skipping, retries, error attribution |
| `selftest_test.go` | Runs every `selftest/*.json` and compares with the expected response |

## Semantics notes

* Numbers are parsed as `json.Number`; arguments require an integer literal
  that fits in i64, while structural equality compares numbers by exact numeric
  value (so `1 == 1.0`).
* Execution picks, at every step, the ready step that appears earliest in the
  `steps` array. Skipped steps count as finished and are recorded with a
  `null` result, in `execution_order` and in `skipped`.
* Only `TRANSIENT_FAILURE` from `fail_until` is retried; when attempts run out
  the reported code is `STEP_FAILED`. All other operation errors are final.
* Runtime failures exit `1` and keep the results/order of steps that already
  finished; validation failures exit `2`.
