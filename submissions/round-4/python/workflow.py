#!/usr/bin/env python3
"""Declarative workflow engine implementing SPEC.md.

Standard-library only.  The program is intentionally self contained: a single
``workflow.py`` entry point is all that is needed to run the CLI.
"""

import json
import math
import re
import sys
from functools import cmp_to_key

INT64_MIN = -(1 << 63)
INT64_MAX = (1 << 63) - 1

STEP_FIELDS = {"id", "op", "args", "depends_on", "when", "retry"}

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
    # test operation
    "fail_until",
}

TYPE_NAMES = {"null", "boolean", "number", "string", "array", "object"}

COND_BINARY = {"eq", "ne", "gt", "gte", "lt", "lte"}

ID_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
ARRAY_INDEX_RE = re.compile(r"^[0-9]+$")

ASCII_WS = " \t\n\r\f\v"

_UPPER_TABLE = str.maketrans(
    "abcdefghijklmnopqrstuvwxyz", "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
)
_LOWER_TABLE = str.maketrans(
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ", "abcdefghijklmnopqrstuvwxyz"
)


class WFError(Exception):
    """An error that maps directly onto the workflow error response."""

    def __init__(self, code, message, step=None):
        super().__init__(message)
        self.code = code
        self.message = message
        self.step = step


def invalid_arg(message="invalid argument"):
    raise WFError("INVALID_ARG", message)


def invalid_reference(message="invalid reference"):
    raise WFError("INVALID_REFERENCE", message)


# ---------------------------------------------------------------------------
# JSON / type helpers
# ---------------------------------------------------------------------------

def is_int_literal(value):
    """True for JSON integers (but not booleans)."""
    return isinstance(value, int) and not isinstance(value, bool)


def require_i64(value, name="value"):
    if not is_int_literal(value):
        invalid_arg("%s must be an integer literal" % name)
    if value < INT64_MIN or value > INT64_MAX:
        invalid_arg("%s does not fit in i64" % name)
    return value


def require_string(value, name="value"):
    if not isinstance(value, str):
        invalid_arg("%s must be a string" % name)
    return value


def check_i64(result):
    if result < INT64_MIN or result > INT64_MAX:
        raise WFError("ARITHMETIC_OVERFLOW", "i64 overflow")
    return result


def type_name(value):
    if value is None:
        return "null"
    if isinstance(value, bool):
        return "boolean"
    if isinstance(value, (int, float)):
        return "number"
    if isinstance(value, str):
        return "string"
    if isinstance(value, list):
        return "array"
    if isinstance(value, dict):
        return "object"
    # Should be unreachable for JSON values.
    return "unknown"


def truthy(value):
    if value is None:
        return False
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return value != 0
    if isinstance(value, str):
        return len(value) > 0
    if isinstance(value, (list, dict)):
        return len(value) > 0
    return False


def utf8_bytes(value):
    return value.encode("utf-8", "surrogatepass")


def json_compact(value):
    return json.dumps(
        value, ensure_ascii=False, separators=(",", ":"), allow_nan=False
    )


# ---------------------------------------------------------------------------
# Value ordering / equality (SPEC section 3)
# ---------------------------------------------------------------------------

_TYPE_RANK = {
    "number": 0,
    "string": 1,
    "boolean": 2,
    "null": 3,
    "array": 4,
    "object": 5,
}


def total_compare(a, b):
    ta = type_name(a)
    tb = type_name(b)
    ra = _TYPE_RANK[ta]
    rb = _TYPE_RANK[tb]
    if ra != rb:
        return -1 if ra < rb else 1

    if ta == "number":
        if a < b:
            return -1
        if a > b:
            return 1
        return 0
    if ta == "string":
        ba = utf8_bytes(a)
        bb = utf8_bytes(b)
        if ba < bb:
            return -1
        if ba > bb:
            return 1
        return 0
    if ta == "boolean":
        if a == b:
            return 0
        return -1 if (not a) else 1
    if ta == "null":
        return 0
    if ta == "array":
        n = min(len(a), len(b))
        for i in range(n):
            c = total_compare(a[i], b[i])
            if c != 0:
                return c
        if len(a) < len(b):
            return -1
        if len(a) > len(b):
            return 1
        return 0
    # objects
    ka = sorted(a.keys(), key=utf8_bytes)
    kb = sorted(b.keys(), key=utf8_bytes)
    n = min(len(ka), len(kb))
    for i in range(n):
        c = total_compare(ka[i], kb[i])
        if c != 0:
            return c
    if len(ka) < len(kb):
        return -1
    if len(ka) > len(kb):
        return 1
    for k in ka:
        c = total_compare(a[k], b[k])
        if c != 0:
            return c
    return 0


