# Build the `expr` CLI in Python

You are implementing one language of a token-usage benchmark. Implement the **expression evaluator** described in the spec, in **Python**, as correctly as possible. An external black-box suite will drive your binary through its CLI and compare stdout exactly.

## Read this first

The full contract is at `../../../tasks/expr/SPEC.md`. **Read it completely before writing code.** It defines the CLI, the request/response envelope, the grammar and precedence, the exact semantics of every operator and function, and — critically — the exact `position` reported for every error. When in doubt, the SPEC wins.

## CLI

```bash
expr eval <request.json>
expr eval -            # request on stdin
```

stdout is exactly one JSON document (`{"status":"ok","value":...}` or `{"status":"error","code":...,"position":...,"message":...}`). Exit codes: `0` ok, `1` evaluation error, `2` invalid request, `3` internal.

The exact commands the harness uses for Python:

```bash
cd submissions/round-8-expr/python
# (interpreted language — no build step)
# run
python3 expr.py eval request.json     # or: - for stdin
```

## Rules

### Read / write boundary

Your working directory is `submissions/round-8-expr/python/`.

- **You may read:** `../../../tasks/expr/SPEC.md`, `../../../PRD.md`, and files inside `submissions/round-8-expr/python/`.
- **You may only create or modify files inside `submissions/round-8-expr/python/`.**
- **Do not read** other submissions, `tasks/`, `benchmark/`, or `results/`.
- If you think you need a change outside your folder, stop and say so.

### Engineering rules

1. Dependencies: standard library plus at most one small JSON library (C may bundle one). Pin versions. No parser generators, no expression libraries, no `eval`.
2. **Write the parser and evaluator yourself.** The point of the task is precedence, associativity and short-circuiting.
3. i64 arithmetic; overflow, division by zero and `INT64_MIN` edge cases exactly as specified.
4. Deterministic: same request -> same response. No randomness, time, locale or network.
5. stdout carries only the JSON document; put diagnostics on stderr.

## Self-verification

Create a `selftest/` folder with requests covering: precedence, parentheses, short-circuit (`false && missing`), ternary, arrays, each function, and at least one error of every code. Run them through your CLI and check the JSON and `position` by hand against the SPEC.

## Deliverables

- The implementation in `submissions/round-8-expr/python/`.
- `submissions/round-8-expr/python/README.md` — build, run, self-test, dependencies.
- The adapter commands above working from `submissions/round-8-expr/python/`.

Implement **every** function, operator and error code in the SPEC. Completeness is the point.
