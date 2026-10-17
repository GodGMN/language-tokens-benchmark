#!/usr/bin/env node
'use strict';

/*
 * expr - a small typed expression language evaluator.
 *
 * Implements the contract in tasks/expr/SPEC.md:
 *   expr eval <request.json>
 *   expr eval -        (request from stdin)
 *
 * Values: null, boolean, i64 integer (BigInt internally), string,
 * array of values.  Numbers from the JSON request that are not integers
 * are kept as JS floats and are never valid in arithmetic.
 */

const fs = require('fs');

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

const I64_MIN = -(1n << 63n);
const I64_MAX = (1n << 63n) - 1n;
// Nesting beyond this would exhaust the JS call stack; report a parse
// error instead of an internal error.  400 levels are well within the stack.
const MAX_PARSE_DEPTH = 400;

class ExprError extends Error {
  constructor(code, position, message) {
    super(message === undefined ? code : message);
    this.name = 'ExprError';
    this.code = code;
    this.position = position === undefined ? null : position;
  }
}

// ---------------------------------------------------------------------------
// Minimal, exact JSON reader/writer.
//
// JSON.parse() would round i64 values to doubles, which would break exact
// integer semantics.  This reader produces BigInt for integer literals and
// the writer emits BigInt as a plain decimal JSON number.
// ---------------------------------------------------------------------------

class JsonParseError extends Error {}

function hasOwn(obj, key) {
  return Object.prototype.hasOwnProperty.call(obj, key);
}

function parseJson(text) {
  let i = 0;
  const n = text.length;

  function fail(msg) {
    throw new JsonParseError(msg + ' at offset ' + i);
  }

  function skipWs() {
    while (i < n) {
      const c = text.charCodeAt(i);
      if (c === 0x20 || c === 0x09 || c === 0x0a || c === 0x0d) i++;
      else break;
    }
  }

  function isDigitAt(p) {
    const c = text.charCodeAt(p);
    return c >= 0x30 && c <= 0x39;
  }

  function parseString() {
    // text[i] === '"'
    i++;
    let out = '';
    for (;;) {
      if (i >= n) fail('unterminated string');
      const c = text.charCodeAt(i);
      if (c === 0x22) {
        i++;
        return out;
      }
      if (c < 0x20) fail('control character in string');
      if (c === 0x5c) {
        i++;
        if (i >= n) fail('unterminated escape');
        const e = text[i];
        i++;
        switch (e) {
          case '"': out += '"'; break;
          case '\\': out += '\\'; break;
          case '/': out += '/'; break;
          case 'b': out += '\b'; break;
          case 'f': out += '\f'; break;
          case 'n': out += '\n'; break;
          case 'r': out += '\r'; break;
          case 't': out += '\t'; break;
          case 'u': {
            if (i + 4 > n) fail('bad unicode escape');
            const hex = text.slice(i, i + 4);
            if (!/^[0-9A-Fa-f]{4}$/.test(hex)) fail('bad unicode escape');
            out += String.fromCharCode(parseInt(hex, 16));
            i += 4;
            break;
          }
          default:
            fail('bad escape');
        }
      } else {
        out += text[i];
        i++;
      }
    }
  }

  function parseNumber() {
    const start = i;
    if (text[i] === '-') i++;
    if (!isDigitAt(i)) fail('bad number');
    if (text[i] === '0') {
      i++;
    } else {
      while (i < n && isDigitAt(i)) i++;
    }
    let isInteger = true;
    if (i < n && text[i] === '.') {
      isInteger = false;
      i++;
      if (!isDigitAt(i)) fail('bad number');
      while (i < n && isDigitAt(i)) i++;
    }
    if (i < n && (text[i] === 'e' || text[i] === 'E')) {
      isInteger = false;
      i++;
      if (i < n && (text[i] === '+' || text[i] === '-')) i++;
      if (!isDigitAt(i)) fail('bad number');
      while (i < n && isDigitAt(i)) i++;
    }
    const raw = text.slice(start, i);
    if (isInteger) {
      // Keep every integer exact.  Values outside i64 are still BigInt; they
      // only trigger OVERFLOW once arithmetic is applied to them.
      return BigInt(raw);
    }
    return Number(raw);
  }

  function parseArray(depth) {
    if (depth > 1000) fail('nesting too deep');
    i++; // '['
    const out = [];
    skipWs();
    if (i < n && text[i] === ']') {
      i++;
      return out;
    }
    for (;;) {
      out.push(parseValue(depth + 1));
      skipWs();
      if (i >= n) fail('unterminated array');
      if (text[i] === ',') {
        i++;
        continue;
      }
      if (text[i] === ']') {
        i++;
        return out;
      }
      fail('expected "," or "]"');
    }
  }

  function parseObject(depth) {
    if (depth > 1000) fail('nesting too deep');
    i++; // '{'
    const out = Object.create(null);
    skipWs();
    if (i < n && text[i] === '}') {
      i++;
      return out;
    }
    for (;;) {
      skipWs();
      if (i >= n || text[i] !== '"') fail('expected object key');
      const key = parseString();
      skipWs();
      if (i >= n || text[i] !== ':') fail('expected ":"');
      i++;
      const value = parseValue(depth + 1);
      out[key] = value;
      skipWs();
      if (i >= n) fail('unterminated object');
      if (text[i] === ',') {
        i++;
        continue;
      }
      if (text[i] === '}') {
        i++;
        return out;
      }
      fail('expected "," or "}"');
    }
  }

  function parseValue(depth) {
    if (depth > 1000) fail('nesting too deep');
    skipWs();
    if (i >= n) fail('unexpected end of input');
    const c = text[i];
    if (c === '"') return parseString();
    if (c === '{') return parseObject(depth);
    if (c === '[') return parseArray(depth);
    if (c === 't') {
      if (text.startsWith('true', i)) { i += 4; return true; }
      fail('bad literal');
    }
    if (c === 'f') {
      if (text.startsWith('false', i)) { i += 5; return false; }
      fail('bad literal');
    }
    if (c === 'n') {
      if (text.startsWith('null', i)) { i += 4; return null; }
      fail('bad literal');
    }
    if (c === '-' || (c >= '0' && c <= '9')) return parseNumber();
    fail('unexpected character');
  }

  const value = parseValue(0);
  skipWs();
  if (i !== n) fail('trailing content');
  return value;
}

