# What we learned from 45 agent sessions

This is a small exploratory benchmark, published so others can inspect it,
challenge the tests and add samples. It asks how much agent-session usage is
recorded when one model configuration implements the same CLI task in five
languages. It does not tell you which language to choose for your next project.

The dataset contains nine rounds: seven workflow rounds, one expression round
and one JSON Patch round. Workflow v1 has one sample per language; workflow v2
has six. Expression and patch each have one. The model version is
operator-reported, not independently attested by the provider.

## Language order depends on the task

These are session output-token results, with correctness gated by the scored
fixture suite. Workflow uses only the six matching spec-v2 rounds.

| Task | Lowest observed output usage | Highest observed output usage | Samples per language |
|---|---|---|---|
| Workflow | JavaScript, median 95,022 | Rust, median 128,855 | 6 |
| Expression evaluator | Rust, 59,463 | Python, 73,983 | 1 |
| JSON Patch | Python, 37,514 | C, 77,186 | 1 |

The expression run puts Rust, JavaScript and Go close together. A single run
cannot establish that Rust has a stable advantage. Likewise, the workflow
JavaScript/Python median difference is small relative to the observed spread.

A cross-task arithmetic mean would place Go (68,380) just ahead of Python
(69,210). That
mixes medians with single observations and weights larger-token tasks more
heavily. We do not claim a meaningful overall winner.

## Repeated runs matter

Workflow v2 output tokens range from 78,385 to 154,250 for Python and from
67,714 to 157,602 for JavaScript. JavaScript's total tokens range from 1,351,236
to 9,721,332. One session is a poor basis for a stable language ranking.

See the [aggregate ranges](../results/aggregate.md) and
[comparisons within matching groups](../results/compare.md). These observations
do not establish statistical significance or generalize to other models.

## Tokens describe the session, not just the program

Agents read specifications, reason, write code, run self-tests and revise their
work. Output usage can include reasoning according to provider accounting.
It is not a token count of the final source files.

Across the 45 published usage rows, cached reads account for approximately
92–98% of total tokens. That makes session length and repeated context
processing central to the total-token result. Different token classes have
different prices, so dollar cost and total-token rankings need not agree.
These are historical recorded costs, not predictions of current pricing.

## Passing the tests has a limited meaning

The recorded submissions pass the scored fixtures, but the suite does not prove
complete correctness. Review found two grader defects: Python equality allowed
booleans to match numbers, and recursive filtering ignored user data named
`message`. The corrected grader now compares these cases properly. It also
handles scalar wrong answers without crashing.

The [evaluation notes](evaluation-notes.md) record the rescore outcome and link
the preserved original artifacts. Correcting a grader is not a new generation
sample and does not increase the sample count.

Some earlier specs, fixtures and oracles were corrected after submissions were
examined. The Git history documents this, but the suite is not an untouched,
prospectively frozen evaluation. Raw session logs are not in this repository,
so usage accounting and compliance with access restrictions cannot be
independently reconstructed. The scratch launcher is not an OS sandbox.

## A playful next version: Token Golf, Agent Edition

The premise is simple: how much does an agent spend getting the same small
program past the referee? Treat these results as an exhibition season.

Keep separate awards for lowest session output usage, lowest recorded dollar
cost, fewest tool calls and consistency across repeated runs. Every award
should show sample count, spread and conformance rate. Avoid a single score
that hides failures or lets a long task dominate all the others.

If an overall output score is useful, normalize each language's median to the
best median within each task, then take the geometric mean across tasks. Give
tasks equal weight and show failure rate separately. With the current
single-sample tasks, that score would remain provisional.

For a new competitive season, fix and freeze the evaluation before generation,
use actual filesystem isolation, preserve sanitized usage and tool traces, and
run at least five independent samples per language and task under equal
budgets. Five is a practical starting target, not a guarantee of sufficient
statistical power. Keep every failure and timeout in the published record.

## Reproduce or extend it

```bash
python3 -m unittest discover -s benchmark/tests -v
python3 benchmark/rescore.py
```

The rescore needs the language toolchains, reuses historical usage metrics and
makes no model calls. See [methodology](methodology.md),
[the dataset](../results/dataset.csv) and [contributing](../CONTRIBUTING.md).
