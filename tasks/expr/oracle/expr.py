#!/usr/bin/env python3
"""Reference implementation of the expression evaluator (SPEC: tasks/expr/SPEC.md)."""
import json
import sys

I64_MIN, I64_MAX = -(2**63), 2**63 - 1


class EvalError(Exception):
    def __init__(self, code, position, message=""):
        super().__init__(code)
        self.code, self.position, self.message = code, position, message


class ParseError(EvalError):
    def __init__(self, position, message=""):
        super().__init__("PARSE_ERROR", position, message)


# ---------------- lexer ----------------
OPS2 = ("||", "&&", "==", "!=", "<=", ">=")
OPS1 = set("+-*/%<>!?()[],:")
KEYWORDS = {"true": True, "false": False, "null": None}

def lex(src):
    toks, i, n = [], 0, len(src)
    while i < n:
        c = src[i]
        if c in " \t\n\r":
            i += 1
            continue
        if c.isdigit():
            j = i
            while j < n and src[j].isdigit():
                j += 1
            text = src[i:j]
            v = int(text)
            if v > I64_MAX:
                raise EvalError("OVERFLOW", i, "integer literal out of range")
            toks.append(("INT", v, i))
            i = j
            continue
        if c == '"':
            val, j = read_string(src, i)
            toks.append(("STR", val, i))
            i = j
            continue
        if c.isalpha() or c == "_":
            j = i
            while j < n and (src[j].isalnum() or src[j] == "_"):
                j += 1
            toks.append(("IDENT", src[i:j], i))
            i = j
            continue
        two = src[i:i + 2]
        if two in OPS2:
            toks.append(("OP", two, i)); i += 2; continue
        if c in OPS1:
            toks.append(("OP", c, i)); i += 1; continue
        raise ParseError(i, f"unexpected character {c!r}")
    toks.append(("EOF", None, n))
    return toks


def read_string(src, start):
    i, n, out = start + 1, len(src), []
    while i < n:
        c = src[i]
        if c == '"':
            return "".join(out), i + 1
        if c == "\\":
            if i + 1 >= n:
                raise ParseError(i, "unterminated escape")
            e = src[i + 1]
            simple = {'"': '"', "\\": "\\", "/": "/", "n": "\n", "t": "\t",
                      "r": "\r", "b": "\b", "f": "\f"}
            if e in simple:
                out.append(simple[e]); i += 2; continue
            if e == "u":
                if i + 6 > n or any(ch not in "0123456789abcdefABCDEF" for ch in src[i+2:i+6]):
                    raise ParseError(i, "bad \\u escape")
                cp = int(src[i+2:i+6], 16)
                i += 6
                if 0xD800 <= cp <= 0xDBFF and src[i:i+2] == "\\u":
                    if i + 6 <= n and all(ch in "0123456789abcdefABCDEF" for ch in src[i+2:i+6]):
                        lo = int(src[i+2:i+6], 16)
                        if 0xDC00 <= lo <= 0xDFFF:
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00)
                            i += 6
                out.append(chr(cp))
                continue
            raise ParseError(i, f"bad escape \\{e}")
        if c == "\n":
            raise ParseError(i, "newline in string")
        out.append(c); i += 1
    raise ParseError(start, "unterminated string")


