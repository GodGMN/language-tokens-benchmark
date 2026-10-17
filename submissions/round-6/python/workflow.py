#!/usr/bin/env python3
"""Declarative workflow execution engine CLI.

Implements the contract in SPEC.md:

    python3 workflow.py run <workflow.json>
    python3 workflow.py run -          # JSON document on stdin

stdout always receives exactly one JSON document; diagnostics go to stderr.
"""

import json
import re
import sys
from decimal import Decimal, InvalidOperation
from functools import cmp_to_key

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

INT64_MIN = -9223372036854775808
INT64_MAX = 9223372036854775807

ID_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*\Z")

MATH_BINARY = ("add", "subtract", "multiply", "divide", "mod")

OP_NAMES = frozenset(
    MATH_BINARY
    + (
        "sum",
        "min",
        "max",
        "range",
        "concat",
        "split",
        "join",
        "uppercase",
        "lowercase",
        "trim",
        "replace",
        "length",
        "map",
        "filter",
        "reduce",
        "sort",
        "reverse",
        "unique",
        "slice",
        "flatten",
        "get",
        "set",
        "keys",
        "values",
        "merge",
        "pick",
        "omit",
        "constant",
        "equals",
        "compare",
        "and",
        "or",
        "not",
        "type",
        "fail_until",
    )
)

COMPARISON_OPS = frozenset(("eq", "ne", "gt", "gte", "lt", "lte"))
TYPE_NAMES = frozenset(("null", "boolean", "number", "string", "array", "object"))
RANK = {"number": 0, "string": 1, "boolean": 2, "null": 3, "array": 4, "object": 5}


# ---------------------------------------------------------------------------
# Errors
# ---------------------------------------------------------------------------


class EngineError(Exception):
    """A failure that maps directly onto an error code in the response."""

    def __init__(self, code, step=None, message=""):
        super().__init__(message)
        self.code = code
        self.step = step
        self.message = message or code


class OpError(Exception):
    """An operation failure (runtime, attributed to the current step)."""

    def __init__(self, code, message=""):
        super().__init__(message)
        self.code = code
        self.message = message or code


# ---------------------------------------------------------------------------
# JSON numbers
# ---------------------------------------------------------------------------


class RawNum:
    """A JSON number literal that is not an integer literal (fraction/exponent).

    Kept verbatim so it can be echoed back unchanged, and compared numerically
    with other numbers for structural equality / ordering.
    """

    __slots__ = ("lit", "dec")

    def __init__(self, lit):
        self.lit = lit
        self.dec = Decimal(lit)

    def __repr__(self):  # pragma: no cover - debugging aid
        return "RawNum(%r)" % self.lit


def _parse_float(lit):
    try:
        return RawNum(lit)
    except InvalidOperation:  # pragma: no cover - json already vetted syntax
        raise ValueError("invalid number literal")


def _parse_constant(tok):
    raise ValueError("non-finite number literal: %s" % tok)


# ---------------------------------------------------------------------------
# Value helpers
# ---------------------------------------------------------------------------


def num_value(v):
    return v if type(v) is int else v.dec


def is_i64(v):
    return type(v) is int and INT64_MIN <= v <= INT64_MAX


def type_name(v):
    if v is None:
        return "null"
    t = type(v)
    if t is bool:
        return "boolean"
    if t is int or isinstance(v, RawNum):
        return "number"
    if t is str:
        return "string"
    if t is list:
        return "array"
    if t is dict:
        return "object"
    raise EngineError("INTERNAL_ERROR", None, "unsupported value type")


def structural_equals(a, b):
    ta = type_name(a)
    tb = type_name(b)
    if ta != tb:
        return False
    if ta == "number":
        return num_value(a) == num_value(b)
    if ta == "null":
        return True
    if ta in ("boolean", "string"):
        return a == b
    if ta == "array":
        if len(a) != len(b):
            return False
        return all(structural_equals(x, y) for x, y in zip(a, b))
    # object
    if len(a) != len(b):
        return False
    if set(a.keys()) != set(b.keys()):
        return False
    return all(structural_equals(a[k], b[k]) for k in a)


def _cmp(x, y):
    return (x > y) - (x < y)


def compare_values(a, b):
    """Total order used by `sort` and `compare`."""
    ta = type_name(a)
    tb = type_name(b)
    if ta != tb:
        return -1 if RANK[ta] < RANK[tb] else 1
    if ta == "number":
        return _cmp(num_value(a), num_value(b))
    if ta == "string":
        return _cmp(a.encode("utf-8"), b.encode("utf-8"))
    if ta == "boolean":
        return _cmp(int(a), int(b))
    if ta == "null":
        return 0
    if ta == "array":
        for x, y in zip(a, b):
            c = compare_values(x, y)
            if c:
                return c
        return _cmp(len(a), len(b))
    # object
    ka = sorted(a.keys(), key=lambda s: s.encode("utf-8"))
    kb = sorted(b.keys(), key=lambda s: s.encode("utf-8"))
    for x, y in zip(ka, kb):
        c = _cmp(x.encode("utf-8"), y.encode("utf-8"))
        if c:
            return c
    c = _cmp(len(ka), len(kb))
    if c:
        return c
    for k in ka:
        c = compare_values(a[k], b[k])
        if c:
            return c
    return 0


