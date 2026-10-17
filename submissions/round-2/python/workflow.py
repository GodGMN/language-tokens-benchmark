#!/usr/bin/env python3
"""Declarative workflow engine CLI.

Implements the contract in SPEC.md: `workflow run <file>` / `workflow run -`.

Standard library only. No user code execution, no network, no filesystem
side effects beyond reading the input document.
"""

import functools
import json
import re
import sys

sys.setrecursionlimit(50000)

INT64_MIN = -(2 ** 63)
INT64_MAX = 2 ** 63 - 1

TYPE_ORDER = {
    "number": 0,
    "string": 1,
    "boolean": 2,
    "null": 3,
    "array": 4,
    "object": 5,
}

TYPE_NAMES = ("null", "boolean", "number", "string", "array", "object")

ID_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
IDX_RE = re.compile(r"^[0-9]+$")

STEP_FIELDS = {"id", "op", "args", "depends_on", "when", "retry"}

CMP_OPS = {"eq", "ne", "gt", "gte", "lt", "lte"}

MATH_BINOPS = {"add", "subtract", "multiply", "divide", "mod"}

ALL_OPS = {
    # math
    "add", "subtract", "multiply", "divide", "mod",
    "sum", "min", "max", "range",
    # strings
    "concat", "split", "join", "uppercase", "lowercase", "trim",
    "replace", "length",
    # arrays
    "map", "filter", "reduce", "sort", "reverse", "unique", "slice",
    "flatten",
    # objects
    "get", "set", "keys", "values", "merge", "pick", "omit",
    # utilities
    "constant", "equals", "compare", "and", "or", "not", "type",
    # test op
    "fail_until",
}

EXIT_FOR_CODE = {
    "INVALID_WORKFLOW": 2,
    "UNKNOWN_DEPENDENCY": 2,
    "CYCLIC_DEPENDENCY": 2,
}
DEFAULT_EXIT = 1


class WFError(Exception):
    def __init__(self, code, message, step=None):
        super().__init__(message)
        self.code = code
        self.message = message
        self.step = step


class JFloat(float):
    """Marker subclass for JSON number literals containing '.' or 'e'."""

    __slots__ = ()


def _parse_float(text):
    v = JFloat(text)
    if v != v or v == float("inf") or v == float("-inf"):
        raise ValueError("number out of representable range: %s" % text)
    return v


# ---------------------------------------------------------------------------
# JSON value helpers
# ---------------------------------------------------------------------------

def jtype(v):
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
    raise WFError("INVALID_WORKFLOW", "unsupported value type")


def u8(s):
    try:
        return s.encode("utf-8")
    except UnicodeEncodeError:
        return s.encode("utf-8", "surrogatepass")


def hkey(v):
    """Hashable canonical key with the same equivalence as jeq()."""
    t = jtype(v)
    if t == "null":
        return ("null",)
    if t == "boolean":
        return ("boolean", v)
    if t == "number":
        return ("number", v)
    if t == "string":
        return ("string", v)
    if t == "array":
        return ("array", tuple(hkey(x) for x in v))
    return (
        "object",
        tuple(
            sorted(((k, hkey(x)) for k, x in v.items()), key=lambda kv: u8(kv[0]))
        ),
    )


def jeq(a, b):
    ta = jtype(a)
    tb = jtype(b)
    if ta != tb:
        return False
    if ta == "null":
        return True
    if ta == "number":
        return a == b
    if ta in ("boolean", "string"):
        return a == b
    if ta == "array":
        if len(a) != len(b):
            return False
        return all(jeq(x, y) for x, y in zip(a, b))
    if ta == "object":
        if set(a.keys()) != set(b.keys()):
            return False
        return all(jeq(a[k], b[k]) for k in a)
    return False


def jcmp(a, b):
    ta = jtype(a)
    tb = jtype(b)
    if ta != tb:
        return -1 if TYPE_ORDER[ta] < TYPE_ORDER[tb] else 1
    if ta == "null":
        return 0
    if ta in ("number", "boolean", "string"):
        # For strings python ordering is code-point order, which for UTF-8
        # byte-wise ordering is identical (UTF-8 preserves code point order).
        # We still use byte keys explicitly for lone-surrogate robustness.
        if ta == "string":
            ba = u8(a)
            bb = u8(b)
            return (ba > bb) - (ba < bb)
        return (a > b) - (a < b)
    if ta == "array":
        for x, y in zip(a, b):
            c = jcmp(x, y)
            if c:
                return c
        return (len(a) > len(b)) - (len(a) < len(b))
    if ta == "object":
        ka = sorted(a.keys(), key=u8)
        kb = sorted(b.keys(), key=u8)
        for x, y in zip(ka, kb):
            c = jcmp(x, y)
            if c:
                return c
        if len(ka) != len(kb):
            return -1 if len(ka) < len(kb) else 1
        for k in ka:
            c = jcmp(a[k], b[k])
            if c:
                return c
        return 0
    return 0


