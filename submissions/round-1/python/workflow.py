#!/usr/bin/env python3
"""Declarative workflow engine (SPEC.md).

Usage:
    python3 workflow.py <workflow.json>
    python3 workflow.py run <workflow.json>
    python3 workflow.py run -          # read document from stdin

stdout always contains exactly one JSON document. Diagnostics go to stderr.
"""

import copy
import json
import math
import re
import sys
from functools import cmp_to_key

I64_MIN = -(2 ** 63)
I64_MAX = 2 ** 63 - 1

TYPE_NAMES = ("null", "boolean", "number", "string", "array", "object")

NUMERIC_TRANSFORMS = ("add", "subtract", "multiply", "divide", "mod")

OPS = {
    # math
    "add", "subtract", "multiply", "divide", "mod", "sum", "min", "max",
    "range",
    # strings
    "concat", "split", "join", "uppercase", "lowercase", "trim", "replace",
    "length",
    # arrays
    "map", "filter", "reduce", "sort", "reverse", "unique", "slice", "flatten",
    # objects
    "get", "set", "keys", "values", "merge", "pick", "omit",
    # utilities
    "constant", "equals", "compare", "and", "or", "not", "type",
    # test
    "fail_until",
}

STEP_FIELDS = {"id", "op", "args", "depends_on", "when", "retry"}
ID_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*\Z")
INDEX_RE = re.compile(r"[0-9]+\Z")


class Err(Exception):
    """An error carrying the SPEC error code and (optionally) the step id."""

    def __init__(self, code, message="", step=None):
        super().__init__(message)
        self.code = code
        self.message = message
        self.step = step


class Step:
    __slots__ = ("id", "op", "args", "deps", "when", "max_attempts")

    def __init__(self, sid, op, args, deps, when, max_attempts):
        self.id = sid
        self.op = op
        self.args = args
        self.deps = deps
        self.when = when
        self.max_attempts = max_attempts


class Ctx:
    __slots__ = ("by_id", "trans", "results", "skipped", "fail_counts")

    def __init__(self, by_id, trans):
        self.by_id = by_id
        self.trans = trans
        self.results = {}
        self.skipped = set()
        self.fail_counts = {}


# ---------------------------------------------------------------------------
# Value helpers
# ---------------------------------------------------------------------------


def utf8_key(s):
    return s.encode("utf-8", "surrogatepass")


def type_name(v):
    if v is None:
        return "null"
    if isinstance(v, bool):
        return "boolean"
    if isinstance(v, (int, float)):
        return "number"
    if isinstance(v, str):
        return "string"
    if isinstance(v, list):
        return "array"
    if isinstance(v, dict):
        return "object"
    raise Err("INVALID_ARG", "unsupported value type")


def truthy(v):
    if v is None or v is False:
        return False
    if isinstance(v, bool):
        return v
    if isinstance(v, (int, float)):
        return v != 0
    if isinstance(v, str):
        return len(v) > 0
    if isinstance(v, list):
        return len(v) > 0
    if isinstance(v, dict):
        return len(v) > 0
    return True


# Total order from SPEC section 3.  Returning 0 is structural equality.
def _rank(v):
    if isinstance(v, bool):
        return 2
    if isinstance(v, (int, float)):
        return 0
    if isinstance(v, str):
        return 1
    if v is None:
        return 3
    if isinstance(v, list):
        return 4
    if isinstance(v, dict):
        return 5
    raise Err("INVALID_ARG", "unsupported value type")


def compare_values(a, b):
    ra = _rank(a)
    rb = _rank(b)
    if ra != rb:
        return -1 if ra < rb else 1
    if ra == 0:  # number
        if a == b:
            return 0
        return -1 if a < b else 1
    if ra == 1:  # string, UTF-8 byte order
        ba = utf8_key(a)
        bb = utf8_key(b)
        if ba == bb:
            return 0
        return -1 if ba < bb else 1
    if ra == 2:  # boolean: false < true
        return (a > b) - (a < b)
    if ra == 3:  # null
        return 0
    if ra == 4:  # array
        for x, y in zip(a, b):
            c = compare_values(x, y)
            if c:
                return c
        return (len(a) > len(b)) - (len(a) < len(b))
    # object: sorted keys first, then values in sorted-key order
    ka = sorted(a, key=utf8_key)
    kb = sorted(b, key=utf8_key)
    for x, y in zip(ka, kb):
        if x != y:
            return -1 if utf8_key(x) < utf8_key(y) else 1
    if len(ka) != len(kb):
        return -1 if len(ka) < len(kb) else 1
    for k in ka:
        c = compare_values(a[k], b[k])
        if c:
            return c
    return 0


