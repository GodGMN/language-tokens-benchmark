#!/usr/bin/env python3
"""Declarative workflow engine CLI.

Implements ../../../SPEC.md.  Standard library only.
"""

import json
import re
import sys
from decimal import Decimal

sys.setrecursionlimit(100000)
try:  # Python 3.11+ limits int<->str conversions; JSON values may be large.
    sys.set_int_max_str_digits(0)
except AttributeError:
    pass

INT64_MIN = -(2 ** 63)
INT64_MAX = 2 ** 63 - 1

ID_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*\Z")


# ---------------------------------------------------------------------------
# Errors
# ---------------------------------------------------------------------------

class WError(Exception):
    """A controlled workflow error."""

    def __init__(self, code, message="", step=None):
        super().__init__(message)
        self.code = code
        self.message = message
        self.step = step

    @property
    def exit_code(self):
        if self.code in ("INVALID_WORKFLOW", "UNKNOWN_DEPENDENCY",
                         "CYCLIC_DEPENDENCY"):
            return 2
        return 1


# ---------------------------------------------------------------------------
# JSON value helpers
# ---------------------------------------------------------------------------

def is_int(v):
    return isinstance(v, int) and not isinstance(v, bool)


def is_number(v):
    return (isinstance(v, int) and not isinstance(v, bool)) or isinstance(v, Decimal)


def is_i64(v):
    return is_int(v) and INT64_MIN <= v <= INT64_MAX


def type_name(v):
    if v is None:
        return "null"
    if isinstance(v, bool):
        return "boolean"
    if is_number(v):
        return "number"
    if isinstance(v, str):
        return "string"
    if isinstance(v, list):
        return "array"
    if isinstance(v, dict):
        return "object"
    raise WError("INVALID_ARG", "not a JSON value")


def need_i64(v, what="value"):
    if not is_i64(v):
        raise WError("INVALID_ARG", "%s must be an i64 integer" % what)
    return v


def checked(x):
    if x < INT64_MIN or x > INT64_MAX:
        raise WError("ARITHMETIC_OVERFLOW", "integer overflow")
    return x


def truthy(v):
    if v is None:
        return False
    if isinstance(v, bool):
        return v
    if is_number(v):
        return v != 0
    if isinstance(v, str):
        return v != ""
    if isinstance(v, list):
        return len(v) != 0
    if isinstance(v, dict):
        return len(v) != 0
    return False


def structural_eq(a, b):
    if is_number(a) and is_number(b):
        return a == b
    ta = type_name(a)
    tb = type_name(b)
    if ta != tb:
        return False
    if ta == "null":
        return True
    if ta == "boolean":
        return a is b
    if ta == "string":
        return a == b
    if ta == "array":
        if len(a) != len(b):
            return False
        return all(structural_eq(x, y) for x, y in zip(a, b))
    if ta == "object":
        if set(a.keys()) != set(b.keys()):
            return False
        return all(structural_eq(a[k], b[k]) for k in a)
    return False


def utf8_key(s):
    return s.encode("utf-8", "surrogatepass")


def compare_values(a, b):
    """Total order from SPEC section 3.  Returns -1, 0 or 1."""
    ra = _type_rank(a)
    rb = _type_rank(b)
    if ra != rb:
        return -1 if ra < rb else 1
    if ra == 0:  # number
        if a == b:
            return 0
        return -1 if a < b else 1
    if ra == 1:  # string
        ba = utf8_key(a)
        bb = utf8_key(b)
        if ba == bb:
            return 0
        return -1 if ba < bb else 1
    if ra == 2:  # boolean
        return 0 if a == b else (-1 if (not a) else 1)
    if ra == 3:  # null
        return 0
    if ra == 4:  # array
        n = min(len(a), len(b))
        for i in range(n):
            c = compare_values(a[i], b[i])
            if c != 0:
                return c
        if len(a) == len(b):
            return 0
        return -1 if len(a) < len(b) else 1
    # object: sorted key lists first, then values in that order
    ka = sorted(a.keys(), key=utf8_key)
    kb = sorted(b.keys(), key=utf8_key)
    n = min(len(ka), len(kb))
    for i in range(n):
        ba = utf8_key(ka[i])
        bb = utf8_key(kb[i])
        if ba != bb:
            return -1 if ba < bb else 1
    if len(ka) != len(kb):
        return -1 if len(ka) < len(kb) else 1
    for k in ka:
        c = compare_values(a[k], b[k])
        if c != 0:
            return c
    return 0


def _type_rank(v):
    if is_number(v):
        return 0
    if isinstance(v, str):
        return 1
    if isinstance(v, bool):
        return 2
    if v is None:
        return 3
    if isinstance(v, list):
        return 4
    if isinstance(v, dict):
        return 5
    raise WError("INVALID_ARG", "not a JSON value")


