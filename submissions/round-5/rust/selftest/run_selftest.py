#!/usr/bin/env python3
"""Self-test suite for the Rust workflow engine.

Each case is a workflow document plus the exact response expected by SPEC.md.
Run from anywhere:  python3 selftest/run_selftest.py [path-to-binary]
"""
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "target", "release", "workflow")


def wf(*steps):
    return {"version": 1, "steps": list(steps)}


def step(id, op, args=None, depends_on=None, when=None, retry=None):
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


CASES = []


def case(name, doc, exit_code, results, order, skipped, error=None):
    CASES.append(
        {
            "name": name,
            "doc": doc,
            "exit": exit_code,
            "results": results,
            "order": order,
            "skipped": skipped,
            "error": error,  # None or (code, step)
        }
    )


# ---------------------------------------------------------------- math
case(
    "math_pipeline",
    wf(
        step("a", "add", {"a": 10, "b": 20}),
        step("s", "subtract", {"a": {"$ref": "a"}, "b": 5}, ["a"]),
        step("m", "multiply", {"a": {"$ref": "s"}, "b": 2}, ["s"]),
        step("d", "divide", {"a": {"$ref": "m"}, "b": 7}, ["m"]),
        step("r", "mod", {"a": {"$ref": "m"}, "b": 7}, ["m"]),
    ),
    0,
    {"a": 30, "s": 25, "m": 50, "d": 7, "r": 1},
    ["a", "s", "m", "d", "r"],
    [],
)

case(
    "integer_signs",
    wf(
        step("d1", "divide", {"a": -7, "b": 2}),
        step("d2", "divide", {"a": -7, "b": -2}),
        step("d3", "divide", {"a": 7, "b": 2}),
        step("m1", "mod", {"a": -7, "b": 2}),
        step("m2", "mod", {"a": 7, "b": -2}),
        step("m3", "mod", {"a": -7, "b": -2}),
    ),
    0,
    {"d1": -3, "d2": 3, "d3": 3, "m1": -1, "m2": 1, "m3": -1},
    ["d1", "d2", "d3", "m1", "m2", "m3"],
    [],
)

case(
    "sum_min_max_range",
    wf(
        step("sum", "sum", {"input": [1, -2, 3]}),
        step("sum0", "sum", {"input": []}),
        step("min", "min", {"input": [3, -5, 2]}),
        step("max", "max", {"input": [3, -5, 2]}),
        step("rng", "range", {"start": 2, "end": 5}),
        step("rng0", "range", {"start": 5, "end": 2}),
        step("rng1", "range", {"start": -1, "end": 0}),
    ),
    0,
    {
        "sum": 2,
        "sum0": 0,
        "min": -5,
        "max": 3,
        "rng": [2, 3, 4, 5],
        "rng0": [],
        "rng1": [-1, 0],
    },
    ["sum", "sum0", "min", "max", "rng", "rng0", "rng1"],
    [],
)

case(
    "overflow_add",
    wf(step("o", "add", {"a": 9223372036854775807, "b": 1})),
    1,
    {},
    [],
    [],
    ("ARITHMETIC_OVERFLOW", "o"),
)
case(
    "overflow_sub",
    wf(step("o", "subtract", {"a": -9223372036854775808, "b": 1})),
    1,
    {},
    [],
    [],
    ("ARITHMETIC_OVERFLOW", "o"),
)
case(
    "overflow_mul",
    wf(step("o", "multiply", {"a": 3037000500, "b": 3037000500})),
    1,
    {},
    [],
    [],
    ("ARITHMETIC_OVERFLOW", "o"),
)
case(
    "overflow_div_min",
    wf(step("o", "divide", {"a": -9223372036854775808, "b": -1})),
    1,
    {},
    [],
    [],
    ("ARITHMETIC_OVERFLOW", "o"),
)
case(
    "overflow_mod_min",
    wf(step("o", "mod", {"a": -9223372036854775808, "b": -1})),
    1,
    {},
    [],
    [],
    ("ARITHMETIC_OVERFLOW", "o"),
)
case(
    "div_zero",
    wf(step("o", "divide", {"a": 1, "b": 0})),
    1,
    {},
    [],
    [],
    ("DIVISION_BY_ZERO", "o"),
)
case(
    "mod_zero",
    wf(step("o", "mod", {"a": 1, "b": 0})),
    1,
    {},
    [],
    [],
    ("DIVISION_BY_ZERO", "o"),
)
case(
    "empty_min",
    wf(step("o", "min", {"input": []})),
    1,
    {},
    [],
    [],
    ("EMPTY_ARRAY", "o"),
)

