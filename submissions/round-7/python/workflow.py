#!/usr/bin/env python3
"""Declarative workflow engine CLI.

Usage:
    python3 workflow.py run <workflow.json>
    python3 workflow.py run -          # read document from stdin

Only the Python standard library is used.
"""

import json
import math
import sys
from functools import cmp_to_key

# ---------------------------------------------------------------------------
# Constants and errors
# ---------------------------------------------------------------------------

I64_MIN = -(1 << 63)
I64_MAX = (1 << 63) - 1

STEP_ID_RE = None  # initialized in _init_regex below to avoid importing re early

COMPARE_OPS = {"eq", "ne", "gt", "gte", "lt", "lte"}
COND_OPS = COMPARE_OPS | {"all", "any", "not"}

OPS = {
    # math
    "add", "subtract", "multiply", "divide", "mod", "sum", "min", "max",
    "range",
    # strings
    "concat", "split", "join", "uppercase", "lowercase", "trim", "replace",
    "length",
    # arrays
    "map", "filter", "reduce", "sort", "reverse", "unique", "slice",
    "flatten",
    # objects
    "get", "set", "keys", "values", "merge", "pick", "omit",
    # utilities
    "constant", "equals", "compare", "and", "or", "not", "type",
    # test helper
    "fail_until",
}

STEP_FIELDS = {"id", "op", "args", "depends_on", "when", "retry"}
TOP_FIELDS = {"version", "steps"}


class WorkflowError(Exception):
    def __init__(self, code, message, step=None):
        super().__init__(message)
        self.code = code
        self.message = message
        self.step = step


class InternalError(Exception):
    pass


def exit_code_for(code):
    if code in ("INVALID_WORKFLOW", "UNKNOWN_DEPENDENCY", "CYCLIC_DEPENDENCY"):
        return 2
    if code == "INTERNAL_ERROR":
        return 3
    return 1


# ---------------------------------------------------------------------------
# JSON value helpers
# ---------------------------------------------------------------------------

def is_ref_object(v):
    """An object is a reference only if it has exactly one key, '$ref',
    whose value is a string."""
    return (isinstance(v, dict) and len(v) == 1
            and "$ref" in v and isinstance(v["$ref"], str))


def is_bool(v):
    return type(v) is bool


def is_int(v):
    # bool is a subclass of int in Python; explicitly exclude it.
    return type(v) is int


def is_number(v):
    return type(v) is int or type(v) is float


def kind(v):
    if v is None:
        return "null"
    if type(v) is bool:
        return "boolean"
    if type(v) is int or type(v) is float:
        return "number"
    if isinstance(v, str):
        return "string"
    if isinstance(v, list):
        return "array"
    if isinstance(v, dict):
        return "object"
    # Should never happen for values sourced from JSON.
    raise InternalError("unknown JSON value kind: %r" % (type(v),))


TYPE_RANK = {
    "number": 0,
    "string": 1,
    "boolean": 2,
    "null": 3,
    "array": 4,
    "object": 5,
}


def utf8_key(s):
    return s.encode("utf-8")


def sorted_keys(obj):
    return sorted(obj.keys(), key=utf8_key)


def struct_eq(a, b):
    ka = kind(a)
    kb = kind(b)
    if ka != kb:
        return False
    if ka == "null":
        return True
    if ka == "number":
        return a == b
    if ka in ("boolean", "string"):
        return a == b
    if ka == "array":
        if len(a) != len(b):
            return False
        return all(struct_eq(x, y) for x, y in zip(a, b))
    # object
    if len(a) != len(b):
        return False
    if set(a.keys()) != set(b.keys()):
        return False
    return all(struct_eq(a[k], b[k]) for k in a)


def cmp_json(a, b):
    """Total order over JSON values, returning -1, 0 or 1."""
    ka = kind(a)
    kb = kind(b)
    if ka != kb:
        return -1 if TYPE_RANK[ka] < TYPE_RANK[kb] else 1
    if ka == "number":
        if a < b:
            return -1
        if a > b:
            return 1
        return 0
    if ka == "string":
        ba = utf8_key(a)
        bb = utf8_key(b)
        if ba < bb:
            return -1
        if ba > bb:
            return 1
        return 0
    if ka == "boolean":
        ia = 1 if a else 0
        ib = 1 if b else 0
        return -1 if ia < ib else (1 if ia > ib else 0)
    if ka == "null":
        return 0
    if ka == "array":
        for x, y in zip(a, b):
            c = cmp_json(x, y)
            if c != 0:
                return c
        if len(a) < len(b):
            return -1
        if len(a) > len(b):
            return 1
        return 0
    # object
    ka_keys = sorted(a.keys(), key=utf8_key)
    kb_keys = sorted(b.keys(), key=utf8_key)
    for x, y in zip(ka_keys, kb_keys):
        bx = utf8_key(x)
        by = utf8_key(y)
        if bx < by:
            return -1
        if bx > by:
            return 1
    if len(ka_keys) < len(kb_keys):
        return -1
    if len(ka_keys) > len(kb_keys):
        return 1
    for k in ka_keys:
        c = cmp_json(a[k], b[k])
        if c != 0:
            return c
    return 0


def truthy(v):
    if v is None:
        return False
    if type(v) is bool:
        return v
    if type(v) is int or type(v) is float:
        return v != 0
    if isinstance(v, (str, list, dict)):
        return len(v) > 0
    return False


