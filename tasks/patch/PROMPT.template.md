# Build the `patch` CLI in {{LANGUAGE}}

You are implementing one language of a token-usage benchmark. Implement the **JSON Patch engine** described in the spec, in **{{LANGUAGE}}**, as correctly as possible. An external black-box suite will drive your binary through its CLI and compare stdout exactly.

## Read this first

The full contract is at `{{SPEC_PATH}}`. **Read it completely before writing code.** It defines the CLI, the request/response envelope, RFC 6901 pointer parsing and escaping, the semantics of every operation, array insertion rules, and the exact error code for each failure. When in doubt, the SPEC wins.

## CLI

```bash
patch apply <request.json>
patch apply -          # request on stdin
```

stdout is exactly one JSON document (`{"status":"ok","document":...}` or `{"status":"error","code":...,"index":...,"message":...}`). Exit codes: `0` ok, `1` patch error, `2` invalid request, `3` internal.

The exact commands the harness uses for {{LANGUAGE}}:

{{ADAPTER_COMMANDS}}

## Rules

### Read / write boundary

Your working directory is `{{PROJECT_DIR}}/`.

- **You may read:** `{{SPEC_PATH}}`, `{{PRD_PATH}}`, and files inside `{{PROJECT_DIR}}/`.
- **You may only create or modify files inside `{{PROJECT_DIR}}/`.**
- **Do not read** other submissions, `tasks/`, `benchmark/`, or `results/`.
- If you think you need a change outside your folder, stop and say so.

### Engineering rules

1. Dependencies: standard library plus at most one small JSON library (C may bundle one). Pin versions. No JSON-patch or JSON-pointer libraries.
2. **Implement pointer parsing and the operations yourself.** The point of the task is escaping, insertion semantics and error classification.
3. The patch is atomic: on error, return the error and no document.
4. Deterministic: same request -> same response. No randomness, time, locale or network.
5. stdout carries only the JSON document; put diagnostics on stderr.

## Self-verification

Create a `selftest/` folder covering: add to objects and arrays (including `-`), remove/replace, move (including into a descendant), copy, test, pointer escapes `~0`/`~1`, nested paths, and at least one error of every code. Check the JSON and `index` by hand against the SPEC.

## Deliverables

- The implementation in `{{PROJECT_DIR}}/`.
- `{{PROJECT_DIR}}/README.md` — build, run, self-test, dependencies.
- The adapter commands above working from `{{PROJECT_DIR}}/`.

Implement **every** operation, pointer rule and error code in the SPEC. Completeness is the point.
