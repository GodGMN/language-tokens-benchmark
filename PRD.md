# PRD: Workflow CLI Language Benchmark

## 1. Executive Summary

Develop an experimental benchmark to compare the token-consumption efficiency of language models when implementing the same application in five languages:

- Python
- JavaScript (Node.js)
- Go
- Rust
- C

The application is a **declarative workflow execution engine driven from the CLI**, capable of interpreting JSON documents, resolving step dependencies, executing operations, managing references, conditions, errors and retries.

Each language has an independent implementation inside the same monorepo.

The project includes a language-agnostic conformance suite, a test execution system, metric collection tooling and report generation.

**Main objective:** determine how many tokens the same model needs to correctly implement an identical functional specification in different languages.

The benchmark measures a model's code-generation efficiency per language — it is not a universal measure of language superiority.

---

## 2. Objectives

### 2.1 Primary Objectives

1. Define a single, complete and unambiguous functional specification.
2. Implement it independently in five languages.
3. Evaluate all implementations with the same external tests.
4. Record input, output and total tokens used during development.
5. Measure the functional correctness of each implementation.
6. Produce a reproducible comparative report.

### 2.2 Secondary Objectives

- Compare lines of code and implementation size.
- Compare number of turns and tool invocations.
- Measure how much extra work each implementation needs to pass all tests.
- Analyze which languages produce more auxiliary code or boilerplate.
- Allow running new rounds with other models.

### 2.3 Out of Scope

- Frontends and graphical interfaces.
- HTTP services.
- Databases.
- Distribution of work across machines.
- Parallel step execution.
- Execution of arbitrary system commands.
- Performance evaluation as a primary criterion.
- Plugin systems.
- Workflows defined by executable code.
- Dependencies on pre-existing workflow engines.

The project must be complex enough to produce a substantial implementation, but scoped narrowly enough that a competent agent can complete it in a single session.

---

## 3. Repository Architecture

The project is organized as a monorepo.

```text
workflow-benchmark/
├── README.md
├── PRD.md
├── SPEC.md
├── AGENTS.md
├── .gitignore
│
├── cli-python/
│   └── README.md
├── cli-javascript/
│   └── README.md
├── cli-go/
│   └── README.md
├── cli-rust/
│   └── README.md
├── cli-c/
│   └── README.md
│
├── benchmark/
│   ├── README.md
│   ├── requirements.txt
│   ├── runner.py
│   ├── build.py
│   ├── collect.py
│   ├── report.py
│   ├── config.json
│   │
│   ├── adapters/
│   │   ├── python.json
│   │   ├── javascript.json
│   │   ├── go.json
│   │   ├── rust.json
│   │   └── c.json
│   │
│   ├── tests/
│   │   ├── test_cli.py
│   │   ├── test_operations.py
│   │   ├── test_dependencies.py
│   │   ├── test_references.py
│   │   ├── test_conditions.py
│   │   ├── test_retries.py
│   │   ├── test_errors.py
│   │   └── test_integration.py
│   │
│   ├── fixtures/
│   │   ├── basic/
│   │   ├── operations/
│   │   ├── dependencies/
│   │   ├── conditions/
│   │   ├── errors/
│   │   └── integration/
│   │
│   └── schemas/
│       ├── workflow.schema.json
│       └── result.schema.json
│
├── results/
│   └── .gitkeep
│
└── scripts/
    ├── test.sh
    ├── test-all.sh
    └── benchmark.sh
```

### 3.1 Responsibilities

**Repository root**

- `README.md`: explanation of the experiment and general instructions.
- `PRD.md`: this document, directed at the development of the benchmark.
- `SPEC.md`: the complete functional contract for the implementing agents.
- `AGENTS.md`: isolation, scope and constraint rules.

**cli-\* directories**

Each directory contains only its language's implementation, its configuration files, build instructions and optional internal tests.

Implementers are free to create subdirectories and files inside their own folder.

