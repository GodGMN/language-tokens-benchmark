#!/usr/bin/env python3
"""Reference oracle for the workflow engine (independent of the 5 implementations).

Used to validate fixtures and the conformance harness. NOT one of the benchmarked
implementations. Implements SPEC.md exactly.

Usage: python3 workflow.py run <workflow.json>   |   python3 workflow.py run -
"""
import json
import sys
from collections import deque

I64_MIN = -(2**63)
I64_MAX = 2**63 - 1


class WFError(Exception):
    def __init__(self, code, step=None, message=""):
        super().__init__(code)
        self.code = code
        self.step = step
        self.message = message


# ---------- i64 ----------

def as_i64(v, what="value"):
    if isinstance(v, bool) or not isinstance(v, (int, float)):
        raise WFError("INVALID_ARG", message=f"expected integer, got {type(v).__name__} ({what})")
    if isinstance(v, float):
        if v != int(v) or not (I64_MIN <= v <= I64_MAX):
            raise WFError("INVALID_ARG", message=f"expected integer, got float {v}")
        return int(v)
    if not (I64_MIN <= v <= I64_MAX):
        raise WFError("INVALID_ARG", message=f"integer out of i64 range: {v}")
    return v


def check_i64(v, what="result"):
    if not (I64_MIN <= v <= I64_MAX):
        raise WFError("ARITHMETIC_OVERFLOW", message=f"{what} out of range: {v}")
    return v


def iadd(a, b):
    return check_i64(a + b, "add")


def isub(a, b):
    return check_i64(a - b, "subtract")


def imul(a, b):
    return check_i64(a * b, "multiply")


def idiv(a, b):
    if b == 0:
        raise WFError("DIVISION_BY_ZERO", message="divide by zero")
    q = abs(a) // abs(b)
    return check_i64(q if (a >= 0) == (b >= 0) else -q, "divide")


def imod(a, b):
    if b == 0:
        raise WFError("DIVISION_BY_ZERO", message="mod by zero")
    r = abs(a) % abs(b)
    return check_i64(r if a >= 0 else -r, "mod")


# ---------- structural equality / ordering ----------

def struct_eq(a, b):
    if isinstance(a, bool) != isinstance(b, bool):
        if (a is None) != (b is None):
            return False
        if isinstance(a, (int, float)) and isinstance(b, (int, float)):
            return a == b
        if isinstance(a, str) == isinstance(b, str):
            return a == b
        if isinstance(a, list) and isinstance(b, list):
            return len(a) == len(b) and all(struct_eq(x, y) for x, y in zip(a, b))
        if isinstance(a, dict) and isinstance(b, dict):
            return a.keys() == b.keys() and all(struct_eq(a[k], b[k]) for k in a)
        if type(a) is type(b):
            return a == b
        return False
    if (a is None) != (b is None):
        return False
    if isinstance(a, bool) and isinstance(b, bool):
        return a == b
    if isinstance(a, (int, float)) and isinstance(b, (int, float)) and not isinstance(a, bool) and not isinstance(b, bool):
        return a == b
    if isinstance(a, str) and isinstance(b, str):
        return a == b
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(struct_eq(x, y) for x, y in zip(a, b))
    if isinstance(a, dict) and isinstance(b, dict):
        return a.keys() == b.keys() and all(struct_eq(a[k], b[k]) for k in a)
    return a is b or a == b


TYPE_RANK = {"number": 0, "string": 1, "boolean": 2, "null": 3, "array": 4, "object": 5}


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
    return "object"


def cmp_values(a, b):
    """Return -1/0/1. Same type: natural order. Different type: TYPE_RANK."""
    ta, tb = jtype(a), jtype(b)
    if ta != tb:
        return (ta > tb) - (ta < tb)
    if ta == "number":
        return (a > b) - (a < b)
    if ta == "string":
        sa, sb = a.encode("utf-8"), b.encode("utf-8")
        return (sa > sb) - (sa < sb)
    if ta == "boolean":
        return (a > b) - (a < b)
    if ta == "null":
        return 0
    if ta == "array":
        if len(a) != len(b):
            return (len(a) > len(b)) - (len(a) < len(b))
        for x, y in zip(a, b):
            c = cmp_values(x, y)
            if c:
                return c
        return 0
    # object: compare by sorted key representation
    ka, kb = sorted(a.keys()), sorted(b.keys())
    if ka != kb:
        return (ka > kb) - (ka < kb)
    for k in ka:
        c = cmp_values(a[k], b[k])
        if c:
            return c
    return 0


