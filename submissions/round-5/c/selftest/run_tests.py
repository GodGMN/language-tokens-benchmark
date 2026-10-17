#!/usr/bin/env python3
"""Self-test harness for the `workflow` C implementation.

Runs each case through `./workflow run -` on stdin and compares the
parsed JSON response and the process exit code. Development aid only.
"""
import json
import subprocess
import sys

BIN = "./workflow"

CASES = []  # (name, input_json_string, expected_response, expected_exit)


def add(name, wf_obj, expected, exit_code):
    CASES.append((name, json.dumps(wf_obj), expected, exit_code))


def wf(steps):
    return {"version": 1, "steps": steps}


def step(id, op, args=None, **kw):
    s = {"id": id, "op": op}
    if args is not None:
        s["args"] = args
    s.update(kw)
    return s


def ok(name, steps, results, order=None, skipped=None):
    expected = {
        "status": "completed",
        "results": results,
        "execution_order": order if order is not None else list(results.keys()),
        "skipped": skipped or [],
        "error": None,
    }
    add(name, wf(steps), expected, 0)


def fail(name, steps, code, step_id, exit_code=1):
    expected = {
        "status": "failed",
        "results": {},
        "execution_order": [],
        "skipped": [],
        "error": {"code": code, "step": step_id, "message": None},
    }
    add(name, wf(steps), expected, exit_code)


def fail_partial(name, steps, results, order, skipped, code, step_id):
    expected = {
        "status": "failed",
        "results": results,
        "execution_order": order,
        "skipped": skipped,
        "error": {"code": code, "step": step_id, "message": None},
    }
    add(name, wf(steps), expected, 1)


def raw(name, text, expected, exit_code):
    CASES.append((name, text, expected, exit_code))


def invalid_wf(name, wf_obj, step_id=None):
    raw(name, json.dumps(wf_obj), {
        "status": "failed", "results": {}, "execution_order": [], "skipped": [],
        "error": {"code": "INVALID_WORKFLOW", "step": step_id, "message": None},
    }, 2)


# ---------------------------------------------------------------- math
ok("add", [step("a", "add", {"a": 2, "b": 3})], {"a": 5})
ok("subtract-neg", [step("a", "subtract", {"a": 2, "b": 3})], {"a": -1})
ok("multiply", [step("a", "multiply", {"a": -4, "b": 3})], {"a": -12})
ok("divide-trunc-pos", [step("a", "divide", {"a": 7, "b": 2})], {"a": 3})
ok("divide-trunc-neg", [step("a", "divide", {"a": -7, "b": 2})], {"a": -3})
ok("divide-trunc-neg2", [step("a", "divide", {"a": -7, "b": -2})], {"a": 3})
ok("mod-pos", [step("a", "mod", {"a": 7, "b": 2})], {"a": 1})
ok("mod-neg-dividend", [step("a", "mod", {"a": -7, "b": 2})], {"a": -1})
ok("mod-neg-divisor", [step("a", "mod", {"a": 7, "b": -2})], {"a": 1})
ok("sum", [step("a", "sum", {"input": [1, 2, 3]})], {"a": 6})
ok("sum-empty", [step("a", "sum", {"input": []})], {"a": 0})
ok("min", [step("a", "min", {"input": [3, -1, 2]})], {"a": -1})
ok("max", [step("a", "max", {"input": [3, -1, 2]})], {"a": 3})
ok("range", [step("a", "range", {"start": 1, "end": 5})], {"a": [1, 2, 3, 4, 5]})
ok("range-empty", [step("a", "range", {"start": 5, "end": 1})], {"a": []})
ok("range-single", [step("a", "range", {"start": -2, "end": -2})], {"a": [-2]})
fail("min-empty", [step("a", "min", {"input": []})], "EMPTY_ARRAY", "a")
fail("max-empty", [step("a", "max", {"input": []})], "EMPTY_ARRAY", "a")
fail("div-zero", [step("a", "divide", {"a": 1, "b": 0})], "DIVISION_BY_ZERO", "a")
fail("mod-zero", [step("a", "mod", {"a": 1, "b": 0})], "DIVISION_BY_ZERO", "a")
fail("add-overflow", [step("a", "add", {"a": 9223372036854775807, "b": 1})],
     "ARITHMETIC_OVERFLOW", "a")
