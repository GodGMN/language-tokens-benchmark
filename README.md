# Language Tokens Benchmark

How many tokens does an AI agent spend implementing the *same* program in
**Python, JavaScript, Go, Rust and C**?

Each sample is one independent agent session given a fully specified CLI task.
The result is judged by a language-agnostic conformance suite that only talks to
the binary through its CLI. A small, not-too-serious experiment in agent-session
efficiency: one model configuration, five languages, three tasks.

**→ [Token Golf](https://tokengolf.gimeno.dev): the results, explained visually.**

## What we found

| task | samples per language | fewest output tokens | most output tokens |
|---|---|---|---|
| workflow | 6 (median) | JavaScript, 95,022 | Rust, 128,855 |
| expr | 1 | Rust, 59,463 | Python, 73,983 |
| patch | 1 | Python, 37,514 | C, 77,186 |

- **The order depends on the task.** No language keeps its rank across all three.
- **Runs vary a lot.** Python's six workflow runs range from 78k to 154k output
  tokens, a wider gap than the one between the top three languages' medians.
- **Tokens describe the session, not the program.** About 97% of all tokens are
  cached context re-reads, and output includes reasoning.
- **Passing has a limited meaning.** All 45 submissions pass their scored suite,
  but the suite is finite and the grader needed corrections.

This does not tell you which language to pick for your next project. Read the
[conclusions](docs/conclusions.md) for the reasoning and caveats, and
[evaluation notes](docs/evaluation-notes.md) for the grader corrections. Model:
DeepSeek V4.1 Flash (1017), `thinking=high`.

## Tasks

Each task is JSON in, JSON out and deterministic. A round targets one task, and
every language implements it from the same spec.

| task | what it is | spec |
|---|---|---|
| **workflow** | declarative workflow engine: DAG execution, `$ref`, conditions, retries, 40+ operations | [`SPEC.md`](SPEC.md) |
| **expr** | expression evaluator: tokenizer, precedence parser, i64 arithmetic, 18 builtins, exact error positions | [`tasks/expr/SPEC.md`](tasks/expr/SPEC.md) |
| **patch** | JSON Patch engine: RFC 6901 pointers, six operations, atomicity, precise error taxonomy | [`tasks/patch/SPEC.md`](tasks/patch/SPEC.md) |

## Reproduce

No provider account or model calls are needed to evaluate committed submissions.
Install Python 3, Node, Go, Rust/Cargo, a C compiler and Make, then run:

```bash
python3 -m unittest discover -s benchmark/tests -v
python3 benchmark/rescore.py                       # or --round round-9-patch
```

This rebuilds every submission, reruns the suite and regenerates reports from
the existing usage metrics. It does not reproduce the original generation
sessions or their token counts.

## Data

| file | contents |
|---|---|
| [`results/dataset.csv`](results/dataset.csv) / [`.json`](results/dataset.json) | one row per (round, language) sample: tokens, cost, turns, conformance |
| [`results/dataset_tests.csv`](results/dataset_tests.csv) | one row per (round, language, test) outcome |
| [`results/aggregate.md`](results/aggregate.md) | medians and ranges per task and spec version |
| [`results/compare.md`](results/compare.md) | comparisons within matching groups |
| `results/<round>/` | per-round report, summary, metrics and test results |

Regenerate the tables with `python3 benchmark/export.py`.

## Contribute

The benchmark grows by pull request. New tests are the most valuable
contribution, and new models, languages and tasks are welcome too. See
[CONTRIBUTING.md](CONTRIBUTING.md) for the repository layout and the workflow
for each, and [methodology](docs/methodology.md) and [isolation](docs/isolation.md)
before publishing a new round.

The site lives in [`site/`](site/) and is generated from `results/dataset.json`:

```bash
python3 site/build.py                              # writes site/dist/
docker build -f site/Dockerfile -t token-golf .    # nginx image, port 80
```

## Roadmap

- More samples for expr and patch, so every task has repeated runs.
- More models per round, and more languages (Java, Kotlin, Swift, TypeScript…).
- Harder tasks and independently reviewed edge cases.
- A frozen, properly isolated competitive season: see the
  [conclusions](docs/conclusions.md#a-playful-next-version-token-golf-agent-edition).

## License

Project code, documentation and published data are available under the
[MIT license](LICENSE). Vendored third-party dependencies retain their own
licenses and notices.
