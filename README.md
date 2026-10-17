# Language Tokens Benchmark

How many tokens does a model need to implement the *same* program in different languages?

This repo asks one question: given an identical, fully-specified CLI program, how many tokens does a frontier model spend to implement it correctly in **Python, JavaScript, Go, Rust and C**? Each implementation is produced by an independent one-shot agent session, then judged by a language-agnostic conformance suite that only talks to the binary through its CLI.

It is a measure of **code-generation efficiency per language**, not a ranking of languages.

## Tasks

Three self-contained programming tasks, each JSON-in/JSON-out and deterministic.
A round targets one task; every language implements the same task from the same
SPEC.

| task | what it is | surface |
|---|---|---|
| **workflow** | declarative workflow engine: DAG execution, `$ref`, conditions, retries, 40+ operations | [`SPEC.md`](SPEC.md) |
| **expr** | expression evaluator: tokenizer, precedence parser, i64 arithmetic, short-circuiting, 18 builtins, exact error positions | [`tasks/expr/SPEC.md`](tasks/expr/SPEC.md) |
| **patch** | JSON Patch engine: RFC 6901 pointers, add/remove/replace/move/copy/test, atomicity, precise error taxonomy | [`tasks/patch/SPEC.md`](tasks/patch/SPEC.md) |

## Results — DeepSeek V4.1 Flash (1017)

One-shot, `thinking=high`. All implementations conform on every task, so these
rank token efficiency. **The ranking changes by task.**

### workflow — 6 samples (median)

| rank | lang | conforming | output tok median | total tok median | turns |
|---|---|---|---|---|---|
| 1 | JavaScript | 6/6 | 95,022 | 3.76M | 47 |
| 2 | Python | 6/6 | 96,132 | 4.63M | 56 |
| 3 | Go | 6/6 | 102,106 | 3.80M | 47 |
| 4 | C | 6/6 | 120,000 | 6.17M | 58 |
| 5 | Rust | 6/6 | 128,855 | 6.78M | 71 |

### expr — 1 sample

| rank | lang | conforming | output tok | total tok | turns |
|---|---|---|---|---|---|
| 1 | Rust | 56/56 | 59,463 | 1.07M | 23 |
| 2 | JavaScript | 56/56 | 60,022 | 1.24M | 22 |
| 3 | Go | 56/56 | 60,130 | 1.41M | 30 |
| 4 | C | 56/56 | 71,378 | 1.89M | 29 |
| 5 | Python | 56/56 | 73,983 | 2.52M | 38 |

### patch — 1 sample

| rank | lang | conforming | output tok | total tok | turns |
|---|---|---|---|---|---|
| 1 | Python | 38/38 | 37,514 | 0.70M | 20 |
| 2 | Go | 38/38 | 42,905 | 0.71M | 21 |
| 3 | Rust | 38/38 | 44,765 | 0.93M | 28 |
| 4 | JavaScript | 38/38 | 66,718 | 1.56M | 29 |
| 5 | C | 38/38 | 77,186 | 2.36M | 41 |

### Across tasks — mean output tokens

| lang | workflow | expr | patch | mean |
|---|---|---|---|---|
| Go | 102,106 | 60,130 | 42,905 | **68,380** |
| Python | 96,132 | 73,983 | 37,514 | **69,210** |
| JavaScript | 95,022 | 60,022 | 66,718 | 73,921 |
| Rust | 128,855 | 59,463 | 44,765 | 77,694 |
| C | 120,000 | 71,378 | 77,186 | 89,521 |

### What this shows

- **Efficiency is task-dependent.** Python is cheapest on `patch` but last on
  `expr`; JavaScript wins `workflow` but is 4th on `patch`; Rust is 1st on
  `expr` and last on `workflow`. No language wins every task.
- **Go and Python are the most consistent** across the three tasks; **C is
  consistently the most expensive** (2× Python on `patch`).
- **Correctness is saturated.** Across every round so far, no implementation
  failed a test that wasn't a defect in my own suite. Frontier models implement
  parsers, patch engines and workflow engines reliably; the discriminating
  signal is cost.
- **Variance dominates single samples.** On `workflow` one language's total
  tokens ranged 1.35M–9.72M over six identical runs. Treat n=1 rows as
  indicative; the `workflow` medians are the only multi-sample result so far.

