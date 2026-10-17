# Contributing

This benchmark is meant to grow by pull request. There are three ways to contribute.

## 1. Add tests

Tests are the most valuable contribution. A test is one JSON file.

- **Public suite** (`benchmark/fixtures/<category>/`): visible to implementers, covers the basics.
- **Holdout suite** (`benchmark/holdout/<category>/`): edge cases scored for the leaderboard.

Format (see [`docs/test-format.md`](docs/test-format.md)):

```json
{
  "name": "division_signs",
  "input": { "version": 1, "steps": [ { "id": "r", "op": "divide", "args": { "a": -7, "b": 2 } } ] },
  "expected_exit_code": 0,
  "expected_output": {
    "status": "completed", "results": { "r": -3 },
    "execution_order": ["r"], "skipped": [], "error": null
  }
}
```

Rules for a test:

1. **Derivable from `SPEC.md` alone.** If your case needs a rule the spec does not state, open a SPEC issue/PR first. Tests that encode hidden requirements will be rejected.
2. **One behaviour per file.** Small and focused beats exhaustive.
3. **Verified against the oracle.** Every fixture must pass `benchmark/oracle/`:

   ```bash
   python3 benchmark/runner.py --language oracle --category <category>
   ```

4. **No absolute paths, times, randomness or network.**
5. **Self-contained input.** The whole workflow lives in the file.

### Spec versioning

If your test depends on a rule that was only just added to the SPEC, tag it:

```json
{ "since": 2, ... }
```

A round with `"spec_version": 1` will report it as *future-spec* and won't score it. This keeps older rounds comparable.

## 2. Add a language

1. Add `benchmark/adapters/<lang>.json`:

   ```json
   {
     "language": "kotlin",
     "build": ["kotlinc", "src", "-include-runtime", "-d", "workflow.jar"],
     "run": ["java", "-jar", "workflow.jar", "run", "{input}"],
     "run_stdin": ["java", "-jar", "workflow.jar", "run", "-"],
     "requires": ["workflow.jar"]
   }
   ```

   `build` may be `null`. `requires` lists files that must exist for the project to be considered implemented. `{input}` is replaced by a temp file path; `run_stdin` must read from stdin.

2. Add a submission slot to a round's `round.json` under `languages`.
3. Add a `prompt-<lang>.md` (generated from `benchmark/rounds/<round>/REQUIREMENTS.template.md`).
4. Implement, then run the suite.

## 3. Add a model / round

Create `benchmark/rounds/<round-id>/round.json`:

```json
{
  "id": "round-2",
  "label": "Some Model — one-shot",
  "spec_version": 2,
  "submissions": "submissions/round-2",
  "model": {
    "name": "model-id", "provider": "provider",
    "display": "Some Model", "params": "…", "thinking": "high", "date": "YYYY-MM-DD"
  },
  "languages": {
    "python": { "path": "python", "session": "round2-python" }
  }
}
```

Then:

```bash
python3 benchmark/rounds/round-2/make_prompts.py
./scripts/launch-round.sh round-2
./scripts/benchmark.sh round-2
```

## Development

```bash
python3 -m py_compile benchmark/*.py benchmark/oracle/workflow.py
python3 benchmark/runner.py --language oracle          # oracle must stay green
python3 benchmark/runner.py --all --public             # public suite only
```

The oracle is the source of truth for the harness. **A PR that changes SPEC.md must update the oracle and add/adjust fixtures in the same PR.**