def truthy(v):
    if v is None or v is False:
        return False
    if v is True:
        return True
    if isinstance(v, (int, float)) and not isinstance(v, bool):
        return v != 0
    if isinstance(v, str):
        return v != ""
    if isinstance(v, list):
        return len(v) > 0
    if isinstance(v, dict):
        return len(v) > 0
    return True


# ---------- references ----------

def is_ref(v):
    return isinstance(v, dict) and list(v.keys()) == ["$ref"] and isinstance(v["$ref"], str)


def resolve_value(v, results, deps_of_step, step_id):
    """Recursively resolve $ref inside v. deps_of_step = set of allowed step ids."""
    if is_ref(v):
        path = v["$ref"].split(".")
        target = path[0]
        if target not in deps_of_step or target == step_id:
            raise WFError("INVALID_REFERENCE", step=step_id, message=f"ref {target} not a dependency")
        if target not in results:
            raise WFError("INVALID_REFERENCE", step=step_id, message=f"ref {target} not finished")
        cur = results[target]
        for seg in path[1:]:
            if isinstance(cur, dict) and seg in cur:
                cur = cur[seg]
            elif isinstance(cur, list):
                if not seg.lstrip("-").isdigit() or not (-(len(cur)) <= int(seg) < len(cur)):
                    raise WFError("INVALID_REFERENCE", step=step_id, message=f"bad index {seg} in {target}")
                cur = cur[int(seg)]
            else:
                raise WFError("INVALID_REFERENCE", step=step_id, message=f"no path {seg} in {target}")
        if target in skipped_set and cur is None:
            # referenced a skipped step whose result is null
            raise WFError("SKIPPED_REFERENCE", step=step_id, message=f"{target} was skipped")
        return cur
    if isinstance(v, dict):
        return {k: resolve_value(x, results, deps_of_step, step_id) for k, x in v.items()}
    if isinstance(v, list):
        return [resolve_value(x, results, deps_of_step, step_id) for x in v]
    return v


skipped_set = set()  # module-level for simplicity of the oracle


# ---------- operations ----------

def op_add(args, step):
    a, b = as_i64(args.get("a")), as_i64(args.get("b"))
    return iadd(a, b)


def op_subtract(args, step):
    return isub(as_i64(args.get("a")), as_i64(args.get("b")))


def op_multiply(args, step):
    return imul(as_i64(args.get("a")), as_i64(args.get("b")))


def op_divide(args, step):
    return idiv(as_i64(args.get("a")), as_i64(args.get("b")))


def op_mod(args, step):
    return imod(as_i64(args.get("a")), as_i64(args.get("b")))


def op_sum(args, step):
    if not isinstance(args.get("input"), list):
        raise WFError("INVALID_ARG", step=step, message="sum: input not array")
    s = 0
    for x in args["input"]:
        s = iadd(s, as_i64(x))
    return s


def op_min(args, step):
    arr = args.get("input")
    if not isinstance(arr, list):
        raise WFError("INVALID_ARG", step=step, message="min: input not array")
    if not arr:
        raise WFError("EMPTY_ARRAY", step=step, message="min of empty array")
    return min(as_i64(x) for x in arr)


def op_max(args, step):
    arr = args.get("input")
    if not isinstance(arr, list):
        raise WFError("INVALID_ARG", step=step, message="max: input not array")
    if not arr:
        raise WFError("EMPTY_ARRAY", step=step, message="max of empty array")
    return max(as_i64(x) for x in arr)


def op_range(args, step):
    start, end = as_i64(args.get("start")), as_i64(args.get("end"))
    return list(range(start, end + 1)) if start <= end else []


def op_concat(args, step):
    a, b = args.get("a"), args.get("b")
    if not isinstance(a, str) or not isinstance(b, str):
        raise WFError("INVALID_ARG", step=step, message="concat: args not strings")
    return a + b


def op_split(args, step):
    inp, sep = args.get("input"), args.get("sep")
    if not isinstance(inp, str) or not isinstance(sep, str):
        raise WFError("INVALID_ARG", step=step, message="split: args not strings")
    if sep == "":
        return list(inp)
    return inp.split(sep)


