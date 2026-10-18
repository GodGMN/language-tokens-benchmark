# Methodology

## What is measured

Each sample is one autonomous agent session implementing one task in one
language. "One-shot" means no human follow-up repair prompt, not one model
response. Agents can reason, use tools, create self-tests and revise their code.

The harness reports two separate outcomes:

- Conformance to the round's scored CLI fixtures.
- Provider-reported session usage: input, output, cache reads/writes, reasoning,
  total tokens and recorded dollar cost, plus assistant turns and tool results.

Session output tokens include work beyond the final source code and may include
reasoning according to provider accounting. They do not measure source-code
length or an inherent property of a programming language.

Per-round leaderboards retain the original ranking by total tokens among
conforming submissions. The README and site order by output tokens. These metrics can
give different rankings. Total tokens count repeated context processing and are
not a price: cached input and generated output have different rates. Use
recorded dollar costs for the historical financial comparison; they are not a
quote for a new run.

Derived `gen eff (k)` and `tot eff (k)` divide passed fixtures by thousands of
output or total tokens. They depend on fixture granularity and should not be
compared between different tasks. `rel cost` is historically named: it is a
ratio of total tokens, not dollars.

## Data and auditability

`benchmark/collect.py` sums usage from pi session JSONL, including assistant,
standalone usage and compaction records. Reasoning is reported separately, not
added again to provider totals. The collector does not independently validate
the provider's accounting. Missing usage fields currently default to zero, so
zero must not be interpreted as proof that no resources were used.

Published `metrics.json` files are retained historical aggregates. The repository
does not contain the raw session JSONL, so a fresh clone can rebuild and rescore
submissions but cannot independently reconstruct usage or audit tool access.
Running the collector requires the original session files. If any configured
session is missing, collection fails without overwriting existing metrics.

## Controls and their limits

Round configs record the model identifier, provider, thinking setting and an
operator-reported model version. The provider's mutable model identifier does
not prove the exact weights used. Temperature and seed are not recorded in the
published configs; they must not be described as verified controls.

Prompts share a task specification and differ by language and adapter commands.
Dependency allowances and toolchain behavior differ by language. These are part
of the tested development environment, not eliminated confounders.

Prompts prohibit access to sibling submissions, fixtures and reference code.
The scratch launcher reduces accidental exposure but does not enforce OS-level
access restrictions. Historical isolation cannot be independently established
from this checkout. See [isolation.md](isolation.md).

## Scoring and versioning

The suite drives each CLI and checks exit code and JSON output. Object order is
irrelevant; array order matters; booleans differ from numbers. Only the
contract's free-form error-message field is ignored. User data named `message`
is compared normally. Passing means conformance to these fixtures, not proof
of complete correctness.

Each round declares `spec_version`, defaulting to 1. Holdout cases with `since`
greater than that version are reported as future-spec and excluded from the
score. Round 1 currently has 106 scored cases and 8 future-spec cases; workflow
v2 rounds have 112 scored cases. This mechanism avoids some retroactive
requirements, but the current tree alone does not freeze every historical spec
and fixture. For future rounds, record the exact spec, fixture and harness Git
revision before generation, and preserve original and corrected scores when an
evaluation defect is found.

The 2026-10-18 grader correction and rescoring are documented in
[evaluation-notes.md](evaluation-notes.md). Original published result artifacts
are preserved under [history/](../history/2026-10-18-pre-grader-fix/).

## Statistical limits

The dataset contains 45 language sessions over nine rounds for one model
configuration. Workflow has one v1 round and six v2 rounds. Expression and patch
each have one round. The five languages in a round are separate samples, not
five repeats of one language.

Aggregate only matching model group, reported version, task and spec version.
Report sample count and spread with medians. Conforming-only medians exclude
failures, so always show the success rate too. Single-sample results cannot
establish stable ordering, and these tasks do not establish performance on
other kinds of software or other models.
