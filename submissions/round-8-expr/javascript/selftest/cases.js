'use strict';

/*
 * Self-test cases for the expr CLI.
 *
 * Every case declares the request and the expected response.  Positions were
 * derived by hand from SPEC.md section 6.1 (0-based offsets into `source`).
 */

const INT64_MIN = -9223372036854775808n;
const INT64_MAX = 9223372036854775807n;

function ok(source, value, vars, opts) {
  const request = { source };
  if (vars !== undefined) request.vars = vars;
  return Object.assign(
    { name: source, request, expected: { status: 'ok', value }, exit: 0 },
    opts || {}
  );
}

function err(source, code, position, vars, opts) {
  const request = { source };
  if (vars !== undefined) request.vars = vars;
  return Object.assign(
    {
      name: source + ' [' + code + ']',
      request,
      expected: { status: 'error', code, position },
      exit: 1,
    },
    opts || {}
  );
}

function badRequest(name, requestText) {
  return {
    name: 'invalid request: ' + name,
    request_text: requestText,
    expected: { status: 'error', code: 'INVALID_REQUEST', position: null },
    exit: 2,
  };
}

const cases = [];

// --- precedence, associativity, unary --------------------------------------
cases.push(ok('1 + 2 * 3', 7n));
cases.push(ok('(1 + 2) * 3', 9n));
cases.push(ok('2 * -3', -6n));
cases.push(ok('10 - 2 - 3', 5n));
cases.push(ok('100 / 10 / 5', 2n));
cases.push(ok('1 + 2 == 3 && 4 > 2', true));
cases.push(ok('true || false && false', true));
cases.push(ok('!true == false', true));
cases.push(ok('1 < 2 == true', true));
cases.push(ok('-7 / 2', -3n));
cases.push(ok('7 / -2', -3n));
cases.push(ok('-7 % 2', -1n));
cases.push(ok('7 % -2', 1n));
cases.push(ok('1 - -1', 2n));
cases.push(ok('2 * 3 % 4', 2n));
cases.push(ok('1 + 2 * 3 - 4 / 2', 5n));
cases.push(ok('-(1 + 2)', -3n));
cases.push(ok('!!true', true));
cases.push(ok('---5', -5n));
cases.push(ok('1 + \n\t 2', 3n));
cases.push(ok('(  (1) )', 1n));

// --- short circuit ---------------------------------------------------------
cases.push(ok('false && missing', false));
cases.push(ok('true || missing', true));
cases.push(ok('false && (1 / 0 == 0)', false));
cases.push(ok('true || (1 / 0 == 0)', true));
cases.push(ok('true && false', false));
cases.push(ok('false || true', true));
cases.push(ok('true || 1/0 == 0', true)); // right side never evaluated
cases.push(err('true && 1/0 == 0', 'DIVISION_BY_ZERO', 9));
cases.push(err('false || 1', 'TYPE_ERROR', 6));
cases.push(err('1 || true', 'TYPE_ERROR', 2));
cases.push(err('1 && true', 'TYPE_ERROR', 2));
cases.push(ok('false && 1', false)); // short-circuits before the bad operand
cases.push(err('true && 1', 'TYPE_ERROR', 5));

// --- ternary ---------------------------------------------------------------
cases.push(ok('x > 3 ? "big" : "small"', 'big', { x: 5n }));
cases.push(ok('x > 3 ? "big" : "small"', 'small', { x: 2n }));
cases.push(err('1 ? 2 : 3', 'TYPE_ERROR', 2));
cases.push(ok('true ? 1 : missing', 1n));
cases.push(ok('false ? missing : 2', 2n));
cases.push(ok('false ? 1 : true ? 2 : 3', 2n));
cases.push(ok('true ? false ? 1 : 2 : 3', 2n));
cases.push(ok('true ? 1 : 2 ? 3 : 4', 1n));
cases.push(err('true ? 1', 'PARSE_ERROR', 8));
cases.push(err('1 ? 2 :', 'PARSE_ERROR', 7));
cases.push(ok('x ? 1 : 2', 1n, { x: true }));

