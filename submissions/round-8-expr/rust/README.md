# expr (Rust)

A dependency-free implementation of the `expr` expression-evaluator CLI from
`tasks/expr/SPEC.md`.

## Build

```bash
cargo build --release
```

This produces `target/release/expr`.

## Run

```bash
target/release/expr eval request.json
target/release/expr eval -          # read the request from stdin
```

A request is a JSON object:

```json
{ "source": "1 + 2 * 3", "vars": { "x": 5 } }
```

stdout is exactly one compact JSON document. Diagnostics go to stderr.

| exit | meaning |
|---|---|
| 0 | `status: "ok"` |
| 1 | evaluation error (parse/type/runtime) |
| 2 | invalid request (`INVALID_REQUEST`) |
| 3 | internal error |

Examples:

```bash
$ echo '{"source":"1 + 2 * 3"}' | target/release/expr eval -
{"status":"ok","value":7}

$ echo '{"source":"1 +"}' | target/release/expr eval -
{"status":"error","code":"PARSE_ERROR","position":3,"message":"unexpected end of input"}
```

## Self-test

```bash
cargo build --release
./selftest/run.sh
```

`selftest/run.sh` reads `selftest/cases.tsv`. Each line contains a case name,
the request JSON, the expected result, and the expected exit code. Success
cases compare the exact stdout document; error cases compare the error code
and position (the message is free-form per the spec). Every case is run twice:
once with a request file and once through stdin (`eval -`).

The corpus covers precedence, associativity, parentheses, short-circuiting,
ternary evaluation, arrays, all built-in functions, Unicode/code-point
handling, integer edge cases, and at least one example of every error code
(`INVALID_REQUEST`, `PARSE_ERROR`, `OVERFLOW`, `DIVISION_BY_ZERO`,
`TYPE_ERROR`, `UNKNOWN_VARIABLE`, `UNKNOWN_FUNCTION`, `ARITY_ERROR`,
`EMPTY_ARRAY`).

## Dependencies

None. The implementation uses only the Rust standard library (edition 2021).
The JSON request parser, the expression lexer/parser, the evaluator, and the
compact JSON writer are all implemented in `src/main.rs`. No parser generator
or expression-evaluation library is used.

## Implementation notes

- Positions are 0-based **character** offsets (not UTF-8 byte offsets), so
  errors after multi-byte characters point at the same character index a
  user sees.
- The expression is fully tokenized and parsed before evaluation. Syntax and
  literal errors are therefore reported even inside a branch that would be
  short-circuited; runtime errors in an unselected branch are not.
- `&&`, `||`, and `?:` evaluate only what they need. All other operators,
  array elements, and call arguments are evaluated left to right.
- String lengths, `substr`, `index_of`, and empty-separator `split` operate on
  Unicode code points.
- i64 edge cases (`INT64_MIN / -1`, `INT64_MIN % -1`, `-INT64_MIN`,
  `abs(INT64_MIN)`, arithmetic overflow, `sum` overflow) are handled with
  checked arithmetic and reported as `OVERFLOW`.
- The process runs the evaluator on a 64 MiB stack so deeply nested
  expressions do not abort; an unexpected panic exits with code 3.