def json_stringify(v):
    return json.dumps(v, ensure_ascii=False, separators=(",", ":"),
                      allow_nan=False)


# ---------------------------------------------------------------------------
# i64 arithmetic
# ---------------------------------------------------------------------------

def check_i64(x):
    if x < I64_MIN or x > I64_MAX:
        raise WorkflowError("ARITHMETIC_OVERFLOW",
                            "integer overflow (result outside i64)")
    return x


def arith_add(a, b):
    return check_i64(a + b)


def arith_sub(a, b):
    return check_i64(a - b)


def arith_mul(a, b):
    return check_i64(a * b)


def trunc_div(a, b):
    if b == 0:
        raise WorkflowError("DIVISION_BY_ZERO", "division by zero")
    if a == I64_MIN and b == -1:
        raise WorkflowError("ARITHMETIC_OVERFLOW",
                            "INT64_MIN / -1 overflows")
    q = abs(a) // abs(b)
    if (a < 0) != (b < 0):
        q = -q
    return check_i64(q)


def trunc_mod(a, b):
    if b == 0:
        raise WorkflowError("DIVISION_BY_ZERO", "modulo by zero")
    if a == I64_MIN and b == -1:
        raise WorkflowError("ARITHMETIC_OVERFLOW",
                            "INT64_MIN % -1 overflows")
    return check_i64(a - trunc_div(a, b) * b)


def arith_negate(a):
    if a == I64_MIN:
        raise WorkflowError("ARITHMETIC_OVERFLOW",
                            "negation of INT64_MIN overflows")
    return -a


def arith_abs(a):
    if a == I64_MIN:
        raise WorkflowError("ARITHMETIC_OVERFLOW",
                            "abs(INT64_MIN) overflows")
    return abs(a)


# ---------------------------------------------------------------------------
# Argument helpers
# ---------------------------------------------------------------------------

def invalid_arg(message):
    return WorkflowError("INVALID_ARG", message)


def check_args(args, allowed, required):
    if not isinstance(args, dict):
        raise invalid_arg("args must be an object")
    for k in args:
        if k not in allowed:
            raise invalid_arg("unknown argument: %r" % (k,))
    for k in required:
        if k not in args:
            raise invalid_arg("missing required argument: %r" % (k,))


def as_i64(v, name="value"):
    if not is_int(v):
        raise invalid_arg("argument %r must be an integer" % (name,))
    if v < I64_MIN or v > I64_MAX:
        raise invalid_arg("argument %r is outside the i64 range" % (name,))
    return v


def as_str(v, name="value"):
    if not isinstance(v, str):
        raise invalid_arg("argument %r must be a string" % (name,))
    return v


def as_list(v, name="value"):
    if not isinstance(v, list):
        raise invalid_arg("argument %r must be an array" % (name,))
    return v


def as_obj(v, name="value"):
    if not isinstance(v, dict):
        raise invalid_arg("argument %r must be an object" % (name,))
    return v


def as_i64_list(v, name="input"):
    arr = as_list(v, name)
    out = []
    for i, x in enumerate(arr):
        if not is_int(x):
            raise invalid_arg("array %r element %d must be an integer"
                              % (name, i))
        if x < I64_MIN or x > I64_MAX:
            raise invalid_arg("array %r element %d is outside the i64 range"
                              % (name, i))
        out.append(x)
    return out


def as_str_list(v, name="keys"):
    arr = as_list(v, name)
    out = []
    for i, x in enumerate(arr):
        if not isinstance(x, str):
            raise invalid_arg("array %r element %d must be a string"
                              % (name, i))
        out.append(x)
    return out


# ---------------------------------------------------------------------------
# Operations
# ---------------------------------------------------------------------------

def op_binary_math(args, fn):
    check_args(args, {"a", "b"}, {"a", "b"})
    a = as_i64(args["a"], "a")
    b = as_i64(args["b"], "b")
    return fn(a, b)


def op_add(args):
    return op_binary_math(args, arith_add)


def op_subtract(args):
    return op_binary_math(args, arith_sub)


def op_multiply(args):
    return op_binary_math(args, arith_mul)


def op_divide(args):
    return op_binary_math(args, trunc_div)


def op_mod(args):
    return op_binary_math(args, trunc_mod)


def op_sum(args):
    check_args(args, {"input"}, {"input"})
    vals = as_i64_list(args["input"], "input")
    acc = 0
    for v in vals:
        acc = arith_add(acc, v)
    return acc


def op_min(args):
    check_args(args, {"input"}, {"input"})
    vals = as_i64_list(args["input"], "input")
    if not vals:
        raise WorkflowError("EMPTY_ARRAY", "min of an empty array")
    return min(vals)


def op_max(args):
    check_args(args, {"input"}, {"input"})
    vals = as_i64_list(args["input"], "input")
    if not vals:
        raise WorkflowError("EMPTY_ARRAY", "max of an empty array")
    return max(vals)


def op_range(args):
    check_args(args, {"start", "end"}, {"start", "end"})
    start = as_i64(args["start"], "start")
    end = as_i64(args["end"], "end")
    if start > end:
        return []
    return list(range(start, end + 1))


def op_concat(args):
    check_args(args, {"a", "b"}, {"a", "b"})
    return as_str(args["a"], "a") + as_str(args["b"], "b")