// --- arrays ----------------------------------------------------------------
cases.push(ok('[]', []));
cases.push(ok('[1, 2, len("abc")]', [1n, 2n, 3n]));
cases.push(ok('[1, [2, [3]]]', [1n, [2n, [3n]]]));
cases.push(ok('[1, 2] == [1, 2]', true));
cases.push(ok('[1, 2] == [1, 3]', false));
cases.push(ok('[1] == 1', false));
cases.push(ok('[[1], [2]] != [[1], [3]]', true));
cases.push(err('[1,]', 'PARSE_ERROR', 3));
cases.push(err('[1 2]', 'PARSE_ERROR', 3));
cases.push(err('[,]', 'PARSE_ERROR', 1));
cases.push(err(']', 'PARSE_ERROR', 0));

// --- literals --------------------------------------------------------------
cases.push(ok('null == null', true));
cases.push(ok('null == false', false));
cases.push(ok('null != null', false));
cases.push(ok('true == true', true));
cases.push(ok('true != false', true));
cases.push(ok(String.raw`"a\nb"`, 'a\nb'));
cases.push(ok(String.raw`"\u00e9"`, '\u00e9'));
cases.push(ok(String.raw`"\ud83d\ude00"`, '\u{1F600}'));
cases.push(ok(String.raw`"a\/b"`, 'a/b'));
cases.push(ok(String.raw`"\"q\""`, '"q"'));
cases.push(ok(String.raw`"\\"`, '\\'));
cases.push(ok(String.raw`"\b\f\r\t"`, '\b\f\r\t'));
cases.push(err(String.raw`"bad\q"`, 'PARSE_ERROR', 0));
cases.push(err('"unterminated', 'PARSE_ERROR', 0));
cases.push(err(String.raw`"\u12"`, 'PARSE_ERROR', 0));
cases.push(ok('true', true));
cases.push(ok('false', false));
cases.push(ok('null', null));
cases.push(err('true(1)', 'PARSE_ERROR', 4));

// --- arithmetic edges ------------------------------------------------------
cases.push(err('9223372036854775807 + 1', 'OVERFLOW', 20));
cases.push(err('-9223372036854775807 - 2', 'OVERFLOW', 21));
cases.push(err('9223372036854775808', 'OVERFLOW', 0));
cases.push(err('-9223372036854775808', 'OVERFLOW', 1));
cases.push(err('9999999999999999999999999999', 'OVERFLOW', 0));
cases.push(err('m / -1', 'OVERFLOW', 2, { m: INT64_MIN }));
cases.push(err('m % -1', 'OVERFLOW', 2, { m: INT64_MIN }));
cases.push(ok('m', INT64_MIN, { m: INT64_MIN }));
cases.push(ok('m + 1', INT64_MIN + 1n, { m: INT64_MIN }));
cases.push(ok('M', INT64_MAX, { M: INT64_MAX }));
cases.push(err('-m', 'OVERFLOW', 0, { m: INT64_MIN }));
cases.push(err('1 / 0', 'DIVISION_BY_ZERO', 2));
cases.push(err('1 % 0', 'DIVISION_BY_ZERO', 2));
cases.push(err('0 / 0', 'DIVISION_BY_ZERO', 2));
cases.push(err('1 + true', 'TYPE_ERROR', 2));
cases.push(err('1 + "a"', 'TYPE_ERROR', 2));
cases.push(err('"a" + "b"', 'TYPE_ERROR', 4));
cases.push(err('1 * null', 'TYPE_ERROR', 2));
cases.push(err('-"a"', 'TYPE_ERROR', 0));
cases.push(err('!1', 'TYPE_ERROR', 0));
cases.push(err('"a" < 1', 'TYPE_ERROR', 4));
cases.push(err('1 < "a"', 'TYPE_ERROR', 2));
cases.push(err('true < false', 'TYPE_ERROR', 5));
cases.push(err('[1] < [2]', 'TYPE_ERROR', 4));
cases.push(ok('"a" < "b"', true));
cases.push(ok('"b" <= "a"', false));
cases.push(ok('"Z" < "a"', true));
cases.push(ok('"abc" > "abd"', false));
cases.push(ok('"abc" >= "abc"', true));
cases.push(ok('1 < 2 == true', true));
cases.push(ok('1 <= 1', true));
cases.push(ok('2 > 1', true));
cases.push(ok('2 >= 3', false));
cases.push(err('1 == 1.5', 'PARSE_ERROR', 6));