def op_join(args, step):
    arr, sep = args.get("input"), args.get("sep")
    if not isinstance(arr, list) or not isinstance(sep, str):
        raise WFError("INVALID_ARG", step=step, message="join: bad args")
    return sep.join(arr)


def ascii_case(s, upper):
    out = []
    for ch in s:
        o = ord(ch)
        if 65 <= o <= 90:
            out.append(ch if upper else chr(o + 32))
        elif 97 <= o <= 122:
            out.append(chr(o - 32) if upper else ch)
        else:
            out.append(ch)
    return "".join(out)


def op_uppercase(args, step):
    return ascii_case(args.get("input", ""), True)


def op_lowercase(args, step):
    return ascii_case(args.get("input", ""), False)


WS = " \t\n\r\f\v"


def op_trim(args, step):
    return args.get("input", "").strip(WS)


def op_replace(args, step):
    return args.get("input", "").replace(args.get("search", ""), args.get("replace", ""))


def op_length(args, step):
    return check_i64(len(args.get("input", "")), "length")


def apply_transform(t, el, step):
    kind = t.get("type")
    if kind == "constant":
        return t.get("value")
    if kind in ("add", "subtract", "multiply", "divide", "mod"):
        v = as_i64(t.get("value"))
        fns = {"add": iadd, "subtract": isub, "multiply": imul, "divide": idiv, "mod": imod}
        return fns[kind](as_i64(el), v)
    if kind == "negate":
        return isub(0, as_i64(el))
    if kind == "abs":
        return abs(as_i64(el))
    if kind == "stringify":
        return json.dumps(el, separators=(",", ":"), ensure_ascii=False, sort_keys=True) if isinstance(el, dict) else json.dumps(el, separators=(",", ":"), ensure_ascii=False)
    if kind == "type":
        return jtype(el)
    raise WFError("INVALID_ARG", step=step, message=f"unknown transform {kind}")


def op_map(args, step):
    arr, t = args.get("input"), args.get("transform")
    if not isinstance(arr, list) or not isinstance(t, dict):
        raise WFError("INVALID_ARG", step=step, message="map: bad args")
    return [apply_transform(t, x, step) for x in arr]


def apply_predicate(p, el, step):
    kind = p.get("type")
    if kind in ("even", "odd"):
        r = imod(as_i64(el), 2)
        return (r == 0) if kind == "even" else (r == 1)
    if kind in ("gt", "gte", "lt", "lte", "eq", "ne"):
        v = as_i64(p.get("value"))
        c = cmp_values(as_i64(el), v)
        return {"gt": c > 0, "gte": c >= 0, "lt": c < 0, "lte": c <= 0, "eq": c == 0, "ne": c != 0}[kind]
    if kind == "truthy":
        return truthy(el)
    if kind == "falsy":
        return not truthy(el)
    if kind == "type_is":
        return jtype(el) == p.get("type")
    if kind == "string_contains":
        return isinstance(el, str) and p.get("value", "") in el
    raise WFError("INVALID_ARG", step=step, message=f"unknown predicate {kind}")


def op_filter(args, step):
    arr, p = args.get("input"), args.get("predicate")
    if not isinstance(arr, list) or not isinstance(p, dict):
        raise WFError("INVALID_ARG", step=step, message="filter: bad args")
    return [x for x in arr if apply_predicate(p, x, step)]


def op_reduce(args, step):
    arr, rop, initial = args.get("input"), args.get("op"), args.get("initial")
    if not isinstance(arr, list) or rop not in ("add", "subtract", "multiply"):
        raise WFError("INVALID_ARG", step=step, message="reduce: bad args")
    fns = {"add": iadd, "subtract": isub, "multiply": imul}
    # Identity defaults: 0 for add/subtract, 1 for multiply.
    if initial is not None:
        acc = as_i64(initial)
    else:
        acc = 1 if rop == "multiply" else 0
    for x in arr:
        acc = fns[rop](acc, as_i64(x))
    return acc


def op_sort(args, step):
    arr = args.get("input")
    if not isinstance(arr, list):
        raise WFError("INVALID_ARG", step=step, message="sort: not array")
    return stable_sort(arr)


