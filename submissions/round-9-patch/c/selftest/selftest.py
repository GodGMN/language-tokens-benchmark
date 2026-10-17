#!/usr/bin/env python3
"""Black-box self-test for the C patch CLI.

Each case is checked structurally: the response must be valid JSON, the status
must match, and the result document (or error code/index) must match exactly.
This mirrors the way JSON results are normally compared by the benchmark.
"""

import json
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "patch")

cases = []


def ok(name, document, patch, expected):
    cases.append((name, {"document": document, "patch": patch},
                  {"status": "ok", "document": expected}))


def err(name, document, patch, code, index):
    cases.append((name, {"document": document, "patch": patch},
                  {"status": "error", "code": code, "index": index}))


def raw(name, payload, expected, exit_code):
    cases.append((name, payload, expected, exit_code))


# ---------------------------------------------------------------------------
# add
# ---------------------------------------------------------------------------
ok("add object member", {"a": 1},
   [{"op": "add", "path": "/b", "value": 2}], {"a": 1, "b": 2})
ok("add overwrites object member", {"a": 1},
   [{"op": "add", "path": "/a", "value": 2}], {"a": 2})
ok("add array start", [1, 2, 3],
   [{"op": "add", "path": "/0", "value": 9}], [9, 1, 2, 3])
ok("add array middle", [1, 2, 3],
   [{"op": "add", "path": "/1", "value": 9}], [1, 9, 2, 3])
ok("add array at length", [1, 2, 3],
   [{"op": "add", "path": "/3", "value": 4}], [1, 2, 3, 4])
ok("add array dash", [1, 2, 3],
   [{"op": "add", "path": "/-", "value": 4}], [1, 2, 3, 4])
ok("add nested array dash", {"a": [1]},
   [{"op": "add", "path": "/a/-", "value": 2}], {"a": [1, 2]})
ok("add dash is an object key", {"a": 1},
   [{"op": "add", "path": "/-", "value": 2}], {"a": 1, "-": 2})
ok("add root", {"a": 1},
   [{"op": "add", "path": "", "value": [1, 2]}], [1, 2])
ok("add empty key", {"a": 1},
   [{"op": "add", "path": "/", "value": 2}], {"a": 1, "": 2})
ok("add nested object", {"a": {"b": 1}},
   [{"op": "add", "path": "/a/c", "value": 2}], {"a": {"b": 1, "c": 2}})
err("add array past end", [1],
    [{"op": "add", "path": "/5", "value": 0}], "INDEX_ERROR", 0)
err("add array leading zero", [1],
    [{"op": "add", "path": "/01", "value": 0}], "INDEX_ERROR", 0)
err("add missing parent", {"a": 1},
    [{"op": "add", "path": "/x/y", "value": 0}], "PATH_NOT_FOUND", 0)
err("add scalar parent", {"a": 1},
    [{"op": "add", "path": "/a/y", "value": 0}], "PATH_NOT_FOUND", 0)

# ---------------------------------------------------------------------------
# remove
# ---------------------------------------------------------------------------
ok("remove object member", {"a": 1, "b": 2},
   [{"op": "remove", "path": "/a"}], {"b": 2})
ok("remove nested member", {"a": {"b": 1, "c": 2}},
   [{"op": "remove", "path": "/a/b"}], {"a": {"c": 2}})
ok("remove array element", [1, 2, 3],
   [{"op": "remove", "path": "/1"}], [1, 3])
ok("remove nested array element", [[1, 2], [3]],
   [{"op": "remove", "path": "/1/0"}], [[1, 2], []])
ok("remove escaped slash key", {"a/b": 1, "a~b": 2},
   [{"op": "remove", "path": "/a~1b"}], {"a~b": 2})
ok("remove escaped tilde key", {"a/b": 1, "a~b": 2},
   [{"op": "remove", "path": "/a~0b"}], {"a/b": 1})
ok("remove key with both escapes", {"a~/b": 1, "x": 2},
   [{"op": "remove", "path": "/a~0~1b"}], {"x": 2})
ok("remove empty key", {"": 1, "a": 2},
   [{"op": "remove", "path": "/"}], {"a": 2})
err("remove root", {"a": 1},
    [{"op": "remove", "path": ""}], "INVALID_PATH", 0)
err("remove missing member", {"a": 1},
    [{"op": "remove", "path": "/x"}], "PATH_NOT_FOUND", 0)
err("remove missing nested member", {"a": {}},
    [{"op": "remove", "path": "/a/x"}], "PATH_NOT_FOUND", 0)
err("remove scalar parent", {"a": 1},
    [{"op": "remove", "path": "/a/x"}], "PATH_NOT_FOUND", 0)
err("remove array out of range", [1],
    [{"op": "remove", "path": "/3"}], "INDEX_ERROR", 0)
