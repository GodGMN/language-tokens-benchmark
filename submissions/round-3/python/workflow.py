#!/usr/bin/env python3
"""Declarative workflow engine CLI.

Implements the contract in SPEC.md. Standard library only.
"""

import json
import math
import re
import sys
from functools import cmp_to_key

I64_MIN = -(2 ** 63)
I64_MAX = 2 ** 63 - 1

ID_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*\Z")
INDEX_RE = re.compile(r"[0-9]+\Z")

OPS = {
    # math
    "add", "subtract", "multiply", "divide", "mod", "sum", "min", "max", "range",
    # strings
    "concat", "split", "join", "uppercase", "lowercase", "trim", "replace", "length",
    # arrays
    "map", "filter", "reduce", "sort", "reverse", "unique", "slice", "flatten",
    # objects
    "get", "set", "keys", "values", "merge", "pick", "omit",
    # utilities
    "constant", "equals", "compare", "and", "or", "not", "type",
    # test
    "fail_until",
}

COND_LEAF_OPS = {"eq", "ne", "gt", "gte", "lt", "lte"}
COND_OPS = COND_LEAF_OPS | {"all", "any", "not"}

TYPE_NAMES = {"null", "boolean", "number", "string", "array", "object"}

TRANSFORM_TYPES = {
    "op", "constant", "add", "subtract", "multiply", "divide", "mod",
    "negate", "abs", "stringify", "type",
}

PREDICATE_TYPES = {
    "even", "odd", "gt", "gte", "lt", "lte", "eq", "ne", "truthy", "falsy",
    "type_is", "string_contains",
}

CMP_OPS = {"gt", "gte", "lt", "lte"}


class WFError(Exception):
    """A workflow error with a spec error code."""

    def __init__(self, code, message="", step=None):
        super().__init__(message)
        self.code = code
        self.message = message
        self.step = step


# ---------------------------------------------------------------------------
# Value helpers
# ---------------------------------------------------------------------------

def is_int(v):
    return type(v) is int


def is_number(v):
    return type(v) is int or type(v) is float


def is_ref(v):
    return (
        isinstance(v, dict)
        and len(v) == 1
        and "$ref" in v
        and isinstance(v["$ref"], str)
    )


def type_name(v):
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
    return "object"


def truthy(v):
    if v is None:
        return False
    if type(v) is bool:
        return v
    if type(v) is int or type(v) is float:
        return v != 0
    if isinstance(v, str):
        return v != ""
    if isinstance(v, list):
        return len(v) > 0
    if isinstance(v, dict):
        return len(v) > 0
    return False


def structural_equal(a, b):
    ta = type_name(a)
    tb = type_name(b)
    if ta != tb:
        return False
    if ta == "null":
        return True
    if ta == "boolean":
        return a is b
    if ta == "number":
        return a == b
    if ta == "string":
        return a == b
    if ta == "array":
        if len(a) != len(b):
            return False
        return all(structural_equal(x, y) for x, y in zip(a, b))
    if ta == "object":
        if len(a) != len(b):
            return False
        if set(a.keys()) != set(b.keys()):
            return False
        return all(structural_equal(a[k], b[k]) for k in a)
    return False


TYPE_RANK = {
    "number": 0,
    "string": 1,
    "boolean": 2,
    "null": 3,
    "array": 4,
    "object": 5,
}


def _cmp(a, b):
    return (a > b) - (a < b)


def _utf8(s):
    # Valid JSON input is valid Unicode; surrogatepass only guards against
    # pathological lone-surrogate escapes so ordering never crashes.
    return s.encode("utf-8", "surrogatepass")


def _sorted_keys(obj):
    return sorted(obj.keys(), key=_utf8)


def compare_values(a, b):
    """Total order from SPEC section 3."""
    ta = type_name(a)
    tb = type_name(b)
    if ta != tb:
        return _cmp(TYPE_RANK[ta], TYPE_RANK[tb])
    if ta == "null":
        return 0
    if ta == "boolean":
        return _cmp(0 if not a else 1, 0 if not b else 1)
    if ta == "number":
        return _cmp(a, b)
    if ta == "string":
        return _cmp(_utf8(a), _utf8(b))
    if ta == "array":
        n = min(len(a), len(b))
        for i in range(n):
            c = compare_values(a[i], b[i])
            if c != 0:
                return c
        return _cmp(len(a), len(b))
    # object
    ka = _sorted_keys(a)
    kb = _sorted_keys(b)
    n = min(len(ka), len(kb))
    for i in range(n):
        c = _cmp(_utf8(ka[i]), _utf8(kb[i]))
        if c != 0:
            return c
    if len(ka) != len(kb):
        return _cmp(len(ka), len(kb))
    for k in ka:
        c = compare_values(a[k], b[k])
        if c != 0:
            return c
    return 0