def op_split(args):
    check_args(args, {"input", "sep"}, {"input", "sep"})
    s = as_str(args["input"], "input")
    sep = as_str(args["sep"], "sep")
    if sep == "":
        return list(s)
    return s.split(sep)


def op_join(args):
    check_args(args, {"input", "sep"}, {"input", "sep"})
    vals = as_str_list(args["input"], "input")
    sep = as_str(args["sep"], "sep")
    return sep.join(vals)


_ASCII_UPPER = str.maketrans(
    "abcdefghijklmnopqrstuvwxyz", "ABCDEFGHIJKLMNOPQRSTUVWXYZ")
_ASCII_LOWER = str.maketrans(
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ", "abcdefghijklmnopqrstuvwxyz")
_ASCII_WS = " \t\n\r\f\v"


def op_uppercase(args):
    check_args(args, {"input"}, {"input"})
    return as_str(args["input"], "input").translate(_ASCII_UPPER)


def op_lowercase(args):
    check_args(args, {"input"}, {"input"})
    return as_str(args["input"], "input").translate(_ASCII_LOWER)


def op_trim(args):
    check_args(args, {"input"}, {"input"})
    return as_str(args["input"], "input").strip(_ASCII_WS)


def op_replace(args):
    check_args(args, {"input", "search", "replace"},
               {"input", "search", "replace"})
    s = as_str(args["input"], "input")
    search = as_str(args["search"], "search")
    repl = as_str(args["replace"], "replace")
    if search == "":
        raise invalid_arg("replace: search string must not be empty")
    return s.replace(search, repl)


def op_length(args):
    check_args(args, {"input"}, {"input"})
    return len(as_str(args["input"], "input"))


# --- map transforms -------------------------------------------------------

_MAP_SIMPLE = {
    "constant", "add", "subtract", "multiply", "divide", "mod", "negate",
    "abs", "stringify", "type", "op",
}
_MAP_BINARY = {
    "add": arith_add,
    "subtract": arith_sub,
    "multiply": arith_mul,
    "divide": trunc_div,
    "mod": trunc_mod,
}
_MAP_INNER_HANDLERS = {
    "add": op_add,
    "subtract": op_subtract,
    "multiply": op_multiply,
    "divide": op_divide,
    "mod": op_mod,
}


def _validate_transform(transform):
    if not isinstance(transform, dict):
        raise invalid_arg("map: transform must be an object")
    t = transform.get("type")
    if not isinstance(t, str):
        raise invalid_arg("map: transform.type must be a string")
    if t not in _MAP_SIMPLE:
        raise invalid_arg("map: unknown transform type %r" % (t,))
    if t == "constant":
        check_args(transform, {"type", "value"}, {"type", "value"})
    elif t in ("add", "subtract", "multiply", "divide", "mod"):
        check_args(transform, {"type", "value"}, {"type", "value"})
        as_i64(transform["value"], "value")
    elif t in ("negate", "abs", "stringify", "type"):
        check_args(transform, {"type"}, {"type"})
    elif t == "op":
        check_args(transform, {"type", "op", "args"}, {"type", "op", "args"})
        inner_op = transform["op"]
        if not isinstance(inner_op, str) or inner_op not in _MAP_BINARY:
            raise invalid_arg("map: unsupported inner op %r" % (inner_op,))
        if not isinstance(transform["args"], dict):
            raise invalid_arg("map: transform.args must be an object")


def _replace_self(v, element):
    if isinstance(v, dict):
        if len(v) == 1 and "$self" in v and v["$self"] is True:
            return element
        return {k: _replace_self(x, element) for k, x in v.items()}
    if isinstance(v, list):
        return [_replace_self(x, element) for x in v]
    return v


def _apply_transform(transform, element):
    t = transform["type"]
    if t == "constant":
        return transform["value"]
    if t in _MAP_BINARY:
        x = as_i64(element, "element")
        y = as_i64(transform["value"], "value")
        return _MAP_BINARY[t](x, y)
    if t == "negate":
        return arith_negate(as_i64(element, "element"))
    if t == "abs":
        return arith_abs(as_i64(element, "element"))
    if t == "stringify":
        return json_stringify(element)
    if t == "type":
        return kind(element)
    if t == "op":
        inner_op = transform["op"]
        resolved_args = _replace_self(transform["args"], element)
        return _MAP_INNER_HANDLERS[inner_op](resolved_args)
    raise invalid_arg("map: unknown transform type %r" % (t,))


def op_map(args):
    check_args(args, {"input", "transform"}, {"input", "transform"})
    arr = as_list(args["input"], "input")
    transform = args["transform"]
    _validate_transform(transform)
    return [_apply_transform(transform, x) for x in arr]


# --- filter predicates ----------------------------------------------------

_FILTER_SIMPLE = {
    "even", "odd", "gt", "gte", "lt", "lte", "eq", "ne", "truthy", "falsy",
    "type_is", "string_contains",
}
_FILTER_CMP = {"gt", "gte", "lt", "lte"}
_FILTER_EQ = {"eq", "ne"}
_JSON_TYPE_NAMES = {"null", "boolean", "number", "string", "array", "object"}


def _validate_predicate(pred):
    if not isinstance(pred, dict):
        raise invalid_arg("filter: predicate must be an object")
    t = pred.get("type")
    if not isinstance(t, str):
        raise invalid_arg("filter: predicate.type must be a string")
    if t not in _FILTER_SIMPLE:
        # SPEC's type_is template repeats the key "type"
        # ({"type": "type_is", "type": "<type>"}); a JSON parser collapses
        # that to {"type": "<type>"}.  Accept that collapsed form too.
        if t in _JSON_TYPE_NAMES:
            check_args(pred, {"type"}, {"type"})
            return
        raise invalid_arg("filter: unknown predicate type %r" % (t,))
    if t in ("even", "odd", "truthy", "falsy"):
        check_args(pred, {"type"}, {"type"})
    elif t in (_FILTER_CMP | _FILTER_EQ):
        check_args(pred, {"type", "value"}, {"type", "value"})
        as_i64(pred["value"], "value")
    elif t == "type_is":
        # The SPEC template {"type": "type_is", "type": "<type>"} repeats the
        # key "type", which JSON parsers cannot represent.  Accept the element
        # type name under either "value" or "type_name".
        check_args(pred, {"type", "value", "type_name"}, set())
        if "value" not in pred and "type_name" not in pred:
            raise invalid_arg(
                "filter: type_is requires a type name in 'value'")
        target = pred.get("type_name", pred.get("value"))
        if not isinstance(target, str):
            raise invalid_arg("filter: type_is type name must be a string")
    elif t == "string_contains":
        check_args(pred, {"type", "value"}, {"type", "value"})
        as_str(pred["value"], "value")


def _apply_predicate(pred, element):
    t = pred["type"]
    if t == "even":
        x = as_i64(element, "element")
        return x % 2 == 0
    if t == "odd":
        x = as_i64(element, "element")
        return x % 2 != 0
    if t in _FILTER_CMP:
        x = as_i64(element, "element")
        y = as_i64(pred["value"], "value")
        c = cmp_json(x, y)
        if t == "gt":
            return c > 0
        if t == "gte":
            return c >= 0
        if t == "lt":
            return c < 0
        return c <= 0
    if t in _FILTER_EQ:
        return struct_eq(element, pred["value"]) == (t == "eq")
    if t == "truthy":
        return truthy(element)
    if t == "falsy":
        return not truthy(element)
    if t == "type_is":
        target = pred.get("type_name", pred.get("value"))
        return kind(element) == target
    if t in _JSON_TYPE_NAMES:
        return kind(element) == t
    if t == "string_contains":
        if not isinstance(element, str):
            raise invalid_arg("filter: string_contains element must be a string")
        return pred["value"] in element
    raise invalid_arg("filter: unknown predicate type %r" % (t,))


def op_filter(args):
    check_args(args, {"input", "predicate"}, {"input", "predicate"})
    arr = as_list(args["input"], "input")
    pred = args["predicate"]
    _validate_predicate(pred)
    return [x for x in arr if _apply_predicate(pred, x)]


# --- other array operations ----------------------------------------------

_REDUCE_OPS = {
    "add": arith_add,
    "subtract": arith_sub,
    "multiply": arith_mul,
}
_REDUCE_IDENTITY = {"add": 0, "subtract": 0, "multiply": 1}


def op_reduce(args):
    check_args(args, {"input", "op", "initial"}, {"input", "op"})
    arr = as_i64_list(args["input"], "input")
    opname = as_str(args["op"], "op")
    if opname not in _REDUCE_OPS:
        raise invalid_arg("reduce: unsupported op %r" % (opname,))
    if "initial" in args:
        acc = as_i64(args["initial"], "initial")
    else:
        acc = _REDUCE_IDENTITY[opname]
    fn = _REDUCE_OPS[opname]
    for x in arr:
        acc = fn(acc, x)
    return acc


def op_sort(args):
    check_args(args, {"input"}, {"input"})
    arr = as_list(args["input"], "input")
    return sorted(arr, key=cmp_to_key(cmp_json))


def op_reverse(args):
    check_args(args, {"input"}, {"input"})
    arr = as_list(args["input"], "input")
    return list(reversed(arr))


def op_unique(args):
    check_args(args, {"input"}, {"input"})
    arr = as_list(args["input"], "input")
    out = []
    for x in arr:
        if not any(struct_eq(x, y) for y in out):
            out.append(x)
    return out


def _normalize_index(idx, n):
    if idx < 0:
        idx += n
    if idx < 0:
        return 0
    if idx > n:
        return n
    return idx


def op_slice(args):
    check_args(args, {"input", "start", "end"}, {"input", "start"})
    arr = as_list(args["input"], "input")
    n = len(arr)
    start = _normalize_index(as_i64(args["start"], "start"), n)
    if "end" in args:
        end = _normalize_index(as_i64(args["end"], "end"), n)
    else:
        end = n
    if start >= end:
        return []
    return arr[start:end]


def op_flatten(args):
    check_args(args, {"input"}, {"input"})
    arr = as_list(args["input"], "input")
    out = []
    for x in arr:
        if isinstance(x, list):
            out.extend(x)
        else:
            out.append(x)
    return out


# --- object operations ----------------------------------------------------

def op_get(args):
    check_args(args, {"input", "key"}, {"input", "key"})
    obj = as_obj(args["input"], "input")
    key = as_str(args["key"], "key")
    return obj.get(key, None)


def op_set(args):
    check_args(args, {"input", "key", "value"}, {"input", "key", "value"})
    obj = as_obj(args["input"], "input")
    key = as_str(args["key"], "key")
    out = dict(obj)
    out[key] = args["value"]
    return {k: out[k] for k in sorted_keys(out)}


def op_keys(args):
    check_args(args, {"input"}, {"input"})
    obj = as_obj(args["input"], "input")
    return sorted_keys(obj)


def op_values(args):
    check_args(args, {"input"}, {"input"})
    obj = as_obj(args["input"], "input")
    return [obj[k] for k in sorted_keys(obj)]


def op_merge(args):
    check_args(args, {"a", "b"}, {"a", "b"})
    a = as_obj(args["a"], "a")
    b = as_obj(args["b"], "b")
    out = dict(a)
    out.update(b)
    return {k: out[k] for k in sorted_keys(out)}


def op_pick(args):
    check_args(args, {"input", "keys"}, {"input", "keys"})
    obj = as_obj(args["input"], "input")
    keys = as_str_list(args["keys"], "keys")
    out = {}
    for k in keys:
        out[k] = obj.get(k, None)
    return {k: out[k] for k in sorted_keys(out)}


def op_omit(args):
    check_args(args, {"input", "keys"}, {"input", "keys"})
    obj = as_obj(args["input"], "input")
    keys = set(as_str_list(args["keys"], "keys"))
    out = {k: v for k, v in obj.items() if k not in keys}
    return {k: out[k] for k in sorted_keys(out)}


# --- utility operations ---------------------------------------------------

def op_constant(args):
    check_args(args, {"value"}, {"value"})
    return args["value"]


def op_equals(args):
    check_args(args, {"a", "b"}, {"a", "b"})
    return struct_eq(args["a"], args["b"])


def op_compare(args):
    check_args(args, {"a", "b", "op"}, {"a", "b", "op"})
    opname = as_str(args["op"], "op")
    if opname not in ("gt", "gte", "lt", "lte"):
        raise invalid_arg("compare: unsupported op %r" % (opname,))
    c = cmp_json(args["a"], args["b"])
    if opname == "gt":
        return c > 0
    if opname == "gte":
        return c >= 0
    if opname == "lt":
        return c < 0
    return c <= 0


def op_and(args):
    check_args(args, {"a", "b"}, {"a", "b"})
    return truthy(args["a"]) and truthy(args["b"])


def op_or(args):
    check_args(args, {"a", "b"}, {"a", "b"})
    return truthy(args["a"]) or truthy(args["b"])


def op_not(args):
    check_args(args, {"a"}, {"a"})
    return not truthy(args["a"])


def op_type(args):
    check_args(args, {"value"}, {"value"})
    return kind(args["value"])


OP_HANDLERS = {
    "add": op_add,
    "subtract": op_subtract,
    "multiply": op_multiply,
    "divide": op_divide,
    "mod": op_mod,
    "sum": op_sum,
    "min": op_min,
    "max": op_max,
    "range": op_range,
    "concat": op_concat,
    "split": op_split,
    "join": op_join,
    "uppercase": op_uppercase,
    "lowercase": op_lowercase,
    "trim": op_trim,
    "replace": op_replace,
    "length": op_length,
    "map": op_map,
    "filter": op_filter,
    "reduce": op_reduce,
    "sort": op_sort,
    "reverse": op_reverse,
    "unique": op_unique,
    "slice": op_slice,
    "flatten": op_flatten,
    "get": op_get,
    "set": op_set,
    "keys": op_keys,
    "values": op_values,
    "merge": op_merge,
    "pick": op_pick,
    "omit": op_omit,
    "constant": op_constant,
    "equals": op_equals,
    "compare": op_compare,
    "and": op_and,
    "or": op_or,
    "not": op_not,
    "type": op_type,
}


# ---------------------------------------------------------------------------
# Structural validation
# ---------------------------------------------------------------------------

def _init_regex():
    global STEP_ID_RE
    import re
    STEP_ID_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


def valid_step_id(s):
    return isinstance(s, str) and STEP_ID_RE.fullmatch(s) is not None


def workflow_error(message, step=None):
    return WorkflowError("INVALID_WORKFLOW", message, step)


def validate_when_structure(w, step_id):
    # A non-object `when` is treated as a literal condition value and is
    # checked for strict boolean-ness at run time (INVALID_CONDITION).
    if not isinstance(w, dict):
        return
    if is_ref_object(w):
        return
    if "op" not in w:
        # A plain object literal; evaluated at runtime and rejected as a
        # non-boolean condition.
        return
    op = w["op"]
    if not isinstance(op, str):
        return
    if op in COMPARE_OPS:
        for k in w:
            if k not in ("op", "left", "right"):
                raise workflow_error("unknown condition field %r" % (k,),
                                     step_id)
        if "left" not in w or "right" not in w:
            raise workflow_error(
                "comparison condition requires 'left' and 'right'", step_id)
    elif op in ("all", "any"):
        for k in w:
            if k not in ("op", "conditions"):
                raise workflow_error("unknown condition field %r" % (k,),
                                     step_id)
        if not isinstance(w.get("conditions"), list):
            raise workflow_error("condition 'conditions' must be an array",
                                 step_id)
    elif op == "not":
        for k in w:
            if k not in ("op", "condition"):
                raise workflow_error("unknown condition field %r" % (k,),
                                     step_id)
        if "condition" not in w:
            raise workflow_error("condition 'not' requires 'condition'",
                                 step_id)
    else:
        raise workflow_error("unknown condition op %r" % (op,), step_id)


def validate_document(doc):
    """Validate structure. Returns list of normalized step dicts."""
    if not isinstance(doc, dict):
        raise workflow_error("workflow document must be an object")

    for k in doc:
        if k not in TOP_FIELDS:
            raise workflow_error("unknown top-level field %r" % (k,))

    if "version" not in doc:
        raise workflow_error("missing workflow version")
    version = doc["version"]
    if not (is_int(version) and version == 1):
        raise workflow_error("workflow version must be the integer 1")

    if "steps" not in doc:
        raise workflow_error("missing workflow steps")
    raw_steps = doc["steps"]
    if not isinstance(raw_steps, list):
        raise workflow_error("workflow steps must be an array")

    steps = []
    seen = set()
    for idx, raw in enumerate(raw_steps):
        if not isinstance(raw, dict):
            raise workflow_error("step %d must be an object" % idx)

        if "id" not in raw:
            raise workflow_error("step %d is missing 'id'" % idx)
        sid = raw["id"]
        if not isinstance(sid, str):
            raise workflow_error("step %d has a non-string 'id'" % idx)
        if not valid_step_id(sid):
            raise workflow_error("invalid step id %r" % (sid,), sid)
        if sid in seen:
            raise workflow_error("duplicate step id %r" % (sid,), sid)
        seen.add(sid)

        for k in raw:
            if k not in STEP_FIELDS:
                raise workflow_error("unknown step field %r" % (k,), sid)

        if "op" not in raw:
            raise workflow_error("step %r is missing 'op'" % (sid,), sid)
        opname = raw["op"]
        if not isinstance(opname, str) or opname not in OPS:
            raise workflow_error("unknown operation %r" % (opname,), sid)

        args = raw.get("args", {})
        if not isinstance(args, dict):
            raise workflow_error("step 'args' must be an object", sid)

        depends_on = raw.get("depends_on", [])
        if not isinstance(depends_on, list):
            raise workflow_error("step 'depends_on' must be an array", sid)
        deps = []
        for d in depends_on:
            if not isinstance(d, str):
                raise workflow_error(
                    "step 'depends_on' entries must be strings", sid)
            deps.append(d)

        if "when" in raw:
            validate_when_structure(raw["when"], sid)

        retry = raw.get("retry", {})
        if not isinstance(retry, dict):
            raise workflow_error("step 'retry' must be an object", sid)
        for k in retry:
            if k != "max_attempts":
                raise workflow_error("unknown retry field %r" % (k,), sid)
        max_attempts = retry.get("max_attempts", 1)
        if not is_int(max_attempts) or max_attempts < 1 or max_attempts > 5:
            raise workflow_error(
                "retry.max_attempts must be an integer in 1..5", sid)

        steps.append({
            "id": sid,
            "op": opname,
            "args": args,
            "depends_on": deps,
            "when": raw.get("when", None),
            "has_when": "when" in raw,
            "max_attempts": max_attempts,
            "index": idx,
        })

    # Unknown dependencies.
    ids = set(s["id"] for s in steps)
    for s in steps:
        for d in s["depends_on"]:
            if d not in ids:
                raise WorkflowError(
                    "UNKNOWN_DEPENDENCY",
                    "step %r depends on unknown step %r" % (s["id"], d),
                    s["id"])

    # Cycles (iterative Kahn's algorithm to avoid recursion limits).
    indegree = {s["id"]: len(s["depends_on"]) for s in steps}
    children = {s["id"]: [] for s in steps}
    for s in steps:
        for d in s["depends_on"]:
            children[d].append(s["id"])
    ready = [sid for sid in indegree if indegree[sid] == 0]
    processed = 0
    while ready:
        u = ready.pop()
        processed += 1
        for v in children[u]:
            indegree[v] -= 1
            if indegree[v] == 0:
                ready.append(v)
    if processed != len(steps):
        raise WorkflowError("CYCLIC_DEPENDENCY",
                            "workflow contains a dependency cycle",
                            None)

    return steps


# ---------------------------------------------------------------------------
# Reference resolution and condition evaluation
# ---------------------------------------------------------------------------

class ExecutionContext:
    def __init__(self, steps):
        self.steps = steps
        self.step_by_id = {s["id"]: s for s in steps}
        self.results = {}
        self.skipped = set()
        self._ancestor_cache = {}

    def ancestors(self, sid):
        cached = self._ancestor_cache.get(sid)
        if cached is not None:
            return cached
        # Iterative post-order traversal (no recursion-limit issues).
        stack = [(sid, False)]
        while stack:
            node, expanded = stack.pop()
            if node in self._ancestor_cache:
                continue
            if expanded:
                out = set()
                for d in self.step_by_id[node]["depends_on"]:
                    out.add(d)
                    out |= self._ancestor_cache.get(d, set())
                self._ancestor_cache[node] = out
            else:
                stack.append((node, True))
                for d in self.step_by_id[node]["depends_on"]:
                    if d not in self._ancestor_cache:
                        stack.append((d, False))
        return self._ancestor_cache[sid]


_ARRAY_INDEX_RE = None


def _init_array_regex():
    global _ARRAY_INDEX_RE
    import re
    _ARRAY_INDEX_RE = re.compile(r"[0-9]+")


def resolve_ref(ref, ctx, current_step_id):
    parts = ref.split(".")
    target = parts[0]
    path = parts[1:]

    if target not in ctx.step_by_id:
        raise WorkflowError("INVALID_REFERENCE",
                            "reference to unknown step %r" % (target,),
                            current_step_id)
    if target == current_step_id:
        raise WorkflowError("INVALID_REFERENCE",
                            "step %r references itself" % (current_step_id,),
                            current_step_id)
    if target not in ctx.ancestors(current_step_id):
        raise WorkflowError(
            "INVALID_REFERENCE",
            "step %r is not a dependency of %r"
            % (target, current_step_id),
            current_step_id)
    if target in ctx.skipped:
        raise WorkflowError(
            "SKIPPED_REFERENCE",
            "reference to skipped step %r" % (target,),
            current_step_id)

    value = ctx.results[target]
    for seg in path:
        if isinstance(value, dict):
            if seg not in value:
                raise WorkflowError(
                    "INVALID_REFERENCE",
                    "reference path segment %r not found" % (seg,),
                    current_step_id)
            value = value[seg]
        elif isinstance(value, list):
            if not _ARRAY_INDEX_RE.fullmatch(seg):
                raise WorkflowError(
                    "INVALID_REFERENCE",
                    "array index %r is not a non-negative integer" % (seg,),
                    current_step_id)
            idx = int(seg)
            if idx >= len(value):
                raise WorkflowError(
                    "INVALID_REFERENCE",
                    "array index %d is out of bounds" % idx,
                    current_step_id)
            value = value[idx]
        else:
            raise WorkflowError(
                "INVALID_REFERENCE",
                "cannot index into a non-container value",
                current_step_id)
    return value


def resolve_values(v, ctx, current_step_id):
    if isinstance(v, dict):
        if is_ref_object(v):
            return resolve_ref(v["$ref"], ctx, current_step_id)
        return {k: resolve_values(x, ctx, current_step_id)
                for k, x in v.items()}
    if isinstance(v, list):
        return [resolve_values(x, ctx, current_step_id) for x in v]
    return v


def condition_compare(opname, left, right):
    if opname == "eq":
        return struct_eq(left, right)
    if opname == "ne":
        return not struct_eq(left, right)
    c = cmp_json(left, right)
    if opname == "gt":
        return c > 0
    if opname == "gte":
        return c >= 0
    if opname == "lt":
        return c < 0
    return c <= 0


def eval_condition_value(cond, ctx, current_step_id):
    if isinstance(cond, dict):
        if is_ref_object(cond):
            return resolve_ref(cond["$ref"], ctx, current_step_id)
        if "op" in cond and isinstance(cond["op"], str):
            opname = cond["op"]
            if opname in COMPARE_OPS:
                if "left" not in cond or "right" not in cond:
                    raise WorkflowError(
                        "INVALID_CONDITION",
                        "comparison condition requires 'left' and 'right'",
                        current_step_id)
                left = resolve_values(cond["left"], ctx, current_step_id)
                right = resolve_values(cond["right"], ctx, current_step_id)
                return condition_compare(opname, left, right)
            if opname in ("all", "any"):
                subs = cond.get("conditions")
                if not isinstance(subs, list):
                    raise WorkflowError(
                        "INVALID_CONDITION",
                        "compound condition requires a 'conditions' array",
                        current_step_id)
                values = [eval_condition(s, ctx, current_step_id)
                          for s in subs]
                return all(values) if opname == "all" else any(values)
            if opname == "not":
                if "condition" not in cond:
                    raise WorkflowError(
                        "INVALID_CONDITION",
                        "'not' condition requires 'condition'",
                        current_step_id)
                return not eval_condition(cond["condition"], ctx,
                                          current_step_id)
            raise WorkflowError(
                "INVALID_CONDITION",
                "unknown condition op %r" % (opname,),
                current_step_id)
        return cond
    return cond


def eval_condition(cond, ctx, current_step_id):
    value = eval_condition_value(cond, ctx, current_step_id)
    if type(value) is not bool:
        raise WorkflowError(
            "INVALID_CONDITION",
            "condition did not evaluate to a boolean",
            current_step_id)
    return value


# ---------------------------------------------------------------------------
# Execution
# ---------------------------------------------------------------------------

def _run_op(opname, args, ctx, step_id, counters):
    if opname == "fail_until":
        check_args(args, {"failures", "value"}, {"failures", "value"})
        failures = as_i64(args["failures"], "failures")
        if failures < 0 or failures > 10:
            raise invalid_arg("failures must be in 0..10")
        attempt = counters.get(step_id, 0) + 1
        counters[step_id] = attempt
        if attempt <= failures:
            raise WorkflowError("TRANSIENT_FAILURE",
                                "transient failure attempt %d" % attempt)
        return args["value"]

    handler = OP_HANDLERS[opname]
    return handler(args)


def _finished_results(ctx, order):
    return {sid: ctx.results[sid] for sid in order}


def execute_workflow(steps):
    ctx = ExecutionContext(steps)
    order = []
    skipped = []
    done = set()
    counters = {}

    try:
        while len(done) < len(steps):
            picked = None
            for s in steps:
                if s["id"] in done:
                    continue
                if all(d in done for d in s["depends_on"]):
                    picked = s
                    break
            if picked is None:
                # Structural validation guarantees this cannot happen.
                raise InternalError("no ready step found; cycle in DAG")

            sid = picked["id"]

            # Condition.
            run_step = True
            if picked["has_when"]:
                run_step = eval_condition(picked["when"], ctx, sid)

            if not run_step:
                ctx.results[sid] = None
                ctx.skipped.add(sid)
                order.append(sid)
                skipped.append(sid)
                done.add(sid)
                continue

            # Resolve args.
            args = resolve_values(picked["args"], ctx, sid)

            max_attempts = picked["max_attempts"]
            value = None
            for attempt in range(1, max_attempts + 1):
                try:
                    value = _run_op(picked["op"], args, ctx, sid, counters)
                    break
                except WorkflowError as e:
                    if e.code == "TRANSIENT_FAILURE":
                        if attempt < max_attempts:
                            continue
                        raise WorkflowError(
                            "STEP_FAILED",
                            "step %r failed after %d attempt(s)"
                            % (sid, max_attempts),
                            sid)
                    if e.step is None:
                        e.step = sid
                    raise

            ctx.results[sid] = value
            order.append(sid)
            done.add(sid)

        return {
            "status": "completed",
            "results": _finished_results(ctx, order),
            "execution_order": order,
            "skipped": skipped,
            "error": None,
        }
    except WorkflowError as e:
        if e.step is None:
            e.step = picked["id"] if picked is not None else None
        return failed_response(
            e.code, e.step, e.message,
            results=_finished_results(ctx, order),
            order=order,
            skipped=skipped)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def failed_response(code, step, message, results=None, order=None,
                    skipped=None):
    return {
        "status": "failed",
        "results": results if results is not None else {},
        "execution_order": order if order is not None else [],
        "skipped": skipped if skipped is not None else [],
        "error": {
            "code": code,
            "step": step,
            "message": message,
        },
    }


def emit(response):
    try:
        text = json.dumps(response, ensure_ascii=False,
                          separators=(",", ":"), allow_nan=False)
    except Exception:
        try:
            text = json.dumps(response, ensure_ascii=True,
                              separators=(",", ":"), allow_nan=False,
                              default=str)
        except Exception:
            text = ('{"status":"failed","results":{},'
                    '"execution_order":[],"skipped":[],"error":'
                    '{"code":"INTERNAL_ERROR","step":null,'
                    '"message":"response serialization failed"}}')
    try:
        sys.stdout.write(text + "\n")
        sys.stdout.flush()
    except UnicodeEncodeError:
        safe = json.dumps(response, ensure_ascii=True,
                          separators=(",", ":"), allow_nan=False,
                          default=str)
        sys.stdout.write(safe + "\n")
        sys.stdout.flush()


def _reject_constant(value):
    raise ValueError("non-standard JSON constant: %s" % (value,))


def read_input(argv):
    if len(argv) != 2 or argv[0] != "run":
        raise WorkflowError(
            "INVALID_WORKFLOW",
            "usage: workflow run <workflow.json> | workflow run -",
            None)
    path = argv[1]
    try:
        if path == "-":
            data = sys.stdin.buffer.read()
        else:
            with open(path, "rb") as f:
                data = f.read()
    except OSError as e:
        raise WorkflowError("INVALID_WORKFLOW",
                            "cannot read input: %s" % (e,), None)
    try:
        return data.decode("utf-8-sig")
    except UnicodeDecodeError as e:
        raise WorkflowError("INVALID_WORKFLOW",
                            "input is not valid UTF-8: %s" % (e,), None)


def _check_json_value(v):
    """Reject values that standard JSON cannot represent faithfully."""
    if isinstance(v, dict):
        for k, x in v.items():
            try:
                k.encode("utf-8")
            except UnicodeEncodeError:
                raise workflow_error("invalid Unicode in object key")
            _check_json_value(x)
    elif isinstance(v, list):
        for x in v:
            _check_json_value(x)
    elif isinstance(v, str):
        try:
            v.encode("utf-8")
        except UnicodeEncodeError:
            raise workflow_error("invalid Unicode in string")
    elif isinstance(v, float):
        if math.isnan(v) or math.isinf(v):
            raise workflow_error("non-finite JSON number")


def parse_document(text):
    try:
        doc = json.loads(text, parse_constant=_reject_constant)
        _check_json_value(doc)
    except WorkflowError:
        raise
    except (ValueError, TypeError, RecursionError) as e:
        raise WorkflowError("INVALID_WORKFLOW",
                            "invalid JSON: %s" % (e,), None)
    return doc


def main(argv):
    try:
        text = read_input(argv)
        doc = parse_document(text)
        steps = validate_document(doc)
    except WorkflowError as e:
        emit(failed_response(e.code, e.step, e.message))
        return exit_code_for(e.code)
    except InternalError as e:
        emit(failed_response("INTERNAL_ERROR", None, str(e)))
        return 3
    except Exception as e:  # pragma: no cover - safety net
        emit(failed_response("INTERNAL_ERROR", None,
                             "internal error: %s" % (e,)))
        return 3

    try:
        response = execute_workflow(steps)
    except InternalError as e:
        emit(failed_response("INTERNAL_ERROR", None, str(e)))
        return 3
    except Exception as e:  # pragma: no cover - safety net
        emit(failed_response("INTERNAL_ERROR", None,
                             "internal error: %s" % (e,)))
        return 3

    emit(response)
    if response["status"] == "completed":
        return 0
    return exit_code_for(response["error"]["code"])


def run():
    # Regexes are initialized lazily to keep module import cheap.
    _init_regex()
    _init_array_regex()
    sys.exit(main(sys.argv[1:]))


if __name__ == "__main__":
    run()
