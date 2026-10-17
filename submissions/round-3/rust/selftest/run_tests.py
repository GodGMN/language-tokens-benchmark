#!/usr/bin/env python3
"""Self-test harness for the workflow CLI.

Runs the release binary against a set of inline workflows and checks the
response fields that the SPEC defines.  Usage:

    python3 selftest/run_tests.py [path-to-binary]
"""

import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_BIN = os.path.join(HERE, "..", "target", "release", "workflow")

CASES = []


def case(name, doc, exit_code, status, results=None, order=None,
         skipped=None, code=None, step="__unset__"):
    CASES.append(dict(name=name, doc=doc, exit_code=exit_code, status=status,
                      results=results, order=order, skipped=skipped,
                      code=code, step=step))


def wf(*steps, version=1):
    return {"version": version, "steps": list(steps)}


def st(id, op, args=None, depends_on=None, when=None, retry=None):
    s = {"id": id, "op": op}
    if args is not None:
        s["args"] = args
    if depends_on is not None:
        s["depends_on"] = depends_on
    if when is not None:
        s["when"] = when
    if retry is not None:
        s["retry"] = retry
    return s


# ---------------------------------------------------------------- misc edges
case("negative_zero_integer", wf(st("r", "add", {"a": -0, "b": 1})),
     0, "completed", {"r": 1}, ["r"], [], None, None)
case("map_empty_bad_transform", wf(st("r", "map", {
    "input": [], "transform": {"type": "bogus"}})),
     1, "failed", {}, [], [], "INVALID_ARG", "r")
case("filter_empty_bad_predicate", wf(st("r", "filter", {
    "input": [], "predicate": {"type": "bogus"}})),
     1, "failed", {}, [], [], "INVALID_ARG", "r")
case("skipped_args_not_resolved", wf(
    st("a", "constant", {"value": 1}),
    st("b", "constant", {"value": {"$ref": "a"}}, depends_on=["a"],
       when={"op": "gt", "left": {"$ref": "a"}, "right": 5})),
     0, "completed", {"a": 1, "b": None}, ["a", "b"], ["b"], None, None)
case("transitive_ref", wf(
    st("a", "constant", {"value": 7}),
    st("b", "constant", {"value": 1}, depends_on=["a"]),
    st("c", "constant", {"value": 1}, depends_on=["b"]),
    st("d", "add", {"a": {"$ref": "a"}, "b": {"$ref": "c"}}, depends_on=["c"])),
     0, "completed", {"a": 7, "b": 1, "c": 1, "d": 8},
     ["a", "b", "c", "d"], [], None, None)
case("huge_integer_constant", wf(st("r", "constant", {"value": 2 ** 63})),
     0, "completed", {"r": 2 ** 63}, ["r"], [], None, None)
case("large_negative_constant", wf(st("r", "constant", {"value": -(2 ** 63)})),
     0, "completed", {"r": -(2 ** 63)}, ["r"], [], None, None)
case("condition_type_order", wf(
    st("a", "constant", {"value": [1]}),
    st("b", "constant", {"value": True}, depends_on=["a"],
       when={"op": "gt", "left": {"$ref": "a"}, "right": True})),
     0, "completed", {"a": [1], "b": True}, ["a", "b"], [], None, None)

# ---------------------------------------------------------------- math
case("add", wf(st("r", "add", {"a": 10, "b": 20})), 0, "completed",
     {"r": 30}, ["r"], [], None, None)
case("subtract", wf(st("r", "subtract", {"a": 3, "b": 10})), 0, "completed",
     {"r": -7}, ["r"], [], None, None)
case("multiply", wf(st("r", "multiply", {"a": -6, "b": 7})), 0, "completed",
     {"r": -42}, ["r"], [], None, None)
case("divide_trunc", wf(st("r", "divide", {"a": -7, "b": 2})), 0, "completed",
     {"r": -3}, ["r"], [], None, None)
case("divide_neg", wf(st("r", "divide", {"a": -7, "b": -2})), 0, "completed",
     {"r": 3}, ["r"], [], None, None)
case("mod_sign", wf(st("r", "mod", {"a": -7, "b": 2})), 0, "completed",
     {"r": -1}, ["r"], [], None, None)
case("mod_neg_divisor", wf(st("r", "mod", {"a": 7, "b": -2})), 0, "completed",
     {"r": 1}, ["r"], [], None, None)