def structural_equals(a, b):
    return total_compare(a, b) == 0


# ---------------------------------------------------------------------------
# Integer arithmetic
# ---------------------------------------------------------------------------

def int_add(a, b):
    return check_i64(a + b)


def int_subtract(a, b):
    return check_i64(a - b)


def int_multiply(a, b):
    return check_i64(a * b)


def int_divide(a, b):
    if b == 0:
        raise WFError("DIVISION_BY_ZERO", "division by zero")
    if a == INT64_MIN and b == -1:
        raise WFError("ARITHMETIC_OVERFLOW", "INT64_MIN / -1 overflow")
    q = abs(a) // abs(b)
    if (a < 0) != (b < 0):
        q = -q
    return q


def int_mod(a, b):
    if b == 0:
        raise WFError("DIVISION_BY_ZERO", "modulo by zero")
    if a == INT64_MIN and b == -1:
        raise WFError("ARITHMETIC_OVERFLOW", "INT64_MIN % -1 overflow")
    q = abs(a) // abs(b)
    if (a < 0) != (b < 0):
        q = -q
    return a - q * b


_BINARY_MATH = {
    "add": int_add,
    "subtract": int_subtract,
    "multiply": int_multiply,
    "divide": int_divide,
    "mod": int_mod,
}


# ---------------------------------------------------------------------------
# Structural validation
# ---------------------------------------------------------------------------

def _condition_error(message):
    raise WFError("INVALID_WORKFLOW", message)


def validate_condition(cond):
    if not isinstance(cond, dict):
        _condition_error("condition must be an object")
    op = cond.get("op")
    if not isinstance(op, str):
        _condition_error("condition.op must be a string")

    if op in COND_BINARY:
        allowed = {"op", "left", "right"}
        for key in cond:
            if key not in allowed:
                _condition_error("unknown condition field %r" % key)
        if "left" not in cond or "right" not in cond:
            _condition_error("condition requires left and right")
    elif op in ("all", "any"):
        allowed = {"op", "conditions"}
        for key in cond:
            if key not in allowed:
                _condition_error("unknown condition field %r" % key)
        if "conditions" not in cond or not isinstance(cond["conditions"], list):
            _condition_error("condition requires a conditions array")
        for sub in cond["conditions"]:
            validate_condition(sub)
    elif op == "not":
        allowed = {"op", "condition"}
        for key in cond:
            if key not in allowed:
                _condition_error("unknown condition field %r" % key)
        if "condition" not in cond:
            _condition_error("not condition requires a condition")
        validate_condition(cond["condition"])
    else:
        _condition_error("unknown condition operator %r" % op)


def _has_cycle(steps, deps):
    state = {}

    for start in [s["id"] for s in steps]:
        if state.get(start, 0) != 0:
            continue
        # Iterative DFS with an explicit stack to avoid recursion limits.
        stack = [(start, iter(deps[start]))]
        state[start] = 1
        while stack:
            node, it = stack[-1]
            try:
                nxt = next(it)
            except StopIteration:
                state[node] = 2
                stack.pop()
                continue
            st = state.get(nxt, 0)
            if st == 1:
                return True
            if st == 0:
                state[nxt] = 1
                stack.append((nxt, iter(deps[nxt])))
    return False


