# SPEC: Expression Evaluator (`expr`)

A CLI that parses and evaluates a small typed expression language. This is a
*parser + evaluator* task: precedence, associativity, short-circuiting, type
checking and precise error positions all matter.

**Spec version: 1.**

## 1. CLI

```bash
expr eval <request.json>
expr eval -              # request from stdin
```

`request.json`:

```json
{ "source": "1 + 2 * 3", "vars": { "x": 5 } }
```

- `source`: required string.
- `vars`: optional object; identifier -> JSON value. Default `{}`.
- Unknown request fields: reject (`INVALID_REQUEST`).

stdout: exactly one JSON document.

### Response (success)

```json
{ "status": "ok", "value": 7 }
```

### Response (error)

```json
{ "status": "error", "code": "TYPE_ERROR", "position": 4, "message": "..." }
```

- `position`: 0-based character offset in `source` where the offending token
  starts, or `null` when there is no meaningful position.
- `message`: free-form, never compared.

### Exit codes

| code | meaning |
|---|---|
| 0 | `status: "ok"` |
| 1 | evaluation error (parse/type/runtime) |
| 2 | invalid request (`INVALID_REQUEST`) |
| 3 | internal error |

## 2. Values

`null`, boolean, **i64 integer**, string (UTF-8), array of values.
There are no floats and no objects as expression values.

## 3. Grammar

Whitespace (space, tab, newline, carriage return) separates tokens and is
otherwise ignored. All binary operators are left-associative except the
ternary, which is right-associative. Unary operators are right-associative.

```
expr        := ternary
ternary     := or ( "?" expr ":" expr )?
or          := and ( "||" and )*
and         := equality ( "&&" equality )*
equality    := comparison ( ("==" | "!=") comparison )*
comparison  := additive ( ("<" | "<=" | ">" | ">=") additive )*
additive    := multiplicative ( ("+" | "-") multiplicative )*
multiplicative := unary ( ("*" | "/" | "%") unary )*
unary       := ("-" | "!") unary | primary
primary     := integer | string | "true" | "false" | "null"
             | identifier "(" arglist? ")"
             | identifier
             | "(" expr ")"
             | "[" (expr ("," expr)*)? "]"
arglist     := expr ("," expr)*
```

Precedence, low to high: `?:` < `||` < `&&` < `== !=` < `< <= > >=` < `+ -`
< `* / %` < unary `- !`.

### Literals

- **Integer**: one or more ASCII digits. A literal outside i64 -> `OVERFLOW`
  at the literal's position. Unary minus is a separate operator.
- **String**: double-quoted. Escapes: `\" \\ \/ \n \t \r \b \f` and
  `\uXXXX` (a single UTF-16 code unit; a surrogate pair forms one code point).
  Any other escape -> `PARSE_ERROR`.
- `true`, `false`, `null` are keywords (identifiers are not reserved otherwise).

### Identifiers

`[A-Za-z_][A-Za-z0-9_]*`. An identifier not followed by `(` is a variable
lookup in `vars`. Missing -> `UNKNOWN_VARIABLE` at the identifier's position.

## 4. Semantics

### Arithmetic `+ - * / %`

Both operands must be integers, else `TYPE_ERROR` at the operator position.
Results are i64; overflow -> `OVERFLOW` at the operator position.
`/` truncates toward zero; `%` takes the sign of the dividend (C99).
`INT64_MIN / -1` and `INT64_MIN % -1` -> `OVERFLOW`.
Division or modulo by zero -> `DIVISION_BY_ZERO` at the operator position.

Unary `-`: integer only; `-INT64_MIN` -> `OVERFLOW`.
Unary `!`: boolean only, else `TYPE_ERROR`.

### Comparison `< <= > >=`

Both integers (numeric) or both strings (byte-wise UTF-8 order). Any other
combination -> `TYPE_ERROR`. Result is boolean.

### Equality `== !=`

Structural equality over any values: numbers by value, strings by code points,
arrays element-wise, `null` equals only `null`, different types are never
equal. Result is boolean.

### Logical `&& ||`

Both operands must be booleans, else `TYPE_ERROR`. **Short-circuit**: the right
operand is not evaluated when the left decides the result. A short-circuited
operand must not produce errors (e.g. an unknown variable there is fine).

### Ternary `cond ? a : b`

`cond` must be a boolean, else `TYPE_ERROR` at `?`. Only the selected branch is
evaluated.

### Array literal

`[e1, e2, ...]` evaluates elements left to right.

## 5. Functions

