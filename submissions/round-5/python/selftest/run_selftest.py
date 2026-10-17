#!/usr/bin/env python3
"""Self-test runner for the workflow CLI.

Run from the submission folder:

    python3 selftest/run_selftest.py

Each case file is a workflow document.  The expected response (only the
fields checked by the benchmark) is defined below.
"""

import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CLI = os.path.join(ROOT, "workflow.py")


CASES = [
    {
        "file": "01_math.json",
        "exit": 0,
        "results": {
            "nums": [1, 2, 3, 4, 5, 6, 7, 8, 9, 10],
            "total": 55, "added": 60, "subbed": 50, "mulled": 100,
            "divved": 14, "rem": 4, "lo": 1, "hi": 10,
        },
        "order": ["nums", "total", "added", "subbed", "mulled",
                  "divved", "rem", "lo", "hi"],
        "skipped": [],
        "error": None,
    },
    {
        "file": "02_strings.json",
        "exit": 0,
        "results": {
            "raw": "  Hello, World  ",
            "trimmed": "Hello, World",
            "upper": "HELLO, WORLD",
            "lower": "hello, world",
            "replaced": "hell0, w0rld",
            "parts": ["he", "", "0, w0r", "d"],
            "joined": "he--0, w0r-d",
            "len": 12,
            "bang": "he--0, w0r-d!",
        },
        "order": ["raw", "trimmed", "upper", "lower", "replaced",
                  "parts", "joined", "len", "bang"],
        "skipped": [],
        "error": None,
    },
    {
        "file": "03_objects.json",
        "exit": 0,
        "results": {
            "obj": {"b": 2, "a": 1, "c": 3},
            "ks": ["a", "b", "c"],
            "vs": [1, 2, 3],
            "b": 2,
            "missing": None,
            "updated": {"b": 2, "a": 1, "c": 3, "d": 4},
            "merged": {"b": 2, "a": 10, "c": 3, "d": 4, "e": 5},
            "picked": {"a": 1, "c": 3, "zz": None},
            "omitted": {"a": 1, "c": 3},
        },
        "order": ["obj", "ks", "vs", "b", "missing", "updated",
                  "merged", "picked", "omitted"],
        "skipped": [],
        "error": None,
    },
    {
        "file": "04_skipped.json",
        "exit": 0,
        "results": {"base": 5, "skipped": None, "ran": 1},
        "order": ["base", "skipped", "ran"],
        "skipped": ["skipped"],
        "error": None,
    },
    {
        "file": "05_retry.json",
        "exit": 0,
        "results": {"flaky": "ok", "after": "ok!"},
        "order": ["flaky", "after"],
        "skipped": [],
        "error": None,
    },
    {
        "file": "06_arrays.json",
        "exit": 0,
        "results": {
            "nums": [1, 2, 3, 4, 5, 6],
            "mapped": [11, 12, 13, 14, 15, 16],
            "filtered": [12, 14, 16],
            "reduced": 42,
            "product": 720,
            "sorted_mixed": [1, 3, "a", False, True, None, [1], {"k": 1}],
            "reversed": [6, 5, 4, 3, 2, 1],
            "uniq": [1, "1", True, None],
            "negslice": [4, 5, 6],
            "midslice": [2, 3, 4],
            "flat": [1, 2, 3, 4],
            "stringified": ["1", '"x"', "true", "null", "[1,2]", '{"a":1}'],
        },
        "order": ["nums", "mapped", "filtered", "reduced", "product",
                  "sorted_mixed", "reversed", "uniq", "negslice",
                  "midslice", "flat", "stringified"],
        "skipped": [],
        "error": None,
    },
    {
        "file": "07_refs.json",
        "exit": 0,
        "results": {
            "data": {"user": {"name": "Ada", "tags": ["x", "y"]},
                     "items": [10, 20]},
            "name": "Ada",
            "tag0": "x",
            "item1": 20,
            "whole_user": {"name": "Ada", "tags": ["x", "y"]},
        },
        "order": ["data", "name", "tag0", "item1", "whole_user"],
        "skipped": [],
        "error": None,
    },
    {
        "file": "08_error_divzero.json",
        "exit": 1,
        "results": {"a": 1},
        "order": ["a"],
        "skipped": [],
        "error": {"code": "DIVISION_BY_ZERO", "step": "bad"},
    },
    {
        "file": "09_error_validation.json",
        "exit": 2,
        "results": {},
        "order": [],
        "skipped": [],
        "error": {"code": "INVALID_WORKFLOW", "step": None},
    },
    {
        "file": "10_error_skipped_ref.json",
        "exit": 1,
        "results": {"x": 1, "y": None},
        "order": ["x", "y"],
        "skipped": ["y"],
        "error": {"code": "SKIPPED_REFERENCE", "step": "z"},
    },
]