fail("mul-overflow", [step("a", "multiply", {"a": 4611686018427387904, "b": 4})],
     "ARITHMETIC_OVERFLOW", "a")
fail("minint-div", [step("a", "divide", {"a": -9223372036854775808, "b": -1})],
     "ARITHMETIC_OVERFLOW", "a")
fail("minint-mod", [step("a", "mod", {"a": -9223372036854775808, "b": -1})],
     "ARITHMETIC_OVERFLOW", "a")
fail("float-arg", [step("a", "add", {"a": 1.0, "b": 2})], "INVALID_ARG", "a")
fail("exp-arg", [step("a", "add", {"a": 1e3, "b": 2})], "INVALID_ARG", "a")
fail("unknown-arg", [step("a", "add", {"a": 1, "b": 2, "c": 3})], "INVALID_ARG", "a")
fail("missing-arg", [step("a", "add", {"a": 1})], "INVALID_ARG", "a")
fail("str-for-num", [step("a", "add", {"a": "1", "b": 2})], "INVALID_ARG", "a")

# ------------------------------------------------------------- strings
ok("concat", [step("a", "concat", {"a": "foo", "b": "bar"})], {"a": "foobar"})
ok("split", [step("a", "split", {"input": "a,b,,c", "sep": ","})],
   {"a": ["a", "b", "", "c"]})
ok("split-trailing", [step("a", "split", {"input": "a,", "sep": ","})],
   {"a": ["a", ""]})
ok("split-empty-sep", [step("a", "split", {"input": "h\u00e9!", "sep": ""})],
   {"a": ["h", "\u00e9", "!"]})
ok("split-empty-input", [step("a", "split", {"input": "", "sep": ","})], {"a": [""]})
ok("split-no-match", [step("a", "split", {"input": "abc", "sep": "x"})], {"a": ["abc"]})
ok("join", [step("a", "join", {"input": ["a", "b"], "sep": "-"})], {"a": "a-b"})
ok("join-empty", [step("a", "join", {"input": [], "sep": "-"})], {"a": ""})
ok("upper", [step("a", "uppercase", {"input": "aZ\u00e9"})], {"a": "AZ\u00e9"})
ok("lower", [step("a", "lowercase", {"input": "Az\u00c9"})], {"a": "az\u00c9"})
ok("trim", [step("a", "trim", {"input": " \t\n ab \r\f\v "})], {"a": "ab"})
ok("replace", [step("a", "replace", {"input": "aaa", "search": "aa", "replace": "b"})],
   {"a": "ba"})
ok("replace-empty-rep",
   [step("a", "replace", {"input": "abc", "search": "b", "replace": ""})], {"a": "ac"})
fail("replace-empty-search",
     [step("a", "replace", {"input": "abc", "search": "", "replace": "x"})],
     "INVALID_ARG", "a")
ok("length-ascii", [step("a", "length", {"input": "abc"})], {"a": 3})
ok("length-unicode", [step("a", "length", {"input": "\u00e9\u4e2d\U0001f600"})],
   {"a": 3})

# -------------------------------------------------------------- arrays
ok("map-add", [step("a", "map",
                    {"input": [1, 2, 3], "transform": {"type": "add", "value": 10}})],
   {"a": [11, 12, 13]})
ok("map-negate", [step("a", "map", {"input": [1, -2], "transform": {"type": "negate"}})],
   {"a": [-1, 2]})
ok("map-abs", [step("a", "map", {"input": [-1, 2], "transform": {"type": "abs"}})],
   {"a": [1, 2]})
ok("map-constant",
   [step("a", "map", {"input": [1, 2], "transform": {"type": "constant", "value": "x"}})],
   {"a": ["x", "x"]})
ok("map-stringify",
   [step("a", "map", {"input": [1, "a", [1]], "transform": {"type": "stringify"}})],
   {"a": ["1", "\"a\"", "[1]"]})
ok("map-type", [step("a", "map", {"input": [1, "a", None], "transform": {"type": "type"}})],
   {"a": ["number", "string", "null"]})
ok("map-op-form",
   [step("a", "map", {"input": [1, 2],
                      "transform": {"type": "op", "op": "add",
                                    "args": {"a": {"$self": True}, "b": 2}}})],
   {"a": [3, 4]})
ok("map-div", [step("a", "map",
                    {"input": [4, 5], "transform": {"type": "divide", "value": 2}})],
   {"a": [2, 2]})
