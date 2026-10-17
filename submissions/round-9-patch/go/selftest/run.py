#!/usr/bin/env python3
"""Black-box self-test for the Go JSON Patch CLI.

Runs ./patch exactly as the external harness does (stdout is parsed as JSON).
Error `message` values are intentionally not compared, per SPEC.md.
"""

import json
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "patch")


def case(name, document, patch, *, expected_document=None, code=None,
         index=None, exit_code=None, via_file=False, request=None):
    return {
        "name": name,
        "document": document,
        "patch": patch,
        "expected_document": expected_document,
        "code": code,
        "index": index,
        "exit_code": exit_code,
        "via_file": via_file,
        "request": request,
    }


CASES = [
    # ---- add to objects and arrays -------------------------------------
    case("add object member", {"a": 1},
         [{"op": "add", "path": "/b", "value": 2}],
         expected_document={"a": 1, "b": 2}, exit_code=0),
    case("add overwrites object member", {"a": 1},
         [{"op": "add", "path": "/a", "value": 2}],
         expected_document={"a": 2}, exit_code=0),
    case("add inserts array element", [1, 2, 3],
         [{"op": "add", "path": "/1", "value": 9}],
         expected_document=[1, 9, 2, 3], exit_code=0),
    case("add array append dash", [1, 2, 3],
         [{"op": "add", "path": "/-", "value": 4}],
         expected_document=[1, 2, 3, 4], exit_code=0),
    case("add array at length", [1, 2, 3],
         [{"op": "add", "path": "/3", "value": 4}],
         expected_document=[1, 2, 3, 4], exit_code=0),
    case("add into empty array", [],
         [{"op": "add", "path": "/0", "value": "x"}],
         expected_document=["x"], exit_code=0),
    case("add replaces root", {"a": 1},
         [{"op": "add", "path": "", "value": [1, 2]}],
         expected_document=[1, 2], via_file=True, exit_code=0),
    case("add null value", {},
         [{"op": "add", "path": "/a", "value": None}],
         expected_document={"a": None}, exit_code=0),
    case("add dash key to object", {},
         [{"op": "add", "path": "/-", "value": 1}],
         expected_document={"-": 1}, exit_code=0),
    case("add empty object key", {},
         [{"op": "add", "path": "/", "value": 1}],
         expected_document={"": 1}, exit_code=0),

    # ---- remove / replace ----------------------------------------------
    case("remove object member", {"a": {"b": 1}},
         [{"op": "remove", "path": "/a/b"}],
         expected_document={"a": {}}, exit_code=0),
    case("remove array element", [1, 2, 3],
         [{"op": "remove", "path": "/1"}],
         expected_document=[1, 3], exit_code=0),
    case("replace object member", {"a": 1},
         [{"op": "replace", "path": "/a", "value": {"x": None}}],
         expected_document={"a": {"x": None}}, exit_code=0),
    case("replace array element", [1, 2, 3],
         [{"op": "replace", "path": "/2", "value": 9}],
         expected_document=[1, 2, 9], exit_code=0),
    case("replace root", {"a": 1},
         [{"op": "replace", "path": "", "value": None}],
         expected_document=None, exit_code=0),

    # ---- move / copy ----------------------------------------------------
    case("move object member", {"a": 1},
         [{"op": "move", "from": "/a", "path": "/b"}],
         expected_document={"b": 1}, exit_code=0),
    case("move within array (post-remove index)", [1, 2, 3],
         [{"op": "move", "from": "/0", "path": "/2"}],
         expected_document=[2, 3, 1], exit_code=0),
    case("move child to ancestor", {"a": {"b": 1}},
         [{"op": "move", "from": "/a/b", "path": "/a"}],
         expected_document={"a": 1}, exit_code=0),
    case("move to root", {"a": 1, "b": 2},
         [{"op": "move", "from": "/a", "path": ""}],
         expected_document=1, exit_code=0),
    case("copy object", {"a": {"b": 1}},
         [{"op": "copy", "from": "/a", "path": "/c"}],
         expected_document={"a": {"b": 1}, "c": {"b": 1}}, exit_code=0),
    case("copy into descendant", {"a": {"b": 1}},
         [{"op": "copy", "from": "/a", "path": "/a/c"}],
         expected_document={"a": {"b": 1, "c": {"b": 1}}}, exit_code=0),
    case("copy root into member", {"a": 1},
         [{"op": "copy", "from": "", "path": "/b"}],
         expected_document={"a": 1, "b": {"a": 1}}, exit_code=0),

    # ---- test ------------------------------------------------------------
    case("test numbers by value", {"a": 1},
         [{"op": "test", "path": "/a", "value": 1.0}],
         expected_document={"a": 1}, exit_code=0),
    case("test object order independent", {"a": 1, "b": [True, None]},
         [{"op": "test", "path": "", "value": {"b": [True, None], "a": 1}}],
         expected_document={"a": 1, "b": [True, None]}, exit_code=0),
    case("test root", None,
         [{"op": "test", "path": "", "value": None}],
         expected_document=None, exit_code=0),
    case("test failure", {"a": 1},
         [{"op": "test", "path": "/a", "value": 2}],
         code="TEST_FAILED", index=0, exit_code=1),
    case("test type mismatch", {"a": 1},
         [{"op": "test", "path": "/a", "value": "1"}],
         code="TEST_FAILED", index=0, exit_code=1),

    # ---- pointer escapes / nested paths ----------------------------------
    case("escape ~1 to slash", {"a/b": 1},
         [{"op": "remove", "path": "/a~1b"}],
         expected_document={}, exit_code=0),
    case("escape ~0 to tilde", {"a~b": 1},
         [{"op": "remove", "path": "/a~0b"}],
         expected_document={}, exit_code=0),
    case("escape ~01 means literal ~1", {"a~1b": 1},
         [{"op": "remove", "path": "/a~01b"}],
         expected_document={}, exit_code=0),
    case("nested path", {"a": {"b": [{"c": 1}]}},
         [{"op": "replace", "path": "/a/b/0/c", "value": 2}],
         expected_document={"a": {"b": [{"c": 2}]}}, exit_code=0),
    case("empty key segments", {"": {"": 1}},
         [{"op": "remove", "path": "//"}],
         expected_document={"": {}}, exit_code=0),

    # ---- INVALID_REQUEST (exit 2) ---------------------------------------
    case("missing document", None, [],
         code="INVALID_REQUEST", index=None, exit_code=2,
         request={"patch": []}),
    case("missing patch", None, [],
         code="INVALID_REQUEST", index=None, exit_code=2,
         request={"document": {}}),
    case("unknown top-level field", None, [],
         code="INVALID_REQUEST", index=None, exit_code=2,
         request={"document": {}, "patch": [], "x": 1}),
    case("patch not array", None, [],
         code="INVALID_REQUEST", index=None, exit_code=2,
         request={"document": {}, "patch": {}}),
    case("op missing value", {"document": {}},
         [{"op": "add", "path": "/a"}],
         code="INVALID_REQUEST", index=0, exit_code=2),
    case("op unexpected field", {"document": {"a": 1}},
         [{"op": "move", "from": "/a", "path": "/b", "value": 1}],
         code="INVALID_REQUEST", index=0, exit_code=2),
    case("op path not string", {"document": {}},
         [{"op": "remove", "path": 1}],
         code="INVALID_REQUEST", index=0, exit_code=2),

    # ---- INVALID_PATH (exit 1) -------------------------------------------
    case("relative path", {"a": 1},
         [{"op": "remove", "path": "a"}],
         code="INVALID_PATH", index=0, exit_code=1),
    case("bad tilde escape", {"a": 1},
         [{"op": "remove", "path": "/a~2"}],
         code="INVALID_PATH", index=0, exit_code=1),
    case("bare trailing tilde", {"a": 1},
         [{"op": "remove", "path": "/a~"}],
         code="INVALID_PATH", index=0, exit_code=1),
    case("remove root", {"a": 1},
         [{"op": "remove", "path": ""}],
         code="INVALID_PATH", index=0, exit_code=1),

    # ---- PATH_NOT_FOUND (exit 1) -----------------------------------------
    case("remove missing member", {"a": 1},
         [{"op": "remove", "path": "/x"}],
         code="PATH_NOT_FOUND", index=0, exit_code=1),
    case("add below scalar", {"a": 1},
         [{"op": "add", "path": "/a/b", "value": 2}],
         code="PATH_NOT_FOUND", index=0, exit_code=1),
    case("replace missing member", {"a": 1},
         [{"op": "replace", "path": "/x", "value": 2}],
         code="PATH_NOT_FOUND", index=0, exit_code=1),
    case("test missing member", {"a": 1},
         [{"op": "test", "path": "/x", "value": 1}],
         code="PATH_NOT_FOUND", index=0, exit_code=1),
    case("copy missing from", {"a": 1},
         [{"op": "copy", "from": "/x", "path": "/b"}],
         code="PATH_NOT_FOUND", index=0, exit_code=1),
    case("move missing from", {"a": 1},
         [{"op": "move", "from": "/x", "path": "/b"}],
         code="PATH_NOT_FOUND", index=0, exit_code=1),

    # ---- INDEX_ERROR (exit 1) --------------------------------------------
    case("add index too large", [1],
         [{"op": "add", "path": "/5", "value": 0}],
         code="INDEX_ERROR", index=0, exit_code=1),
    case("leading zero index", [1, 2],
         [{"op": "remove", "path": "/01"}],
         code="INDEX_ERROR", index=0, exit_code=1),
    case("remove index out of range", [1],
         [{"op": "remove", "path": "/1"}],
         code="INDEX_ERROR", index=0, exit_code=1),
    case("replace dash on array", [1],
         [{"op": "replace", "path": "/-", "value": 2}],
         code="INDEX_ERROR", index=0, exit_code=1),
    case("test index out of range", [1],
         [{"op": "test", "path": "/1", "value": 1}],
         code="INDEX_ERROR", index=0, exit_code=1),
    case("nested through dash", ["x"],
         [{"op": "add", "path": "/-/a", "value": 1}],
         code="INDEX_ERROR", index=0, exit_code=1),

    # ---- UNKNOWN_OP (exit 1) ---------------------------------------------
    case("unknown op", {"a": 1},
         [{"op": "merge", "path": "/a"}],
         code="UNKNOWN_OP", index=0, exit_code=1),

    # ---- INVALID_MOVE (exit 1) -------------------------------------------
    case("move onto itself", {"a": 1},
         [{"op": "move", "from": "/a", "path": "/a"}],
         code="INVALID_MOVE", index=0, exit_code=1),
    case("move into descendant", {"a": 1},
         [{"op": "move", "from": "/a", "path": "/a/b"}],
         code="INVALID_MOVE", index=0, exit_code=1),
    case("move root into itself", {"a": 1},
         [{"op": "move", "from": "", "path": "/a"}],
         code="INVALID_MOVE", index=0, exit_code=1),

    # ---- atomicity / index reporting -------------------------------------
    case("empty patch", {"a": 1}, [], expected_document={"a": 1}, exit_code=0),
    case("second op fails atomically", {"a": 1},
         [{"op": "add", "path": "/b", "value": 2},
          {"op": "remove", "path": "/missing"}],
         code="PATH_NOT_FOUND", index=1, exit_code=1),
    case("document null is a valid document", None,
         [{"op": "add", "path": "/a", "value": 1}],
         code="PATH_NOT_FOUND", index=0, exit_code=1),
]