err("remove array dash", [1],
    [{"op": "remove", "path": "/-"}], "INDEX_ERROR", 0)
err("remove array malformed", [1],
    [{"op": "remove", "path": "/1x"}], "INDEX_ERROR", 0)

# ---------------------------------------------------------------------------
# replace
# ---------------------------------------------------------------------------
ok("replace object member", {"a": 1, "b": 2},
   [{"op": "replace", "path": "/a", "value": 9}], {"a": 9, "b": 2})
ok("replace nested member", {"a": {"b": 1}},
   [{"op": "replace", "path": "/a/b", "value": None}],
   {"a": {"b": None}})
ok("replace array element", [1, 2, 3],
   [{"op": "replace", "path": "/1", "value": 9}], [1, 9, 3])
ok("replace root", {"a": 1},
   [{"op": "replace", "path": "", "value": "x"}], "x")
err("replace missing member", {"a": 1},
    [{"op": "replace", "path": "/x", "value": 2}], "PATH_NOT_FOUND", 0)
err("replace array out of range", [1],
    [{"op": "replace", "path": "/1", "value": 2}], "INDEX_ERROR", 0)
err("replace array dash", [1],
    [{"op": "replace", "path": "/-", "value": 2}], "INDEX_ERROR", 0)

# ---------------------------------------------------------------------------
# move
# ---------------------------------------------------------------------------
ok("move object member", {"a": 1},
   [{"op": "move", "from": "/a", "path": "/b"}], {"b": 1})
ok("move overwrites destination", {"a": 1, "b": 2},
   [{"op": "move", "from": "/a", "path": "/b"}], {"b": 1})
ok("move array element forward", [1, 2, 3],
   [{"op": "move", "from": "/0", "path": "/2"}], [2, 3, 1])
ok("move array element backward", [1, 2, 3],
   [{"op": "move", "from": "/2", "path": "/0"}], [3, 1, 2])
ok("move array element to end", [1, 2, 3],
   [{"op": "move", "from": "/0", "path": "/-"}], [2, 3, 1])
ok("move to root", {"a": 1, "b": 2},
   [{"op": "move", "from": "/a", "path": ""}], 1)
err("move to itself", {"a": 1},
    [{"op": "move", "from": "/a", "path": "/a"}], "INVALID_MOVE", 0)
err("move into descendant", {"a": 1},
    [{"op": "move", "from": "/a", "path": "/a/b"}], "INVALID_MOVE", 0)
err("move escaped descendant", {"a/b": 1},
    [{"op": "move", "from": "/a~1b", "path": "/a~1b/c"}],
    "INVALID_MOVE", 0)
err("move root into child", {"a": 1},
    [{"op": "move", "from": "", "path": "/b"}], "INVALID_MOVE", 0)
err("move missing source", {"a": 1},
    [{"op": "move", "from": "/x", "path": "/b"}], "PATH_NOT_FOUND", 0)
err("move bad destination parent", {"a": 1},
    [{"op": "move", "from": "/a", "path": "/b/c"}], "PATH_NOT_FOUND", 0)

# ---------------------------------------------------------------------------
# copy
# ---------------------------------------------------------------------------
ok("copy object member", {"a": 1},
   [{"op": "copy", "from": "/a", "path": "/b"}], {"a": 1, "b": 1})
ok("copy deep value", {"a": {"b": [1]}},
   [{"op": "copy", "from": "/a", "path": "/c"}],
   {"a": {"b": [1]}, "c": {"b": [1]}})
ok("copy into descendant", {"a": {"b": 1}},
   [{"op": "copy", "from": "/a", "path": "/a/c"}],
   {"a": {"b": 1, "c": {"b": 1}}})
ok("copy root into child", {"a": 1},
   [{"op": "copy", "from": "", "path": "/b"}],
   {"a": 1, "b": {"a": 1}})
ok("copy to root", {"a": 1},
   [{"op": "copy", "from": "/a", "path": ""}], 1)
ok("copy same location", {"a": 1},
   [{"op": "copy", "from": "/a", "path": "/a"}], {"a": 1})
err("copy missing source", {"a": 1},
    [{"op": "copy", "from": "/x", "path": "/b"}], "PATH_NOT_FOUND", 0)
err("copy source index out of range", [1],
    [{"op": "copy", "from": "/2", "path": "/-"}], "INDEX_ERROR", 0)

# ---------------------------------------------------------------------------
# test
# ---------------------------------------------------------------------------
ok("test root", {"a": 1},
   [{"op": "test", "path": "", "value": {"a": 1}}], {"a": 1})
ok("test root object order", {"a": 1, "b": 2},
   [{"op": "test", "path": "", "value": {"b": 2, "a": 1}}],
   {"a": 1, "b": 2})