def stable_sort(arr):
    # insertion sort for stability with composite key
    def key_of(v):
        return (TYPE_RANK[jtype(v)], v)

    out = list(arr)
    for i in range(1, len(out)):
        cur = out[i]
        j = i - 1
        while j >= 0 and cmp_composite(key_of(out[j]), key_of(cur)) > 0:
            out[j + 1] = out[j]
            j -= 1
        out[j + 1] = cur
    return out


def cmp_composite(ka, kb):
    if ka[0] != kb[0]:
        return ka[0] - kb[0]
    return cmp_values(ka[1], kb[1])


def op_reverse(args, step):
    arr = args.get("input")
    if not isinstance(arr, list):
        raise WFError("INVALID_ARG", step=step, message="reverse: not array")
    return list(reversed(arr))


def op_unique(args, step):
    arr = args.get("input")
    if not isinstance(arr, list):
        raise WFError("INVALID_ARG", step=step, message="unique: not array")
    out, seen = [], []
    for x in arr:
        if not any(struct_eq(x, s) for s in seen):
            seen.append(x)
            out.append(x)
    return out


def op_slice(args, step):
    arr = args.get("input")
    if not isinstance(arr, list):
        raise WFError("INVALID_ARG", step=step, message="slice: not array")
    start = as_i64(args.get("start"))
    end = args.get("end")
    end = as_i64(end) if end is not None else len(arr)
    n = len(arr)
    s = max(0, min(n, start + n if start < 0 else start))
    e = max(0, min(n, end + n if end < 0 else end))
    return arr[s:e]


def op_flatten(args, step):
    arr = args.get("input")
    if not isinstance(arr, list):
        raise WFError("INVALID_ARG", step=step, message="flatten: not array")
    out = []
    for x in arr:
        if isinstance(x, list):
            out.extend(x)
        else:
            out.append(x)
    return out


def op_get(args, step):
    obj, key = args.get("input"), args.get("key")
    if not isinstance(obj, dict) or not isinstance(key, str):
        raise WFError("INVALID_ARG", step=step, message="get: bad args")
    return obj.get(key)


def op_set(args, step):
    obj, key, val = args.get("input"), args.get("key"), args.get("value", None)
    if not isinstance(obj, dict) or not isinstance(key, str):
        raise WFError("INVALID_ARG", step=step, message="set: bad args")
    out = dict(obj)
    out[key] = val
    return out


def op_keys(args, step):
    obj = args.get("input")
    if not isinstance(obj, dict):
        raise WFError("INVALID_ARG", step=step, message="keys: not object")
    return sorted(obj.keys(), key=lambda k: k.encode("utf-8"))


def op_values(args, step):
    obj = args.get("input")
    if not isinstance(obj, dict):
        raise WFError("INVALID_ARG", step=step, message="values: not object")
    return [obj[k] for k in sorted(obj.keys(), key=lambda k: k.encode("utf-8"))]


def op_merge(args, step):
    a, b = args.get("a"), args.get("b")
    if not isinstance(a, dict) or not isinstance(b, dict):
        raise WFError("INVALID_ARG", step=step, message="merge: not objects")
    out = dict(a)
    out.update(b)
    return out


def op_pick(args, step):
    obj, keys = args.get("input"), args.get("keys")
    if not isinstance(obj, dict) or not isinstance(keys, list):
        raise WFError("INVALID_ARG", step=step, message="pick: bad args")
    return {k: obj.get(k) for k in keys}


def op_omit(args, step):
    obj, keys = args.get("input"), args.get("keys")
    if not isinstance(obj, dict) or not isinstance(keys, list):
        raise WFError("INVALID_ARG", step=step, message="omit: bad args")
    return {k: v for k, v in obj.items() if k not in keys}


def op_constant(args, step):
    return args.get("value")


def op_equals(args, step):
    return struct_eq(args.get("a"), args.get("b"))


def op_compare(args, step):
    a, b, cop = args.get("a"), args.get("b"), args.get("op")
    if cop not in ("gt", "gte", "lt", "lte"):
        raise WFError("INVALID_ARG", step=step, message="compare: bad op")
    c = cmp_values(a, b)
    return {"gt": c > 0, "gte": c >= 0, "lt": c < 0, "lte": c <= 0}[cop]


def op_and(args, step):
    return truthy(args.get("a")) and truthy(args.get("b"))