# ---------------------------------------------------------------- strings
case(
    "strings",
    wf(
        step("cat", "concat", {"a": "foo", "b": "bar"}),
        step("split", "split", {"input": "a,b,", "sep": ","}),
        step("chars", "split", {"input": "a\u00e9\U0001f642", "sep": ""}),
        step("split_empty", "split", {"input": "", "sep": ","}),
        step("join", "join", {"input": ["a", "b", "c"], "sep": "-"}),
        step("join0", "join", {"input": [], "sep": "-"}),
        step("up", "uppercase", {"input": "Ab\u00c7z!"}),
        step("low", "lowercase", {"input": "Ab\u00c7Z!"}),
        step("trim", "trim", {"input": " \t\n\r\f\u000b hi \u000b"}),
        step("rep", "replace", {"input": "aaa", "search": "aa", "replace": "X"}),
        step("len", "length", {"input": "a\u00e9\U0001f642"}),
        step("len0", "length", {"input": ""}),
    ),
    0,
    {
        "cat": "foobar",
        "split": ["a", "b", ""],
        "chars": ["a", "\u00e9", "\U0001f642"],
        "split_empty": [""],
        "join": "a-b-c",
        "join0": "",
        "up": "AB\u00c7Z!",
        "low": "ab\u00c7z!",
        "trim": "hi",
        "rep": "Xa",
        "len": 3,
        "len0": 0,
    },
    ["cat", "split", "chars", "split_empty", "join", "join0", "up", "low", "trim", "rep", "len", "len0"],
    [],
)

# ---------------------------------------------------------------- arrays
case(
    "array_ops",
    wf(
        step("m", "map", {"input": [1, 2, 3], "transform": {"type": "add", "value": 10}}),
        step("neg", "map", {"input": [1, -2], "transform": {"type": "negate"}}),
        step("abs", "map", {"input": [-1, -5, 3], "transform": {"type": "abs"}}),
        step(
            "str",
            "map",
            {
                "input": [1, "a", True, None, [1], {"a": 1}],
                "transform": {"type": "stringify"},
            },
        ),
        step("typ", "map", {"input": [1, "a", None, [], {}], "transform": {"type": "type"}}),
        step(
            "const",
            "map",
            {"input": [1, 2], "transform": {"type": "constant", "value": {"k": 1}}},
        ),
        step(
            "opform",
            "map",
            {
                "input": [1, 2],
                "transform": {
                    "type": "op",
                    "op": "add",
                    "args": {"a": {"$self": True}, "b": 2},
                },
            },
        ),
        step("sort", "sort", {"input": [3, "b", True, None, False, "a", [2], [1], {"b": 1}, {"a": 2}, 1]}),
        step("sort_uni", "sort", {"input": ["\u00e9", "z", "A"]}),
        step("rev", "reverse", {"input": [1, 2, 3]}),
        step(
            "uniq",
            "unique",
            {"input": [1, 2, 1, 1.0, "1", {"a": 1, "b": 2}, {"b": 2, "a": 1}, [1], [1]]},
        ),
        step("slice1", "slice", {"input": [0, 1, 2, 3, 4], "start": -2}),
        step("slice2", "slice", {"input": [0, 1, 2, 3, 4], "start": 1, "end": -1}),
        step("slice3", "slice", {"input": [0, 1, 2, 3, 4], "start": 4, "end": 2}),
        step("slice4", "slice", {"input": [0, 1, 2, 3, 4], "start": -100, "end": 100}),
        step("flat", "flatten", {"input": [1, [2, 3], [], [[4]], "x"]}),
    ),
    0,
    {
        "m": [11, 12, 13],
        "neg": [-1, 2],
        "abs": [1, 5, 3],
        "str": ["1", '"a"', "true", "null", "[1]", '{"a":1}'],
        "typ": ["number", "string", "null", "array", "object"],
        "const": [{"k": 1}, {"k": 1}],
        "opform": [3, 4],
        "sort": [1, 3, "a", "b", False, True, None, [1], [2], {"a": 2}, {"b": 1}],
        "sort_uni": ["A", "z", "\u00e9"],
        "rev": [3, 2, 1],
        "uniq": [1, 2, "1", {"a": 1, "b": 2}, [1]],
        "slice1": [3, 4],
        "slice2": [1, 2, 3],
        "slice3": [],
        "slice4": [0, 1, 2, 3, 4],
        "flat": [1, 2, 3, [4], "x"],
    },
    [
        "m",
        "neg",
        "abs",
        "str",
        "typ",
        "const",
        "opform",
        "sort",
        "sort_uni",
        "rev",
        "uniq",
        "slice1",
        "slice2",
        "slice3",
        "slice4",
        "flat",
    ],
    [],
)

