#!/usr/bin/env python3
"""expr - a small typed expression evaluator.

Implements the CLI contract described in tasks/expr/SPEC.md (version 1).
Only the Python standard library is used.
"""

import json
import sys

MIN_I64 = -(1 << 63)
MAX_I64 = (1 << 63) - 1

_WS = " \t\n\r"
_TRIM_WS = " \t\n\r\f\v"


# ---------------------------------------------------------------------------
# Errors
# ---------------------------------------------------------------------------

class ExprError(Exception):
    def __init__(self, code, position, message=""):
        super().__init__(message)
        self.code = code
        self.position = position
        self.message = message


def invalid_request(message):
    return ExprError("INVALID_REQUEST", None, message)


# ---------------------------------------------------------------------------
# Lexer
# ---------------------------------------------------------------------------

class Token:
    __slots__ = ("kind", "value", "pos")

    def __init__(self, kind, value, pos):
        self.kind = kind
        self.value = value
        self.pos = pos

    def __repr__(self):  # pragma: no cover - debugging aid
        return "Token(%r, %r, %r)" % (self.kind, self.value, self.pos)


_HEX = "0123456789abcdefABCDEF"


class Lexer:
    def __init__(self, source):
        self.source = source
        self.i = 0
        self.n = len(source)

    def _error(self, pos, message):
        raise ExprError("PARSE_ERROR", pos, message)

    def next_token(self):
        src = self.source
        n = self.n
        i = self.i

        while i < n and src[i] in _WS:
            i += 1

        if i >= n:
            self.i = n
            return Token("eof", None, n)

        c = src[i]

        # Integer literal: digits only.  Range checking happens in the parser
        # so that a stray token is still reported as a syntax error.
        if "0" <= c <= "9":
            start = i
            i += 1
            while i < n and "0" <= src[i] <= "9":
                i += 1
            self.i = i
            return Token("int", src[start:i], start)

        # String literal.
        if c == '"':
            return self._scan_string(i)

        # Identifier.
        if ("A" <= c <= "Z") or ("a" <= c <= "z") or c == "_":
            start = i
            i += 1
            while i < n:
                d = src[i]
                if (
                    ("A" <= d <= "Z")
                    or ("a" <= d <= "z")
                    or ("0" <= d <= "9")
                    or d == "_"
                ):
                    i += 1
                else:
                    break
            self.i = i
            return Token("ident", src[start:i], start)

        # Operators / punctuation.  Multi-character operators first.
        two = src[i:i + 2]
        if two in ("==", "!=", "<=", ">=", "&&", "||"):
            self.i = i + 2
            return Token("op", two, i)

        if c in "+-*/%<>!?:,()[]":
            self.i = i + 1
            return Token("op", c, i)

        self._error(i, "unexpected character %r" % c)

    def _scan_string(self, start):
        src = self.source
        n = self.n
        i = start + 1
        out = []

        while True:
            if i >= n:
                self._error(start, "unterminated string literal")
            c = src[i]

            if c == '"':
                self.i = i + 1
                return Token("string", "".join(out), start)

            if c != "\\":
                out.append(c)
                i += 1
                continue

            # Escape sequence.
            if i + 1 >= n:
                self._error(start, "unterminated escape sequence")
            e = src[i + 1]

            if e == '"':
                out.append('"')
                i += 2
            elif e == "\\":
                out.append("\\")
                i += 2
            elif e == "/":
                out.append("/")
                i += 2
            elif e == "n":
                out.append("\n")
                i += 2
            elif e == "t":
                out.append("\t")
                i += 2
            elif e == "r":
                out.append("\r")
                i += 2
            elif e == "b":
                out.append("\b")
                i += 2
            elif e == "f":
                out.append("\f")
                i += 2
            elif e == "u":
                cp, i = self._scan_unicode_escape(start, i)
                out.append(cp)
            else:
                self._error(start, "invalid escape sequence")

    def _scan_unicode_escape(self, str_start, backslash_pos):
        """Read \\uXXXX (and a trailing surrogate), returning (text, new_index)."""
        src = self.source
        n = self.n

        digits_pos = backslash_pos + 2
        if digits_pos + 4 > n:
            self._error(str_start, "incomplete unicode escape")

        digits = src[digits_pos:digits_pos + 4]
        for d in digits:
            if d not in _HEX:
                self._error(str_start, "invalid unicode escape")
        code = int(digits, 16)
        index = digits_pos + 4

        if 0xD800 <= code <= 0xDBFF:
            # High surrogate: must be followed by a \\uXXXX low surrogate.
            if index + 2 > n or src[index] != "\\" or src[index + 1] != "u":
                self._error(str_start, "unpaired high surrogate")
            low_pos = index + 2
            if low_pos + 4 > n:
                self._error(str_start, "incomplete unicode escape")
            low_digits = src[low_pos:low_pos + 4]
            for d in low_digits:
                if d not in _HEX:
                    self._error(str_start, "invalid unicode escape")
            low = int(low_digits, 16)
            if not (0xDC00 <= low <= 0xDFFF):
                self._error(str_start, "unpaired high surrogate")
            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00)
            index = low_pos + 4
        elif 0xDC00 <= code <= 0xDFFF:
            self._error(str_start, "unpaired low surrogate")

        return chr(code), index


