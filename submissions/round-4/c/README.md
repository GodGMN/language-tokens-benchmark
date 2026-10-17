# `workflow` — declarative workflow engine in C

A single-binary implementation of the declarative workflow engine described in
[`../../../SPEC.md`](../../../SPEC.md). It reads a workflow JSON document,
validates it, executes the steps in dependency order and prints exactly one JSON
response document on stdout.

## Build

From this directory (`submissions/round-4/c`):

```bash
make
```

This produces the executable `./workflow`.

The only requirement is a C11 compiler (GCC or Clang). The build was verified
with GCC 12 (`cc -std=c11 -O2 -Wall -Wextra -Wpedantic`), which is the default
used by the `Makefile`. To clean up:

```bash
make clean
```

## Run

```bash
./workflow run myworkflow.json
./workflow run -            # read the JSON document from stdin
cat myworkflow.json | ./workflow run -
```

- **stdout**: exactly one JSON document (compact, followed by a newline).
- **stderr**: unused by the program (nothing is written there).
- **exit codes**:
  - `0` — workflow executed, response `status` is `completed`
  - `1` — runtime workflow failure, response `status` is `failed`
  - `2` — input/argument/validation error, response `status` is `failed`
  - `3` — internal error (out of memory or an unreachable internal state)

Example:

```bash
$ ./workflow run selftest/04_skipped.json
{"status":"completed","results":{"x":3,"y":null,"z":7},"execution_order":["x","y","z"],"skipped":["y"],"error":null}
```

## Self-test

```bash
make selftest
# or equivalently
sh selftest/run.sh
```

`selftest/` contains ten hand-written workflows covering a math pipeline, a
string pipeline, object operations, a skipped step (`when`), a retry
(`fail_until`), array operations, nested references/compound conditions, an
arithmetic-overflow runtime failure, an unknown dependency validation failure
and the utility operations. For each workflow the runner:

1. runs `./workflow run <file>`,
2. checks stdout byte-for-byte against `selftest/expected/<name>.json`,
3. checks the exit code against `selftest/expected/<name>.exit`,
4. repeats the run through `./workflow run -` (stdin) and checks that it is
   identical.

Any mismatch makes the runner exit non-zero.

## Dependencies

**None.** The implementation uses only the C standard library. The JSON parser
and serializer are small, purpose-built routines inside `workflow.c` (the
SPEC explicitly allows a small JSON library for C; writing one in-tree keeps the
build fully offline and lets the engine distinguish JSON integer literals from
fractional/exponent literals, which the spec requires).

- Language: C11 (`_Static_assert`, `snprintf`, `qsort`, …).
- Compiler builtins used: `__builtin_{add,sub,mul}_overflow` (GCC/Clang) for
  exact i64 overflow detection.
- No third-party libraries, no network access, no filesystem writes.

## Implementation notes

- **Integers**: all numeric parameters and results are signed 64-bit. A numeric
  parameter must be a JSON integer literal that fits in i64 (`1.0`, `1e3` are
  `INVALID_ARG`). Arithmetic is overflow-checked with `ARITHMETIC_OVERFLOW`;
  division truncates toward zero; `%` takes the sign of the dividend;
  `INT64_MIN / -1` and `INT64_MIN % -1` are `ARITHMETIC_OVERFLOW`; division or
  modulo by zero is `DIVISION_BY_ZERO`.
- **Values** keep their JSON type. Structural equality treats `1` and `1.0` as
  equal. The total order used by `sort`/`compare`/comparison conditions is
  `number < string < boolean < null < array < object`, with byte-wise UTF-8
  string ordering; `sort` is a stable merge sort.
- **Strings**: `length` and `split ""` count Unicode code points; `uppercase`,
  `lowercase` and `trim` are ASCII-only.
- **References**: `{"$ref": "step.path.0"}` is resolved recursively throughout
  an operation's arguments (and through `when`'s `left`/`right`). Targets must
  be direct or transitive `depends_on` ancestors; missing paths and non-numeric
  or negative indices are `INVALID_REFERENCE`; a skipped target is
  `SKIPPED_REFERENCE`.
- **Execution**: structural validation happens before any step runs. Steps are
  scheduled topologically, choosing the earliest ready step in the original
  `steps` array. Skipped steps are recorded as `null` results, appear in
  `execution_order` and in `skipped`. A definitive failure stops execution and
  the response contains only already-finished steps (the failed step is not in
  `results`/`execution_order`).
- **Retries**: only `TRANSIENT_FAILURE` (produced by `fail_until`) is retried,
  up to `retry.max_attempts` (1..5, default 1). When attempts are exhausted the
  reported code is `STEP_FAILED`. Attempt counters are local to the step and to
  the single workflow execution.
- **Output**: object key order is not significant to JSON consumers; arrays are
  emitted in their specified order (execution order, sorted keys/values, etc.).

## Files

```
workflow.c                 implementation (JSON parser/serializer + engine)
Makefile                   build / clean / selftest targets
README.md                  this file
selftest/*.json            ten self-test workflows
selftest/expected/*.json   expected stdout for each self-test
selftest/expected/*.exit   expected exit code for each self-test
selftest/run.sh            self-test runner
```