def canonical_key(v):
    """Hashable key consistent with structural_equals."""
    t = type_name(v)
    if t == "number":
        return ("number", num_value(v))
    if t == "null":
        return ("null",)
    if t == "boolean":
        return ("boolean", v)
    if t == "string":
        return ("string", v)
    if t == "array":
        return ("array", tuple(canonical_key(x) for x in v))
    return (
        "object",
        tuple(
            (k, canonical_key(v[k]))
            for k in sorted(v.keys(), key=lambda s: s.encode("utf-8"))
        ),
    )


def truthy(v):
    t = type_name(v)
    if t == "null":
        return False
    if t == "boolean":
        return v
    if t == "number":
        return num_value(v) != 0
    if t == "string":
        return v != ""
    return len(v) != 0


# ---------------------------------------------------------------------------
# JSON serialization
# ---------------------------------------------------------------------------


def dump_json(v):
    out = []
    _emit(v, out)
    return "".join(out)


def _emit(v, out):
    if v is None:
        out.append("null")
    elif v is True:
        out.append("true")
    elif v is False:
        out.append("false")
    elif type(v) is int:
        out.append(str(v))
    elif isinstance(v, RawNum):
        out.append(v.lit)
    elif type(v) is str:
        out.append(json.dumps(v, ensure_ascii=False))
    elif type(v) is list:
        out.append("[")
        for i, item in enumerate(v):
            if i:
                out.append(",")
            _emit(item, out)
        out.append("]")
    elif type(v) is dict:
        out.append("{")
        first = True
        for k, item in v.items():
            if not first:
                out.append(",")
            first = False
            out.append(json.dumps(k, ensure_ascii=False))
            out.append(":")
            _emit(item, out)
        out.append("}")
    else:  # pragma: no cover - defensive
        raise EngineError("INTERNAL_ERROR", None, "cannot serialize value")


# ---------------------------------------------------------------------------
# i64 arithmetic
# ---------------------------------------------------------------------------


def _checked(r):
    if r < INT64_MIN or r > INT64_MAX:
        raise OpError("ARITHMETIC_OVERFLOW", "i64 overflow")
    return r


def arith_add(a, b):
    return _checked(a + b)


def arith_subtract(a, b):
    return _checked(a - b)


def arith_multiply(a, b):
    return _checked(a * b)


def arith_divide(a, b):
    if b == 0:
        raise OpError("DIVISION_BY_ZERO", "division by zero")
    if a == INT64_MIN and b == -1:
        raise OpError("ARITHMETIC_OVERFLOW", "i64 overflow")
    q = abs(a) // abs(b)
    return q if (a < 0) == (b < 0) else -q


def arith_mod(a, b):
    if b == 0:
        raise OpError("DIVISION_BY_ZERO", "modulo by zero")
    if a == INT64_MIN and b == -1:
        raise OpError("ARITHMETIC_OVERFLOW", "i64 overflow")
    q = arith_divide(a, b)
    return a - q * b


ARITH = {
    "add": arith_add,
    "subtract": arith_subtract,
    "multiply": arith_multiply,
    "divide": arith_divide,
    "mod": arith_mod,
}


# ---------------------------------------------------------------------------
# Argument helpers for operations
# ---------------------------------------------------------------------------


def _keys(args, allowed):
    for k in args:
        if k not in allowed:
            raise OpError("INVALID_ARG", "unknown argument %r" % (k,))


def _require(args, name):
    if name not in args:
        raise OpError("INVALID_ARG", "missing argument %r" % (name,))
    return args[name]


def _arg_i64(args, name):
    v = _require(args, name)
    if not is_i64(v):
        raise OpError("INVALID_ARG", "argument %r must be an i64 integer" % (name,))
    return v


def _arg_str(args, name):
    v = _require(args, name)
    if type(v) is not str:
        raise OpError("INVALID_ARG", "argument %r must be a string" % (name,))
    return v


def _arg_list(args, name):
    v = _require(args, name)
    if type(v) is not list:
        raise OpError("INVALID_ARG", "argument %r must be an array" % (name,))
    return v


def _arg_obj(args, name):
    v = _require(args, name)
    if type(v) is not dict:
        raise OpError("INVALID_ARG", "argument %r must be an object" % (name,))
    return v


def _value_i64(v, what):
    if not is_i64(v):
        raise OpError("INVALID_ARG", "%s must be an i64 integer" % (what,))
    return v


# ---------------------------------------------------------------------------
# Operations: math
# ---------------------------------------------------------------------------


def op_add(args, ctx, step):
    _keys(args, ("a", "b"))
    return arith_add(_arg_i64(args, "a"), _arg_i64(args, "b"))


def op_subtract(args, ctx, step):
    _keys(args, ("a", "b"))
    return arith_subtract(_arg_i64(args, "a"), _arg_i64(args, "b"))


def op_multiply(args, ctx, step):
    _keys(args, ("a", "b"))
    return arith_multiply(_arg_i64(args, "a"), _arg_i64(args, "b"))


def op_divide(args, ctx, step):
    _keys(args, ("a", "b"))
    return arith_divide(_arg_i64(args, "a"), _arg_i64(args, "b"))