# ---------------------------------------------------------------------------
# AST nodes
# ---------------------------------------------------------------------------

def is_int(value):
    return type(value) is int


def is_bool(value):
    return type(value) is bool


def check_i64(value, pos):
    if value < MIN_I64 or value > MAX_I64:
        raise ExprError("OVERFLOW", pos, "i64 overflow")
    return value


def type_name(value):
    if value is None:
        return "null"
    if type(value) is bool:
        return "boolean"
    if type(value) is int:
        return "integer"
    if type(value) is str:
        return "string"
    if type(value) is list:
        return "array"
    return "null"  # unreachable for validated values


def values_equal(a, b):
    ta = type(a)
    tb = type(b)
    if ta is not tb:
        return False
    if ta is list:
        if len(a) != len(b):
            return False
        for x, y in zip(a, b):
            if not values_equal(x, y):
                return False
        return True
    return a == b


def apply_unary(op, value, pos):
    if op == "-":
        if not is_int(value):
            raise ExprError("TYPE_ERROR", pos, "unary minus needs an integer")
        return check_i64(-value, pos)
    # "!"
    if not is_bool(value):
        raise ExprError("TYPE_ERROR", pos, "logical not needs a boolean")
    return not value


def apply_binary(op, left, right, pos):
    """Apply a non-short-circuiting binary operator."""
    if op in ("+", "-", "*", "/", "%"):
        if not is_int(left) or not is_int(right):
            raise ExprError(
                "TYPE_ERROR", pos, "arithmetic operands must be integers"
            )

        if op == "+":
            result = left + right
        elif op == "-":
            result = left - right
        elif op == "*":
            result = left * right
        elif op == "/":
            if right == 0:
                raise ExprError("DIVISION_BY_ZERO", pos, "division by zero")
            if left == MIN_I64 and right == -1:
                raise ExprError("OVERFLOW", pos, "i64 overflow")
            quotient = abs(left) // abs(right)
            if (left < 0) != (right < 0):
                quotient = -quotient
            result = quotient
        else:  # "%"
            if right == 0:
                raise ExprError("DIVISION_BY_ZERO", pos, "modulo by zero")
            if left == MIN_I64 and right == -1:
                raise ExprError("OVERFLOW", pos, "i64 overflow")
            quotient = abs(left) // abs(right)
            if (left < 0) != (right < 0):
                quotient = -quotient
            result = left - quotient * right

        return check_i64(result, pos)

    if op in ("<", "<=", ">", ">="):
        if is_int(left) and is_int(right):
            if op == "<":
                return left < right
            if op == "<=":
                return left <= right
            if op == ">":
                return left > right
            return left >= right
        if type(left) is str and type(right) is str:
            l = left.encode("utf-8")
            r = right.encode("utf-8")
            if op == "<":
                return l < r
            if op == "<=":
                return l <= r
            if op == ">":
                return l > r
            return l >= r
        raise ExprError(
            "TYPE_ERROR",
            pos,
            "comparison operands must both be integers or both be strings",
        )

    # "==" / "!="
    equal = values_equal(left, right)
    return equal if op == "==" else not equal


class Node:
    __slots__ = ()

    def eval(self, env):  # pragma: no cover - abstract
        raise NotImplementedError


class Literal(Node):
    __slots__ = ("value",)

    def __init__(self, value):
        self.value = value

    def eval(self, env):
        return self.value