def op_or(args, step):
    return truthy(args.get("a")) or truthy(args.get("b"))


def op_not(args, step):
    return not truthy(args.get("a"))


def op_type(args, step):
    return jtype(args.get("value"))


class AttemptCounter:
    def __init__(self):
        self.counts = {}

    def bump(self, step):
        self.counts[step] = self.counts.get(step, 0) + 1
        return self.counts[step]


def op_fail_until(args, step, counter):
    failures = as_i64(args.get("failures"))
    attempt = counter.bump(step)
    if attempt <= failures:
        raise WFError("TRANSIENT_FAILURE", step=step, message=f"attempt {attempt} of fail_until")
    return args.get("value")


OPS = {
    "add": lambda a, s, c: op_add(a, s),
    "subtract": lambda a, s, c: op_subtract(a, s),
    "multiply": lambda a, s, c: op_multiply(a, s),
    "divide": lambda a, s, c: op_divide(a, s),
    "mod": lambda a, s, c: op_mod(a, s),
    "sum": lambda a, s, c: op_sum(a, s),
    "min": lambda a, s, c: op_min(a, s),
    "max": lambda a, s, c: op_max(a, s),
    "range": lambda a, s, c: op_range(a, s),
    "concat": lambda a, s, c: op_concat(a, s),
    "split": lambda a, s, c: op_split(a, s),
    "join": lambda a, s, c: op_join(a, s),
    "uppercase": lambda a, s, c: op_uppercase(a, s),
    "lowercase": lambda a, s, c: op_lowercase(a, s),
    "trim": lambda a, s, c: op_trim(a, s),
    "replace": lambda a, s, c: op_replace(a, s),
    "length": lambda a, s, c: op_length(a, s),
    "map": lambda a, s, c: op_map(a, s),
    "filter": lambda a, s, c: op_filter(a, s),
    "reduce": lambda a, s, c: op_reduce(a, s),
    "sort": lambda a, s, c: op_sort(a, s),
    "reverse": lambda a, s, c: op_reverse(a, s),
    "unique": lambda a, s, c: op_unique(a, s),
    "slice": lambda a, s, c: op_slice(a, s),
    "flatten": lambda a, s, c: op_flatten(a, s),
    "get": lambda a, s, c: op_get(a, s),
    "set": lambda a, s, c: op_set(a, s),
    "keys": lambda a, s, c: op_keys(a, s),
    "values": lambda a, s, c: op_values(a, s),
    "merge": lambda a, s, c: op_merge(a, s),
    "pick": lambda a, s, c: op_pick(a, s),
    "omit": lambda a, s, c: op_omit(a, s),
    "constant": lambda a, s, c: op_constant(a, s),
    "equals": lambda a, s, c: op_equals(a, s),
    "compare": lambda a, s, c: op_compare(a, s),
    "and": lambda a, s, c: op_and(a, s),
    "or": lambda a, s, c: op_or(a, s),
    "not": lambda a, s, c: op_not(a, s),
    "type": lambda a, s, c: op_type(a, s),
    "fail_until": lambda a, s, c: op_fail_until(a, s, c),
}

TRANSIENT = {"TRANSIENT_FAILURE"}
# Structural validation failures exit 2 (SPEC §7); every runtime
# failure, including INVALID_ARG / INVALID_CONDITION, exits 1.
VALIDATION = {"INVALID_WORKFLOW", "UNKNOWN_DEPENDENCY", "CYCLIC_DEPENDENCY"}


# ---------- engine ----------