case(
    "filter_ops",
    wf(
        step("even", "filter", {"input": [1, 2, 3, 4], "predicate": {"type": "even"}}),
        step("odd", "filter", {"input": [1, 2, 3, 4], "predicate": {"type": "odd"}}),
        step("gt", "filter", {"input": [1, 2, 3, 4], "predicate": {"type": "gt", "value": 2}}),
        step("lte", "filter", {"input": [1, 2, 3, 4], "predicate": {"type": "lte", "value": 2}}),
        step(
            "truthy",
            "filter",
            {
                "input": [0, 1, "", "x", None, [], [1], {}, {"a": 1}, False, True],
                "predicate": {"type": "truthy"},
            },
        ),
        step(
            "falsy",
            "filter",
            {"input": [0, 1, "", "x", None, [], [1], {}, False, True], "predicate": {"type": "falsy"}},
        ),
        step(
            "type_is",
            "filter",
            {"input": [1, "a", None, [], {}], "predicate": {"type": "type_is", "value": "number"}},
        ),
        step(
            "type_is_compact",
            "filter",
            {"input": [1, "a", None, [], {}], "predicate": {"type": "number"}},
        ),
        step(
            "contains",
            "filter",
            {"input": ["apple", "banana", "cherry"], "predicate": {"type": "string_contains", "value": "an"}},
        ),
        step(
            "eq",
            "filter",
            {"input": [1, 2, 1.0, "1"], "predicate": {"type": "eq", "value": 1}},
        ),
    ),
    0,
    {
        "even": [2, 4],
        "odd": [1, 3],
        "gt": [3, 4],
        "lte": [1, 2],
        "truthy": [1, "x", [1], {"a": 1}, True],
        "falsy": [0, "", None, [], {}, False],
        "type_is": [1],
        "type_is_compact": [1],
        "contains": ["banana"],
        "eq": [1, 1.0],
    },
    ["even", "odd", "gt", "lte", "truthy", "falsy", "type_is", "type_is_compact", "contains", "eq"],
    [],
)

case(
    "reduce_ops",
    wf(
        step("add0", "reduce", {"input": [], "op": "add"}),
        step("sub", "reduce", {"input": [1, 2, 3], "op": "subtract", "initial": 10}),
        step("mul0", "reduce", {"input": [], "op": "multiply"}),
        step("mul", "reduce", {"input": [2, 3], "op": "multiply"}),
        step("mul_init", "reduce", {"input": [3, 4], "op": "multiply", "initial": 2}),
    ),
    0,
    {"add0": 0, "sub": 4, "mul0": 1, "mul": 6, "mul_init": 24},
    ["add0", "sub", "mul0", "mul", "mul_init"],
    [],
)