// --- functions: len / sum / min / max / abs / neg --------------------------
cases.push(ok('len("abc")', 3n));
cases.push(ok('len("h\u00e9llo")', 5n));
cases.push(ok('len("\u{1F600}a")', 2n));
cases.push(ok('len([1, 2, 3])', 3n));
cases.push(ok('len([])', 0n));
cases.push(err('len(1)', 'TYPE_ERROR', 0));
cases.push(err('len(true)', 'TYPE_ERROR', 0));
cases.push(err('len()', 'ARITY_ERROR', 0));
cases.push(err('len("a", "b")', 'ARITY_ERROR', 0));
cases.push(ok('sum([1, 2, 3])', 6n));
cases.push(ok('sum([])', 0n));
cases.push(ok('sum([-5, 5])', 0n));
cases.push(err('sum([9223372036854775807, 1])', 'OVERFLOW', 0));
cases.push(err('sum([1, "a"])', 'TYPE_ERROR', 0));
cases.push(err('sum(1)', 'TYPE_ERROR', 0));
cases.push(ok('min([3, 1, 2])', 1n));
cases.push(ok('max([3, 1, 2])', 3n));
cases.push(ok('min([-2, -5])', -5n));
cases.push(err('min([])', 'EMPTY_ARRAY', 0));
cases.push(err('max([])', 'EMPTY_ARRAY', 0));
cases.push(err('min([1, "a"])', 'TYPE_ERROR', 0));
cases.push(ok('abs(-5)', 5n));
cases.push(ok('abs(5)', 5n));
cases.push(ok('abs(0)', 0n));
cases.push(err('abs(m)', 'OVERFLOW', 0, { m: INT64_MIN }));
cases.push(err('abs("x")', 'TYPE_ERROR', 0));
cases.push(ok('neg(5)', -5n));
cases.push(ok('neg(-5)', 5n));
cases.push(err('neg(m)', 'OVERFLOW', 0, { m: INT64_MIN }));
cases.push(ok('abs(neg(7))', 7n));