Function names are matched exactly. Unknown name -> `UNKNOWN_FUNCTION`.
Wrong argument count -> `ARITY_ERROR`. Wrong argument type -> `TYPE_ERROR`.
All arguments are evaluated left to right before the call.

| function | args | result |
|---|---|---|
| `len(x)` | string or array | i64 length (string: Unicode code points) |
| `sum(a)` | array of integers | i64 sum (overflow -> OVERFLOW) |
| `min(a)` | array of integers | i64; empty -> `EMPTY_ARRAY` |
| `max(a)` | array of integers | i64; empty -> `EMPTY_ARRAY` |
| `abs(i)` | integer | \|i\|; `abs(INT64_MIN)` -> `OVERFLOW` |
| `neg(i)` | integer | -i; `neg(INT64_MIN)` -> `OVERFLOW` |
| `upper(s)` | string | ASCII uppercase only |
| `lower(s)` | string | ASCII lowercase only |
| `trim(s)` | string | strip ASCII whitespace (` \t\n\r\f\v`) |
| `contains(s, sub)` | strings | boolean, literal substring |
| `starts_with(s, p)` | strings | boolean |
| `ends_with(s, s)` | strings | boolean |
| `substr(s, start, len)` | string, integer, integer | see below |
| `index_of(s, sub)` | strings | i64 index or -1; empty `sub` -> 0 |
| `join(a, sep)` | array of strings, string | string |
| `split(s, sep)` | strings | array; empty `sep` -> one element per code point |
| `type(x)` | any | `"integer"`, `"boolean"`, `"string"`, `"array"`, `"null"` |
| `to_string(x)` | any | see below |

`substr`: `start` and `len` are i64; `len < 0` -> `TYPE_ERROR`. A negative
`start` counts from the end; values are clamped to `[0, length]`. The result is
the code-point slice `[start, start+len)`.

`to_string`: integer -> decimal; boolean -> `"true"`/`"false"`; null ->
`"null"`; string -> itself; array -> compact JSON (no spaces) with the same
string escaping as JSON.

## 6. Error codes

| code | exit | when |
|---|---|---|
| `INVALID_REQUEST` | 2 | bad request envelope (`source` missing/not a string, unknown fields, `vars` not an object) |
| `PARSE_ERROR` | 1 | syntax error, bad literal/escape |
| `OVERFLOW` | 1 | i64 overflow |
| `DIVISION_BY_ZERO` | 1 | `/` or `%` by zero |
| `TYPE_ERROR` | 1 | operand/argument of the wrong type |
| `UNKNOWN_VARIABLE` | 1 | identifier missing from `vars` |
| `UNKNOWN_FUNCTION` | 1 | call to an unknown function |
| `ARITY_ERROR` | 1 | wrong number of arguments |
| `EMPTY_ARRAY` | 1 | `min`/`max` of an empty array |

## 6.1 Position rules

`position` must be exact, because responses are compared byte-for-byte:

| error | position |
|---|---|
| `PARSE_ERROR` | start of the offending token; at end of input, `len(source)` |
| integer literal out of range | start of the literal |
| binary operator error (type, overflow, divide by zero) | the operator token |
| unary operator error | the operator token |
| `UNKNOWN_VARIABLE` | the identifier token |
| `UNKNOWN_FUNCTION`, `ARITY_ERROR`, `EMPTY_ARRAY`, function type/overflow error | the function name token |
| `TYPE_ERROR` on `&&` / `||` | the operator token |
| `TYPE_ERROR` on a ternary condition | the `?` token |
| `INVALID_REQUEST` | `null` |

## 7. Examples

| source | vars | result |
|---|---|---|
| `1 + 2 * 3` | `{}` | `{"status":"ok","value":7}` |
| `(1 + 2) * 3` | `{}` | `9` |
| `2 * -3` | `{}` | `-6` |
| `7 / 2` | `{}` | `3`, and `-7 / 2` -> `-3` |
| `x > 3 ? "big" : "small"` | `{"x":5}` | `"big"` |
| `[1, 2, len("abc")]` | `{}` | `[1, 2, 3]` |
| `false && (1 / 0 == 0)` | `{}` | `{"status":"ok","value":false}` |
| `1 +` | `{}` | `{"status":"error","code":"PARSE_ERROR","position":3}` |
| `y` | `{}` | `UNKNOWN_VARIABLE` at 0 |
| `"a" < 1` | `{}` | `TYPE_ERROR` at the `<` |
| `sum([1, 2, 3])` | `{}` | `6` |
| `type(1)` | `{}` | `"integer"` |

## 8. Determinism

No randomness, time, locale or filesystem access. Same request -> same
response. stdout contains only the JSON document.