def op_mod(args, ctx, step):
    _keys(args, ("a", "b"))
    return arith_mod(_arg_i64(args, "a"), _arg_i64(args, "b"))


def _i64_array(args, name="input"):
    arr = _arg_list(args, name)
    for x in arr:
        if not is_i64(x):
            raise OpError("INVALID_ARG", "%s must contain only i64 integers" % (name,))
    return arr


def op_sum(args, ctx, step):
    _keys(args, ("input",))
    total = 0
    for x in _i64_array(args):
        total += x
    return _checked(total)


def op_min(args, ctx, step):
    _keys(args, ("input",))
    arr = _i64_array(args)
    if not arr:
        raise OpError("EMPTY_ARRAY", "min of empty array")
    return min(arr)


def op_max(args, ctx, step):
    _keys(args, ("input",))
    arr = _i64_array(args)
    if not arr:
        raise OpError("EMPTY_ARRAY", "max of empty array")
    return max(arr)


def op_range(args, ctx, step):
    _keys(args, ("start", "end"))
    start = _arg_i64(args, "start")
    end = _arg_i64(args, "end")
    if start > end:
        return []
    return list(range(start, end + 1))


# ---------------------------------------------------------------------------
# Operations: strings
# ---------------------------------------------------------------------------

_UPPER_TABLE = {c: c - 32 for c in range(ord("a"), ord("z") + 1)}
_LOWER_TABLE = {c: c + 32 for c in range(ord("A"), ord("Z") + 1)}
_ASCII_WS = " \t\n\r\f\v"


def op_concat(args, ctx, step):
    _keys(args, ("a", "b"))
    return _arg_str(args, "a") + _arg_str(args, "b")


def op_split(args, ctx, step):
    _keys(args, ("input", "sep"))
    text = _arg_str(args, "input")
    sep = _arg_str(args, "sep")
    if sep == "":
        return list(text)
    return text.split(sep)


def op_join(args, ctx, step):
    _keys(args, ("input", "sep"))
    arr = _arg_list(args, "input")
    for x in arr:
        if type(x) is not str:
            raise OpError("INVALID_ARG", "join input must contain only strings")
    sep = _arg_str(args, "sep")
    return sep.join(arr)


def op_uppercase(args, ctx, step):
    _keys(args, ("input",))
    return _arg_str(args, "input").translate(_UPPER_TABLE)


def op_lowercase(args, ctx, step):
    _keys(args, ("input",))
    return _arg_str(args, "input").translate(_LOWER_TABLE)


def op_trim(args, ctx, step):
    _keys(args, ("input",))
    return _arg_str(args, "input").strip(_ASCII_WS)


def op_replace(args, ctx, step):
    _keys(args, ("input", "search", "replace"))
    text = _arg_str(args, "input")
    search = _arg_str(args, "search")
    repl = _arg_str(args, "replace")
    if search == "":
        raise OpError("INVALID_ARG", "replace search must not be empty")
    return text.replace(search, repl)


def op_length(args, ctx, step):
    _keys(args, ("input",))
    return len(_arg_str(args, "input"))


# ---------------------------------------------------------------------------
# Operations: arrays (map / filter transform compilation)
# ---------------------------------------------------------------------------


def _is_self(v):
    return type(v) is dict and len(v) == 1 and v.get("$self") is True


def compile_transform(tr):
    if type(tr) is not dict:
        raise OpError("INVALID_ARG", "transform must be an object")
    typ = tr.get("type")
    if type(typ) is not str:
        raise OpError("INVALID_ARG", "transform type must be a string")

    if typ == "constant":
        _keys(tr, ("type", "value"))
        if "value" not in tr:
            raise OpError("INVALID_ARG", "constant transform requires value")
        value = tr["value"]

        def fn(_x):
            return value

        return fn

    if typ == "op":
        _keys(tr, ("type", "op", "args"))
        op = tr.get("op")
        if type(op) is not str or op not in MATH_BINARY:
            raise OpError("INVALID_ARG", "unsupported transform op")
        targs = tr.get("args")
        if type(targs) is not dict:
            raise OpError("INVALID_ARG", "transform args must be an object")
        _keys(targs, ("a", "b"))
        if "a" not in targs or "b" not in targs:
            raise OpError("INVALID_ARG", "transform args require a and b")
        ra, rb = targs["a"], targs["b"]
        self_a = _is_self(ra)
        self_b = _is_self(rb)
        if not self_a:
            _value_i64(ra, "transform operand a")
        if not self_b:
            _value_i64(rb, "transform operand b")
        f = ARITH[op]

        def fn(x):
            a = _value_i64(x, "element") if self_a else ra
            b = _value_i64(x, "element") if self_b else rb
            return f(a, b)

        return fn

    if typ in MATH_BINARY:
        _keys(tr, ("type", "value"))
        if "value" not in tr:
            raise OpError("INVALID_ARG", "numeric transform requires value")
        operand = _value_i64(tr["value"], "transform value")
        f = ARITH[typ]

        def fn(x):
            return f(_value_i64(x, "element"), operand)

        return fn

    if typ == "negate":
        _keys(tr, ("type",))

        def fn(x):
            x = _value_i64(x, "element")
            if x == INT64_MIN:
                raise OpError("ARITHMETIC_OVERFLOW", "i64 overflow")
            return -x

        return fn

    if typ == "abs":
        _keys(tr, ("type",))

        def fn(x):
            x = _value_i64(x, "element")
            if x == INT64_MIN:
                raise OpError("ARITHMETIC_OVERFLOW", "i64 overflow")
            return abs(x)

        return fn

    if typ == "stringify":
        _keys(tr, ("type",))

        def fn(x):
            return dump_json(x)

        return fn

    if typ == "type":
        _keys(tr, ("type",))

        def fn(x):
            return type_name(x)

        return fn

    raise OpError("INVALID_ARG", "unsupported transform type %r" % (typ,))