# ---------------------------------------------------------------- objects
OBJ = {"b": 2, "a": 1, "c": None}
case(
    "object_ops",
    wf(
        step("obj", "constant", {"value": OBJ}),
        step("get", "get", {"input": {"$ref": "obj"}, "key": "b"}, ["obj"]),
        step("getmiss", "get", {"input": {"$ref": "obj"}, "key": "zz"}, ["obj"]),
        step(
            "set",
            "set",
            {"input": {"$ref": "obj"}, "key": "z", "value": 5},
            ["obj"],
        ),
        step("keys", "keys", {"input": {"$ref": "obj"}}, ["obj"]),
        step("values", "values", {"input": {"$ref": "obj"}}, ["obj"]),
        step(
            "merge",
            "merge",
            {"a": {"x": 1, "y": 2}, "b": {"y": 20, "z": 30}},
        ),
        step(
            "pick",
            "pick",
            {"input": {"$ref": "obj"}, "keys": ["a", "zz"]},
            ["obj"],
        ),
        step("omit", "omit", {"input": {"$ref": "obj"}, "keys": ["b"]}, ["obj"]),
        step("keys_uni", "keys", {"input": {"\u00e9": 1, "a": 2, "Z": 3}}),
    ),
    0,
    {
        "obj": {"a": 1, "b": 2, "c": None},
        "get": 2,
        "getmiss": None,
        "set": {"a": 1, "b": 2, "c": None, "z": 5},
        "keys": ["a", "b", "c"],
        "values": [1, 2, None],
        "merge": {"x": 1, "y": 20, "z": 30},
        "pick": {"a": 1, "zz": None},
        "omit": {"a": 1, "c": None},
        "keys_uni": ["Z", "a", "\u00e9"],
    },
    ["obj", "get", "getmiss", "set", "keys", "values", "merge", "pick", "omit", "keys_uni"],
    [],
)

# ---------------------------------------------------------------- utilities
case(
    "utilities",
    wf(
        step("eq_num", "equals", {"a": 1, "b": 1.0}),
        step("eq_obj", "equals", {"a": {"x": 1, "y": [1, 2]}, "b": {"y": [1, 2], "x": 1}}),
        step("ne_obj", "equals", {"a": {"x": 1}, "b": {"x": 2}}),
        step("eq_types", "equals", {"a": 1, "b": "1"}),
        step("cmp_mixed", "compare", {"a": 1, "b": "a", "op": "lt"}),
        step("cmp_str", "compare", {"a": "a", "b": "b", "op": "lt"}),
        step("cmp_num", "compare", {"a": 5, "b": 5, "op": "gte"}),
        step("and1", "and", {"a": True, "b": False}),
        step("and2", "and", {"a": 1, "b": "x"}),
        step("or1", "or", {"a": False, "b": "x"}),
        step("or2", "or", {"a": 0, "b": None}),
        step("not1", "not", {"a": 0}),
        step("type_null", "type", {"value": None}),
        step("type_arr", "type", {"value": []}),
    ),
    0,
    {
        "eq_num": True,
        "eq_obj": True,
        "ne_obj": False,
        "eq_types": False,
        "cmp_mixed": True,
        "cmp_str": True,
        "cmp_num": True,
        "and1": False,
        "and2": True,
        "or1": True,
        "or2": False,
        "not1": True,
        "type_null": "null",
        "type_arr": "array",
    },
    [
        "eq_num",
        "eq_obj",
        "ne_obj",
        "eq_types",
        "cmp_mixed",
        "cmp_str",
        "cmp_num",
        "and1",
        "and2",
        "or1",
        "or2",
        "not1",
        "type_null",
        "type_arr",
    ],
    [],
)

# ---------------------------------------------------------------- conditions
case(
    "conditions",
    wf(
        step("x", "constant", {"value": 5}),
        step(
            "all",
            "constant",
            {"value": "ran"},
            ["x"],
            {"op": "all", "conditions": [
                {"op": "gt", "left": {"$ref": "x"}, "right": 0},
                {"op": "lt", "left": {"$ref": "x"}, "right": 10},
            ]},
        ),
        step(
            "any",
            "constant",
            {"value": "ran"},
            ["x"],
            {"op": "any", "conditions": [
                {"op": "gt", "left": {"$ref": "x"}, "right": 100},
                {"op": "eq", "left": {"$ref": "x"}, "right": 5},
            ]},
        ),
        step(
            "not",
            "constant",
            {"value": "no"},
            ["x"],
            {"op": "not", "condition": {"op": "eq", "left": {"$ref": "x"}, "right": 5}},
        ),
        step(
            "ne",
            "constant",
            {"value": "ran"},
            ["x"],
            {"op": "ne", "left": {"$ref": "x"}, "right": 6},
        ),
        step(
            "lit",
            "constant",
            {"value": "ran"},
            ["x"],
            {"op": "gte", "left": 5, "right": 5},
        ),
    ),
    0,
    {"x": 5, "all": "ran", "any": "ran", "not": None, "ne": "ran", "lit": "ran"},
    ["x", "all", "any", "not", "ne", "lit"],
    ["not"],
)