def sort_key(v):
    """Native-sort key implementing the SPEC total order (stable sort)."""
    if is_number(v):
        return (0, v)
    if isinstance(v, str):
        return (1, utf8_key(v))
    if isinstance(v, bool):
        return (2, 0 if not v else 1)
    if v is None:
        return (3,)
    if isinstance(v, list):
        return (4, tuple(sort_key(x) for x in v))
    if isinstance(v, dict):
        keys = sorted(v.keys(), key=utf8_key)
        return (5,
                tuple(utf8_key(k) for k in keys),
                tuple(sort_key(v[k]) for k in keys))
    raise WError("INVALID_ARG", "not a JSON value")


def canonical(v):
    """Hashable key preserving structural equality (numbers join across int/Decimal)."""
    if v is None:
        return ("null",)
    if isinstance(v, bool):
        return ("bool", v)
    if is_number(v):
        d = v if isinstance(v, Decimal) else Decimal(v)
        # Decimal's numeric hash/equality agrees with int equality.
        return ("num", d)
    if isinstance(v, str):
        return ("str", v)
    if isinstance(v, list):
        return ("arr", tuple(canonical(x) for x in v))
    if isinstance(v, dict):
        return ("obj", tuple((k, canonical(v[k])) for k in sorted(v.keys(), key=utf8_key)))
    raise WError("INVALID_ARG", "not a JSON value")


# ---------------------------------------------------------------------------
# JSON output
# ---------------------------------------------------------------------------

def dump_json(v, ascii_only=False):
    if v is None:
        return "null"
    if v is True:
        return "true"
    if v is False:
        return "false"
    if is_number(v):
        return str(v)
    if isinstance(v, str):
        return json.dumps(v, ensure_ascii=ascii_only)
    if isinstance(v, list):
        return "[" + ",".join(dump_json(x, ascii_only) for x in v) + "]"
    if isinstance(v, dict):
        return "{" + ",".join(
            json.dumps(k, ensure_ascii=ascii_only) + ":" + dump_json(val, ascii_only)
            for k, val in v.items()
        ) + "}"
    raise WError("INVALID_ARG", "not a JSON value")


# ---------------------------------------------------------------------------
# Conditions (shape validation)
# ---------------------------------------------------------------------------

CMP_OPS = ("eq", "ne", "gt", "gte", "lt", "lte")


def validation_error(message, step=None):
    return WError("INVALID_WORKFLOW", message, step)


def validate_condition(c, step):
    if not isinstance(c, dict):
        raise validation_error("condition must be an object", step)
    if "op" not in c or not isinstance(c["op"], str):
        raise validation_error("condition.op must be a string", step)
    op = c["op"]
    if op in CMP_OPS:
        for k in c:
            if k not in ("op", "left", "right"):
                raise validation_error("unknown condition field: %s" % k, step)
        if "left" not in c or "right" not in c:
            raise validation_error("condition requires left and right", step)
    elif op in ("all", "any"):
        for k in c:
            if k not in ("op", "conditions"):
                raise validation_error("unknown condition field: %s" % k, step)
        if "conditions" not in c or not isinstance(c["conditions"], list):
            raise validation_error("condition requires a conditions array", step)
        for sub in c["conditions"]:
            validate_condition(sub, step)
    elif op == "not":
        for k in c:
            if k not in ("op", "condition"):
                raise validation_error("unknown condition field: %s" % k, step)
        if "condition" not in c:
            raise validation_error("not requires a condition", step)
        validate_condition(c["condition"], step)
    else:
        raise validation_error("unknown condition operator: %s" % op, step)


# ---------------------------------------------------------------------------
# Workflow structural validation
# ---------------------------------------------------------------------------

OPS = frozenset([
    "add", "subtract", "multiply", "divide", "mod", "sum", "min", "max", "range",
    "concat", "split", "join", "uppercase", "lowercase", "trim", "replace", "length",
    "map", "filter", "reduce", "sort", "reverse", "unique", "slice", "flatten",
    "get", "set", "keys", "values", "merge", "pick", "omit",
    "constant", "equals", "compare", "and", "or", "not", "type",
    "fail_until",
])

STEP_FIELDS = frozenset(["id", "op", "args", "depends_on", "when", "retry"])


