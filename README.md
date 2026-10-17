# Workflow CLI Language Benchmark

An experiment that measures how many **tokens** a language model needs to correctly implement the same declarative workflow engine in five languages: **Python, JavaScript (Node.js), Go, Rust and C**.

Each implementation is written by a coding agent (one independent session per language), then judged by a single language-agnostic conformance suite that drives the binaries through their CLI and inspects their JSON output.

> The goal is not to rank languages. The goal is to measure a model's code-generation efficiency per language under an identical functional specification.

## How it works

1. **Phase 0 — Preparation.** This repo: spec, conformance suite, fixtures, adapters, scripts.
2. **Phase 1 — One-shot implementation.** One agent session per `cli-*` folder, same model, same prompt (except the language), no access to the other implementations or to the private tests.
3. **Phase 2 — Evaluation.** The runner builds each project and runs the full conformance suite against it.
4. **Phase 3 — Optional repair.** Failed tests are fed back to each agent with an extra token budget.

## Repository layout

```text
├── PRD.md              # Product requirements (this experiment)
├── SPEC.md             # The functional contract given to the implementing agents
├── AGENTS.md           # Isolation rules for the implementing agents
├── cli-python/         # Python implementation        (agent workspace)
├── cli-javascript/     # Node.js implementation       (agent workspace)
├── cli-go/             # Go implementation            (agent workspace)
├── cli-rust/           # Rust implementation          (agent workspace)
├── cli-c/              # C implementation             (agent workspace)
├── benchmark/          # Python evaluation harness: adapters, tests, fixtures
├── results/            # Generated reports per round
└── scripts/            # Shell wrappers around the Python entry points
```

## The deliverable each agent must build

A CLI named `workflow` that executes a single command:

```bash
workflow run <workflow.json>     # or: workflow run -   (JSON via stdin)
```

stdout contains exactly one JSON document:

```json
{
  "status": "completed",
  "results": { "a": 10, "b": 20 },
  "execution_order": ["a", "b"],
  "skipped": []
}
```

Exit codes: `0` ok, `1` workflow failure, `2` validation error, `3` internal error.

The engine interprets a JSON workflow (steps with `op`, `args`, `depends_on`, `when`, `retry`), resolves DAG dependencies deterministically, supports `$ref` references between steps, conditions, retries and a fixed set of declarative operations (math, strings, arrays, objects, utilities). The full contract lives in [SPEC.md](SPEC.md).

## Running the benchmark

```bash
# Build all implementations
python3 benchmark/build.py --all

# Run the conformance suite for one language / all languages
python3 benchmark/runner.py --language go
python3 benchmark/runner.py --all

# Collect metrics + generate the comparative report
python3 benchmark/collect.py --round <id>
python3 benchmark/report.py --latest
```

Or use the wrappers in `scripts/` (`test.sh`, `test-all.sh`, `benchmark.sh`).

## Running an implementation round

Each language implementation is produced by one `pi` session started inside its `cli-*` folder with the requirements prompt (see `benchmark/rounds/`). Pi records exact per-message token usage in its session JSONL files, which `benchmark/collect.py` parses to build the per-language token ledger.

## Reading the results

Reports land in `results/<round-id>/` as raw data (per-session usage, per-test outcomes) plus a human-readable `report.md`. The main ranking: lowest total tokens among fully conforming implementations.