case(
    "condition_bool_ref",
    wf(
        step("flag", "constant", {"value": False}),
        step(
            "y",
            "constant",
            {"value": 1},
            ["flag"],
            {"$ref": "flag"},
        ),
    ),
    0,
    {"flag": False, "y": None},
    ["flag", "y"],
    ["y"],
)

# ---------------------------------------------------------------- references
case(
    "nested_refs",
    wf(
        step("x", "constant", {"value": {"items": [{"name": "a"}, {"name": "b"}], "n": {"k": 1}}}),
        step("y", "constant", {"value": {"$ref": "x.items.1.name"}}, ["x"]),
        step("z", "constant", {"value": {"$ref": "x.n.k"}}, ["x"]),
        step("null_step", "constant", {"value": None}),
        step("null_ref", "constant", {"value": {"$ref": "null_step"}}, ["null_step"]),
    ),
    0,
    {"x": {"items": [{"name": "a"}, {"name": "b"}], "n": {"k": 1}},
     "y": "b", "z": 1, "null_step": None, "null_ref": None},
    ["x", "y", "z", "null_step", "null_ref"],
    [],
)

case(
    "skipped_ref",
    wf(
        step("x", "constant", {"value": 1}, when={"op": "gt", "left": 1, "right": 10}),
        step("y", "constant", {"value": {"$ref": "x"}}, ["x"]),
    ),
    1,
    {"x": None},
    ["x"],
    ["x"],
    ("SKIPPED_REFERENCE", "y"),
)

case(
    "ref_missing_key",
    wf(
        step("x", "constant", {"value": {"items": [1, 2]}}),
        step("y", "constant", {"value": {"$ref": "x.items.z"}}, ["x"]),
    ),
    1,
    {"x": {"items": [1, 2]}},
    ["x"],
    [],
    ("INVALID_REFERENCE", "y"),
)
case(
    "ref_negative_index",
    wf(
        step("x", "constant", {"value": {"items": [1, 2]}}),
        step("y", "constant", {"value": {"$ref": "x.items.-1"}}, ["x"]),
    ),
    1,
    {"x": {"items": [1, 2]}},
    ["x"],
    [],
    ("INVALID_REFERENCE", "y"),
)
case(
    "ref_out_of_range",
    wf(
        step("x", "constant", {"value": {"items": [1, 2]}}),
        step("y", "constant", {"value": {"$ref": "x.items.5"}}, ["x"]),
    ),
    1,
    {"x": {"items": [1, 2]}},
    ["x"],
    [],
    ("INVALID_REFERENCE", "y"),
)
case(
    "ref_not_dependency",
    wf(
        step("x", "constant", {"value": 1}),
        step("y", "constant", {"value": {"$ref": "x"}}),
    ),
    1,
    {"x": 1},
    ["x"],
    [],
    ("INVALID_REFERENCE", "y"),
)
case(
    "ref_unknown_step",
    wf(step("y", "constant", {"value": {"$ref": "nope"}})),
    1,
    {},
    [],
    [],
    ("INVALID_REFERENCE", "y"),
)
case(
    "ref_transitive",
    wf(
        step("a", "constant", {"value": {"k": 7}}),
        step("b", "constant", {"value": {"$ref": "a.k"}}, ["a"]),
        step("c", "constant", {"value": {"$ref": "a.k"}}, ["b"]),
    ),
    0,
    {"a": {"k": 7}, "b": 7, "c": 7},
    ["a", "b", "c"],
    [],
)