def validate_document(doc):
    if not isinstance(doc, dict):
        raise validation_error("workflow must be a JSON object")
    for k in doc:
        if k not in ("version", "steps"):
            raise validation_error("unknown workflow field: %s" % k)
    if "version" not in doc:
        raise validation_error("missing version")
    if not is_int(doc["version"]) or doc["version"] != 1:
        raise validation_error("version must be the integer 1")
    if "steps" not in doc:
        raise validation_error("missing steps")
    steps = doc["steps"]
    if not isinstance(steps, list):
        raise validation_error("steps must be an array")

    seen = set()
    for st in steps:
        if not isinstance(st, dict):
            raise validation_error("each step must be an object")
        report = st.get("id") if isinstance(st.get("id"), str) else None
        for k in st:
            if k not in STEP_FIELDS:
                raise validation_error("unknown step field: %s" % k, report)
        if "id" not in st or not isinstance(st["id"], str):
            raise validation_error("step id must be a string", report)
        sid = st["id"]
        report = sid
        if not ID_RE.match(sid):
            raise validation_error("invalid step id: %s" % sid, sid)
        if sid in seen:
            raise validation_error("duplicate step id: %s" % sid, sid)
        seen.add(sid)

        if "op" not in st or not isinstance(st["op"], str):
            raise validation_error("step op must be a string", sid)
        if st["op"] not in OPS:
            raise validation_error("unknown operation: %s" % st["op"], sid)

        if "args" in st and not isinstance(st["args"], dict):
            raise validation_error("args must be an object", sid)

        if "depends_on" in st:
            deps = st["depends_on"]
            if not isinstance(deps, list):
                raise validation_error("depends_on must be an array", sid)
            for d in deps:
                if not isinstance(d, str):
                    raise validation_error("depends_on entries must be strings", sid)

        if "when" in st:
            validate_condition(st["when"], sid)

        if "retry" in st:
            r = st["retry"]
            if not isinstance(r, dict):
                raise validation_error("retry must be an object", sid)
            for k in r:
                if k != "max_attempts":
                    raise validation_error("unknown retry field: %s" % k, sid)
            if "max_attempts" in r:
                m = r["max_attempts"]
                if not is_int(m) or m < 1 or m > 5:
                    raise validation_error("max_attempts must be an integer 1..5", sid)

    # Dependency existence.
    for st in steps:
        for d in st.get("depends_on", []):
            if d not in seen:
                raise WError("UNKNOWN_DEPENDENCY",
                             "unknown dependency: %s" % d, st["id"])

    # Cycle detection (Kahn).
    indeg = {}
    dependents = {sid: [] for sid in seen}
    for st in steps:
        sid = st["id"]
        deps = set(st.get("depends_on", []))
        indeg[sid] = len(deps)
        for d in deps:
            dependents[d].append(sid)
    ready = [st["id"] for st in steps if indeg[st["id"]] == 0]
    processed = 0
    while ready:
        sid = ready.pop()
        processed += 1
        for dep in dependents[sid]:
            indeg[dep] -= 1
            if indeg[dep] == 0:
                ready.append(dep)
    if processed != len(seen):
        raise WError("CYCLIC_DEPENDENCY", "dependency cycle detected")


# ---------------------------------------------------------------------------
# References
# ---------------------------------------------------------------------------

class RefContext:
    def __init__(self, allowed, results, skipped):
        self.allowed = allowed
        self.results = results
        self.skipped = skipped


def _is_ref_object(v):
    return isinstance(v, dict) and len(v) == 1 and isinstance(v.get("$ref"), str)


def resolve_value(v, ctx):
    if _is_ref_object(v):
        return resolve_ref(v["$ref"], ctx)
    if isinstance(v, dict):
        return {k: resolve_value(val, ctx) for k, val in v.items()}
    if isinstance(v, list):
        return [resolve_value(x, ctx) for x in v]
    return v


def resolve_ref(ref, ctx):
    parts = ref.split(".")
    sid = parts[0]
    if sid not in ctx.allowed:
        raise WError("INVALID_REFERENCE", "reference to non-dependency: %s" % ref)
    if sid in ctx.skipped:
        raise WError("SKIPPED_REFERENCE", "reference to skipped step: %s" % sid)
    if sid not in ctx.results:
        raise WError("INVALID_REFERENCE", "reference to unfinished step: %s" % sid)
    cur = ctx.results[sid]
    for seg in parts[1:]:
        if isinstance(cur, dict):
            if seg not in cur:
                raise WError("INVALID_REFERENCE", "missing object key in reference: %s" % ref)
            cur = cur[seg]
        elif isinstance(cur, list):
            if seg == "" or any(ch < "0" or ch > "9" for ch in seg):
                raise WError("INVALID_REFERENCE", "invalid array index in reference: %s" % ref)
            idx = int(seg)
            if idx >= len(cur):
                raise WError("INVALID_REFERENCE", "array index out of range: %s" % ref)
            cur = cur[idx]
        else:
            raise WError("INVALID_REFERENCE", "cannot descend into scalar: %s" % ref)
    return cur