def validate_workflow(doc):
    if not isinstance(doc, dict):
        raise WFError("INVALID_WORKFLOW", "workflow must be a JSON object")

    for key in doc:
        if key not in ("version", "steps"):
            raise WFError("INVALID_WORKFLOW", "unknown top-level field %r" % key)

    if "version" not in doc:
        raise WFError("INVALID_WORKFLOW", "missing version")
    version = doc["version"]
    if not is_int_literal(version) or version != 1:
        raise WFError("INVALID_WORKFLOW", "version must be the integer 1")

    if "steps" not in doc:
        raise WFError("INVALID_WORKFLOW", "missing steps")
    steps = doc["steps"]
    if not isinstance(steps, list):
        raise WFError("INVALID_WORKFLOW", "steps must be an array")

    ids = set()
    deps = {}

    for step in steps:
        if not isinstance(step, dict):
            raise WFError("INVALID_WORKFLOW", "step must be an object")

        raw_id = step.get("id")
        sid_for_error = raw_id if isinstance(raw_id, str) else None

        for key in step:
            if key not in STEP_FIELDS:
                raise WFError(
                    "INVALID_WORKFLOW",
                    "unknown step field %r" % key,
                    sid_for_error,
                )

        if "id" not in step or not isinstance(raw_id, str):
            raise WFError("INVALID_WORKFLOW", "step id must be a string", None)
        sid = raw_id
        if not ID_RE.match(sid):
            raise WFError("INVALID_WORKFLOW", "invalid step id", sid)
        if sid in ids:
            raise WFError("INVALID_WORKFLOW", "duplicate step id", sid)
        ids.add(sid)

        op = step.get("op")
        if not isinstance(op, str):
            raise WFError("INVALID_WORKFLOW", "step op must be a string", sid)
        if op not in OPS:
            raise WFError("INVALID_WORKFLOW", "unknown operation %r" % op, sid)

        if "args" in step and not isinstance(step["args"], dict):
            raise WFError("INVALID_WORKFLOW", "args must be an object", sid)

        raw_deps = step.get("depends_on", [])
        if not isinstance(raw_deps, list):
            raise WFError(
                "INVALID_WORKFLOW", "depends_on must be an array", sid
            )
        for dep in raw_deps:
            if not isinstance(dep, str):
                raise WFError(
                    "INVALID_WORKFLOW",
                    "depends_on entries must be strings",
                    sid,
                )
        deps[sid] = list(raw_deps)

        if "retry" in step:
            retry = step["retry"]
            if not isinstance(retry, dict):
                raise WFError("INVALID_WORKFLOW", "retry must be an object", sid)
            for key in retry:
                if key != "max_attempts":
                    raise WFError(
                        "INVALID_WORKFLOW", "unknown retry field %r" % key, sid
                    )
            if "max_attempts" in retry:
                m = retry["max_attempts"]
                if not is_int_literal(m) or m < 1 or m > 5:
                    raise WFError(
                        "INVALID_WORKFLOW",
                        "retry.max_attempts must be an integer 1..5",
                        sid,
                    )

        if "when" in step:
            try:
                validate_condition(step["when"])
            except WFError as exc:
                raise WFError(exc.code, exc.message, sid)

    # All ids are known: verify dependencies.
    for step in steps:
        sid = step["id"]
        for dep in step.get("depends_on", []):
            if dep not in ids:
                raise WFError(
                    "UNKNOWN_DEPENDENCY",
                    "unknown dependency %r" % dep,
                    sid,
                )

    if _has_cycle(steps, deps):
        raise WFError(
            "CYCLIC_DEPENDENCY", "workflow contains a dependency cycle", None
        )

    return steps


# ---------------------------------------------------------------------------
# Argument checking
# ---------------------------------------------------------------------------

def check_args(args, allowed, required):
    if not isinstance(args, dict):
        invalid_arg("args must be an object")
    for key in args:
        if key not in allowed:
            invalid_arg("unknown argument %r" % key)
    for key in required:
        if key not in args:
            invalid_arg("missing argument %r" % key)


def require_array(value, name="input"):
    if not isinstance(value, list):
        invalid_arg("%s must be an array" % name)
    return value


def require_object(value, name="input"):
    if not isinstance(value, dict):
        invalid_arg("%s must be an object" % name)
    return value


# ---------------------------------------------------------------------------
# map transforms / filter predicates
# ---------------------------------------------------------------------------

def _check_declared_keys(obj, allowed, what):
    for key in obj:
        if key not in allowed:
            invalid_arg("unknown %s field %r" % (what, key))