# ---------------- parser ----------------
class Parser:
    def __init__(self, toks):
        self.t, self.i = toks, 0

    def peek(self):
        return self.t[self.i]

    def next(self):
        tok = self.t[self.i]; self.i += 1; return tok

    def expect_op(self, op):
        k, v, p = self.peek()
        if k != "OP" or v != op:
            raise ParseError(p, f"expected {op!r}")
        return self.next()

    def parse(self):
        e = self.ternary()
        k, v, p = self.peek()
        if k != "EOF":
            raise ParseError(p, "trailing tokens")
        return e

    def ternary(self):
        cond = self.or_()
        k, v, p = self.peek()
        if k == "OP" and v == "?":
            self.next()
            a = self.ternary()
            self.expect_op(":")
            b = self.ternary()
            return ("ternary", cond, a, b, p)
        return cond

    def or_(self):
        e = self.and_()
        while self._is("||"):
            _, _, p = self.next(); e = ("bin", "||", e, self.and_(), p)
        return e

    def and_(self):
        e = self.equality()
        while self._is("&&"):
            _, _, p = self.next(); e = ("bin", "&&", e, self.equality(), p)
        return e

    def equality(self):
        e = self.comparison()
        while self._is("==") or self._is("!="):
            _, v, p = self.next(); e = ("bin", v, e, self.comparison(), p)
        return e

    def comparison(self):
        e = self.additive()
        while any(self._is(x) for x in ("<", "<=", ">", ">=")):
            _, v, p = self.next(); e = ("bin", v, e, self.additive(), p)
        return e

    def additive(self):
        e = self.multiplicative()
        while self._is("+") or self._is("-"):
            _, v, p = self.next(); e = ("bin", v, e, self.multiplicative(), p)
        return e

    def multiplicative(self):
        e = self.unary()
        while any(self._is(x) for x in ("*", "/", "%")):
            _, v, p = self.next(); e = ("bin", v, e, self.unary(), p)
        return e

    def unary(self):
        if self._is("-") or self._is("!"):
            _, v, p = self.next()
            return ("un", v, self.unary(), p)
        return self.primary()

    def primary(self):
        k, v, p = self.next()
        if k == "INT":
            return ("lit", v, p)
        if k == "STR":
            return ("lit", v, p)
        if k == "IDENT":
            if v in KEYWORDS:
                return ("lit", KEYWORDS[v], p)
            if self._is("("):
                self.next()
                args = []
                if not self._is(")"):
                    args.append(self.expr())
                    while self._is(","):
                        self.next(); args.append(self.expr())
                self.expect_op(")")
                return ("call", v, args, p)
            return ("var", v, p)
        if k == "OP" and v == "(":
            e = self.expr()
            self.expect_op(")")
            return e
        if k == "OP" and v == "[":
            elems = []
            if not self._is("]"):
                elems.append(self.expr())
                while self._is(","):
                    self.next(); elems.append(self.expr())
            self.expect_op("]")
            return ("array", elems, p)
        raise ParseError(p, f"unexpected token {v!r}")

    def expr(self):
        return self.ternary()

    def _is(self, op):
        k, v, _ = self.peek()
        return k == "OP" and v == op


# ---------------- evaluation ----------------
def check_i64(v, pos, op=""):
    if not (I64_MIN <= v <= I64_MAX):
        raise EvalError("OVERFLOW", pos, f"{op} overflow")
    return v


def is_int(v):
    return isinstance(v, int) and not isinstance(v, bool)


def ifelse(cond, a, b):
    return a if cond else b


def eq(a, b):
    if isinstance(a, bool) != isinstance(b, bool):
        return False
    if is_int(a) and is_int(b):
        return a == b
    if type(a) is not type(b):
        return (a is None and b is None)
    if isinstance(a, list):
        return len(a) == len(b) and all(eq(x, y) for x, y in zip(a, b))
    return a == b


def arith(op, a, b, pos):
    if not (is_int(a) and is_int(b)):
        raise EvalError("TYPE_ERROR", pos, f"{op} needs integers")
    if op == "+":
        return check_i64(a + b, pos, "+")
    if op == "-":
        return check_i64(a - b, pos, "-")
    if op == "*":
        return check_i64(a * b, pos, "*")
    if b == 0:
        raise EvalError("DIVISION_BY_ZERO", pos, "division by zero")
    if a == I64_MIN and b == -1:
        raise EvalError("OVERFLOW", pos, "INT64_MIN / -1")
    q = abs(a) // abs(b)
    q = q if (a >= 0) == (b >= 0) else -q
    if op == "/":
        return check_i64(q, pos, "/")
    r = abs(a) % abs(b)
    return r if a >= 0 else -r


def compare(op, a, b, pos):
    if is_int(a) and is_int(b):
        c = (a > b) - (a < b)
    elif isinstance(a, str) and isinstance(b, str):
        x, y = a.encode(), b.encode()
        c = (x > y) - (x < y)
    else:
        raise EvalError("TYPE_ERROR", pos, f"{op} needs two integers or two strings")
    return {"<": c < 0, "<=": c <= 0, ">": c > 0, ">=": c >= 0}[op]


def truth(v, pos, what):
    if not isinstance(v, bool):
        raise EvalError("TYPE_ERROR", pos, f"{what} needs a boolean")
    return v


def fn_len(args, pos):
    x = args[0]
    if isinstance(x, str):
        return len(x)
    if isinstance(x, list):
        return len(x)
    raise EvalError("TYPE_ERROR", pos, "len expects string or array")