# ---------------------------------------------------------------- DAG
case(
    "tie_break_order",
    wf(
        step("c", "constant", {"value": 3}, ["b"]),
        step("b", "constant", {"value": 2}, ["a"]),
        step("a", "constant", {"value": 1}),
        step("d", "constant", {"value": 4}),
    ),
    0,
    {"a": 1, "b": 2, "c": 3, "d": 4},
    ["a", "b", "c", "d"],
    [],
)
case(
    "cycle",
    wf(
        step("a", "constant", {"value": 1}, ["b"]),
        step("b", "constant", {"value": 2}, ["a"]),
    ),
    2,
    {},
    [],
    [],
    ("CYCLIC_DEPENDENCY", None),
)
case(
    "self_cycle",
    wf(step("a", "constant", {"value": 1}, ["a"])),
    2,
    {},
    [],
    [],
    ("CYCLIC_DEPENDENCY", None),
)
case(
    "unknown_dependency",
    wf(step("a", "constant", {"value": 1}, ["nope"])),
    2,
    {},
    [],
    [],
    ("UNKNOWN_DEPENDENCY", "a"),
)

# ---------------------------------------------------------------- validation
def vcase(name, doc, code, step_id):
    case(name, doc, 2, {}, [], [], (code, step_id))


vcase("missing_version", {"steps": []}, "INVALID_WORKFLOW", None)
vcase("version_float", {"version": 1.0, "steps": []}, "INVALID_WORKFLOW", None)
vcase("version_string", {"version": "1", "steps": []}, "INVALID_WORKFLOW", None)
vcase("version_two", {"version": 2, "steps": []}, "INVALID_WORKFLOW", None)
vcase("unknown_top", {"version": 1, "steps": [], "extra": 1}, "INVALID_WORKFLOW", None)
vcase("missing_steps", {"version": 1}, "INVALID_WORKFLOW", None)
vcase("steps_object", {"version": 1, "steps": {}}, "INVALID_WORKFLOW", None)
vcase("step_not_object", {"version": 1, "steps": [5]}, "INVALID_WORKFLOW", None)
vcase(
    "unknown_step_field",
    wf({"id": "a", "op": "constant", "args": {"value": 1}, "extra": 2}),
    "INVALID_WORKFLOW",
    "a",
)
vcase("missing_id", wf({"op": "constant", "args": {"value": 1}}), "INVALID_WORKFLOW", None)
vcase("nonstring_id", wf({"id": 5, "op": "constant"}), "INVALID_WORKFLOW", None)
vcase("bad_id", wf({"id": "1bad", "op": "constant"}), "INVALID_WORKFLOW", "1bad")
vcase(
    "duplicate_id",
    wf(
        {"id": "a", "op": "constant", "args": {"value": 1}},
        {"id": "a", "op": "constant", "args": {"value": 2}},
    ),
    "INVALID_WORKFLOW",
    "a",
)
vcase("missing_op", wf({"id": "a", "args": {}}), "INVALID_WORKFLOW", "a")
vcase("unknown_op", wf({"id": "a", "op": "nope", "args": {}}), "INVALID_WORKFLOW", "a")
vcase(
    "args_not_object",
    wf({"id": "a", "op": "constant", "args": []}),
    "INVALID_WORKFLOW",
    "a",
)
vcase(
    "depends_not_array",
    wf({"id": "a", "op": "constant", "depends_on": "b"}),
    "INVALID_WORKFLOW",
    "a",
)
vcase(
    "depends_nonstring",
    wf({"id": "a", "op": "constant", "depends_on": [5]}),
    "INVALID_WORKFLOW",
    "a",
)
vcase(
    "retry_not_object",
    wf({"id": "a", "op": "constant", "retry": 3}),
    "INVALID_WORKFLOW",
    "a",
)
vcase(
    "retry_zero",
    wf({"id": "a", "op": "constant", "retry": {"max_attempts": 0}}),
    "INVALID_WORKFLOW",
    "a",
)
vcase(
    "retry_six",
    wf({"id": "a", "op": "constant", "retry": {"max_attempts": 6}}),
    "INVALID_WORKFLOW",
    "a",
)
vcase(
    "retry_float",
    wf({"id": "a", "op": "constant", "retry": {"max_attempts": 2.0}}),
    "INVALID_WORKFLOW",
    "a",
)
vcase(
    "retry_string",
    wf({"id": "a", "op": "constant", "retry": {"max_attempts": "3"}}),
    "INVALID_WORKFLOW",
    "a",
)
vcase(
    "retry_unknown_field",
    wf({"id": "a", "op": "constant", "retry": {"max_attempts": 2, "x": 1}}),
    "INVALID_WORKFLOW",
    "a",
)
vcase(
    "when_not_object",
    wf({"id": "a", "op": "constant", "when": True}),
    "INVALID_WORKFLOW",
    "a",
)
vcase(
    "when_no_op",
    wf({"id": "a", "op": "constant", "when": {}}),
    "INVALID_WORKFLOW",
    "a",
)

