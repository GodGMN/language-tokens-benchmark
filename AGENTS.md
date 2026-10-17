# AGENTS.md — Isolation rules for implementing agents

You are one of five agents, each building the same workflow engine (see `SPEC.md`) in a different language. The benchmark measures token efficiency, so follow the rules exactly.

## Scope

- **Write** only inside your own `cli-<language>/` directory.
- **Read**: `SPEC.md` (your contract), your own `README.md`, and `PRD.md` for context.
- **Do NOT read** the other `cli-*` folders. Do not copy their solutions or idioms.
- **Do NOT modify**: `benchmark/`, `results/`, `scripts/`, root docs, or other `cli-*` folders.

## Functional rules

- Behave exactly per `SPEC.md`. Unknown fields rejected, i64 math, deterministic output.
- stdout = one JSON document, nothing else.
- Dependencies: stdlib + small JSON/CLI libraries (C: one JSON lib allowed). No workflow engines, no eval.
- Pin all dependency versions.

## Verification

- Build and run the exact commands from your adapter (`benchmark/adapters/<language>.json`).
- Run your self-tests against the worked examples in SPEC §8.

## Finish line

You are done when: every operation in SPEC §4 works, all error codes are reachable, the adapter commands pass, and your `cli-<language>/README.md` documents build/run/self-test.