def ints(arr, pos, name):
    if not isinstance(arr, list) or not all(is_int(x) for x in arr):
        raise EvalError("TYPE_ERROR", pos, f"{name} expects an array of integers")
    return arr


def fn_sum(args, pos):
    s = 0
    for x in ints(args[0], pos, "sum"):
        s = check_i64(s + x, pos, "sum")
    return s


def fn_min(args, pos):
    a = ints(args[0], pos, "min")
    if not a:
        raise EvalError("EMPTY_ARRAY", pos, "min of empty array")
    return min(a)


def fn_max(args, pos):
    a = ints(args[0], pos, "max")
    if not a:
        raise EvalError("EMPTY_ARRAY", pos, "max of empty array")
    return max(a)


def fn_abs(args, pos):
    x = args[0]
    if not is_int(x):
        raise EvalError("TYPE_ERROR", pos, "abs expects an integer")
    if x == I64_MIN:
        raise EvalError("OVERFLOW", pos, "abs(INT64_MIN)")
    return abs(x)


def fn_neg(args, pos):
    x = args[0]
    if not is_int(x):
        raise EvalError("TYPE_ERROR", pos, "neg expects an integer")
    if x == I64_MIN:
        raise EvalError("OVERFLOW", pos, "neg(INT64_MIN)")
    return -x


def ascii_case(s, up, pos, name):
    if not isinstance(s, str):
        raise EvalError("TYPE_ERROR", pos, f"{name} expects a string")
    out = []
    for ch in s:
        o = ord(ch)
        if 65 <= o <= 90:
            out.append(ch if up else chr(o + 32))
        elif 97 <= o <= 122:
            out.append(chr(o - 32) if up else ch)
        else:
            out.append(ch)
    return "".join(out)


def need_strs(args, pos, name, count):
    if len(args) != count or not all(isinstance(a, str) for a in args):
        raise EvalError("TYPE_ERROR", pos, f"{name} expects {count} strings")
    return args


def fn_substr(args, pos):
    s, start, ln = args
    if not isinstance(s, str) or not is_int(start) or not is_int(ln):
        raise EvalError("TYPE_ERROR", pos, "substr expects (string, integer, integer)")
    if ln < 0:
        raise EvalError("TYPE_ERROR", pos, "substr length must be >= 0")
    n = len(s)
    a = max(0, min(n, start + n if start < 0 else start))
    b = max(a, min(n, a + ln))
    return s[a:b]


def fn_index_of(args, pos):
    s, sub = need_strs(args, pos, "index_of", 2)
    return s.find(sub)


def fn_join(args, pos):
    arr, sep = args
    if not isinstance(arr, list) or not all(isinstance(x, str) for x in arr) or not isinstance(sep, str):
        raise EvalError("TYPE_ERROR", pos, "join expects (array of strings, string)")
    return sep.join(arr)


def fn_split(args, pos):
    s, sep = need_strs(args, pos, "split", 2)
    return list(s) if sep == "" else s.split(sep)


def fn_type(args, pos):
    x = args[0]
    if x is None:
        return "null"
    if isinstance(x, bool):
        return "boolean"
    if is_int(x):
        return "integer"
    if isinstance(x, str):
        return "string"
    return "array"


def fn_to_string(args, pos):
    x = args[0]
    if x is None:
        return "null"
    if isinstance(x, bool):
        return "true" if x else "false"
    if is_int(x):
        return str(x)
    if isinstance(x, str):
        return x
    return json.dumps(x, separators=(",", ":"), ensure_ascii=False)


FUNCS = {
    "len": (1, fn_len), "sum": (1, fn_sum), "min": (1, fn_min), "max": (1, fn_max),
    "abs": (1, fn_abs), "neg": (1, fn_neg), "type": (1, fn_type), "to_string": (1, fn_to_string),
    "upper": (1, lambda a, p: ascii_case(a[0], True, p, "upper")),
    "lower": (1, lambda a, p: ascii_case(a[0], False, p, "lower")),
    "trim": (1, lambda a, p: a[0].strip(" \t\n\r\f\v") if isinstance(a[0], str)
             else (_ for _ in ()).throw(EvalError("TYPE_ERROR", p, "trim expects a string"))),
    "contains": (2, lambda a, p: need_strs(a, p, "contains", 2)[1] in a[0]),
    "starts_with": (2, lambda a, p: need_strs(a, p, "starts_with", 2)[0].startswith(a[1])),
    "ends_with": (2, lambda a, p: need_strs(a, p, "ends_with", 2)[0].endswith(a[1])),
    "substr": (3, fn_substr), "index_of": (2, fn_index_of),
    "join": (2, fn_join), "split": (2, fn_split),
}