def jtruthy(v):
    t = jtype(v)
    if t == "null":
        return False
    if t == "boolean":
        return v
    if t == "number":
        return v != 0
    if t == "string":
        return len(v) != 0
    if t == "array":
        return len(v) != 0
    if t == "object":
        return len(v) != 0
    return False


def sort_key_cmp():
    return functools.cmp_to_key(jcmp)


# ---------------------------------------------------------------------------
# Integer helpers
# ---------------------------------------------------------------------------

def need_i64(v, what):
    if isinstance(v, bool) or not isinstance(v, int):
        raise WFError("INVALID_ARG", "%s must be an integer literal" % what)
    if v < INT64_MIN or v > INT64_MAX:
        raise WFError("INVALID_ARG", "%s out of i64 range" % what)
    return v


def fit_i64(x, what="result"):
    if x < INT64_MIN or x > INT64_MAX:
        raise WFError("ARITHMETIC_OVERFLOW", "%s overflows i64" % what)
    return x


def trunc_div(a, b):
    q = abs(a) // abs(b)
    if (a < 0) != (b < 0):
        q = -q
    return q


def do_binary(op, a, b):
    if op == "add":
        return fit_i64(a + b)
    if op == "subtract":
        return fit_i64(a - b)
    if op == "multiply":
        return fit_i64(a * b)
    if op == "divide":
        if b == 0:
            raise WFError("DIVISION_BY_ZERO", "division by zero")
        if a == INT64_MIN and b == -1:
            raise WFError("ARITHMETIC_OVERFLOW", "INT64_MIN / -1")
        return trunc_div(a, b)
    if op == "mod":
        if b == 0:
            raise WFError("DIVISION_BY_ZERO", "mod by zero")
        if a == INT64_MIN and b == -1:
            raise WFError("ARITHMETIC_OVERFLOW", "INT64_MIN % -1")
        return a - b * trunc_div(a, b)
    raise WFError("INVALID_ARG", "unknown binary op %r" % op)


# ---------------------------------------------------------------------------
# Args helpers
# ---------------------------------------------------------------------------

def check_args(args, required, optional=(), name=""):
    if not isinstance(args, dict):
        raise WFError("INVALID_ARG", "%s: args must be an object" % name)
    for k in required:
        if k not in args:
            raise WFError("INVALID_ARG", "%s: missing arg %r" % (name, k))
    allowed = set(required) | set(optional)
    for k in args:
        if k not in allowed:
            raise WFError("INVALID_ARG", "%s: unknown arg %r" % (name, k))


def need_str(v, what):
    if not isinstance(v, str):
        raise WFError("INVALID_ARG", "%s must be a string" % what)
    return v


def need_list(v, what):
    if not isinstance(v, list):
        raise WFError("INVALID_ARG", "%s must be an array" % what)
    return v


def need_obj(v, what):
    if not isinstance(v, dict):
        raise WFError("INVALID_ARG", "%s must be an object" % what)
    return v


def need_str_list(v, what):
    lst = need_list(v, what)
    out = []
    for i, item in enumerate(lst):
        if not isinstance(item, str):
            raise WFError("INVALID_ARG", "%s[%d] must be a string" % (what, i))
        out.append(item)
    return out


# ---------------------------------------------------------------------------
# References
# ---------------------------------------------------------------------------

def is_ref(v):
    return (
        isinstance(v, dict)
        and len(v) == 1
        and "$ref" in v
        and isinstance(v["$ref"], str)
    )


class RefCtx(object):
    def __init__(self, step_id, steps_by_id, transitive_deps, results, skipped):
        self.step_id = step_id
        self.steps_by_id = steps_by_id
        self.transitive_deps = transitive_deps
        self.results = results
        self.skipped = skipped


def resolve_ref(path, ctx):
    parts = path.split(".")
    target = parts[0]
    if target not in ctx.steps_by_id:
        raise WFError("INVALID_REFERENCE", "unknown step %r" % target)
    if target == ctx.step_id:
        raise WFError("INVALID_REFERENCE", "self reference to %r" % target)
    if target not in ctx.transitive_deps.get(ctx.step_id, set()):
        raise WFError(
            "INVALID_REFERENCE", "%r is not a dependency of %r" % (target, ctx.step_id)
        )
    if target in ctx.skipped:
        raise WFError("SKIPPED_REFERENCE", "reference to skipped step %r" % target)
    if target not in ctx.results:
        raise WFError("INVALID_REFERENCE", "step %r has no result" % target)
    cur = ctx.results[target]
    for seg in parts[1:]:
        if isinstance(cur, dict):
            if seg not in cur:
                raise WFError("INVALID_REFERENCE", "missing key %r in reference" % seg)
            cur = cur[seg]
        elif isinstance(cur, list):
            if not IDX_RE.match(seg):
                raise WFError(
                    "INVALID_REFERENCE", "invalid array index %r in reference" % seg
                )
            idx = int(seg)
            if idx >= len(cur):
                raise WFError(
                    "INVALID_REFERENCE", "array index %r out of range" % seg
                )
            cur = cur[idx]
        else:
            raise WFError(
                "INVALID_REFERENCE", "cannot descend into scalar with %r" % seg
            )
    return cur