**benchmark/**

Contains the evaluation system code, written in Python.

This code is completely independent of the workflow engines.

**results/**

Contains reports, statistics and run logs. Results from different rounds are stored in subdirectories identified by an ID.

---

## 4. Isolation Rules

Each implementing agent has the following constraints:

1. It may only modify files inside its own `cli-*` directory.
2. It may read `SPEC.md` and its own `README.md`.
3. It may not inspect the other languages' implementations.
4. It may not modify the evaluation suite.
5. It may not modify the shared fixtures.
6. It may not modify the input/output contract.
7. It may not introduce language-specific functional differences.

The orchestrator must run each session with its own independent context and no history from the other implementations.

Folder separation is not considered sufficient isolation: it must also be enforced through permissions, sandbox configuration or independent worktrees whenever possible.

### 4.1 Dependencies

The language's standard library and general-purpose external libraries for JSON parsing and CLI are allowed.

Libraries that implement workflow engines, complete execution graphs, or the specified business operations are forbidden.

All dependencies must have their versions declared through the usual mechanisms of the ecosystem.

Building and running must not require Internet access after dependencies are installed.

Special treatment is allowed for C, due to the lack of a standard JSON library.

---

## 5. CLI Contract

All implementations must expose an equivalent command:

```bash
workflow run <workflow.json>
```

The runner may use a per-language adapter to invoke the corresponding executable.

The following must also be supported:

```bash
workflow run -
```

In this case, the JSON document is read from stdin.

### 5.1 Standard Output

Stdout contains exclusively a valid JSON document.

No logs, informational messages, progress bars or extra text are allowed.

Diagnostic logs may be written to stderr.

### 5.2 Exit Codes

- `0`: workflow executed without errors.
- `1`: error during workflow execution.
- `2`: input, argument or validation error.
- `3`: internal program error.

### 5.3 Successful Response

```json
{
  "status": "completed",
  "results": {
    "a": 10,
    "b": 20,
    "c": 30
  },
  "execution_order": ["a", "b", "c"]
}
```

### 5.4 Error Response

```json
{
  "status": "failed",
  "results": {},
  "execution_order": [],
  "error": {
    "code": "INVALID_WORKFLOW",
    "step": null,
    "message": "Missing workflow version"
  }
}
```

The format is mandatory.

The `message` text may vary, but `code`, `step`, `status` and the remaining fields are checked.

---

## 6. Workflow Format

Workflows are defined in JSON.

Example:

```json
{
  "version": 1,
  "steps": [
    {
      "id": "numbers",
      "op": "range",
      "args": {
        "start": 1,
        "end": 10
      }
    },
    {
      "id": "evens",
      "op": "filter",
      "depends_on": ["numbers"],
      "args": {
        "input": {"$ref": "numbers"},
        "predicate": "even"
      }
    },
    {
      "id": "total",
      "op": "sum",
      "depends_on": ["evens"],
      "args": {
        "input": {"$ref": "evens"}
      }
    }
  ]
}
```

### 6.1 Fields

**Workflow**

- `version`: required; only the integer `1` is accepted.
- `steps`: required array, may be empty.

**Step**

- `id`: required string, unique, matching `[A-Za-z_][A-Za-z0-9_]*`.
- `op`: required operation name.
- `args`: optional object, defaults to `{}`.
- `depends_on`: optional array of IDs, defaults to `[]`.
- `when`: optional condition.
- `retry`: optional retry policy.

Unknown fields must be rejected to avoid interpretation differences.

The `SPEC.md` file must fully document every field, type and accepted value.

---

## 7. Required Operations

The engine implements the following families.

### 7.1 Math

- `add`: sum of two numbers.
- `subtract`: difference.
- `multiply`: product.
- `divide`: division.
- `mod`: modulo.
- `sum`: sum of array elements.
- `min`: array minimum.
- `max`: array maximum.
- `range`: generation of an inclusive integer sequence.

### 7.2 Strings

- `concat`: concatenate strings.
- `split`: split a string by a literal separator.
- `join`: join an array of strings.
- `uppercase`: convert to uppercase.
- `lowercase`: convert to lowercase.
- `trim`: remove ASCII whitespace from both ends.
- `replace`: replace all occurrences of a literal substring.
- `length`: string length in Unicode code points.

### 7.3 Arrays

- `map`: transform each element with a declarative transformation.
- `filter`: filter elements.
- `reduce`: aggregate elements with a supported operator.
- `sort`: order elements.
- `reverse`: invert order.
- `unique`: remove duplicates, preserving first occurrence.
- `slice`: extract a range.
- `flatten`: flatten one level of nesting.

### 7.4 Objects

- `get`: get a property.
- `set`: create a copy of the object with a modified property.
- `keys`: get keys.
- `values`: get values.
- `merge`: combine two objects.
- `pick`: select properties.
- `omit`: exclude properties.

### 7.5 Utilities

- `constant`: return a JSON value.
- `equals`: compare two values.
- `compare`: relational comparison.
- `and`: logical operation.
- `or`: logical operation.
- `not`: negation.
- `type`: return the JSON type of a value.

Operations must be implemented without executing user-supplied code.

### 7.6 Shared Semantics

To avoid cross-language discrepancies, `SPEC.md` must define:

- Accepted argument types for each operation.
- Exact names of `args` properties.
- Empty-array handling.
- Division-by-zero handling.
- Result ordering.
- Comparisons across types.
- Structural equality rules.
- Behavior with missing values.
- Negative index and range semantics.
- Lexicographic string ordering.
- Distinction between null values and absent properties.

Arithmetic is based on signed 64-bit integers for the numeric operations in v1. Decimals are rejected in numeric parameters. Overflows produce `ARITHMETIC_OVERFLOW`.

This eliminates most of the differences between `Number`, Python integers, Go/Rust integers and C types.

To simplify portability, division operations are integer divisions truncating toward zero, and the remainder follows the sign of the dividend.

For Unicode operations, only valid UTF-8 handling is required. Uppercase/lowercase transformations are ASCII, leaving characters outside that set unchanged.

Object comparison ignores key order. When an operation returns keys or values, they are ordered by key in UTF-8 byte lexicographic order.

---

## 8. Dependencies and Execution

Steps form a directed acyclic graph.

The engine must:

1. Validate that all IDs are unique.
2. Verify that all dependencies exist.
3. Detect circular dependencies.
4. Resolve a valid topological order.
5. Execute steps sequentially.

### 8.1 Deterministic Order

Among steps executable without pending dependencies, the one appearing first in the `steps` array is chosen.

This means the order depends on the original document, not alphabetical order.

### 8.2 Cycles

Workflows with direct or indirect cycles must be rejected before any step executes.

Error code: `CYCLIC_DEPENDENCY`.

### 8.3 Missing Dependencies

If a step depends on an unknown identifier:

`UNKNOWN_DEPENDENCY`.

No partial execution may occur during structural validation.

---

## 9. References Between Steps

Arguments may contain references to earlier steps' results.

Syntax:

```json
{"$ref": "step_id"}
```

Access to nested properties and elements is also allowed:

```json
{"$ref": "step_id.user.name"}
```

Array indices are represented with decimal segments.

Example:

```json
{"$ref": "step_id.items.0"}
```

### 9.1 Resolution

Resolution is recursive inside arrays and objects.

An object is considered a reference only when it contains exactly the `$ref` property with a string value.

Objects with other properties are treated as normal objects and resolved recursively.

There is no string interpolation like `"Hello ${name}"`.

### 9.2 Constraints

- A reference may only point to the result of a declared dependency, direct or transitive.
- A reference may not point to a step that has not finished yet.
- A reference to a missing property produces `INVALID_REFERENCE`.
- Referencing a skipped step is not allowed.
- Cyclic references or self-references are invalid.

References must be resolved before invoking the operation.

---

## 10. Conditions

Each step may contain a `when` field.

Example:

```json
{
  "id": "conditional",
  "op": "constant",
  "args": {
    "value": "executed"
  },
  "when": {
    "op": "gt",
    "left": {"$ref": "total"},
    "right": 10
  },
  "depends_on": ["total"]
}
```

Supported operators:

- `eq`
- `ne`
- `gt`
- `gte`
- `lt`
- `lte`

Compound conditions are also supported:

- `all`
- `any`
- `not`

A condition must evaluate to a strict boolean.

### 10.1 Skipped Steps

If `when` is false:

- The operation is not executed.
- The step's result is `null`.
- Its ID is added to `execution_order`.
- Its ID is added to a `skipped` field, always present in completed responses.
- Referencing a skipped step later causes `SKIPPED_REFERENCE`.

The execution order includes skipped steps, but not individual execution attempts.

---

## 11. Error Handling and Retries

A step may declare:

```json
{
  "retry": {
    "max_attempts": 3
  }
}
```

`max_attempts` includes the initial execution and ranges from 1 to 5. The default is 1.

There are no waits between retries. This keeps the benchmark fast and deterministic.

### 11.1 Special Test Operation

To test retries without depending on external errors, a `fail_until` operation is added.

```json
{
  "id": "unstable",
  "op": "fail_until",
  "args": {
    "failures": 2,
    "value": "success"
  },
  "retry": {
    "max_attempts": 3
  }
}
```

The operation fails during its first two attempts and returns `"success"` on the third.

The attempt counter is local to that step and to a single workflow execution.

### 11.2 Failures

The following are distinguished:

- Validation errors: never retried.
- Reference or condition errors: never retried.
- Operation errors from arguments or arithmetic: never retried.
- Transient failures produced by `fail_until`: retriable.

If attempts are exhausted, execution stops and returns `STEP_FAILED`.

Steps after a definitive failure are not executed.

The error result includes already-completed results and the order of finished steps, without adding the failed step.

---

## 12. Language-Agnostic Conformance Suite

The suite is implemented in Python with `pytest` and `subprocess`.

It does not import or directly call the implementations' code.

Each test:

1. Prepares a JSON document.
2. Runs the corresponding CLI through the adapter.
3. Captures stdout, stderr and exit code.
4. Parses the output JSON.
5. Validates the response schema.
6. Compares results against expected values.
7. Reports the test outcome.

### 12.1 Categories

Target: 120 tests.

| Category | Tests |
|---|---:|
| CLI and validation | 15 |
| Math operations | 15 |
| Strings and arrays | 20 |
| Objects and utilities | 15 |
| Dependencies and DAG | 15 |
| References | 15 |
| Conditions | 10 |
| Retries and errors | 10 |
| Integration | 5 |
| **Total** | **120** |

These numbers are indicative. Coverage of the specification takes priority over hitting an exact count.

### 12.2 Public and Private Tests

They are split into two sets:

**Public tests**

- Basic cases of each operation.
- Dependency examples.
- Simple references.
- Simple conditions.
- Common errors.
- Integration cases.

**Evaluation tests**

- Edge cases.
- Operation combinations.
- Extensive workflows.
- Complex cycles.
- Nested references.
- Numeric limits.
- Malformed inputs.
- Combined failures and retries.

Private tests must not be accessible during implementation.

Both sets must derive from the same public specification. They may not introduce hidden requirements.

### 12.3 Fixtures

Input/output fixtures are JSON files.

Each fixture may define:

```json
{
  "name": "basic_addition",
  "input": {
    "version": 1,
    "steps": [
      {
        "id": "result",
        "op": "add",
        "args": {"a": 10, "b": 20}
      }
    ]
  },
  "expected_exit_code": 0,
  "expected_output": {
    "status": "completed",
    "results": {"result": 30},
    "execution_order": ["result"],
    "skipped": []
  }
}
```

JSON comparisons ignore property order, but strictly respect array order.

### 12.4 Protections

- Per-test execution timeout: 5 seconds.
- Configurable timeout for large workflows.
- Output size limit.
- Execution in a temporary, isolated cwd.
- Crash and signal capture.
- Rejection of stdout polluted with logs.
- No Internet access during evaluation.

A maximum input document size of 1 MiB and a maximum of 1000 steps per workflow are set.

Tests must not depend on absolute paths, dates, times or randomness.

---

## 13. Per-Language Adapters

Adapters define how each project is prepared and executed.

Conceptual example:

```json
{
  "language": "go",
  "project_dir": "cli-go",
  "build": ["go", "build", "-o", "workflow", "."],
  "run": ["./workflow", "run", "{input}"]
}
```

For interpreted languages, `build` may be `null`.

Adapters are benchmark infrastructure, not part of the implementations.

Processes are executed without an intermediate shell and with separate arguments to avoid quoting differences and injection.

The `run -` invocation is tested by passing the JSON through stdin.

### 13.1 Commands

```bash
# Build all projects
python benchmark/build.py --all

# Run the suite for one language
python benchmark/runner.py --language rust

# Run everything
python benchmark/runner.py --all

# Run only one group
python benchmark/runner.py --language go --category references

# Generate a report
python benchmark/report.py --latest
```

Shell scripts are optional wrappers.

Python is the canonical execution interface.

---

## 14. Metric Collection

Implementation behavior and agent development cost are measured separately.

### 14.1 Development Metrics

- Input tokens.
- Output tokens.
- Total tokens.
- Reasoning tokens, when the provider exposes them.
- Cache tokens, when available.
- Number of turns.
- Number of tool calls.
- Number of compilation errors.
- Number of repair iterations.
- Total session duration.
- Model and version used.
- Provider and inference configuration.

Tokens must not be reconstructed through estimates when exact provider metrics exist.

When exact metrics do not exist, they must be explicitly marked as estimates.

Reasoning tokens must not be double-counted when they are already included in the output tokens reported by the provider.

### 14.2 Code Metrics

- Total lines.
- Lines of code excluding comments and blanks.
- Number of source code files.
- Total source size in bytes.
- Compiled executable size, where applicable.
- Number of direct external dependencies.

### 14.3 Validation Metrics

- Tests passed.
- Tests failed.
- Conformance percentage.
- Errors by category.
- Suite execution time.
- Timeout or crash cases.

### 14.4 Derived Metrics

**Generation efficiency:**

`passed_tests / output_tokens * 1000`

**Total efficiency:**

`passed_tests / total_tokens * 1000`

**Relative cost:**

`total_tokens / min(total_tokens of conforming implementations)`

These metrics are auxiliary: passing individual tests does not mean two implementations are functionally equivalent.

The main ranking among conforming implementations is by the lowest total tokens to reach conformance.

Non-conforming implementations are presented separately, with their test-passing percentages.

---

## 15. Experiment Phases

### Phase 0 — Preparation

A coordinating agent creates:

- Repository structure.
- Documentation.
- Functional specification.
- Conformance suite.
- Fixtures.
- Adapters.
- Scripts.
- Report tooling.

**Acceptance criteria:** infrastructure and tests ready before launching the implementations.

The suite must be validated with an independent reference oracle or a controlled generator of expected results, without using any of the five implementations as a source of truth.

### Phase 1 — One-shot Implementation

Five independent sessions are run with:

- Same model.
- Same configuration.
- Same context budget.
- Same prompt, except the language.
- Same specification.
- No access to the evaluation tests.
- No access to the other languages.

Each agent must produce its complete implementation and run the verifications available to it within its session.

Token consumption logs and the final commit are preserved.

No human corrections are made.

### Phase 2 — Initial Evaluation

The runner builds the five projects and runs all conformance tests.

A `one-shot` report with first-attempt metrics is generated.

These results are frozen and may not be modified by later repairs.

### Phase 3 — Optional Repair

A second phase may be enabled where each agent receives the results of its failed tests.

Each agent gets an identical additional budget of tokens or turns.

Counted:

- Additional tokens and turns.
- Tests newly passed.
- Tests broken.
- Cost per additional passing test.

The final report separates one-shot and repaired results.

---

## 16. Risks and Mitigations

| Risk | Mitigation |
|---|---|
| Hidden interpretation differences in the spec | Over-specified semantics in `SPEC.md`; fixtures as executable examples. |
| Agents reading each other's code | Sandboxing, isolated worktrees, restricted file access. |
| Provider token metrics inconsistency | Record provider, model and API version; store raw usage data. |
| Non-deterministic tests | No randomness, no time, no absolute paths; fixed timeouts. |
| Agents overfitting to public tests | Hidden evaluation tests derived from the same spec. |
| C portability issues | Special JSON dependency treatment; fixed C standard. |
| One-shot session exhaustion | Context budget sized after a dry run with one language. |

---

## 17. Success Criteria

The experiment is considered successful when:

1. Five independent implementations pass the same conformance suite.
2. Token consumption per language is recorded from provider-exact metrics.
3. A comparative report can be regenerated from raw data without re-running sessions.
4. A new model can be evaluated with minimal changes (new round directory, same infrastructure).
5. The whole pipeline (build, test, collect, report) runs unattended.