def compile_predicate(pr):
    # Accept the compact string form as well (e.g. {"predicate": "even"}),
    # which the PRD shows for simple parameterless predicates.
    if type(pr) is str:
        pr = {"type": pr}
    if type(pr) is not dict:
        raise OpError("INVALID_ARG", "predicate must be an object")
    typ = pr.get("type")
    if type(typ) is not str:
        raise OpError("INVALID_ARG", "predicate type must be a string")

    if typ in ("even", "odd"):
        _keys(pr, ("type",))
        want_even = typ == "even"

        def fn(x):
            x = _value_i64(x, "element")
            r = arith_mod(x, 2)
            return (r == 0) if want_even else (r != 0)

        return fn

    if typ in ("gt", "gte", "lt", "lte"):
        _keys(pr, ("type", "value"))
        if "value" not in pr:
            raise OpError("INVALID_ARG", "comparison predicate requires value")
        operand = _value_i64(pr["value"], "predicate value")

        def fn(x):
            c = compare_values(x, operand)
            if typ == "gt":
                return c > 0
            if typ == "gte":
                return c >= 0
            if typ == "lt":
                return c < 0
            return c <= 0

        return fn

    if typ in ("eq", "ne"):
        _keys(pr, ("type", "value"))
        if "value" not in pr:
            raise OpError("INVALID_ARG", "comparison predicate requires value")
        operand = pr["value"]
        want = typ == "eq"

        def fn(x):
            eq = structural_equals(x, operand)
            return eq if want else not eq

        return fn

    if typ in ("truthy", "falsy"):
        _keys(pr, ("type",))
        want = typ == "truthy"

        def fn(x):
            t = truthy(x)
            return t if want else not t

        return fn

    if typ == "type_is":
        allowed = ("type", "value", "type_name", "target", "expected", "name")
        _keys(pr, allowed)
        target = None
        for k in ("value", "type_name", "target", "expected", "name"):
            if k in pr:
                target = pr[k]
                break
        if target is None:
            raise OpError("INVALID_ARG", "type_is predicate requires a type name")
        if type(target) is not str or target not in TYPE_NAMES:
            raise OpError("INVALID_ARG", "type_is predicate has invalid type name")

        def fn(x):
            return type_name(x) == target

        return fn

    # Duplicate-key tolerant form: {"type": "type_is", "type": "number"} is
    # impossible in JSON, parsers keep the last key, leaving {"type":"number"}.
    if typ in TYPE_NAMES:
        _keys(pr, ("type",))
        target = typ

        def fn(x):
            return type_name(x) == target

        return fn

    if typ == "string_contains":
        _keys(pr, ("type", "value"))
        if "value" not in pr:
            raise OpError("INVALID_ARG", "string_contains predicate requires value")
        needle = pr["value"]
        if type(needle) is not str:
            raise OpError("INVALID_ARG", "string_contains value must be a string")

        def fn(x):
            if type(x) is not str:
                raise OpError(
                    "INVALID_ARG", "string_contains predicate requires string elements"
                )
            return needle in x

        return fn

    raise OpError("INVALID_ARG", "unsupported predicate type %r" % (typ,))


def op_map(args, ctx, step):
    _keys(args, ("input", "transform"))
    arr = _arg_list(args, "input")
    if "transform" not in args:
        raise OpError("INVALID_ARG", "map requires transform")
    fn = compile_transform(args["transform"])
    return [fn(x) for x in arr]


def op_filter(args, ctx, step):
    _keys(args, ("input", "predicate"))
    arr = _arg_list(args, "input")
    if "predicate" not in args:
        raise OpError("INVALID_ARG", "filter requires predicate")
    fn = compile_predicate(args["predicate"])
    return [x for x in arr if fn(x)]


def op_reduce(args, ctx, step):
    _keys(args, ("input", "op", "initial"))
    arr = _i64_array(args)
    op = args.get("op")
    if type(op) is not str or op not in ("add", "subtract", "multiply"):
        raise OpError("INVALID_ARG", "reduce op must be add, subtract or multiply")
    if "initial" in args:
        acc = _arg_i64(args, "initial")
    else:
        acc = 1 if op == "multiply" else 0
    f = ARITH[op]
    for x in arr:
        acc = f(acc, x)
    return acc


def op_sort(args, ctx, step):
    _keys(args, ("input",))
    arr = _arg_list(args, "input")
    return sorted(arr, key=cmp_to_key(compare_values))


def op_reverse(args, ctx, step):
    _keys(args, ("input",))
    return list(reversed(_arg_list(args, "input")))