case("div_zero", wf(st("r", "divide", {"a": 1, "b": 0})), 1, "failed",
     {}, [], [], "DIVISION_BY_ZERO", "r")
case("mod_zero", wf(st("r", "mod", {"a": 1, "b": 0})), 1, "failed",
     {}, [], [], "DIVISION_BY_ZERO", "r")
case("overflow_add", wf(st("r", "add", {"a": 9223372036854775807, "b": 1})),
     1, "failed", {}, [], [], "ARITHMETIC_OVERFLOW", "r")
case("overflow_sub", wf(st("r", "subtract", {"a": -9223372036854775808, "b": 1})),
     1, "failed", {}, [], [], "ARITHMETIC_OVERFLOW", "r")
case("overflow_mul", wf(st("r", "multiply", {"a": 9223372036854775807, "b": 2})),
     1, "failed", {}, [], [], "ARITHMETIC_OVERFLOW", "r")
case("overflow_div_min", wf(st("r", "divide", {"a": -9223372036854775808, "b": -1})),
     1, "failed", {}, [], [], "ARITHMETIC_OVERFLOW", "r")
case("float_arg_rejected", wf(st("r", "add", {"a": 1.0, "b": 2})),
     1, "failed", {}, [], [], "INVALID_ARG", "r")
case("exp_arg_rejected", wf(st("r", "add", {"a": 1e3, "b": 2})),
     1, "failed", {}, [], [], "INVALID_ARG", "r")
case("unknown_arg", wf(st("r", "add", {"a": 1, "b": 2, "c": 3})),
     1, "failed", {}, [], [], "INVALID_ARG", "r")
case("missing_arg", wf(st("r", "add", {"a": 1})),
     1, "failed", {}, [], [], "INVALID_ARG", "r")
case("sum_empty", wf(st("r", "sum", {"input": []})), 0, "completed",
     {"r": 0}, ["r"], [], None, None)
case("sum", wf(st("r", "sum", {"input": [1, 2, 3, -4]})), 0, "completed",
     {"r": 2}, ["r"], [], None, None)
case("min", wf(st("r", "min", {"input": [4, -2, 9]})), 0, "completed",
     {"r": -2}, ["r"], [], None, None)
case("max", wf(st("r", "max", {"input": [4, -2, 9]})), 0, "completed",
     {"r": 9}, ["r"], [], None, None)
case("min_empty", wf(st("r", "min", {"input": []})), 1, "failed",
     {}, [], [], "EMPTY_ARRAY", "r")
case("max_empty", wf(st("r", "max", {"input": []})), 1, "failed",
     {}, [], [], "EMPTY_ARRAY", "r")
case("range", wf(st("r", "range", {"start": 2, "end": 5})), 0, "completed",
     {"r": [2, 3, 4, 5]}, ["r"], [], None, None)
case("range_empty", wf(st("r", "range", {"start": 5, "end": 2})), 0, "completed",
     {"r": []}, ["r"], [], None, None)
case("range_single", wf(st("r", "range", {"start": 3, "end": 3})), 0, "completed",
     {"r": [3]}, ["r"], [], None, None)

# ------------------------------------------------------------- strings
case("concat", wf(st("r", "concat", {"a": "ab", "b": "cd"})), 0, "completed",
     {"r": "abcd"}, ["r"], [], None, None)
case("split", wf(st("r", "split", {"input": "a,b,,c", "sep": ","})), 0, "completed",
     {"r": ["a", "b", "", "c"]}, ["r"], [], None, None)
case("split_empty_sep", wf(st("r", "split", {"input": "héllo", "sep": ""})),
     0, "completed", {"r": ["h", "é", "l", "l", "o"]}, ["r"], [], None, None)
case("join", wf(st("r", "join", {"input": ["a", "b"], "sep": "-"})), 0, "completed",
     {"r": "a-b"}, ["r"], [], None, None)
case("join_empty", wf(st("r", "join", {"input": [], "sep": "-"})), 0, "completed",
     {"r": ""}, ["r"], [], None, None)
case("uppercase", wf(st("r", "uppercase", {"input": "aZé1"})), 0, "completed",
     {"r": "AZé1"}, ["r"], [], None, None)