// --- functions: strings -----------------------------------------------------
cases.push(ok('upper("aBc\u00e9")', 'ABC\u00e9'));
cases.push(ok('lower("AbC\u00c9")', 'abc\u00c9'));
cases.push(ok('trim("  hi \\t\\n")', 'hi'));
cases.push(ok(String.raw`trim("\u000b\u000c x \r")`, 'x'));
cases.push(ok('trim("   ")', ''));
cases.push(ok('contains("hello", "ell")', true));
cases.push(ok('contains("hello", "z")', false));
cases.push(ok('contains("hello", "")', true));
cases.push(ok('starts_with("hello", "he")', true));
cases.push(ok('starts_with("hello", "lo")', false));
cases.push(ok('ends_with("hello", "lo")', true));
cases.push(ok('ends_with("hello", "he")', false));
cases.push(ok('substr("abcdef", 1, 3)', 'bcd'));
cases.push(ok('substr("abcdef", 0, 0)', ''));
cases.push(ok('substr("abcdef", -2, 5)', 'ef'));
cases.push(ok('substr("abcdef", 2, 100)', 'cdef'));
cases.push(ok('substr("abcdef", 10, 1)', ''));
cases.push(ok('substr("abcdef", -100, 2)', 'ab'));
cases.push(ok('substr("\u{1F600}ab", 1, 1)', 'a'));
cases.push(err('substr("abcdef", 1, -1)', 'TYPE_ERROR', 0));
cases.push(err('substr("abcdef", "1", 1)', 'TYPE_ERROR', 0));
cases.push(err('substr("abcdef", 1, 1, 1)', 'ARITY_ERROR', 0));
cases.push(ok('index_of("abcabc", "bc")', 1n));
cases.push(ok('index_of("abc", "")', 0n));
cases.push(ok('index_of("abc", "z")', -1n));
cases.push(ok('index_of("\u{1F600}ab", "a")', 1n));
cases.push(ok('index_of("", "")', 0n));
cases.push(ok('join(["a", "b"], "-")', 'a-b'));
cases.push(ok('join([], "-")', ''));
cases.push(ok('join(["a"], "")', 'a'));
cases.push(err('join([1], "-")', 'TYPE_ERROR', 0));
cases.push(err('join(["a"], 1)', 'TYPE_ERROR', 0));
cases.push(err('join("a", "-")', 'TYPE_ERROR', 0));
cases.push(ok('split("a,b,c", ",")', ['a', 'b', 'c']));
cases.push(ok('split("abc", "")', ['a', 'b', 'c']));
cases.push(ok('split("\u{1F600}b", "")', ['\u{1F600}', 'b']));
cases.push(ok('split("abc", ",")', ['abc']));
cases.push(ok('split("", ",")', ['']));
cases.push(ok('split("a--b", "--")', ['a', 'b']));
cases.push(ok('split(",,", ",")', ['', '', '']));

// --- functions: type / to_string -------------------------------------------
cases.push(ok('type(1)', 'integer'));
cases.push(ok('type(true)', 'boolean'));
cases.push(ok('type("s")', 'string'));
cases.push(ok('type([1])', 'array'));
cases.push(ok('type(null)', 'null'));
cases.push(ok('type(1 + 2)', 'integer'));
cases.push(ok('type(type(1))', 'string'));
cases.push(err('type()', 'ARITY_ERROR', 0));
cases.push(ok('to_string(42)', '42'));
cases.push(ok('to_string(-42)', '-42'));
cases.push(ok('to_string(m)', '-9223372036854775808', { m: INT64_MIN }));
cases.push(ok('to_string(true)', 'true'));
cases.push(ok('to_string(false)', 'false'));
cases.push(ok('to_string(null)', 'null'));
cases.push(ok('to_string("x")', 'x'));
cases.push(ok('to_string([])', '[]'));
cases.push(ok('to_string([1, "a", null])', '[1,"a",null]'));
cases.push(ok('to_string([[1], [2, 3]])', '[[1],[2,3]]'));
cases.push(ok(String.raw`to_string(["a\nb"])`, String.raw`["a\nb"]`));
cases.push(ok('len(to_string([1, 2, 3]))', 7n));

// --- unknown names ---------------------------------------------------------
cases.push(err('nope(1)', 'UNKNOWN_FUNCTION', 0));
cases.push(err('Len("a")', 'UNKNOWN_FUNCTION', 0));
cases.push(err('y', 'UNKNOWN_VARIABLE', 0));
cases.push(err('x + y', 'UNKNOWN_VARIABLE', 4, { x: 1n }));
cases.push(err('len(y)', 'UNKNOWN_VARIABLE', 4));
cases.push(err('  y  ', 'UNKNOWN_VARIABLE', 2));
cases.push(err('x', 'UNKNOWN_VARIABLE', 0, { other: 1n }));