def eval_condition(c, ctx):
    op = c["op"]
    if op in CMP_OPS:
        left = resolve_value(c["left"], ctx)
        right = resolve_value(c["right"], ctx)
        if op == "eq":
            return structural_eq(left, right)
        if op == "ne":
            return not structural_eq(left, right)
        cmp = compare_values(left, right)
        if op == "gt":
            return cmp > 0
        if op == "gte":
            return cmp >= 0
        if op == "lt":
            return cmp < 0
        return cmp <= 0
    if op == "all":
        for sub in c["conditions"]:
            v = eval_condition(sub, ctx)
            if not isinstance(v, bool):
                raise WError("INVALID_CONDITION", "condition did not yield a boolean")
            if not v:
                return False
        return True
    if op == "any":
        for sub in c["conditions"]:
            v = eval_condition(sub, ctx)
            if not isinstance(v, bool):
                raise WError("INVALID_CONDITION", "condition did not yield a boolean")
            if v:
                return True
        return False
    # not
    v = eval_condition(c["condition"], ctx)
    if not isinstance(v, bool):
        raise WError("INVALID_CONDITION", "condition did not yield a boolean")
    return not v


# ---------------------------------------------------------------------------
# Argument checking and arithmetic
# ---------------------------------------------------------------------------

def check_args(args, required=(), optional=()):
    if not isinstance(args, dict):
        raise WError("INVALID_ARG", "args must be an object")
    allowed = set(required) | set(optional)
    for k in args:
        if k not in allowed:
            raise WError("INVALID_ARG", "unknown argument: %s" % k)
    for k in required:
        if k not in args:
            raise WError("INVALID_ARG", "missing argument: %s" % k)


def get_i64(args, name):
    if name not in args:
        raise WError("INVALID_ARG", "missing argument: %s" % name)
    return need_i64(args[name], name)


def get_str(args, name):
    if name not in args:
        raise WError("INVALID_ARG", "missing argument: %s" % name)
    v = args[name]
    if not isinstance(v, str):
        raise WError("INVALID_ARG", "%s must be a string" % name)
    return v


def get_list(args, name):
    if name not in args:
        raise WError("INVALID_ARG", "missing argument: %s" % name)
    v = args[name]
    if not isinstance(v, list):
        raise WError("INVALID_ARG", "%s must be an array" % name)
    return v


def get_obj(args, name):
    if name not in args:
        raise WError("INVALID_ARG", "missing argument: %s" % name)
    v = args[name]
    if not isinstance(v, dict):
        raise WError("INVALID_ARG", "%s must be an object" % name)
    return v


def math_bin(op, a, b):
    a = need_i64(a, "a")
    b = need_i64(b, "b")
    if op == "add":
        return checked(a + b)
    if op == "subtract":
        return checked(a - b)
    if op == "multiply":
        return checked(a * b)
    if op == "divide":
        if b == 0:
            raise WError("DIVISION_BY_ZERO", "division by zero")
        if a == INT64_MIN and b == -1:
            raise WError("ARITHMETIC_OVERFLOW", "integer overflow")
        q = abs(a) // abs(b)
        if (a < 0) != (b < 0):
            q = -q
        return q
    if op == "mod":
        if b == 0:
            raise WError("DIVISION_BY_ZERO", "modulo by zero")
        if a == INT64_MIN and b == -1:
            raise WError("ARITHMETIC_OVERFLOW", "integer overflow")
        r = abs(a) % abs(b)
        if a < 0:
            r = -r
        return r
    raise WError("INVALID_ARG", "unknown arithmetic op: %s" % op)


def negate_i64(a):
    a = need_i64(a, "value")
    if a == INT64_MIN:
        raise WError("ARITHMETIC_OVERFLOW", "integer overflow")
    return -a


def abs_i64(a):
    a = need_i64(a, "value")
    if a == INT64_MIN:
        raise WError("ARITHMETIC_OVERFLOW", "integer overflow")
    return -a if a < 0 else a


# ---------------------------------------------------------------------------
# map transform / filter predicate
# ---------------------------------------------------------------------------

def _subst_self(v, elem):
    if isinstance(v, dict):
        if len(v) == 1 and v.get("$self") is True:
            return elem
        return {k: _subst_self(x, elem) for k, x in v.items()}
    if isinstance(v, list):
        return [_subst_self(x, elem) for x in v]
    return v


def validate_transform_shape(t):
    if isinstance(t, str):
        t = {"type": t}
    if not isinstance(t, dict):
        raise WError("INVALID_ARG", "transform must be an object")
    typ = t.get("type")
    if not isinstance(typ, str):
        raise WError("INVALID_ARG", "transform.type must be a string")
    if typ == "op":
        check_args(t, ("type", "op", "args"))
        op = t["op"]
        if not isinstance(op, str) or op not in OPS or op == "fail_until":
            raise WError("INVALID_ARG", "invalid transform op")
        if not isinstance(t["args"], dict):
            raise WError("INVALID_ARG", "transform op args must be an object")
    elif typ == "constant":
        for k in t:
            if k not in ("type", "value"):
                raise WError("INVALID_ARG", "unknown transform field: %s" % k)
    elif typ in ("add", "subtract", "multiply", "divide", "mod"):
        check_args(t, ("type", "value"))
        need_i64(t["value"], "value")
    elif typ in ("negate", "abs", "stringify", "type"):
        check_args(t, ("type",))
    else:
        raise WError("INVALID_ARG", "unknown transform type: %s" % typ)