def compare_op(op, a, b):
    c = compare_values(a, b)
    if op == "gt":
        return c > 0
    if op == "gte":
        return c >= 0
    if op == "lt":
        return c < 0
    if op == "lte":
        return c <= 0
    raise WFError("INVALID_ARG", "unknown comparison operator: %r" % (op,))


def compact_json(v):
    return json.dumps(v, ensure_ascii=False, separators=(",", ":"))


# ---------------------------------------------------------------------------
# Argument validation helpers (all raise INVALID_ARG)
# ---------------------------------------------------------------------------

def require_args(args, required, optional=()):
    if not isinstance(args, dict):
        raise WFError("INVALID_ARG", "args must be an object")
    allowed = set(required) | set(optional)
    for k in args:
        if k not in allowed:
            raise WFError("INVALID_ARG", "unknown argument: %s" % k)
    for k in required:
        if k not in args:
            raise WFError("INVALID_ARG", "missing argument: %s" % k)


def arg_i64(args, name):
    v = args[name]
    if not is_int(v):
        raise WFError("INVALID_ARG", "%s must be an integer" % name)
    if v < I64_MIN or v > I64_MAX:
        raise WFError("INVALID_ARG", "%s out of i64 range" % name)
    return v


def arg_str(args, name):
    v = args[name]
    if not isinstance(v, str):
        raise WFError("INVALID_ARG", "%s must be a string" % name)
    return v


def arg_list(args, name):
    v = args[name]
    if not isinstance(v, list):
        raise WFError("INVALID_ARG", "%s must be an array" % name)
    return v


def arg_obj(args, name):
    v = args[name]
    if not isinstance(v, dict):
        raise WFError("INVALID_ARG", "%s must be an object" % name)
    return v


def arg_str_list(args, name):
    v = arg_list(args, name)
    for x in v:
        if not isinstance(x, str):
            raise WFError("INVALID_ARG", "%s must contain only strings" % name)
    return v


def require_i64_value(v, what="value"):
    if not is_int(v):
        raise WFError("INVALID_ARG", "%s must be an integer" % what)
    if v < I64_MIN or v > I64_MAX:
        raise WFError("INVALID_ARG", "%s out of i64 range" % what)
    return v


# ---------------------------------------------------------------------------
# i64 arithmetic
# ---------------------------------------------------------------------------

def _checked(x):
    if x < I64_MIN or x > I64_MAX:
        raise WFError("ARITHMETIC_OVERFLOW", "i64 overflow")
    return x


def trunc_div(a, b):
    if b == 0:
        raise WFError("DIVISION_BY_ZERO", "division by zero")
    if a == I64_MIN and b == -1:
        raise WFError("ARITHMETIC_OVERFLOW", "INT64_MIN / -1 overflow")
    q = abs(a) // abs(b)
    if (a < 0) != (b < 0):
        q = -q
    return _checked(q)


def trunc_mod(a, b):
    if b == 0:
        raise WFError("DIVISION_BY_ZERO", "modulo by zero")
    if a == I64_MIN and b == -1:
        raise WFError("ARITHMETIC_OVERFLOW", "INT64_MIN % -1 overflow")
    q = trunc_div(a, b)
    return a - q * b


def math_binop(op, a, b):
    if op == "add":
        return _checked(a + b)
    if op == "subtract":
        return _checked(a - b)
    if op == "multiply":
        return _checked(a * b)
    if op == "divide":
        return trunc_div(a, b)
    if op == "mod":
        return trunc_mod(a, b)
    raise WFError("INVALID_ARG", "not a binary math op: %s" % op)


def ascii_upper(s):
    out = []
    for ch in s:
        if "a" <= ch <= "z":
            out.append(chr(ord(ch) - 32))
        else:
            out.append(ch)
    return "".join(out)


def ascii_lower(s):
    out = []
    for ch in s:
        if "A" <= ch <= "Z":
            out.append(chr(ord(ch) + 32))
        else:
            out.append(ch)
    return "".join(out)


TRIM_CHARS = " \t\n\r\f\v"


# ---------------------------------------------------------------------------
# map transforms and filter predicates
# ---------------------------------------------------------------------------