def eval_node(n, vars):
    kind = n[0]
    if kind == "lit":
        return n[1]
    if kind == "var":
        name, pos = n[1], n[2]
        if name not in vars:
            raise EvalError("UNKNOWN_VARIABLE", pos, f"unknown variable {name}")
        return vars[name]
    if kind == "array":
        return [eval_node(e, vars) for e in n[1]]
    if kind == "un":
        op, operand, pos = n[1], n[2], n[3]
        v = eval_node(operand, vars)
        if op == "-":
            if not is_int(v):
                raise EvalError("TYPE_ERROR", pos, "unary - needs an integer")
            if v == I64_MIN:
                raise EvalError("OVERFLOW", pos, "negate INT64_MIN")
            return -v
        return not truth(v, pos, "!")
    if kind == "bin":
        op, l, r, pos = n[1], n[2], n[3], n[4]
        if op == "&&":
            lv = truth(eval_node(l, vars), pos, "&&")
            if not lv:
                return False
            return truth(eval_node(r, vars), pos, "&&")
        if op == "||":
            lv = truth(eval_node(l, vars), pos, "||")
            if lv:
                return True
            return truth(eval_node(r, vars), pos, "||")
        lv, rv = eval_node(l, vars), eval_node(r, vars)
        if op in ("+", "-", "*", "/", "%"):
            return arith(op, lv, rv, pos)
        if op in ("<", "<=", ">", ">="):
            return compare(op, lv, rv, pos)
        if op == "==":
            return eq(lv, rv)
        if op == "!=":
            return not eq(lv, rv)
        raise EvalError("PARSE_ERROR", pos, f"bad operator {op}")
    if kind == "ternary":
        cond, a, b, pos = n[1], n[2], n[3], n[4]
        c = truth(eval_node(cond, vars), pos, "?")
        return eval_node(a if c else b, vars)
    if kind == "call":
        name, arg_nodes, pos = n[1], n[2], n[3]
        if name not in FUNCS:
            raise EvalError("UNKNOWN_FUNCTION", pos, f"unknown function {name}")
        arity, fn = FUNCS[name]
        if len(arg_nodes) != arity:
            raise EvalError("ARITY_ERROR", pos, f"{name} expects {arity} args")
        args = [eval_node(a, vars) for a in arg_nodes]
        return fn(args, pos)
    raise EvalError("PARSE_ERROR", 0, "bad node")


def evaluate(src, vars):
    ast = Parser(lex(src)).parse()
    return eval_node(ast, vars)


def main():
    argv = sys.argv[1:]
    if len(argv) != 2 or argv[0] != "eval":
        sys.stderr.write("usage: expr eval <request.json|->\n")
        sys.exit(2)
    try:
        raw = sys.stdin.read() if argv[1] == "-" else open(argv[1]).read()
    except OSError as e:
        print(json.dumps({"status": "error", "code": "INVALID_REQUEST", "position": None, "message": str(e)}))
        sys.exit(2)
    try:
        req = json.loads(raw)
    except json.JSONDecodeError as e:
        print(json.dumps({"status": "error", "code": "INVALID_REQUEST", "position": None, "message": str(e)}))
        sys.exit(2)
    if (not isinstance(req, dict) or set(req) - {"source", "vars"}
            or not isinstance(req.get("source"), str)
            or ("vars" in req and not isinstance(req["vars"], dict))):
        print(json.dumps({"status": "error", "code": "INVALID_REQUEST", "position": None,
                          "message": "bad request"}))
        sys.exit(2)
    try:
        value = evaluate(req["source"], req.get("vars", {}))
    except EvalError as e:
        print(json.dumps({"status": "error", "code": e.code, "position": e.position,
                          "message": e.message}))
        sys.exit(1)
    except Exception as e:  # internal
        print(json.dumps({"status": "error", "code": "INTERNAL", "position": None,
                          "message": f"{type(e).__name__}: {e}"}))
        sys.exit(3)
    print(json.dumps({"status": "ok", "value": value}))
    sys.exit(0)


if __name__ == "__main__":
    main()