def resolve_value(v, ctx):
    if is_ref(v):
        return resolve_ref(v["$ref"], ctx)
    if isinstance(v, dict):
        return {k: resolve_value(x, ctx) for k, x in v.items()}
    if isinstance(v, list):
        return [resolve_value(x, ctx) for x in v]
    return v


# ---------------------------------------------------------------------------
# Conditions
# ---------------------------------------------------------------------------

def eval_condition(cond, ctx):
    """Evaluate a condition (or a plain value/ref expression).

    Returns the raw JSON value; the caller enforces that the final result is a
    strict boolean.  Condition nodes are objects carrying an ``op`` key; every
    other value is a literal (possibly containing ``$ref``s).
    """
    if isinstance(cond, dict) and "op" in cond:
        op = cond.get("op")
        if not isinstance(op, str):
            raise WFError("INVALID_CONDITION", "condition op must be a string")
        if op in CMP_OPS:
            if "left" not in cond or "right" not in cond:
                raise WFError("INVALID_CONDITION", "condition missing operands")
            left = resolve_value(cond["left"], ctx)
            right = resolve_value(cond["right"], ctx)
            if op == "eq":
                return jeq(left, right)
            if op == "ne":
                return not jeq(left, right)
            c = jcmp(left, right)
            if op == "gt":
                return c > 0
            if op == "gte":
                return c >= 0
            if op == "lt":
                return c < 0
            return c <= 0
        if op == "all":
            subs = cond.get("conditions")
            if not isinstance(subs, list):
                raise WFError("INVALID_CONDITION", "all: conditions must be an array")
            for sub in subs:
                if not _bool_result(eval_condition(sub, ctx)):
                    return False
            return True
        if op == "any":
            subs = cond.get("conditions")
            if not isinstance(subs, list):
                raise WFError("INVALID_CONDITION", "any: conditions must be an array")
            for sub in subs:
                if _bool_result(eval_condition(sub, ctx)):
                    return True
            return False
        if op == "not":
            return not _bool_result(eval_condition(cond.get("condition"), ctx))
        raise WFError("INVALID_CONDITION", "unknown condition op %r" % (op,))
    # Plain value / reference expression.
    return resolve_value(cond, ctx)


def _bool_result(v):
    if not isinstance(v, bool):
        raise WFError("INVALID_CONDITION", "condition did not yield a boolean")
    return v


# ---------------------------------------------------------------------------
# map / filter helpers
# ---------------------------------------------------------------------------

def substitute_self(v, elem):
    if isinstance(v, dict):
        if len(v) == 1 and "$self" in v:
            return elem
        return {k: substitute_self(x, elem) for k, x in v.items()}
    if isinstance(v, list):
        return [substitute_self(x, elem) for x in v]
    return v


def apply_transform(t, elem, ctx):
    t = need_obj(t, "map.transform")
    typ = t.get("type")
    if not isinstance(typ, str):
        raise WFError("INVALID_ARG", "map.transform.type must be a string")
    if typ == "op":
        check_args(t, ("type", "op"), ("args",), "map.transform")
        opname = need_str(t["op"], "map.transform.op")
        if opname not in ALL_OPS:
            raise WFError("INVALID_ARG", "map.transform: unknown op %r" % opname)
        raw_args = t.get("args", {})
        args = substitute_self(raw_args, elem)
        return execute_op(opname, args, ctx)
    if typ == "constant":
        check_args(t, ("type", "value"), (), "map.transform")
        return t["value"]
    if typ in MATH_BINOPS:
        check_args(t, ("type", "value"), (), "map.transform")
        val = need_i64(t["value"], "map.transform.value")
        ev = need_i64(elem, "map element")
        return do_binary(typ, ev, val)
    if typ == "negate":
        check_args(t, ("type",), (), "map.transform")
        ev = need_i64(elem, "map element")
        if ev == INT64_MIN:
            raise WFError("ARITHMETIC_OVERFLOW", "negate overflow")
        return -ev
    if typ == "abs":
        check_args(t, ("type",), (), "map.transform")
        ev = need_i64(elem, "map element")
        if ev == INT64_MIN:
            raise WFError("ARITHMETIC_OVERFLOW", "abs overflow")
        return abs(ev)
    if typ == "stringify":
        check_args(t, ("type",), (), "map.transform")
        return json.dumps(elem, ensure_ascii=True, separators=(",", ":"),
                          allow_nan=False)
    if typ == "type":
        check_args(t, ("type",), (), "map.transform")
        return jtype(elem)
    raise WFError("INVALID_ARG", "map.transform: unknown type %r" % typ)


