#!/usr/bin/env python3
"""Self-test for the C `expr` implementation.

Each case is (name, request-object, expected-stdout-bytes).  The expected
stdout is the *complete* JSON document printed by the CLI (newline included),
so this also checks formatting, key order and error positions.

Run from the project directory:  python3 selftest/run.py
"""
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
EXE = os.path.join(ROOT, "expr")


def ok(value):
    return json.dumps({"status": "ok", "value": value},
                      ensure_ascii=False, separators=(",", ":")) + "\n"


def err(code, position, message):
    return ('{"status":"error","code":"%s","position":%s,"message":"%s"}\n'
            % (code, "null" if position is None else position, message))


CASES = []


def case(name, source, expected, vars=None):
    req = {"source": source}
    if vars is not None:
        req["vars"] = vars
    CASES.append((name, req, expected))


# ---------------------------------------------------------------- literals
case("int", "42", ok(42))
case("int-leading-zeros", "007", ok(7))
case("int-max", "9223372036854775807", ok(9223372036854775807))
case("string", '"hello"', ok("hello"))
case("string-empty", '""', ok(""))
case("string-escapes", r'"a\"b\\c\/d\ne\tf\rg\bh\fi"', ok('a"b\\c/d\ne\tf\rg\bh\fi'))
case("string-unicode-bmp", r'"caf\u00e9"', ok("café"))
case("string-unicode-pair", r'"\ud83d\ude00"', ok("\U0001f600"))
case("string-raw-unicode", '"héllo 😀"', ok("héllo 😀"))
case("true", "true", ok(True))
case("false", "false", ok(False))
case("null", "null", ok(None))

# --------------------------------------------------------------- precedence
case("prec-mul", "1 + 2 * 3", ok(7))
case("prec-paren", "(1 + 2) * 3", ok(9))
case("prec-unary", "2 * -3", ok(-6))
case("prec-unary-nested", "- -5", ok(5))
case("prec-not-not", "!!true", ok(True))
case("prec-left-sub", "10 - 3 - 2", ok(5))
case("prec-left-div", "100 / 5 / 2", ok(10))
case("prec-div-mod", "7 / 2", ok(3))
case("prec-div-neg", "-7 / 2", ok(-3))
case("prec-mod-neg", "-7 % 3", ok(-1))
case("prec-mod-neg2", "7 % -3", ok(1))
case("prec-add-mul", "2 * 3 + 4 * 5", ok(26))
case("prec-cmp-eq", "1 < 2 == true", ok(True))
case("prec-and-or", "true || false && false", ok(True))
case("prec-parens", "((((1))))", ok(1))
case("prec-ternary-true", "x > 3 ? \"big\" : \"small\"", ok("big"), {"x": 5})
case("prec-ternary-false", "x > 3 ? \"big\" : \"small\"", ok("small"), {"x": 1})
case("prec-ternary-right", "true ? 1 : false ? 2 : 3", ok(1))
case("prec-ternary-right2", "false ? 1 : true ? 2 : 3", ok(2))
case("prec-ternary-nested-then", "true ? false ? 8 : 9 : 10", ok(9))

# ------------------------------------------------------------ short circuit
case("sc-and-false", "false && missing", ok(False))
case("sc-or-true", "true || missing", ok(True))
case("sc-and-false-div", "false && (1 / 0 == 0)", ok(False))
case("sc-or-true-div", "true || 1 / 0 == 0", ok(True))
case("sc-ternary-skip", "true ? 1 : missing", ok(1))
case("sc-ternary-skip2", "false ? missing : 2", ok(2))
case("sc-ternary-unselected-unknown", "false ? missing : missing2", err("UNKNOWN_VARIABLE", 18, "unknown variable"))
case("sc-and-biglit", "false && 99999999999999999999", ok(False))
case("sc-or-biglit", "true || 99999999999999999999", ok(True))

# ------------------------------------------------------------------ arrays
case("array-empty", "[]", ok([]))
case("array", "[1, 2, len(\"abc\")]", ok([1, 2, 3]))
case("array-nested", "[1, [2, [3, []]], null, true, \"s\"]",
     ok([1, [2, [3, []]], None, True, "s"]))