ok("test nested object", {"a": {"b": [1, 2]}},
   [{"op": "test", "path": "/a", "value": {"b": [1, 2]}}],
   {"a": {"b": [1, 2]}})
ok("test array", [1, {"x": None}],
   [{"op": "test", "path": "", "value": [1, {"x": None}]}],
   [1, {"x": None}])
ok("test null", {"a": None},
   [{"op": "test", "path": "/a", "value": None}], {"a": None})
ok("test integer/float numeric equality", {"a": 1},
   [{"op": "test", "path": "/a", "value": 1.0}], {"a": 1})
ok("test exponent numeric equality", {"a": 100},
   [{"op": "test", "path": "/a", "value": 1e2}], {"a": 100})
ok("test negative zero equals zero", {"a": -0.0},
   [{"op": "test", "path": "/a", "value": 0}], {"a": -0.0})
ok("test escaped key", {"a/b": 1},
   [{"op": "test", "path": "/a~1b", "value": 1}], {"a/b": 1})
ok("test unicode", {"é": "😀"},
   [{"op": "test", "path": "/é", "value": "😀"}], {"é": "😀"})
err("test mismatch", {"a": 1},
    [{"op": "test", "path": "/a", "value": 2}], "TEST_FAILED", 0)
err("test type mismatch", {"a": 1},
    [{"op": "test", "path": "/a", "value": "1"}], "TEST_FAILED", 0)
err("test missing member", {"a": 1},
    [{"op": "test", "path": "/x", "value": 1}], "PATH_NOT_FOUND", 0)
err("test array out of range", [1],
    [{"op": "test", "path": "/2", "value": 1}], "INDEX_ERROR", 0)

# ---------------------------------------------------------------------------
# paths / nested paths / indexes
# ---------------------------------------------------------------------------
ok("nested add/remove path", {"a": [{"b": [0]}]},
   [{"op": "add", "path": "/a/0/b/-", "value": 1},
    {"op": "replace", "path": "/a/0/b/0", "value": 9},
    {"op": "remove", "path": "/a/0/b/1"}],
   {"a": [{"b": [9]}]})
ok("empty key segment", {"": {"": 1}},
   [{"op": "replace", "path": "//", "value": 2}], {"": {"": 2}})
err("path without leading slash", {"a": 1},
    [{"op": "remove", "path": "a"}], "INVALID_PATH", 0)
err("dangling tilde", {"a": 1},
    [{"op": "remove", "path": "/a~"}], "INVALID_PATH", 0)
err("bad tilde escape", {"a": 1},
    [{"op": "remove", "path": "/a~2b"}], "INVALID_PATH", 0)
err("malformed array index on traversal", {"a": [1]},
    [{"op": "add", "path": "/a/x/y", "value": 1}], "INDEX_ERROR", 0)
err("intermediate array index out of range", {"a": [1]},
    [{"op": "add", "path": "/a/5/y", "value": 1}], "INDEX_ERROR", 0)

# ---------------------------------------------------------------------------
# operation indexes and atomicity
# ---------------------------------------------------------------------------
err("error index is first failing op", {"a": 1},
    [{"op": "add", "path": "/b", "value": 2},
     {"op": "remove", "path": "/missing"},
     {"op": "add", "path": "/c", "value": 3}],
    "PATH_NOT_FOUND", 1)
ok("empty patch", {"a": 1}, [], {"a": 1})
err("patch is atomic", {"a": 1},
    [{"op": "add", "path": "/b", "value": 2},
     {"op": "test", "path": "/b", "value": 99}],
    "TEST_FAILED", 1)

# ---------------------------------------------------------------------------
# envelope and operation shape errors
# ---------------------------------------------------------------------------
raw("invalid JSON", b'{"document": {"a": 1}, "patch": [}',
    {"status": "error", "code": "INVALID_REQUEST", "index": None}, 2)
raw("request not object", b'[]',
    {"status": "error", "code": "INVALID_REQUEST", "index": None}, 2)
raw("unknown top-level field",
    b'{"document": {}, "patch": [], "extra": 1}',
    {"status": "error", "code": "INVALID_REQUEST", "index": None}, 2)
raw("missing document", b'{"patch": []}',
    {"status": "error", "code": "INVALID_REQUEST", "index": None}, 2)
raw("missing patch", b'{"document": {}}',
    {"status": "error", "code": "INVALID_REQUEST", "index": None}, 2)
raw("patch not array", b'{"document": {}, "patch": {}}',
    {"status": "error", "code": "INVALID_REQUEST", "index": None}, 2)
raw("operation not object", b'{"document": {}, "patch": [1]}',
    {"status": "error", "code": "INVALID_REQUEST", "index": 0}, 2)