def apply_predicate(p, elem, ctx):
    p = need_obj(p, "filter.predicate")
    typ = p.get("type")
    if not isinstance(typ, str):
        raise WFError("INVALID_ARG", "filter.predicate.type must be a string")
    if typ in ("even", "odd"):
        check_args(p, ("type",), (), "filter.predicate")
        ev = need_i64(elem, "filter element")
        r = ev % 2 == 0
        return r if typ == "even" else not r
    if typ in ("gt", "gte", "lt", "lte"):
        check_args(p, ("type", "value"), (), "filter.predicate")
        val = need_i64(p["value"], "filter.predicate.value")
        c = jcmp(elem, val)
        if typ == "gt":
            return c > 0
        if typ == "gte":
            return c >= 0
        if typ == "lt":
            return c < 0
        return c <= 0
    if typ in ("eq", "ne"):
        check_args(p, ("type", "value"), (), "filter.predicate")
        r = jeq(elem, p["value"])
        return r if typ == "eq" else not r
    if typ in ("truthy", "falsy"):
        check_args(p, ("type",), (), "filter.predicate")
        r = jtruthy(elem)
        return r if typ == "truthy" else not r
    if typ == "type_is":
        # SPEC writes `{"type": "type_is", "type": "<type>"}`, which would
        # collide with the discriminator key.  Accept the type name from the
        # usual carriers, or from any other key holding a known type name.
        target = None
        for cand in ("value", "type_name", "name", "target"):
            if cand in p:
                target = p[cand]
                break
        if target is None:
            for k, v in p.items():
                if k != "type" and isinstance(v, str) and v in TYPE_NAMES:
                    target = v
                    break
        if target is None:
            raise WFError(
                "INVALID_ARG", "filter.predicate: type_is needs a type name"
            )
        wanted = need_str(target, "filter.predicate value")
        if wanted not in TYPE_NAMES:
            raise WFError("INVALID_ARG", "filter.predicate: unknown type %r" % wanted)
        return jtype(elem) == wanted
    if typ == "string_contains":
        check_args(p, ("type", "value"), (), "filter.predicate")
        needle = need_str(p["value"], "filter.predicate.value")
        hay = need_str(elem, "filter element")
        return needle in hay
    raise WFError("INVALID_ARG", "filter.predicate: unknown type %r" % typ)


# ---------------------------------------------------------------------------
# Operation execution
# ---------------------------------------------------------------------------

class ExecCtx(object):
    def __init__(self):
        self.fail_counters = {}
        self.current_step = None