case("lowercase", wf(st("r", "lowercase", {"input": "AbÇ"})), 0, "completed",
     {"r": "abÇ"}, ["r"], [], None, None)
case("trim", wf(st("r", "trim", {"input": " \t\n x \r\f\v "})), 0, "completed",
     {"r": "x"}, ["r"], [], None, None)
case("replace", wf(st("r", "replace", {"input": "aaaa", "search": "aa", "replace": "b"})),
     0, "completed", {"r": "bb"}, ["r"], [], None, None)
case("replace_empty", wf(st("r", "replace", {"input": "abc", "search": "", "replace": "x"})),
     1, "failed", {}, [], [], "INVALID_ARG", "r")
case("length", wf(st("r", "length", {"input": "héllo"})), 0, "completed",
     {"r": 5}, ["r"], [], None, None)

# -------------------------------------------------------------- arrays
case("map_add", wf(st("r", "map", {"input": [1, 2, 3],
                                    "transform": {"type": "add", "value": 10}})),
     0, "completed", {"r": [11, 12, 13]}, ["r"], [], None, None)
case("map_constant", wf(st("r", "map", {"input": [1, 2],
                                         "transform": {"type": "constant", "value": "x"}})),
     0, "completed", {"r": ["x", "x"]}, ["r"], [], None, None)
case("map_negate", wf(st("r", "map", {"input": [1, -2],
                                       "transform": {"type": "negate"}})),
     0, "completed", {"r": [-1, 2]}, ["r"], [], None, None)
case("map_abs", wf(st("r", "map", {"input": [-1, 2],
                                    "transform": {"type": "abs"}})),
     0, "completed", {"r": [1, 2]}, ["r"], [], None, None)
case("map_stringify", wf(st("r", "map", {"input": [1, "a", None, True],
                                          "transform": {"type": "stringify"}})),
     0, "completed", {"r": ["1", "\"a\"", "null", "true"]}, ["r"], [], None, None)
case("map_type", wf(st("r", "map", {"input": [1, "a", None, [], {}],
                                     "transform": {"type": "type"}})),
     0, "completed",
     {"r": ["number", "string", "null", "array", "object"]}, ["r"], [], None, None)
case("map_op_form", wf(st("r", "map", {
    "input": [1, 2],
    "transform": {"type": "op", "op": "add",
                  "args": {"a": {"$self": True}, "b": 2}}})),
     0, "completed", {"r": [3, 4]}, ["r"], [], None, None)
case("filter_even", wf(st("r", "filter", {"input": [1, 2, 3, 4],
                                           "predicate": {"type": "even"}})),
     0, "completed", {"r": [2, 4]}, ["r"], [], None, None)
case("filter_odd", wf(st("r", "filter", {"input": [1, 2, 3, 4],
                                          "predicate": {"type": "odd"}})),
     0, "completed", {"r": [1, 3]}, ["r"], [], None, None)
case("filter_gt", wf(st("r", "filter", {"input": [1, 5, 9],
                                         "predicate": {"type": "gt", "value": 4}})),
     0, "completed", {"r": [5, 9]}, ["r"], [], None, None)
case("filter_truthy", wf(st("r", "filter", {
    "input": [0, 1, "", "x", None, [], [1], {}, {"a": 1}, False, True],
    "predicate": {"type": "truthy"}})),
     0, "completed", {"r": [1, "x", [1], {"a": 1}, True]}, ["r"], [], None, None)
case("filter_type_is", wf(st("r", "filter", {
    "input": [1, "a", None],
    "predicate": {"type": "type_is", "value": "string"}})),
     0, "completed", {"r": ["a"]}, ["r"], [], None, None)
case("filter_contains", wf(st("r", "filter", {
    "input": ["apple", "banana", "cherry"],
    "predicate": {"type": "string_contains", "value": "an"}})),
     0, "completed", {"r": ["banana"]}, ["r"], [], None, None)
case("reduce_add", wf(st("r", "reduce", {"input": [1, 2, 3], "op": "add"})),
     0, "completed", {"r": 6}, ["r"], [], None, None)
case("reduce_sub_identity", wf(st("r", "reduce", {"input": [1, 2, 3], "op": "subtract"})),
     0, "completed", {"r": -6}, ["r"], [], None, None)