// Compact JSON with exact i64 integers.  Used both for responses and for
// to_string() on arrays.
function jsonSerialize(value) {
  if (value === null) return 'null';
  const t = typeof value;
  if (t === 'bigint') return value.toString();
  if (t === 'boolean') return value ? 'true' : 'false';
  if (t === 'string') return JSON.stringify(value);
  if (t === 'number') {
    if (!Number.isFinite(value)) return 'null';
    return JSON.stringify(value);
  }
  if (Array.isArray(value)) {
    let out = '[';
    for (let i = 0; i < value.length; i++) {
      if (i > 0) out += ',';
      out += jsonSerialize(value[i]);
    }
    return out + ']';
  }
  if (t === 'object') {
    const keys = Object.keys(value);
    let out = '{';
    for (let i = 0; i < keys.length; i++) {
      if (i > 0) out += ',';
      out += JSON.stringify(keys[i]) + ':' + jsonSerialize(value[keys[i]]);
    }
    return out + '}';
  }
  return 'null';
}

// ---------------------------------------------------------------------------
// Lexer for expressions (lazy: tokens are produced as the parser asks).
// ---------------------------------------------------------------------------

const TWO_CHAR_OPS = new Set(['||', '&&', '==', '!=', '<=', '>=']);
const ONE_CHAR_OPS = new Set([
  '<', '>', '+', '-', '*', '/', '%', '!', '?', ':', '(', ')', '[', ']', ',',
]);

function isDigit(ch) {
  return ch >= '0' && ch <= '9';
}

function isIdentStart(ch) {
  return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch === '_';
}

function isIdentPart(ch) {
  return isIdentStart(ch) || isDigit(ch);
}