def execute_op(name, args, ctx):
    if name not in ALL_OPS:
        raise WFError("INVALID_ARG", "unknown op %r" % (name,))
    if not isinstance(args, dict):
        raise WFError("INVALID_ARG", "%s: args must be an object" % name)

    # --- math -------------------------------------------------------------
    if name in MATH_BINOPS:
        check_args(args, ("a", "b"), (), name)
        a = need_i64(args["a"], "a")
        b = need_i64(args["b"], "b")
        return do_binary(name, a, b)

    if name == "sum":
        check_args(args, ("input",), (), name)
        arr = need_list(args["input"], "input")
        total = 0
        for item in arr:
            total = fit_i64(total + need_i64(item, "sum element"))
        return total

    if name in ("min", "max"):
        check_args(args, ("input",), (), name)
        arr = need_list(args["input"], "input")
        if not arr:
            raise WFError("EMPTY_ARRAY", "%s on empty array" % name)
        vals = [need_i64(x, "%s element" % name) for x in arr]
        return min(vals) if name == "min" else max(vals)

    if name == "range":
        check_args(args, ("start", "end"), (), name)
        start = need_i64(args["start"], "start")
        end = need_i64(args["end"], "end")
        if start > end:
            return []
        return list(range(start, end + 1))

    # --- strings ----------------------------------------------------------
    if name == "concat":
        check_args(args, ("a", "b"), (), name)
        return need_str(args["a"], "a") + need_str(args["b"], "b")

    if name == "split":
        check_args(args, ("input", "sep"), (), name)
        s = need_str(args["input"], "input")
        sep = need_str(args["sep"], "sep")
        if sep == "":
            return list(s)
        return s.split(sep)

    if name == "join":
        check_args(args, ("input", "sep"), (), name)
        arr = need_str_list(args["input"], "input")
        sep = need_str(args["sep"], "sep")
        return sep.join(arr)

    if name in ("uppercase", "lowercase"):
        check_args(args, ("input",), (), name)
        s = need_str(args["input"], "input")
        if name == "uppercase":
            return s.translate(_ASCII_UPPER)
        return s.translate(_ASCII_LOWER)

    if name == "trim":
        check_args(args, ("input",), (), name)
        s = need_str(args["input"], "input")
        return s.strip(" \t\n\r\f\v")

    if name == "replace":
        check_args(args, ("input", "search", "replace"), (), name)
        s = need_str(args["input"], "input")
        search = need_str(args["search"], "search")
        repl = need_str(args["replace"], "replace")
        if search == "":
            raise WFError("INVALID_ARG", "replace: empty search")
        return s.replace(search, repl)

    if name == "length":
        check_args(args, ("input",), (), name)
        s = need_str(args["input"], "input")
        return len(s)

    # --- arrays -----------------------------------------------------------
    if name == "map":
        check_args(args, ("input", "transform"), (), name)
        arr = need_list(args["input"], "input")
        tr = args["transform"]
        return [apply_transform(tr, elem, ctx) for elem in arr]

    if name == "filter":
        check_args(args, ("input", "predicate"), (), name)
        arr = need_list(args["input"], "input")
        pred = args["predicate"]
        out = []
        for elem in arr:
            if apply_predicate(pred, elem, ctx):
                out.append(elem)
        return out

    if name == "reduce":
        check_args(args, ("input", "op"), ("initial",), name)
        arr = need_list(args["input"], "input")
        rop = need_str(args["op"], "op")
        if rop not in MATH_BINOPS:
            raise WFError("INVALID_ARG", "reduce: unsupported op %r" % rop)
        if rop in ("add", "subtract"):
            acc = 0
        else:
            acc = 1
        if "initial" in args:
            acc = need_i64(args["initial"], "initial")
        for elem in arr:
            ev = need_i64(elem, "reduce element")
            acc = fit_i64(do_binary(rop, acc, ev))
        return acc

    if name == "sort":
        check_args(args, ("input",), (), name)
        arr = need_list(args["input"], "input")
        return sorted(arr, key=sort_key_cmp())

    if name == "reverse":
        check_args(args, ("input",), (), name)
        arr = need_list(args["input"], "input")
        return list(reversed(arr))

    if name == "unique":
        check_args(args, ("input",), (), name)
        arr = need_list(args["input"], "input")
        out = []
        seen = set()
        for elem in arr:
            k = hkey(elem)
            if k not in seen:
                seen.add(k)
                out.append(elem)
        return out

    if name == "slice":
        check_args(args, ("input", "start"), ("end",), name)
        arr = need_list(args["input"], "input")
        n = len(arr)
        start = need_i64(args["start"], "start")
        if "end" in args:
            end = need_i64(args["end"], "end")
        else:
            end = n
        if start < 0:
            start = n + start
            if start < 0:
                start = 0
        elif start > n:
            start = n
        if end < 0:
            end = n + end
            if end < 0:
                end = 0
        elif end > n:
            end = n
        if start > end:
            return []
        return arr[start:end]

    if name == "flatten":
        check_args(args, ("input",), (), name)
        arr = need_list(args["input"], "input")
        out = []
        for elem in arr:
            if isinstance(elem, list):
                out.extend(elem)
            else:
                out.append(elem)
        return out

    # --- objects ----------------------------------------------------------
    if name == "get":
        check_args(args, ("input", "key"), (), name)
        obj = need_obj(args["input"], "input")
        key = need_str(args["key"], "key")
        return obj[key] if key in obj else None

    if name == "set":
        check_args(args, ("input", "key", "value"), (), name)
        obj = need_obj(args["input"], "input")
        key = need_str(args["key"], "key")
        out = dict(obj)
        out[key] = args["value"]
        return out

    if name == "keys":
        check_args(args, ("input",), (), name)
        obj = need_obj(args["input"], "input")
        return sorted(obj.keys(), key=u8)

    if name == "values":
        check_args(args, ("input",), (), name)
        obj = need_obj(args["input"], "input")
        return [obj[k] for k in sorted(obj.keys(), key=u8)]

    if name == "merge":
        check_args(args, ("a", "b"), (), name)
        a = need_obj(args["a"], "a")
        b = need_obj(args["b"], "b")
        out = dict(a)
        out.update(b)
        return out

    if name == "pick":
        check_args(args, ("input", "keys"), (), name)
        obj = need_obj(args["input"], "input")
        keys = need_str_list(args["keys"], "keys")
        out = {}
        for k in sorted(set(keys), key=u8):
            out[k] = obj[k] if k in obj else None
        return out

    if name == "omit":
        check_args(args, ("input", "keys"), (), name)
        obj = need_obj(args["input"], "input")
        keys = set(need_str_list(args["keys"], "keys"))
        return {k: v for k, v in obj.items() if k not in keys}

    # --- utilities --------------------------------------------------------
    if name == "constant":
        check_args(args, ("value",), (), name)
        return args["value"]

    if name == "equals":
        check_args(args, ("a", "b"), (), name)
        return jeq(args["a"], args["b"])

    if name == "compare":
        check_args(args, ("a", "b", "op"), (), name)
        cop = need_str(args["op"], "op")
        if cop not in ("gt", "gte", "lt", "lte"):
            raise WFError("INVALID_ARG", "compare: bad op %r" % cop)
        c = jcmp(args["a"], args["b"])
        if cop == "gt":
            return c > 0
        if cop == "gte":
            return c >= 0
        if cop == "lt":
            return c < 0
        return c <= 0

    if name == "and":
        check_args(args, ("a", "b"), (), name)
        return bool(jtruthy(args["a"]) and jtruthy(args["b"]))

    if name == "or":
        check_args(args, ("a", "b"), (), name)
        return bool(jtruthy(args["a"]) or jtruthy(args["b"]))

    if name == "not":
        check_args(args, ("a",), (), name)
        return not jtruthy(args["a"])

    if name == "type":
        check_args(args, ("value",), (), name)
        return jtype(args["value"])

    if name == "fail_until":
        check_args(args, ("failures", "value"), (), name)
        failures = need_i64(args["failures"], "failures")
        if failures < 0 or failures > 10:
            raise WFError("INVALID_ARG", "fail_until: failures out of range")
        key = (ctx.current_step, "fail_until")
        count = ctx.fail_counters.get(key, 0) + 1
        ctx.fail_counters[key] = count
        if count <= failures:
            raise WFError(
                "TRANSIENT_FAILURE", "transient failure (attempt %d)" % count
            )
        return args["value"]

    raise WFError("INVALID_ARG", "unknown op %r" % (name,))