def op_unique(args, ctx, step):
    _keys(args, ("input",))
    seen = set()
    out = []
    for x in _arg_list(args, "input"):
        k = canonical_key(x)
        if k in seen:
            continue
        seen.add(k)
        out.append(x)
    return out


def op_slice(args, ctx, step):
    _keys(args, ("input", "start", "end"))
    arr = _arg_list(args, "input")
    start = _arg_i64(args, "start")
    if "end" in args:
        end = _arg_i64(args, "end")
    else:
        end = None
    return arr[start:end]


def op_flatten(args, ctx, step):
    _keys(args, ("input",))
    out = []
    for x in _arg_list(args, "input"):
        if type(x) is list:
            out.extend(x)
        else:
            out.append(x)
    return out


# ---------------------------------------------------------------------------
# Operations: objects
# ---------------------------------------------------------------------------


def _sorted_keys(obj):
    return sorted(obj.keys(), key=lambda s: s.encode("utf-8"))


def op_get(args, ctx, step):
    _keys(args, ("input", "key"))
    obj = _arg_obj(args, "input")
    key = _arg_str(args, "key")
    return obj.get(key)


def op_set(args, ctx, step):
    _keys(args, ("input", "key", "value"))
    obj = _arg_obj(args, "input")
    key = _arg_str(args, "key")
    if "value" not in args:
        raise OpError("INVALID_ARG", "set requires value")
    out = dict(obj)
    out[key] = args["value"]
    return out


def op_keys(args, ctx, step):
    _keys(args, ("input",))
    return _sorted_keys(_arg_obj(args, "input"))


def op_values(args, ctx, step):
    _keys(args, ("input",))
    obj = _arg_obj(args, "input")
    return [obj[k] for k in _sorted_keys(obj)]


def op_merge(args, ctx, step):
    _keys(args, ("a", "b"))
    a = _arg_obj(args, "a")
    b = _arg_obj(args, "b")
    out = dict(a)
    out.update(b)
    return out


def op_pick(args, ctx, step):
    _keys(args, ("input", "keys"))
    obj = _arg_obj(args, "input")
    keys = _arg_list(args, "keys")
    for k in keys:
        if type(k) is not str:
            raise OpError("INVALID_ARG", "pick keys must be strings")
    out = {}
    for k in sorted(set(keys), key=lambda s: s.encode("utf-8")):
        out[k] = obj.get(k)
    return out


def op_omit(args, ctx, step):
    _keys(args, ("input", "keys"))
    obj = _arg_obj(args, "input")
    keys = _arg_list(args, "keys")
    for k in keys:
        if type(k) is not str:
            raise OpError("INVALID_ARG", "omit keys must be strings")
    drop = set(keys)
    return {k: v for k, v in obj.items() if k not in drop}


# ---------------------------------------------------------------------------
# Operations: utilities
# ---------------------------------------------------------------------------


def op_constant(args, ctx, step):
    _keys(args, ("value",))
    if "value" not in args:
        raise OpError("INVALID_ARG", "constant requires value")
    return args["value"]


def op_equals(args, ctx, step):
    _keys(args, ("a", "b"))
    if "a" not in args or "b" not in args:
        raise OpError("INVALID_ARG", "equals requires a and b")
    return structural_equals(args["a"], args["b"])


def op_compare(args, ctx, step):
    _keys(args, ("a", "b", "op"))
    if "a" not in args or "b" not in args or "op" not in args:
        raise OpError("INVALID_ARG", "compare requires a, b and op")
    op = args["op"]
    if type(op) is not str or op not in ("gt", "gte", "lt", "lte"):
        raise OpError("INVALID_ARG", "compare op must be gt, gte, lt or lte")
    c = compare_values(args["a"], args["b"])
    if op == "gt":
        return c > 0
    if op == "gte":
        return c >= 0
    if op == "lt":
        return c < 0
    return c <= 0


def op_and(args, ctx, step):
    _keys(args, ("a", "b"))
    if "a" not in args or "b" not in args:
        raise OpError("INVALID_ARG", "and requires a and b")
    return truthy(args["a"]) and truthy(args["b"])


def op_or(args, ctx, step):
    _keys(args, ("a", "b"))
    if "a" not in args or "b" not in args:
        raise OpError("INVALID_ARG", "or requires a and b")
    return truthy(args["a"]) or truthy(args["b"])


def op_not(args, ctx, step):
    _keys(args, ("a",))
    if "a" not in args:
        raise OpError("INVALID_ARG", "not requires a")
    return not truthy(args["a"])


def op_type(args, ctx, step):
    _keys(args, ("value",))
    if "value" not in args:
        raise OpError("INVALID_ARG", "type requires value")
    return type_name(args["value"])


def op_fail_until(args, ctx, step):
    _keys(args, ("failures", "value"))
    failures = _arg_i64(args, "failures")
    if failures < 0 or failures > 10:
        raise OpError("INVALID_ARG", "failures must be between 0 and 10")
    if "value" not in args:
        raise OpError("INVALID_ARG", "fail_until requires value")
    n = ctx.counters.get(step.id, 0) + 1
    ctx.counters[step.id] = n
    if n <= failures:
        raise OpError("TRANSIENT_FAILURE", "transient failure")
    return args["value"]


