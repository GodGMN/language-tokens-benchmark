# `expr` — expression evaluator (Go)

A small typed expression language parser + evaluator, exposed as a CLI that
speaks the request/response envelope from `tasks/expr/SPEC.md`.

## Build

```bash
cd submissions/round-8-expr/go
go build -o expr .
```

Go 1.21+ is required. There are **no third-party dependencies** (standard
library only); `go.mod` pins the module path/Go version and no network access is
needed to build.

## Run

```bash
./expr eval request.json
./expr eval -            # request from stdin
```

`request.json`:

```json
{ "source": "1 + 2 * 3", "vars": { "x": 5 } }
```

stdout is exactly one JSON document:

- success: `{"status":"ok","value":...}`
- error: `{"status":"error","code":"...","position":<n|null>,"message":"..."}`

Exit codes: `0` success, `1` evaluation error, `2` invalid request,
`3` internal error. Diagnostics never go to stdout.

## Implementation

The implementation is hand-written; no parser generators or expression
libraries are used.

| file        | contents                                                        |
|-------------|-----------------------------------------------------------------|
| `main.go`   | CLI, request envelope validation, JSON `vars` conversion         |
| `lexer.go`  | tokenizer: integers, strings/escapes, identifiers, operators     |
| `parser.go` | recursive-descent parser (precedence, associativity, AST)         |
| `eval.go`   | evaluator, i64 arithmetic/overflow checks, all functions         |
| `value.go`  | runtime values, structural equality, compact JSON serialization  |
| `errors.go` | error codes, positions and response emission                     |

Notes:

- Positions are 0-based **code-point** offsets in `source`; end-of-input
  positions are `len(source)` in code points.
- `&&` / `||` short-circuit and only the selected ternary branch is evaluated.
- All arithmetic is i64 with explicit overflow checks (including
  `INT64_MIN / -1`, `INT64_MIN % -1`, `-INT64_MIN`, `abs`/`neg` of
  `INT64_MIN`, and `sum` overflow).
- JSON output uses minimal escaping (`"` `\` and control characters); non-ASCII
  text is emitted as raw UTF-8.

## Self-test

`selftest/cases/` contains one request (`*.json`), expected stdout
(`*.expected`) and expected exit code (`*.exit`) per case, covering precedence,
parentheses, associativity, short-circuiting, ternary, arrays, every function,
and at least one error of every code.

```bash
./selftest/run.sh
```

The runner rebuilds the binary and checks every case plus stdin mode.