def substitute_self(v, element):
    if isinstance(v, dict):
        if len(v) == 1 and "$self" in v and v["$self"] is True:
            return element
        return {k: substitute_self(val, element) for k, val in v.items()}
    if isinstance(v, list):
        return [substitute_self(x, element) for x in v]
    return v


def apply_transform(t, element, runtime, step_id):
    if isinstance(t, str):
        t = {"type": t}
    if not isinstance(t, dict):
        raise WFError("INVALID_ARG", "transform must be an object")
    ttype = t.get("type")
    if not isinstance(ttype, str) or ttype not in TRANSFORM_TYPES:
        raise WFError("INVALID_ARG", "unknown transform type")

    if ttype == "op":
        opname = t.get("op")
        if not isinstance(opname, str) or opname not in OPS:
            raise WFError("INVALID_ARG", "unknown transform op")
        sub = t.get("args", {})
        if not isinstance(sub, dict):
            raise WFError("INVALID_ARG", "transform args must be an object")
        sub = substitute_self(sub, element)
        return execute_op(opname, sub, runtime, step_id)

    if ttype == "constant":
        if "value" not in t:
            raise WFError("INVALID_ARG", "constant transform needs value")
        return t["value"]

    if ttype in ("add", "subtract", "multiply", "divide", "mod"):
        if "value" not in t:
            raise WFError("INVALID_ARG", "%s transform needs value" % ttype)
        b = require_i64_value(t["value"], "transform value")
        a = require_i64_value(element, "element")
        return math_binop(ttype, a, b)

    if ttype == "negate":
        a = require_i64_value(element, "element")
        if a == I64_MIN:
            raise WFError("ARITHMETIC_OVERFLOW", "negate overflow")
        return -a

    if ttype == "abs":
        a = require_i64_value(element, "element")
        if a == I64_MIN:
            raise WFError("ARITHMETIC_OVERFLOW", "abs overflow")
        return a if a >= 0 else -a

    if ttype == "stringify":
        return compact_json(element)

    if ttype == "type":
        return type_name(element)

    raise WFError("INVALID_ARG", "unknown transform type")


def _type_is_target(p):
    """Return the requested type name for a type_is predicate, or None."""
    # SPEC writes {"type":"type_is","type":"<type>"} (a duplicate key),
    # so accept the type name under any plausible extra key, and also accept
    # the literal parsed form {"type":"<type>"}.
    ptype = p.get("type")
    if isinstance(ptype, str) and ptype in TYPE_NAMES:
        return ptype
    for key in ("value", "type_name", "target", "name", "of"):
        cand = p.get(key)
        if isinstance(cand, str) and cand in TYPE_NAMES:
            return cand
    for key, cand in p.items():
        if key == "type":
            continue
        if isinstance(cand, str) and cand in TYPE_NAMES:
            return cand
    return None


def validate_transform_shape(t):
    if isinstance(t, str):
        t = {"type": t}
    if not isinstance(t, dict):
        raise WFError("INVALID_ARG", "transform must be an object")
    ttype = t.get("type")
    if not isinstance(ttype, str) or ttype not in TRANSFORM_TYPES:
        raise WFError("INVALID_ARG", "unknown transform type")
    if ttype == "op":
        opname = t.get("op")
        if not isinstance(opname, str) or opname not in OPS:
            raise WFError("INVALID_ARG", "unknown transform op")
        if not isinstance(t.get("args", {}), dict):
            raise WFError("INVALID_ARG", "transform args must be an object")
    elif ttype == "constant":
        if "value" not in t:
            raise WFError("INVALID_ARG", "constant transform needs value")
    elif ttype in ("add", "subtract", "multiply", "divide", "mod"):
        if "value" not in t:
            raise WFError("INVALID_ARG", "%s transform needs value" % ttype)
        require_i64_value(t["value"], "transform value")


def validate_predicate_shape(p):
    if isinstance(p, str):
        p = {"type": p}
    if not isinstance(p, dict):
        raise WFError("INVALID_ARG", "predicate must be an object")
    ptype = p.get("type")
    if isinstance(ptype, str) and ptype in TYPE_NAMES:
        return
    if not isinstance(ptype, str) or ptype not in PREDICATE_TYPES:
        raise WFError("INVALID_ARG", "unknown predicate type")
    if ptype in ("gt", "gte", "lt", "lte", "eq", "ne"):
        if "value" not in p:
            raise WFError("INVALID_ARG", "%s predicate needs value" % ptype)
        require_i64_value(p["value"], "predicate value")
    elif ptype == "type_is":
        if _type_is_target(p) is None:
            raise WFError("INVALID_ARG", "type_is needs a valid type name")
    elif ptype == "string_contains":
        if "value" not in p:
            raise WFError("INVALID_ARG", "string_contains needs value")
        if not isinstance(p["value"], str):
            raise WFError("INVALID_ARG",
                          "string_contains value must be a string")