def structural_equal(a, b):
    return compare_values(a, b) == 0


def canon(v):
    """Hashable canonical form consistent with structural equality."""
    if v is None:
        return (3,)
    if isinstance(v, bool):
        return (2, 1 if v else 0)
    if isinstance(v, (int, float)):
        return (0, v)
    if isinstance(v, str):
        return (1, v)
    if isinstance(v, list):
        return (4, tuple(canon(x) for x in v))
    if isinstance(v, dict):
        return (5, tuple((k, canon(v[k])) for k in sorted(v, key=utf8_key)))
    raise Err("INVALID_ARG", "unsupported value type")


# ---------------------------------------------------------------------------
# i64 arithmetic
# ---------------------------------------------------------------------------


def _checked(r):
    if r < I64_MIN or r > I64_MAX:
        raise Err("ARITHMETIC_OVERFLOW", "i64 overflow")
    return r


def iadd(a, b):
    return _checked(a + b)


def isub(a, b):
    return _checked(a - b)


def imul(a, b):
    return _checked(a * b)


def idiv(a, b):
    if b == 0:
        raise Err("DIVISION_BY_ZERO", "division by zero")
    q = abs(a) // abs(b)
    if (a < 0) != (b < 0):
        q = -q
    return _checked(q)


def imod(a, b):
    if b == 0:
        raise Err("DIVISION_BY_ZERO", "modulo by zero")
    q = abs(a) // abs(b)
    if (a < 0) != (b < 0):
        q = -q
    return a - q * b


def ineg(a):
    return _checked(-a)


def iabs(a):
    return _checked(abs(a))


# ---------------------------------------------------------------------------
# Argument helpers
# ---------------------------------------------------------------------------


def check_args(args, required=(), optional=()):
    if not isinstance(args, dict):
        raise Err("INVALID_ARG", "args must be an object")
    allowed = set(required) | set(optional)
    for k in args:
        if k not in allowed:
            raise Err("INVALID_ARG", "unknown argument %r" % (k,))
    for k in required:
        if k not in args:
            raise Err("INVALID_ARG", "missing argument %r" % (k,))


def as_int(v):
    if isinstance(v, bool):
        raise Err("INVALID_ARG", "expected an integer")
    if isinstance(v, int):
        if I64_MIN <= v <= I64_MAX:
            return v
        raise Err("INVALID_ARG", "integer outside i64 range")
    if isinstance(v, float):
        if math.isfinite(v) and v.is_integer() and I64_MIN <= v <= I64_MAX:
            return int(v)
        raise Err("INVALID_ARG", "expected an integer")
    raise Err("INVALID_ARG", "expected an integer")


def as_str(v):
    if not isinstance(v, str):
        raise Err("INVALID_ARG", "expected a string")
    return v


def as_list(v):
    if not isinstance(v, list):
        raise Err("INVALID_ARG", "expected an array")
    return v


def as_obj(v):
    if not isinstance(v, dict):
        raise Err("INVALID_ARG", "expected an object")
    return v


# ---------------------------------------------------------------------------
# Condition validation / evaluation
# ---------------------------------------------------------------------------

COND_BINARY = {"eq", "ne", "gt", "gte", "lt", "lte"}


def _is_ref_object(value):
    return (
        isinstance(value, dict)
        and len(value) == 1
        and "$ref" in value
        and isinstance(value["$ref"], str)
    )


def validate_condition(cond, step_id):
    if not isinstance(cond, dict):
        raise Err("INVALID_WORKFLOW", "condition must be an object", step=step_id)
    # A bare reference is accepted as a condition; its resolved value must be
    # a strict boolean (otherwise INVALID_CONDITION at runtime).
    if _is_ref_object(cond):
        return
    op = cond.get("op")
    if not isinstance(op, str):
        raise Err("INVALID_WORKFLOW", "condition op must be a string", step=step_id)
    if op in COND_BINARY:
        if "left" not in cond or "right" not in cond:
            raise Err(
                "INVALID_WORKFLOW",
                "condition %r requires left and right" % op,
                step=step_id,
            )
    elif op in ("all", "any"):
        subs = cond.get("conditions")
        if not isinstance(subs, list):
            raise Err(
                "INVALID_WORKFLOW",
                "condition %r requires a conditions array" % op,
                step=step_id,
            )
        for sub in subs:
            validate_condition(sub, step_id)
    elif op == "not":
        sub = cond.get("condition")
        if not isinstance(sub, dict):
            raise Err(
                "INVALID_WORKFLOW",
                "condition 'not' requires a condition object",
                step=step_id,
            )
        validate_condition(sub, step_id)
    else:
        raise Err(
            "INVALID_WORKFLOW", "unknown condition op %r" % (op,), step=step_id
        )