case("reduce_mul_initial", wf(st("r", "reduce", {
    "input": [2, 3], "op": "multiply", "initial": 10})),
     0, "completed", {"r": 60}, ["r"], [], None, None)
case("reduce_empty", wf(st("r", "reduce", {"input": [], "op": "multiply"})),
     0, "completed", {"r": 1}, ["r"], [], None, None)
case("sort_mixed", wf(st("r", "sort", {"input": [
    "b", 3, None, True, [1], {"a": 1}, "a", 1, False, [0]]})),
     0, "completed",
     {"r": [1, 3, "a", "b", False, True, None, [0], [1], {"a": 1}]},
     ["r"], [], None, None)
case("reverse", wf(st("r", "reverse", {"input": [1, 2, 3]})), 0, "completed",
     {"r": [3, 2, 1]}, ["r"], [], None, None)
case("unique", wf(st("r", "unique", {"input": [1, 2, 1, {"a": 1}, {"a": 1}, [1], [1]]})),
     0, "completed", {"r": [1, 2, {"a": 1}, [1]]}, ["r"], [], None, None)
case("slice", wf(st("r", "slice", {"input": [0, 1, 2, 3, 4], "start": 1, "end": 4})),
     0, "completed", {"r": [1, 2, 3]}, ["r"], [], None, None)
case("slice_negative", wf(st("r", "slice", {"input": [0, 1, 2, 3, 4], "start": -2})),
     0, "completed", {"r": [3, 4]}, ["r"], [], None, None)
case("slice_clamped", wf(st("r", "slice", {
    "input": [0, 1, 2], "start": -100, "end": 100})),
     0, "completed", {"r": [0, 1, 2]}, ["r"], [], None, None)
case("slice_start_gt_end", wf(st("r", "slice", {
    "input": [0, 1, 2], "start": 2, "end": 1})),
     0, "completed", {"r": []}, ["r"], [], None, None)
case("flatten", wf(st("r", "flatten", {"input": [[1, 2], 3, [], [[4]]]})),
     0, "completed", {"r": [1, 2, 3, [4]]}, ["r"], [], None, None)

# ------------------------------------------------------------- objects
case("get_present", wf(st("r", "get", {"input": {"a": 1}, "key": "a"})),
     0, "completed", {"r": 1}, ["r"], [], None, None)
case("get_missing", wf(st("r", "get", {"input": {"a": 1}, "key": "b"})),
     0, "completed", {"r": None}, ["r"], [], None, None)
case("set", wf(st("r", "set", {"input": {"a": 1}, "key": "b", "value": 2})),
     0, "completed", {"r": {"a": 1, "b": 2}}, ["r"], [], None, None)
case("keys", wf(st("r", "keys", {"input": {"b": 1, "a": 2, "C": 3}})),
     0, "completed", {"r": ["C", "a", "b"]}, ["r"], [], None, None)
case("values", wf(st("r", "values", {"input": {"b": 1, "a": 2, "C": 3}})),
     0, "completed", {"r": [3, 2, 1]}, ["r"], [], None, None)
case("merge", wf(st("r", "merge", {"a": {"x": 1, "y": 2}, "b": {"y": 9, "z": 3}})),
     0, "completed", {"r": {"x": 1, "y": 9, "z": 3}}, ["r"], [], None, None)
case("pick", wf(st("r", "pick", {"input": {"a": 1, "b": 2}, "keys": ["b", "zz"]})),
     0, "completed", {"r": {"b": 2, "zz": None}}, ["r"], [], None, None)
case("omit", wf(st("r", "omit", {"input": {"a": 1, "b": 2}, "keys": ["a"]})),
     0, "completed", {"r": {"b": 2}}, ["r"], [], None, None)

# ----------------------------------------------------------- utilities
case("constant", wf(st("r", "constant", {"value": {"a": [1, None]}})),
     0, "completed", {"r": {"a": [1, None]}}, ["r"], [], None, None)
case("equals_numbers", wf(st("r", "equals", {"a": 1, "b": 1.0})),
     0, "completed", {"r": True}, ["r"], [], None, None)
case("equals_objects_order", wf(st("r", "equals", {
    "a": {"x": 1, "y": 2}, "b": {"y": 2, "x": 1}})),
     0, "completed", {"r": True}, ["r"], [], None, None)
