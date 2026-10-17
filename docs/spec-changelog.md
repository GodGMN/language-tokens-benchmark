# Spec changelog

Each change bumps the spec version. A round records the version it was run
against (`spec_version` in `round.json`) and is always scored against that
version. Holdout fixtures tagged `"since": N` only count for rounds with
`spec_version >= N`.

## v3 — condition shape validation made explicit

`SPEC.md` §5 now states that a condition object must contain exactly `op` plus
the fields documented for that operator, and that an unknown `op`, a missing
required field, or an extra field is `INVALID_WORKFLOW` (structural, before any
step runs).

Before v3 this was only implied by the phrase "`when` shape" in §7.1. Every
observed implementation failure across six samples (rounds 2–7) was one of the
two cases this now states explicitly; nothing else failed. Those two fixtures
are now tagged `"since": 3`.

**Why this matters:** a holdout test that relies on an under-specified corner
measures the ambiguity, not the implementation. Once v3 existed, the same task
was fully saturated on v2 rules across 30 samples.

## v2 — numeric and ordering clarifications

- Numeric parameters must be JSON **integer literals** (`1.0`, `1e3` rejected).
- `INT64_MIN / -1` and `INT64_MIN % -1` are `ARITHMETIC_OVERFLOW`.
- `replace` with an empty `search` is `INVALID_ARG`.
- Unknown argument names are `INVALID_ARG`.
- `$ref` array indices are non-negative decimals.
- `version` must be the integer literal `1`.
- Arrays compare element-wise with the shorter-prefix rule; documented as a
  total order used by `sort`/`compare`.
- `reduce` with `multiply` folds from the identity `1`, not `0`.

## v1 — initial contract

The original specification: DAG execution, `$ref`, conditions, retries, 40+
operations, i64 arithmetic, deterministic ordering and a closed error set.