def eval_condition(cond, step, ctx):
    if _is_ref_object(cond):
        value = resolve_ref(cond["$ref"], step, ctx)
        if not isinstance(value, bool):
            raise Err(
                "INVALID_CONDITION", "condition did not yield a boolean"
            )
        return value
    op = cond["op"]
    if op in COND_BINARY:
        left = resolve_value(cond["left"], step, ctx)
        right = resolve_value(cond["right"], step, ctx)
        c = compare_values(left, right)
        if op == "eq":
            return c == 0
        if op == "ne":
            return c != 0
        if op == "gt":
            return c > 0
        if op == "gte":
            return c >= 0
        if op == "lt":
            return c < 0
        return c <= 0  # lte
    if op == "all":
        for sub in cond["conditions"]:
            if eval_condition(sub, step, ctx) is not True:
                return False
        return True
    if op == "any":
        for sub in cond["conditions"]:
            if eval_condition(sub, step, ctx) is True:
                return True
        return False
    # not
    return not eval_condition(cond["condition"], step, ctx)


# ---------------------------------------------------------------------------
# Reference resolution
# ---------------------------------------------------------------------------


def resolve_value(value, step, ctx):
    if isinstance(value, dict):
        if len(value) == 1 and "$ref" in value and isinstance(value["$ref"], str):
            return resolve_ref(value["$ref"], step, ctx)
        return {k: resolve_value(v, step, ctx) for k, v in value.items()}
    if isinstance(value, list):
        return [resolve_value(v, step, ctx) for v in value]
    return value


def resolve_ref(path, step, ctx):
    parts = path.split(".")
    target = parts[0]
    if target not in ctx.by_id:
        raise Err("INVALID_REFERENCE", "reference to unknown step %r" % (target,))
    if target not in ctx.trans[step.id]:
        raise Err(
            "INVALID_REFERENCE",
            "step %r is not a dependency of %r" % (target, step.id),
        )
    if target in ctx.skipped:
        raise Err("SKIPPED_REFERENCE", "reference to skipped step %r" % (target,))
    if target not in ctx.results:
        raise Err("INVALID_REFERENCE", "step %r has no result" % (target,))
    value = ctx.results[target]
    for seg in parts[1:]:
        if isinstance(value, dict):
            if seg not in value:
                raise Err(
                    "INVALID_REFERENCE",
                    "missing key %r in reference %r" % (seg, path),
                )
            value = value[seg]
        elif isinstance(value, list):
            if not INDEX_RE.match(seg):
                raise Err(
                    "INVALID_REFERENCE",
                    "invalid array index %r in reference %r" % (seg, path),
                )
            idx = int(seg)
            if idx >= len(value):
                raise Err(
                    "INVALID_REFERENCE",
                    "array index out of range in reference %r" % (path,),
                )
            value = value[idx]
        else:
            raise Err(
                "INVALID_REFERENCE",
                "cannot descend into non-container in reference %r" % (path,),
            )
    return value


def _subst_self(value, element):
    if isinstance(value, dict):
        if len(value) == 1 and value.get("$self") is True:
            return element
        return {k: _subst_self(v, element) for k, v in value.items()}
    if isinstance(value, list):
        return [_subst_self(v, element) for v in value]
    return value


# ---------------------------------------------------------------------------
# map transform / filter predicate
# ---------------------------------------------------------------------------


def _only_keys(obj, allowed):
    for k in obj:
        if k not in allowed:
            raise Err("INVALID_ARG", "unknown field %r" % (k,))