def parse_transform(transform):
    if not isinstance(transform, dict):
        invalid_arg("transform must be an object")
    typ = transform.get("type")
    if not isinstance(typ, str):
        invalid_arg("transform.type must be a string")

    if typ == "constant":
        _check_declared_keys(transform, {"type", "value"}, "transform")
        if "value" not in transform:
            invalid_arg("constant transform requires value")
        return ("constant", transform["value"])

    if typ in _BINARY_MATH:
        _check_declared_keys(transform, {"type", "value"}, "transform")
        if "value" not in transform:
            invalid_arg("%s transform requires value" % typ)
        value = require_i64(transform["value"], "transform value")
        return (typ, value)

    if typ in ("negate", "abs", "stringify", "type"):
        _check_declared_keys(transform, {"type"}, "transform")
        return (typ, None)

    if typ == "op":
        _check_declared_keys(transform, {"type", "op", "args"}, "transform")
        op = transform.get("op")
        if not isinstance(op, str) or op not in _BINARY_MATH:
            invalid_arg("op transform requires a supported binary op")
        args = transform.get("args")
        if not isinstance(args, dict):
            invalid_arg("op transform requires an args object")
        _check_declared_keys(args, {"a", "b"}, "transform args")
        if "a" not in args or "b" not in args:
            invalid_arg("op transform requires a and b")

        def is_self(v):
            return (
                isinstance(v, dict)
                and len(v) == 1
                and v.get("$self") is True
            )

        a_self = is_self(args["a"])
        b_self = is_self(args["b"])
        if a_self == b_self:
            invalid_arg("op transform needs exactly one $self operand")
        if a_self:
            literal = require_i64(args["b"], "transform operand")
            return ("op", op, "a", literal)
        literal = require_i64(args["a"], "transform operand")
        return ("op", op, "b", literal)

    invalid_arg("unknown transform type %r" % typ)


def apply_transform(parsed, element):
    kind = parsed[0]

    if kind == "constant":
        return parsed[1]

    if kind in _BINARY_MATH:
        a = require_i64(element, "map element")
        b = parsed[1]
        return _BINARY_MATH[kind](a, b)

    if kind == "negate":
        a = require_i64(element, "map element")
        return check_i64(-a)

    if kind == "abs":
        a = require_i64(element, "map element")
        if a == INT64_MIN:
            raise WFError("ARITHMETIC_OVERFLOW", "abs(INT64_MIN) overflow")
        return abs(a)

    if kind == "stringify":
        return json_compact(element)

    if kind == "type":
        return type_name(element)

    # kind == "op"
    _, op, self_side, literal = parsed
    element = require_i64(element, "map element")
    if self_side == "a":
        return _BINARY_MATH[op](element, literal)
    return _BINARY_MATH[op](literal, element)


def parse_predicate(predicate):
    if not isinstance(predicate, dict):
        invalid_arg("predicate must be an object")
    typ = predicate.get("type")
    if not isinstance(typ, str):
        invalid_arg("predicate.type must be a string")

    if typ in ("even", "odd", "truthy", "falsy"):
        _check_declared_keys(predicate, {"type"}, "predicate")
        return (typ, None)

    if typ in ("gt", "gte", "lt", "lte", "eq", "ne"):
        _check_declared_keys(predicate, {"type", "value"}, "predicate")
        if "value" not in predicate:
            invalid_arg("%s predicate requires value" % typ)
        value = require_i64(predicate["value"], "predicate value")
        return (typ, value)

    if typ == "type_is":
        # The spec's example uses two `type` keys; JSON parsers normally keep
        # the last one.  Accept the clear spellings as well as that collapsed
        # form so all plausible documents behave sensibly.
        allowed = {"type", "value", "target", "target_type", "type_name"}
        _check_declared_keys(predicate, allowed, "predicate")
        target = None
        for key in ("value", "target", "target_type", "type_name"):
            if key in predicate:
                target = predicate[key]
                break
        if target is None:
            invalid_arg("type_is predicate requires a type name")
        if not isinstance(target, str) or target not in TYPE_NAMES:
            invalid_arg("type_is predicate requires a valid type name")
        return ("type_is", target)

    # Collapsed duplicate-key form: {"type":"type_is","type":"number"}.
    if typ in TYPE_NAMES:
        _check_declared_keys(predicate, {"type"}, "predicate")
        return ("type_is", typ)

    if typ == "string_contains":
        _check_declared_keys(predicate, {"type", "value"}, "predicate")
        if "value" not in predicate:
            invalid_arg("string_contains predicate requires value")
        value = require_string(predicate["value"], "predicate value")
        return (typ, value)

    invalid_arg("unknown predicate type %r" % typ)