OPS = {
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
    "fail_until": op_fail_until,
}


# ---------------------------------------------------------------------------
# References
# ---------------------------------------------------------------------------


def is_ref(v):
    return (
        type(v) is dict
        and len(v) == 1
        and "$ref" in v
        and type(v["$ref"]) is str
    )


def resolve_ref(ctx, step_id, path):
    parts = path.split(".")
    target = parts[0]
    if target not in ctx.by_id:
        raise EngineError("INVALID_REFERENCE", step_id, "unknown reference target")
    if target == step_id:
        raise EngineError("INVALID_REFERENCE", step_id, "self reference")
    if target not in ctx.ancestors[step_id]:
        raise EngineError(
            "INVALID_REFERENCE", step_id, "reference to a non-dependency step"
        )
    if target in ctx.skipped_set:
        raise EngineError("SKIPPED_REFERENCE", step_id, "reference to skipped step")
    if target not in ctx.results:
        raise EngineError("INVALID_REFERENCE", step_id, "reference to unfinished step")
    value = ctx.results[target]
    for seg in parts[1:]:
        if type(value) is dict:
            if seg not in value:
                raise EngineError("INVALID_REFERENCE", step_id, "missing object key")
            value = value[seg]
        elif type(value) is list:
            if seg == "" or any(c < "0" or c > "9" for c in seg):
                raise EngineError("INVALID_REFERENCE", step_id, "invalid array index")
            idx = int(seg)
            if idx >= len(value):
                raise EngineError("INVALID_REFERENCE", step_id, "array index out of range")
            value = value[idx]
        else:
            raise EngineError("INVALID_REFERENCE", step_id, "cannot index value")
    return value


def resolve_value(ctx, step_id, value):
    if is_ref(value):
        return resolve_ref(ctx, step_id, value["$ref"])
    if type(value) is list:
        return [resolve_value(ctx, step_id, x) for x in value]
    if type(value) is dict:
        return {k: resolve_value(ctx, step_id, v) for k, v in value.items()}
    return value


# ---------------------------------------------------------------------------
# Conditions
# ---------------------------------------------------------------------------


def validate_condition(cond, step_id):
    if type(cond) is bool:
        return
    if type(cond) is not dict:
        raise EngineError("INVALID_WORKFLOW", step_id, "condition must be an object")
    if is_ref(cond):
        return
    if "op" not in cond:
        raise EngineError("INVALID_WORKFLOW", step_id, "condition requires op")
    op = cond["op"]
    if type(op) is not str:
        raise EngineError("INVALID_WORKFLOW", step_id, "condition op must be a string")
    if op in COMPARISON_OPS:
        if set(cond.keys()) != {"op", "left", "right"}:
            raise EngineError(
                "INVALID_WORKFLOW",
                step_id,
                "comparison condition requires exactly op, left, right",
            )
    elif op in ("all", "any"):
        if set(cond.keys()) != {"op", "conditions"}:
            raise EngineError(
                "INVALID_WORKFLOW",
                step_id,
                "compound condition requires exactly op, conditions",
            )
        sub = cond["conditions"]
        if type(sub) is not list:
            raise EngineError(
                "INVALID_WORKFLOW", step_id, "conditions must be an array"
            )
        for c in sub:
            validate_condition(c, step_id)
    elif op == "not":
        if set(cond.keys()) != {"op", "condition"}:
            raise EngineError(
                "INVALID_WORKFLOW",
                step_id,
                "not condition requires exactly op, condition",
            )
        validate_condition(cond["condition"], step_id)
    else:
        raise EngineError("INVALID_WORKFLOW", step_id, "unknown condition op %r" % (op,))


def eval_condition(ctx, step, cond):
    if type(cond) is bool:
        return cond
    if is_ref(cond):
        value = resolve_ref(ctx, step.id, cond["$ref"])
        if type(value) is not bool:
            raise EngineError(
                "INVALID_CONDITION", step.id, "condition did not evaluate to boolean"
            )
        return value
    op = cond["op"]
    if op == "all":
        for c in cond["conditions"]:
            if not eval_condition(ctx, step, c):
                return False
        return True
    if op == "any":
        for c in cond["conditions"]:
            if eval_condition(ctx, step, c):
                return True
        return False
    if op == "not":
        return not eval_condition(ctx, step, cond["condition"])
    left = resolve_value(ctx, step.id, cond["left"])
    right = resolve_value(ctx, step.id, cond["right"])
    if op == "eq":
        return structural_equals(left, right)
    if op == "ne":
        return not structural_equals(left, right)
    c = compare_values(left, right)
    if op == "gt":
        return c > 0
    if op == "gte":
        return c >= 0
    if op == "lt":
        return c < 0
    return c <= 0  # lte


# ---------------------------------------------------------------------------
# Workflow validation
# ---------------------------------------------------------------------------


class Step:
    __slots__ = ("id", "op", "args", "depends_on", "when", "max_attempts")

    def __init__(self, sid, op, args, depends_on, when, max_attempts):
        self.id = sid
        self.op = op
        self.args = args
        self.depends_on = depends_on
        self.when = when
        self.max_attempts = max_attempts