def structural_validate(doc):
    if not isinstance(doc, dict) or set(doc.keys()) - {"version", "steps"}:
        raise WFError("INVALID_WORKFLOW", message="unknown top-level fields")
    if doc.get("version") != 1:
        raise WFError("INVALID_WORKFLOW", message="version must be 1")
    steps = doc.get("steps")
    if not isinstance(steps, list):
        raise WFError("INVALID_WORKFLOW", message="steps must be array")
    seen = set()
    for i, st in enumerate(steps):
        if not isinstance(st, dict) or set(st.keys()) - {"id", "op", "args", "depends_on", "when", "retry"}:
            hint = st.get("id") if isinstance(st, dict) else None
            where = hint if isinstance(hint, str) else f"steps[{i}]"
            raise WFError("INVALID_WORKFLOW", step=where, message="unknown step fields")
        sid = st.get("id")
        if not isinstance(sid, str) or not _id_ok(sid):
            raise WFError("INVALID_WORKFLOW", step=f"steps[{i}]", message=f"bad id {sid!r}")
        if sid in seen:
            raise WFError("INVALID_WORKFLOW", step=sid, message="duplicate id")
        seen.add(sid)
        if st.get("op") not in OPS:
            raise WFError("INVALID_WORKFLOW", step=sid, message=f"unknown op {st.get('op')}")
        if st.get("args") is not None and not isinstance(st.get("args"), dict):
            raise WFError("INVALID_WORKFLOW", step=sid, message="args must be object")
        deps = st.get("depends_on", [])
        if not isinstance(deps, list) or not all(isinstance(d, str) for d in deps):
            raise WFError("INVALID_WORKFLOW", step=sid, message="depends_on must be array of ids")
        retry = st.get("retry")
        if retry is not None:
            if not isinstance(retry, dict) or set(retry.keys()) != {"max_attempts"}:
                raise WFError("INVALID_WORKFLOW", step=sid, message="bad retry")
            ma = retry["max_attempts"]
            if not isinstance(ma, int) or isinstance(ma, bool) or not (1 <= ma <= 5):
                raise WFError("INVALID_WORKFLOW", step=sid, message="max_attempts out of range")
        if st.get("when") is not None:
            _validate_condition(st["when"], sid)
    for st in steps:
        for d in st.get("depends_on", []):
            if d not in seen:
                raise WFError("UNKNOWN_DEPENDENCY", step=st["id"], message=f"missing dependency {d}")
        if st["id"] in st.get("depends_on", []):
            raise WFError("CYCLIC_DEPENDENCY", step=None, message="self dependency")
    # cycle detection
    idx = {st["id"]: i for i, st in enumerate(steps)}
    deps = {st["id"]: st.get("depends_on", []) for st in steps}
    state = {}

    def visit(n, stack):
        if state.get(n) == 1:
            raise WFError("CYCLIC_DEPENDENCY", step=None, message="cycle")
        if state.get(n) == 2:
            return
        state[n] = 1
        for d in deps[n]:
            visit(d, stack + [n])
        state[n] = 2

    for n in deps:
        visit(n, [])
    return steps


def _id_ok(s):
    import re
    return re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", s) is not None


def _validate_condition(c, sid):
    if not isinstance(c, dict) or "op" not in c:
        raise WFError("INVALID_WORKFLOW", step=sid, message="bad condition")
    op = c["op"]
    if op in ("eq", "ne", "gt", "gte", "lt", "lte"):
        if "left" not in c or "right" not in c:
            raise WFError("INVALID_WORKFLOW", step=sid, message="comparison needs left/right")
    elif op in ("all", "any"):
        conds = c.get("conditions")
        if not isinstance(conds, list):
            raise WFError("INVALID_WORKFLOW", step=sid, message="all/any needs conditions")
        for x in conds:
            _validate_condition(x, sid)
    elif op == "not":
        _validate_condition(c.get("condition"), sid)
    else:
        raise WFError("INVALID_WORKFLOW", step=sid, message=f"unknown condition op {op}")


def eval_condition(c, results, deps, step_id):
    op = c["op"]
    if op in ("eq", "ne", "gt", "gte", "lt", "lte"):
        l = resolve_value(c["left"], results, deps, step_id)
        r = resolve_value(c["right"], results, deps, step_id)
        if op == "eq":
            return struct_eq(l, r)
        if op == "ne":
            return not struct_eq(l, r)
        c2 = cmp_values(l, r)
        return {"gt": c2 > 0, "gte": c2 >= 0, "lt": c2 < 0, "lte": c2 <= 0}[op]
    if op == "all":
        return all(eval_condition(x, results, deps, step_id) for x in c["conditions"])
    if op == "any":
        return any(eval_condition(x, results, deps, step_id) for x in c["conditions"])
    if op == "not":
        return not eval_condition(c["condition"], results, deps, step_id)
    raise WFError("INVALID_CONDITION", step=step_id, message=f"unknown condition op {op}")