def apply_transform(elem, t, state, step_id):
    if isinstance(t, str):
        t = {"type": t}
    if not isinstance(t, dict):
        raise WError("INVALID_ARG", "transform must be an object")
    typ = t.get("type")
    if not isinstance(typ, str):
        raise WError("INVALID_ARG", "transform.type must be a string")

    if typ == "op":
        check_args(t, ("type", "op", "args"))
        op = t["op"]
        if not isinstance(op, str) or op not in OPS:
            raise WError("INVALID_ARG", "invalid transform op")
        if op == "fail_until":
            raise WError("INVALID_ARG", "fail_until is not allowed as transform")
        sub = _subst_self(t["args"], elem)
        return execute_op(op, sub, state, step_id)

    if typ == "constant":
        for k in t:
            if k not in ("type", "value"):
                raise WError("INVALID_ARG", "unknown transform field: %s" % k)
        return t.get("value")

    if typ in ("add", "subtract", "multiply", "divide", "mod"):
        check_args(t, ("type", "value"))
        return math_bin(typ, elem, need_i64(t["value"], "value"))

    if typ == "negate":
        check_args(t, ("type",))
        return negate_i64(elem)

    if typ == "abs":
        check_args(t, ("type",))
        return abs_i64(elem)

    if typ == "stringify":
        check_args(t, ("type",))
        return dump_json(elem)

    if typ == "type":
        check_args(t, ("type",))
        return type_name(elem)

    raise WError("INVALID_ARG", "unknown transform type: %s" % typ)


def apply_predicate(elem, p):
    if isinstance(p, str):
        p = {"type": p}
    if not isinstance(p, dict):
        raise WError("INVALID_ARG", "predicate must be an object")
    typ = p.get("type")
    if not isinstance(typ, str):
        raise WError("INVALID_ARG", "predicate.type must be a string")

    if typ == "even":
        check_args(p, ("type",))
        v = need_i64(elem, "element")
        return v % 2 == 0
    if typ == "odd":
        check_args(p, ("type",))
        v = need_i64(elem, "element")
        return v % 2 != 0
    if typ in ("gt", "gte", "lt", "lte", "eq", "ne"):
        check_args(p, ("type", "value"))
        ref = need_i64(p["value"], "value")
        if typ == "eq":
            return structural_eq(elem, ref)
        if typ == "ne":
            return not structural_eq(elem, ref)
        cmp = compare_values(elem, ref)
        if typ == "gt":
            return cmp > 0
        if typ == "gte":
            return cmp >= 0
        if typ == "lt":
            return cmp < 0
        return cmp <= 0
    if typ == "truthy":
        check_args(p, ("type",))
        return truthy(elem)
    if typ == "falsy":
        check_args(p, ("type",))
        return not truthy(elem)
    if typ == "string_contains":
        check_args(p, ("type", "value"))
        needle = p["value"]
        if not isinstance(needle, str):
            raise WError("INVALID_ARG", "value must be a string")
        if not isinstance(elem, str):
            raise WError("INVALID_ARG", "element must be a string")
        return needle in elem
    raise WError("INVALID_ARG", "unknown predicate type: %s" % typ)


def validate_predicate_shape(p):
    if isinstance(p, str):
        p = {"type": p}
    if not isinstance(p, dict):
        raise WError("INVALID_ARG", "predicate must be an object")
    typ = p.get("type")
    if not isinstance(typ, str):
        raise WError("INVALID_ARG", "predicate.type must be a string")
    if typ in ("even", "odd", "truthy", "falsy"):
        check_args(p, ("type",))
    elif typ in ("gt", "gte", "lt", "lte", "eq", "ne"):
        check_args(p, ("type", "value"))
        need_i64(p["value"], "value")
    elif typ == "string_contains":
        check_args(p, ("type", "value"))
        if not isinstance(p["value"], str):
            raise WError("INVALID_ARG", "value must be a string")
    elif typ == "type_is":
        apply_predicate2(None, p)  # validates the shape
    elif typ in ("null", "boolean", "number", "string", "array", "object"):
        # Also accept the duplicate-key form after a lossy round trip, which
        # leaves {"type": "<type-name>"} behind.
        for k in p:
            if k != "type":
                raise WError("INVALID_ARG", "unknown predicate field: %s" % k)
    else:
        raise WError("INVALID_ARG", "unknown predicate type: %s" % typ)


