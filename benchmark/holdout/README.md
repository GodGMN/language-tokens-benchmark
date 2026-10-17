# Private evaluation fixtures (gitignored)

This directory holds the hidden evaluation set described in PRD §12.2. Its
contents are **not** committed and must not exist on disk while an implementing
agent is running in a `cli-*` folder.

The runner picks these up automatically alongside `benchmark/fixtures/` when the
directory exists:

    python3 benchmark/runner.py --all          # public + private
    python3 benchmark/runner.py --all --public # public only

Rules for anything placed here:
- Derived strictly from `SPEC.md`; no new requirements.
- Include the categories from PRD §12.1 (edge cases, nested refs, numeric
  limits, malformed input, combined retries, large workflows).