// --- variables -------------------------------------------------------------
cases.push(ok('x * 2', 42n, { x: 21n }));
cases.push(ok('x', [1n, 'a', null], { x: [1n, 'a', null] }));
cases.push(ok('x == [1, 2]', true, { x: [1n, 2n] }));
cases.push(ok('x == y', true, { x: [1n, [2n]], y: [1n, [2n]] }));
cases.push(ok('len(x)', 2n, { x: 'ab' }));
cases.push(ok('x_max == 9223372036854775807', true, { x_max: INT64_MAX }));
cases.push(ok('x_min + 1 == -9223372036854775807', true, { x_min: INT64_MIN }));

// --- parse errors ----------------------------------------------------------
cases.push(err('1 +', 'PARSE_ERROR', 3));
cases.push(err('1 +   ', 'PARSE_ERROR', 6));
cases.push(err('(1', 'PARSE_ERROR', 2));
cases.push(err('1)', 'PARSE_ERROR', 1));
cases.push(err('1 @ 2', 'PARSE_ERROR', 2));
cases.push(err('1 = 2', 'PARSE_ERROR', 2));
cases.push(err('1 & 2', 'PARSE_ERROR', 2));
cases.push(err('', 'PARSE_ERROR', 0));
cases.push(err('   ', 'PARSE_ERROR', 3));
cases.push(err('1 2', 'PARSE_ERROR', 2));
cases.push(err('foo(1,', 'PARSE_ERROR', 6));
cases.push(err('foo(,1)', 'PARSE_ERROR', 4));
cases.push(err('()', 'PARSE_ERROR', 1));
cases.push(err(') ', 'PARSE_ERROR', 0));
cases.push(err('"a" "b"', 'PARSE_ERROR', 4));
cases.push(err('* 1', 'PARSE_ERROR', 0));
cases.push(err('1 * * 2', 'PARSE_ERROR', 4));
cases.push(err('1 ? : 2', 'PARSE_ERROR', 4));
cases.push(err('len("a"', 'PARSE_ERROR', 7));
cases.push(err('len(1 2)', 'PARSE_ERROR', 6));

// --- request envelope errors ----------------------------------------------
cases.push(badRequest('missing source', '{}'));
cases.push(badRequest('source not a string', '{"source": 1}'));
cases.push(badRequest('source null', '{"source": null}'));
cases.push(badRequest('vars is an array', '{"source": "1", "vars": []}'));
cases.push(badRequest('vars is null', '{"source": "1", "vars": null}'));
cases.push(badRequest('vars is a string', '{"source": "1", "vars": "x"}'));
cases.push(badRequest('unknown field', '{"source": "1", "extra": 1}'));
cases.push(badRequest('malformed json', '{"source": "1"'));
cases.push(badRequest('not json', 'hello'));
cases.push(badRequest('null document', 'null'));
cases.push(badRequest('array document', '[]'));
cases.push(badRequest('number document', '5'));
cases.push(badRequest('trailing garbage', '{"source": "1"} x'));
cases.push(badRequest('missing vars field only', '{"vars": {}}'));

// --- deliberate lexical overflow (literals are validated when parsed) ------
cases.push(err('false && 9223372036854775808', 'OVERFLOW', 9));
cases.push(err('true ? 1 : 9223372036854775808', 'OVERFLOW', 11));

// --- out-of-i64 integers in vars (kept exact, overflow on arithmetic) ------
cases.push(ok('big', 99999999999999999999n, { big: 99999999999999999999n }));
cases.push(err('big + 1', 'OVERFLOW', 4, { big: 99999999999999999999n }));

// --- unicode ordering / splitting corners ----------------------------------
cases.push(ok(String.raw`"\uFFFF" < "\ud83d\ude00"`, true));
cases.push(ok('split("", "")', []));
cases.push(ok('index_of("", "")', 0n));

// --- stdin delivery --------------------------------------------------------
cases.push(ok('2 + 2', 4n, undefined, { stdin: true }));
cases.push(err('1 / 0', 'DIVISION_BY_ZERO', 2, undefined, { stdin: true }));

module.exports = cases;