def apply_predicate2(elem, p):
    """Predicate evaluation with the type_is special-case handled sanely.

    SPEC writes ``{"type": "type_is", "type": "<type>"}`` which contains a
    duplicate JSON key; the parser normalises that form into ``value``.  The
    ``type_name`` spelling is accepted as well.
    """
    if isinstance(p, str):
        return apply_predicate(elem, {"type": p})
    if not isinstance(p, dict):
        raise WError("INVALID_ARG", "predicate must be an object")
    if p.get("type") == "type_is" or p.get("type") in (
            "null", "boolean", "number", "string", "array", "object"):
        if p.get("type") != "type_is":
            name = p["type"]
            for k in p:
                if k != "type":
                    raise WError("INVALID_ARG", "unknown predicate field: %s" % k)
        else:
            name = None
            allowed = None
            for field in ("value", "type_name", "expected", "of",
                          "type_value", "name", "expected_type"):
                if field in p:
                    name = p[field]
                    allowed = ("type", field)
                    break
            if name is None:
                raise WError("INVALID_ARG", "type_is requires a type name")
            for k in p:
                if k not in allowed:
                    raise WError("INVALID_ARG", "unknown predicate field: %s" % k)
        if not isinstance(name, str):
            raise WError("INVALID_ARG", "type name must be a string")
        if name not in ("null", "boolean", "number", "string", "array", "object"):
            raise WError("INVALID_ARG", "invalid type name: %s" % name)
        return type_name(elem) == name
    return apply_predicate(elem, p)


# ---------------------------------------------------------------------------
# Operations
# ---------------------------------------------------------------------------

class ExecState:
    def __init__(self):
        self.fail_counts = {}


