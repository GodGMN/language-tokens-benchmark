# `workflow` — declarative workflow engine (C11)

A self-contained implementation of the workflow engine described in
[`../../../SPEC.md`](../../../SPEC.md). It reads one workflow JSON document,
executes its steps in deterministic dependency order and prints exactly one
JSON response document on stdout.

## Build

```bash
cd submissions/round-3/c
make
```

This produces the executable `./workflow` (compiled with `cc -std=c11 -O2`).
There are **no third-party dependencies**: the JSON parser, serializer and
engine are all contained in the single file `workflow.c`. Only the C standard
library is used (`libc`).

## Run

```bash
./workflow run myworkflow.json     # read a document from a file
./workflow run -                   # read the document from stdin
```

* stdout: exactly one JSON response document.
* stderr: diagnostics only (nothing is written in normal operation).
* exit codes: `0` completed, `1` runtime workflow failure, `2` input /
  validation error, `3` internal error.

Example:

```bash
cat > /tmp/example.json <<'JSON'
{
  "version": 1,
  "steps": [
    { "id": "n", "op": "range", "args": { "start": 1, "end": 5 } },
    { "id": "evens", "op": "filter", "depends_on": ["n"],
      "args": { "input": {"$ref": "n"}, "predicate": {"type": "even"} } },
    { "id": "total", "op": "sum", "depends_on": ["evens"],
      "args": { "input": {"$ref": "evens"} } }
  ]
}
JSON
./workflow run /tmp/example.json
# {"status":"completed","results":{"n":[1,2,3,4,5],"evens":[2,4],"total":6},"execution_order":["n","evens","total"],"skipped":[],"error":null}
```

## Self-test

```bash
make selftest
# or directly:
python3 selftest/check.py
```

`selftest/` contains ten hand-written workflow documents covering a math
pipeline, a string pipeline, object operations, skipped steps (`when`), a
successful retry (`fail_until`), an exhausted retry, arrays/transforms,
transitive references with the deterministic execution order, `type_is`
filtering and an empty workflow. `check.py` runs each fixture through the CLI,
verifies the exit code and compares the parsed response structurally against a
hard-coded expected value (object key order ignored, array order enforced,
free-form error messages ignored).

## Implementation notes

* **Values** are an immutable JSON tree. Parsed number lexemes are preserved
  (`1.0` round-trips as `1.0`) while numeric parameters are validated to be
  integer literals that fit in `i64`. All arithmetic uses signed 64-bit
  integers with explicit overflow checks (`__builtin_*_overflow`), C99
  truncating division and remainder with the dividend's sign.
* **Structural equality** treats `1` and `1.0` as equal and ignores object key
  order. **Total ordering** follows SPEC §3: number < string < boolean < null
  < array < object; objects compare sorted key lists first and then values.
  `sort` uses a stable merge sort. `unique` uses a structural hash table so it
  stays linear on large inputs.
* **Strings** are length-aware byte strings (embedded NUL is supported).
  Uppercase/lowercase are ASCII-only, `length` counts UTF-8 code points and
  byte-wise ordering is used everywhere the SPEC requires it.
* **References** are resolved recursively inside `args` (and condition
  operands) before the operation runs; only direct or transitive
  `depends_on` targets are legal. Path segments follow the
  object-key / non-negative-array-index rules.
* **Structural validation** (document shape, ids, operations, `depends_on`,
  cycles, `retry`, `when` shape) happens before any step executes and reports
  `INVALID_WORKFLOW`, `UNKNOWN_DEPENDENCY` or `CYCLIC_DEPENDENCY` with exit
  code 2.
* **Retries** apply only to `TRANSIENT_FAILURE` from `fail_until`; when the
  attempts are exhausted the final error code is `STEP_FAILED`. Other
  operation errors are reported immediately and never retried.
* The engine is deterministic: no randomness, clocks, network or filesystem
  writes. It only reads the given document (or stdin).

### Notes on ambiguous SPEC corners

* The SPEC writes the `type_is` filter predicate as
  `{"type": "type_is", "type": "<type>"}` (a duplicate JSON key). The engine
  accepts that literal form, the equivalent `{"type": "type_is", "value":
  "<type>"}` form, and the collapsed `{"type": "<type>"}` form.
* The PRD shows a bare-string filter shorthand (`"predicate": "even"`); it is
  accepted in addition to the SPEC's object form. Bare strings are also
  accepted for `map`'s `transform`.
* `and` / `or` / `not` require strict JSON booleans (wrong type =>
  `INVALID_ARG`), matching SPEC's "JSON booleans" wording and the general
  wrong-type rule.