fail("map-div-zero",
     [step("a", "map", {"input": [4], "transform": {"type": "divide", "value": 0}})],
     "DIVISION_BY_ZERO", "a")
fail("map-bad-type", [step("a", "map", {"input": [1], "transform": {"type": "zzz"}})],
     "INVALID_ARG", "a")
ok("filter-even", [step("a", "filter",
                        {"input": [1, 2, 3, 4], "predicate": {"type": "even"}})],
   {"a": [2, 4]})
ok("filter-odd", [step("a", "filter",
                       {"input": [1, 2, 3], "predicate": {"type": "odd"}})],
   {"a": [1, 3]})
ok("filter-gt", [step("a", "filter",
                      {"input": [1, 5, 10], "predicate": {"type": "gt", "value": 4}})],
   {"a": [5, 10]})
ok("filter-lte", [step("a", "filter",
                       {"input": [1, 5, 10], "predicate": {"type": "lte", "value": 5}})],
   {"a": [1, 5]})
ok("filter-truthy",
   [step("a", "filter",
         {"input": [0, 1, "", "x", [], [1], {}, {"a": 1}, None, False, True],
          "predicate": {"type": "truthy"}})],
   {"a": [1, "x", [1], {"a": 1}, True]})
ok("filter-falsy",
   [step("a", "filter",
         {"input": [0, 1, "", "x", [], [1], {}, {"a": 1}, None, False, True],
          "predicate": {"type": "falsy"}})],
   {"a": [0, "", [], {}, None, False]})
ok("filter-type-is",
   [step("a", "filter",
         {"input": [1, "x", None, True, [], {}],
          "predicate": {"type": "type_is", "value": "string"}})],
   {"a": ["x"]})
raw("filter-type-is-dup-key",
    '{"version":1,"steps":[{"id":"a","op":"filter","args":'
    '{"input":[1,"x",null],"predicate":{"type":"type_is","type":"number"}}}]}',
    {"status": "completed", "results": {"a": [1]}, "execution_order": ["a"],
     "skipped": [], "error": None}, 0)
raw("filter-type-is-type-name",
    '{"version":1,"steps":[{"id":"a","op":"filter","args":'
    '{"input":[1,"x",null],"predicate":{"type":"type_is","type_name":"number"}}}]}',
    {"status": "completed", "results": {"a": [1]}, "execution_order": ["a"],
     "skipped": [], "error": None}, 0)
ok("filter-contains",
   [step("a", "filter",
         {"input": ["apple", "banana", "cherry"],
          "predicate": {"type": "string_contains", "value": "an"}})],
   {"a": ["banana"]})
ok("filter-eq", [step("a", "filter",
                      {"input": [1, 2, 1], "predicate": {"type": "eq", "value": 1}})],
   {"a": [1, 1]})
ok("reduce-add",
   [step("a", "reduce", {"input": [1, 2, 3], "op": "add", "initial": 10})], {"a": 16})
ok("reduce-default", [step("a", "reduce", {"input": [1, 2, 3], "op": "multiply"})],
   {"a": 6})
ok("reduce-sub-default", [step("a", "reduce", {"input": [1, 2, 3], "op": "subtract"})],
   {"a": -6})
ok("reduce-empty", [step("a", "reduce", {"input": [], "op": "add"})], {"a": 0})
ok("sort-mixed",
   [step("a", "sort", {"input": [True, "b", 2, None, [1], {"x": 1}, False, "a", 1]})],
   {"a": [1, 2, "a", "b", False, True, None, [1], {"x": 1}]})
ok("sort-stable",
   [step("a", "sort", {"input": [{"k": 1, "n": "a"}, {"k": 1, "n": "b"}]})],
   {"a": [{"k": 1, "n": "a"}, {"k": 1, "n": "b"}]})
ok("sort-unicode-bytes", [step("a", "sort", {"input": ["\u00e9", "z", "a"]})],
   {"a": ["a", "z", "\u00e9"]})
ok("sort-arrays-prefix", [step("a", "sort", {"input": [[1, 2], [1], []]})],
   {"a": [[], [1], [1, 2]]})
ok("reverse", [step("a", "reverse", {"input": [1, 2, 3]})], {"a": [3, 2, 1]})
ok("unique",
   [step("a", "unique", {"input": [1, 2, 1, "1", 2.0, {"x": 1}, {"x": 1}]})],
   {"a": [1, 2, "1", {"x": 1}]})