_ASCII_UPPER = str.maketrans(
    "abcdefghijklmnopqrstuvwxyz", "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
)
_ASCII_LOWER = str.maketrans(
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ", "abcdefghijklmnopqrstuvwxyz"
)


# ---------------------------------------------------------------------------
# Structural validation
# ---------------------------------------------------------------------------

def _vworkflow_fail(msg, step=None):
    raise WFError("INVALID_WORKFLOW", msg, step=step)


def validate_condition_shape(cond, step_id):
    """Validate the structural shape of a `when` condition.

    The condition operator *names* are a runtime concern (an unknown operator
    surfaces as INVALID_CONDITION while evaluating), but the JSON shape of the
    condition tree is validated before any step runs.
    """
    if not isinstance(cond, dict):
        _vworkflow_fail("when must be an object", step_id)
    op = cond.get("op")
    if not isinstance(op, str):
        _vworkflow_fail("when.op must be a string", step_id)
    if op in CMP_OPS:
        if "left" not in cond or "right" not in cond:
            _vworkflow_fail("when: comparison needs left and right", step_id)
    elif op in ("all", "any"):
        subs = cond.get("conditions")
        if not isinstance(subs, list):
            _vworkflow_fail("when: %s needs a conditions array" % op, step_id)
        for sub in subs:
            if isinstance(sub, dict):
                validate_condition_shape(sub, step_id)
    elif op == "not":
        sub = cond.get("condition")
        if not isinstance(sub, dict):
            _vworkflow_fail("when: not needs a condition object", step_id)
        validate_condition_shape(sub, step_id)