Full data: [`results/aggregate.md`](results/aggregate.md),
[`results/compare.md`](results/compare.md),
[`results/round-8-expr/report.md`](results/round-8-expr/report.md),
[`results/round-9-patch/report.md`](results/round-9-patch/report.md).

## The task

Every agent must build a CLI that executes a declarative JSON workflow:

```bash
workflow run <workflow.json>      # or: workflow run -   (JSON on stdin)
```

It parses a DAG of steps, resolves `$ref` references between them, evaluates `when` conditions, applies retries, and executes a fixed set of 40+ operations (arithmetic on `i64`, strings, arrays, objects, utilities) — emitting exactly one JSON document on stdout with deterministic ordering and a closed set of error codes. The full contract is [`SPEC.md`](SPEC.md).

The task is deliberately sized to be **completable in one session but large enough to measure**: it has enough surface (integer overflow rules, UTF-8 code points, structural equality, total ordering across mixed types, skipped-step propagation, transient retries) that implementations cannot be gamed by a short answer, while still fitting in a single context window.

## Repository layout

```text
SPEC.md                     The functional contract (frozen per spec version)
PRD.md                      Experiment design and rationale
AGENTS.md                   Rules given to implementing agents

submissions/<round>/<lang>/ One implementation per round per language
benchmark/
  adapters/<lang>.json      How to build/run each language
  fixtures/<category>/      Public conformance suite
  holdout/<category>/       Edge-case evaluation suite (scored per spec version)
  oracle/                   Independent reference implementation (Python)
  rounds/<round>/           round.json (model, sessions) + agent prompts
  common.py build.py runner.py collect.py report.py
results/<round>/            report.md, summary.json, per-language results
scripts/                    Wrappers (test, test-all, benchmark, launch-round)
docs/                       Methodology, isolation, contributing
```

## Reproducing a round

```bash
python3 benchmark/rounds/round-1/make_prompts.py     # render per-language prompts
./scripts/launch-round.sh round-1                    # 5 agents in parallel
./scripts/benchmark.sh round-1                       # build + test + collect + report
```

`collect.py` reads pi's session JSONL and sums **provider-reported** usage (input, output, cache read/write, reasoning, total, cost) per session — no estimates. See [`docs/methodology.md`](docs/methodology.md).

## Data for analysis

Every round is exported to tidy tables for analysis:

```bash
python3 benchmark/export.py
```

| file | rows |
|---|---|
| `results/dataset.csv` | one per (round, language) sample: tokens, cost, turns, conformance |
| `results/dataset_tests.csv` | one per (round, language, test) outcome |
| `results/dataset.json` | the sample rows as JSON |

Per-round detail lives in `results/<round>/` (`report.md`, `summary.json`,
`metrics.json`, `<lang>/test-results.json`).

## Extending it

This is meant to grow by PR. See [`CONTRIBUTING.md`](CONTRIBUTING.md).

- **Add tests** → drop a fixture in `benchmark/fixtures/<category>/` (public) or `benchmark/holdout/<category>/` (scored edge cases). Format is documented in [`docs/test-format.md`](docs/test-format.md).
- **Add a language** → add `benchmark/adapters/<lang>.json`, a submission dir, and a prompt template.
- **Add a model** → create `benchmark/rounds/<round>/round.json` with the model metadata; `launch-round.sh` and `report.py` pick it up.

## Methodology, briefly

- **Isolation:** each agent runs in its own submission directory with its own session id, same prompt except the language, no access to other submissions or the holdout suite. See [`docs/isolation.md`](docs/isolation.md).
- **Spec versioning:** the SPEC can be clarified, but a round is always scored against the version it was run with. Holdout cases tagged `"since": N` only count once a round declares `spec_version >= N`, so nobody is penalised retroactively for a rule that did not exist.
- **Correctness first:** non-conforming implementations are listed separately with their pass rate; the leaderboard only ranks conforming ones, by total tokens.
- **Reproducible:** every round commits its model metadata, per-test outcomes and a machine-readable `summary.json`.

## Roadmap

- **Enlarge the task.** Correctness is saturated at 40+ operations; a harder
  benchmark would add surface (nested workflows, richer expressions, streaming
  output) so conformance itself becomes a discriminator again.
- More languages (Java, Kotlin, Swift, TypeScript…).
- More models per round (the round config already records model + version).
- A published site generated from `results/index.json`.
