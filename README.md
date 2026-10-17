# Language Tokens Benchmark

How many tokens does a model need to implement the *same* program in different languages?

This repo asks one question: given an identical, fully-specified CLI program, how many tokens does a frontier model spend to implement it correctly in **Python, JavaScript, Go, Rust and C**? Each implementation is produced by an independent one-shot agent session, then judged by a language-agnostic conformance suite that only talks to the binary through its CLI.

It is a measure of **code-generation efficiency per language**, not a ranking of languages.

## Leaderboard

DeepSeek V4.1 Flash, one-shot, `thinking=high`. Two rounds: spec v1 (107 scored tests) and spec v2 (114, adding the clarified edge rules). Only **conforming** implementations are ranked.

**Round 2 — spec v2 (current)**

| rank | language | scored | output tok | total tok | cost | tool calls |
|---|---|---|---|---|---|---|
| 1 | **Go** | 114/114 | 110,406 | 3,455,929 | $0.163 | 38 |
| 2 | **Rust** | 114/114 | 104,408 | 4,194,694 | $0.158 | 56 |
| 3 | **JavaScript** | 114/114 | 119,392 | 4,913,756 | $0.180 | 62 |
| 4 | **C** | 114/114 | 118,885 | 6,107,267 | $0.192 | 61 |
| — | Python | 112/114 | 82,123 | 3,323,523 | $0.128 | 50 |

**Round 1 — spec v1**

| rank | language | scored | output tok | total tok | cost | tool calls |
|---|---|---|---|---|---|---|
| 1 | **JavaScript** | 107/107 | 90,736 | 2,959,548 | $0.134 | 57 |
| 2 | **Go** | 107/107 | 113,306 | 3,365,415 | $0.162 | 46 |
| 3 | **C** | 107/107 | 121,998 | 5,224,057 | $0.191 | 68 |
| 4 | **Python** | 107/107 | 125,031 | 6,222,745 | $0.200 | 76 |
| 5 | **Rust** | 107/107 | 156,063 | 9,405,161 | $0.255 | 85 |

### What the two rounds show

- **Correctness is high and mostly language-independent.** On spec v1, all five are 107/107 across 75 edge cases. On spec v2, only Python fails — it does not reject malformed `when` conditions (112/114).
- **Single-run rankings are noisy.** JavaScript was cheapest in round 1 and third in round 2; Rust went from most expensive to second. **Total tokens** swing with session length (cache reads dominate), so treat one round as one sample.
- **Output tokens are the more stable signal** (Go and C vary ~3% across rounds; JS ~32%, Rust ~49%). The leaderboard uses total tokens because that is the true cost, but output tokens tell the same story with less noise.
- **Cheapest ≠ correct.** Python is the cheapest in round 2 but is the only non-conforming implementation.

Full data: [`results/round-2/report.md`](results/round-2/report.md), [`results/compare.md`](results/compare.md).

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

- More languages (Java, Kotlin, Swift, TypeScript…).
- More models per round (the round config already records model + params).
- A published site generated from `results/*/summary.json`.