ok("slice", [step("a", "slice", {"input": [0, 1, 2, 3, 4], "start": 1, "end": 3})],
   {"a": [1, 2]})
ok("slice-neg", [step("a", "slice", {"input": [0, 1, 2, 3, 4], "start": -2})],
   {"a": [3, 4]})
ok("slice-neg-end",
   [step("a", "slice", {"input": [0, 1, 2, 3, 4], "start": 0, "end": -1})],
   {"a": [0, 1, 2, 3]})
ok("slice-clamp", [step("a", "slice", {"input": [0, 1, 2], "start": -100, "end": 100})],
   {"a": [0, 1, 2]})
ok("slice-reverse-range",
   [step("a", "slice", {"input": [0, 1, 2], "start": 2, "end": 1})], {"a": []})
ok("flatten", [step("a", "flatten", {"input": [1, [2, 3], [[4]], 5]})],
   {"a": [1, 2, 3, [4], 5]})

# ------------------------------------------------------------- objects
ok("get", [step("a", "get", {"input": {"x": 1}, "key": "x"})], {"a": 1})
ok("get-missing", [step("a", "get", {"input": {"x": 1}, "key": "y"})], {"a": None})
ok("set", [step("a", "set", {"input": {"x": 1}, "key": "y", "value": 2})],
   {"a": {"x": 1, "y": 2}})
ok("set-overwrite", [step("a", "set", {"input": {"x": 1}, "key": "x", "value": 2})],
   {"a": {"x": 2}})
ok("keys-sorted", [step("a", "keys", {"input": {"b": 1, "a": 2, "c": 3}})],
   {"a": ["a", "b", "c"]})
ok("values-sorted", [step("a", "values", {"input": {"b": 1, "a": 2, "c": 3}})],
   {"a": [2, 1, 3]})
ok("merge", [step("a", "merge", {"a": {"x": 1, "y": 1}, "b": {"y": 2, "z": 3}})],
   {"a": {"x": 1, "y": 2, "z": 3}})
ok("pick", [step("a", "pick", {"input": {"a": 1, "b": 2, "c": 3}, "keys": ["c", "a"]})],
   {"a": {"a": 1, "c": 3}})
ok("pick-missing", [step("a", "pick", {"input": {"a": 1}, "keys": ["a", "z"]})],
   {"a": {"a": 1, "z": None}})
ok("omit", [step("a", "omit", {"input": {"a": 1, "b": 2}, "keys": ["a"]})],
   {"a": {"b": 2}})
fail("get-non-object", [step("a", "get", {"input": [1], "key": "x"})],
     "INVALID_ARG", "a")

# ----------------------------------------------------------- utilities
ok("constant", [step("a", "constant", {"value": {"k": [1, 2]}})], {"a": {"k": [1, 2]}})
ok("constant-float-passthrough",
   [step("a", "constant", {"value": 1.5})], {"a": 1.5})
ok("equals-num", [step("a", "equals", {"a": 1, "b": 1.0})], {"a": True})
ok("equals-obj-order",
   [step("a", "equals", {"a": {"x": 1, "y": 2}, "b": {"y": 2, "x": 1}})], {"a": True})
ok("equals-types", [step("a", "equals", {"a": True, "b": 1})], {"a": False})
ok("equals-array", [step("a", "equals", {"a": [1, [2]], "b": [1, [2]]})], {"a": True})
ok("compare-num", [step("a", "compare", {"a": 2, "b": 3, "op": "lt"})], {"a": True})
ok("compare-str", [step("a", "compare", {"a": "b", "b": "a", "op": "gt"})], {"a": True})
ok("compare-mixed", [step("a", "compare", {"a": 2, "b": "a", "op": "lt"})], {"a": True})
ok("compare-null-max", [step("a", "compare", {"a": {}, "b": None, "op": "gt"})],
   {"a": True})
ok("and", [step("a", "and", {"a": 1, "b": ""})], {"a": False})
ok("or", [step("a", "or", {"a": 0, "b": "x"})], {"a": True})
ok("not", [step("a", "not", {"a": []})], {"a": True})
ok("type-num", [step("a", "type", {"value": 1.5})], {"a": "number"})
ok("type-arr", [step("a", "type", {"value": []})], {"a": "array"})
ok("fail-until-zero", [step("a", "fail_until", {"failures": 0, "value": 7})], {"a": 7})

