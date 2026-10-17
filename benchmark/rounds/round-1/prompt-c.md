# Build the `workflow` CLI in C

You are implementing one language of a token-usage benchmark. Your goal: implement the complete declarative workflow engine described in the spec, in **C**, as correctly as possible. You will be judged by an external black-box test suite that only talks to your binary through its CLI.

## Read this first

The full functional contract is at `../SPEC.md` (relative to this folder). **Read it completely before writing code.** It defines the CLI, the workflow JSON format, every operation, reference semantics, conditions, retries, error codes and worked examples. When in doubt, the SPEC wins over your instincts.

Also skim `../PRD.md` §5 (CLI contract) for context.

## What you must build

A CLI named `workflow` supporting exactly:

```bash
workflow run <workflow.json>
workflow run -        # JSON document on stdin
```

- **stdout**: exactly one valid JSON document, no other output.
- **stderr**: free for diagnostics.
- Exit codes: `0` completed, `1` runtime workflow failure, `2` validation/input error, `3` internal error.

The exact build/run commands the test harness will use for C:

```bash
cd cli-c
# build
make
# run
./workflow run myworkflow.json   # or: run -  (stdin)
```

Make sure those commands work from this folder (`cli-c`).

## Rules

### Read / write boundary

Your working directory is `cli-c/`. Pi was started here, and the session record is tied to this folder.

- **You may read:** everything you need *below* the repository root — `../SPEC.md`, `../PRD.md`, and files inside `cli-c/`.
- **You may only create or modify files inside `cli-c/`.** Everything else in the repo is read-only for you.
- **Do not read** other `cli-*` folders. Do not read or modify `benchmark/`, `results/`, or `scripts/`.
- Do not `cd` out of `cli-c/` to work, and do not create files anywhere else (no `/tmp` artifacts that end up in the repo, no edits to root docs).

If you believe you need a change outside `cli-c/`, stop and say so instead of making it.

### Engineering rules

1. **Dependencies.** Standard library plus at most a small number of well-known libraries for JSON parsing and CLI arg handling are allowed (for C: one small JSON library is explicitly allowed — e.g. a single-header one). Pin versions (package manager lockfile / Makefile / vendored). **No workflow engines, no DAG/graph libraries, no eval/code-interpreters.**
2. **No user-code execution.** Operations are a fixed, closed set (see SPEC §4). No `eval`, no user-supplied functions.
3. **i64 arithmetic.** Numbers are signed 64-bit integers; overflow = `ARITHMETIC_OVERFLOW`; division truncates toward zero; mod takes the dividend's sign. No floats anywhere in math.
4. **Deterministic.** Same input -> same output, always. No randomness, no timestamps in output, no wall-clock dependence.
5. **No network, no filesystem side effects** beyond reading the input document (and stdin).

## Self-verification (do this before finishing)

Create a small `selftest/` subfolder (or similar) inside your folder with 5-10 hand-written workflow JSON files covering: a math pipeline, a string pipeline, an object op, a skipped step (`when`), and a retry (`fail_until`). Run them through your CLI and eyeball the JSON against SPEC §8's worked examples. Your README should list exactly how to build, run and self-test.

## Deliverables

- Working implementation in `cli-c/`.
- `cli-c/README.md` — how to build, run, self-test, and your dependency list.
- The exact commands in ````bash
cd cli-c
# build
make
# run
./workflow run myworkflow.json   # or: run -  (stdin)
```` working.

Do not stop early: implement **every** operation in SPEC §4, all error codes in SPEC §7, references (§6), conditions (§5) and retries. Completeness is the whole point.
