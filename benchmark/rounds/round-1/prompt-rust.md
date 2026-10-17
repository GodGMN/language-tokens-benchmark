# Build the `workflow` CLI in Rust

You are implementing one language of a token-usage benchmark. Your goal: implement the complete declarative workflow engine described in the spec, in **Rust**, as correctly as possible. You will be judged by an external black-box test suite that only talks to your binary through its CLI.

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

The exact build/run commands the test harness will use for Rust:

```bash
cd cli-rust
# build
cargo build --release
# run
target/release/workflow run myworkflow.json   # or: run -  (stdin)
```

Make sure those commands work from this folder (`cli-rust`).

## Rules

1. **Stay in your folder.** Only create/modify files inside `cli-rust/`. Do not touch `benchmark/`, other `cli-*` folders, or the root docs.
2. **Dependencies.** Standard library plus at most a small number of well-known libraries for JSON parsing and CLI arg handling are allowed (for C: one small JSON library is explicitly allowed — e.g. a single-header one). Pin versions (package manager lockfile / Makefile / vendored). **No workflow engines, no DAG/graph libraries, no eval/code-interpreters.**
3. **No user-code execution.** Operations are a fixed, closed set (see SPEC §4). No `eval`, no user-supplied functions.
4. **i64 arithmetic.** Numbers are signed 64-bit integers; overflow = `ARITHMETIC_OVERFLOW`; division truncates toward zero; mod takes the dividend's sign. No floats anywhere in math.
5. **Deterministic.** Same input -> same output, always. No randomness, no timestamps in output, no wall-clock dependence.
6. **No network, no filesystem side effects** beyond reading the input document (and stdin).

## Self-verification (do this before finishing)

Create a small `selftest/` subfolder (or similar) inside your folder with 5-10 hand-written workflow JSON files covering: a math pipeline, a string pipeline, an object op, a skipped step (`when`), and a retry (`fail_until`). Run them through your CLI and eyeball the JSON against SPEC §8's worked examples. Your README should list exactly how to build, run and self-test.

## Deliverables

- Working implementation in `cli-rust/`.
- `cli-rust/README.md` — how to build, run, self-test, and your dependency list.
- The exact commands in ````bash
cd cli-rust
# build
cargo build --release
# run
target/release/workflow run myworkflow.json   # or: run -  (stdin)
```` working.

Do not stop early: implement **every** operation in SPEC §4, all error codes in SPEC §7, references (§6), conditions (§5) and retries. Completeness is the whole point.