case("compare_mixed", wf(st("r", "compare", {
    "a": 99, "b": "a", "op": "lt"})),
     0, "completed", {"r": True}, ["r"], [], None, None)
case("and_false", wf(st("r", "and", {"a": True, "b": 0})), 0, "completed",
     {"r": False}, ["r"], [], None, None)
case("or_true", wf(st("r", "or", {"a": "", "b": [1]})), 0, "completed",
     {"r": True}, ["r"], [], None, None)
case("not", wf(st("r", "not", {"a": None})), 0, "completed",
     {"r": True}, ["r"], [], None, None)
case("type_number_float", wf(st("r", "type", {"value": 1.5})), 0, "completed",
     {"r": "number"}, ["r"], [], None, None)
case("fail_until_ok", wf(st("r", "fail_until", {"failures": 0, "value": 7})),
     0, "completed", {"r": 7}, ["r"], [], None, None)
case("fail_until_exhausted", wf(st("r", "fail_until", {"failures": 5, "value": 7},
                                    retry={"max_attempts": 3})),
     1, "failed", {}, [], [], "STEP_FAILED", "r")
case("fail_until_default_retry", wf(st("r", "fail_until", {"failures": 1, "value": 7})),
     1, "failed", {}, [], [], "STEP_FAILED", "r")

# ------------------------------------------------- dependencies and refs
case("order_tiebreak", wf(
    st("a", "constant", {"value": 1}),
    st("b", "constant", {"value": 2}, depends_on=["a"]),
    st("c", "constant", {"value": 3})),
     0, "completed", {"a": 1, "b": 2, "c": 3}, ["a", "b", "c"], [], None, None)
case("nested_ref", wf(
    st("a", "constant", {"value": {"user": {"name": "ann"}, "items": [10, 20]}}),
    st("b", "constant", {"value": None}, depends_on=["a"],
       ),
    st("c", "concat", {"a": {"$ref": "a.user.name"}, "b": "!"},
       depends_on=["b"]),
    st("d", "add", {"a": {"$ref": "a.items.1"}, "b": 1}, depends_on=["c"])),
     0, "completed",
     {"a": {"user": {"name": "ann"}, "items": [10, 20]}, "b": None,
      "c": "ann!", "d": 21},
     ["a", "b", "c", "d"], [], None, None)
case("ref_not_dependency", wf(
    st("a", "constant", {"value": 1}),
    st("b", "add", {"a": {"$ref": "a"}, "b": 1})),
     1, "failed", {"a": 1}, ["a"], [], "INVALID_REFERENCE", "b")
case("ref_missing_key", wf(
    st("a", "constant", {"value": {"x": 1}}),
    st("b", "constant", {"value": {"$ref": "a.y"}}, depends_on=["a"])),
     1, "failed", {"a": {"x": 1}}, ["a"], [], "INVALID_REFERENCE", "b")
case("ref_bad_index", wf(
    st("a", "constant", {"value": [1, 2]}),
    st("b", "constant", {"value": {"$ref": "a.x"}}, depends_on=["a"])),
     1, "failed", {"a": [1, 2]}, ["a"], [], "INVALID_REFERENCE", "b")
case("ref_negative_index", wf(
    st("a", "constant", {"value": [1, 2]}),
    st("b", "constant", {"value": {"$ref": "a.-1"}}, depends_on=["a"])),
     1, "failed", {"a": [1, 2]}, ["a"], [], "INVALID_REFERENCE", "b")
case("ref_self", wf(st("a", "constant", {"value": {"$ref": "a"}})),
     1, "failed", {}, [], [], "INVALID_REFERENCE", "a")
case("ref_skipped", wf(
    st("a", "constant", {"value": 1}),
    st("b", "constant", {"value": 2}, depends_on=["a"],
       when={"op": "gt", "left": {"$ref": "a"}, "right": 5}),
    st("c", "add", {"a": {"$ref": "b"}, "b": 1}, depends_on=["b"])),
     1, "failed", {"a": 1, "b": None}, ["a", "b"], ["b"],
     "SKIPPED_REFERENCE", "c")
case("ref_null_not_skipped", wf(
    st("a", "constant", {"value": None}),
    st("b", "type", {"value": {"$ref": "a"}}, depends_on=["a"])),
     0, "completed", {"a": None, "b": "null"}, ["a", "b"], [], None, None)