def apply_transform(t, element, ctx, step_id):
    if not isinstance(t, dict):
        raise Err("INVALID_ARG", "transform must be an object")
    typ = t.get("type")
    if not isinstance(typ, str):
        raise Err("INVALID_ARG", "transform type must be a string")

    if typ == "op":
        _only_keys(t, ("type", "op", "args"))
        op = t.get("op")
        if not isinstance(op, str) or op not in NUMERIC_TRANSFORMS:
            raise Err("INVALID_ARG", "transform op must be a 2-number op")
        targs = t.get("args", {})
        if not isinstance(targs, dict):
            raise Err("INVALID_ARG", "transform args must be an object")
        return apply_op(op, _subst_self(targs, element), ctx, step_id)

    if typ == "constant":
        _only_keys(t, ("type", "value"))
        if "value" not in t:
            raise Err("INVALID_ARG", "constant transform requires value")
        return copy.deepcopy(t["value"])

    if typ in NUMERIC_TRANSFORMS:
        _only_keys(t, ("type", "value"))
        if "value" not in t:
            raise Err("INVALID_ARG", "transform %r requires value" % (typ,))
        b = as_int(t["value"])
        a = as_int(element)
        if typ == "add":
            return iadd(a, b)
        if typ == "subtract":
            return isub(a, b)
        if typ == "multiply":
            return imul(a, b)
        if typ == "divide":
            return idiv(a, b)
        return imod(a, b)

    if typ == "negate":
        _only_keys(t, ("type",))
        return ineg(as_int(element))
    if typ == "abs":
        _only_keys(t, ("type",))
        return iabs(as_int(element))
    if typ == "stringify":
        _only_keys(t, ("type",))
        return json.dumps(element, separators=(",", ":"), ensure_ascii=False)
    if typ == "type":
        _only_keys(t, ("type",))
        return type_name(element)

    raise Err("INVALID_ARG", "unknown transform type %r" % (typ,))


def predicate_match(p, element):
    # The PRD shows the shorthand "predicate": "even"; the SPEC uses the
    # object form.  Accept the shorthand for predicates that need no value.
    if isinstance(p, str):
        if p in ("even", "odd", "truthy", "falsy"):
            p = {"type": p}
        else:
            raise Err("INVALID_ARG", "invalid predicate %r" % (p,))
    if not isinstance(p, dict):
        raise Err("INVALID_ARG", "predicate must be an object")
    typ = p.get("type")
    if not isinstance(typ, str):
        raise Err("INVALID_ARG", "predicate type must be a string")

    if typ in ("even", "odd"):
        _only_keys(p, ("type",))
        n = as_int(element)
        is_even = (n % 2) == 0
        return is_even if typ == "even" else not is_even

    if typ in ("gt", "gte", "lt", "lte", "eq", "ne"):
        _only_keys(p, ("type", "value"))
        if "value" not in p:
            raise Err("INVALID_ARG", "predicate %r requires value" % (typ,))
        c = compare_values(element, p["value"])
        if typ == "gt":
            return c > 0
        if typ == "gte":
            return c >= 0
        if typ == "lt":
            return c < 0
        if typ == "lte":
            return c <= 0
        if typ == "eq":
            return c == 0
        return c != 0

    if typ == "truthy":
        _only_keys(p, ("type",))
        return truthy(element)
    if typ == "falsy":
        _only_keys(p, ("type",))
        return not truthy(element)

    if typ == "type_is":
        # The SPEC writes {"type": "type_is", "type": "<type>"}, which cannot
        # survive normal JSON parsing as two keys.  Accept all sane spellings.
        _only_keys(p, ("type", "value", "type_name"))
        expected = None
        if isinstance(p.get("type_name"), str):
            expected = p["type_name"]
        elif isinstance(p.get("value"), str):
            expected = p["value"]
        if expected is None:
            raise Err("INVALID_ARG", "type_is requires the expected type")
        return type_name(element) == expected

    if typ in TYPE_NAMES:
        # Fallback for a serialized {"type":"type_is","type":"<type>"}
        # object whose duplicate key was collapsed to the expected type.
        _only_keys(p, ("type", "value", "type_name"))
        expected = typ
        if "value" in p and isinstance(p["value"], str):
            expected = p["value"]
        return type_name(element) == expected

    if typ == "string_contains":
        _only_keys(p, ("type", "value"))
        needle = p.get("value")
        if not isinstance(needle, str):
            raise Err("INVALID_ARG", "string_contains requires a string value")
        if not isinstance(element, str):
            raise Err("INVALID_ARG", "string_contains expects a string element")
        return needle in element

    raise Err("INVALID_ARG", "unknown predicate type %r" % (typ,))