def apply_predicate(parsed, element):
    kind, value = parsed

    if kind == "truthy":
        return truthy(element)
    if kind == "falsy":
        return not truthy(element)
    if kind == "type_is":
        return type_name(element) == value
    if kind == "string_contains":
        if not isinstance(element, str):
            invalid_arg("string_contains element must be a string")
        return value in element

    if kind in ("even", "odd"):
        n = require_i64(element, "filter element")
        is_even = (n % 2) == 0
        return is_even if kind == "even" else not is_even

    # Comparisons use the same total ordering as `sort` and `compare`.
    c = total_compare(element, value)
    if kind == "eq":
        return c == 0
    if kind == "ne":
        return c != 0
    if kind == "gt":
        return c > 0
    if kind == "gte":
        return c >= 0
    if kind == "lt":
        return c < 0
    if kind == "lte":
        return c <= 0

    invalid_arg("unknown predicate")


# ---------------------------------------------------------------------------
# Operations
# ---------------------------------------------------------------------------

def execute_op(op, args, ctx, step):
    sid = step["id"]

    if op in ("add", "subtract", "multiply", "divide", "mod"):
        check_args(args, {"a", "b"}, {"a", "b"})
        a = require_i64(args["a"], "a")
        b = require_i64(args["b"], "b")
        return _BINARY_MATH[op](a, b)

    if op == "sum":
        check_args(args, {"input"}, {"input"})
        values = require_array(args["input"])
        total = 0
        for v in values:
            total = int_add(total, require_i64(v, "sum element"))
        return total

    if op in ("min", "max"):
        check_args(args, {"input"}, {"input"})
        values = require_array(args["input"])
        if not values:
            raise WFError("EMPTY_ARRAY", "%s of empty array" % op)
        checked = [require_i64(v, "%s element" % op) for v in values]
        return min(checked) if op == "min" else max(checked)

    if op == "range":
        check_args(args, {"start", "end"}, {"start", "end"})
        start = require_i64(args["start"], "start")
        end = require_i64(args["end"], "end")
        if start > end:
            return []
        return list(range(start, end + 1))

    if op == "concat":
        check_args(args, {"a", "b"}, {"a", "b"})
        a = require_string(args["a"], "a")
        b = require_string(args["b"], "b")
        return a + b

    if op == "split":
        check_args(args, {"input", "sep"}, {"input", "sep"})
        text = require_string(args["input"], "input")
        sep = require_string(args["sep"], "sep")
        if sep == "":
            return list(text)
        return text.split(sep)

    if op == "join":
        check_args(args, {"input", "sep"}, {"input", "sep"})
        values = require_array(args["input"])
        sep = require_string(args["sep"], "sep")
        parts = []
        for v in values:
            parts.append(require_string(v, "join element"))
        return sep.join(parts)

    if op == "uppercase":
        check_args(args, {"input"}, {"input"})
        return require_string(args["input"], "input").translate(_UPPER_TABLE)

    if op == "lowercase":
        check_args(args, {"input"}, {"input"})
        return require_string(args["input"], "input").translate(_LOWER_TABLE)

    if op == "trim":
        check_args(args, {"input"}, {"input"})
        return require_string(args["input"], "input").strip(ASCII_WS)

    if op == "replace":
        check_args(
            args, {"input", "search", "replace"}, {"input", "search", "replace"}
        )
        text = require_string(args["input"], "input")
        search = require_string(args["search"], "search")
        replacement = require_string(args["replace"], "replace")
        if search == "":
            invalid_arg("replace search must not be empty")
        return text.replace(search, replacement)

    if op == "length":
        check_args(args, {"input"}, {"input"})
        return len(require_string(args["input"], "input"))

    if op == "map":
        check_args(args, {"input", "transform"}, {"input", "transform"})
        values = require_array(args["input"])
        transform = parse_transform(args["transform"])
        return [apply_transform(transform, v) for v in values]

    if op == "filter":
        check_args(args, {"input", "predicate"}, {"input", "predicate"})
        values = require_array(args["input"])
        predicate = parse_predicate(args["predicate"])
        return [v for v in values if apply_predicate(predicate, v)]

    if op == "reduce":
        check_args(args, {"input", "op", "initial"}, {"input", "op"})
        values = require_array(args["input"])
        reduce_op = args["op"]
        if not isinstance(reduce_op, str) or reduce_op not in (
            "add", "subtract", "multiply"
        ):
            invalid_arg("reduce op must be add, subtract or multiply")
        if "initial" in args:
            acc = require_i64(args["initial"], "initial")
        else:
            acc = 0 if reduce_op in ("add", "subtract") else 1
        fn = _BINARY_MATH[reduce_op]
        for v in values:
            acc = fn(acc, require_i64(v, "reduce element"))
        return acc

    if op == "sort":
        check_args(args, {"input"}, {"input"})
        values = require_array(args["input"])
        return sorted(values, key=cmp_to_key(total_compare))

    if op == "reverse":
        check_args(args, {"input"}, {"input"})
        return list(reversed(require_array(args["input"])))

    if op == "unique":
        check_args(args, {"input"}, {"input"})
        values = require_array(args["input"])
        result = []
        for v in values:
            if not any(structural_equals(v, existing) for existing in result):
                result.append(v)
        return result

    if op == "slice":
        check_args(args, {"input", "start", "end"}, {"input", "start"})
        values = require_array(args["input"])
        start = require_i64(args["start"], "start")
        if "end" in args:
            end = require_i64(args["end"], "end")
            return values[start:end]
        return values[start:]

    if op == "flatten":
        check_args(args, {"input"}, {"input"})
        values = require_array(args["input"])
        result = []
        for v in values:
            if isinstance(v, list):
                result.extend(v)
            else:
                result.append(v)
        return result

    if op == "get":
        check_args(args, {"input", "key"}, {"input", "key"})
        obj = require_object(args["input"])
        key = require_string(args["key"], "key")
        return obj.get(key)

    if op == "set":
        check_args(args, {"input", "key", "value"}, {"input", "key", "value"})
        obj = require_object(args["input"])
        key = require_string(args["key"], "key")
        result = dict(obj)
        result[key] = args["value"]
        return result

    if op == "keys":
        check_args(args, {"input"}, {"input"})
        obj = require_object(args["input"])
        return sorted(obj.keys(), key=utf8_bytes)

    if op == "values":
        check_args(args, {"input"}, {"input"})
        obj = require_object(args["input"])
        return [obj[k] for k in sorted(obj.keys(), key=utf8_bytes)]

    if op == "merge":
        check_args(args, {"a", "b"}, {"a", "b"})
        a = require_object(args["a"], "a")
        b = require_object(args["b"], "b")
        result = dict(a)
        result.update(b)
        return result

    if op == "pick":
        check_args(args, {"input", "keys"}, {"input", "keys"})
        obj = require_object(args["input"])
        keys = require_array(args["keys"], "keys")
        for key in keys:
            require_string(key, "pick key")
        result = {}
        for key in sorted(set(keys), key=utf8_bytes):
            result[key] = obj.get(key)
        return result

    if op == "omit":
        check_args(args, {"input", "keys"}, {"input", "keys"})
        obj = require_object(args["input"])
        keys = require_array(args["keys"], "keys")
        for key in keys:
            require_string(key, "omit key")
        omitted = set(keys)
        result = {}
        for key in sorted(obj.keys(), key=utf8_bytes):
            if key not in omitted:
                result[key] = obj[key]
        return result

    if op == "constant":
        check_args(args, {"value"}, {"value"})
        return args["value"]

    if op == "equals":
        check_args(args, {"a", "b"}, {"a", "b"})
        return structural_equals(args["a"], args["b"])

    if op == "compare":
        check_args(args, {"a", "b", "op"}, {"a", "b", "op"})
        cmp_op = args["op"]
        if not isinstance(cmp_op, str) or cmp_op not in (
            "gt", "gte", "lt", "lte"
        ):
            invalid_arg("compare op must be gt, gte, lt or lte")
        c = total_compare(args["a"], args["b"])
        if cmp_op == "gt":
            return c > 0
        if cmp_op == "gte":
            return c >= 0
        if cmp_op == "lt":
            return c < 0
        return c <= 0

    if op == "and":
        check_args(args, {"a", "b"}, {"a", "b"})
        return truthy(args["a"]) and truthy(args["b"])

    if op == "or":
        check_args(args, {"a", "b"}, {"a", "b"})
        return truthy(args["a"]) or truthy(args["b"])

    if op == "not":
        check_args(args, {"a"}, {"a"})
        return not truthy(args["a"])

    if op == "type":
        check_args(args, {"value"}, {"value"})
        return type_name(args["value"])

    if op == "fail_until":
        check_args(args, {"failures", "value"}, {"failures", "value"})
        failures = require_i64(args["failures"], "failures")
        if failures < 0 or failures > 10:
            invalid_arg("failures must be between 0 and 10")
        ctx.setdefault("fail_counts", {})
        count = ctx["fail_counts"].get(sid, 0) + 1
        ctx["fail_counts"][sid] = count
        if count <= failures:
            raise WFError(
                "TRANSIENT_FAILURE", "transient failure on attempt %d" % count
            )
        return args["value"]

    invalid_arg("unknown operation %r" % op)