class Variable(Node):
    __slots__ = ("name", "pos")

    def __init__(self, name, pos):
        self.name = name
        self.pos = pos

    def eval(self, env):
        if self.name not in env:
            raise ExprError(
                "UNKNOWN_VARIABLE", self.pos, "unknown variable %r" % self.name
            )
        return env[self.name]


class ArrayLiteral(Node):
    __slots__ = ("items",)

    def __init__(self, items):
        self.items = items

    def eval(self, env):
        return [item.eval(env) for item in self.items]


class Unary(Node):
    __slots__ = ("op", "operand", "pos")

    def __init__(self, op, operand, pos):
        self.op = op
        self.operand = operand
        self.pos = pos

    def eval(self, env):
        return apply_unary(self.op, self.operand.eval(env), self.pos)


class Binary(Node):
    __slots__ = ("op", "left", "right", "pos")

    def __init__(self, op, left, right, pos):
        self.op = op
        self.left = left
        self.right = right
        self.pos = pos

    def eval(self, env):
        op = self.op

        if op == "&&":
            left = self.left.eval(env)
            if not is_bool(left):
                raise ExprError("TYPE_ERROR", self.pos, "&& needs booleans")
            if not left:
                return False
            right = self.right.eval(env)
            if not is_bool(right):
                raise ExprError("TYPE_ERROR", self.pos, "&& needs booleans")
            return right

        if op == "||":
            left = self.left.eval(env)
            if not is_bool(left):
                raise ExprError("TYPE_ERROR", self.pos, "|| needs booleans")
            if left:
                return True
            right = self.right.eval(env)
            if not is_bool(right):
                raise ExprError("TYPE_ERROR", self.pos, "|| needs booleans")
            return right

        return apply_binary(op, self.left.eval(env), self.right.eval(env), self.pos)


class Ternary(Node):
    __slots__ = ("cond", "then", "otherwise", "pos")

    def __init__(self, cond, then, otherwise, pos):
        self.cond = cond
        self.then = then
        self.otherwise = otherwise
        self.pos = pos

    def eval(self, env):
        cond = self.cond.eval(env)
        if not is_bool(cond):
            raise ExprError(
                "TYPE_ERROR", self.pos, "ternary condition must be a boolean"
            )
        if cond:
            return self.then.eval(env)
        return self.otherwise.eval(env)


class Call(Node):
    __slots__ = ("name", "args", "pos")

    def __init__(self, name, args, pos):
        self.name = name
        self.args = args
        self.pos = pos

    def eval(self, env):
        args = [arg.eval(env) for arg in self.args]
        return call_function(self.name, args, self.pos)


# ---------------------------------------------------------------------------
# JSON string escaping (for to_string of arrays)
# ---------------------------------------------------------------------------

def json_escape_string(s):
    out = ['"']
    for ch in s:
        o = ord(ch)
        if ch == '"':
            out.append('\\"')
        elif ch == "\\":
            out.append("\\\\")
        elif ch == "\b":
            out.append("\\b")
        elif ch == "\f":
            out.append("\\f")
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\r":
            out.append("\\r")
        elif ch == "\t":
            out.append("\\t")
        elif o < 0x20 or 0xD800 <= o <= 0xDFFF:
            out.append("\\u%04x" % o)
        else:
            out.append(ch)
    out.append('"')
    return "".join(out)


def json_compact(value):
    if value is None:
        return "null"
    if type(value) is bool:
        return "true" if value else "false"
    if type(value) is int:
        return str(value)
    if type(value) is str:
        return json_escape_string(value)
    if type(value) is list:
        return "[" + ",".join(json_compact(item) for item in value) + "]"
    raise ExprError("TYPE_ERROR", None, "unsupported value")


# ---------------------------------------------------------------------------
# Built-in functions
# ---------------------------------------------------------------------------

ARITY = {
    "len": 1,
    "sum": 1,
    "min": 1,
    "max": 1,
    "abs": 1,
    "neg": 1,
    "upper": 1,
    "lower": 1,
    "trim": 1,
    "contains": 2,
    "starts_with": 2,
    "ends_with": 2,
    "substr": 3,
    "index_of": 2,
    "join": 2,
    "split": 2,
    "type": 1,
    "to_string": 1,
}


def _type_error(pos, message):
    raise ExprError("TYPE_ERROR", pos, message)


def _fn_len(args, pos):
    value = args[0]
    if type(value) is str or type(value) is list:
        return len(value)
    _type_error(pos, "len expects a string or an array")


