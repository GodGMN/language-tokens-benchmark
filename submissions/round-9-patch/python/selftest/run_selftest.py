#!/usr/bin/env python3
"""Self-verification suite for patch.py.

Run from the language directory:

    python3 selftest/run_selftest.py

Every case invokes the real CLI through a subprocess, reads stdout, parses it,
and checks the response and the process exit code.  Expected values are
written by hand from SPEC.md.

Cases are represented as:
    (name, request_object_or_raw_text, expected_response, expected_exit)
If the request is a string it is sent verbatim (used for malformed JSON).
"""

import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PATCH = os.path.join(ROOT, "patch.py")


def ok(document):
    return {"status": "ok", "document": document}


def err(code, index, message="ignored"):
    return {"status": "error", "code": code, "index": index, "message": message}


# fmt: off
CASES = [
    # ---------- envelope / basic ----------
    ("empty patch keeps document",
     {"document": {"a": 1}, "patch": []},
     ok({"a": 1}), 0),
    ("document may be null",
     {"document": None, "patch": []},
     ok(None), 0),
    ("document may be a scalar",
     {"document": 42, "patch": []},
     ok(42), 0),

    # ---------- add: objects ----------
    ("add creates object member",
     {"document": {"a": 1}, "patch": [{"op": "add", "path": "/b", "value": 2}]},
     ok({"a": 1, "b": 2}), 0),
    ("add overwrites object member",
     {"document": {"a": 1}, "patch": [{"op": "add", "path": "/a", "value": 2}]},
     ok({"a": 2}), 0),
    ("add nested creates last key only",
     {"document": {"a": {}}, "patch": [{"op": "add", "path": "/a/b", "value": 3}]},
     ok({"a": {"b": 3}}), 0),
    ("add empty object key via /",
     {"document": {}, "patch": [{"op": "add", "path": "/", "value": 1}]},
     ok({"": 1}), 0),
    ("add literal '-' as object key",
     {"document": {}, "patch": [{"op": "add", "path": "/-", "value": 1}]},
     ok({"-": 1}), 0),
    ("add replaces whole document at root",
     {"document": {"a": 1}, "patch": [{"op": "add", "path": "", "value": [1]}]},
     ok([1]), 0),
    ("add null value",
     {"document": {}, "patch": [{"op": "add", "path": "/a", "value": None}]},
     ok({"a": None}), 0),

    # ---------- add: arrays ----------
    ("add array at front",
     {"document": [1, 2, 3], "patch": [{"op": "add", "path": "/0", "value": 9}]},
     ok([9, 1, 2, 3]), 0),
    ("add array middle",
     {"document": [1, 2, 3], "patch": [{"op": "add", "path": "/1", "value": 9}]},
     ok([1, 9, 2, 3]), 0),
    ("add array at len appends",
     {"document": [1, 2, 3], "patch": [{"op": "add", "path": "/3", "value": 4}]},
     ok([1, 2, 3, 4]), 0),
    ("add array with dash appends",
     {"document": [1, 2, 3], "patch": [{"op": "add", "path": "/-", "value": 4}]},
     ok([1, 2, 3, 4]), 0),
    ("add nested array element",
     {"document": {"a": [{"b": 1}]},
      "patch": [{"op": "add", "path": "/a/0/c", "value": 2}]},
     ok({"a": [{"b": 1, "c": 2}]}), 0),
    ("add into array inside array",
     {"document": [[1], [2]], "patch": [{"op": "add", "path": "/1/0", "value": 0}]},
     ok([[1], [0, 2]]), 0),

    # ---------- pointer escapes ----------
    ("pointer escape ~1 addresses slash key",
     {"document": {"a/b": 1}, "patch": [{"op": "remove", "path": "/a~1b"}]},
     ok({}), 0),
    ("pointer escape ~0 addresses tilde key",
     {"document": {"a~b": 1}, "patch": [{"op": "remove", "path": "/a~0b"}]},
     ok({}), 0),
    ("escape order: ~01 means literal ~1",
     {"document": {"a~1b": 1}, "patch": [{"op": "remove", "path": "/a~01b"}]},
     ok({}), 0),
    ("nested escaped paths",
     {"document": {"a/b": {"c~d": 1}},
      "patch": [{"op": "replace", "path": "/a~1b/c~0d", "value": 2}]},
     ok({"a/b": {"c~d": 2}}), 0),

    # ---------- remove ----------
    ("remove object member",
     {"document": {"a": 1, "b": 2}, "patch": [{"op": "remove", "path": "/a"}]},
     ok({"b": 2}), 0),
    ("remove array element shifts left",
     {"document": [1, 2, 3], "patch": [{"op": "remove", "path": "/1"}]},
     ok([1, 3]), 0),
    ("remove nested",
     {"document": {"a": {"b": 1}}, "patch": [{"op": "remove", "path": "/a/b"}]},
     ok({"a": {}}), 0),
    ("remove empty key",
     {"document": {"": 1}, "patch": [{"op": "remove", "path": "/"}]},
     ok({}), 0),

    # ---------- replace ----------
    ("replace object member",
     {"document": {"a": 1}, "patch": [{"op": "replace", "path": "/a", "value": 2}]},
     ok({"a": 2}), 0),
    ("replace array element",
     {"document": [1, 2, 3], "patch": [{"op": "replace", "path": "/1", "value": 9}]},
     ok([1, 9, 3]), 0),
    ("replace root",
     {"document": {"a": 1}, "patch": [{"op": "replace", "path": "", "value": None}]},
     ok(None), 0),

    # ---------- move ----------
    ("move object member",
     {"document": {"a": 1}, "patch": [{"op": "move", "from": "/a", "path": "/b"}]},
     ok({"b": 1}), 0),
    ("move array element forward uses remove-then-add",
     {"document": [1, 2, 3], "patch": [{"op": "move", "from": "/0", "path": "/2"}]},
     ok([2, 3, 1]), 0),
    ("move array element backward",
     {"document": [1, 2, 3], "patch": [{"op": "move", "from": "/2", "path": "/0"}]},
     ok([3, 1, 2]), 0),
    ("move to dash appends",
     {"document": {"a": 1, "b": [1]},
      "patch": [{"op": "move", "from": "/a", "path": "/b/-"}]},
     ok({"b": [1, 1]}), 0),
    ("move value to root",
     {"document": {"a": {"x": 1}, "b": 2},
      "patch": [{"op": "move", "from": "/a", "path": ""}]},
     ok({"x": 1}), 0),
    ("move across nested containers",
     {"document": {"a": {"b": [1, 2]}, "c": {}},
      "patch": [{"op": "move", "from": "/a/b/0", "path": "/c/x"}]},
     ok({"a": {"b": [2]}, "c": {"x": 1}}), 0),

    # ---------- copy ----------
    ("copy object member",
     {"document": {"a": 1}, "patch": [{"op": "copy", "from": "/a", "path": "/b"}]},
     ok({"a": 1, "b": 1}), 0),
    ("copy is a deep copy",
     {"document": {"a": {"x": 1}},
      "patch": [{"op": "copy", "from": "/a", "path": "/b"},
                {"op": "add", "path": "/b/y", "value": 2}]},
     ok({"a": {"x": 1}, "b": {"x": 1, "y": 2}}), 0),
    ("copy into descendant is allowed",
     {"document": {"a": {"b": 1}},
      "patch": [{"op": "copy", "from": "/a", "path": "/a/c"}]},
     ok({"a": {"b": 1, "c": {"b": 1}}}), 0),
    ("copy root into child",
     {"document": {"a": 1},
      "patch": [{"op": "copy", "from": "", "path": "/b"}]},
     ok({"a": 1, "b": {"a": 1}}), 0),
    ("copy from array inserts",
     {"document": [1, 2], "patch": [{"op": "copy", "from": "/0", "path": "/1"}]},
     ok([1, 1, 2]), 0),

    # ---------- test ----------
    ("test scalar succeeds",
     {"document": {"a": 1}, "patch": [{"op": "test", "path": "/a", "value": 1}]},
     ok({"a": 1}), 0),
    ("test root succeeds",
     {"document": {"a": 1}, "patch": [{"op": "test", "path": "", "value": {"a": 1}}]},
     ok({"a": 1}), 0),
    ("test objects ignore key order",
     {"document": {"a": 1, "b": {"c": 2}},
      "patch": [{"op": "test", "path": "", "value": {"b": {"c": 2}, "a": 1}}]},
     ok({"a": 1, "b": {"c": 2}}), 0),
    ("test numbers compare by value",
     {"document": {"a": 1}, "patch": [{"op": "test", "path": "/a", "value": 1.0}]},
     ok({"a": 1}), 0),
    ("test nested array",
     {"document": {"a": [1, {"b": None}]},
      "patch": [{"op": "test", "path": "/a/1/b", "value": None}]},
     ok({"a": [1, {"b": None}]}), 0),
    ("test null member exists",
     {"document": {"a": None}, "patch": [{"op": "test", "path": "/a", "value": None}]},
     ok({"a": None}), 0),

    # ---------- sequences / unicode ----------
    ("multiple operations apply in order",
     {"document": {"a": 1},
      "patch": [
          {"op": "add", "path": "/b", "value": [1]},
          {"op": "add", "path": "/b/-", "value": 2},
          {"op": "replace", "path": "/a", "value": 10},
          {"op": "test", "path": "/b", "value": [1, 2]},
      ]},
     ok({"a": 10, "b": [1, 2]}), 0),
    ("unicode key and value",
     {"document": {}, "patch": [{"op": "add", "path": "/caf\u00e9", "value": "\u2615"}]},
     ok({"caf\u00e9": "\u2615"}), 0),
    ("empty string key with escaped path remains distinct",
     {"document": {"": {"": 1}},
      "patch": [{"op": "remove", "path": "//"}]},
     ok({"": {}}), 0),

    # ---------- error: envelope INVALID_REQUEST (index null, exit 2) ----------
    ("malformed JSON", "{not json",
     err("INVALID_REQUEST", None), 2),
    ("empty input", "",
     err("INVALID_REQUEST", None), 2),
    ("top-level is array", "[]",
     err("INVALID_REQUEST", None), 2),
    ("top-level is string", '"x"',
     err("INVALID_REQUEST", None), 2),
    ("missing document", {"patch": []},
     err("INVALID_REQUEST", None), 2),
    ("missing patch", {"document": {}},
     err("INVALID_REQUEST", None), 2),
    ("patch is not array", {"document": {}, "patch": {}},
     err("INVALID_REQUEST", None), 2),
    ("unknown top-level field", {"document": {}, "patch": [], "x": 1},
     err("INVALID_REQUEST", None), 2),
    ("non-standard NaN constant", '{"document": NaN, "patch": []}',
     err("INVALID_REQUEST", None), 2),

    # ---------- error: op-shape INVALID_REQUEST (index set, exit 2) ----------
    ("operation is not object",
     {"document": {}, "patch": [5]},
     err("INVALID_REQUEST", 0), 2),
    ("missing op",
     {"document": {}, "patch": [{"path": "/a"}]},
     err("INVALID_REQUEST", 0), 2),
    ("op is not string",
     {"document": {}, "patch": [{"op": 1, "path": "/a"}]},
     err("INVALID_REQUEST", 0), 2),
    ("missing path",
     {"document": {}, "patch": [{"op": "remove"}]},
     err("INVALID_REQUEST", 0), 2),
    ("path is not string",
     {"document": {}, "patch": [{"op": "remove", "path": 1}]},
     err("INVALID_REQUEST", 0), 2),
    ("add missing value",
     {"document": {}, "patch": [{"op": "add", "path": "/a"}]},
     err("INVALID_REQUEST", 0), 2),
    ("remove with extra value",
     {"document": {"a": 1}, "patch": [{"op": "remove", "path": "/a", "value": 1}]},
     err("INVALID_REQUEST", 0), 2),
    ("move missing from",
     {"document": {}, "patch": [{"op": "move", "path": "/a"}]},
     err("INVALID_REQUEST", 0), 2),
    ("from is not a string",
     {"document": {}, "patch": [{"op": "copy", "from": 1, "path": "/a"}]},
     err("INVALID_REQUEST", 0), 2),
    ("op-shape error at index one",
     {"document": {}, "patch": [{"op": "add", "path": "/a", "value": 1},
                                 {"op": "add", "path": "/b"}]},
     err("INVALID_REQUEST", 1), 2),

    # ---------- error: UNKNOWN_OP ----------
    ("unknown op",
     {"document": {}, "patch": [{"op": "merge", "path": "/a"}]},
     err("UNKNOWN_OP", 0), 1),
    ("unknown op at second index",
     {"document": {}, "patch": [{"op": "add", "path": "/a", "value": 1},
                                 {"op": "noop", "path": "/b"}]},
     err("UNKNOWN_OP", 1), 1),

    # ---------- error: INVALID_PATH ----------
    ("path does not start with slash",
     {"document": {}, "patch": [{"op": "add", "path": "a", "value": 1}]},
     err("INVALID_PATH", 0), 1),
    ("invalid tilde escape",
     {"document": {}, "patch": [{"op": "add", "path": "/a~2", "value": 1}]},
     err("INVALID_PATH", 0), 1),
    ("trailing tilde",
     {"document": {}, "patch": [{"op": "add", "path": "/a~", "value": 1}]},
     err("INVALID_PATH", 0), 1),
    ("remove root is invalid",
     {"document": {"a": 1}, "patch": [{"op": "remove", "path": ""}]},
     err("INVALID_PATH", 0), 1),
    ("invalid from pointer",
     {"document": {"a": 1}, "patch": [{"op": "copy", "from": "a", "path": "/b"}]},
     err("INVALID_PATH", 0), 1),

    # ---------- error: PATH_NOT_FOUND ----------
    ("remove missing object member",
     {"document": {"a": 1}, "patch": [{"op": "remove", "path": "/x"}]},
     err("PATH_NOT_FOUND", 0), 1),
    ("add parent missing",
     {"document": {}, "patch": [{"op": "add", "path": "/a/b", "value": 1}]},
     err("PATH_NOT_FOUND", 0), 1),
    ("add parent is scalar",
     {"document": {"a": 1}, "patch": [{"op": "add", "path": "/a/b", "value": 2}]},
     err("PATH_NOT_FOUND", 0), 1),
    ("add root-level member on scalar document",
     {"document": 1, "patch": [{"op": "add", "path": "/a", "value": 2}]},
     err("PATH_NOT_FOUND", 0), 1),
    ("replace missing member",
     {"document": {}, "patch": [{"op": "replace", "path": "/a", "value": 1}]},
     err("PATH_NOT_FOUND", 0), 1),
    ("test missing member",
     {"document": {}, "patch": [{"op": "test", "path": "/a", "value": None}]},
     err("PATH_NOT_FOUND", 0), 1),
    ("test traverses into scalar",
     {"document": {"a": 1}, "patch": [{"op": "test", "path": "/a/b", "value": 1}]},
     err("PATH_NOT_FOUND", 0), 1),
    ("move from missing",
     {"document": {}, "patch": [{"op": "move", "from": "/a", "path": "/b"}]},
     err("PATH_NOT_FOUND", 0), 1),
    ("copy from missing",
     {"document": {}, "patch": [{"op": "copy", "from": "/a", "path": "/b"}]},
     err("PATH_NOT_FOUND", 0), 1),

    # ---------- error: INDEX_ERROR ----------
    ("add array index beyond len",
     {"document": [1], "patch": [{"op": "add", "path": "/5", "value": 0}]},
     err("INDEX_ERROR", 0), 1),
    ("add array index with leading zero",
     {"document": [1], "patch": [{"op": "add", "path": "/01", "value": 0}]},
     err("INDEX_ERROR", 0), 1),
    ("add array index negative",
     {"document": [1], "patch": [{"op": "add", "path": "/-1", "value": 0}]},
     err("INDEX_ERROR", 0), 1),
    ("remove array index equal to len",
     {"document": [1], "patch": [{"op": "remove", "path": "/1"}]},
     err("INDEX_ERROR", 0), 1),
    ("remove array dash",
     {"document": [1], "patch": [{"op": "remove", "path": "/-"}]},
     err("INDEX_ERROR", 0), 1),
    ("replace array index out of range",
     {"document": [1], "patch": [{"op": "replace", "path": "/1", "value": 0}]},
     err("INDEX_ERROR", 0), 1),
    ("test array dash",
     {"document": [1], "patch": [{"op": "test", "path": "/-", "value": 1}]},
     err("INDEX_ERROR", 0), 1),
    ("array traversal with non-numeric token",
     {"document": [[1]], "patch": [{"op": "remove", "path": "/a/0"}]},
     err("INDEX_ERROR", 0), 1),
    ("array traversal index out of range",
     {"document": [[1]], "patch": [{"op": "remove", "path": "/1/0"}]},
     err("INDEX_ERROR", 0), 1),
    ("copy from array dash",
     {"document": [1], "patch": [{"op": "copy", "from": "/-", "path": "/x"}]},
     err("INDEX_ERROR", 0), 1),
    ("move from array malformed index",
     {"document": [1], "patch": [{"op": "move", "from": "/00", "path": "/x"}]},
     err("INDEX_ERROR", 0), 1),

    # ---------- error: TEST_FAILED ----------
    ("test value mismatch",
     {"document": {"a": 1}, "patch": [{"op": "test", "path": "/a", "value": 2}]},
     err("TEST_FAILED", 0), 1),
    ("bool is not a number",
     {"document": {"a": True}, "patch": [{"op": "test", "path": "/a", "value": 1}]},
     err("TEST_FAILED", 0), 1),
    ("array order matters",
     {"document": {"a": [1, 2]},
      "patch": [{"op": "test", "path": "/a", "value": [2, 1]}]},
     err("TEST_FAILED", 0), 1),
    ("test after successful op fails at index one",
     {"document": {"a": 1},
      "patch": [{"op": "add", "path": "/b", "value": 2},
                {"op": "test", "path": "/a", "value": 99}]},
     err("TEST_FAILED", 1), 1),

    # ---------- error: INVALID_MOVE ----------
    ("move to itself",
     {"document": {"a": 1}, "patch": [{"op": "move", "from": "/a", "path": "/a"}]},
     err("INVALID_MOVE", 0), 1),
    ("move into descendant",
     {"document": {"a": 1}, "patch": [{"op": "move", "from": "/a", "path": "/a/b"}]},
     err("INVALID_MOVE", 0), 1),
    ("move root into child",
     {"document": {"a": 1}, "patch": [{"op": "move", "from": "", "path": "/a"}]},
     err("INVALID_MOVE", 0), 1),
    ("move root to root",
     {"document": {"a": 1}, "patch": [{"op": "move", "from": "", "path": ""}]},
     err("INVALID_MOVE", 0), 1),
    ("similar prefix is not a descendant",
     {"document": {"a": 1, "ab": 2},
      "patch": [{"op": "move", "from": "/a", "path": "/ab"}]},
     ok({"ab": 1}), 0),
]
# fmt: on


