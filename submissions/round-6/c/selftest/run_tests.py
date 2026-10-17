#!/usr/bin/env python3
"""Self-tests for the C workflow engine.

Runs ./workflow with many small documents and checks the parsed JSON response.
This script is a development aid; it is also the self-test referenced by README.
"""
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BIN = os.path.join(ROOT, "workflow")

failures = []
count = 0


def run(doc, stdin=False):
    if isinstance(doc, str):
        payload = doc.encode()
    else:
        payload = json.dumps(doc).encode()
    if stdin:
        p = subprocess.run([BIN, "run", "-"], input=payload,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    else:
        import tempfile
        with tempfile.NamedTemporaryFile("wb", suffix=".json", delete=False) as f:
            f.write(payload)
            name = f.name
        try:
            p = subprocess.run([BIN, "run", name], stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE)
        finally:
            os.unlink(name)
    try:
        out = json.loads(p.stdout.decode())
    except Exception as e:
        raise AssertionError("bad JSON output: %r (%s)" % (p.stdout, e))
    return p.returncode, out


def wf(steps, version=1):
    return {"version": version, "steps": steps}


def step(id, op, args=None, deps=None, when=None, retry=None):
    s = {"id": id, "op": op}
    if args is not None:
        s["args"] = args
    if deps is not None:
        s["depends_on"] = deps
    if when is not None:
        s["when"] = when
    if retry is not None:
        s["retry"] = retry
    return s


def check(name, doc, expected_status, expected_results=None, expected_order=None,
          expected_skipped=None, expected_code=None, expected_step="__any__",
          expected_exit=None, stdin=False):
    global count
    count += 1
    rc, out = run(doc, stdin)
    problems = []
    if out.get("status") != expected_status:
        problems.append("status=%r want %r" % (out.get("status"), expected_status))
    if expected_results is not None and out.get("results") != expected_results:
        problems.append("results=%r want %r" % (out.get("results"), expected_results))
    if expected_order is not None and out.get("execution_order") != expected_order:
        problems.append("order=%r want %r" % (out.get("execution_order"), expected_order))
    if expected_skipped is not None and out.get("skipped") != expected_skipped:
        problems.append("skipped=%r want %r" % (out.get("skipped"), expected_skipped))
    err = out.get("error")
    if expected_code is None:
        if err is not None:
            problems.append("error=%r want null" % (err,))
    else:
        if not err:
            problems.append("error missing, want code %s" % expected_code)
        else:
            if err.get("code") != expected_code:
                problems.append("code=%r want %r" % (err.get("code"), expected_code))
            if expected_step != "__any__" and err.get("step") != expected_step:
                problems.append("step=%r want %r" % (err.get("step"), expected_step))
    if not isinstance(out.get("skipped"), list):
        problems.append("skipped not a list")
    if expected_exit is not None and rc != expected_exit:
        problems.append("exit=%r want %r" % (rc, expected_exit))
    if problems:
        failures.append((name, problems, out))


# ---------------------------------------------------------------- basics
check("empty workflow", wf([]), "completed", {}, [], [])
check("constants", wf([
    step("a", "constant", {"value": 1}),
    step("b", "constant", {"value": "x"}),
    step("c", "constant", {"value": None}),
    step("d", "constant", {"value": [1, {"z": 2}]}),
]), "completed", {"a": 1, "b": "x", "c": None, "d": [1, {"z": 2}]},
    ["a", "b", "c", "d"], [])

# ---------------------------------------------------------------- math
def math_case(op, a, b, want, name=None):
    check(name or ("%s %s %s" % (op, a, b)), wf([
        step("s", op, {"a": a, "b": b})]), "completed", {"s": want}, ["s"], [])

math_case("add", 2, 3, 5)
math_case("subtract", 2, 5, -3)
math_case("multiply", -4, 3, -12)
math_case("divide", 7, 2, 3)
math_case("divide", -7, 2, -3)
math_case("divide", -7, -2, 3)
math_case("mod", 7, 2, 1)
math_case("mod", -7, 2, -1)
math_case("mod", 7, -2, 1)
math_case("mod", -7, -2, -1)
check("add overflow", wf([step("s", "add", {"a": 9223372036854775807, "b": 1})]),
      "failed", {}, [], [], "ARITHMETIC_OVERFLOW", "s", 1)
check("sub overflow", wf([step("s", "subtract", {"a": -9223372036854775808, "b": 1})]),
      "failed", {}, [], [], "ARITHMETIC_OVERFLOW", "s", 1)
check("mul overflow", wf([step("s", "multiply", {"a": 3037000500, "b": 3037000500})]),
      "failed", {}, [], [], "ARITHMETIC_OVERFLOW", "s", 1)
check("min div -1", wf([step("s", "divide", {"a": -9223372036854775808, "b": -1})]),
      "failed", {}, [], [], "ARITHMETIC_OVERFLOW", "s", 1)
check("min mod -1", wf([step("s", "mod", {"a": -9223372036854775808, "b": -1})]),
      "failed", {}, [], [], "ARITHMETIC_OVERFLOW", "s", 1)
check("div zero", wf([step("s", "divide", {"a": 1, "b": 0})]),
      "failed", {}, [], [], "DIVISION_BY_ZERO", "s", 1)
check("mod zero", wf([step("s", "mod", {"a": 1, "b": 0})]),
      "failed", {}, [], [], "DIVISION_BY_ZERO", "s", 1)
check("float arg rejected", wf([step("s", "add", {"a": 1.0, "b": 2})]),
      "failed", {}, [], [], "INVALID_ARG", "s", 1)
check("exponent arg rejected", wf([step("s", "add", {"a": 1e3, "b": 2})]),
      "failed", {}, [], [], "INVALID_ARG", "s", 1)
check("string arg rejected", wf([step("s", "add", {"a": "1", "b": 2})]),
      "failed", {}, [], [], "INVALID_ARG", "s", 1)
check("missing arg", wf([step("s", "add", {"a": 1})]),
      "failed", {}, [], [], "INVALID_ARG", "s", 1)
check("unknown arg", wf([step("s", "add", {"a": 1, "b": 2, "c": 3})]),
      "failed", {}, [], [], "INVALID_ARG", "s", 1)

check("sum", wf([step("s", "sum", {"input": [1, 2, -3, 10]})]),
      "completed", {"s": 10}, ["s"], [])
check("sum empty", wf([step("s", "sum", {"input": []})]),
      "completed", {"s": 0}, ["s"], [])
check("sum overflow", wf([step("s", "sum", {"input": [9223372036854775807, 1]})]),
      "failed", {}, [], [], "ARITHMETIC_OVERFLOW", "s", 1)
check("sum bad elem", wf([step("s", "sum", {"input": [1, "x"]})]),
      "failed", {}, [], [], "INVALID_ARG", "s", 1)
check("min", wf([step("s", "min", {"input": [3, -2, 9]})]),
      "completed", {"s": -2}, ["s"], [])
check("max", wf([step("s", "max", {"input": [3, -2, 9]})]),
      "completed", {"s": 9}, ["s"], [])
check("min empty", wf([step("s", "min", {"input": []})]),
      "failed", {}, [], [], "EMPTY_ARRAY", "s", 1)
check("max empty", wf([step("s", "max", {"input": []})]),
      "failed", {}, [], [], "EMPTY_ARRAY", "s", 1)
check("range", wf([step("s", "range", {"start": 1, "end": 5})]),
      "completed", {"s": [1, 2, 3, 4, 5]}, ["s"], [])
check("range empty", wf([step("s", "range", {"start": 5, "end": 1})]),
      "completed", {"s": []}, ["s"], [])
check("range single", wf([step("s", "range", {"start": -2, "end": -2})]),
      "completed", {"s": [-2]}, ["s"], [])

# ---------------------------------------------------------------- strings
def str_case(op, args, want, name=None):
    check(name or op, wf([step("s", op, args)]), "completed", {"s": want}, ["s"], [])

str_case("concat", {"a": "foo", "b": "bar"}, "foobar")
str_case("uppercase", {"input": "aBc-Äé"}, "ABC-Äé")
str_case("lowercase", {"input": "AbC-Äé"}, "abc-Äé")
str_case("trim", {"input": " \t\nhi there\r\f\v "}, "hi there")
str_case("length", {"input": "héllo"}, 5)
str_case("length", {"input": ""}, 0)
str_case("length", {"input": "😀x"}, 2)
str_case("split", {"input": "a,b,,c", "sep": ","}, ["a", "b", "", "c"])
str_case("split", {"input": "abc", "sep": ""}, ["a", "b", "c"])
str_case("split", {"input": "hé", "sep": ""}, ["h", "é"])
str_case("split", {"input": "abc", "sep": "x"}, ["abc"])
str_case("join", {"input": ["a", "b", "c"], "sep": "-"}, "a-b-c")
str_case("join", {"input": [], "sep": "-"}, "")
str_case("replace", {"input": "aaa", "search": "aa", "replace": "b"}, "ba")
str_case("replace", {"input": "abc", "search": "b", "replace": ""}, "ac")
check("replace empty search", wf([step("s", "replace",
      {"input": "abc", "search": "", "replace": "x"})]),
      "failed", {}, [], [], "INVALID_ARG", "s", 1)
check("join non-string", wf([step("s", "join", {"input": ["a", 1], "sep": "-"})]),
      "failed", {}, [], [], "INVALID_ARG", "s", 1)

# ---------------------------------------------------------------- arrays
check("sort mixed", wf([step("s", "sort", {"input": [
    {"a": 1}, [1, 2], None, True, False, "b", "a", 2, 1.5, -1]})]),
    "completed", {"s": [-1, 1.5, 2, "a", "b", False, True, None, [1, 2], {"a": 1}]},
    ["s"], [])
check("sort stable", wf([step("s", "sort", {"input": [3, 1, 2, 1]})]),
      "completed", {"s": [1, 1, 2, 3]}, ["s"], [])
check("sort objects", wf([step("s", "sort", {"input": [
    {"b": 1, "a": 2}, {"a": 1}, {"a": 1, "b": 0}]})]),
    "completed", {"s": [{"a": 1}, {"a": 1, "b": 0}, {"b": 1, "a": 2}]}, ["s"], [])
check("reverse", wf([step("s", "reverse", {"input": [1, 2, 3]})]),
      "completed", {"s": [3, 2, 1]}, ["s"], [])
check("unique", wf([step("s", "unique", {"input": [1, 1.0, "1", 1, [1], [1], None, None]})]),
      "completed", {"s": [1, "1", [1], None]}, ["s"], [])
check("slice basic", wf([step("s", "slice", {"input": [0, 1, 2, 3, 4], "start": 1, "end": 3})]),
      "completed", {"s": [1, 2]}, ["s"], [])
check("slice negative", wf([step("s", "slice", {"input": [0, 1, 2, 3, 4], "start": -2})]),
      "completed", {"s": [3, 4]}, ["s"], [])
check("slice clamp", wf([step("s", "slice", {"input": [0, 1, 2], "start": -100, "end": 100})]),
      "completed", {"s": [0, 1, 2]}, ["s"], [])
check("slice start>end", wf([step("s", "slice", {"input": [0, 1, 2], "start": 2, "end": 1})]),
      "completed", {"s": []}, ["s"], [])
check("flatten", wf([step("s", "flatten", {"input": [1, [2, [3]], 4]})]),
      "completed", {"s": [1, 2, [3], 4]}, ["s"], [])

def map_case(tr, elems, want):
    check("map %r" % (tr,), wf([step("s", "map", {"input": elems, "transform": tr})]),
          "completed", {"s": want}, ["s"], [])

map_case({"type": "add", "value": 2}, [1, 2, 3], [3, 4, 5])
map_case({"type": "subtract", "value": 2}, [1, 2, 3], [-1, 0, 1])
map_case({"type": "multiply", "value": 3}, [1, 2], [3, 6])
map_case({"type": "divide", "value": 2}, [7, -7], [3, -3])
map_case({"type": "mod", "value": 2}, [7, -7], [1, -1])
map_case({"type": "negate"}, [1, -2, 0], [-1, 2, 0])
map_case({"type": "abs"}, [1, -2, 0], [1, 2, 0])
map_case({"type": "constant", "value": 9}, [1, 2], [9, 9])
map_case({"type": "stringify"}, ["a", 1, None, [1, 2]], ["\"a\"", "1", "null", "[1,2]"])
map_case({"type": "type"}, [1, "a", None, [], {}, True],
         ["number", "string", "null", "array", "object", "boolean"])
map_case({"type": "op", "op": "add", "args": {"a": {"$self": True}, "b": 2}},
         [1, 2], [3, 4])
map_case({"type": "op", "op": "multiply", "args": {"a": 10, "b": {"$self": True}}},
         [1, 2], [10, 20])

def filter_case(pr, elems, want):
    check("filter %r" % (pr,), wf([step("s", "filter", {"input": elems, "predicate": pr})]),
          "completed", {"s": want}, ["s"], [])

filter_case({"type": "even"}, [1, 2, 3, 4, -4], [2, 4, -4])
filter_case({"type": "odd"}, [1, 2, 3, 4, -3], [1, 3, -3])
filter_case({"type": "gt", "value": 2}, [1, 2, 3, 4], [3, 4])
filter_case({"type": "gte", "value": 2}, [1, 2, 3], [2, 3])
filter_case({"type": "lt", "value": 2}, [1, 2, 3], [1])
filter_case({"type": "lte", "value": 2}, [1, 2, 3], [1, 2])
filter_case({"type": "eq", "value": 2}, [1, 2, 2.0], [2, 2.0])
filter_case({"type": "ne", "value": 2}, [1, 2], [1])
filter_case({"type": "truthy"}, [0, 1, "", "x", None, [], [1], {}, {"a": 1}, False, True],
            [1, "x", [1], {"a": 1}, True])
filter_case({"type": "falsy"}, [0, 1, "", "x", None, [], [1], {}, {"a": 1}, False, True],
            [0, "", None, [], {}, False])
filter_case({"type": "type_is", "value": "string"}, [1, "a", None, [], {}], ["a"])
filter_case({"type": "string_contains", "value": "ll"}, ["hello", "world", "ill"], ["hello", "ill"])

check("reduce add", wf([step("s", "reduce", {"input": [1, 2, 3], "op": "add"})]),
      "completed", {"s": 6}, ["s"], [])
check("reduce add initial", wf([step("s", "reduce", {"input": [1, 2, 3], "op": "add", "initial": 10})]),
      "completed", {"s": 16}, ["s"], [])
check("reduce subtract", wf([step("s", "reduce", {"input": [1, 2, 3], "op": "subtract"})]),
      "completed", {"s": -6}, ["s"], [])
check("reduce multiply", wf([step("s", "reduce", {"input": [2, 3, 4], "op": "multiply"})]),
      "completed", {"s": 24}, ["s"], [])
check("reduce empty", wf([step("s", "reduce", {"input": [], "op": "multiply"})]),
      "completed", {"s": 1}, ["s"], [])
check("reduce empty initial", wf([step("s", "reduce", {"input": [], "op": "add", "initial": 7})]),
      "completed", {"s": 7}, ["s"], [])
check("reduce bad op", wf([step("s", "reduce", {"input": [1], "op": "divide"})]),
      "failed", {}, [], [], "INVALID_ARG", "s", 1)

# ---------------------------------------------------------------- objects
check("get", wf([step("s", "get", {"input": {"a": 1, "b": None}, "key": "b"})]),
      "completed", {"s": None}, ["s"], [])
check("get missing", wf([step("s", "get", {"input": {"a": 1}, "key": "z"})]),
      "completed", {"s": None}, ["s"], [])
check("set", wf([step("s", "set", {"input": {"a": 1}, "key": "b", "value": [2]})]),
      "completed", {"s": {"a": 1, "b": [2]}}, ["s"], [])
check("keys sorted", wf([step("s", "keys", {"input": {"b": 1, "a": 2, "C": 3}})]),
      "completed", {"s": ["C", "a", "b"]}, ["s"], [])
check("values sorted", wf([step("s", "values", {"input": {"b": 1, "a": 2}})]),
      "completed", {"s": [2, 1]}, ["s"], [])
check("merge", wf([step("s", "merge", {"a": {"x": 1, "y": 2}, "b": {"y": 20, "z": 3}})]),
      "completed", {"s": {"x": 1, "y": 20, "z": 3}}, ["s"], [])
check("pick", wf([step("s", "pick", {"input": {"a": 1, "b": 2, "c": 3}, "keys": ["c", "a", "z"]})]),
      "completed", {"s": {"a": 1, "c": 3, "z": None}}, ["s"], [])
check("omit", wf([step("s", "omit", {"input": {"a": 1, "b": 2, "c": 3}, "keys": ["b"]})]),
      "completed", {"s": {"a": 1, "c": 3}}, ["s"], [])

# ---------------------------------------------------------------- utilities
check("equals num", wf([step("s", "equals", {"a": 1, "b": 1.0})]),
      "completed", {"s": True}, ["s"], [])
check("equals obj order", wf([step("s", "equals", {"a": {"x": 1, "y": [2]},
                                                  "b": {"y": [2.0], "x": 1}})]),
      "completed", {"s": True}, ["s"], [])
check("equals different types", wf([step("s", "equals", {"a": 1, "b": "1"})]),
      "completed", {"s": False}, ["s"], [])
check("compare mixed", wf([step("s", "compare", {"a": "a", "b": 5, "op": "gt"})]),
      "completed", {"s": True}, ["s"], [])
check("compare eq not allowed", wf([step("s", "compare", {"a": 1, "b": 1, "op": "eq"})]),
      "failed", {}, [], [], "INVALID_ARG", "s", 1)
check("and", wf([step("s", "and", {"a": 1, "b": ""})]),
      "completed", {"s": False}, ["s"], [])
check("or", wf([step("s", "or", {"a": 0, "b": "x"})]),
      "completed", {"s": True}, ["s"], [])
check("not", wf([step("s", "not", {"a": []})]),
      "completed", {"s": True}, ["s"], [])
check("type", wf([step("s", "type", {"value": [1]})]),
      "completed", {"s": "array"}, ["s"], [])
check("type null", wf([step("s", "type", {"value": None})]),
      "completed", {"s": "null"}, ["s"], [])

# ---------------------------------------------------------------- refs
check("dotted ref", wf([
    step("a", "constant", {"value": {"user": {"name": "ada", "tags": ["x", "y"]}}}),
    step("b", "constant", {"value": None}, ["a"],
         when={"op": "eq", "left": {"$ref": "a.user.name"}, "right": "ada"}),
    step("c", "get", {"input": {"$ref": "a.user"}, "key": "name"}, ["a", "b"]),
    step("d", "constant", {"value": {"$ref": "a.user.tags.1"}}, ["a"]),
]), "completed",
    {"a": {"user": {"name": "ada", "tags": ["x", "y"]}},
     "b": None, "c": "ada", "d": "y"},
    ["a", "b", "c", "d"], [])

check("bad ref path", wf([
    step("a", "constant", {"value": {"x": 1}}),
    step("b", "constant", {"value": {"$ref": "a.y"}}, ["a"]),
]), "failed", {"a": {"x": 1}}, ["a"], [], "INVALID_REFERENCE", "b", 1)
check("bad array index", wf([
    step("a", "constant", {"value": [1, 2]}),
    step("b", "constant", {"value": {"$ref": "a.-1"}}, ["a"]),
]), "failed", {"a": [1, 2]}, ["a"], [], "INVALID_REFERENCE", "b", 1)
check("bad array index text", wf([
    step("a", "constant", {"value": [1, 2]}),
    step("b", "constant", {"value": {"$ref": "a.x"}}, ["a"]),
]), "failed", {"a": [1, 2]}, ["a"], [], "INVALID_REFERENCE", "b", 1)
check("ref out of bounds", wf([
    step("a", "constant", {"value": [1, 2]}),
    step("b", "constant", {"value": {"$ref": "a.5"}}, ["a"]),
]), "failed", {"a": [1, 2]}, ["a"], [], "INVALID_REFERENCE", "b", 1)
check("ref unknown step", wf([
    step("a", "constant", {"value": 1}),
    step("b", "constant", {"value": {"$ref": "nope"}}, ["a"]),
]), "failed", {"a": 1}, ["a"], [], "INVALID_REFERENCE", "b", 1)
check("ref non-dependency", wf([
    step("a", "constant", {"value": 1}),
    step("b", "constant", {"value": {"$ref": "a"}}),
]), "failed", {"a": 1}, ["a"], [], "INVALID_REFERENCE", "b", 1)
check("ref skipped", wf([
    step("a", "constant", {"value": 1}),
    step("b", "constant", {"value": 1}, ["a"],
         when={"op": "gt", "left": {"$ref": "a"}, "right": 5}),
    step("c", "constant", {"value": {"$ref": "b"}}, ["b"]),
]), "failed", {"a": 1, "b": None}, ["a", "b"], ["b"],
    "SKIPPED_REFERENCE", "c", 1)

# ---------------------------------------------------------------- conditions
check("condition all/any/not", wf([
    step("a", "constant", {"value": 5}),
    step("b", "constant", {"value": 1}, ["a"],
         when={"op": "all", "conditions": [
             {"op": "gt", "left": {"$ref": "a"}, "right": 4},
             {"op": "not", "condition": {"op": "eq", "left": {"$ref": "a"}, "right": 0}},
             {"op": "any", "conditions": [
                 {"op": "lt", "left": 1, "right": 2},
                 {"op": "eq", "left": 1, "right": 2},
             ]},
         ]}),
]), "completed", {"a": 5, "b": 1}, ["a", "b"], [])
check("condition literal bool", wf([
    step("a", "constant", {"value": 1}, when=True),
    step("b", "constant", {"value": 2}, when=False),
]), "completed", {"a": 1, "b": None}, ["a", "b"], ["b"])
check("condition ref bool", wf([
    step("a", "constant", {"value": True}),
    step("b", "constant", {"value": 2}, ["a"], when={"$ref": "a"}),
]), "completed", {"a": True, "b": 2}, ["a", "b"], [])
check("condition non-bool", wf([
    step("a", "constant", {"value": 1}),
    step("b", "constant", {"value": 2}, ["a"],
         when={"op": "eq", "left": {"$ref": "a"}, "right": 1}),
]), "completed", {"a": 1, "b": 2}, ["a", "b"], [])

# ---------------------------------------------------------------- retry / fail
check("retry succeeds", wf([step("f", "fail_until", {"failures": 2, "value": "ok"},
                                 retry={"max_attempts": 3})]),
      "completed", {"f": "ok"}, ["f"], [])
check("retry exhausted", wf([step("f", "fail_until", {"failures": 3, "value": "ok"},
                                  retry={"max_attempts": 2})]),
      "failed", {}, [], [], "STEP_FAILED", "f", 1)
check("no retry", wf([step("f", "fail_until", {"failures": 1, "value": "ok"})]),
      "failed", {}, [], [], "STEP_FAILED", "f", 1)
check("fail_until zero", wf([step("f", "fail_until", {"failures": 0, "value": 42})]),
      "completed", {"f": 42}, ["f"], [])
check("fail_until out of range", wf([step("f", "fail_until", {"failures": 11, "value": 1})]),
      "failed", {}, [], [], "INVALID_ARG", "f", 1)

# ---------------------------------------------------------------- structure
check("bad json", "{not json", "failed", {}, [], [], "INVALID_WORKFLOW", None, 2)
check("missing version", {"steps": []}, "failed", {}, [], [], "INVALID_WORKFLOW", None, 2)
check("version float", {"version": 1.0, "steps": []},
      "failed", {}, [], [], "INVALID_WORKFLOW", None, 2)
check("version string", {"version": "1", "steps": []},
      "failed", {}, [], [], "INVALID_WORKFLOW", None, 2)
check("unknown top field", {"version": 1, "steps": [], "x": 1},
      "failed", {}, [], [], "INVALID_WORKFLOW", None, 2)
check("steps not array", {"version": 1, "steps": {}},
      "failed", {}, [], [], "INVALID_WORKFLOW", None, 2)
check("unknown step field", wf([{"id": "a", "op": "constant", "nope": 1}]),
      "failed", {}, [], [], "INVALID_WORKFLOW", "a", 2)
check("unknown op", wf([step("a", "frobnicate")]),
      "failed", {}, [], [], "INVALID_WORKFLOW", "a", 2)
check("bad id", wf([step("1a", "constant", {"value": 1})]),
      "failed", {}, [], [], "INVALID_WORKFLOW", "1a", 2)
check("duplicate id", wf([step("a", "constant", {"value": 1}),
                          step("a", "constant", {"value": 2})]),
      "failed", {}, [], [], "INVALID_WORKFLOW", "a", 2)
check("missing id", wf([{"op": "constant", "args": {"value": 1}}]),
      "failed", {}, [], [], "INVALID_WORKFLOW", None, 2)
check("id not string", wf([{"id": 5, "op": "constant", "args": {"value": 1}}]),
      "failed", {}, [], [], "INVALID_WORKFLOW", None, 2)
check("args not object", wf([{"id": "a", "op": "constant", "args": 5}]),
      "failed", {}, [], [], "INVALID_WORKFLOW", "a", 2)
check("unknown dependency", wf([step("a", "constant", {"value": 1}, ["nope"])]),
      "failed", {}, [], [], "UNKNOWN_DEPENDENCY", "a", 2)
check("cycle", wf([step("a", "constant", {"value": 1}, ["b"]),
                   step("b", "constant", {"value": 2}, ["a"])]),
      "failed", {}, [], [], "CYCLIC_DEPENDENCY", None, 2)
check("self cycle", wf([step("a", "constant", {"value": 1}, ["a"])]),
      "failed", {}, [], [], "CYCLIC_DEPENDENCY", None, 2)
check("retry out of range", wf([step("a", "constant", {"value": 1},
                                     retry={"max_attempts": 6})]),
      "failed", {}, [], [], "INVALID_WORKFLOW", "a", 2)
check("retry zero", wf([step("a", "constant", {"value": 1},
                             retry={"max_attempts": 0})]),
      "failed", {}, [], [], "INVALID_WORKFLOW", "a", 2)
check("bad when op", wf([step("a", "constant", {"value": 1}, when={"op": "wat"})]),
      "failed", {}, [], [], "INVALID_WORKFLOW", "a", 2)
check("bad when shape", wf([step("a", "constant", {"value": 1},
                                 when={"op": "gt", "left": 1})]),
      "failed", {}, [], [], "INVALID_WORKFLOW", "a", 2)

# ---------------------------------------------------------------- ordering
check("topological tie-break", wf([
    step("a", "constant", {"value": 1}),
    step("b", "constant", {"value": 2}, ["a"]),
    step("c", "constant", {"value": 3}),
    step("d", "constant", {"value": 4}, ["b", "c"]),
]), "completed", {"a": 1, "b": 2, "c": 3, "d": 4}, ["a", "b", "c", "d"], [])
check("independent order", wf([
    step("z", "constant", {"value": 1}),
    step("a", "constant", {"value": 2}),
]), "completed", {"z": 1, "a": 2}, ["z", "a"], [])

# ---------------------------------------------------------------- compatibility forms
check("bare predicate", wf([step("s", "filter",
      {"input": [1, 2, 3], "predicate": "even"})]),
      "completed", {"s": [2]}, ["s"], [])
check("bare transform", wf([step("s", "map",
      {"input": [1, 2], "transform": "stringify"})]),
      "completed", {"s": ["1", "2"]}, ["s"], [])
check("when non-boolean", wf([step("s", "constant", {"value": 1}, when=5)]),
      "failed", {}, [], [], "INVALID_CONDITION", "s", 1)
check("when ref non-boolean", wf([
    step("a", "constant", {"value": 1}),
    step("b", "constant", {"value": 2}, ["a"], when={"$ref": "a"}),
]), "failed", {"a": 1}, ["a"], [], "INVALID_CONDITION", "b", 1)

# ---------------------------------------------------------------- stdin
check("stdin", wf([step("a", "constant", {"value": 7})]),
      "completed", {"a": 7}, ["a"], [], stdin=True)

# ---------------------------------------------------------------- report
if failures:
    print("%d/%d checks FAILED" % (len(failures), count))
    for name, problems, out in failures:
        print("\nFAIL:", name)
        for p in problems:
            print("   -", p)
        print("   output:", json.dumps(out))
    sys.exit(1)
print("all %d checks passed" % count)