function makeLexer(src) {
  let pos = 0;
  const n = src.length;

  function skipWs() {
    while (pos < n) {
      const c = src.charCodeAt(pos);
      if (c === 0x20 || c === 0x09 || c === 0x0a || c === 0x0d) pos++;
      else break;
    }
  }

  function readString(startPos) {
    pos++; // opening quote
    let out = '';
    for (;;) {
      if (pos >= n) {
        throw new ExprError('PARSE_ERROR', startPos, 'unterminated string literal');
      }
      const c = src.charCodeAt(pos);
      if (c === 0x22) {
        pos++;
        return out;
      }
      if (c < 0x20) {
        throw new ExprError('PARSE_ERROR', startPos, 'control character in string literal');
      }
      if (c === 0x5c) {
        pos++;
        if (pos >= n) {
          throw new ExprError('PARSE_ERROR', startPos, 'unterminated escape sequence');
        }
        const e = src[pos];
        pos++;
        switch (e) {
          case '"': out += '"'; break;
          case '\\': out += '\\'; break;
          case '/': out += '/'; break;
          case 'b': out += '\b'; break;
          case 'f': out += '\f'; break;
          case 'n': out += '\n'; break;
          case 'r': out += '\r'; break;
          case 't': out += '\t'; break;
          case 'u': {
            if (pos + 4 > n) {
              throw new ExprError('PARSE_ERROR', startPos, 'incomplete unicode escape');
            }
            const hex = src.slice(pos, pos + 4);
            if (!/^[0-9A-Fa-f]{4}$/.test(hex)) {
              throw new ExprError('PARSE_ERROR', startPos, 'invalid unicode escape');
            }
            const unit = parseInt(hex, 16);
            pos += 4;
            if (
              unit >= 0xd800 && unit <= 0xdbff &&
              pos + 6 <= n && src[pos] === '\\' && src[pos + 1] === 'u'
            ) {
              const hex2 = src.slice(pos + 2, pos + 6);
              if (/^[0-9A-Fa-f]{4}$/.test(hex2)) {
                const low = parseInt(hex2, 16);
                if (low >= 0xdc00 && low <= 0xdfff) {
                  pos += 6;
                  out += String.fromCodePoint(
                    0x10000 + ((unit - 0xd800) << 10) + (low - 0xdc00)
                  );
                  break;
                }
              }
            }
            out += String.fromCharCode(unit);
            break;
          }
          default:
            throw new ExprError('PARSE_ERROR', startPos, 'invalid escape sequence');
        }
      } else {
        out += src[pos];
        pos++;
      }
    }
  }

  return {
    next() {
      skipWs();
      if (pos >= n) return { type: 'eof', pos: n };
      const start = pos;
      const ch = src[pos];
      if (isDigit(ch)) {
        while (pos < n && isDigit(src[pos])) pos++;
        const raw = src.slice(start, pos);
        let value;
        try {
          value = BigInt(raw);
        } catch (e) {
          throw new ExprError('OVERFLOW', start, 'integer literal out of range');
        }
        if (value < I64_MIN || value > I64_MAX) {
          throw new ExprError('OVERFLOW', start, 'integer literal out of range');
        }
        return { type: 'int', value, pos: start };
      }
      if (ch === '"') {
        const value = readString(start);
        return { type: 'str', value, pos: start };
      }
      if (isIdentStart(ch)) {
        while (pos < n && isIdentPart(src[pos])) pos++;
        return { type: 'ident', text: src.slice(start, pos), pos: start };
      }
      if (pos + 1 < n) {
        const two = src.slice(pos, pos + 2);
        if (TWO_CHAR_OPS.has(two)) {
          pos += 2;
          return { type: 'op', text: two, pos: start };
        }
      }
      if (ONE_CHAR_OPS.has(ch)) {
        pos++;
        return { type: 'op', text: ch, pos: start };
      }
      throw new ExprError('PARSE_ERROR', start, 'unexpected character');
    },
  };
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

class Parser {
  constructor(src) {
    this.lexer = makeLexer(src);
    this.tok = null;
    this.depth = 0;
  }

  peek() {
    if (this.tok === null) this.tok = this.lexer.next();
    return this.tok;
  }

  advance() {
    const t = this.peek();
    this.tok = null;
    return t;
  }

  isOp(text) {
    const t = this.peek();
    return t.type === 'op' && t.text === text;
  }

  enter() {
    this.depth++;
    if (this.depth > MAX_PARSE_DEPTH) {
      const t = this.peek();
      throw new ExprError('PARSE_ERROR', t.pos, 'expression nesting too deep');
    }
  }

  leave() {
    this.depth--;
  }

  parseProgram() {
    const node = this.parseExpr();
    const t = this.peek();
    if (t.type !== 'eof') {
      throw new ExprError('PARSE_ERROR', t.pos, 'unexpected token');
    }
    return node;
  }

  parseExpr() {
    // Depth is tracked in parseUnary(), which every nesting level passes
    // through, so that unary chains are bounded as well.
    return this.parseTernary();
  }

  parseTernary() {
    const cond = this.parseOr();
    if (this.isOp('?')) {
      const q = this.advance();
      const thenBranch = this.parseExpr();
      if (!this.isOp(':')) {
        const t = this.peek();
        throw new ExprError('PARSE_ERROR', t.pos, 'expected ":"');
      }
      this.advance();
      const elseBranch = this.parseExpr();
      return { kind: 'ternary', cond, then: thenBranch, els: elseBranch, pos: q.pos };
    }
    return cond;
  }

  makeLeftAssoc(nextFn, ops) {
    let left = nextFn.call(this);
    for (;;) {
      const t = this.peek();
      if (t.type === 'op' && ops.has(t.text)) {
        this.advance();
        const right = nextFn.call(this);
        left = { kind: 'binary', op: t.text, left, right, pos: t.pos };
      } else {
        return left;
      }
    }
  }

  parseOr() {
    return this.makeLeftAssoc(this.parseAnd, OR_OPS);
  }

  parseAnd() {
    return this.makeLeftAssoc(this.parseEquality, AND_OPS);
  }

  parseEquality() {
    return this.makeLeftAssoc(this.parseComparison, EQUALITY_OPS);
  }

  parseComparison() {
    return this.makeLeftAssoc(this.parseAdditive, COMPARISON_OPS);
  }

  parseAdditive() {
    return this.makeLeftAssoc(this.parseMultiplicative, ADDITIVE_OPS);
  }

  parseMultiplicative() {
    return this.makeLeftAssoc(this.parseUnary, MULTIPLICATIVE_OPS);
  }

  parseUnary() {
    this.enter();
    try {
      const t = this.peek();
      if (t.type === 'op' && (t.text === '-' || t.text === '!')) {
        this.advance();
        const operand = this.parseUnary();
        return { kind: 'unary', op: t.text, operand, pos: t.pos };
      }
      return this.parsePrimary();
    } finally {
      this.leave();
    }
  }

  expectClose(text) {
    if (!this.isOp(text)) {
      const t = this.peek();
      throw new ExprError('PARSE_ERROR', t.pos, 'expected "' + text + '"');
    }
    this.advance();
  }

  parsePrimary() {
    const t = this.peek();
    if (t.type === 'int') {
      this.advance();
      return { kind: 'int', value: t.value };
    }
    if (t.type === 'str') {
      this.advance();
      return { kind: 'str', value: t.value };
    }
    if (t.type === 'ident') {
      this.advance();
      if (t.text === 'true') return { kind: 'bool', value: true };
      if (t.text === 'false') return { kind: 'bool', value: false };
      if (t.text === 'null') return { kind: 'null' };
      if (this.isOp('(')) {
        this.advance();
        const args = [];
        if (!this.isOp(')')) {
          for (;;) {
            args.push(this.parseExpr());
            if (this.isOp(',')) {
              this.advance();
              continue;
            }
            break;
          }
        }
        this.expectClose(')');
        return { kind: 'call', name: t.text, args, pos: t.pos };
      }
      return { kind: 'var', name: t.text, pos: t.pos };
    }
    if (t.type === 'op' && t.text === '(') {
      this.advance();
      const inner = this.parseExpr();
      this.expectClose(')');
      return inner;
    }
    if (t.type === 'op' && t.text === '[') {
      this.advance();
      const elems = [];
      if (!this.isOp(']')) {
        for (;;) {
          elems.push(this.parseExpr());
          if (this.isOp(',')) {
            this.advance();
            continue;
          }
          break;
        }
      }
      this.expectClose(']');
      return { kind: 'array', elems };
    }
    throw new ExprError('PARSE_ERROR', t.pos, 'unexpected token');
  }
}

const OR_OPS = new Set(['||']);
const AND_OPS = new Set(['&&']);
const EQUALITY_OPS = new Set(['==', '!=']);
const COMPARISON_OPS = new Set(['<', '<=', '>', '>=']);
const ADDITIVE_OPS = new Set(['+', '-']);
const MULTIPLICATIVE_OPS = new Set(['*', '/', '%']);

function parseSource(src) {
  return new Parser(src).parseProgram();
}

// ---------------------------------------------------------------------------
// Value helpers
// ---------------------------------------------------------------------------

function typeName(v) {
  if (v === null) return 'null';
  const t = typeof v;
  if (t === 'bigint') return 'integer';
  if (t === 'boolean') return 'boolean';
  if (t === 'string') return 'string';
  if (t === 'number') return 'number';
  if (t === 'object') return Array.isArray(v) ? 'array' : 'object';
  return 'unknown';
}

function codePointLength(s) {
  let n = 0;
  for (const _ch of s) n++;
  return n;
}

function codePoints(s) {
  const out = [];
  for (const ch of s) out.push(ch);
  return out;
}

// Compare two strings by Unicode code point, which is the same order as a
// byte-wise comparison of their UTF-8 encodings.
function compareStrings(a, b) {
  const ia = a[Symbol.iterator]();
  const ib = b[Symbol.iterator]();
  for (;;) {
    const x = ia.next();
    const y = ib.next();
    if (x.done && y.done) return 0;
    if (x.done) return -1;
    if (y.done) return 1;
    const cx = x.value.codePointAt(0);
    const cy = y.value.codePointAt(0);
    if (cx !== cy) return cx < cy ? -1 : 1;
  }
}

function valuesEqual(a, b) {
  const ta = typeName(a);
  const tb = typeName(b);
  if (ta !== tb) return false;
  switch (ta) {
    case 'null':
      return true;
    case 'integer':
    case 'number':
    case 'boolean':
    case 'string':
      return a === b;
    case 'array': {
      if (a.length !== b.length) return false;
      for (let i = 0; i < a.length; i++) {
        if (!valuesEqual(a[i], b[i])) return false;
      }
      return true;
    }
    case 'object': {
      const ka = Object.keys(a);
      const kb = Object.keys(b);
      if (ka.length !== kb.length) return false;
      for (const k of ka) {
        if (!hasOwn(b, k)) return false;
        if (!valuesEqual(a[k], b[k])) return false;
      }
      return true;
    }
    default:
      return false;
  }
}

// ---------------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------------

function checkI64(v, pos, what) {
  if (v < I64_MIN || v > I64_MAX) {
    throw new ExprError('OVERFLOW', pos, (what || 'integer') + ' overflow');
  }
  return v;
}

function evalNode(node, vars) {
  switch (node.kind) {
    case 'int':
      return node.value;
    case 'str':
      return node.value;
    case 'bool':
      return node.value;
    case 'null':
      return null;
    case 'var': {
      if (hasOwn(vars, node.name)) return vars[node.name];
      throw new ExprError('UNKNOWN_VARIABLE', node.pos, 'unknown variable: ' + node.name);
    }
    case 'array': {
      const out = [];
      for (const e of node.elems) out.push(evalNode(e, vars));
      return out;
    }
    case 'unary':
      return evalUnary(node, vars);
    case 'binary':
      return evalBinary(node, vars);
    case 'ternary': {
      const cond = evalNode(node.cond, vars);
      if (typeof cond !== 'boolean') {
        throw new ExprError('TYPE_ERROR', node.pos, 'ternary condition must be boolean');
      }
      return cond ? evalNode(node.then, vars) : evalNode(node.els, vars);
    }
    case 'call':
      return evalCall(node, vars);
    default:
      throw new Error('internal: unknown AST node ' + node.kind);
  }
}

function evalUnary(node, vars) {
  const v = evalNode(node.operand, vars);
  if (node.op === '-') {
    if (typeof v !== 'bigint') {
      throw new ExprError('TYPE_ERROR', node.pos, 'unary - expects an integer');
    }
    return checkI64(-v, node.pos, 'negation');
  }
  // '!'
  if (typeof v !== 'boolean') {
    throw new ExprError('TYPE_ERROR', node.pos, 'unary ! expects a boolean');
  }
  return !v;
}

function evalBinary(node, vars) {
  const op = node.op;
  const left = evalNode(node.left, vars);

  if (op === '&&') {
    if (typeof left !== 'boolean') {
      throw new ExprError('TYPE_ERROR', node.pos, '&& expects booleans');
    }
    if (!left) return false;
    const right = evalNode(node.right, vars);
    if (typeof right !== 'boolean') {
      throw new ExprError('TYPE_ERROR', node.pos, '&& expects booleans');
    }
    return right;
  }
  if (op === '||') {
    if (typeof left !== 'boolean') {
      throw new ExprError('TYPE_ERROR', node.pos, '|| expects booleans');
    }
    if (left) return true;
    const right = evalNode(node.right, vars);
    if (typeof right !== 'boolean') {
      throw new ExprError('TYPE_ERROR', node.pos, '|| expects booleans');
    }
    return right;
  }

  const right = evalNode(node.right, vars);

  if (op === '==') return valuesEqual(left, right);
  if (op === '!=') return !valuesEqual(left, right);

  if (op === '<' || op === '<=' || op === '>' || op === '>=') {
    let cmp;
    if (typeof left === 'bigint' && typeof right === 'bigint') {
      cmp = left < right ? -1 : left > right ? 1 : 0;
    } else if (typeof left === 'string' && typeof right === 'string') {
      cmp = compareStrings(left, right);
    } else {
      throw new ExprError('TYPE_ERROR', node.pos, 'comparison requires two integers or two strings');
    }
    if (op === '<') return cmp < 0;
    if (op === '<=') return cmp <= 0;
    if (op === '>') return cmp > 0;
    return cmp >= 0;
  }

  // Arithmetic
  if (typeof left !== 'bigint' || typeof right !== 'bigint') {
    throw new ExprError('TYPE_ERROR', node.pos, 'arithmetic requires integers');
  }
  switch (op) {
    case '+':
      return checkI64(left + right, node.pos, 'addition');
    case '-':
      return checkI64(left - right, node.pos, 'subtraction');
    case '*':
      return checkI64(left * right, node.pos, 'multiplication');
    case '/':
      if (right === 0n) {
        throw new ExprError('DIVISION_BY_ZERO', node.pos, 'division by zero');
      }
      if (left === I64_MIN && right === -1n) {
        throw new ExprError('OVERFLOW', node.pos, 'division overflow');
      }
      return left / right;
    case '%':
      if (right === 0n) {
        throw new ExprError('DIVISION_BY_ZERO', node.pos, 'modulo by zero');
      }
      if (left === I64_MIN && right === -1n) {
        throw new ExprError('OVERFLOW', node.pos, 'modulo overflow');
      }
      return left % right;
    default:
      throw new Error('internal: unknown operator ' + op);
  }
}

// --- functions -------------------------------------------------------------

function needString(v, pos) {
  if (typeof v !== 'string') {
    throw new ExprError('TYPE_ERROR', pos, 'expected a string argument');
  }
  return v;
}

function needInteger(v, pos) {
  if (typeof v !== 'bigint') {
    throw new ExprError('TYPE_ERROR', pos, 'expected an integer argument');
  }
  return v;
}

function needArray(v, pos) {
  if (!Array.isArray(v)) {
    throw new ExprError('TYPE_ERROR', pos, 'expected an array argument');
  }
  return v;
}

function needIntegerArray(v, pos) {
  const arr = needArray(v, pos);
  for (const e of arr) {
    if (typeof e !== 'bigint') {
      throw new ExprError('TYPE_ERROR', pos, 'expected an array of integers');
    }
  }
  return arr;
}

function asciiUpper(s) {
  let out = '';
  for (let i = 0; i < s.length; i++) {
    const c = s.charCodeAt(i);
    out += c >= 0x61 && c <= 0x7a ? String.fromCharCode(c - 32) : s[i];
  }
  return out;
}

function asciiLower(s) {
  let out = '';
  for (let i = 0; i < s.length; i++) {
    const c = s.charCodeAt(i);
    out += c >= 0x41 && c <= 0x5a ? String.fromCharCode(c + 32) : s[i];
  }
  return out;
}

const ASCII_WS = ' \t\n\r\f\v';

function trimAscii(s) {
  let start = 0;
  let end = s.length;
  while (start < end && ASCII_WS.indexOf(s[start]) !== -1) start++;
  while (end > start && ASCII_WS.indexOf(s[end - 1]) !== -1) end--;
  return s.slice(start, end);
}

function fnSubstr(args, pos) {
  const s = needString(args[0], pos);
  const start = needInteger(args[1], pos);
  const len = needInteger(args[2], pos);
  if (len < 0n) {
    throw new ExprError('TYPE_ERROR', pos, 'substr length must be non-negative');
  }
  const cps = codePoints(s);
  const total = BigInt(cps.length);
  let from = start;
  if (from < 0n) from = total + from;
  if (from < 0n) from = 0n;
  if (from > total) from = total;
  let to = from + len;
  if (to > total) to = total;
  return cps.slice(Number(from), Number(to)).join('');
}

function fnIndexOf(args, pos) {
  const s = needString(args[0], pos);
  const sub = needString(args[1], pos);
  if (sub === '') return 0n;
  const hay = codePoints(s);
  const needle = codePoints(sub);
  outer: for (let i = 0; i + needle.length <= hay.length; i++) {
    for (let j = 0; j < needle.length; j++) {
      if (hay[i + j] !== needle[j]) continue outer;
    }
    return BigInt(i);
  }
  return -1n;
}

function fnJoin(args, pos) {
  const arr = needArray(args[0], pos);
  const sep = needString(args[1], pos);
  const parts = [];
  for (const e of arr) {
    if (typeof e !== 'string') {
      throw new ExprError('TYPE_ERROR', pos, 'join expects an array of strings');
    }
    parts.push(e);
  }
  return parts.join(sep);
}

function fnSplit(args, pos) {
  const s = needString(args[0], pos);
  const sep = needString(args[1], pos);
  if (sep === '') return codePoints(s);
  return s.split(sep);
}

function fnToString(args, pos) {
  const v = args[0];
  const t = typeName(v);
  switch (t) {
    case 'integer':
      return v.toString();
    case 'boolean':
      return v ? 'true' : 'false';
    case 'null':
      return 'null';
    case 'string':
      return v;
    case 'array':
      return jsonSerialize(v);
    case 'number':
    case 'object':
      return jsonSerialize(v);
    default:
      throw new ExprError('TYPE_ERROR', pos, 'cannot convert value to string');
  }
}

const FUNCTIONS = Object.create(null);

FUNCTIONS.len = {
  arity: 1,
  fn(args, pos) {
    const v = args[0];
    if (typeof v === 'string') return BigInt(codePointLength(v));
    if (Array.isArray(v)) return BigInt(v.length);
    throw new ExprError('TYPE_ERROR', pos, 'len expects a string or an array');
  },
};

FUNCTIONS.sum = {
  arity: 1,
  fn(args, pos) {
    const arr = needIntegerArray(args[0], pos);
    let total = 0n;
    for (const e of arr) {
      total += e;
      if (total < I64_MIN || total > I64_MAX) {
        throw new ExprError('OVERFLOW', pos, 'sum overflow');
      }
    }
    return total;
  },
};

FUNCTIONS.min = {
  arity: 1,
  fn(args, pos) {
    const arr = needIntegerArray(args[0], pos);
    if (arr.length === 0) {
      throw new ExprError('EMPTY_ARRAY', pos, 'min of an empty array');
    }
    let best = arr[0];
    for (let i = 1; i < arr.length; i++) {
      if (arr[i] < best) best = arr[i];
    }
    return best;
  },
};

FUNCTIONS.max = {
  arity: 1,
  fn(args, pos) {
    const arr = needIntegerArray(args[0], pos);
    if (arr.length === 0) {
      throw new ExprError('EMPTY_ARRAY', pos, 'max of an empty array');
    }
    let best = arr[0];
    for (let i = 1; i < arr.length; i++) {
      if (arr[i] > best) best = arr[i];
    }
    return best;
  },
};

FUNCTIONS.abs = {
  arity: 1,
  fn(args, pos) {
    const v = needInteger(args[0], pos);
    if (v === I64_MIN) {
      throw new ExprError('OVERFLOW', pos, 'abs overflow');
    }
    return checkI64(v < 0n ? -v : v, pos, 'abs');
  },
};

FUNCTIONS.neg = {
  arity: 1,
  fn(args, pos) {
    const v = needInteger(args[0], pos);
    if (v === I64_MIN) {
      throw new ExprError('OVERFLOW', pos, 'neg overflow');
    }
    return checkI64(-v, pos, 'neg');
  },
};

FUNCTIONS.upper = {
  arity: 1,
  fn(args, pos) {
    return asciiUpper(needString(args[0], pos));
  },
};

FUNCTIONS.lower = {
  arity: 1,
  fn(args, pos) {
    return asciiLower(needString(args[0], pos));
  },
};

FUNCTIONS.trim = {
  arity: 1,
  fn(args, pos) {
    return trimAscii(needString(args[0], pos));
  },
};

FUNCTIONS.contains = {
  arity: 2,
  fn(args, pos) {
    const s = needString(args[0], pos);
    const sub = needString(args[1], pos);
    return s.indexOf(sub) !== -1;
  },
};

FUNCTIONS.starts_with = {
  arity: 2,
  fn(args, pos) {
    const s = needString(args[0], pos);
    const p = needString(args[1], pos);
    return s.startsWith(p);
  },
};

FUNCTIONS.ends_with = {
  arity: 2,
  fn(args, pos) {
    const s = needString(args[0], pos);
    const p = needString(args[1], pos);
    return s.endsWith(p);
  },
};

FUNCTIONS.substr = { arity: 3, fn: fnSubstr };

FUNCTIONS.index_of = { arity: 2, fn: fnIndexOf };

FUNCTIONS.join = { arity: 2, fn: fnJoin };

FUNCTIONS.split = { arity: 2, fn: fnSplit };

FUNCTIONS.type = {
  arity: 1,
  fn(args) {
    return typeName(args[0]);
  },
};

FUNCTIONS.to_string = { arity: 1, fn: fnToString };

function evalCall(node, vars) {
  const spec = hasOwn(FUNCTIONS, node.name) ? FUNCTIONS[node.name] : null;
  if (spec === null) {
    throw new ExprError('UNKNOWN_FUNCTION', node.pos, 'unknown function: ' + node.name);
  }
  if (node.args.length !== spec.arity) {
    throw new ExprError(
      'ARITY_ERROR',
      node.pos,
      node.name + ' expects ' + spec.arity + ' argument(s), got ' + node.args.length
    );
  }
  const args = [];
  for (const a of node.args) args.push(evalNode(a, vars));
  return spec.fn(args, node.pos);
}

// ---------------------------------------------------------------------------
// Request handling / CLI
// ---------------------------------------------------------------------------

function invalidRequest(msg) {
  return new ExprError('INVALID_REQUEST', null, msg);
}

function validateRequest(req) {
  if (req === null || typeof req !== 'object' || Array.isArray(req)) {
    throw invalidRequest('request must be a JSON object');
  }
  for (const key of Object.keys(req)) {
    if (key !== 'source' && key !== 'vars') {
      throw invalidRequest('unknown request field: ' + key);
    }
  }
  if (!hasOwn(req, 'source') || typeof req.source !== 'string') {
    throw invalidRequest('"source" must be a string');
  }
  let vars = Object.create(null);
  if (hasOwn(req, 'vars')) {
    const v = req.vars;
    if (v === null || typeof v !== 'object' || Array.isArray(v)) {
      throw invalidRequest('"vars" must be an object');
    }
    vars = v;
  }
  return { source: req.source, vars };
}

function readRequest(argv) {
  if (argv.length !== 2 || argv[0] !== 'eval') {
    throw invalidRequest('usage: expr eval <request.json | ->');
  }
  const target = argv[1];
  let text;
  try {
    if (target === '-') {
      text = fs.readFileSync(0, 'utf8');
    } else {
      text = fs.readFileSync(target, 'utf8');
    }
  } catch (e) {
    throw invalidRequest('cannot read request: ' + e.message);
  }
  let req;
  try {
    req = parseJson(text);
  } catch (e) {
    if (e instanceof JsonParseError) {
      throw invalidRequest('malformed JSON request: ' + e.message);
    }
    throw e;
  }
  return validateRequest(req);
}

function errorResponse(code, position, message) {
  return (
    '{"status":"error","code":' + JSON.stringify(code) +
    ',"position":' + (position === null ? 'null' : String(position)) +
    ',"message":' + JSON.stringify(message === undefined ? code : message) +
    '}'
  );
}

function evaluateRequest(argv) {
  const { source, vars } = readRequest(argv);
  const ast = parseSource(source);
  const value = evalNode(ast, vars);
  return value;
}

function main() {
  const argv = process.argv.slice(2);
  let output;
  let exitCode;
  try {
    const value = evaluateRequest(argv);
    output = '{"status":"ok","value":' + jsonSerialize(value) + '}';
    exitCode = 0;
  } catch (e) {
    if (e instanceof ExprError) {
      output = errorResponse(e.code, e.position, e.message);
      exitCode = e.code === 'INVALID_REQUEST' ? 2 : 1;
    } else {
      try {
        process.stderr.write('internal error: ' + (e && e.stack ? e.stack : String(e)) + '\n');
      } catch (_ignored) { /* stderr may be closed */ }
      output = errorResponse('INTERNAL_ERROR', null, 'internal error');
      exitCode = 3;
    }
  }
  process.stdout.write(output + '\n');
  process.exitCode = exitCode;
}

if (require.main === module) {
  main();
}

module.exports = {
  ExprError,
  parseJson,
  jsonSerialize,
  parseSource,
  evalNode,
  typeName,
};