def validate_document(doc):
    """Validate structure. Returns list of validated step dicts (normalized)."""
    if not isinstance(doc, dict):
        _vworkflow_fail("workflow document must be an object")
    for key in doc:
        if key not in ("version", "steps"):
            _vworkflow_fail("unknown top-level field %r" % (key,))
    if "version" not in doc:
        _vworkflow_fail("missing version")
    version = doc["version"]
    if isinstance(version, bool) or not isinstance(version, int) or version != 1:
        _vworkflow_fail("version must be the integer 1")
    if "steps" not in doc:
        _vworkflow_fail("missing steps")
    steps = doc["steps"]
    if not isinstance(steps, list):
        _vworkflow_fail("steps must be an array")

    seen = set()
    normalized = []
    for idx, raw in enumerate(steps):
        if not isinstance(raw, dict):
            _vworkflow_fail("step[%d] must be an object" % idx)
        # determine error-step id early (string id even if invalid pattern)
        raw_id = raw.get("id")
        sid = raw_id if isinstance(raw_id, str) else None
        for key in raw:
            if key not in STEP_FIELDS:
                _vworkflow_fail("step[%d]: unknown field %r" % (idx, key), sid)
        if "id" not in raw:
            _vworkflow_fail("step[%d]: missing id" % idx, None)
        if not isinstance(raw_id, str):
            _vworkflow_fail("step[%d]: id must be a string" % idx, None)
        if not ID_RE.match(raw_id):
            _vworkflow_fail("step[%d]: invalid id %r" % (idx, raw_id), sid)
        if raw_id in seen:
            _vworkflow_fail("duplicate step id %r" % raw_id, sid)
        seen.add(raw_id)

        if "op" not in raw:
            _vworkflow_fail("step %r: missing op" % raw_id, sid)
        op = raw["op"]
        if not isinstance(op, str):
            _vworkflow_fail("step %r: op must be a string" % raw_id, sid)
        if op not in ALL_OPS:
            _vworkflow_fail("step %r: unknown op %r" % (raw_id, op), sid)

        args = raw.get("args", {})
        if not isinstance(args, dict):
            _vworkflow_fail("step %r: args must be an object" % raw_id, sid)

        depends = raw.get("depends_on", [])
        if not isinstance(depends, list):
            _vworkflow_fail("step %r: depends_on must be an array" % raw_id, sid)
        dep_list = []
        for i, d in enumerate(depends):
            if not isinstance(d, str):
                _vworkflow_fail(
                    "step %r: depends_on[%d] must be a string" % (raw_id, i), sid
                )
            dep_list.append(d)

        when = raw.get("when")
        if "when" in raw:
            validate_condition_shape(when, sid)

        retry = raw.get("retry")
        max_attempts = 1
        if "retry" in raw:
            if not isinstance(retry, dict):
                _vworkflow_fail("step %r: retry must be an object" % raw_id, sid)
            for key in retry:
                if key != "max_attempts":
                    _vworkflow_fail(
                        "step %r: unknown retry field %r" % (raw_id, key), sid
                    )
            ma = retry.get("max_attempts", 1)
            if isinstance(ma, bool) or not isinstance(ma, int):
                _vworkflow_fail(
                    "step %r: retry.max_attempts must be an integer" % raw_id, sid
                )
            if ma < 1 or ma > 5:
                _vworkflow_fail(
                    "step %r: retry.max_attempts out of range" % raw_id, sid
                )
            max_attempts = ma

        normalized.append(
            {
                "id": raw_id,
                "op": op,
                "args": args,
                "depends_on": dep_list,
                "when": when if "when" in raw else None,
                "has_when": "when" in raw,
                "max_attempts": max_attempts,
            }
        )

    id_set = set(seen)
    for step in normalized:
        for d in step["depends_on"]:
            if d not in id_set:
                raise WFError(
                    "UNKNOWN_DEPENDENCY",
                    "step %r depends on unknown step %r" % (step["id"], d),
                    step=step["id"],
                )

    # cycle detection (Kahn)
    indeg = {s["id"]: 0 for s in normalized}
    adj = {s["id"]: [] for s in normalized}
    for s in normalized:
        for d in s["depends_on"]:
            adj[d].append(s["id"])
            indeg[s["id"]] += 1
    queue = [s["id"] for s in normalized if indeg[s["id"]] == 0]
    processed = 0
    qi = 0
    while qi < len(queue):
        n = queue[qi]
        qi += 1
        processed += 1
        for m in adj[n]:
            indeg[m] -= 1
            if indeg[m] == 0:
                queue.append(m)
    if processed != len(normalized):
        raise WFError("CYCLIC_DEPENDENCY", "dependency cycle detected", step=None)

    return normalized


def compute_transitive_deps(normalized):
    by_id = {s["id"]: s for s in normalized}
    memo = {}

    def visit(sid):
        if sid in memo:
            return memo[sid]
        out = set()
        for d in by_id[sid]["depends_on"]:
            out.add(d)
            out |= visit(d)
        memo[sid] = out
        return out

    for s in normalized:
        visit(s["id"])
    return memo


# ---------------------------------------------------------------------------
# Execution
# ---------------------------------------------------------------------------

def run_workflow(doc):
    normalized = validate_document(doc)
    transitive = compute_transitive_deps(normalized)
    by_id = {s["id"]: s for s in normalized}
    ctx = ExecCtx()
    results = {}
    execution_order = []
    skipped = []
    finished = set()

    pending = [s["id"] for s in normalized]

    try:
        _execute_loop(
            pending, by_id, transitive, ctx, results,
            execution_order, skipped, finished,
        )
    except WFError as exc:
        if exc.code in EXIT_FOR_CODE:
            raise
        return error_response(
            exc.code,
            exc.message,
            step=exc.step,
            results=results,
            execution_order=execution_order,
            skipped=skipped,
        ), EXIT_FOR_CODE.get(exc.code, DEFAULT_EXIT)

    return {
        "status": "completed",
        "results": results,
        "execution_order": execution_order,
        "skipped": skipped,
        "error": None,
    }, 0