def eval_predicate(p, element):
    if isinstance(p, str):
        p = {"type": p}
    if not isinstance(p, dict):
        raise WFError("INVALID_ARG", "predicate must be an object")
    ptype = p.get("type")
    if isinstance(ptype, str) and ptype in TYPE_NAMES:
        # Tolerate the literal duplicate-key spelling from SPEC:
        # {"type": "type_is", "type": "number"} parses as {"type": "number"}.
        return type_name(element) == ptype
    if not isinstance(ptype, str) or ptype not in PREDICATE_TYPES:
        raise WFError("INVALID_ARG", "unknown predicate type")

    if ptype in ("even", "odd"):
        e = require_i64_value(element, "element")
        if ptype == "even":
            return e % 2 == 0
        return e % 2 != 0

    if ptype in ("gt", "gte", "lt", "lte"):
        if "value" not in p:
            raise WFError("INVALID_ARG", "%s predicate needs value" % ptype)
        v = require_i64_value(p["value"], "predicate value")
        return compare_op(ptype, element, v)

    if ptype in ("eq", "ne"):
        if "value" not in p:
            raise WFError("INVALID_ARG", "%s predicate needs value" % ptype)
        v = require_i64_value(p["value"], "predicate value")
        eq = structural_equal(element, v)
        return eq if ptype == "eq" else not eq

    if ptype == "truthy":
        return truthy(element)

    if ptype == "falsy":
        return not truthy(element)

    if ptype == "type_is":
        target = _type_is_target(p)
        if target is None:
            raise WFError("INVALID_ARG", "type_is needs a valid type name")
        return type_name(element) == target

    if ptype == "string_contains":
        if "value" not in p:
            raise WFError("INVALID_ARG", "string_contains needs value")
        v = p["value"]
        if not isinstance(v, str):
            raise WFError("INVALID_ARG", "string_contains value must be a string")
        if not isinstance(element, str):
            raise WFError("INVALID_ARG", "element must be a string")
        return v in element

    raise WFError("INVALID_ARG", "unknown predicate type")


# ---------------------------------------------------------------------------
# Operation execution
# ---------------------------------------------------------------------------