case("array-eq", "[1, 2] == [1, 2]", ok(True))
case("array-ne", "[1, 2] == [1, 3]", ok(False))
case("array-len-diff", "[1] == [1, 2]", ok(False))
case("array-nested-eq", "[1, [2]] == [1, [2]]", ok(True))

# --------------------------------------------------------------- equality
case("eq-int-str", '1 == "1"', ok(False))
case("eq-null", "null == null", ok(True))
case("eq-null-false", "null == false", ok(False))
case("eq-bool-int", "true == 1", ok(False))
case("eq-ne", "1 != 2", ok(True))
case("eq-strings", '"abc" == "abc"', ok(True))

# ------------------------------------------------------------- comparison
case("cmp-int", "1 < 2", ok(True))
case("cmp-int-le", "2 <= 2", ok(True))
case("cmp-int-gt", "3 > 4", ok(False))
case("cmp-int-ge", "4 >= 4", ok(True))
case("cmp-str", '"abc" < "abd"', ok(True))
case("cmp-str-case", '"Z" < "a"', ok(True))
case("cmp-str-utf8", '"é" > "z"', ok(True))
case("cmp-str-neg", "-1 < 0", ok(True))

# -------------------------------------------------------------- functions
case("len-str", 'len("abc")', ok(3))
case("len-uni", 'len("héllo")', ok(5))
case("len-emoji", r'len("\ud83d\ude00")', ok(1))
case("len-arr", "len([1, 2, 3])", ok(3))
case("len-empty", 'len("")', ok(0))
case("sum", "sum([1, 2, 3])", ok(6))
case("sum-empty", "sum([])", ok(0))
case("sum-neg", "sum([-1, -2, 5])", ok(2))
case("min", "min([3, 1, 2])", ok(1))
case("max", "max([3, 1, 2])", ok(3))
case("min-one", "min([7])", ok(7))
case("abs-pos", "abs(5)", ok(5))
case("abs-neg", "abs(-5)", ok(5))
case("abs-min1", "abs(-9223372036854775807)", ok(9223372036854775807))
case("neg", "neg(5)", ok(-5))
case("neg-neg", "neg(-5)", ok(5))
case("upper", 'upper("aBc1é")', ok("ABC1é"))
case("lower", 'lower("AbC1É")', ok("abc1É"))
case("trim", r'trim("\u000b \t\n hi there \r\f\u000b")', ok("hi there"))
case("trim-none", 'trim("x")', ok("x"))
case("contains-yes", 'contains("hello", "ell")', ok(True))
case("contains-no", 'contains("hello", "xyz")', ok(False))
case("contains-empty", 'contains("hello", "")', ok(True))
case("starts-yes", 'starts_with("hello", "he")', ok(True))
case("starts-no", 'starts_with("hello", "lo")', ok(False))
case("starts-empty", 'starts_with("hello", "")', ok(True))
case("ends-yes", 'ends_with("hello", "lo")', ok(True))
case("ends-no", 'ends_with("hello", "he")', ok(False))
case("ends-empty", 'ends_with("hello", "")', ok(True))
case("substr-basic", 'substr("hello", 1, 3)', ok("ell"))
case("substr-neg-start", 'substr("hello", -2, 2)', ok("lo"))
case("substr-neg-far", 'substr("hello", -100, 2)', ok("he"))
case("substr-clamp-end", 'substr("hello", 3, 100)', ok("lo"))
case("substr-past-end", 'substr("hello", 10, 2)', ok(""))
case("substr-zero-len", 'substr("hello", 2, 0)', ok(""))
case("substr-uni", 'substr("héllo", 1, 2)', ok("él"))
case("substr-emoji", 'substr("a\\ud83d\\ude00b", 1, 1)', ok("\U0001f600"))
case("index-of", 'index_of("hello", "ll")', ok(2))
case("index-of-none", 'index_of("hello", "z")', ok(-1))
case("index-of-empty", 'index_of("hello", "")', ok(0))
case("index-of-uni", 'index_of("héllo", "llo")', ok(2))
case("join", 'join(["a", "b", "c"], "-")', ok("a-b-c"))
case("join-empty", 'join([], "-")', ok(""))
case("join-one", 'join(["a"], "-")', ok("a"))
case("join-empty-sep", 'join(["a", "b"], "")', ok("ab"))
case("split", 'split("a,b,c", ",")', ok(["a", "b", "c"]))
case("split-empty-piece", 'split("a,,b", ",")', ok(["a", "", "b"]))
case("split-no-sep", 'split("abc", "x")', ok(["abc"]))
case("split-empty-str", 'split("", ",")', ok([""]))
case("split-empty-sep", 'split("abc", "")', ok(["a", "b", "c"]))
case("split-empty-sep-uni", 'split("hé", "")', ok(["h", "é"]))
case("split-empty-both", 'split("", "")', ok([]))
case("type-null", "type(null)", ok("null"))
case("type-bool", "type(true)", ok("boolean"))
case("type-int", "type(1)", ok("integer"))
case("type-str", 'type("s")', ok("string"))
case("type-arr", "type([])", ok("array"))
case("to-string-int", "to_string(123)", ok("123"))
case("to-string-neg", "to_string(-5)", ok("-5"))
case("to-string-bool", "to_string(true)", ok("true"))
case("to-string-null", "to_string(null)", ok("null"))
case("to-string-str", 'to_string("x")', ok("x"))
case("to-string-arr", 'to_string([1, "a", null, true, [2]])',
     ok('[1,"a",null,true,[2]]'))