def invoke(request, via_file):
    payload = json.dumps(request, separators=(",", ":"))
    if via_file:
        fd, path = tempfile.mkstemp(suffix=".json")
        try:
            with os.fdopen(fd, "w") as f:
                f.write(payload)
            proc = subprocess.run([BIN, "apply", path], capture_output=True,
                                  text=True)
        finally:
            os.unlink(path)
        return proc
    return subprocess.run([BIN, "apply", "-"], input=payload,
                          capture_output=True, text=True)


def main():
    if not os.path.isfile(BIN):
        print(f"missing binary: {BIN}; run 'go build -o patch .' first")
        return 90

    failures = 0
    for c in CASES:
        request = c["request"]
        if request is None:
            request = {"document": c["document"], "patch": c["patch"]}
        proc = invoke(request, c["via_file"])
        problems = []

        if proc.returncode != c["exit_code"]:
            problems.append(f"exit={proc.returncode}, want {c['exit_code']}")

        if proc.stderr:
            problems.append(f"unexpected stderr: {proc.stderr.strip()!r}")

        try:
            out = json.loads(proc.stdout)
        except Exception as exc:  # noqa: BLE001 - test harness
            problems.append(f"stdout is not one JSON document: {exc}")
            out = None

        if out is not None:
            if c["code"] is None:
                if set(out) != {"status", "document"}:
                    problems.append(f"ok response keys: {sorted(out)}")
                elif out.get("status") != "ok":
                    problems.append(f"status={out.get('status')!r}")
                elif out.get("document") != c["expected_document"]:
                    problems.append(
                        f"document={out.get('document')!r}, "
                        f"want {c['expected_document']!r}")
            else:
                if set(out) != {"status", "code", "index", "message"}:
                    problems.append(f"error response keys: {sorted(out)}")
                elif out.get("status") != "error":
                    problems.append(f"status={out.get('status')!r}")
                elif out.get("code") != c["code"]:
                    problems.append(f"code={out.get('code')!r}, want {c['code']!r}")
                elif out.get("index") != c["index"]:
                    problems.append(f"index={out.get('index')!r}, want {c['index']!r}")
                elif not isinstance(out.get("message"), str):
                    problems.append("message is not a string")

        if problems:
            failures += 1
            print(f"FAIL {c['name']}")
            for p in problems:
                print(f"     {p}")
        else:
            print(f"ok   {c['name']}")

    print()
    print(f"{len(CASES) - failures}/{len(CASES)} passed")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