case("empty_steps", wf(), 0, {}, [], [])
case(
    "step_no_args",
    wf(step("z", "constant", {"value": 0})),
    0,
    {"z": 0},
    ["z"],
    [],
)

# ---------------------------------------------------------------- runtime args
case(
    "unknown_arg",
    wf(step("a", "add", {"a": 1, "b": 2, "c": 3})),
    1,
    {},
    [],
    [],
    ("INVALID_ARG", "a"),
)
case(
    "missing_arg",
    wf(step("a", "add", {"a": 1})),
    1,
    {},
    [],
    [],
    ("INVALID_ARG", "a"),
)
case(
    "arg_wrong_type",
    wf(step("a", "add", {"a": "1", "b": 2})),
    1,
    {},
    [],
    [],
    ("INVALID_ARG", "a"),
)
case(
    "arg_float_literal",
    wf(step("a", "add", {"a": 1.0, "b": 2})),
    1,
    {},
    [],
    [],
    ("INVALID_ARG", "a"),
)
case(
    "arg_exp_literal",
    wf(step("a", "add", {"a": 1e3, "b": 2})),
    1,
    {},
    [],
    [],
    ("INVALID_ARG", "a"),
)
case(
    "constant_null_arg",
    wf(step("a", "constant", {"value": None})),
    0,
    {"a": None},
    ["a"],
    [],
)
case(
    "constant_missing_value",
    wf(step("a", "constant", {})),
    1,
    {},
    [],
    [],
    ("INVALID_ARG", "a"),
)
case(
    "bad_condition_op",
    wf(step("a", "constant", {"value": 1}, when={"op": "bogus", "left": 1, "right": 1})),
    1,
    {},
    [],
    [],
    ("INVALID_CONDITION", "a"),
)
case(
    "condition_missing_operand",
    wf(step("a", "constant", {"value": 1}, when={"op": "gt", "left": 1})),
    1,
    {},
    [],
    [],
    ("INVALID_CONDITION", "a"),
)
case(
    "condition_nonboolean_sub",
    wf(
        step(
            "a",
            "constant",
            {"value": 1},
            when={"op": "all", "conditions": [1]},
        )
    ),
    1,
    {},
    [],
    [],
    ("INVALID_CONDITION", "a"),
)

