#!/usr/bin/env python3
"""Run the hand-written self-test workflows and verify their output.

Usage (from submissions/round-7/python):
    python3 selftest/run_selftest.py

Exits 0 when every case matches, 1 otherwise.
"""

import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
WORKFLOW = os.path.join(ROOT, "workflow.py")

EXPECTED = {
    "math_pipeline.json": {
        "exit": 0,
        "status": "completed",
        "results": {
            "nums": [1, 2, 3, 4, 5, 6, 7, 8, 9, 10],
            "shifted": [3, 6, 9, 12, 15, 18, 21, 24, 27, 30],
            "total": 165,
            "avgish": 16,
        },
        "execution_order": ["nums", "shifted", "total", "avgish"],
        "skipped": [],
    },
    "string_pipeline.json": {
        "exit": 0,
        "status": "completed",
        "results": {
            "raw": "  Hello, World!  ",
            "trimmed": "Hello, World!",
            "lower": "hello, world!",
            "parts": ["hello", "world!"],
            "joined": "hello_world!",
            "loud": "HELLO_WORLD!",
            "size": 12,
        },
        "execution_order": ["raw", "trimmed", "lower", "parts", "joined",
                            "loud", "size"],
        "skipped": [],
    },
    "objects.json": {
        "exit": 0,
        "status": "completed",
        "results": {
            "base": {"b": 2, "a": 1},
            "merged": {"a": 9, "b": 2, "c": 3},
            "picked": {"a": 9, "c": 3, "z": None},
            "omitted": {"a": 9, "c": 3},
            "key_list": ["a", "b", "c"],
            "value_list": [9, 2, 3],
        },
        "execution_order": ["base", "merged", "picked", "omitted",
                            "key_list", "value_list"],
        "skipped": [],
    },
    "skipped_step.json": {
        "exit": 0,
        "status": "completed",
        "results": {"x": 3, "y": None, "z": "after-skip"},
        "execution_order": ["x", "y", "z"],
        "skipped": ["y"],
    },
    "retry.json": {
        "exit": 0,
        "status": "completed",
        "results": {"flaky": "ok", "after": "ok!"},
        "execution_order": ["flaky", "after"],
        "skipped": [],
    },
    "references.json": {
        "exit": 0,
        "status": "completed",
        "results": {
            "obj": {"user": {"name": "Ada"}, "items": [10, 20, 30]},
            "name": "Ada",
            "second": 20,
            "echoed": {"who": "Ada", "n": 10},
        },
        "execution_order": ["obj", "name", "second", "echoed"],
        "skipped": [],
    },
    "conditions.json": {
        "exit": 0,
        "status": "completed",
        "results": {"a": 5, "all_true": "yes", "any_false": None},
        "execution_order": ["a", "all_true", "any_false"],
        "skipped": ["any_false"],
    },
    "arrays.json": {
        "exit": 0,
        "status": "completed",
        "results": {
            "vals": [3, 1, 2, 1, 3],
            "sorted": [1, 1, 2, 3, 3],
            "uniq": [3, 1, 2],
            "rev": [3, 1, 2, 1, 3],
            "sliced": [1, 2, 1],
            "nested": [[1, 2], 3, [4]],
            "flat": [1, 2, 3, 4],
            "reduced": 18,
        },
        "execution_order": ["vals", "sorted", "uniq", "rev", "sliced",
                            "nested", "flat", "reduced"],
        "skipped": [],
    },
    "error_overflow.json": {
        "exit": 1,
        "status": "failed",
        "results": {"big": 9223372036854775807},
        "execution_order": ["big"],
        "skipped": [],
        "error": {"code": "ARITHMETIC_OVERFLOW", "step": "boom"},
    },
    "validation_cycle.json": {
        "exit": 2,
        "status": "failed",
        "results": {},
        "execution_order": [],
        "skipped": [],
        "error": {"code": "CYCLIC_DEPENDENCY", "step": None},
    },
}


def fail(msg):
    print("FAIL: " + msg)
    return 1


def check(name, expected):
    path = os.path.join(HERE, name)
    proc = subprocess.run(
        [sys.executable, WORKFLOW, "run", path],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    problems = []
    if proc.returncode != expected["exit"]:
        problems.append("exit %r != %r" % (proc.returncode,
                                           expected["exit"]))
    try:
        response = json.loads(proc.stdout.decode("utf-8"))
    except Exception as e:
        problems.append("stdout is not valid JSON: %s (stdout=%r)"
                        % (e, proc.stdout[:200]))
        response = None
    if response is not None:
        for key in ("status", "results", "execution_order", "skipped",
                    "error"):
            if key not in response:
                problems.append("missing response field %r" % key)
        if response.get("status") != expected["status"]:
            problems.append("status %r != %r"
                            % (response.get("status"), expected["status"]))
        if response.get("results") != expected["results"]:
            problems.append("results %r != %r"
                            % (response.get("results"), expected["results"]))
        if response.get("execution_order") != expected["execution_order"]:
            problems.append("execution_order %r != %r"
                            % (response.get("execution_order"),
                               expected["execution_order"]))
        if response.get("skipped") != expected["skipped"]:
            problems.append("skipped %r != %r"
                            % (response.get("skipped"), expected["skipped"]))
        exp_error = expected.get("error")
        got_error = response.get("error")
        if exp_error is None:
            if got_error is not None:
                problems.append("expected error null, got %r" % (got_error,))
        else:
            if not isinstance(got_error, dict):
                problems.append("expected error object, got %r"
                                % (got_error,))
            else:
                if got_error.get("code") != exp_error["code"]:
                    problems.append("error.code %r != %r"
                                    % (got_error.get("code"),
                                       exp_error["code"]))
                if got_error.get("step") != exp_error["step"]:
                    problems.append("error.step %r != %r"
                                    % (got_error.get("step"),
                                       exp_error["step"]))
    if problems:
        for p in problems:
            print("FAIL %s: %s" % (name, p))
        if proc.stderr:
            print("  stderr: %s" % proc.stderr.decode("utf-8", "replace"))
        return 1
    print("ok   %s" % name)
    return 0


def main():
    failures = 0
    for name in sorted(EXPECTED):
        failures += check(name, EXPECTED[name])
    print()
    if failures:
        print("%d self-test(s) failed" % failures)
        return 1
    print("all %d self-tests passed" % len(EXPECTED))
    return 0


if __name__ == "__main__":
    sys.exit(main())