raw("missing op", b'{"document": {}, "patch": [{"path": "/a"}]}',
    {"status": "error", "code": "INVALID_REQUEST", "index": 0}, 2)
raw("op not string",
    b'{"document": {}, "patch": [{"op": 1, "path": "/a"}]}',
    {"status": "error", "code": "INVALID_REQUEST", "index": 0}, 2)
raw("missing path",
    b'{"document": {}, "patch": [{"op": "remove"}]}',
    {"status": "error", "code": "INVALID_REQUEST", "index": 0}, 2)
raw("path not string",
    b'{"document": {}, "patch": [{"op": "remove", "path": 1}]}',
    {"status": "error", "code": "INVALID_REQUEST", "index": 0}, 2)
raw("add missing value",
    b'{"document": {}, "patch": [{"op": "add", "path": "/a"}]}',
    {"status": "error", "code": "INVALID_REQUEST", "index": 0}, 2)
raw("add extra from",
    b'{"document": {}, "patch": [{"op": "add", "path": "/a", "value": 1, "from": "/b"}]}',
    {"status": "error", "code": "INVALID_REQUEST", "index": 0}, 2)
raw("remove extra value",
    b'{"document": {}, "patch": [{"op": "remove", "path": "/a", "value": 1}]}',
    {"status": "error", "code": "INVALID_REQUEST", "index": 0}, 2)
raw("move missing from",
    b'{"document": {}, "patch": [{"op": "move", "path": "/a"}]}',
    {"status": "error", "code": "INVALID_REQUEST", "index": 0}, 2)
raw("move from not string",
    b'{"document": {}, "patch": [{"op": "move", "from": 1, "path": "/a"}]}',
    {"status": "error", "code": "INVALID_REQUEST", "index": 0}, 2)
raw("test extra field",
    b'{"document": {}, "patch": [{"op": "test", "path": "", "value": 1, "x": 2}]}',
    {"status": "error", "code": "INVALID_REQUEST", "index": 0}, 2)
raw("unknown op",
    b'{"document": {}, "patch": [{"op": "bogus", "path": "/a"}]}',
    {"status": "error", "code": "UNKNOWN_OP", "index": 0}, 1)


def run_stdin(payload):
    return subprocess.run([BIN, "apply", "-"], input=payload,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def main():
    failures = 0
    for case in cases:
        if len(case) == 4:
            name, payload, expected, expected_exit = case
        else:
            name, request, expected = case
            payload = json.dumps(request, ensure_ascii=False).encode("utf-8")
            expected_exit = 0 if expected["status"] == "ok" else (
                2 if expected["code"] == "INVALID_REQUEST" else 1)

        proc = run_stdin(payload)
        problems = []
        try:
            actual = json.loads(proc.stdout.decode("utf-8"))
        except Exception as exc:
            problems.append("stdout is not JSON: %s (%r)" % (exc, proc.stdout))
            actual = None

        if proc.returncode != expected_exit:
            problems.append("exit %d != %d" % (proc.returncode, expected_exit))

        if actual is not None:
            if actual.get("status") != expected["status"]:
                problems.append("status %r != %r" %
                                (actual.get("status"), expected["status"]))
            elif expected["status"] == "ok":
                if actual.get("document") != expected["document"]:
                    problems.append("document %r != %r" %
                                    (actual.get("document"),
                                     expected["document"]))
            else:
                if actual.get("code") != expected["code"]:
                    problems.append("code %r != %r" %
                                    (actual.get("code"), expected["code"]))
                if actual.get("index") != expected["index"]:
                    problems.append("index %r != %r" %
                                    (actual.get("index"), expected["index"]))
                if "document" in actual:
                    problems.append("error response contains a document")

        if problems:
            failures += 1
            print("FAIL:", name)
            for problem in problems:
                print("   ", problem)
            print("    stdout:", proc.stdout.decode("utf-8", "replace").strip())
            print("    stderr:", proc.stderr.decode("utf-8", "replace").strip())

    # Exercise file input in addition to stdin.
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "request.json")
        with open(path, "w", encoding="utf-8") as f:
            json.dump({"document": {"a": 1},
                       "patch": [{"op": "add", "path": "/b", "value": 2}]}, f)
        proc = subprocess.run([BIN, "apply", path], stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE)
        try:
            actual = json.loads(proc.stdout.decode("utf-8"))
        except Exception:
            actual = None
        if proc.returncode != 0 or actual != {
                "status": "ok", "document": {"a": 1, "b": 2}}:
            failures += 1
            print("FAIL: file input")
            print("    stdout:", proc.stdout.decode("utf-8", "replace").strip())

    total = len(cases) + 1
    if failures:
        print("%d/%d self-test cases failed" % (failures, total))
        return 1
    print("all %d self-test cases passed" % total)
    return 0


if __name__ == "__main__":
    sys.exit(main())
