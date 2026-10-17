# expr — expression evaluator (JavaScript / Node.js)

Implementation of the `expr` CLI described in `../../../tasks/expr/SPEC.md`
(spec version 1).

## Requirements

- Node.js >= 16 (uses `BigInt`, `String.fromCodePoint`, `fs.readFileSync(0)`).
- No external dependencies: only the Node standard library (`fs`). Nothing to
  install or build.

## Build

No build step (interpreted). Ensure `expr.js` is executable if you want to run
it directly:

```bash
chmod +x expr.js        # optional; the harness invokes `node expr.js`
```

## Run

```bash
node expr.js eval request.json
node expr.js eval -            # read the request from stdin
```

`request.json`:

```json
{ "source": "1 + 2 * 3", "vars": { "x": 5 } }
```

stdout is exactly one JSON document:

- `{"status":"ok","value":...}`
- `{"status":"error","code":"...","position":...,"message":"..."}`

Exit codes: `0` ok, `1` evaluation error, `2` invalid request, `3` internal
error. Diagnostics go to stderr.

Examples:

```bash
$ echo '{"source":"false && (1 / 0 == 0)"}' | node expr.js eval -
{"status":"ok","value":false}

$ echo '{"source":"\"a\" < 1"}' | node expr.js eval -
{"status":"error","code":"TYPE_ERROR","position":4,"message":"comparison requires two integers or two strings"}
```

## Self-test

```bash
node selftest/run.js            # summary
node selftest/run.js --verbose  # print each response
```

`selftest/cases.js` holds 256 cases: precedence/associativity, parentheses,
short-circuiting, ternary, arrays, every function, i64 boundaries, and at least
one case for every error code (`INVALID_REQUEST`, `PARSE_ERROR`, `OVERFLOW`,
`DIVISION_BY_ZERO`, `TYPE_ERROR`, `UNKNOWN_VARIABLE`, `UNKNOWN_FUNCTION`,
`ARITY_ERROR`, `EMPTY_ARRAY`), with hand-computed `position` values. The runner
spawns the real CLI for each case (some via stdin), checks the exit code, that
stdout is a single strict-JSON document, and that the response matches the
expected document (the free-form `message` is excluded, per SPEC §1).

## Design notes

- **i64 exactness.** Expression integers are JavaScript `BigInt` values and all
  arithmetic is checked against `INT64_MIN`/`INT64_MAX` after every operation.
- **Exact JSON.** `JSON.parse` would round i64 values to doubles, so the
  request is read with a small strict JSON reader (`parseJson`) that produces
  `BigInt` for integer literals, and responses are written by a matching
  compact serializer (`jsonSerialize`). `to_string` on arrays reuses the same
  serializer.
- **Hand-written recursive-descent parser** following the grammar in SPEC §3,
  one function per precedence level (`or` → `and` → `equality` → `comparison` →
  `additive` → `multiplicative` → `unary` → `primary`), with the ternary handled
  right-associatively above `||`.
- **Lazy lexer.** Tokens are scanned on demand so that a parse error earlier in
  the source wins over a malformed token later. Every AST node carries the
  source offset of the token that SPEC §6.1 requires for errors (operators,
  function names, identifiers, literals, `?`).
- **Evaluation order.** Left operand first; `&&`/`||` and the ternary evaluate
  only what is needed, so short-circuited operands can contain anything. All
  call arguments are evaluated left to right before the function's type checks.
- **Unicode.** Strings are compared by Unicode code point (equivalent to
  UTF-8 byte order), `len`/`substr`/`index_of`/`split("")` work on code points,
  and `upper`/`lower` touch ASCII only. `\uXXXX` escapes combine surrogate
  pairs into one code point.
- The code contains no `eval`, no randomness, no time or locale dependence, and
  no filesystem access other than reading the request.