def _fn_sum(args, pos):
    value = args[0]
    if type(value) is not list:
        _type_error(pos, "sum expects an array")
    total = 0
    for item in value:
        if not is_int(item):
            _type_error(pos, "sum expects an array of integers")
        total += item
        if total < MIN_I64 or total > MAX_I64:
            raise ExprError("OVERFLOW", pos, "i64 overflow")
    return total


def _fn_min(args, pos):
    value = args[0]
    if type(value) is not list:
        _type_error(pos, "min expects an array")
    if not value:
        raise ExprError("EMPTY_ARRAY", pos, "min of an empty array")
    best = None
    for item in value:
        if not is_int(item):
            _type_error(pos, "min expects an array of integers")
        if best is None or item < best:
            best = item
    return best


def _fn_max(args, pos):
    value = args[0]
    if type(value) is not list:
        _type_error(pos, "max expects an array")
    if not value:
        raise ExprError("EMPTY_ARRAY", pos, "max of an empty array")
    best = None
    for item in value:
        if not is_int(item):
            _type_error(pos, "max expects an array of integers")
        if best is None or item > best:
            best = item
    return best


def _fn_abs(args, pos):
    value = args[0]
    if not is_int(value):
        _type_error(pos, "abs expects an integer")
    if value == MIN_I64:
        raise ExprError("OVERFLOW", pos, "i64 overflow")
    return abs(value)


def _fn_neg(args, pos):
    value = args[0]
    if not is_int(value):
        _type_error(pos, "neg expects an integer")
    if value == MIN_I64:
        raise ExprError("OVERFLOW", pos, "i64 overflow")
    return -value


def _fn_upper(args, pos):
    value = args[0]
    if type(value) is not str:
        _type_error(pos, "upper expects a string")
    return "".join(
        chr(ord(ch) - 32) if "a" <= ch <= "z" else ch for ch in value
    )


def _fn_lower(args, pos):
    value = args[0]
    if type(value) is not str:
        _type_error(pos, "lower expects a string")
    return "".join(
        chr(ord(ch) + 32) if "A" <= ch <= "Z" else ch for ch in value
    )


def _fn_trim(args, pos):
    value = args[0]
    if type(value) is not str:
        _type_error(pos, "trim expects a string")
    return value.strip(_TRIM_WS)


def _fn_contains(args, pos):
    s, sub = args
    if type(s) is not str or type(sub) is not str:
        _type_error(pos, "contains expects strings")
    return sub in s


def _fn_starts_with(args, pos):
    s, prefix = args
    if type(s) is not str or type(prefix) is not str:
        _type_error(pos, "starts_with expects strings")
    return s.startswith(prefix)


def _fn_ends_with(args, pos):
    s, suffix = args
    if type(s) is not str or type(suffix) is not str:
        _type_error(pos, "ends_with expects strings")
    return s.endswith(suffix)


def _fn_substr(args, pos):
    s, start, length = args
    if type(s) is not str:
        _type_error(pos, "substr expects a string as first argument")
    if not is_int(start) or not is_int(length):
        _type_error(pos, "substr expects integer start and length")
    if length < 0:
        _type_error(pos, "substr length must not be negative")
    n = len(s)
    if start < 0:
        start += n
    if start < 0:
        start = 0
    elif start > n:
        start = n
    end = start + length
    if end > n:
        end = n
    return s[start:end]


def _fn_index_of(args, pos):
    s, sub = args
    if type(s) is not str or type(sub) is not str:
        _type_error(pos, "index_of expects strings")
    return s.find(sub)


def _fn_join(args, pos):
    array, sep = args
    if type(array) is not list:
        _type_error(pos, "join expects an array")
    if type(sep) is not str:
        _type_error(pos, "join expects a string separator")
    for item in array:
        if type(item) is not str:
            _type_error(pos, "join expects an array of strings")
    return sep.join(array)


def _fn_split(args, pos):
    s, sep = args
    if type(s) is not str or type(sep) is not str:
        _type_error(pos, "split expects strings")
    if sep == "":
        return list(s)
    return s.split(sep)


def _fn_type(args, pos):
    return type_name(args[0])


def _fn_to_string(args, pos):
    value = args[0]
    if value is None:
        return "null"
    if type(value) is bool:
        return "true" if value else "false"
    if type(value) is int:
        return str(value)
    if type(value) is str:
        return value
    if type(value) is list:
        return json_compact(value)
    raise ExprError("TYPE_ERROR", pos, "unsupported value")


