# expr — Python implementation

A parser + evaluator for the expression language defined in
`tasks/expr/SPEC.md` (spec version 1). Written from scratch with the Python
standard library only: no parser generators, no expression libraries, no
`eval`.

## Requirements

- Python 3.8 or newer (tested with CPython 3.11).
- No third-party dependencies. No build step.

## Run

From this directory (`submissions/round-8-expr/python`):

```bash
python3 expr.py eval request.json
python3 expr.py eval -            # request from stdin
```

`request.json` is the envelope from the spec:

```json
{ "source": "1 + 2 * 3", "vars": { "x": 5 } }
```

stdout contains exactly one compact JSON document. Diagnostics go to stderr.

Exit codes: `0` success, `1` evaluation error, `2` invalid request,
`3` internal error.

## Self-tests

```bash
python3 selftest/run.py        # summary
python3 selftest/run.py -v     # print every response
```

`selftest/cases.json` contains 237 cases covering precedence and
associativity, parentheses, short-circuiting, ternary laziness, arrays,
structural equality, every built-in function, i64 limits, and every error
code. Cases are driven through the real CLI (both stdin and request-file
modes) and compare the response JSON, exit code and `position`. The
free-form `message` field is intentionally not compared.

## Implementation notes

- `expr.py` contains the lexer, a recursive-descent parser matching the
  spec grammar, and an iterative evaluator (so long flat expressions do not
  hit Python's recursion limit).
- Values use Python `None`/`bool`/`int`/`str`/`list`; booleans are kept
  distinct from integers.
- Arithmetic is checked against the signed 64-bit range after every
  operation. Division truncates toward zero and `%` follows the sign of the
  dividend. `INT64_MIN / -1`, `INT64_MIN % -1`, `-INT64_MIN` and
  `abs(INT64_MIN)` report `OVERFLOW`.
- Error positions are 0-based code-point offsets; operator errors point at
  the operator token, function errors at the function-name token, and
  end-of-input parse errors at `len(source)`.
- Logical operators short-circuit before touching the right operand, and
  only the selected ternary branch is evaluated.
