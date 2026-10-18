# Evaluation correction, 2026-10-18

The first public release includes an audit and rescore of the committed
submissions. No generation sessions were rerun, no submission source was
modified, and no provider usage totals were changed.

## Outcome

All 45 language submissions still pass their scored suite after correction:

- Workflow v1: 106/106 for each of five submissions.
- Workflow v2: 112/112 for each of 30 submissions across six rounds.
- Expression evaluator: 56/56 for each of five submissions.
- JSON Patch: 38/38 for each of five submissions.

Every language and oracle was rebuilt or run using its task adapter. The export
contains 5,352 individual test outcomes, including oracle checks and future-spec
cases. No individual pass/fail outcome changed relative to the preserved
snapshot. Some future-spec cases still fail and remain excluded from older
rounds' scored counts. Passing remains a claim about these fixtures only.

The original result artifacts from commit
`372f9f93fbfb7483ca07ae18c685b2222ee531c5` are preserved under
[history/2026-10-18-pre-grader-fix/](../history/2026-10-18-pre-grader-fix/).
Per-round `evaluation.json` records the rescore date, grader version and runtime
platform. Durations in current test results come from the rescore machine;
they are not historical generation time or a controlled runtime-performance
comparison.

## Grader defects corrected

1. Python's ordinary equality treats `true` as equal to `1` and `false` as equal
   to `0`. The grader now compares JSON recursively with booleans distinct from
   numbers. Numeric values still compare by value.
2. The old comparison removed every key named `message`, including keys inside
   user documents and results. The corrected comparison ignores only workflow
   `error.message` or the top-level `message` in generic error responses.
3. A malformed scalar response could crash mismatch diagnostics. The grader now
   reports the mismatch and safely handles an error-code check on that value.
4. Missing submissions now cause a suite invocation to fail even when other
   submissions pass, rather than allowing a partial run to appear successful.

Regression checks exercise the grading results, including nested values and
legitimate free-form error messages. They do not claim the fixture suite is
exhaustive.

## Execution and reporting corrections

The ordinary launcher now changes into each submission directory before
starting the agent. A fake-agent integration check verifies the observed
working directory. This does not provide filesystem isolation.

The full-pipeline wrapper now builds the requested round instead of silently
building the default round. Missing session files make token collection fail
without overwriting existing metrics; the pipeline stops on that failure.
Comparison stability tables no longer mix tasks,
spec versions or reported model versions. Aggregate groups also separate
reported model versions. Summary and site-index records include task and
rescore provenance.

The conclusions and methodology distinguish session output, total tokens and
recorded dollar cost. Historical access isolation, raw usage accounting and
provider model version remain incompletely auditable. Documentation changes do
not retroactively establish those controls.

## Reproduce

```bash
python3 -m unittest discover -s benchmark/tests -v
python3 benchmark/rescore.py
```

This requires Python, Node, Go, Rust/Cargo, Make and a C compiler. The rescore
uses the committed usage aggregates, invokes no model and updates current
reports and exports. Logs are written to ignored `results/<round>/rescore.log`
files. Nonzero exit status identifies build or scored-conformance failures.
The preserved historical snapshot is never overwritten by this command.