FUNCTIONS = {
    "len": _fn_len,
    "sum": _fn_sum,
    "min": _fn_min,
    "max": _fn_max,
    "abs": _fn_abs,
    "neg": _fn_neg,
    "upper": _fn_upper,
    "lower": _fn_lower,
    "trim": _fn_trim,
    "contains": _fn_contains,
    "starts_with": _fn_starts_with,
    "ends_with": _fn_ends_with,
    "substr": _fn_substr,
    "index_of": _fn_index_of,
    "join": _fn_join,
    "split": _fn_split,
    "type": _fn_type,
    "to_string": _fn_to_string,
}


def call_function(name, args, pos):
    function = FUNCTIONS.get(name)
    if function is None:
        raise ExprError(
            "UNKNOWN_FUNCTION", pos, "unknown function %r" % name
        )
    if len(args) != ARITY[name]:
        raise ExprError(
            "ARITY_ERROR",
            pos,
            "%s expects %d argument(s)" % (name, ARITY[name]),
        )
    return function(args, pos)


# ---------------------------------------------------------------------------
# Iterative evaluator
# ---------------------------------------------------------------------------

def evaluate(root, env):
    """Evaluate an AST without recursion (large flat expressions are fine)."""
    values = []
    work = [("eval", root)]

    while work:
        task = work.pop()
        kind = task[0]

        if kind == "eval":
            node = task[1]
            if isinstance(node, Literal):
                values.append(node.value)
            elif isinstance(node, Variable):
                if node.name not in env:
                    raise ExprError(
                        "UNKNOWN_VARIABLE", node.pos,
                        "unknown variable %r" % node.name,
                    )
                values.append(env[node.name])
            elif isinstance(node, ArrayLiteral):
                work.append(("array", len(node.items)))
                for item in reversed(node.items):
                    work.append(("eval", item))
            elif isinstance(node, Unary):
                work.append(("unary", node.op, node.pos))
                work.append(("eval", node.operand))
            elif isinstance(node, Binary):
                if node.op == "&&":
                    work.append(("and", node.right, node.pos))
                    work.append(("eval", node.left))
                elif node.op == "||":
                    work.append(("or", node.right, node.pos))
                    work.append(("eval", node.left))
                else:
                    work.append(("binary", node.op, node.pos))
                    work.append(("eval", node.right))
                    work.append(("eval", node.left))
            elif isinstance(node, Ternary):
                work.append(
                    ("ternary", node.then, node.otherwise, node.pos)
                )
                work.append(("eval", node.cond))
            elif isinstance(node, Call):
                work.append(("call", node.name, len(node.args), node.pos))
                for arg in reversed(node.args):
                    work.append(("eval", arg))
            else:  # pragma: no cover - all node types are covered
                raise RuntimeError("unknown AST node")

        elif kind == "unary":
            values.append(apply_unary(task[1], values.pop(), task[2]))

        elif kind == "binary":
            right = values.pop()
            left = values.pop()
            values.append(apply_binary(task[1], left, right, task[2]))

        elif kind == "and":
            left = values.pop()
            if not is_bool(left):
                raise ExprError("TYPE_ERROR", task[2], "&& needs booleans")
            if not left:
                values.append(False)
            else:
                work.append(("and_right", task[2]))
                work.append(("eval", task[1]))

        elif kind == "and_right":
            right = values.pop()
            if not is_bool(right):
                raise ExprError("TYPE_ERROR", task[1], "&& needs booleans")
            values.append(right)

        elif kind == "or":
            left = values.pop()
            if not is_bool(left):
                raise ExprError("TYPE_ERROR", task[2], "|| needs booleans")
            if left:
                values.append(True)
            else:
                work.append(("or_right", task[2]))
                work.append(("eval", task[1]))

        elif kind == "or_right":
            right = values.pop()
            if not is_bool(right):
                raise ExprError("TYPE_ERROR", task[1], "|| needs booleans")
            values.append(right)

        elif kind == "ternary":
            condition = values.pop()
            if not is_bool(condition):
                raise ExprError(
                    "TYPE_ERROR", task[3],
                    "ternary condition must be a boolean",
                )
            work.append(("eval", task[1] if condition else task[2]))

        elif kind == "array":
            count = task[1]
            if count:
                items = values[-count:]
                del values[-count:]
            else:
                items = []
            values.append(items)

        elif kind == "call":
            count = task[2]
            if count:
                args = values[-count:]
                del values[-count:]
            else:
                args = []
            values.append(call_function(task[1], args, task[3]))

        else:  # pragma: no cover - internal consistency check
            raise RuntimeError("unknown evaluation task")

    return values[0]