def execute_op(op, args, runtime, step_id):
    if not isinstance(args, dict):
        raise WFError("INVALID_ARG", "args must be an object")

    # --- math ---
    if op in ("add", "subtract", "multiply", "divide", "mod"):
        require_args(args, ("a", "b"))
        a = arg_i64(args, "a")
        b = arg_i64(args, "b")
        return math_binop(op, a, b)

    if op == "sum":
        require_args(args, ("input",))
        items = arg_list(args, "input")
        total = 0
        for x in items:
            total = _checked(total + require_i64_value(x, "input element"))
        return total

    if op == "min":
        require_args(args, ("input",))
        items = arg_list(args, "input")
        if not items:
            raise WFError("EMPTY_ARRAY", "min of empty array")
        vals = [require_i64_value(x, "input element") for x in items]
        return min(vals)

    if op == "max":
        require_args(args, ("input",))
        items = arg_list(args, "input")
        if not items:
            raise WFError("EMPTY_ARRAY", "max of empty array")
        vals = [require_i64_value(x, "input element") for x in items]
        return max(vals)

    if op == "range":
        require_args(args, ("start", "end"))
        start = arg_i64(args, "start")
        end = arg_i64(args, "end")
        if start > end:
            return []
        return list(range(start, end + 1))

    # --- strings ---
    if op == "concat":
        require_args(args, ("a", "b"))
        return arg_str(args, "a") + arg_str(args, "b")

    if op == "split":
        require_args(args, ("input", "sep"))
        s = arg_str(args, "input")
        sep = arg_str(args, "sep")
        if sep == "":
            return list(s)
        return s.split(sep)

    if op == "join":
        require_args(args, ("input", "sep"))
        items = arg_str_list(args, "input")
        sep = arg_str(args, "sep")
        return sep.join(items)

    if op == "uppercase":
        require_args(args, ("input",))
        return ascii_upper(arg_str(args, "input"))

    if op == "lowercase":
        require_args(args, ("input",))
        return ascii_lower(arg_str(args, "input"))

    if op == "trim":
        require_args(args, ("input",))
        return arg_str(args, "input").strip(TRIM_CHARS)

    if op == "replace":
        require_args(args, ("input", "search", "replace"))
        s = arg_str(args, "input")
        search = arg_str(args, "search")
        repl = arg_str(args, "replace")
        if search == "":
            raise WFError("INVALID_ARG", "replace search must not be empty")
        return s.replace(search, repl)

    if op == "length":
        require_args(args, ("input",))
        return len(arg_str(args, "input"))

    # --- arrays ---
    if op == "map":
        require_args(args, ("input", "transform"))
        items = arg_list(args, "input")
        t = args["transform"]
        validate_transform_shape(t)
        return [apply_transform(t, x, runtime, step_id) for x in items]

    if op == "filter":
        require_args(args, ("input", "predicate"))
        items = arg_list(args, "input")
        p = args["predicate"]
        validate_predicate_shape(p)
        return [x for x in items if eval_predicate(p, x)]

    if op == "reduce":
        require_args(args, ("input", "op"), optional=("initial",))
        items = arg_list(args, "input")
        rop = arg_str(args, "op")
        if rop not in ("add", "subtract", "multiply"):
            raise WFError("INVALID_ARG", "unsupported reduce op")
        if "initial" in args:
            acc = arg_i64(args, "initial")
        else:
            acc = 0 if rop in ("add", "subtract") else 1
        for x in items:
            v = require_i64_value(x, "input element")
            acc = math_binop(rop, acc, v)
        return acc

    if op == "sort":
        require_args(args, ("input",))
        items = arg_list(args, "input")
        return sorted(items, key=cmp_to_key(compare_values))

    if op == "reverse":
        require_args(args, ("input",))
        return list(reversed(arg_list(args, "input")))

    if op == "unique":
        require_args(args, ("input",))
        items = arg_list(args, "input")
        out = []
        for x in items:
            if not any(structural_equal(x, y) for y in out):
                out.append(x)
        return out

    if op == "slice":
        require_args(args, ("input", "start"), optional=("end",))
        items = arg_list(args, "input")
        start = arg_i64(args, "start")
        if "end" in args:
            end = arg_i64(args, "end")
        else:
            end = None
        s_idx, e_idx, _ = slice(start, end).indices(len(items))
        return items[s_idx:e_idx]

    if op == "flatten":
        require_args(args, ("input",))
        items = arg_list(args, "input")
        out = []
        for x in items:
            if isinstance(x, list):
                out.extend(x)
            else:
                out.append(x)
        return out

    # --- objects ---
    if op == "get":
        require_args(args, ("input", "key"))
        obj = arg_obj(args, "input")
        key = arg_str(args, "key")
        return obj.get(key)

    if op == "set":
        require_args(args, ("input", "key", "value"))
        obj = arg_obj(args, "input")
        key = arg_str(args, "key")
        out = dict(obj)
        out[key] = args["value"]
        return out

    if op == "keys":
        require_args(args, ("input",))
        obj = arg_obj(args, "input")
        return _sorted_keys(obj)

    if op == "values":
        require_args(args, ("input",))
        obj = arg_obj(args, "input")
        return [obj[k] for k in _sorted_keys(obj)]

    if op == "merge":
        require_args(args, ("a", "b"))
        a = arg_obj(args, "a")
        b = arg_obj(args, "b")
        out = dict(a)
        out.update(b)
        return out

    if op == "pick":
        require_args(args, ("input", "keys"))
        obj = arg_obj(args, "input")
        keys = arg_str_list(args, "keys")
        return {k: obj.get(k) for k in sorted(set(keys), key=_utf8)}

    if op == "omit":
        require_args(args, ("input", "keys"))
        obj = arg_obj(args, "input")
        keys = set(arg_str_list(args, "keys"))
        return {k: v for k, v in obj.items() if k not in keys}

    # --- utilities ---
    if op == "constant":
        require_args(args, ("value",))
        return args["value"]

    if op == "equals":
        require_args(args, ("a", "b"))
        return structural_equal(args["a"], args["b"])

    if op == "compare":
        require_args(args, ("a", "b", "op"))
        cop = arg_str(args, "op")
        if cop not in CMP_OPS:
            raise WFError("INVALID_ARG", "unknown compare op")
        return compare_op(cop, args["a"], args["b"])

    if op == "and":
        require_args(args, ("a", "b"))
        return bool(truthy(args["a"]) and truthy(args["b"]))

    if op == "or":
        require_args(args, ("a", "b"))
        return bool(truthy(args["a"]) or truthy(args["b"]))

    if op == "not":
        require_args(args, ("a",))
        return not truthy(args["a"])

    if op == "type":
        require_args(args, ("value",))
        return type_name(args["value"])

    # --- test op ---
    if op == "fail_until":
        require_args(args, ("failures", "value"))
        failures = arg_i64(args, "failures")
        if failures < 0 or failures > 10:
            raise WFError("INVALID_ARG", "failures must be 0..10")
        counts = runtime.setdefault("fail_until", {})
        n = counts.get(step_id, 0) + 1
        counts[step_id] = n
        if n <= failures:
            raise WFError("TRANSIENT_FAILURE",
                          "transient failure %d of %d" % (n, failures))
        return args["value"]

    raise WFError("INVALID_ARG", "unknown operation: %s" % op)