# ---------------------------------------------------------------------------
# Operations
# ---------------------------------------------------------------------------

UPPER_TABLE = {i: i - 32 for i in range(ord("a"), ord("z") + 1)}
LOWER_TABLE = {i: i + 32 for i in range(ord("A"), ord("Z") + 1)}
ASCII_WS = " \t\n\r\f\v"


def apply_op(op, args, ctx, step_id):
    if not isinstance(args, dict):
        raise Err("INVALID_ARG", "args must be an object")

    # ----- math -----------------------------------------------------------
    if op in ("add", "subtract", "multiply", "divide", "mod"):
        check_args(args, ("a", "b"))
        a = as_int(args["a"])
        b = as_int(args["b"])
        if op == "add":
            return iadd(a, b)
        if op == "subtract":
            return isub(a, b)
        if op == "multiply":
            return imul(a, b)
        if op == "divide":
            return idiv(a, b)
        return imod(a, b)

    if op in ("sum", "min", "max"):
        check_args(args, ("input",))
        values = [as_int(v) for v in as_list(args["input"])]
        if op == "sum":
            acc = 0
            for v in values:
                acc = iadd(acc, v)
            return acc
        if not values:
            raise Err("EMPTY_ARRAY", "%s of an empty array" % op)
        if op == "min":
            return min(values)
        return max(values)

    if op == "range":
        check_args(args, ("start", "end"))
        start = as_int(args["start"])
        end = as_int(args["end"])
        if start > end:
            return []
        return list(range(start, end + 1))

    # ----- strings --------------------------------------------------------
    if op == "concat":
        check_args(args, ("a", "b"))
        return as_str(args["a"]) + as_str(args["b"])

    if op == "split":
        check_args(args, ("input", "sep"))
        text = as_str(args["input"])
        sep = as_str(args["sep"])
        if sep == "":
            return list(text)
        return text.split(sep)

    if op == "join":
        check_args(args, ("input", "sep"))
        items = as_list(args["input"])
        sep = as_str(args["sep"])
        for v in items:
            if not isinstance(v, str):
                raise Err("INVALID_ARG", "join expects an array of strings")
        return sep.join(items)

    if op == "uppercase":
        check_args(args, ("input",))
        return as_str(args["input"]).translate(UPPER_TABLE)

    if op == "lowercase":
        check_args(args, ("input",))
        return as_str(args["input"]).translate(LOWER_TABLE)

    if op == "trim":
        check_args(args, ("input",))
        return as_str(args["input"]).strip(ASCII_WS)

    if op == "replace":
        check_args(args, ("input", "search", "replace"))
        text = as_str(args["input"])
        search = as_str(args["search"])
        replacement = as_str(args["replace"])
        return text.replace(search, replacement)

    if op == "length":
        check_args(args, ("input",))
        return len(as_str(args["input"]))

    # ----- arrays ---------------------------------------------------------
    if op == "map":
        check_args(args, ("input", "transform"))
        items = as_list(args["input"])
        transform = args["transform"]
        return [apply_transform(transform, item, ctx, step_id) for item in items]

    if op == "filter":
        check_args(args, ("input", "predicate"))
        items = as_list(args["input"])
        predicate = args["predicate"]
        return [item for item in items if predicate_match(predicate, item)]

    if op == "reduce":
        check_args(args, ("input", "op"), ("initial",))
        items = as_list(args["input"])
        reduce_op = args["op"]
        if not isinstance(reduce_op, str) or reduce_op not in (
            "add",
            "subtract",
            "multiply",
        ):
            raise Err("INVALID_ARG", "reduce op must be add/subtract/multiply")
        if "initial" in args:
            acc = as_int(args["initial"])
        else:
            acc = 1 if reduce_op == "multiply" else 0
        for item in items:
            v = as_int(item)
            if reduce_op == "add":
                acc = iadd(acc, v)
            elif reduce_op == "subtract":
                acc = isub(acc, v)
            else:
                acc = imul(acc, v)
        return acc

    if op == "sort":
        check_args(args, ("input",))
        return sorted(as_list(args["input"]), key=cmp_to_key(compare_values))

    if op == "reverse":
        check_args(args, ("input",))
        return list(reversed(as_list(args["input"])))

    if op == "unique":
        check_args(args, ("input",))
        seen = set()
        out = []
        for item in as_list(args["input"]):
            key = canon(item)
            if key not in seen:
                seen.add(key)
                out.append(item)
        return out

    if op == "slice":
        check_args(args, ("input", "start"), ("end",))
        items = as_list(args["input"])
        start = as_int(args["start"])
        if "end" in args:
            end = as_int(args["end"])
        else:
            end = None
        return items[start:end]

    if op == "flatten":
        check_args(args, ("input",))
        out = []
        for item in as_list(args["input"]):
            if isinstance(item, list):
                out.extend(item)
            else:
                out.append(item)
        return out

    # ----- objects --------------------------------------------------------
    if op == "get":
        check_args(args, ("input", "key"))
        obj = as_obj(args["input"])
        key = as_str(args["key"])
        return obj.get(key)

    if op == "set":
        check_args(args, ("input", "key", "value"))
        obj = as_obj(args["input"])
        key = as_str(args["key"])
        out = dict(obj)
        out[key] = args["value"]
        return out

    if op == "keys":
        check_args(args, ("input",))
        return sorted(as_obj(args["input"]), key=utf8_key)

    if op == "values":
        check_args(args, ("input",))
        obj = as_obj(args["input"])
        return [obj[k] for k in sorted(obj, key=utf8_key)]

    if op == "merge":
        check_args(args, ("a", "b"))
        a = as_obj(args["a"])
        b = as_obj(args["b"])
        out = dict(a)
        out.update(b)
        return out

    if op == "pick":
        check_args(args, ("input", "keys"))
        obj = as_obj(args["input"])
        keys = as_list(args["keys"])
        for k in keys:
            if not isinstance(k, str):
                raise Err("INVALID_ARG", "pick keys must be strings")
        ordered = sorted(set(keys), key=utf8_key)
        return {k: obj.get(k) for k in ordered}

    if op == "omit":
        check_args(args, ("input", "keys"))
        obj = as_obj(args["input"])
        keys = as_list(args["keys"])
        for k in keys:
            if not isinstance(k, str):
                raise Err("INVALID_ARG", "omit keys must be strings")
        excluded = set(keys)
        return {k: v for k, v in obj.items() if k not in excluded}

    # ----- utilities ------------------------------------------------------
    if op == "constant":
        check_args(args, ("value",))
        return args["value"]

    if op == "equals":
        check_args(args, ("a", "b"))
        return structural_equal(args["a"], args["b"])

    if op == "compare":
        check_args(args, ("a", "b", "op"))
        cmp_op = args["op"]
        if not isinstance(cmp_op, str) or cmp_op not in ("gt", "gte", "lt", "lte"):
            raise Err("INVALID_ARG", "compare op must be gt/gte/lt/lte")
        c = compare_values(args["a"], args["b"])
        if cmp_op == "gt":
            return c > 0
        if cmp_op == "gte":
            return c >= 0
        if cmp_op == "lt":
            return c < 0
        return c <= 0

    if op == "and":
        check_args(args, ("a", "b"))
        return truthy(args["a"]) and truthy(args["b"])

    if op == "or":
        check_args(args, ("a", "b"))
        return truthy(args["a"]) or truthy(args["b"])

    if op == "not":
        check_args(args, ("a",))
        return not truthy(args["a"])

    if op == "type":
        check_args(args, ("value",))
        return type_name(args["value"])

    if op == "fail_until":
        check_args(args, ("failures", "value"))
        failures = as_int(args["failures"])
        if failures < 0 or failures > 10:
            raise Err("INVALID_ARG", "failures must be between 0 and 10")
        attempts = ctx.fail_counts.get(step_id, 0) + 1
        ctx.fail_counts[step_id] = attempts
        if attempts <= failures:
            raise Err("TRANSIENT_FAILURE", "transient failure on attempt %d" % attempts)
        return args["value"]

    raise Err("INVALID_WORKFLOW", "unknown op %r" % (op,))