# ---------------------------------------------------------------------------
# Parser
# ---------------------------------------------------------------------------

class Parser:
    def __init__(self, source):
        self.lexer = Lexer(source)
        self.source_length = len(source)
        self.current = None

    # -- token helpers ----------------------------------------------------
    def peek(self):
        if self.current is None:
            self.current = self.lexer.next_token()
        return self.current

    def advance(self):
        token = self.peek()
        self.current = None
        return token

    def _is_op(self, token, text):
        return token.kind == "op" and token.value == text

    def _error(self, token, message):
        raise ExprError("PARSE_ERROR", token.pos, message)

    def _expect_op(self, text):
        token = self.peek()
        if not self._is_op(token, text):
            self._error(token, "expected %r" % text)
        return self.advance()

    # -- grammar ----------------------------------------------------------
    def parse(self):
        node = self.parse_expr()
        token = self.peek()
        if token.kind != "eof":
            self._error(token, "unexpected token")
        return node

    def parse_expr(self):
        return self.parse_ternary()

    def parse_ternary(self):
        condition = self.parse_or()
        token = self.peek()
        if self._is_op(token, "?"):
            self.advance()
            then_branch = self.parse_expr()
            self._expect_op(":")
            else_branch = self.parse_expr()
            return Ternary(condition, then_branch, else_branch, token.pos)
        return condition

    def parse_or(self):
        node = self.parse_and()
        while True:
            token = self.peek()
            if self._is_op(token, "||"):
                self.advance()
                node = Binary("||", node, self.parse_and(), token.pos)
            else:
                return node

    def parse_and(self):
        node = self.parse_equality()
        while True:
            token = self.peek()
            if self._is_op(token, "&&"):
                self.advance()
                node = Binary("&&", node, self.parse_equality(), token.pos)
            else:
                return node

    def parse_equality(self):
        node = self.parse_comparison()
        while True:
            token = self.peek()
            if self._is_op(token, "==") or self._is_op(token, "!="):
                self.advance()
                node = Binary(
                    token.value, node, self.parse_comparison(), token.pos
                )
            else:
                return node

    def parse_comparison(self):
        node = self.parse_additive()
        while True:
            token = self.peek()
            if (
                self._is_op(token, "<")
                or self._is_op(token, "<=")
                or self._is_op(token, ">")
                or self._is_op(token, ">=")
            ):
                self.advance()
                node = Binary(token.value, node, self.parse_additive(), token.pos)
            else:
                return node

    def parse_additive(self):
        node = self.parse_multiplicative()
        while True:
            token = self.peek()
            if self._is_op(token, "+") or self._is_op(token, "-"):
                self.advance()
                node = Binary(
                    token.value, node, self.parse_multiplicative(), token.pos
                )
            else:
                return node

    def parse_multiplicative(self):
        node = self.parse_unary()
        while True:
            token = self.peek()
            if (
                self._is_op(token, "*")
                or self._is_op(token, "/")
                or self._is_op(token, "%")
            ):
                self.advance()
                node = Binary(token.value, node, self.parse_unary(), token.pos)
            else:
                return node

    def parse_unary(self):
        operators = []
        while True:
            token = self.peek()
            if token.kind == "op" and (token.value == "-" or token.value == "!"):
                self.advance()
                operators.append((token.value, token.pos))
            else:
                break
        node = self.parse_primary()
        for op, pos in reversed(operators):
            node = Unary(op, node, pos)
        return node

    def parse_primary(self):
        token = self.peek()

        if token.kind == "int":
            self.advance()
            digits = token.value.lstrip("0")
            if digits == "":
                return Literal(0)
            if len(digits) > 19:
                raise ExprError("OVERFLOW", token.pos, "integer literal out of range")
            value = int(digits)
            if value > MAX_I64:
                raise ExprError("OVERFLOW", token.pos, "integer literal out of range")
            return Literal(value)

        if token.kind == "string":
            self.advance()
            return Literal(token.value)

        if token.kind == "ident":
            self.advance()
            if token.value == "true":
                return Literal(True)
            if token.value == "false":
                return Literal(False)
            if token.value == "null":
                return Literal(None)

            if self._is_op(self.peek(), "("):
                self.advance()
                args = self.parse_arguments()
                return Call(token.value, args, token.pos)
            return Variable(token.value, token.pos)

        if self._is_op(token, "("):
            self.advance()
            node = self.parse_expr()
            self._expect_op(")")
            return node

        if self._is_op(token, "["):
            self.advance()
            items = []
            if self._is_op(self.peek(), "]"):
                self.advance()
                return ArrayLiteral(items)
            while True:
                items.append(self.parse_expr())
                following = self.peek()
                if self._is_op(following, ","):
                    self.advance()
                    continue
                if self._is_op(following, "]"):
                    self.advance()
                    return ArrayLiteral(items)
                self._error(following, "expected ',' or ']'")

        self._error(token, "unexpected token")

    def parse_arguments(self):
        args = []
        if self._is_op(self.peek(), ")"):
            self.advance()
            return args
        while True:
            args.append(self.parse_expr())
            token = self.peek()
            if self._is_op(token, ","):
                self.advance()
                continue
            if self._is_op(token, ")"):
                self.advance()
                return args
            self._error(token, "expected ',' or ')'")


