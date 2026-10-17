# Language Tokens Benchmark

How many tokens does a model need to implement the *same* program in different languages?

This repo asks one question: given an identical, fully-specified CLI program, how many tokens does a frontier model spend to implement it correctly in **Python, JavaScript, Go, Rust and C**? Each implementation is produced by an independent one-shot agent session, then judged by a language-agnostic conformance suite that only talks to the binary through its CLI.

It is a measure of **code-generation efficiency per language**, not a ranking of languages.

## Leaderboard

**DeepSeek V4.1 Flash** (`deepseek/deepseek-flash`, DeepSeek-V4.1-Flash-1017) · one-shot · `thinking=high` · spec v2 · **6 samples per language**.

All 30 samples pass every v2-scored test (112 of 114; the other two rely on a condition rule that was only made explicit in spec v3 and are excluded — see [`docs/spec-changelog.md`](docs/spec-changelog.md)). At this task size **correctness is saturated**, so the leaderboard ranks token efficiency.

| rank | language | conforming | output tok median [range] | total tok median [range] | turns |
|---|---|---|---|---|---|
| 1 | **JavaScript** | 6/6 | **95,022** [67,714–157,602] | **3,760,233** [1,351,236–9,721,332] | 47 |
| 2 | **Python** | 6/6 | 96,132 [78,385–154,250] | 4,626,750 [3,121,625–11,614,289] | 56 |
| 3 | **Go** | 6/6 | 102,106 [69,835–170,119] | 3,802,535 [1,891,864–6,404,923] | 47 |
| 4 | **C** | 6/6 | 120,000 [108,581–154,778] | 6,165,159 [3,997,438–12,365,110] | 58 |
| 5 | **Rust** | 6/6 | 128,855 [104,408–140,846] | 6,776,508 [4,194,694–8,987,971] | 71 |

### What the samples show

- **JavaScript, Python and Go are effectively tied** on output tokens (within ~7%); Rust is clearly the most expensive (≈35% more than JavaScript, and the most turns at 71 median). C sits mid-high.
- **Variance is enormous.** JavaScript's total tokens ranged 1.35M–9.72M across six identical runs — a 7× spread. Single-run rankings are meaningless; the median and range are the result.
- **Total tokens ≈ turns × context.** Cache reads are ~95% of total, so the metric mostly tracks how many iterations a run took. Output tokens are the cleaner "effort" signal; total tokens is "cost".
- **Cheapest ≠ correct was a real effect** before the spec was clarified: JavaScript once finished in 23 turns and failed two tests. Once the ambiguity was removed, every sample conformed, and cheap runs stayed cheap.

Per-round detail: [`results/aggregate.md`](results/aggregate.md), [`results/compare.md`](results/compare.md), [`results/round-2/report.md`](results/round-2/report.md).

### Why not more rounds

Six samples already show a 7× spread, so the marginal value of more samples is in tightening the median, not in changing the ordering. The bigger lever is **task size**: to get a benchmark that discriminates on *correctness* rather than cost, the task needs more surface (see Roadmap).

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