# ---------------------------------------------------------------------------
# Structural validation
# ---------------------------------------------------------------------------


def validate_condition_shape(cond, step_id):
    validate_condition(cond, step_id)


def validate_document(doc):
    if not isinstance(doc, dict):
        raise Err("INVALID_WORKFLOW", "workflow must be a JSON object")
    for k in doc:
        if k not in ("version", "steps"):
            raise Err("INVALID_WORKFLOW", "unknown top-level field %r" % (k,))
    if "version" not in doc:
        raise Err("INVALID_WORKFLOW", "missing version")
    version = doc["version"]
    if isinstance(version, bool) or not isinstance(version, int) or version != 1:
        raise Err("INVALID_WORKFLOW", "version must be the integer 1")
    if "steps" not in doc:
        raise Err("INVALID_WORKFLOW", "missing steps")
    raw_steps = doc["steps"]
    if not isinstance(raw_steps, list):
        raise Err("INVALID_WORKFLOW", "steps must be an array")

    steps = []
    seen = set()
    for item in raw_steps:
        if not isinstance(item, dict):
            raise Err("INVALID_WORKFLOW", "each step must be an object")
        sid = item.get("id")
        if not isinstance(sid, str):
            raise Err("INVALID_WORKFLOW", "step id must be a string")
        for k in item:
            if k not in STEP_FIELDS:
                raise Err(
                    "INVALID_WORKFLOW", "unknown step field %r" % (k,), step=sid
                )
        if not ID_RE.match(sid):
            raise Err("INVALID_WORKFLOW", "invalid step id %r" % (sid,), step=sid)
        if sid in seen:
            raise Err("INVALID_WORKFLOW", "duplicate step id %r" % (sid,), step=sid)
        seen.add(sid)

        op = item.get("op")
        if not isinstance(op, str):
            raise Err("INVALID_WORKFLOW", "step op must be a string", step=sid)
        if op not in OPS:
            raise Err("INVALID_WORKFLOW", "unknown op %r" % (op,), step=sid)

        args = item.get("args", {})
        if not isinstance(args, dict):
            raise Err("INVALID_WORKFLOW", "args must be an object", step=sid)

        deps = item.get("depends_on", [])
        if not isinstance(deps, list):
            raise Err(
                "INVALID_WORKFLOW", "depends_on must be an array", step=sid
            )
        for d in deps:
            if not isinstance(d, str):
                raise Err(
                    "INVALID_WORKFLOW",
                    "depends_on entries must be strings",
                    step=sid,
                )

        max_attempts = 1
        if "retry" in item:
            retry = item["retry"]
            if not isinstance(retry, dict):
                raise Err("INVALID_WORKFLOW", "retry must be an object", step=sid)
            for k in retry:
                if k != "max_attempts":
                    raise Err(
                        "INVALID_WORKFLOW",
                        "unknown retry field %r" % (k,),
                        step=sid,
                    )
            if "max_attempts" in retry:
                m = retry["max_attempts"]
                if (
                    isinstance(m, bool)
                    or not isinstance(m, int)
                    or m < 1
                    or m > 5
                ):
                    raise Err(
                        "INVALID_WORKFLOW",
                        "max_attempts must be an integer between 1 and 5",
                        step=sid,
                    )
                max_attempts = m

        when = None
        if "when" in item:
            when = item["when"]
            validate_condition_shape(when, sid)

        steps.append(Step(sid, op, args, list(deps), when, max_attempts))

    by_id = {s.id: s for s in steps}

    # Dependency existence.
    for s in steps:
        for d in s.deps:
            if d not in by_id:
                raise Err(
                    "UNKNOWN_DEPENDENCY",
                    "step %r depends on unknown step %r" % (s.id, d),
                    step=s.id,
                )

    # Cycle detection (Kahn).
    indegree = {s.id: 0 for s in steps}
    dependents = {s.id: [] for s in steps}
    for s in steps:
        for d in set(s.deps):
            indegree[s.id] += 1
            dependents[d].append(s.id)
    queue = [s.id for s in steps if indegree[s.id] == 0]
    visited = 0
    while queue:
        cur = queue.pop()
        visited += 1
        for nxt in dependents[cur]:
            indegree[nxt] -= 1
            if indegree[nxt] == 0:
                queue.append(nxt)
    if visited != len(steps):
        raise Err("CYCLIC_DEPENDENCY", "workflow has a dependency cycle")

    return steps, by_id