# ---------------------------------------------------------------------------
# Request handling
# ---------------------------------------------------------------------------

def _has_surrogate(s):
    for ch in s:
        if 0xD800 <= ord(ch) <= 0xDFFF:
            return True
    return False


def _valid_value(value):
    if value is None:
        return True
    if type(value) is bool:
        return True
    if type(value) is int:
        return MIN_I64 <= value <= MAX_I64
    if type(value) is str:
        return not _has_surrogate(value)
    if type(value) is list:
        return all(_valid_value(item) for item in value)
    return False


def _reject_json_constant(_text):
    raise ValueError("non-finite JSON number")


def _decode_json(data):
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError:
        raise invalid_request("request is not valid UTF-8")
    try:
        return json.loads(text, parse_constant=_reject_json_constant)
    except (ValueError, TypeError):
        raise invalid_request("request is not valid JSON")


def _read_request_bytes(argv):
    if len(argv) != 1:
        raise invalid_request("usage: expr eval <request.json>")
    target = argv[0]
    if target == "-":
        try:
            return sys.stdin.buffer.read()
        except OSError as exc:
            raise invalid_request("cannot read stdin: %s" % exc)
    try:
        with open(target, "rb") as handle:
            return handle.read()
    except OSError as exc:
        raise invalid_request("cannot read request file: %s" % exc)


def load_request(argv):
    if not argv or argv[0] != "eval":
        raise invalid_request("unknown command")
    data = _read_request_bytes(argv[1:])
    request = _decode_json(data)

    if type(request) is not dict:
        raise invalid_request("request must be a JSON object")

    for key in request:
        if key not in ("source", "vars"):
            raise invalid_request("unknown request field %r" % key)

    if "source" not in request or type(request["source"]) is not str:
        raise invalid_request("'source' must be a string")
    source = request["source"]
    if _has_surrogate(source):
        raise invalid_request("'source' contains an unpaired surrogate")

    variables = request.get("vars", {})
    if type(variables) is not dict:
        raise invalid_request("'vars' must be an object")
    for name, value in variables.items():
        if not _valid_value(value):
            raise invalid_request("invalid value for variable %r" % name)

    return source, variables


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def _emit(response):
    text = json.dumps(response, ensure_ascii=False, separators=(",", ":"))
    data = text.encode("utf-8") + b"\n"
    sys.stdout.buffer.write(data)
    sys.stdout.buffer.flush()


def _error_response(code, position, message):
    return {
        "status": "error",
        "code": code,
        "position": position,
        "message": message,
    }


def main(argv):
    try:
        source, variables = load_request(argv)
        ast = Parser(source).parse()
        value = evaluate(ast, variables)
    except ExprError as exc:
        _emit(_error_response(exc.code, exc.position, exc.message))
        return 2 if exc.code == "INVALID_REQUEST" else 1
    except Exception as exc:  # internal failure: keep stdout a JSON document
        sys.stderr.write("internal error: %s\n" % exc)
        _emit(_error_response("INTERNAL_ERROR", None, "internal error"))
        return 3

    _emit({"status": "ok", "value": value})
    return 0


if __name__ == "__main__":
    sys.setrecursionlimit(300000)
    sys.exit(main(sys.argv[1:]))