# ---------------------------------------------------------------- retries
case(
    "retry_exhausted",
    wf(
        step(
            "flaky",
            "fail_until",
            {"failures": 3, "value": "ok"},
            retry={"max_attempts": 2},
        )
    ),
    1,
    {},
    [],
    [],
    ("STEP_FAILED", "flaky"),
)
case(
    "retry_default_one",
    wf(step("flaky", "fail_until", {"failures": 1, "value": "ok"})),
    1,
    {},
    [],
    [],
    ("STEP_FAILED", "flaky"),
)
case(
    "fail_until_zero",
    wf(step("ok", "fail_until", {"failures": 0, "value": "ok"})),
    0,
    {"ok": "ok"},
    ["ok"],
    [],
)
case(
    "non_transient_not_retried",
    wf(
        step("d", "divide", {"a": 1, "b": 0}, retry={"max_attempts": 5}),
    ),
    1,
    {},
    [],
    [],
    ("DIVISION_BY_ZERO", "d"),
)
case(
    "fail_until_range",
    wf(step("f", "fail_until", {"failures": 11, "value": 1})),
    1,
    {},
    [],
    [],
    ("INVALID_ARG", "f"),
)
case(
    "failure_keeps_prior",
    wf(
        step("a", "constant", {"value": 1}),
        step("b", "constant", {"value": 2}, ["a"]),
        step("bad", "divide", {"a": {"$ref": "b"}, "b": 0}, ["b"]),
        step("never", "constant", {"value": 9}, ["bad"]),
    ),
    1,
    {"a": 1, "b": 2},
    ["a", "b"],
    [],
    ("DIVISION_BY_ZERO", "bad"),
)
case(
    "skip_then_failure",
    wf(
        step("a", "constant", {"value": 1}, when={"op": "gt", "left": 1, "right": 2}),
        step("b", "divide", {"a": 1, "b": 0}, ["a"]),
    ),
    1,
    {"a": None},
    ["a"],
    ["a"],
    ("DIVISION_BY_ZERO", "b"),
)

# ---------------------------------------------------------------- map errors
case(
    "map_bad_element",
    wf(step("m", "map", {"input": [1, "x"], "transform": {"type": "negate"}})),
    1,
    {},
    [],
    [],
    ("INVALID_ARG", "m"),
)
case(
    "map_unknown_transform",
    wf(step("m", "map", {"input": [1], "transform": {"type": "nope"}})),
    1,
    {},
    [],
    [],
    ("INVALID_ARG", "m"),
)
case(
    "reduce_bad_op",
    wf(step("r", "reduce", {"input": [1], "op": "divide"})),
    1,
    {},
    [],
    [],
    ("INVALID_ARG", "r"),
)
case(
    "replace_empty_search",
    wf(step("r", "replace", {"input": "abc", "search": "", "replace": "x"})),
    1,
    {},
    [],
    [],
    ("INVALID_ARG", "r"),
)


def run_case(c):
    proc = subprocess.run(
        [BIN, "run", "-"],
        input=json.dumps(c["doc"]),
        capture_output=True,
        text=True,
        timeout=30,
    )
    problems = []
    if proc.returncode != c["exit"]:
        problems.append(f"exit {proc.returncode} != {c['exit']}")
    try:
        out = json.loads(proc.stdout)
    except Exception as e:  # noqa: BLE001
        return [f"stdout is not one JSON document: {e}: {proc.stdout!r} {proc.stderr!r}"]
    for key in ("status", "results", "execution_order", "skipped", "error"):
        if key not in out:
            problems.append(f"missing key {key}")
    if problems:
        return problems

    expected_status = "completed" if c["exit"] == 0 else "failed"
    if out["status"] != expected_status:
        problems.append(f"status {out['status']!r} != {expected_status!r}")
    if out["results"] != c["results"]:
        problems.append(f"results {out['results']!r} != {c['results']!r}")
    if out["execution_order"] != c["order"]:
        problems.append(f"order {out['execution_order']!r} != {c['order']!r}")
    if out["skipped"] != c["skipped"]:
        problems.append(f"skipped {out['skipped']!r} != {c['skipped']!r}")
    if c["error"] is None:
        if out["error"] is not None:
            problems.append(f"error {out['error']!r} != None")
    else:
        code, sid = c["error"]
        err = out["error"]
        if not isinstance(err, dict):
            problems.append(f"error {err!r} is not an object")
        else:
            if err.get("code") != code:
                problems.append(f"error code {err.get('code')!r} != {code!r}")
            if err.get("step") != sid:
                problems.append(f"error step {err.get('step')!r} != {sid!r}")
    return problems


def main():
    failures = 0
    for c in CASES:
        try:
            problems = run_case(c)
        except Exception as e:  # noqa: BLE001
            problems = [f"exception: {e}"]
        if problems:
            failures += 1
            print(f"FAIL {c['name']}")
            for p in problems:
                print(f"     {p}")
        else:
            print(f"ok   {c['name']}")
    print()
    print(f"{len(CASES) - failures}/{len(CASES)} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
