#!/usr/bin/env python3
"""Self-test runner for the C workflow engine.

Runs every hand-written fixture in this directory through the CLI and
compares the parsed response with the expected value.  The comparison is
structural: object key order is irrelevant, array order is significant.
"""
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BIN = os.environ.get("WORKFLOW_BIN", os.path.join(ROOT, "workflow"))


def completed(results, order, skipped=()):
    return {
        "status": "completed",
        "results": results,
        "execution_order": order,
        "skipped": list(skipped),
        "error": None,
    }


def failed(code, step):
    return {
        "status": "failed",
        "results": {},
        "execution_order": [],
        "skipped": [],
        "error": {"code": code, "step": step},
    }


CASES = [
    ("01_math_pipeline.json", 0, completed(
        {
            "nums": [1, 2, 3, 4, 5, 6, 7, 8, 9, 10],
            "evens": [2, 4, 6, 8, 10],
            "total": 30,
            "scaled": 90,
            "quot": -22,
            "rem": 2,
            "lo": 1,
            "hi": 10,
        },
        ["nums", "evens", "total", "scaled", "quot", "rem", "lo", "hi"],
    )),
    ("02_strings.json", 0, completed(
        {
            "greeting": "Hello world",
            "letters": ["h", "\u00e9", "l", "l", "o"],
            "joined": "a, b, c",
            "upper": "MIXEDCASE-\u00e9",
            "lower": "mixedcase-\u00c9",
            "trimmed": "padded",
            "replaced": "1 two 1",
            "size": 5,
        },
        ["greeting", "letters", "joined", "upper", "lower", "trimmed",
         "replaced", "size"],
    )),
    ("03_objects.json", 0, completed(
        {
            "obj": {"b": 2, "a": 1, "c": None},
            "ks": ["a", "b", "c"],
            "vs": [1, 2, None],
            "got": None,
            "missing": None,
            "setd": {"b": 2, "a": 1, "c": None, "d": 4},
            "merged": {"x": 1, "y": 3, "z": 4},
            "picked": {"a": 1, "c": None},
            "omitted": {"a": 1, "c": None},
        },
        ["obj", "ks", "vs", "got", "missing", "setd", "merged", "picked",
         "omitted"],
    )),
    ("04_skip_when.json", 0, completed(
        {
            "base": 4,
            "too_small": None,
            "all_ok": "ran",
            "any_ok": None,
            "not_ok": None,
            "empty_all": "ran",
        },
        ["base", "too_small", "all_ok", "any_ok", "not_ok", "empty_all"],
        ["too_small", "any_ok", "not_ok"],
    )),
    ("05_retry.json", 0, completed(
        {"flaky": "ok", "steady": "fine"}, ["flaky", "steady"]
    )),
    ("06_arrays.json", 0, completed(
        {
            "base": [5, 3, 5, 1, 2],
            "mapped": [15, 13, 15, 11, 12],
            "mapped_op": [10, 6, 10, 2, 4],
            "mapped_types": ["null", "boolean", "number", "string",
                             "array", "object"],
            "filtered": [5, 3, 5],
            "product": 150,
            "from100": 84,
            "sorted": [1, 3, "a", True, None, [1], {"k": 2}],
            "reversed": [3, 2, 1],
            "uniq": [1, "1", None, [1]],
            "sliced": [2, 3],
            "flat": [1, 2, [3], 4],
            "negated": [-1, 2],
        },
        ["base", "mapped", "mapped_op", "mapped_types", "filtered",
         "product", "from100", "sorted", "reversed", "uniq", "sliced",
         "flat", "negated"],
    )),
    ("07_transitive_refs.json", 0, completed(
        {"a": [10, 20], "b": 30, "c": "array", "d": "last"},
        ["a", "b", "c", "d"],
    )),
    ("08_type_is.json", 0, completed(
        {
            "vals": [1, "a", None, True, [], {}],
            "nums": [1],
            "objects": [{}],
        },
        ["vals", "nums", "objects"],
    )),
    ("09_empty.json", 0, completed({}, [])),
    ("10_retry_exhausted.json", 1, failed("STEP_FAILED", "flaky")),
]


def normalize(expected, actual):
    """Expected error messages are free-form; drop them before comparing."""
    exp = json.loads(json.dumps(expected))
    act = json.loads(json.dumps(actual))
    if exp.get("error") and act.get("error"):
        exp["error"].pop("message", None)
        act["error"].pop("message", None)
    return exp, act


def main():
    if not os.path.exists(BIN):
        print(f"missing binary: {BIN} (run make first)", file=sys.stderr)
        return 2
    failures = 0
    for name, want_rc, want in CASES:
        path = os.path.join(HERE, name)
        proc = subprocess.run([BIN, "run", path], capture_output=True,
                              text=True)
        problems = []
        if proc.returncode != want_rc:
            problems.append(f"exit {proc.returncode}, expected {want_rc}")
        try:
            got = json.loads(proc.stdout)
        except Exception as exc:  # noqa: BLE001
            problems.append(f"invalid JSON on stdout: {exc}: {proc.stdout!r}")
            got = None
        if got is not None:
            exp, act = normalize(want, got)
            if exp != act:
                problems.append(f"response mismatch:\n  want {exp}\n  got  {act}")
        if problems:
            failures += 1
            print(f"FAIL {name}")
            for p in problems:
                print("  " + p.replace("\n", "\n  "))
        else:
            print(f"ok   {name}")
    print()
    print(f"{len(CASES) - failures}/{len(CASES)} self-tests passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