case("to-string-escape", 'to_string(["a\\"b\\n"])', ok('["a\\"b\\n"]'))

# ------------------------------------------------------------------ vars
case("vars-int", "x + 1", ok(6), {"x": 5})
case("vars-str", "x", ok("hi"), {"x": "hi"})
case("vars-bool", "!x", ok(False), {"x": True})
case("vars-null", "x == null", ok(True), {"x": None})
case("vars-array", "sum(a)", ok(6), {"a": [1, 2, 3]})
case("vars-nested-array", "len(a)", ok(3), {"a": [[1], [2, 3], []]})
case("vars-min-int", "x", ok(-9223372036854775808),
     {"x": -9223372036854775808})
case("vars-max-int", "x + 0", ok(9223372036854775807),
     {"x": 9223372036854775807})
case("vars-unused-missing", "1", ok(1), {"other": 2})

# ---------------------------------------------------------------- errors
case("err-parse-add-eof", "1 +", err("PARSE_ERROR", 3, "parse error"))
case("err-parse-empty", "", err("PARSE_ERROR", 0, "parse error"))
case("err-parse-ws", "   ", err("PARSE_ERROR", 3, "parse error"))
case("err-parse-unclosed-paren", "(1", err("PARSE_ERROR", 2, "parse error"))
case("err-parse-double-op", "1 +* 2", err("PARSE_ERROR", 3, "parse error"))
case("err-parse-char", "@", err("PARSE_ERROR", 0, "parse error"))
case("err-parse-eq-single", "1 = 2", err("PARSE_ERROR", 2, "parse error"))
case("err-parse-bad-escape", r'"\q"', err("PARSE_ERROR", 0, "parse error"))
case("err-parse-unterminated", '"abc', err("PARSE_ERROR", 0, "parse error"))
case("err-parse-bad-unicode", r'"\u12"', err("PARSE_ERROR", 0, "parse error"))
case("err-parse-array-comma", "[1,]", err("PARSE_ERROR", 3, "parse error"))
case("err-parse-empty-parens", "()", err("PARSE_ERROR", 1, "parse error"))
case("err-parse-juxtapose", "1 2", err("PARSE_ERROR", 2, "parse error"))
case("err-parse-keyword-call", "true(1)", err("PARSE_ERROR", 4, "parse error"))
case("err-parse-ternary-no-colon", "1 ? 2", err("PARSE_ERROR", 5, "parse error"))
case("err-parse-ternary-cond-end", "true ?", err("PARSE_ERROR", 6, "parse error"))
case("err-parse-uni-pos", '"é" + 1', err("TYPE_ERROR", 4, "type error"))
case("err-overflow-lit", "9223372036854775808",
     err("OVERFLOW", 0, "integer overflow"))