def transitive_deps(steps):
    direct = {st["id"]: set(st.get("depends_on", [])) for st in steps}
    memo = {}

    def deps_of(n, seen=frozenset()):
        if n in memo:
            return memo[n]
        out = set(direct[n])
        for d in direct[n]:
            if d not in seen:
                out |= deps_of(d, seen | {n})
        memo[n] = out
        return out

    for n in direct:
        deps_of(n)
    return memo


def _completed(results, order, skipped):
    return {"status": "completed", "results": results, "execution_order": order,
            "skipped": skipped, "error": None}


def _failed(results, order, skipped, err):
    return {"status": "failed", "results": results, "execution_order": order,
            "skipped": skipped,
            "error": {"code": err.code, "step": err.step, "message": err.message}}


def _exit_code(code):
    return 2 if code in VALIDATION else 1


def _ready(pending, results):
    """Ready steps in document order (deterministic tie-break)."""
    return [st for st in pending if all(d in results for d in st.get("depends_on", []))]


def _evaluate_when(step, results, tdeps):
    """Return True to run, False to skip. Raises INVALID_CONDITION otherwise."""
    when = step.get("when")
    if when is None:
        return True
    cond = eval_condition(when, results, tdeps[step["id"]], step["id"])
    if not isinstance(cond, bool):
        raise WFError("INVALID_CONDITION", step=step["id"], message="condition is not a boolean")
    return cond


def _run_step(step, results, tdeps, counter):
    """Resolve args and run the op, retrying only transient failures."""
    sid = step["id"]
    max_attempts = step.get("retry", {}).get("max_attempts", 1)
    for attempt in range(max_attempts):
        try:
            args = resolve_value(step.get("args", {}), results, tdeps[sid], sid)
            return OPS[step["op"]](args, sid, counter)
        except WFError as e:
            if e.code in TRANSIENT and attempt < max_attempts - 1:
                continue
            raise


def execute(doc):
    """Execute a workflow document. Returns (response, exit_code)."""
    global skipped_set
    skipped_set = set()

    try:
        steps = structural_validate(doc)
    except WFError as e:
        return _failed({}, [], [], e), _exit_code(e.code)

    counter = AttemptCounter()
    tdeps = transitive_deps(steps)
    results, order, skipped = {}, [], []
    pending = list(steps)

    while pending:
        ready = _ready(pending, results)
        if not ready:
            err = WFError("CYCLIC_DEPENDENCY", message="no runnable step (cycle)")
            return _failed(results, order, skipped, err), _exit_code(err.code)

        step = ready[0]
        sid = step["id"]
        pending.remove(step)
        try:
            if not _evaluate_when(step, results, tdeps):
                results[sid] = None
                order.append(sid)
                skipped.append(sid)
                skipped_set.add(sid)
                continue
            results[sid] = _run_step(step, results, tdeps, counter)
            order.append(sid)
        except WFError as e:
            # TRANSIENT_FAILURE is the per-attempt code; once attempts are
            # exhausted the response reports STEP_FAILED (SPEC §7).
            # Runtime errors (bad args, div-by-zero, ...) are raised inside the
            # op without a step, so stamp the step that is currently running.
            code = "STEP_FAILED" if e.code in TRANSIENT else e.code
            err = WFError(code, e.step or sid, e.message)
            return _failed(results, order, skipped, err), _exit_code(code)

    return _completed(results, order, skipped), 0


def _emit(response, code):
    sys.stdout.write(json.dumps(response) + "\n")
    sys.exit(code)


def main():
    argv = sys.argv[1:]
    if len(argv) != 2 or argv[0] != "run":
        sys.stderr.write("usage: workflow run <file|->\n")
        sys.exit(2)

    try:
        raw = sys.stdin.read() if argv[1] == "-" else open(argv[1]).read()
    except OSError as e:
        _emit(_failed({}, [], [], WFError("INVALID_WORKFLOW", None, f"cannot read input: {e}")), 2)

    try:
        doc = json.loads(raw)
    except json.JSONDecodeError as e:
        _emit(_failed({}, [], [], WFError("INVALID_WORKFLOW", None, f"bad JSON: {e}")), 2)

    try:
        response, code = execute(doc)
    except Exception as e:  # internal error: best-effort JSON, exit 3
        _emit(_failed({}, [], [], WFError("INTERNAL", None, f"{type(e).__name__}: {e}")), 3)

    _emit(response, code)


if __name__ == "__main__":
    main()