# ---------------------------------------------------------------------------
# Reference resolution and condition evaluation
# ---------------------------------------------------------------------------

def is_reference(value):
    return (
        isinstance(value, dict)
        and len(value) == 1
        and "$ref" in value
        and isinstance(value["$ref"], str)
    )


def execute_workflow(steps):
    step_by_id = {s["id"]: s for s in steps}
    ids = [s["id"] for s in steps]
    deps = {s["id"]: list(s.get("depends_on", [])) for s in steps}

    # Transitive dependency closures.
    closure = {}
    for sid in ids:
        seen = set()
        stack = list(deps[sid])
        while stack:
            dep = stack.pop()
            if dep not in seen:
                seen.add(dep)
                stack.extend(deps[dep])
        closure[sid] = seen

    results = {}
    execution_order = []
    skipped = []
    skipped_set = set()
    done = set()
    ctx = {"fail_counts": {}}

    def resolve_reference(ref, step):
        sid = step["id"]
        ref_text = ref["$ref"]
        parts = ref_text.split(".")
        target = parts[0]

        if target not in step_by_id:
            invalid_reference("unknown reference target %r" % target)
        if target == sid or target not in closure[sid]:
            invalid_reference(
                "reference target %r is not a dependency of %r" % (target, sid)
            )
        if target in skipped_set:
            raise WFError(
                "SKIPPED_REFERENCE",
                "reference to skipped step %r" % target,
            )
        if target not in results:
            invalid_reference("reference target %r has not run" % target)

        current = results[target]
        for segment in parts[1:]:
            if isinstance(current, dict):
                if segment not in current:
                    invalid_reference("missing object key %r" % segment)
                current = current[segment]
            elif isinstance(current, list):
                if not ARRAY_INDEX_RE.match(segment):
                    invalid_reference("invalid array index %r" % segment)
                index = int(segment)
                if index >= len(current):
                    invalid_reference("array index %r out of range" % segment)
                current = current[index]
            else:
                invalid_reference(
                    "cannot descend into %s with %r"
                    % (type_name(current), segment)
                )
        return current

    def resolve_value(value, step):
        if is_reference(value):
            return resolve_reference(value, step)
        if isinstance(value, dict):
            return {k: resolve_value(v, step) for k, v in value.items()}
        if isinstance(value, list):
            return [resolve_value(v, step) for v in value]
        return value

    def condition_operand(value, step):
        if is_reference(value):
            return resolve_reference(value, step)
        return value

    def eval_condition(cond, step):
        op = cond["op"]
        if op in COND_BINARY:
            left = condition_operand(cond["left"], step)
            right = condition_operand(cond["right"], step)
            c = total_compare(left, right)
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
            return c <= 0

        if op == "all":
            for sub in cond["conditions"]:
                value = eval_condition(sub, step)
                if not isinstance(value, bool):
                    raise WFError(
                        "INVALID_CONDITION", "condition is not boolean"
                    )
                if not value:
                    return False
            return True

        if op == "any":
            for sub in cond["conditions"]:
                value = eval_condition(sub, step)
                if not isinstance(value, bool):
                    raise WFError(
                        "INVALID_CONDITION", "condition is not boolean"
                    )
                if value:
                    return True
            return False

        # not
        value = eval_condition(cond["condition"], step)
        if not isinstance(value, bool):
            raise WFError("INVALID_CONDITION", "condition is not boolean")
        return not value

    def failure(code, step_id, message):
        return {
            "status": "failed",
            "results": results,
            "execution_order": execution_order,
            "skipped": skipped,
            "error": {
                "code": code,
                "step": step_id,
                "message": message,
            },
        }

    while len(done) < len(steps):
        chosen = None
        for step in steps:
            sid = step["id"]
            if sid in done:
                continue
            if all(dep in done for dep in deps[sid]):
                chosen = step
                break

        if chosen is None:
            # Structural validation should make this unreachable.
            raise RuntimeError("no ready step found")

        sid = chosen["id"]

        try:
            if "when" in chosen:
                when_value = eval_condition(chosen["when"], chosen)
                if not isinstance(when_value, bool):
                    raise WFError(
                        "INVALID_CONDITION", "when did not evaluate to boolean"
                    )
                if not when_value:
                    results[sid] = None
                    skipped.append(sid)
                    skipped_set.add(sid)
                    execution_order.append(sid)
                    done.add(sid)
                    continue

            args = resolve_value(chosen.get("args", {}), chosen)
            retry = chosen.get("retry", {})
            max_attempts = retry.get("max_attempts", 1)
            attempt = 0

            while True:
                attempt += 1
                try:
                    value = execute_op(chosen["op"], args, ctx, chosen)
                    break
                except WFError as exc:
                    if exc.code == "TRANSIENT_FAILURE":
                        if attempt < max_attempts:
                            continue
                        raise WFError(
                            "STEP_FAILED",
                            "step failed after %d attempt(s)" % attempt,
                        )
                    raise

            results[sid] = value
            execution_order.append(sid)
            done.add(sid)
        except WFError as exc:
            return failure(exc.code, sid, exc.message), 1

    return {
        "status": "completed",
        "results": results,
        "execution_order": execution_order,
        "skipped": skipped,
        "error": None,
    }, 0


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def error_response(code, step, message):
    return {
        "status": "failed",
        "results": {},
        "execution_order": [],
        "skipped": [],
        "error": {"code": code, "step": step, "message": message},
    }