def execute_op(op, args, state, step_id):
    if not isinstance(args, dict):
        raise WError("INVALID_ARG", "args must be an object")

    # --- math ---
    if op in ("add", "subtract", "multiply", "divide", "mod"):
        check_args(args, ("a", "b"))
        return math_bin(op, args["a"], args["b"])

    if op == "sum":
        check_args(args, ("input",))
        arr = get_list(args, "input")
        total = 0
        for x in arr:
            total = checked(total + need_i64(x, "element"))
        return total

    if op in ("min", "max"):
        check_args(args, ("input",))
        arr = get_list(args, "input")
        if not arr:
            raise WError("EMPTY_ARRAY", "%s of empty array" % op)
        vals = [need_i64(x, "element") for x in arr]
        return min(vals) if op == "min" else max(vals)

    if op == "range":
        check_args(args, ("start", "end"))
        start = get_i64(args, "start")
        end = get_i64(args, "end")
        if start > end:
            return []
        return list(range(start, end + 1))

    # --- strings ---
    if op == "concat":
        check_args(args, ("a", "b"))
        return get_str(args, "a") + get_str(args, "b")

    if op == "split":
        check_args(args, ("input", "sep"))
        s = get_str(args, "input")
        sep = get_str(args, "sep")
        if sep == "":
            return list(s)
        return s.split(sep)

    if op == "join":
        check_args(args, ("input", "sep"))
        arr = get_list(args, "input")
        sep = get_str(args, "sep")
        for x in arr:
            if not isinstance(x, str):
                raise WError("INVALID_ARG", "join elements must be strings")
        return sep.join(arr)

    if op == "uppercase":
        check_args(args, ("input",))
        return get_str(args, "input").translate(
            str.maketrans("abcdefghijklmnopqrstuvwxyz",
                          "ABCDEFGHIJKLMNOPQRSTUVWXYZ"))

    if op == "lowercase":
        check_args(args, ("input",))
        return get_str(args, "input").translate(
            str.maketrans("ABCDEFGHIJKLMNOPQRSTUVWXYZ",
                          "abcdefghijklmnopqrstuvwxyz"))

    if op == "trim":
        check_args(args, ("input",))
        s = get_str(args, "input")
        ws = " \t\n\r\f\v"
        i = 0
        j = len(s)
        while i < j and s[i] in ws:
            i += 1
        while j > i and s[j - 1] in ws:
            j -= 1
        return s[i:j]

    if op == "replace":
        check_args(args, ("input", "search", "replace"))
        s = get_str(args, "input")
        search = get_str(args, "search")
        repl = get_str(args, "replace")
        if search == "":
            raise WError("INVALID_ARG", "search must not be empty")
        return s.replace(search, repl)

    if op == "length":
        check_args(args, ("input",))
        return len(get_str(args, "input"))

    # --- arrays ---
    if op == "map":
        check_args(args, ("input", "transform"))
        arr = get_list(args, "input")
        validate_transform_shape(args["transform"])
        return [apply_transform(x, args["transform"], state, step_id) for x in arr]

    if op == "filter":
        check_args(args, ("input", "predicate"))
        arr = get_list(args, "input")
        validate_predicate_shape(args["predicate"])
        return [x for x in arr if apply_predicate2(x, args["predicate"])]

    if op == "reduce":
        check_args(args, ("input", "op"), ("initial",))
        arr = get_list(args, "input")
        rop = args["op"]
        if not isinstance(rop, str) or rop not in ("add", "subtract", "multiply"):
            raise WError("INVALID_ARG", "reduce op must be add, subtract or multiply")
        if "initial" in args:
            acc = need_i64(args["initial"], "initial")
        else:
            acc = 1 if rop == "multiply" else 0
        for x in arr:
            acc = math_bin(rop, acc, need_i64(x, "element"))
        return acc

    if op == "sort":
        check_args(args, ("input",))
        arr = get_list(args, "input")
        return sorted(arr, key=sort_key)

    if op == "reverse":
        check_args(args, ("input",))
        return list(reversed(get_list(args, "input")))

    if op == "unique":
        check_args(args, ("input",))
        arr = get_list(args, "input")
        seen = set()
        out = []
        for x in arr:
            k = canonical(x)
            if k not in seen:
                seen.add(k)
                out.append(x)
        return out

    if op == "slice":
        check_args(args, ("input", "start"), ("end",))
        arr = get_list(args, "input")
        start = get_i64(args, "start")
        if "end" in args:
            end = get_i64(args, "end")
            return arr[start:end]
        return arr[start:]

    if op == "flatten":
        check_args(args, ("input",))
        arr = get_list(args, "input")
        out = []
        for x in arr:
            if isinstance(x, list):
                out.extend(x)
            else:
                out.append(x)
        return out

    # --- objects ---
    if op == "get":
        check_args(args, ("input", "key"))
        obj = get_obj(args, "input")
        key = get_str(args, "key")
        return obj.get(key)

    if op == "set":
        check_args(args, ("input", "key", "value"))
        obj = get_obj(args, "input")
        key = get_str(args, "key")
        out = dict(obj)
        out[key] = args["value"]
        return out

    if op == "keys":
        check_args(args, ("input",))
        obj = get_obj(args, "input")
        return sorted(obj.keys(), key=utf8_key)

    if op == "values":
        check_args(args, ("input",))
        obj = get_obj(args, "input")
        return [obj[k] for k in sorted(obj.keys(), key=utf8_key)]

    if op == "merge":
        check_args(args, ("a", "b"))
        a = get_obj(args, "a")
        b = get_obj(args, "b")
        out = dict(a)
        out.update(b)
        return out

    if op == "pick":
        check_args(args, ("input", "keys"))
        obj = get_obj(args, "input")
        keys = get_list(args, "keys")
        for k in keys:
            if not isinstance(k, str):
                raise WError("INVALID_ARG", "pick keys must be strings")
        out = {}
        for k in sorted(set(keys), key=utf8_key):
            out[k] = obj[k] if k in obj else None
        return out

    if op == "omit":
        check_args(args, ("input", "keys"))
        obj = get_obj(args, "input")
        keys = get_list(args, "keys")
        drop = set()
        for k in keys:
            if not isinstance(k, str):
                raise WError("INVALID_ARG", "omit keys must be strings")
            drop.add(k)
        return {k: v for k, v in obj.items() if k not in drop}

    # --- utilities ---
    if op == "constant":
        check_args(args, ("value",))
        return args["value"]

    if op == "equals":
        check_args(args, ("a", "b"))
        return structural_eq(args["a"], args["b"])

    if op == "compare":
        check_args(args, ("a", "b", "op"))
        cop = args["op"]
        if not isinstance(cop, str) or cop not in ("gt", "gte", "lt", "lte"):
            raise WError("INVALID_ARG", "compare op must be gt, gte, lt or lte")
        cmp = compare_values(args["a"], args["b"])
        if cop == "gt":
            return cmp > 0
        if cop == "gte":
            return cmp >= 0
        if cop == "lt":
            return cmp < 0
        return cmp <= 0

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

    # --- test op ---
    if op == "fail_until":
        check_args(args, ("failures", "value"))
        failures = get_i64(args, "failures")
        if failures < 0 or failures > 10:
            raise WError("INVALID_ARG", "failures must be between 0 and 10")
        n = state.fail_counts.get(step_id, 0)
        state.fail_counts[step_id] = n + 1
        if n < failures:
            raise WError("TRANSIENT_FAILURE", "transient failure")
        return args["value"]

    raise WError("INVALID_ARG", "unknown operation: %s" % op)


# ---------------------------------------------------------------------------
# Execution
# ---------------------------------------------------------------------------

def _dependency_closure(deps, memo, sid, stack):
    if sid in memo:
        return memo[sid]
    result = set()
    for d in deps[sid]:
        result.add(d)
        result |= _dependency_closure(deps, memo, d, stack)
    memo[sid] = result
    return result


def failure_response(results, order, skipped, err):
    return {
        "status": "failed",
        "results": results,
        "execution_order": order,
        "skipped": skipped,
        "error": {
            "code": err.code,
            "step": err.step,
            "message": err.message,
        },
    }