# ---------------------------------------------------------------------------
# Structural validation
# ---------------------------------------------------------------------------

class Step:
    __slots__ = ("id", "op", "args", "depends_on", "when", "max_attempts",
                 "allowed_refs")

    def __init__(self, sid, op, args, depends_on, when, max_attempts):
        self.id = sid
        self.op = op
        self.args = args
        self.depends_on = depends_on
        self.when = when
        self.max_attempts = max_attempts
        self.allowed_refs = set()


def _invalid(message, step=None):
    raise WFError("INVALID_WORKFLOW", message, step)


def validate_condition(cond, step_id):
    if not isinstance(cond, dict):
        _invalid("condition must be an object", step_id)
    op = cond.get("op")
    if not isinstance(op, str) or op not in COND_OPS:
        _invalid("invalid condition op", step_id)

    if op in COND_LEAF_OPS:
        for k in cond:
            if k not in ("op", "left", "right"):
                _invalid("unknown condition field: %s" % k, step_id)
        if "left" not in cond or "right" not in cond:
            _invalid("condition needs left and right", step_id)
        return

    if op in ("all", "any"):
        for k in cond:
            if k not in ("op", "conditions"):
                _invalid("unknown condition field: %s" % k, step_id)
        subs = cond.get("conditions")
        if not isinstance(subs, list):
            _invalid("%s needs conditions array" % op, step_id)
        for sub in subs:
            validate_condition(sub, step_id)
        return

    # not
    for k in cond:
        if k not in ("op", "condition"):
            _invalid("unknown condition field: %s" % k, step_id)
    if "condition" not in cond:
        _invalid("not needs condition", step_id)
    validate_condition(cond["condition"], step_id)


def validate_workflow(doc):
    if not isinstance(doc, dict):
        _invalid("workflow must be an object")

    for k in doc:
        if k not in ("version", "steps"):
            _invalid("unknown top-level field: %s" % k)

    if "version" not in doc:
        _invalid("missing version")
    version = doc["version"]
    if not is_int(version) or version != 1:
        _invalid("version must be the integer 1")

    if "steps" not in doc:
        _invalid("missing steps")
    raw_steps = doc["steps"]
    if not isinstance(raw_steps, list):
        _invalid("steps must be an array")

    steps = []
    seen = {}

    for raw in raw_steps:
        if not isinstance(raw, dict):
            _invalid("step must be an object")

        sid = raw.get("id")
        if not isinstance(sid, str):
            _invalid("step id must be a string", None)
        if not ID_RE.fullmatch(sid):
            _invalid("invalid step id: %r" % sid, sid)
        if sid in seen:
            _invalid("duplicate step id: %s" % sid, sid)

        op = raw.get("op")
        if not isinstance(op, str) or op not in OPS:
            _invalid("invalid or missing op", sid)

        for k in raw:
            if k not in ("id", "op", "args", "depends_on", "when", "retry"):
                _invalid("unknown step field: %s" % k, sid)

        if "args" in raw:
            args = raw["args"]
            if not isinstance(args, dict):
                _invalid("args must be an object", sid)
        else:
            args = {}

        if "depends_on" in raw:
            deps_raw = raw["depends_on"]
            if not isinstance(deps_raw, list):
                _invalid("depends_on must be an array", sid)
            deps = []
            for d in deps_raw:
                if not isinstance(d, str):
                    _invalid("depends_on entries must be strings", sid)
                deps.append(d)
        else:
            deps = []

        when = None
        if "when" in raw:
            when = raw["when"]
            validate_condition(when, sid)

        max_attempts = 1
        if "retry" in raw:
            retry = raw["retry"]
            if not isinstance(retry, dict):
                _invalid("retry must be an object", sid)
            for k in retry:
                if k != "max_attempts":
                    _invalid("unknown retry field: %s" % k, sid)
            if "max_attempts" in retry:
                ma = retry["max_attempts"]
                if not is_int(ma) or ma < 1 or ma > 5:
                    _invalid("max_attempts must be an integer 1..5", sid)
                max_attempts = ma

        seen[sid] = True
        steps.append(Step(sid, op, args, deps, when, max_attempts))

    by_id = {s.id: s for s in steps}

    # Unknown dependencies
    for s in steps:
        for d in s.depends_on:
            if d not in by_id:
                raise WFError("UNKNOWN_DEPENDENCY",
                              "step %s depends on unknown step %s" % (s.id, d),
                              s.id)

    # Cycle detection (DFS with colors)
    color = {s.id: 0 for s in steps}  # 0 unvisited, 1 visiting, 2 done

    def visit(sid):
        color[sid] = 1
        for d in by_id[sid].depends_on:
            if color[d] == 1:
                raise WFError("CYCLIC_DEPENDENCY",
                              "dependency cycle involving %s" % d, None)
            if color[d] == 0:
                visit(d)
        color[sid] = 2

    for s in steps:
        if color[s.id] == 0:
            visit(s.id)

    # Transitive dependency closure (allowed $ref targets)
    memo = {}

    def closure(sid):
        if sid in memo:
            return memo[sid]
        out = set()
        for d in by_id[sid].depends_on:
            out.add(d)
            out |= closure(d)
        memo[sid] = out
        return out

    for s in steps:
        s.allowed_refs = closure(s.id)

    return steps


