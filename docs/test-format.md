# Test format

A test is a single JSON file under `benchmark/fixtures/<category>/` or
`benchmark/holdout/<category>/`. The category is the directory name and is what
`--category` filters on.

## Fields

| field | required | meaning |
|---|---|---|
| `name` | yes | unique-ish label, shown in output |
| `input` | yes | the workflow document handed to the CLI |
| `expected_exit_code` | yes | 0 / 1 / 2 / 3 (SPEC §5.2) |
| `expected_output` | no | exact response, compared with `error.message` ignored |
| `expected_error_code` | no | shorthand: check `error.code` without spelling the whole response |
| `stdin` | no | if `true`, pipe `input` to `run -` instead of writing a temp file |
| `since` | no | spec version this case requires (default 1); future-spec cases are not scored for older rounds |

## Comparison rules

- Property order is irrelevant; array order is exact.
- Workflow `error.message` is ignored. For expr/patch error responses, only the top-level `message` is ignored. User-data keys named `message` are always compared.
- Booleans are distinct from numbers, including inside nested objects and arrays. Numbers compare by value.
- `error.code` and `error.step` are always compared.
- If `expected_output` is present, all five envelope fields are compared. If
  only `expected_error_code` is present, only the exit code and error code are.

## Envelope

Every response must have exactly these five fields (see `benchmark/runner.py:shape_errors`):

```json
{ "status": "completed|failed", "results": {}, "execution_order": [], "skipped": [], "error": null }
```

## Example: an error case

```json
{
  "name": "mod_min_neg1",
  "since": 2,
  "input": { "version": 1, "steps": [ { "id": "r", "op": "mod", "args": { "a": -9223372036854775808, "b": -1 } } ] },
  "expected_exit_code": 1,
  "expected_error_code": "ARITHMETIC_OVERFLOW",
  "expected_output": {
    "status": "failed", "results": {}, "execution_order": [], "skipped": [],
    "error": { "code": "ARITHMETIC_OVERFLOW", "step": "r", "message": "" }
  }
}
```

## Verifying a new test

```bash
python3 benchmark/runner.py --language oracle --category <category>
```

The oracle must pass before the test is mergeable.
