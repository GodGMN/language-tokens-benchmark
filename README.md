# Language Tokens Benchmark

How many tokens does a model need to implement the *same* program in different languages?

This repo asks one question: given an identical, fully-specified CLI program, how many session tokens does an agent spend to implement it correctly in **Python, JavaScript, Go, Rust and C**? Each implementation is produced by an independent one-shot agent session, then judged by a language-agnostic conformance suite that only talks to the binary through its CLI.

A small, not-too-serious experiment in **agent-session efficiency**. The current
data covers one model configuration, five languages and three tasks. It does
not establish a general ranking of programming languages.

**Start here:** [conclusions](docs/conclusions.md) ·
[methodology](docs/methodology.md) · [evaluation corrections](docs/evaluation-notes.md) ·
[MIT license](LICENSE)

The original result artifacts are [preserved](history/2026-10-18-pre-grader-fix/).
Current results use the corrected JSON grader; historical usage totals are
unchanged. Raw provider sessions are not included, so usage and historical
isolation cannot be independently audited from this checkout.

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

One autonomous session per sample, `thinking=high`. Agents could use tools and
self-tests. Tables below order by session output tokens; generated per-round
reports retain their original total-token ordering. These are different metrics.
Passing the fixture suite is a prerequisite for ranking, not proof of full correctness.

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

### Across tasks — descriptive mean output tokens

This combines workflow medians with single samples for expr and patch. It is
not a statistically established overall leaderboard; larger-token tasks have
more influence on this arithmetic mean. The small Go/Python gap does not
establish a winner.

| lang | workflow | expr | patch | mean |
|---|---|---|---|---|
| Go | 102,106 | 60,130 | 42,905 | **68,380** |
| Python | 96,132 | 73,983 | 37,514 | **69,210** |
| JavaScript | 95,022 | 60,022 | 66,718 | 73,921 |
| Rust | 128,855 | 59,463 | 44,765 | 77,694 |
| C | 120,000 | 71,378 | 77,186 | 89,521 |

### What this supports

- **Task-dependent results.** Language order changes between tasks in this
  dataset. The single-sample expr and patch results need replication.
- **Substantial run-to-run variation.** Workflow v2 has six samples per
  language. Read the medians together with their ranges, not as precise rankings.
- **A limited correctness claim.** Recorded submissions pass the scored suite.
  The suite is finite and the grader needed corrections; this does not establish
  that correctness is saturated for these problem domains.
- **Session usage, not source length.** Output includes reasoning and other
  agent work according to provider accounting. Total tokens are dominated by
  repeated cached-context reads and are not equivalent to dollar cost.

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

## Reproducing the evaluation

No provider account or model calls are needed to evaluate committed submissions.
Install Python 3, Node, Go, Rust/Cargo, a C compiler and Make, then run:

```bash
python3 -m unittest discover -s benchmark/tests -v
python3 benchmark/rescore.py
```

For one task round:

```bash
python3 benchmark/rescore.py --round round-9-patch
```

This builds, tests and regenerates reports using the existing `metrics.json`.
It does not reproduce the original generation sessions or their token counts.
Test times reflect the machine doing the rescoring.

For a **new generation round**, create a new round id, session ids and empty
submission directories. Generate prompts with
`python3 benchmark/make_prompts.py --round <new-round>`, then launch in an
actually isolated environment. See [contributing](CONTRIBUTING.md) and
[isolation](docs/isolation.md). Do not relaunch historical round ids over their
published submissions.

`collect.py` reads original pi session JSONL and sums provider-reported usage.
It needs those session files on the collecting machine; a fresh clone only has
the aggregates. See [methodology](docs/methodology.md).

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

- Shared task specs and language-specific adapters; model/provider/thinking
  metadata recorded per round. Temperature and seed are not recorded.
- Prompts forbid reading other solutions and the suite. Working-copy separation
  is not OS-enforced isolation; historical compliance is not independently
  auditable here.
- Holdout `since` tags exclude future-spec cases from older rounds. Future runs
  should also record the exact input and evaluation Git revisions.
- Only conforming samples enter token rankings. Success counts remain visible.
- Submissions, per-test outcomes, usage aggregates and tidy exports are committed.
  [Evaluation notes](docs/evaluation-notes.md) describe corrections and provenance.

## Roadmap

- **Expand coverage.** Independently reviewed edge cases and harder tasks could add surface (nested workflows, richer expressions, streaming
  output) to test correctness beyond the current suite.
- More languages (Java, Kotlin, Swift, TypeScript…).
- More models per round (the round config already records model + version).
- A published site generated from `results/index.json`.

## License

Project code, documentation and published data are available under the
[MIT license](LICENSE). Vendored third-party dependencies retain their own
licenses and notices.