# ------------------------------------------------------------ conditions
case("when_true", wf(
    st("a", "constant", {"value": 5}),
    st("b", "constant", {"value": "yes"}, depends_on=["a"],
       when={"op": "gte", "left": {"$ref": "a"}, "right": 5})),
     0, "completed", {"a": 5, "b": "yes"}, ["a", "b"], [], None, None)
case("when_all", wf(
    st("a", "constant", {"value": 5}),
    st("b", "constant", {"value": "yes"}, depends_on=["a"],
       when={"op": "all", "conditions": [
           {"op": "gt", "left": {"$ref": "a"}, "right": 1},
           {"op": "lt", "left": {"$ref": "a"}, "right": 10}]})),
     0, "completed", {"a": 5, "b": "yes"}, ["a", "b"], [], None, None)
case("when_any_false", wf(
    st("a", "constant", {"value": 5}),
    st("b", "constant", {"value": "yes"}, depends_on=["a"],
       when={"op": "any", "conditions": [
           {"op": "gt", "left": {"$ref": "a"}, "right": 10},
           {"op": "lt", "left": {"$ref": "a"}, "right": 1}]})),
     0, "completed", {"a": 5, "b": None}, ["a", "b"], ["b"], None, None)
case("when_not", wf(
    st("a", "constant", {"value": 5}),
    st("b", "constant", {"value": "yes"}, depends_on=["a"],
       when={"op": "not", "condition":
             {"op": "eq", "left": {"$ref": "a"}, "right": 5}})),
     0, "completed", {"a": 5, "b": None}, ["a", "b"], ["b"], None, None)
case("when_type_order", wf(
    st("a", "constant", {"value": "s"}),
    st("b", "constant", {"value": 1}, depends_on=["a"],
       when={"op": "gt", "left": {"$ref": "a"}, "right": 10})),
     0, "completed", {"a": "s", "b": 1}, ["a", "b"], [], None, None)
case("when_skipped_ref", wf(
    st("a", "constant", {"value": 1}),
    st("b", "constant", {"value": 2}, depends_on=["a"],
       when={"op": "gt", "left": {"$ref": "a"}, "right": 5}),
    st("c", "constant", {"value": 3}, depends_on=["b"],
       when={"op": "eq", "left": {"$ref": "b"}, "right": None})),
     1, "failed", {"a": 1, "b": None}, ["a", "b"], ["b"],
     "SKIPPED_REFERENCE", "c")

# -------------------------------------------------- validation and errors
case("empty_workflow", wf(), 0, "completed", {}, [], [], None, None)
case("version_float", wf(st("a", "constant", {"value": 1}), version=1.0),
     2, "failed", {}, [], [], "INVALID_WORKFLOW", None)
case("version_two", wf(st("a", "constant", {"value": 1}), version=2),
     2, "failed", {}, [], [], "INVALID_WORKFLOW", None)
case("unknown_top_field", {"version": 1, "steps": [], "name": "x"},
     2, "failed", {}, [], [], "INVALID_WORKFLOW", None)
case("missing_steps", {"version": 1},
     2, "failed", {}, [], [], "INVALID_WORKFLOW", None)
case("bad_id", wf(st("1bad", "constant", {"value": 1})),
     2, "failed", {}, [], [], "INVALID_WORKFLOW", "1bad")
case("duplicate_id", wf(
    st("a", "constant", {"value": 1}),
    st("a", "constant", {"value": 2})),
     2, "failed", {}, [], [], "INVALID_WORKFLOW", "a")
case("unknown_op", wf(st("a", "explode", {})),
     2, "failed", {}, [], [], "INVALID_WORKFLOW", "a")
case("unknown_step_field", wf({**st("a", "constant", {"value": 1}), "foo": 1}),
     2, "failed", {}, [], [], "INVALID_WORKFLOW", "a")
case("unknown_dep", wf(st("a", "constant", {"value": 1}, depends_on=["zzz"])),
     2, "failed", {}, [], [], "UNKNOWN_DEPENDENCY", "a")
case("cycle", wf(
    st("a", "constant", {"value": 1}, depends_on=["b"]),
    st("b", "constant", {"value": 2}, depends_on=["a"])),
     2, "failed", {}, [], [], "CYCLIC_DEPENDENCY", None)