def run_cli(args, stdin_data=None):
    proc = subprocess.run(
        [sys.executable, CLI] + args,
        input=stdin_data,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        cwd=ROOT,
    )
    return proc


def check_case(case):
    path = os.path.join(HERE, case["file"])
    proc = run_cli(["run", path])
    problems = []
    if proc.returncode != case["exit"]:
        problems.append("exit %r != %r" % (proc.returncode, case["exit"]))
    try:
        resp = json.loads(proc.stdout.decode("utf-8"))
    except Exception as e:
        problems.append("stdout is not JSON: %r (%r)" % (proc.stdout, e))
        return problems
    if resp.get("status") != ("completed" if case["exit"] == 0 else "failed"):
        problems.append("status %r" % (resp.get("status"),))
    if resp.get("results") != case["results"]:
        problems.append("results %r != %r" % (resp.get("results"), case["results"]))
    if resp.get("execution_order") != case["order"]:
        problems.append("execution_order %r != %r" % (resp.get("execution_order"), case["order"]))
    if resp.get("skipped") != case["skipped"]:
        problems.append("skipped %r != %r" % (resp.get("skipped"), case["skipped"]))
    if case["error"] is None:
        if resp.get("error") is not None:
            problems.append("error %r != None" % (resp.get("error"),))
    else:
        err = resp.get("error") or {}
        if err.get("code") != case["error"]["code"]:
            problems.append("error.code %r != %r" % (err.get("code"), case["error"]["code"]))
        if err.get("step") != case["error"]["step"]:
            problems.append("error.step %r != %r" % (err.get("step"), case["error"]["step"]))
        if not isinstance(err.get("message"), str):
            problems.append("error.message is not a string")
    for key in ("status", "results", "execution_order", "skipped", "error"):
        if key not in resp:
            problems.append("missing response field %s" % key)
    return problems


def check_stdin():
    path = os.path.join(HERE, "01_math.json")
    with open(path, "rb") as f:
        data = f.read()
    proc = run_cli(["run", "-"], stdin_data=data)
    problems = []
    if proc.returncode != 0:
        problems.append("stdin run exit %r" % (proc.returncode,))
    try:
        resp = json.loads(proc.stdout.decode("utf-8"))
    except Exception as e:
        problems.append("stdin stdout is not JSON: %r" % (e,))
        return problems
    if resp.get("results", {}).get("total") != 55:
        problems.append("stdin run wrong total: %r" % (resp.get("results"),))
    return problems


def main():
    failures = 0
    for case in CASES:
        problems = check_case(case)
        if problems:
            failures += 1
            print("FAIL %s" % case["file"])
            for p in problems:
                print("     - %s" % p)
        else:
            print("ok   %s" % case["file"])
    problems = check_stdin()
    if problems:
        failures += 1
        print("FAIL stdin")
        for p in problems:
            print("     - %s" % p)
    else:
        print("ok   stdin")
    print()
    if failures:
        print("%d self-test case(s) failed" % failures)
        return 1
    print("all self-tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