def run_case(request, use_file):
    if use_file:
        path = os.path.join(HERE, "_request.json")
        with open(path, "wb") as handle:
            if isinstance(request, str):
                handle.write(request.encode("utf-8"))
            else:
                handle.write(json.dumps(request).encode("utf-8"))
        try:
            proc = subprocess.run(
                [sys.executable, PATCH, "apply", path],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
        finally:
            try:
                os.unlink(path)
            except OSError:
                pass
        return proc

    if isinstance(request, str):
        raw = request.encode("utf-8")
    else:
        raw = json.dumps(request).encode("utf-8")
    return subprocess.run(
        [sys.executable, PATCH, "apply", "-"],
        input=raw,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def main():
    failures = []
    for index, (name, request, expected, expected_exit) in enumerate(CASES):
        use_file = index % 5 == 0
        proc = run_case(request, use_file)
        stdout = proc.stdout.decode("utf-8", "replace")
        try:
            actual = json.loads(stdout)
        except Exception as exc:
            failures.append(
                "%s: invalid stdout JSON (%s): %r" % (name, exc, stdout)
            )
            continue

        # The message is never compared, but its presence/type is part of the
        # response contract.
        if "message" in expected:
            expected = dict(expected)
            expected["message"] = actual.get("message")
            if not isinstance(actual.get("message"), str):
                failures.append("%s: message is not a string" % name)
                continue

        if actual != expected:
            failures.append(
                "%s:\n  expected %s\n  actual   %s\n  exit %s (wanted %s)"
                % (
                    name,
                    json.dumps(expected, sort_keys=True),
                    json.dumps(actual, sort_keys=True),
                    proc.returncode,
                    expected_exit,
                )
            )
            continue
        if proc.returncode != expected_exit:
            failures.append(
                "%s: exit %s, wanted %s" % (name, proc.returncode, expected_exit)
            )
            continue
        if stdout.count("\n") != 1 or not stdout.endswith("\n"):
            failures.append("%s: stdout is not exactly one JSON line: %r" % (name, stdout))
        if proc.stderr:
            # stderr is allowed, but these cases should be silent.
            failures.append("%s: unexpected stderr: %r" % (name, proc.stderr))

    # CLI misuse cases.
    misuse = [
        ("no arguments", [sys.executable, PATCH]),
        ("unknown subcommand", [sys.executable, PATCH, "run", "-"]),
        ("too many arguments", [sys.executable, PATCH, "apply", "-", "x"]),
        ("missing file", [sys.executable, PATCH, "apply", HERE + "/no-such-file.json"]),
    ]
    for name, argv in misuse:
        proc = subprocess.run(argv, input=b"", stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            actual = json.loads(proc.stdout.decode("utf-8"))
        except Exception as exc:
            failures.append("%s: invalid stdout JSON (%s)" % (name, exc))
            continue
        if (
            actual.get("status") != "error"
            or actual.get("code") != "INVALID_REQUEST"
            or actual.get("index") is not None
            or proc.returncode != 2
        ):
            failures.append("%s: wrong misuse response %r / exit %s"
                            % (name, actual, proc.returncode))

    if failures:
        sys.stderr.write("FAILED %d case(s):\n\n" % len(failures))
        for failure in failures:
            sys.stderr.write(failure + "\n\n")
        return 1

    total = len(CASES) + len(misuse)
    print("OK: %d self-test cases passed" % total)
    return 0


if __name__ == "__main__":
    sys.exit(main())