def run_document(doc):
    try:
        validate_document(doc)
    except WError as e:
        return failure_response({}, [], [], e), e.exit_code

    steps = doc["steps"]
    by_id = {st["id"]: st for st in steps}
    deps = {st["id"]: list(set(st.get("depends_on", []))) for st in steps}
    memo = {}
    closures = {
        sid: _dependency_closure(deps, memo, sid, set())
        for sid in by_id
    }

    state = ExecState()
    results = {}
    order = []
    skipped = []
    finished = set()
    remaining = list(range(len(steps)))

    while remaining:
        picked = None
        for i in remaining:
            sid = steps[i]["id"]
            if all(d in finished for d in deps[sid]):
                picked = i
                break
        if picked is None:
            e = WError("CYCLIC_DEPENDENCY", "dependency cycle detected")
            return failure_response(results, order, skipped, e), e.exit_code

        remaining.remove(picked)
        st = steps[picked]
        sid = st["id"]
        current = sid
        try:
            ctx = RefContext(closures[sid], results, set(skipped))

            if "when" in st:
                cv = eval_condition(st["when"], ctx)
                if not isinstance(cv, bool):
                    raise WError("INVALID_CONDITION",
                                 "condition did not yield a boolean")
                if not cv:
                    results[sid] = None
                    order.append(sid)
                    skipped.append(sid)
                    finished.add(sid)
                    continue

            raw_args = st.get("args", {})
            args = resolve_value(raw_args, ctx)
            if not isinstance(args, dict):
                raise WError("INVALID_ARG", "args must be an object")
            max_attempts = st.get("retry", {}).get("max_attempts", 1)
            attempt = 0
            while True:
                attempt += 1
                try:
                    value = execute_op(st["op"], args, state, sid)
                    break
                except WError as e:
                    if e.code == "TRANSIENT_FAILURE" and attempt < max_attempts:
                        continue
                    if e.code == "TRANSIENT_FAILURE":
                        raise WError("STEP_FAILED",
                                     "step failed after retries", sid)
                    raise
            results[sid] = value
            order.append(sid)
            finished.add(sid)
        except WError as e:
            if e.step is None:
                e.step = current
            return failure_response(results, order, skipped, e), e.exit_code

    return {
        "status": "completed",
        "results": results,
        "execution_order": order,
        "skipped": skipped,
        "error": None,
    }, 0


# ---------------------------------------------------------------------------
# Input / CLI
# ---------------------------------------------------------------------------

def _reject_constant(s):
    raise ValueError("invalid JSON numeric constant: %s" % s)


def _object_pairs(pairs):
    """Build an object, normalising the spec's duplicate-key type_is form."""
    obj = {}
    first_type = None
    saw_type = False
    for k, v in pairs:
        if k == "type":
            if saw_type:
                if first_type == "type_is":
                    # {"type": "type_is", "type": "number"} seen literally
                    obj = {"type": "type_is", "value": v}
                    continue
            else:
                saw_type = True
                first_type = v
        obj[k] = v
    return obj


def load_document(source):
    try:
        if source == "-":
            data = sys.stdin.buffer.read()
        else:
            with open(source, "rb") as f:
                data = f.read()
    except OSError as e:
        raise WError("INVALID_WORKFLOW", "cannot read input: %s" % e)

    try:
        text = data.decode("utf-8-sig")
    except UnicodeDecodeError as e:
        raise WError("INVALID_WORKFLOW", "input is not valid UTF-8: %s" % e)

    try:
        return json.loads(text, parse_float=Decimal,
                          parse_constant=_reject_constant,
                          object_pairs_hook=_object_pairs)
    except Exception as e:
        raise WError("INVALID_WORKFLOW", "invalid JSON: %s" % e)


def emit(resp):
    text = dump_json(resp)
    try:
        data = text.encode("utf-8")
    except UnicodeEncodeError:
        data = dump_json(resp, ascii_only=True).encode("utf-8", "backslashreplace")
    sys.stdout.buffer.write(data)
    sys.stdout.buffer.write(b"\n")
    sys.stdout.buffer.flush()


def main(argv):
    if len(argv) != 3 or argv[1] != "run":
        e = WError("INVALID_WORKFLOW",
                   "usage: workflow run <workflow.json | ->")
        emit(failure_response({}, [], [], e))
        return 2

    try:
        doc = load_document(argv[2])
        resp, code = run_document(doc)
        emit(resp)
        return code
    except WError as e:
        emit(failure_response({}, [], [], e))
        return e.exit_code
    except Exception as e:  # internal bug
        try:
            sys.stderr.write("internal error: %r\n" % (e,))
        except Exception:
            pass
        resp = {
            "status": "failed",
            "results": {},
            "execution_order": [],
            "skipped": [],
            "error": {
                "code": "INTERNAL_ERROR",
                "step": None,
                "message": str(e),
            },
        }
        try:
            emit(resp)
        except Exception:
            pass
        return 3


if __name__ == "__main__":
    sys.exit(main(sys.argv))