def validate_workflow(doc):
    if type(doc) is not dict:
        raise EngineError("INVALID_WORKFLOW", None, "workflow must be an object")

    for k in doc:
        if k not in ("version", "steps"):
            raise EngineError(
                "INVALID_WORKFLOW", None, "unknown top-level field %r" % (k,)
            )

    if "version" not in doc:
        raise EngineError("INVALID_WORKFLOW", None, "missing version")
    version = doc["version"]
    if type(version) is not int or version != 1:
        raise EngineError("INVALID_WORKFLOW", None, "version must be the integer 1")

    if "steps" not in doc:
        raise EngineError("INVALID_WORKFLOW", None, "missing steps")
    raw_steps = doc["steps"]
    if type(raw_steps) is not list:
        raise EngineError("INVALID_WORKFLOW", None, "steps must be an array")

    steps = []
    seen = set()

    for rs in raw_steps:
        if type(rs) is not dict:
            raise EngineError("INVALID_WORKFLOW", None, "step must be an object")

        raw_id = rs.get("id")
        step_id = raw_id if type(raw_id) is str else None

        for k in rs:
            if k not in ("id", "op", "args", "depends_on", "when", "retry"):
                raise EngineError(
                    "INVALID_WORKFLOW", step_id, "unknown step field %r" % (k,)
                )

        if "id" not in rs:
            raise EngineError("INVALID_WORKFLOW", None, "step is missing id")
        if type(raw_id) is not str:
            raise EngineError("INVALID_WORKFLOW", None, "step id must be a string")
        if not ID_RE.match(raw_id):
            raise EngineError("INVALID_WORKFLOW", raw_id, "invalid step id")
        if raw_id in seen:
            raise EngineError("INVALID_WORKFLOW", raw_id, "duplicate step id")
        seen.add(raw_id)

        if "op" not in rs:
            raise EngineError("INVALID_WORKFLOW", raw_id, "step is missing op")
        op = rs["op"]
        if type(op) is not str:
            raise EngineError("INVALID_WORKFLOW", raw_id, "op must be a string")
        if op not in OP_NAMES:
            raise EngineError("INVALID_WORKFLOW", raw_id, "unknown op %r" % (op,))

        args = rs.get("args", {})
        if type(args) is not dict:
            raise EngineError("INVALID_WORKFLOW", raw_id, "args must be an object")

        depends_on = rs.get("depends_on", [])
        if type(depends_on) is not list:
            raise EngineError(
                "INVALID_WORKFLOW", raw_id, "depends_on must be an array"
            )
        for d in depends_on:
            if type(d) is not str:
                raise EngineError(
                    "INVALID_WORKFLOW", raw_id, "depends_on entries must be strings"
                )

        when = None
        if "when" in rs:
            when = rs["when"]
            validate_condition(when, raw_id)

        max_attempts = 1
        if "retry" in rs:
            retry = rs["retry"]
            if type(retry) is not dict:
                raise EngineError("INVALID_WORKFLOW", raw_id, "retry must be an object")
            for k in retry:
                if k != "max_attempts":
                    raise EngineError(
                        "INVALID_WORKFLOW", raw_id, "unknown retry field %r" % (k,)
                    )
            if "max_attempts" in retry:
                ma = retry["max_attempts"]
                if type(ma) is not int or ma < 1 or ma > 5:
                    raise EngineError(
                        "INVALID_WORKFLOW",
                        raw_id,
                        "max_attempts must be an integer between 1 and 5",
                    )
                max_attempts = ma

        steps.append(
            Step(raw_id, op, args, list(depends_on), when, max_attempts)
        )

    # dependency existence
    for s in steps:
        for d in s.depends_on:
            if d not in seen:
                raise EngineError(
                    "UNKNOWN_DEPENDENCY",
                    s.id,
                    "dependency %r does not exist" % (d,),
                )

    # cycle detection (Kahn) and ancestor closure
    order = _topological_order(steps)
    if order is None:
        raise EngineError("CYCLIC_DEPENDENCY", None, "dependency cycle detected")

    by_id = {s.id: s for s in steps}
    ancestors = {s.id: set() for s in steps}
    for sid in order:
        s = by_id[sid]
        acc = set()
        for d in s.depends_on:
            acc.add(d)
            acc |= ancestors[d]
        ancestors[sid] = acc

    return steps, ancestors


def _topological_order(steps):
    by_id = {s.id: s for s in steps}
    indeg = {s.id: 0 for s in steps}
    dependents = {s.id: [] for s in steps}
    for s in steps:
        deps = set(s.depends_on)
        indeg[s.id] = len(deps)
        for d in deps:
            dependents[d].append(s.id)
    ready = [s.id for s in steps if indeg[s.id] == 0]
    order = []
    while ready:
        sid = ready.pop(0)
        order.append(sid)
        for dep in dependents[sid]:
            indeg[dep] -= 1
            if indeg[dep] == 0:
                ready.append(dep)
    if len(order) != len(by_id):
        return None
    return order


# ---------------------------------------------------------------------------
# Execution
# ---------------------------------------------------------------------------