# ---------------------------------------------------------------------------
# Reference resolution and conditions
# ---------------------------------------------------------------------------

def resolve_reference(ref, allowed, results, skipped):
    parts = ref.split(".")
    target = parts[0]
    if target not in allowed:
        raise WFError("INVALID_REFERENCE", "reference to non-dependency: %s" % ref)
    if target in skipped:
        raise WFError("SKIPPED_REFERENCE", "reference to skipped step: %s" % target)
    if target not in results:
        raise WFError("INVALID_REFERENCE", "reference to unfinished step: %s" % target)

    cur = results[target]
    for seg in parts[1:]:
        if isinstance(cur, dict):
            if seg not in cur:
                raise WFError("INVALID_REFERENCE", "missing key in reference: %s" % ref)
            cur = cur[seg]
        elif isinstance(cur, list):
            if not INDEX_RE.fullmatch(seg):
                raise WFError("INVALID_REFERENCE", "bad array index in reference: %s" % ref)
            idx = int(seg)
            if idx >= len(cur):
                raise WFError("INVALID_REFERENCE", "array index out of range: %s" % ref)
            cur = cur[idx]
        else:
            raise WFError("INVALID_REFERENCE", "cannot traverse scalar in reference: %s" % ref)
    return cur


def resolve_value(v, allowed, results, skipped):
    if is_ref(v):
        return resolve_reference(v["$ref"], allowed, results, skipped)
    if isinstance(v, list):
        return [resolve_value(x, allowed, results, skipped) for x in v]
    if isinstance(v, dict):
        return {k: resolve_value(x, allowed, results, skipped)
                for k, x in v.items()}
    return v


def eval_condition(cond, allowed, results, skipped):
    op = cond["op"]
    if op in COND_LEAF_OPS:
        left = resolve_value(cond["left"], allowed, results, skipped)
        right = resolve_value(cond["right"], allowed, results, skipped)
        if op == "eq":
            return structural_equal(left, right)
        if op == "ne":
            return not structural_equal(left, right)
        return compare_op(op, left, right)
    if op == "all":
        return all(eval_condition(c, allowed, results, skipped)
                   for c in cond["conditions"])
    if op == "any":
        return any(eval_condition(c, allowed, results, skipped)
                   for c in cond["conditions"])
    # not
    return not eval_condition(cond["condition"], allowed, results, skipped)


# ---------------------------------------------------------------------------
# Execution
# ---------------------------------------------------------------------------