def _execute_loop(pending, by_id, transitive, ctx, results,
                  execution_order, skipped, finished):
    while pending:
        chosen = None
        for sid in pending:
            step = by_id[sid]
            if all(d in finished for d in step["depends_on"]):
                chosen = sid
                break
        if chosen is None:
            # Should not happen after cycle validation.
            raise WFError("CYCLIC_DEPENDENCY", "no runnable step", step=None)
        pending.remove(chosen)
        step = by_id[chosen]
        rc = RefCtx(chosen, by_id, transitive, results, skipped)

        if step["has_when"]:
            try:
                cond_val = eval_condition(step["when"], rc)
            except WFError as exc:
                raise WFError(
                    exc.code,
                    "step %r: %s" % (chosen, exc.message),
                    step=chosen,
                )
            if not isinstance(cond_val, bool):
                raise WFError(
                    "INVALID_CONDITION",
                    "when did not evaluate to a boolean",
                    step=chosen,
                )
            if not cond_val:
                results[chosen] = None
                execution_order.append(chosen)
                skipped.append(chosen)
                finished.add(chosen)
                continue

        attempts = 0
        ctx.current_step = chosen
        while True:
            attempts += 1
            try:
                args = resolve_value(step["args"], rc)
                value = execute_op(step["op"], args, ctx)
            except WFError as exc:
                if exc.code == "TRANSIENT_FAILURE" and attempts < step["max_attempts"]:
                    continue
                if exc.code == "TRANSIENT_FAILURE":
                    raise WFError(
                        "STEP_FAILED",
                        "step %r failed after %d attempt(s)" % (chosen, attempts),
                        step=chosen,
                    )
                raise WFError(
                    exc.code,
                    "step %r: %s" % (chosen, exc.message),
                    step=chosen if exc.step is None else exc.step,
                )
            results[chosen] = value
            execution_order.append(chosen)
            finished.add(chosen)
            break


def error_response(code, message, step=None, results=None, execution_order=None,
                   skipped=None):
    return {
        "status": "failed",
        "results": results if results is not None else {},
        "execution_order": execution_order if execution_order is not None else [],
        "skipped": skipped if skipped is not None else [],
        "error": {"code": code, "step": step, "message": message},
    }


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def _reject_constant(text):
    raise ValueError("invalid JSON constant %s" % text)


def _load_doc(text):
    return json.loads(
        text,
        parse_float=_parse_float,
        parse_constant=_reject_constant,
    )


def emit(resp, code):
    sys.stdout.write(
        json.dumps(resp, ensure_ascii=True, separators=(",", ":"), allow_nan=False)
    )
    sys.stdout.write("\n")
    sys.stdout.flush()
    return code


def main(argv):
    if len(argv) != 3 or argv[1] != "run":
        return emit(
            error_response(
                "INVALID_WORKFLOW",
                "usage: workflow run <workflow.json> | workflow run -",
            ),
            2,
        )
    path = argv[2]
    try:
        if path == "-":
            text = sys.stdin.buffer.read().decode("utf-8")
        else:
            with open(path, "r", encoding="utf-8") as fh:
                text = fh.read()
        if text.startswith("\ufeff"):
            text = text[1:]
    except OSError as exc:
        return emit(
            error_response("INVALID_WORKFLOW", "cannot read input: %s" % exc), 2
        )
    except UnicodeDecodeError as exc:
        return emit(
            error_response("INVALID_WORKFLOW", "input is not valid UTF-8: %s" % exc), 2
        )

    try:
        doc = _load_doc(text)
    except (ValueError, RecursionError) as exc:
        return emit(
            error_response("INVALID_WORKFLOW", "invalid JSON: %s" % exc), 2
        )

    try:
        resp, code = run_workflow(doc)
        return emit(resp, code)
    except WFError as exc:
        exit_code = EXIT_FOR_CODE.get(exc.code, DEFAULT_EXIT)
        return emit(
            error_response(exc.code, exc.message, step=exc.step), exit_code
        )
    except Exception as exc:  # pragma: no cover - internal safety net
        try:
            return emit(
                error_response("INTERNAL_ERROR", "internal error: %s" % exc), 3
            )
        except Exception:
            sys.stdout.write(
                '{"status":"failed","results":{},"execution_order":[],'
                '"skipped":[],"error":{"code":"INTERNAL_ERROR","step":null,'
                '"message":"internal error"}}\n'
            )
            return 3


if __name__ == "__main__":
    sys.exit(main(sys.argv))
