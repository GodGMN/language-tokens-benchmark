#!/usr/bin/env python3
"""Run the hand-written self-test workflows through ./workflow and compare.

Usage (from submissions/round-4/go, after `go build -o workflow .`):
    python3 selftest/check.py
"""
import json
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
CASES = ROOT / "selftest" / "cases"
BIN = ROOT / "workflow"

# Every expected document below was derived by hand from SPEC.md.
EXPECTED = {
    "01_math_pipeline.json": {
        "exit": 0,
        "response": {
            "status": "completed",
            "results": {
                "n": [0, 1, 2, 3, 4, 5, 6],
                "evens": [0, 2, 4, 6],
                "shifted": [10, 12, 14, 16],
                "total": 52,
            },
            "execution_order": ["n", "evens", "shifted", "total"],
            "skipped": [],
            "error": None,
        },
    },
    "02_string_pipeline.json": {
        "exit": 0,
        "response": {
            "status": "completed",
            "results": {
                "raw": "  Abc,Def,Ghi  ",
                "trimmed": "Abc,Def,Ghi",
                "parts": ["Abc", "Def", "Ghi"],
                "joined": "Abc-Def-Ghi",
                "upper": "ABC-DEF-GHI",
                "lowered": "abc-def-ghi",
                "n": 11,
            },
            "execution_order": ["raw", "trimmed", "parts", "joined", "upper", "lowered", "n"],
            "skipped": [],
            "error": None,
        },
    },
    "03_objects.json": {
        "exit": 0,
        "response": {
            "status": "completed",
            "results": {
                "base": {"z": 1, "a": 2, "nested": {"x": True}},
                "keys": ["a", "nested", "z"],
                "vals": [2, {"x": True}, 1],
                "merged": {"z": 9, "a": 2, "nested": {"x": True}, "b": 3},
                "picked": {"a": 2, "missing": None},
                "extended": {"z": 1, "a": 2, "nested": {"x": True}, "extra": None},
                "omitted": {"a": 2, "nested": {"x": True}},
            },
            "execution_order": ["base", "keys", "vals", "merged", "picked", "extended", "omitted"],
            "skipped": [],
            "error": None,
        },
    },
    "04_skipped.json": {
        "exit": 0,
        "response": {
            "status": "completed",
            "results": {"x": 3, "y": None, "z": 7},
            "execution_order": ["x", "y", "z"],
            "skipped": ["y"],
            "error": None,
        },
    },
    "05_retry.json": {
        "exit": 0,
        "response": {
            "status": "completed",
            "results": {"flaky": "ok", "after": 1},
            "execution_order": ["flaky", "after"],
            "skipped": [],
            "error": None,
        },
    },
    "06_conditions.json": {
        "exit": 0,
        "response": {
            "status": "completed",
            "results": {"a": 5, "b": "big", "c": "neg", "d": None},
            "execution_order": ["a", "b", "c", "d"],
            "skipped": ["d"],
            "error": None,
        },
    },
    "07_arrays.json": {
        "exit": 0,
        "response": {
            "status": "completed",
            "results": {
                "list": [3, 1, 2, 2, 4],
                "sorted": [1, 2, 2, 3, 4],
                "uniq": [3, 1, 2, 4],
                "rev": [4, 2, 2, 1, 3],
                "sliced": [1, 2, 2],
                "flat": [1, 2, 3, 4],
                "prod": 48,
                "negs": [-3, -1, -2, -2, -4],
                "odds": [3, 1],
                "strings": ["3", "1", "2", "2", "4"],
            },
            "execution_order": [
                "list", "sorted", "uniq", "rev", "sliced",
                "flat", "prod", "negs", "odds", "strings",
            ],
            "skipped": [],
            "error": None,
        },
    },
    "08_error_overflow.json": {
        "exit": 1,
        "response": {
            "status": "failed",
            "results": {"max": 9223372036854775807},
            "execution_order": ["max"],
            "skipped": [],
            "error": {
                "code": "ARITHMETIC_OVERFLOW",
                "step": "inc",
                "message": "*",  # message text is free-form
            },
        },
    },
    "09_validation_cycle.json": {
        "exit": 2,
        "response": {
            "status": "failed",
            "results": {},
            "execution_order": [],
            "skipped": [],
            "error": {
                "code": "CYCLIC_DEPENDENCY",
                "step": None,
                "message": "*",
            },
        },
    },
    "10_utilities.json": {
        "exit": 0,
        "response": {
            "status": "completed",
            "results": {
                "obj": {"b": 2, "a": {"x": 1}},
                "nested": 1,
                "eq": True,
                "cmp": True,
                "both": False,
                "neither": False,
                "neg": True,
                "tnull": "null",
                "tnum": "number",
            },
            "execution_order": ["obj", "nested", "eq", "cmp", "both", "neither", "neg", "tnull", "tnum"],
            "skipped": [],
            "error": None,
        },
    },
}


def compare(actual, expected, path="response"):
    problems = []
    if isinstance(expected, dict):
        if not isinstance(actual, dict):
            return [f"{path}: expected object, got {actual!r}"]
        for key, value in expected.items():
            if key not in actual:
                problems.append(f"{path}.{key}: missing")
            elif value == "*":
                continue
            else:
                problems.extend(compare(actual[key], value, f"{path}.{key}"))
        for key in actual:
            if key not in expected:
                problems.append(f"{path}.{key}: unexpected")
        return problems
    if isinstance(expected, list):
        if not isinstance(actual, list):
            return [f"{path}: expected array, got {actual!r}"]
        if len(actual) != len(expected):
            problems.append(f"{path}: length {len(actual)} != {len(expected)}")
        for i, (a, e) in enumerate(zip(actual, expected)):
            problems.extend(compare(a, e, f"{path}[{i}]"))
        return problems
    if isinstance(expected, bool) or actual is None or expected is None:
        if actual != expected or type(actual) is not type(expected):
            problems.append(f"{path}: {actual!r} != {expected!r}")
        return problems
    if isinstance(expected, (int, float)) and isinstance(actual, (int, float)):
        if actual != expected:
            problems.append(f"{path}: {actual!r} != {expected!r}")
        return problems
    if actual != expected:
        problems.append(f"{path}: {actual!r} != {expected!r}")
    return problems


def main():
    if not BIN.exists():
        sys.exit(f"missing {BIN}; run: go build -o workflow .")
    failures = 0
    for name in sorted(EXPECTED):
        fixture = CASES / name
        data = fixture.read_bytes()
        proc = subprocess.run([str(BIN), "run", "-"], input=data, capture_output=True)
        try:
            actual = json.loads(proc.stdout)
        except Exception:
            print(f"FAIL {name}: stdout is not JSON: {proc.stdout!r}")
            failures += 1
            continue
        expected = EXPECTED[name]
        problems = compare(actual, expected["response"])
        if proc.returncode != expected["exit"]:
            problems.insert(0, f"exit {proc.returncode} != {expected['exit']}")
        if problems:
            failures += 1
            print(f"FAIL {name}")
            for problem in problems:
                print(f"  {problem}")
            if proc.stderr:
                print(f"  stderr: {proc.stderr.decode(errors='replace').strip()}")
        else:
            print(f"ok   {name}")
    if failures:
        print(f"\n{failures} self-test(s) failed")
        return 1
    print(f"\nall {len(EXPECTED)} self-tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
