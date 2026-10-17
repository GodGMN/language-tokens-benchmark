# Methodology

## What is measured

Two independent things per implementation:

1. **Development cost** — provider-reported tokens from the agent's session
   (`input`, `output`, `cacheRead`, `cacheWrite`, `reasoning`, `totalTokens`,
   `cost`), plus turns and tool calls.
2. **Correctness** — pass rate on a language-agnostic conformance suite that
   drives the binary through its CLI only.

The leaderboard ranks **conforming** implementations by `totalTokens`. Output
tokens are reported too and, in practice, rank the same way; they are the
cleaner "how much did the model write" measure.

Derived metrics (reported, not used for ranking):

- `gen eff (k)` = passed / output_tokens × 1000
- `tot eff (k)` = passed / total_tokens × 1000
- `rel cost` = total_tokens ÷ best conforming total

## Token collection

`benchmark/collect.py` parses pi session JSONL files. pi records the provider's
usage on every assistant message:

```
~/.pi/agent/sessions/--<cwd>--/<timestamp>_<session-id>.jsonl
```

Sums include standalone `usage` entries (e.g. cache warming) and `compaction`
entries. Reasoning is reported separately and is **not** added twice: providers
already include it in `totalTokens`. Nothing is estimated — if a provider does
not report usage, it is absent.

## Fairness controls

- Same model, provider, thinking level and temperature for every language in a
  round (recorded in `round.json`).
- Same prompt template, differing only in the language name and adapter commands.
- One-shot: no follow-up corrections (repair rounds, if any, are reported
  separately).
- No access to other implementations or to the holdout suite (see isolation.md).
- Deterministic tests: no time, randomness, network or absolute paths.

## Spec versioning

The SPEC will be clarified over time. To keep rounds comparable:

- `round.json` declares `spec_version` (default 1).
- A holdout fixture may declare `"since": N`. It is **scored** only when the
  round's `spec_version >= N`, and otherwise reported as *future-spec*.

So a round is always judged against the spec as it existed when the round ran.
Round 1, for example, scores 107/107 for every implementation against spec v1;
the seven rules added in v2 (integer-literal strictness, `INT64_MIN % -1`,
empty-search `replace`, strict condition fields) are shown separately and are
not counted against it.

## Known limitations

- **One sample per (model, language) per round.** Two rounds of the same model
  and task already show large movement: total tokens for a language changed by
  up to ~4x, and the ranking order changed substantially (see
  `results/compare.md`). Output tokens are much more stable than total tokens,
  but a single round is still one sample. Report ranges across rounds, not a
  single point estimate.
- `totalTokens` is dominated by cache reads for long sessions; that is why
  output tokens are reported alongside.
- A language that is easy to write may still be hard to make *correct*; the
  holdout suite mitigates but cannot eliminate overfitting to the public suite.