def build_transitive(by_id):
    trans = {}
    for sid, step in by_id.items():
        out = set()
        stack = list(step.deps)
        while stack:
            d = stack.pop()
            if d in out:
                continue
            out.add(d)
            stack.extend(by_id[d].deps)
        trans[sid] = out
    return trans


# ---------------------------------------------------------------------------
# Execution
# ---------------------------------------------------------------------------


def execute(doc):
    steps, by_id = validate_document(doc)
    ctx = Ctx(by_id, build_transitive(by_id))

    order = []
    results = ctx.results
    skipped_order = []
    finished = set()
    remaining = set(range(len(steps)))

    while remaining:
        chosen = None
        for idx in range(len(steps)):
            if idx not in remaining:
                continue
            step = steps[idx]
            if all(d in finished for d in step.deps):
                chosen = idx
                break
        if chosen is None:  # validated DAG, so this is an internal bug
            raise Err("INVALID_WORKFLOW", "no executable step found")

        step = steps[chosen]
        remaining.discard(chosen)
        sid = step.id

        try:
            if step.when is not None:
                cond_value = eval_condition(step.when, step, ctx)
                if not isinstance(cond_value, bool):
                    raise Err(
                        "INVALID_CONDITION", "condition did not yield a boolean"
                    )
                if not cond_value:
                    results[sid] = None
                    ctx.skipped.add(sid)
                    skipped_order.append(sid)
                    finished.add(sid)
                    order.append(sid)
                    continue

            resolved = resolve_value(step.args, step, ctx)
            attempt = 0
            while True:
                attempt += 1
                try:
                    value = apply_op(step.op, resolved, ctx, sid)
                    break
                except Err as exc:
                    if exc.code == "TRANSIENT_FAILURE":
                        if attempt < step.max_attempts:
                            continue
                        raise Err(
                            "STEP_FAILED",
                            "step failed after %d attempt(s)" % attempt,
                            step=sid,
                        )
                    raise
            results[sid] = value
            finished.add(sid)
            order.append(sid)
        except Err as exc:
            if exc.step is None:
                exc.step = sid
            return response(
                "failed",
                results,
                order,
                skipped_order,
                {
                    "code": exc.code,
                    "step": exc.step,
                    "message": exc.message,
                },
            )

    return response("completed", results, order, skipped_order, None)