class Context:
    def __init__(self, steps, ancestors):
        self.steps = steps
        self.by_id = {s.id: s for s in steps}
        self.ancestors = ancestors
        self.results = {}
        self.skipped = []
        self.skipped_set = set()
        self.order = []
        self.counters = {}


def execute_step(ctx, step):
    if step.when is not None:
        if not eval_condition(ctx, step, step.when):
            ctx.results[step.id] = None
            ctx.skipped_set.add(step.id)
            ctx.skipped.append(step.id)
            return

    # Resolve references anywhere inside args (including the args object as a
    # whole, if it is itself a {"$ref": ...} document).
    args = resolve_value(ctx, step.id, step.args)
    if type(args) is not dict:
        raise EngineError(
            "INVALID_ARG", step.id, "resolved args are not an object"
        )
    handler = OPS[step.op]
    attempt = 0
    while True:
        attempt += 1
        try:
            value = handler(args, ctx, step)
        except OpError as exc:
            if exc.code == "TRANSIENT_FAILURE":
                if attempt < step.max_attempts:
                    continue
                raise EngineError(
                    "STEP_FAILED", step.id, "step failed after %d attempt(s)" % attempt
                )
            raise EngineError(exc.code, step.id, exc.message)
        ctx.results[step.id] = value
        return


def run_steps(ctx):
    finished = set()
    pending = list(ctx.steps)
    while pending:
        chosen = None
        chosen_idx = -1
        for i, s in enumerate(pending):
            if all(d in finished for d in s.depends_on):
                chosen = s
                chosen_idx = i
                break
        if chosen is None:  # pragma: no cover - cycles rejected earlier
            raise EngineError(
                "CYCLIC_DEPENDENCY", None, "dependency cycle during execution"
            )
        pending.pop(chosen_idx)
        execute_step(ctx, chosen)
        finished.add(chosen.id)
        ctx.order.append(chosen.id)


# ---------------------------------------------------------------------------
# Response construction
# ---------------------------------------------------------------------------


def build_response(ctx, error):
    results = {}
    for sid in ctx.order:
        if sid in ctx.skipped_set:
            results[sid] = None
        else:
            results[sid] = ctx.results.get(sid)
    return {
        "status": "completed" if error is None else "failed",
        "results": results,
        "execution_order": list(ctx.order),
        "skipped": list(ctx.skipped),
        "error": None
        if error is None
        else {
            "code": error.code,
            "step": error.step,
            "message": error.message,
        },
    }


def empty_response(error):
    return {
        "status": "failed" if error is not None else "completed",
        "results": {},
        "execution_order": [],
        "skipped": [],
        "error": None
        if error is None
        else {
            "code": error.code,
            "step": error.step,
            "message": error.message,
        },
    }


def exit_code_for(code):
    if code in ("INVALID_WORKFLOW", "UNKNOWN_DEPENDENCY", "CYCLIC_DEPENDENCY"):
        return 2
    return 1


def run_workflow(doc):
    steps, ancestors = validate_workflow(doc)
    ctx = Context(steps, ancestors)
    try:
        run_steps(ctx)
    except EngineError as exc:
        return build_response(ctx, exc), exit_code_for(exc.code)
    return build_response(ctx, None), 0


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def load_document(path):
    if path == "-":
        try:
            data = sys.stdin.buffer.read()
        except Exception:
            raise EngineError("INVALID_WORKFLOW", None, "cannot read stdin")
    else:
        try:
            with open(path, "rb") as fh:
                data = fh.read()
        except OSError as exc:
            raise EngineError(
                "INVALID_WORKFLOW", None, "cannot read input file: %s" % (exc,)
            )

    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError:
        raise EngineError("INVALID_WORKFLOW", None, "input is not valid UTF-8")

    if text.startswith("\ufeff"):
        text = text[1:]

    try:
        return json.loads(
            text,
            parse_float=_parse_float,
            parse_int=int,
            parse_constant=_parse_constant,
        )
    except (ValueError, RecursionError):
        raise EngineError("INVALID_WORKFLOW", None, "input is not valid JSON")


def main(argv):
    try:
        if len(argv) != 3 or argv[1] != "run":
            raise EngineError(
                "INVALID_WORKFLOW", None, "usage: workflow run <workflow.json>|-"
            )
        doc = load_document(argv[2])
        response, code = run_workflow(doc)
    except EngineError as exc:
        response = empty_response(exc)
        code = exit_code_for(exc.code)
    except BaseException as exc:  # internal error: best-effort response
        err = EngineError("INTERNAL_ERROR", None, "%s: %s" % (type(exc).__name__, exc))
        response = empty_response(err)
        code = 3

    try:
        sys.stdout.write(dump_json(response))
        sys.stdout.write("\n")
        sys.stdout.flush()
    except Exception:  # pragma: no cover - last resort
        try:
            sys.stdout.write(
                '{"status":"failed","results":{},"execution_order":[],'
                '"skipped":[],"error":{"code":"INTERNAL_ERROR","step":null,'
                '"message":"output failure"}}\n'
            )
            sys.stdout.flush()
        except Exception:
            pass
        return 3

    return code


if __name__ == "__main__":
    sys.setrecursionlimit(20000)
    try:
        _code = main(sys.argv)
    except BaseException:  # pragma: no cover
        _code = 3
    sys.exit(_code)
