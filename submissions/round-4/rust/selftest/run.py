#!/usr/bin/env python3
"""Run the hand-written self-tests against the release binary."""
import json
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent
BIN = ROOT / "target" / "release" / "workflow"


def main() -> int:
    if not BIN.exists():
        print(f"missing binary: {BIN}\nrun: cargo build --release", file=sys.stderr)
        return 2
    failures = 0
    tests = sorted(p for p in HERE.glob("*.json") if not p.name.endswith(".expected.json"))
    for test in tests:
        exp_path = test.with_suffix(".expected.json")
        if not exp_path.exists():
            print(f"SKIP {test.name} (no expected file)")
            continue
        expected = json.loads(exp_path.read_text())
        proc = subprocess.run(
            [str(BIN), "run", str(test)], capture_output=True, text=True
        )
        problems = []
        try:
            actual = json.loads(proc.stdout)
        except json.JSONDecodeError as exc:
            print(f"FAIL {test.name}: stdout is not JSON ({exc}): {proc.stdout!r}")
            failures += 1
            continue
        if proc.returncode != expected["exit"]:
            problems.append(f"exit {proc.returncode} != {expected['exit']}")
        for key in ("status", "results", "execution_order", "skipped"):
            if actual.get(key) != expected[key]:
                problems.append(
                    f"{key}: {actual.get(key)!r} != {expected[key]!r}"
                )
        exp_err = expected["error"]
        act_err = actual.get("error")
        if exp_err is None:
            if act_err is not None:
                problems.append(f"error: {act_err!r} != None")
        else:
            if act_err is None:
                problems.append(f"error: None != {exp_err!r}")
            else:
                for key in ("code", "step"):
                    if act_err.get(key) != exp_err[key]:
                        problems.append(
                            f"error.{key}: {act_err.get(key)!r} != {exp_err[key]!r}"
                        )
        if problems:
            failures += 1
            print(f"FAIL {test.name}")
            for p in problems:
                print(f"  - {p}")
        else:
            print(f"PASS {test.name}")
    print(f"\n{len(tests) - failures}/{len(tests)} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
