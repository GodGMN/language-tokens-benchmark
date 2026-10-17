#!/usr/bin/env python3
"""Run the workflow CLI against the hand-written self-test fixtures.

Each fixture NAME.json has a NAME.expected.json wrapper of the form:
    {"exit_code": 0, "response": { ... }}
The response is compared as parsed JSON (object key order is irrelevant).
"""

import json
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
PROJECT = HERE.parent
BINARY = PROJECT / "target" / "release" / "workflow"


def main() -> int:
    if not BINARY.exists():
        print(f"missing binary: {BINARY}", file=sys.stderr)
        print("build it first with: cargo build --release", file=sys.stderr)
        return 2

    fixtures = sorted(
        p for p in HERE.glob("*.json") if not p.name.endswith(".expected.json")
    )
    failures = 0
    for fixture in fixtures:
        expected_path = fixture.with_name(fixture.stem + ".expected.json")
        expected = json.loads(expected_path.read_text())
        proc = subprocess.run(
            [str(BINARY), "run", "-"],
            input=fixture.read_text(),
            text=True,
            capture_output=True,
        )
        try:
            response = json.loads(proc.stdout)
        except json.JSONDecodeError as exc:
            print(f"FAIL {fixture.name}: stdout is not one JSON document: {exc}")
            failures += 1
            continue
        if proc.returncode != expected["exit_code"]:
            print(
                f"FAIL {fixture.name}: exit {proc.returncode}, "
                f"expected {expected['exit_code']}"
            )
            failures += 1
        elif response != expected["response"]:
            print(f"FAIL {fixture.name}: response mismatch")
            print("  got:     ", json.dumps(response, sort_keys=True))
            print("  expected:", json.dumps(expected["response"], sort_keys=True))
            failures += 1
        else:
            print(f"ok   {fixture.name} (exit {proc.returncode})")

    # Also exercise the file-path invocation form, not only stdin.
    sample = HERE / "math_pipeline.json"
    expected = json.loads((HERE / "math_pipeline.expected.json").read_text())
    proc = subprocess.run(
        [str(BINARY), "run", str(sample)], text=True, capture_output=True
    )
    try:
        response = json.loads(proc.stdout)
    except json.JSONDecodeError:
        response = None
    if proc.returncode != expected["exit_code"] or response != expected["response"]:
        print("FAIL file-path invocation")
        failures += 1
    else:
        print("ok   file-path invocation")

    if failures:
        print(f"{failures} self-test(s) failed")
        return 1
    print(f"all {len(fixtures) + 1} self-tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