# ------------------------------------------------------------- ordering
ok("topo-tiebreak",
   [step("b", "constant", {"value": 2}),
    step("a", "constant", {"value": 1}),
    step("c", "add", {"a": {"$ref": "b"}, "b": {"$ref": "a"}},
         depends_on=["b", "a"])],
   {"b": 2, "a": 1, "c": 3}, order=["b", "a", "c"])

# ---------------------------------------------------------------- refs
ok("ref-path",
   [step("a", "constant", {"value": {"user": {"name": "bo"}, "items": [10, 20]}}),
    step("b", "constant", {"value": {"$ref": "a.user.name"}}, depends_on=["a"]),
    step("c", "constant", {"value": {"$ref": "a.items.1"}}, depends_on=["a"])],
   {"a": {"user": {"name": "bo"}, "items": [10, 20]}, "b": "bo", "c": 20})
ok("ref-transitive",
   [step("a", "constant", {"value": 5}),
    step("b", "add", {"a": {"$ref": "a"}, "b": 1}, depends_on=["a"]),
    step("c", "multiply", {"a": {"$ref": "b"}, "b": 2}, depends_on=["b"])],
   {"a": 5, "b": 6, "c": 12})
ok("ref-nested-args",
   [step("a", "constant", {"value": 3}),
    step("b", "sum", {"input": [{"$ref": "a"}, 4, {"$ref": "a"}]}, depends_on=["a"])],
   {"a": 3, "b": 10})
ok("ref-nonref-object",
   [step("a", "constant", {"value": 3}),
    step("b", "constant", {"value": {"$ref": "a", "extra": 1}}, depends_on=["a"])],
   {"a": 3, "b": {"$ref": "a", "extra": 1}})
ok("ref-array-of-objects",
   [step("a", "constant", {"value": [{"k": [1, 2]}]}),
    step("b", "constant", {"value": {"$ref": "a.0.k.1"}}, depends_on=["a"])],
   {"a": [{"k": [1, 2]}], "b": 2})
ok("ref-null-path",
   [step("a", "constant", {"value": {"x": None}}),
    step("b", "constant", {"value": {"$ref": "a.x"}}, depends_on=["a"])],
   {"a": {"x": None}, "b": None})
ok("when-ref",
   [step("a", "constant", {"value": 5}),
    step("b", "constant", {"value": 1}, depends_on=["a"],
         when={"op": "eq", "left": {"$ref": "a"}, "right": 5})],
   {"a": 5, "b": 1})
fail_partial("ref-missing-path",
             [step("a", "constant", {"value": {"x": 1}}),
              step("b", "constant", {"value": {"$ref": "a.y"}}, depends_on=["a"])],
             {"a": {"x": 1}}, ["a"], [], "INVALID_REFERENCE", "b")
fail_partial("ref-bad-index",
             [step("a", "constant", {"value": [1]}),
              step("b", "constant", {"value": {"$ref": "a.x"}}, depends_on=["a"])],
             {"a": [1]}, ["a"], [], "INVALID_REFERENCE", "b")
fail_partial("ref-negative-index",
             [step("a", "constant", {"value": [1, 2]}),
              step("b", "constant", {"value": {"$ref": "a.-1"}}, depends_on=["a"])],
             {"a": [1, 2]}, ["a"], [], "INVALID_REFERENCE", "b")
fail_partial("ref-index-out-of-range",
             [step("a", "constant", {"value": [1]}),
              step("b", "constant", {"value": {"$ref": "a.1"}}, depends_on=["a"])],
             {"a": [1]}, ["a"], [], "INVALID_REFERENCE", "b")
fail("ref-self", [step("a", "constant", {"value": {"$ref": "a"}})],
     "INVALID_REFERENCE", "a")
fail_partial("ref-non-dependency",
             [step("a", "constant", {"value": 1}),
              step("b", "constant", {"value": {"$ref": "a"}})],
             {"a": 1}, ["a"], [], "INVALID_REFERENCE", "b")
fail_partial("ref-missing-target",
             [step("a", "constant", {"value": 1}),
              step("b", "constant", {"value": {"$ref": "nope"}}, depends_on=["a"])],
             {"a": 1}, ["a"], [], "INVALID_REFERENCE", "b")