def execute_workflow(steps):
    results = {}
    order = []
    skipped = set()
    skipped_order = []
    done = set()
    runtime = {}
    remaining = list(steps)

    def failure_response(exc):
        return {
            "status": "failed",
            "results": results,
            "execution_order": order,
            "skipped": skipped_order,
            "error": {
                "code": exc.code,
                "step": exc.step,
                "message": exc.message or exc.code,
            },
        }

    while remaining:
        chosen = None
        for s in remaining:
            if all(d in done for d in s.depends_on):
                chosen = s
                break
        if chosen is None:
            # Should be unreachable after cycle validation.
            raise WFError("CYCLIC_DEPENDENCY", "no runnable step", None)

        remaining.remove(chosen)

        if chosen.when is not None:
            try:
                cond = eval_condition(chosen.when, chosen.allowed_refs,
                                      results, skipped)
            except WFError as exc:
                exc.step = chosen.id
                return failure_response(exc)
            if not isinstance(cond, bool):
                return failure_response(WFError(
                    "INVALID_CONDITION",
                    "condition did not evaluate to a boolean",
                    chosen.id))
            if not cond:
                results[chosen.id] = None
                skipped.add(chosen.id)
                skipped_order.append(chosen.id)
                order.append(chosen.id)
                done.add(chosen.id)
                continue

        final_value = None
        final_error = None
        for attempt in range(1, chosen.max_attempts + 1):
            try:
                resolved_args = resolve_value(chosen.args, chosen.allowed_refs,
                                              results, skipped)
                value = execute_op(chosen.op, resolved_args, runtime, chosen.id)
                final_value = value
                final_error = None
                break
            except WFError as exc:
                if exc.code == "TRANSIENT_FAILURE":
                    if attempt < chosen.max_attempts:
                        continue
                    final_error = WFError("STEP_FAILED",
                                          "step failed after %d attempt(s)" %
                                          chosen.max_attempts,
                                          chosen.id)
                else:
                    exc.step = chosen.id
                    final_error = exc
                break

        if final_error is not None:
            final_error.step = chosen.id
            return failure_response(final_error)

        results[chosen.id] = final_value
        order.append(chosen.id)
        done.add(chosen.id)

    return {
        "status": "completed",
        "results": results,
        "execution_order": order,
        "skipped": skipped_order,
        "error": None,
    }


def error_response(code, step, message):
    return {
        "status": "failed",
        "results": {},
        "execution_order": [],
        "skipped": [],
        "error": {"code": code, "step": step, "message": message},
    }


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def _reject_constant(text):
    raise ValueError("invalid JSON constant: %s" % text)


def _parse_float(text):
    value = float(text)
    if not math.isfinite(value):
        raise ValueError("number out of range: %s" % text)
    return value


def _emit(response):
    # ASCII-escaped output is still exactly one valid JSON document and is
    # safe to write even if a pathological lone-surrogate string appears.
    sys.stdout.write(json.dumps(response, ensure_ascii=True,
                                separators=(",", ":")))
    sys.stdout.write("\n")
    sys.stdout.flush()


def main(argv):
    if len(argv) != 3 or argv[1] != "run":
        _emit(error_response("INVALID_WORKFLOW", None,
                             "usage: workflow run <workflow.json|->"))
        return 2

    path = argv[2]
    try:
        if path == "-":
            data = sys.stdin.buffer.read()
        else:
            with open(path, "rb") as fh:
                data = fh.read()
    except OSError as exc:
        _emit(error_response("INVALID_WORKFLOW", None,
                             "cannot read input: %s" % exc))
        return 2

    try:
        text = data.decode("utf-8-sig")
    except UnicodeDecodeError as exc:
        _emit(error_response("INVALID_WORKFLOW", None,
                             "input is not valid UTF-8: %s" % exc))
        return 2

    try:
        doc = json.loads(text, parse_constant=_reject_constant,
                         parse_float=_parse_float)
    except (ValueError, RecursionError) as exc:
        _emit(error_response("INVALID_WORKFLOW", None,
                             "invalid JSON: %s" % exc))
        return 2

    try:
        steps = validate_workflow(doc)
    except WFError as exc:
        _emit(error_response(exc.code, exc.step, exc.message or exc.code))
        return 2
    except RecursionError as exc:
        _emit(error_response("INVALID_WORKFLOW", None,
                             "document nesting too deep: %s" % exc))
        return 2

    try:
        response = execute_workflow(steps)
    except WFError as exc:
        # Should normally be handled inside execute_workflow; be safe.
        _emit(error_response(exc.code, exc.step, exc.message or exc.code))
        return 1
    except RecursionError as exc:
        _emit(error_response("INTERNAL_ERROR", None,
                             "recursion limit exceeded: %s" % exc))
        return 3

    _emit(response)
    return 0 if response["status"] == "completed" else 1


if __name__ == "__main__":
    sys.setrecursionlimit(20000)
    try:
        code = main(sys.argv)
    except Exception as exc:  # noqa: BLE001 - final safety net
        try:
            _emit(error_response("INTERNAL_ERROR", None,
                                 "internal error: %s" % exc))
        except Exception:
            pass
        code = 3
    sys.exit(code)
