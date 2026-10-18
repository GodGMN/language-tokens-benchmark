# Contributing

This benchmark is meant to grow by pull request. There are four ways to contribute.

## Repository layout

```text
SPEC.md                     The workflow task contract (frozen per spec version)
PRD.md                      Experiment design and rationale
AGENTS.md                   Rules given to implementing agents
tasks/<task>/               Spec, prompt, oracle, adapters and suites for other tasks

submissions/<round>/<lang>/ One implementation per round per language
benchmark/
  adapters/<lang>.json      How to build/run each language
  fixtures/<category>/      Public conformance suite
  holdout/<category>/       Edge-case evaluation suite (scored per spec version)
  oracle/                   Independent reference implementation (Python)
  rounds/<round>/           round.json (model, sessions) + agent prompts
  common.py build.py runner.py collect.py report.py
results/<round>/            report.md, summary.json, per-language results
site/                       Token Golf website, generated from results/dataset.json
scripts/                    Wrappers (test, test-all, benchmark, launch-round)
docs/                       Methodology, isolation, conclusions
```

## 1. Add a task

A *task* is a self-contained programming problem under `tasks/<task>/`:

```
tasks/<task>/
  SPEC.md              the contract (grammar/semantics/errors)
  PROMPT.template.md   the per-language prompt
  oracle/              reference implementation (Python)
  adapters/<lang>.json build/run commands + requires
  fixtures/<cat>/      public suite
  holdout/<cat>/       scored edge-case suite
```

Then add `"task": "<task>"` to a round's `round.json`. The harness resolves
SPEC, adapters, fixtures and oracle from `tasks/<task>/`. `workflow` is special:
it lives at the repo root for back-compat.

Rules: the spec must be complete enough that a careful implementer needs no
outside knowledge; the oracle must pass 100% of its own suite; and every error
must have a precisely defined code **and position/index**, because responses
are compared exactly.

## 2. Add tests

Tests are the most valuable contribution. A test is one JSON file.

- **Public suite** (`benchmark/fixtures/<category>/`): published for reproducibility; generation agents must not read it.
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

## 3. Add a language

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

## 4. Add a model / round

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
python3 benchmark/make_prompts.py --round round-2
./scripts/launch-round.sh round-2
./scripts/benchmark.sh round-2
```

## Website

[`site/`](site/) builds a static page from `results/dataset.json`. Copy lives
in `site/src/index.html`; every number is computed by `site/build.py`, so a new
round only needs an export and a rebuild:

```bash
python3 benchmark/export.py
python3 site/build.py          # writes site/dist/, open site/dist/index.html
```

## Development

```bash
python3 -m unittest discover -s benchmark/tests -v
python3 -m py_compile benchmark/*.py benchmark/oracle/workflow.py
python3 benchmark/runner.py --language oracle          # oracle must stay green
python3 benchmark/runner.py --all --public             # public suite only
```

The oracle is the source of truth for the harness. **A PR that changes SPEC.md must update the oracle and add/adjust fixtures in the same PR.**

## Publishing a result

Use a new round id and session ids; do not relaunch a historical round over its
published submissions. Record the input Git revision and actual isolation
mechanism. Preserve failures and timeouts. See [methodology](docs/methodology.md)
and [isolation](docs/isolation.md) before collecting a publishable round.

To rebuild and rescore existing submissions without calling a model:

```bash
python3 benchmark/rescore.py
```

This requires Python, Node, Go, Rust/Cargo, a C compiler and Make. It retains
historical usage metrics and updates derived evaluation artifacts. A nonzero
exit means a build or scored conformance check failed; inspect the round's
`rescore.log`. Do not repair historical submissions while reporting their old
generation usage as the cost of the repaired code.

Project contributions use the [MIT license](LICENSE). Vendored dependencies
retain their own licenses and notices.