def response(status, results, order, skipped, error):
    return {
        "status": status,
        "results": results,
        "execution_order": order,
        "skipped": skipped,
        "error": error,
    }


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def _parse_float(text):
    value = float(text)
    if not math.isfinite(value):
        raise ValueError("non-finite number")
    return value


def _reject_constant(text):
    raise ValueError("invalid JSON constant %s" % text)


def load_document(argv):
    args = list(argv[1:])
    if args and args[0] == "run":
        args = args[1:]
    if len(args) != 1:
        raise Err("INVALID_WORKFLOW", "usage: workflow run <workflow.json>")
    target = args[0]
    try:
        if target == "-":
            raw = sys.stdin.buffer.read()
        else:
            with open(target, "rb") as handle:
                raw = handle.read()
    except OSError as exc:
        raise Err("INVALID_WORKFLOW", "cannot read input: %s" % exc)
    try:
        text = raw.decode("utf-8-sig")
    except UnicodeDecodeError:
        raise Err("INVALID_WORKFLOW", "input is not valid UTF-8")
    try:
        return json.loads(
            text,
            parse_float=_parse_float,
            parse_constant=_reject_constant,
        )
    except Exception as exc:  # noqa: BLE001 - any JSON failure is invalid input
        raise Err("INVALID_WORKFLOW", "invalid JSON: %s" % exc)


VALIDATION_CODES = {"INVALID_WORKFLOW", "UNKNOWN_DEPENDENCY", "CYCLIC_DEPENDENCY"}


def emit(doc):
    sys.stdout.write(
        json.dumps(doc, separators=(",", ":"), ensure_ascii=True) + "\n"
    )
    sys.stdout.flush()


def main(argv):
    try:
        doc = load_document(argv)
        result = execute(doc)
        emit(result)
        if result["status"] == "completed":
            return 0
        code = result["error"]["code"] if result["error"] else "STEP_FAILED"
        return 2 if code in VALIDATION_CODES else 1
    except Err as exc:
        emit(
            response(
                "failed",
                {},
                [],
                [],
                {
                    "code": exc.code,
                    "step": exc.step,
                    "message": exc.message,
                },
            )
        )
        return 2 if exc.code in VALIDATION_CODES else 1
    except Exception as exc:  # noqa: BLE001 - internal bug: best-effort JSON
        emit(
            response(
                "failed",
                {},
                [],
                [],
                {
                    "code": "INTERNAL_ERROR",
                    "step": None,
                    "message": str(exc),
                },
            )
        )
        return 3


if __name__ == "__main__":
    sys.exit(main(sys.argv))