case("self_cycle", wf(st("a", "constant", {"value": 1}, depends_on=["a"])),
     2, "failed", {}, [], [], "CYCLIC_DEPENDENCY", None)
case("bad_retry", wf(st("a", "constant", {"value": 1}, retry={"max_attempts": 6})),
     2, "failed", {}, [], [], "INVALID_WORKFLOW", "a")
case("bad_retry_float", wf(st("a", "constant", {"value": 1}, retry={"max_attempts": 2.0})),
     2, "failed", {}, [], [], "INVALID_WORKFLOW", "a")
case("bad_when_shape", wf(st("a", "constant", {"value": 1},
                             when={"op": "gt", "left": 1})),
     2, "failed", {}, [], [], "INVALID_WORKFLOW", "a")
case("bad_args_type", wf({"id": "a", "op": "constant", "args": 5}),
     2, "failed", {}, [], [], "INVALID_WORKFLOW", "a")
case("partial_failure", wf(
    st("a", "constant", {"value": 1}),
    st("b", "initialize_unknown", {}),
    st("c", "constant", {"value": 3}, depends_on=["a"])),
     2, "failed", {}, [], [], "INVALID_WORKFLOW", "b")
case("runtime_partial", wf(
    st("a", "constant", {"value": 1}),
    st("b", "divide", {"a": 1, "b": 0}, depends_on=["a"]),
    st("c", "constant", {"value": 3}, depends_on=["a"])),
     1, "failed", {"a": 1}, ["a"], [], "DIVISION_BY_ZERO", "b")
case("dependency_partial", wf(
    st("a", "constant", {"value": 1}),
    st("b", "divide", {"a": 1, "b": 0}, depends_on=["a"]),
    st("c", "constant", {"value": 3}, depends_on=["b"])),
     1, "failed", {"a": 1}, ["a"], [], "DIVISION_BY_ZERO", "b")
case("skip_then_decide", wf(
    st("a", "constant", {"value": 1}),
    st("b", "constant", {"value": 2}, depends_on=["a"],
       when={"op": "gt", "left": {"$ref": "a"}, "right": 0}),
    st("c", "add", {"a": {"$ref": "b"}, "b": 3}, depends_on=["b"])),
     0, "completed", {"a": 1, "b": 2, "c": 5}, ["a", "b", "c"], [], None, None)


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_BIN
    binary = os.path.abspath(binary)
    if not os.path.exists(binary):
        print("binary not found:", binary)
        return 2
    failures = 0
    for c in CASES:
        proc = subprocess.run([binary, "run", "-"], input=json.dumps(c["doc"]),
                              capture_output=True, text=True)
        try:
            out = json.loads(proc.stdout)
        except Exception:
            print(f"FAIL {c['name']}: stdout is not JSON: {proc.stdout!r}")
            failures += 1
            continue
        problems = []
        if proc.returncode != c["exit_code"]:
            problems.append(f"exit {proc.returncode} != {c['exit_code']}")
        if out.get("status") != c["status"]:
            problems.append(f"status {out.get('status')!r} != {c['status']!r}")
        if c["results"] is not None and out.get("results") != c["results"]:
            problems.append(f"results {out.get('results')!r} != {c['results']!r}")
        if c["order"] is not None and out.get("execution_order") != c["order"]:
            problems.append(f"order {out.get('execution_order')!r} != {c['order']!r}")
        if c["skipped"] is not None and out.get("skipped") != c["skipped"]:
            problems.append(f"skipped {out.get('skipped')!r} != {c['skipped']!r}")
        err = out.get("error")
        if c["code"] is None:
            if err is not None:
                problems.append(f"expected error null, got {err!r}")
        else:
            if not isinstance(err, dict):
                problems.append(f"expected error code {c['code']}, got {err!r}")
            else:
                if err.get("code") != c["code"]:
                    problems.append(
                        f"code {err.get('code')!r} != {c['code']!r}")
                if c["step"] != "__unset__" and err.get("step") != c["step"]:
                    problems.append(f"step {err.get('step')!r} != {c['step']!r}")
        if problems:
            failures += 1
            print(f"FAIL {c['name']}: " + "; ".join(problems))
            print("  stderr:", proc.stderr.strip()[:300])
    total = len(CASES)
    if failures:
        print(f"\n{failures}/{total} tests failed")
        return 1
    print(f"all {total} tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