case("err-overflow-lit-neg", "-9223372036854775808",
     err("OVERFLOW", 1, "integer overflow"))
case("err-overflow-add", "9223372036854775807 + 1",
     err("OVERFLOW", 20, "integer overflow"))
case("err-overflow-sub", "-9223372036854775807 - 2",
     err("OVERFLOW", 21, "integer overflow"))
case("err-overflow-mul", "2 * 9223372036854775807",
     err("OVERFLOW", 2, "integer overflow"))
case("err-overflow-unary", "-(-9223372036854775807 - 1)",
     err("OVERFLOW", 0, "integer overflow"))
case("err-overflow-div", "x / -1", err("OVERFLOW", 2, "integer overflow"),
     {"x": -9223372036854775808})
case("err-overflow-mod", "x % -1", err("OVERFLOW", 2, "integer overflow"),
     {"x": -9223372036854775808})
case("err-overflow-abs", "abs(x)", err("OVERFLOW", 0, "integer overflow"),
     {"x": -9223372036854775808})
case("err-overflow-neg", "neg(x)", err("OVERFLOW", 0, "integer overflow"),
     {"x": -9223372036854775808})
case("err-overflow-sum", "sum([9223372036854775807, 1])",
     err("OVERFLOW", 0, "integer overflow"))
case("err-div-zero", "1 / 0", err("DIVISION_BY_ZERO", 2, "division by zero"))
case("err-mod-zero", "1 % 0", err("DIVISION_BY_ZERO", 2, "division by zero"))
case("err-div-zero-right", "1 / (2 - 2)",
     err("DIVISION_BY_ZERO", 2, "division by zero"))
case("err-type-add", "1 + true", err("TYPE_ERROR", 2, "type error"))
case("err-type-str-add", '"a" + "b"', err("TYPE_ERROR", 4, "type error"))
case("err-type-unary-minus", '-"a"', err("TYPE_ERROR", 0, "type error"))
case("err-type-not", "!1", err("TYPE_ERROR", 0, "type error"))
case("err-type-cmp-mixed", '"a" < 1', err("TYPE_ERROR", 4, "type error"))
case("err-type-cmp-bools", "true < false", err("TYPE_ERROR", 5, "type error"))
case("err-type-cmp-null", "null <= null", err("TYPE_ERROR", 5, "type error"))
case("err-type-and", "true && 1", err("TYPE_ERROR", 5, "type error"))
case("err-type-or", "false || 1", err("TYPE_ERROR", 6, "type error"))
case("err-type-ternary", "1 ? 2 : 3", err("TYPE_ERROR", 2, "type error"))
case("err-unknown-var", "y", err("UNKNOWN_VARIABLE", 0, "unknown variable"))
case("err-unknown-var-pos", "1 + y", err("UNKNOWN_VARIABLE", 4, "unknown variable"))
case("err-unknown-var-nested", "[1, 2, zz]",
     err("UNKNOWN_VARIABLE", 7, "unknown variable"))
case("err-unknown-fn", "foo(1)", err("UNKNOWN_FUNCTION", 0, "unknown function"))
case("err-unknown-fn-args", "1 + foo(1)",
     err("UNKNOWN_FUNCTION", 4, "unknown function"))
case("err-unknown-fn-arg-err", "foo(missing)",
     err("UNKNOWN_VARIABLE", 4, "unknown variable"))
case("err-arity-zero", "len()", err("ARITY_ERROR", 0, "wrong number of arguments"))
case("err-arity-two", "len(1, 2)", err("ARITY_ERROR", 0, "wrong number of arguments"))
case("err-arity-substr", "substr(\"a\", 1)",
     err("ARITY_ERROR", 0, "wrong number of arguments"))
case("err-empty-min", "min([])", err("EMPTY_ARRAY", 0, "empty array"))
case("err-empty-max", "max([])", err("EMPTY_ARRAY", 0, "empty array"))
case("err-fn-type-len", "len(1)", err("TYPE_ERROR", 0, "type error"))
case("err-fn-type-sum", 'sum([1, "a"])', err("TYPE_ERROR", 0, "type error"))
case("err-fn-type-min", "min(5)", err("TYPE_ERROR", 0, "type error"))
case("err-fn-type-sum-nonarr", "sum(5)", err("TYPE_ERROR", 0, "type error"))
case("err-fn-type-substr-neg", 'substr("abc", 0, -1)',
     err("TYPE_ERROR", 0, "type error"))