def emit(response, exit_code):
    try:
        text = json.dumps(
            response,
            ensure_ascii=False,
            separators=(",", ":"),
            allow_nan=False,
        )
        data = text.encode("utf-8")
    except Exception:
        # Last-resort, fully-ASCII encoding for pathological strings.
        text = json.dumps(
            response,
            ensure_ascii=True,
            separators=(",", ":"),
            allow_nan=False,
        )
        data = text.encode("ascii")
    sys.stdout.buffer.write(data + b"\n")
    sys.stdout.buffer.flush()
    return exit_code


def _reject_constant(token):
    raise ValueError("non-standard JSON constant %s" % token)


def _parse_float(token):
    value = float(token)
    if not math.isfinite(value):
        raise ValueError("non-finite JSON number")
    return value


def main(argv):
    if len(argv) != 2 or argv[0] != "run":
        return emit(
            error_response(
                "INVALID_WORKFLOW",
                None,
                "usage: workflow run <workflow.json> | workflow run -",
            ),
            2,
        )

    target = argv[1]
    try:
        if target == "-":
            raw = sys.stdin.buffer.read()
        else:
            with open(target, "rb") as handle:
                raw = handle.read()
        text = raw.decode("utf-8")
    except Exception as exc:
        return emit(
            error_response("INVALID_WORKFLOW", None, "cannot read input: %s" % exc),
            2,
        )

    try:
        document = json.loads(
            text,
            parse_constant=_reject_constant,
            parse_float=_parse_float,
        )
    except Exception as exc:
        return emit(
            error_response("INVALID_WORKFLOW", None, "invalid JSON: %s" % exc),
            2,
        )

    try:
        steps = validate_workflow(document)
    except WFError as exc:
        return emit(
            error_response(exc.code, exc.step, exc.message),
            2,
        )
    except Exception as exc:
        return emit(
            error_response("INTERNAL_ERROR", None, "internal error: %s" % exc),
            3,
        )

    try:
        response, exit_code = execute_workflow(steps)
        return emit(response, exit_code)
    except WFError as exc:
        # Should normally be handled inside execute_workflow.
        return emit(
            error_response(exc.code, exc.step, exc.message),
            1 if exc.code != "INVALID_WORKFLOW" else 2,
        )
    except Exception as exc:
        return emit(
            error_response("INTERNAL_ERROR", None, "internal error: %s" % exc),
            3,
        )


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
