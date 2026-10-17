# `expr` — expression evaluator in C

Implementation of the expression-evaluator contract in
`tasks/expr/SPEC.md` (spec version 1).  Hand-written JSON reader/writer,
lexer, recursive-descent parser and evaluator.  No parser generators, no
expression libraries, no `eval`.

## Build

```bash
make            # produces ./expr
make clean
```

Requirements: a C11 compiler (`cc`/`gcc`/`clang`).  Only the C standard
library is used — no third-party dependencies, nothing to download.

## Run

```bash
./expr eval request.json
./expr eval -                 # request from stdin
```

`stdout` contains exactly one JSON document and nothing else.  Diagnostics go
to `stderr`.

Exit codes: `0` ok, `1` evaluation error, `2` invalid request, `3` internal
error.

Example:

```bash
$ echo '{"source":"1 + 2 * 3"}' | ./expr eval -
{"status":"ok","value":7}
```

## Self-test

```bash
sh selftest/run.sh            # or: python3 selftest/run.py
```

`selftest/run.py` runs 214 inline cases covering precedence and
associativity, parentheses, short-circuit `&&`/`||`, the ternary, arrays,
every function, every error code with exact `position`, and request
validation.  `selftest/cases/` holds plain request files (`.json`) with the
expected stdout (`.out`) and exit code (`.code`) for a quick manual check.

The self-test runner needs `python3`; the evaluator itself does not.

## Layout

| file | contents |
|---|---|
| `expr.c` | the whole implementation |
| `Makefile` | build |
| `selftest/run.py` | inline conformance cases |
| `selftest/cases/` | request / expected-output file pairs |
| `selftest/run.sh` | runs both of the above |

## Semantics notes (points where the spec leaves room)

- **Error positions** are 0-based *character* (Unicode code point) offsets,
  as the spec says "character offset"; byte offsets are converted before
  printing.
- **Integer literals** are range-checked when the literal is *evaluated*, so
  a literal in a short-circuited / unselected branch does not raise
  `OVERFLOW` (short-circuiting means no errors from the skipped side).
  Syntax is still checked for the whole expression up front (`PARSE_ERROR`).
- **Function calls**: arguments are evaluated left to right *before* the call
  is dispatched, so an argument error wins over `UNKNOWN_FUNCTION` /
  `ARITY_ERROR`.  Arity is checked before argument types.
- **`sum` / `min` / `max`** scan the array once: for each element the type is
  checked, then it is accumulated/compared (an overflow earlier in the array
  is reported before a wrong type later in the array).
- **Request `vars`**: only the five expression value types are accepted
  (null, boolean, i64 integer, string, array).  A var holding an object, a
  non-integer number or an out-of-i64 number makes the request
  `INVALID_REQUEST`.
- **JSON** output prints non-ASCII string contents as raw UTF-8 (valid JSON);
  control characters are escaped as JSON requires.  `to_string` of an array
  uses the same compact rendering.
- `i64` arithmetic uses compiler overflow builtins; `/` truncates toward
  zero and `%` follows the dividend sign (C99).  `INT64_MIN / -1` and
  `INT64_MIN % -1` are `OVERFLOW`, as is `-INT64_MIN` and `abs(INT64_MIN)`.