fail_partial("ref-skipped",
             [step("a", "constant", {"value": 1},
                   when={"op": "eq", "left": 1, "right": 2}),
              step("b", "constant", {"value": {"$ref": "a"}}, depends_on=["a"])],
             {"a": None}, ["a"], ["a"], "SKIPPED_REFERENCE", "b")

# ---------------------------------------------------------- conditions
def when_pair(when):
    return [step("a", "constant", {"value": 5}),
            step("b", "constant", {"value": 1}, depends_on=["a"], when=when)]


ok("when-true", when_pair({"op": "gte", "left": {"$ref": "a"}, "right": 5}),
   {"a": 5, "b": 1})
ok("when-false", when_pair({"op": "lt", "left": {"$ref": "a"}, "right": 5}),
   {"a": 5, "b": None}, skipped=["b"])
ok("when-ne", when_pair({"op": "ne", "left": {"$ref": "a"}, "right": 5}),
   {"a": 5, "b": None}, skipped=["b"])
ok("when-all", when_pair({"op": "all", "conditions": [
    {"op": "gt", "left": 2, "right": 1},
    {"op": "lt", "left": 2, "right": 3}]}), {"a": 5, "b": 1})
ok("when-all-empty", when_pair({"op": "all", "conditions": []}), {"a": 5, "b": 1})
ok("when-any-empty", when_pair({"op": "any", "conditions": []}),
   {"a": 5, "b": None}, skipped=["b"])
ok("when-any", when_pair({"op": "any", "conditions": [
    {"op": "gt", "left": 2, "right": 3},
    {"op": "lt", "left": 2, "right": 3}]}), {"a": 5, "b": 1})
ok("when-not", when_pair({"op": "not", "condition": {"op": "gt", "left": 2, "right": 3}}),
   {"a": 5, "b": 1})
ok("when-eq-types", when_pair({"op": "eq", "left": 1, "right": 1.0}), {"a": 5, "b": 1})
ok("when-nested",
   when_pair({"op": "not", "condition": {"op": "all", "conditions": [
       {"op": "gt", "left": 2, "right": 3},
       {"op": "lt", "left": 2, "right": 3}]}}), {"a": 5, "b": 1})
fail("when-bad-op",
     [step("a", "constant", {"value": 1}, when={"op": "bogus", "left": 1, "right": 1})],
     "INVALID_CONDITION", "a")
fail("when-missing-left",
     [step("a", "constant", {"value": 1}, when={"op": "eq", "right": 1})],
     "INVALID_CONDITION", "a")
fail("when-bad-conditions",
     [step("a", "constant", {"value": 1}, when={"op": "all", "conditions": "x"})],
     "INVALID_CONDITION", "a")
fail_partial("when-ref-nondep",
             [step("a", "constant", {"value": 1}),
              step("b", "constant", {"value": 1},
                   when={"op": "eq", "left": {"$ref": "a"}, "right": 1})],
             {"a": 1}, ["a"], [], "INVALID_REFERENCE", "b")

# ------------------------------------------------------------- retries
ok("retry-once", [step("a", "fail_until", {"failures": 1, "value": 9},
                       retry={"max_attempts": 2})], {"a": 9})
fail("retry-exhausted",
     [step("a", "fail_until", {"failures": 5, "value": 9},
           retry={"max_attempts": 3})], "STEP_FAILED", "a")
fail("fail-until-no-retry",
     [step("a", "fail_until", {"failures": 1, "value": 9})], "STEP_FAILED", "a")
ok("retry-not-needed", [step("a", "fail_until", {"failures": 0, "value": 9},
                             retry={"max_attempts": 5})], {"a": 9})
fail("no-retry-on-nontransient",
     [step("a", "divide", {"a": 1, "b": 0}, retry={"max_attempts": 5})],
     "DIVISION_BY_ZERO", "a")

# ------------------------------------------------- structural validation
invalid_wf("version-float", {"version": 1.0, "steps": []})
invalid_wf("version-missing", {"steps": []})
invalid_wf("version-2", {"version": 2, "steps": []})
invalid_wf("version-string", {"version": "1", "steps": []})
invalid_wf("steps-missing", {"version": 1})
invalid_wf("steps-not-array", {"version": 1, "steps": {}})
invalid_wf("unknown-top-field", {"version": 1, "steps": [], "extra": 1})
invalid_wf("unknown-step-field",
           wf([{"id": "a", "op": "constant", "args": {"value": 1}, "zzz": 1}]), "a")