case("err-fn-type-join", 'join([1], ",")', err("TYPE_ERROR", 0, "type error"))
case("err-fn-type-split", 'split(1, ",")', err("TYPE_ERROR", 0, "type error"))
case("err-fn-type-indexof", "index_of(1, \"a\")", err("TYPE_ERROR", 0, "type error"))
case("err-fn-pos-in-call", "1 + abs(\"x\")", err("TYPE_ERROR", 4, "type error"))
case("err-cmp-chain", "1 < 2 < 3", err("TYPE_ERROR", 6, "type error"))


# ------------------------------------------------------ invalid requests
INVALID = {"status": "error", "code": "INVALID_REQUEST", "position": None,
           "message": "invalid request"}
INVALID_BYTES = (json.dumps(INVALID, separators=(",", ":")) + "\n").encode()


def run_request_bytes(raw):
    p = subprocess.run([EXE, "eval", "-"], input=raw,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return p.returncode, p.stdout


def main():
    failures = 0
    total = 0
    for name, req, expected in CASES:
        total += 1
        raw = json.dumps(req, ensure_ascii=False).encode()
        rc, out = run_request_bytes(raw)
        exp = expected.encode()
        want_rc = 0 if expected.startswith('{"status":"ok"') else 1
        if out != exp or rc != want_rc:
            failures += 1
            print("FAIL %-32s" % name)
            print("  request : %r" % raw)
            print("  expected: %r (exit %d)" % (exp, want_rc))
            print("  actual  : %r (exit %d)" % (out, rc))

    invalid_requests = [
        ("missing source", b'{}'),
        ("source not string", b'{"source":1}'),
        ("source null", b'{"source":null}'),
        ("unknown field", b'{"source":"1","extra":1}'),
        ("vars not object", b'{"source":"1","vars":[]}'),
        ("vars null", b'{"source":"1","vars":null}'),
        ("vars object value", b'{"source":"1","vars":{"x":{"a":1}}}'),
        ("vars float", b'{"source":"1","vars":{"x":1.5}}'),
        ("vars big", b'{"source":"1","vars":{"x":9223372036854775808}}'),
        ("malformed json", b'{"source":'),
        ("trailing garbage", b'{"source":"1"} x'),
        ("top level array", b'[]'),
        ("top level string", b'"x"'),
        ("empty input", b''),
        ("bad escape", b'{"source":"\\q"}'),
        ("duplicate-ok-but-last-source", b'{"source":"1","source":"2"}'),
    ]
    for name, raw in invalid_requests:
        total += 1
        rc, out = run_request_bytes(raw)
        # duplicate keys: last one wins in this implementation
        if name == "duplicate-ok-but-last-source":
            if out != b'{"status":"ok","value":2}\n' or rc != 0:
                failures += 1
                print("FAIL %-32s (exit %d) got %r" % (name, rc, out))
            continue
        if out != INVALID_BYTES or rc != 2:
            failures += 1
            print("FAIL %-32s" % name)
            print("  request : %r" % raw)
            print("  expected: %r (exit 2)" % INVALID_BYTES)
            print("  actual  : %r (exit %d)" % (out, rc))

    # file argument form
    total += 1
    import tempfile
    with tempfile.NamedTemporaryFile("wb", suffix=".json", delete=False) as f:
        f.write(b'{"source":"1 + 2","vars":{}}')
        path = f.name
    try:
        p = subprocess.run([EXE, "eval", path], stdout=subprocess.PIPE)
        if p.returncode != 0 or p.stdout != b'{"status":"ok","value":3}\n':
            failures += 1
            print("FAIL file-argument (exit %d) got %r"
                  % (p.returncode, p.stdout))
    finally:
        os.unlink(path)

    print("%d/%d cases passed" % (total - failures, total))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