invalid_wf("bad-id", wf([{"id": "1bad", "op": "constant", "args": {"value": 1}}]),
           "1bad")
invalid_wf("dup-id", wf([{"id": "a", "op": "constant", "args": {"value": 1}},
                         {"id": "a", "op": "constant", "args": {"value": 2}}]), "a")
invalid_wf("unknown-op", wf([{"id": "a", "op": "bogus"}]), "a")
invalid_wf("retry-range",
           wf([{"id": "a", "op": "constant", "retry": {"max_attempts": 6}}]), "a")
invalid_wf("retry-zero",
           wf([{"id": "a", "op": "constant", "retry": {"max_attempts": 0}}]), "a")
invalid_wf("args-not-object", wf([{"id": "a", "op": "constant", "args": 1}]), "a")
invalid_wf("when-not-object", wf([{"id": "a", "op": "constant", "when": 1}]), "a")
invalid_wf("step-not-object", {"version": 1, "steps": [1]})
invalid_wf("top-level-array", [1, 2])
raw("bad-json", "{not json", {
    "status": "failed", "results": {}, "execution_order": [], "skipped": [],
    "error": {"code": "INVALID_WORKFLOW", "step": None, "message": None}}, 2)
raw("empty-input", "", {
    "status": "failed", "results": {}, "execution_order": [], "skipped": [],
    "error": {"code": "INVALID_WORKFLOW", "step": None, "message": None}}, 2)

raw("unknown-dep",
    json.dumps(wf([{"id": "a", "op": "constant", "depends_on": ["zz"]}])), {
        "status": "failed", "results": {}, "execution_order": [], "skipped": [],
        "error": {"code": "UNKNOWN_DEPENDENCY", "step": "a", "message": None}}, 2)
raw("cycle",
    json.dumps(wf([{"id": "a", "op": "constant", "depends_on": ["b"]},
                   {"id": "b", "op": "constant", "depends_on": ["a"]}])), {
        "status": "failed", "results": {}, "execution_order": [], "skipped": [],
        "error": {"code": "CYCLIC_DEPENDENCY", "step": None, "message": None}}, 2)
raw("self-cycle",
    json.dumps(wf([{"id": "a", "op": "constant", "depends_on": ["a"]}])), {
        "status": "failed", "results": {}, "execution_order": [], "skipped": [],
        "error": {"code": "CYCLIC_DEPENDENCY", "step": None, "message": None}}, 2)
raw("empty-workflow", json.dumps({"version": 1, "steps": []}), {
    "status": "completed", "results": {}, "execution_order": [], "skipped": [],
    "error": None}, 0)

# -------------------------------------------- partial results on failure
fail_partial("partial-results",
             [step("a", "constant", {"value": 1}),
              step("b", "constant", {"value": 2}, depends_on=["a"],
                   when={"op": "eq", "left": 1, "right": 2}),
              step("c", "divide", {"a": 1, "b": 0}, depends_on=["a"])],
             {"a": 1, "b": None}, ["a", "b"], ["b"], "DIVISION_BY_ZERO", "c")


def norm(v):
    if isinstance(v, dict):
        return {k: (None if k == "message" else norm(x)) for k, x in v.items()}
    if isinstance(v, list):
        return [norm(x) for x in v]
    return v


def main():
    total = 0
    failed_n = 0
    for name, input_json, expected, exit_code in CASES:
        total += 1
        proc = subprocess.run([BIN, "run", "-"], input=input_json.encode(),
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            got = json.loads(proc.stdout)
        except Exception:
            failed_n += 1
            print(f"FAIL {name}: invalid JSON output {proc.stdout!r} "
                  f"stderr={proc.stderr!r}")
            continue
        if norm(got) != norm(expected) or proc.returncode != exit_code:
            failed_n += 1
            print(f"FAIL {name}: exit {proc.returncode} (want {exit_code})")
            print(f"  got:  {json.dumps(got, sort_keys=True)}")
            print(f"  want: {json.dumps(expected, sort_keys=True)}")
    print(f"\n{total - failed_n}/{total} passed")
    return 1 if failed_n else 0


if __name__ == "__main__":
    sys.exit(main())
