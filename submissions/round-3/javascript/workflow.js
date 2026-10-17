#!/usr/bin/env node
'use strict';

/*
 * Declarative workflow engine CLI.
 *
 *   workflow run <workflow.json>
 *   workflow run -            (JSON document on stdin)
 *
 * stdout: exactly one JSON document. stderr: diagnostics.
 * Exit codes: 0 completed, 1 runtime failure, 2 validation/input error, 3 internal.
 *
 * No third-party dependencies: only the Node.js standard library is used.
 */

const fs = require('fs');

/* ------------------------------------------------------------------ */
/* Numeric model                                                       */
/* ------------------------------------------------------------------ */

const I64_MIN = -(1n << 63n);
const I64_MAX = (1n << 63n) - 1n;

/*
 * Parsed numbers keep their original JSON literal so that a numeric
 * parameter written as `1.0` or `1e3` can be rejected with INVALID_ARG
 * (a plain JSON.parse would lose that information), and so integer
 * literals beyond 2^53 keep full precision.
 *
 * Values produced by operations are plain BigInt (always i64-range).
 */
class Num {
  constructor(raw) {
    this.raw = raw;
    this.isInt = /^-?[0-9]+$/.test(raw);
    this.num = Number(raw);
    this.big = this.isInt ? BigInt(raw) : null;
  }
}

function toI64(v) {
  if (typeof v === 'bigint') {
    return v >= I64_MIN && v <= I64_MAX ? v : null;
  }
  if (v instanceof Num) {
    if (!v.isInt) return null;
    if (v.big < I64_MIN || v.big > I64_MAX) return null;
    return v.big;
  }
  return null;
}

function exactBig(v) {
  if (typeof v === 'bigint') return v;
  if (v instanceof Num && v.isInt) return v.big;
  return null;
}

function numValue(v) {
  return v instanceof Num ? v.num : Number(v);
}

function numCompare(a, b) {
  const ab = exactBig(a);
  const bb = exactBig(b);
  if (ab !== null && bb !== null) return ab < bb ? -1 : ab > bb ? 1 : 0;
  const an = numValue(a);
  const bn = numValue(b);
  if (an < bn) return -1;
  if (an > bn) return 1;
  return 0;
}

function checkI64(b) {
  if (b < I64_MIN || b > I64_MAX) {
    throw new WErr('ARITHMETIC_OVERFLOW', 'i64 overflow');
  }
  return b;
}

/* ------------------------------------------------------------------ */
/* Errors                                                              */
/* ------------------------------------------------------------------ */

class WErr extends Error {
  constructor(code, message, step) {
    super(message || code);
    this.name = 'WErr';
    this.code = code;
    this.step = step === undefined ? undefined : step;
  }
}

function invalidArg(message) {
  return new WErr('INVALID_ARG', message);
}

const EXIT_2_CODES = new Set([
  'INVALID_WORKFLOW',
  'UNKNOWN_DEPENDENCY',
  'CYCLIC_DEPENDENCY',
]);

function exitFor(code) {
  return EXIT_2_CODES.has(code) ? 2 : 1;
}

/* ------------------------------------------------------------------ */
/* JSON parsing (custom, preserves number literals)                    */
/* ------------------------------------------------------------------ */

const NUM_RE = /-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?/y;

function parseJson(text) {
  let i = 0;
  const n = text.length;

  function fail(msg) {
    throw new WErr('INVALID_WORKFLOW', 'JSON parse error: ' + msg);
  }

  function skipWs() {
    while (i < n) {
      const c = text.charCodeAt(i);
      if (c === 0x20 || c === 0x09 || c === 0x0a || c === 0x0d) i++;
      else break;
    }
  }

  function parseString() {
    i++; // opening quote
    let out = '';
    let start = i;
    while (i < n) {
      const c = text.charCodeAt(i);
      if (c === 0x22) {
        out += text.slice(start, i);
        i++;
        return out;
      }
      if (c === 0x5c) {
        out += text.slice(start, i);
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
            if (!/^[0-9a-fA-F]{4}$/.test(hex)) fail('bad unicode escape');
            out += String.fromCharCode(parseInt(hex, 16));
            i += 4;
            break;
          }
          default:
            fail('bad escape');
        }
        start = i;
      } else if (c < 0x20) {
        fail('control character in string');
      } else {
        i++;
      }
    }
    fail('unterminated string');
  }

  function parseNumber() {
    NUM_RE.lastIndex = i;
    const m = NUM_RE.exec(text);
    if (!m) fail('bad number');
    i = NUM_RE.lastIndex;
    return new Num(m[0]);
  }

  function parseObject() {
    i++; // {
    const obj = Object.create(null);
    skipWs();
    if (text[i] === '}') {
      i++;
      return obj;
    }
    for (;;) {
      skipWs();
      if (text[i] !== '"') fail('expected object key');
      const key = parseString();
      skipWs();
      if (text[i] !== ':') fail('expected ":"');
      i++;
      const val = parseValue();
      obj[key] = val;
      skipWs();
      if (text[i] === ',') {
        i++;
        continue;
      }
      if (text[i] === '}') {
        i++;
        return obj;
      }
      fail('expected "," or "}"');
    }
  }

  function parseArray() {
    i++; // [
    const arr = [];
    skipWs();
    if (text[i] === ']') {
      i++;
      return arr;
    }
    for (;;) {
      arr.push(parseValue());
      skipWs();
      if (text[i] === ',') {
        i++;
        continue;
      }
      if (text[i] === ']') {
        i++;
        return arr;
      }
      fail('expected "," or "]"');
    }
  }

  function parseValue() {
    skipWs();
    if (i >= n) fail('unexpected end of input');
    const c = text[i];
    if (c === '{') return parseObject();
    if (c === '[') return parseArray();
    if (c === '"') return parseString();
    if (c === '-' || (c >= '0' && c <= '9')) return parseNumber();
    if (text.startsWith('true', i)) { i += 4; return true; }
    if (text.startsWith('false', i)) { i += 5; return false; }
    if (text.startsWith('null', i)) { i += 4; return null; }
    fail('unexpected token');
  }

  const value = parseValue();
  skipWs();
  if (i !== n) fail('trailing content');
  return value;
}

/* ------------------------------------------------------------------ */
/* JSON serialization (BigInt aware)                                   */
/* ------------------------------------------------------------------ */

function serialize(v) {
  if (v === null || v === undefined) return 'null';
  const t = typeof v;
  if (t === 'boolean') return v ? 'true' : 'false';
  if (t === 'string') return JSON.stringify(v);
  if (t === 'bigint') return v.toString();
  if (t === 'number') {
    if (Number.isFinite(v)) return JSON.stringify(v);
    return 'null';
  }
  if (v instanceof Num) return v.raw;
  if (Array.isArray(v)) {
    return '[' + v.map(serialize).join(',') + ']';
  }
  if (isPlainObject(v)) {
    const keys = Object.keys(v);
    return '{' + keys.map((k) => JSON.stringify(k) + ':' + serialize(v[k])).join(',') + '}';
  }
  return 'null';
}

/* ------------------------------------------------------------------ */
/* Value helpers                                                       */
/* ------------------------------------------------------------------ */

function isPlainObject(v) {
  return (
    v !== null &&
    typeof v === 'object' &&
    !Array.isArray(v) &&
    !(v instanceof Num)
  );
}

function hasOwn(obj, key) {
  return Object.prototype.hasOwnProperty.call(obj, key);
}

function typeName(v) {
  if (v === null || v === undefined) return 'null';
  const t = typeof v;
  if (t === 'boolean') return 'boolean';
  if (t === 'bigint' || v instanceof Num) return 'number';
  if (t === 'string') return 'string';
  if (Array.isArray(v)) return 'array';
  return 'object';
}

function typeRank(v) {
  switch (typeName(v)) {
    case 'number': return 0;
    case 'string': return 1;
    case 'boolean': return 2;
    case 'null': return 3;
    case 'array': return 4;
    default: return 5;
  }
}

function byteCompare(a, b) {
  return Buffer.compare(Buffer.from(a, 'utf8'), Buffer.from(b, 'utf8'));
}

function valueEquals(a, b) {
  const ta = typeName(a);
  const tb = typeName(b);
  if (ta !== tb) return false;
  switch (ta) {
    case 'null':
      return true;
    case 'boolean':
      return a === b;
    case 'number': {
      const ab = exactBig(a);
      const bb = exactBig(b);
      if (ab !== null && bb !== null) return ab === bb;
      return numValue(a) === numValue(b);
    }
    case 'string':
      return a === b;
    case 'array': {
      if (a.length !== b.length) return false;
      for (let k = 0; k < a.length; k++) {
        if (!valueEquals(a[k], b[k])) return false;
      }
      return true;
    }
    default: {
      const ka = Object.keys(a);
      const kb = Object.keys(b);
      if (ka.length !== kb.length) return false;
      for (const k of ka) {
        if (!hasOwn(b, k)) return false;
        if (!valueEquals(a[k], b[k])) return false;
      }
      return true;
    }
  }
}

function totalOrder(a, b) {
  const ra = typeRank(a);
  const rb = typeRank(b);
  if (ra !== rb) return ra < rb ? -1 : 1;
  switch (ra) {
    case 0:
      return numCompare(a, b);
    case 1:
      return byteCompare(a, b);
    case 2:
      if (a === b) return 0;
      return a ? 1 : -1;
    case 3:
      return 0;
    case 4: {
      const len = Math.min(a.length, b.length);
      for (let k = 0; k < len; k++) {
        const c = totalOrder(a[k], b[k]);
        if (c !== 0) return c;
      }
      if (a.length === b.length) return 0;
      return a.length < b.length ? -1 : 1;
    }
    default: {
      const ka = Object.keys(a).sort(byteCompare);
      const kb = Object.keys(b).sort(byteCompare);
      const len = Math.min(ka.length, kb.length);
      for (let k = 0; k < len; k++) {
        const c = byteCompare(ka[k], kb[k]);
        if (c !== 0) return c;
      }
      if (ka.length !== kb.length) return ka.length < kb.length ? -1 : 1;
      for (let k = 0; k < ka.length; k++) {
        const c = totalOrder(a[ka[k]], b[ka[k]]);
        if (c !== 0) return c;
      }
      return 0;
    }
  }
}

function truthy(v) {
  if (v === null || v === undefined) return false;
  const t = typeof v;
  if (t === 'boolean') return v;
  if (t === 'string') return v.length > 0;
  if (t === 'bigint') return v !== 0n;
  if (v instanceof Num) return v.num !== 0;
  if (Array.isArray(v)) return v.length > 0;
  if (isPlainObject(v)) return Object.keys(v).length > 0;
  return false;
}

function asciiUpper(s) {
  return s.replace(/[a-z]/g, (c) =>
    String.fromCharCode(c.charCodeAt(0) - 32)
  );
}

function asciiLower(s) {
  return s.replace(/[A-Z]/g, (c) =>
    String.fromCharCode(c.charCodeAt(0) + 32)
  );
}

function argI64(args, name) {
  requireArg(args, name);
  const b = toI64(args[name]);
  if (b === null) throw invalidArg('argument "' + name + '" must be an i64 integer');
  return b;
}

function requireArg(args, name) {
  if (!hasOwn(args, name)) throw invalidArg('missing required argument "' + name + '"');
  return args[name];
}

function checkKnownArgs(args, allowed) {
  for (const k of Object.keys(args)) {
    if (!allowed.includes(k)) throw invalidArg('unknown argument "' + k + '"');
  }
}

function needString(v, what) {
  if (typeof v !== 'string') throw invalidArg(what + ' must be a string');
  return v;
}

function needArray(v, what) {
  if (!Array.isArray(v)) throw invalidArg(what + ' must be an array');
  return v;
}

function needObject(v, what) {
  if (!isPlainObject(v)) throw invalidArg(what + ' must be an object');
  return v;
}

function stringArray(v, what) {
  const arr = needArray(v, what);
  for (const x of arr) {
    if (typeof x !== 'string') throw invalidArg(what + ' must contain strings');
  }
  return arr;
}

/* ------------------------------------------------------------------ */
/* Numeric operations                                                  */
/* ------------------------------------------------------------------ */

function binaryNumeric(op, a, b) {
  const x = toI64(a);
  if (x === null) throw invalidArg('left operand must be an i64 integer');
  const y = toI64(b);
  if (y === null) throw invalidArg('right operand must be an i64 integer');
  switch (op) {
    case 'add':
      return checkI64(x + y);
    case 'subtract':
      return checkI64(x - y);
    case 'multiply':
      return checkI64(x * y);
    case 'divide':
      if (y === 0n) throw new WErr('DIVISION_BY_ZERO', 'division by zero');
      if (x === I64_MIN && y === -1n) throw new WErr('ARITHMETIC_OVERFLOW', 'i64 overflow');
      return x / y;
    case 'mod':
      if (y === 0n) throw new WErr('DIVISION_BY_ZERO', 'modulo by zero');
      if (x === I64_MIN && y === -1n) throw new WErr('ARITHMETIC_OVERFLOW', 'i64 overflow');
      return x % y;
    default:
      throw invalidArg('unsupported numeric op "' + op + '"');
  }
}

function negateI64(x) {
  const b = toI64(x);
  if (b === null) throw invalidArg('expected an i64 integer');
  return checkI64(-b);
}

function absI64(x) {
  const b = toI64(x);
  if (b === null) throw invalidArg('expected an i64 integer');
  return b < 0n ? checkI64(-b) : b;
}

/* ------------------------------------------------------------------ */
/* map.transform and filter.predicate                                  */
/* ------------------------------------------------------------------ */

const BINARY_OPS = new Set(['add', 'subtract', 'multiply', 'divide', 'mod']);

function substituteSelf(value, element) {
  if (isPlainObject(value)) {
    const keys = Object.keys(value);
    if (keys.length === 1 && keys[0] === '$self' && value.$self === true) {
      return element;
    }
    const out = Object.create(null);
    for (const k of keys) out[k] = substituteSelf(value[k], element);
    return out;
  }
  if (Array.isArray(value)) return value.map((v) => substituteSelf(v, element));
  return value;
}

function applyTransform(element, t) {
  if (!isPlainObject(t)) throw invalidArg('transform must be an object');
  if (typeof t.type !== 'string') throw invalidArg('transform.type must be a string');
  switch (t.type) {
    case 'op': {
      checkKnownArgs(t, ['type', 'op', 'args']);
      const opName = requireArg(t, 'op');
      if (typeof opName !== 'string' || !BINARY_OPS.has(opName)) {
        throw invalidArg('transform.op must be a binary numeric op');
      }
      const targs = needObject(requireArg(t, 'args'), 'transform.args');
      checkKnownArgs(targs, ['a', 'b']);
      const a = substituteSelf(requireArg(targs, 'a'), element);
      const b = substituteSelf(requireArg(targs, 'b'), element);
      return binaryNumeric(opName, a, b);
    }
    case 'constant': {
      checkKnownArgs(t, ['type', 'value']);
      return requireArg(t, 'value');
    }
    case 'add':
    case 'subtract':
    case 'multiply':
    case 'divide':
    case 'mod': {
      checkKnownArgs(t, ['type', 'value']);
      const v = requireArg(t, 'value');
      return binaryNumeric(t.type, element, v);
    }
    case 'negate': {
      checkKnownArgs(t, ['type']);
      return negateI64(element);
    }
    case 'abs': {
      checkKnownArgs(t, ['type']);
      return absI64(element);
    }
    case 'stringify': {
      checkKnownArgs(t, ['type']);
      return serialize(element);
    }
    case 'type': {
      checkKnownArgs(t, ['type']);
      return typeName(element);
    }
    default:
      throw invalidArg('unknown transform type "' + t.type + '"');
  }
}

function applyPredicate(element, p) {
  if (!isPlainObject(p)) throw invalidArg('predicate must be an object');
  if (typeof p.type !== 'string') throw invalidArg('predicate.type must be a string');
  switch (p.type) {
    case 'even':
    case 'odd': {
      checkKnownArgs(p, ['type']);
      const b = toI64(element);
      if (b === null) throw invalidArg('even/odd predicate requires an i64 element');
      const isEven = b % 2n === 0n;
      return p.type === 'even' ? isEven : !isEven;
    }
    case 'gt':
    case 'gte':
    case 'lt':
    case 'lte': {
      checkKnownArgs(p, ['type', 'value']);
      const v = requireArg(p, 'value');
      if (toI64(v) === null) throw invalidArg('predicate.value must be an i64 integer');
      const c = totalOrder(element, v);
      switch (p.type) {
        case 'gt': return c > 0;
        case 'gte': return c >= 0;
        case 'lt': return c < 0;
        default: return c <= 0;
      }
    }
    case 'eq':
    case 'ne': {
      checkKnownArgs(p, ['type', 'value']);
      const v = requireArg(p, 'value');
      const eq = valueEquals(element, v);
      return p.type === 'eq' ? eq : !eq;
    }
    case 'truthy': {
      checkKnownArgs(p, ['type']);
      return truthy(element);
    }
    case 'falsy': {
      checkKnownArgs(p, ['type']);
      return !truthy(element);
    }
    case 'type_is': {
      // The target type name is carried by whatever extra key the document
      // uses (`value`, `type_name`, `of`, ...). Accept the first string-valued
      // key other than `type` to be tolerant of the exact spelling.
      let target;
      for (const k of Object.keys(p)) {
        if (k === 'type') continue;
        if (typeof p[k] === 'string') { target = p[k]; break; }
      }
      if (target === undefined) throw invalidArg('type_is predicate requires a target type');
      return typeName(element) === target;
    }
    case 'string_contains': {
      checkKnownArgs(p, ['type', 'value']);
      const v = requireArg(p, 'value');
      if (typeof v !== 'string') throw invalidArg('string_contains value must be a string');
      if (typeof element !== 'string') {
        throw invalidArg('string_contains requires a string element');
      }
      return element.includes(v);
    }
    default:
      throw invalidArg('unknown predicate type "' + p.type + '"');
  }
}

/* ------------------------------------------------------------------ */
/* Operation dispatch                                                  */
/* ------------------------------------------------------------------ */

function pySliceBounds(len, start, end) {
  const n = BigInt(len);
  let s = start < 0n ? n + start : start;
  let e = end === undefined ? n : end < 0n ? n + end : end;
  if (s < 0n) s = 0n;
  if (s > n) s = n;
  if (e < 0n) e = 0n;
  if (e > n) e = n;
  return [Number(s), Number(e)];
}

function dispatch(op, args, stepId, failCounters) {
  switch (op) {
    /* ---------------- math ---------------- */
    case 'add':
    case 'subtract':
    case 'multiply':
    case 'divide':
    case 'mod': {
      checkKnownArgs(args, ['a', 'b']);
      return binaryNumeric(op, requireArg(args, 'a'), requireArg(args, 'b'));
    }
    case 'sum': {
      checkKnownArgs(args, ['input']);
      const input = needArray(requireArg(args, 'input'), 'input');
      let acc = 0n;
      for (const el of input) {
        const b = toI64(el);
        if (b === null) throw invalidArg('sum input must contain i64 integers');
        acc = checkI64(acc + b);
      }
      return acc;
    }
    case 'min':
    case 'max': {
      checkKnownArgs(args, ['input']);
      const input = needArray(requireArg(args, 'input'), 'input');
      if (input.length === 0) throw new WErr('EMPTY_ARRAY', op + ' of empty array');
      let best = null;
      for (const el of input) {
        const b = toI64(el);
        if (b === null) throw invalidArg(op + ' input must contain i64 integers');
        if (best === null) best = b;
        else if (op === 'min' ? b < best : b > best) best = b;
      }
      return best;
    }
    case 'range': {
      checkKnownArgs(args, ['start', 'end']);
      const start = argI64(args, 'start');
      const end = argI64(args, 'end');
      const out = [];
      if (start <= end) {
        const span = end - start;
        if (span > 10000000n) {
          throw invalidArg('range is too large');
        }
        for (let v = start; v <= end; v++) out.push(v);
      }
      return out;
    }

    /* ---------------- strings ---------------- */
    case 'concat': {
      checkKnownArgs(args, ['a', 'b']);
      const a = needString(requireArg(args, 'a'), 'a');
      const b = needString(requireArg(args, 'b'), 'b');
      return a + b;
    }
    case 'split': {
      checkKnownArgs(args, ['input', 'sep']);
      const input = needString(requireArg(args, 'input'), 'input');
      const sep = needString(requireArg(args, 'sep'), 'sep');
      if (sep === '') return Array.from(input);
      return input.split(sep);
    }
    case 'join': {
      checkKnownArgs(args, ['input', 'sep']);
      const input = stringArray(requireArg(args, 'input'), 'input');
      const sep = needString(requireArg(args, 'sep'), 'sep');
      return input.join(sep);
    }
    case 'uppercase': {
      checkKnownArgs(args, ['input']);
      return asciiUpper(needString(requireArg(args, 'input'), 'input'));
    }
    case 'lowercase': {
      checkKnownArgs(args, ['input']);
      return asciiLower(needString(requireArg(args, 'input'), 'input'));
    }
    case 'trim': {
      checkKnownArgs(args, ['input']);
      const s = needString(requireArg(args, 'input'), 'input');
      return s.replace(/^[ \t\n\r\f\v]+/, '').replace(/[ \t\n\r\f\v]+$/, '');
    }
    case 'replace': {
      checkKnownArgs(args, ['input', 'search', 'replace']);
      const input = needString(requireArg(args, 'input'), 'input');
      const search = needString(requireArg(args, 'search'), 'search');
      const repl = needString(requireArg(args, 'replace'), 'replace');
      if (search === '') throw invalidArg('search must not be empty');
      return input.split(search).join(repl);
    }
    case 'length': {
      checkKnownArgs(args, ['input']);
      const s = needString(requireArg(args, 'input'), 'input');
      return BigInt(Array.from(s).length);
    }

    /* ---------------- arrays ---------------- */
    case 'map': {
      checkKnownArgs(args, ['input', 'transform']);
      const input = needArray(requireArg(args, 'input'), 'input');
      const transform = requireArg(args, 'transform');
      return input.map((el) => applyTransform(el, transform));
    }
    case 'filter': {
      checkKnownArgs(args, ['input', 'predicate']);
      const input = needArray(requireArg(args, 'input'), 'input');
      const predicate = requireArg(args, 'predicate');
      return input.filter((el) => applyPredicate(el, predicate));
    }
    case 'reduce': {
      checkKnownArgs(args, ['input', 'op', 'initial']);
      const input = needArray(requireArg(args, 'input'), 'input');
      const opName = requireArg(args, 'op');
      if (opName !== 'add' && opName !== 'subtract' && opName !== 'multiply') {
        throw invalidArg('reduce.op must be add, subtract or multiply');
      }
      let acc;
      if (hasOwn(args, 'initial')) {
        const b = toI64(args.initial);
        if (b === null) throw invalidArg('initial must be an i64 integer');
        acc = b;
      } else {
        acc = opName === 'multiply' ? 1n : 0n;
      }
      for (const el of input) {
        const b = toI64(el);
        if (b === null) throw invalidArg('reduce input must contain i64 integers');
        acc = binaryNumeric(opName, acc, b);
      }
      return acc;
    }
    case 'sort': {
      checkKnownArgs(args, ['input']);
      const input = needArray(requireArg(args, 'input'), 'input');
      return input.slice().sort(totalOrder);
    }
    case 'reverse': {
      checkKnownArgs(args, ['input']);
      const input = needArray(requireArg(args, 'input'), 'input');
      return input.slice().reverse();
    }
    case 'unique': {
      checkKnownArgs(args, ['input']);
      const input = needArray(requireArg(args, 'input'), 'input');
      const out = [];
      for (const el of input) {
        let seen = false;
        for (const prev of out) {
          if (valueEquals(prev, el)) { seen = true; break; }
        }
        if (!seen) out.push(el);
      }
      return out;
    }
    case 'slice': {
      checkKnownArgs(args, ['input', 'start', 'end']);
      const input = needArray(requireArg(args, 'input'), 'input');
      const start = argI64(args, 'start');
      const end = hasOwn(args, 'end') ? toI64(args.end) : undefined;
      if (end === null) throw invalidArg('end must be an i64 integer');
      const [s, e] = pySliceBounds(input.length, start, end);
      if (s > e) return [];
      return input.slice(s, e);
    }
    case 'flatten': {
      checkKnownArgs(args, ['input']);
      const input = needArray(requireArg(args, 'input'), 'input');
      const out = [];
      for (const el of input) {
        if (Array.isArray(el)) out.push(...el);
        else out.push(el);
      }
      return out;
    }

    /* ---------------- objects ---------------- */
    case 'get': {
      checkKnownArgs(args, ['input', 'key']);
      const input = needObject(requireArg(args, 'input'), 'input');
      const key = needString(requireArg(args, 'key'), 'key');
      return hasOwn(input, key) ? input[key] : null;
    }
    case 'set': {
      checkKnownArgs(args, ['input', 'key', 'value']);
      const input = needObject(requireArg(args, 'input'), 'input');
      const key = needString(requireArg(args, 'key'), 'key');
      const value = requireArg(args, 'value');
      const out = Object.create(null);
      for (const k of Object.keys(input)) out[k] = input[k];
      out[key] = value;
      return out;
    }
    case 'keys': {
      checkKnownArgs(args, ['input']);
      const input = needObject(requireArg(args, 'input'), 'input');
      return Object.keys(input).sort(byteCompare);
    }
    case 'values': {
      checkKnownArgs(args, ['input']);
      const input = needObject(requireArg(args, 'input'), 'input');
      return Object.keys(input).sort(byteCompare).map((k) => input[k]);
    }
    case 'merge': {
      checkKnownArgs(args, ['a', 'b']);
      const a = needObject(requireArg(args, 'a'), 'a');
      const b = needObject(requireArg(args, 'b'), 'b');
      const out = Object.create(null);
      for (const k of Object.keys(a)) out[k] = a[k];
      for (const k of Object.keys(b)) out[k] = b[k];
      return out;
    }
    case 'pick': {
      checkKnownArgs(args, ['input', 'keys']);
      const input = needObject(requireArg(args, 'input'), 'input');
      const keys = stringArray(requireArg(args, 'keys'), 'keys');
      const sorted = Array.from(new Set(keys)).sort(byteCompare);
      const out = Object.create(null);
      for (const k of sorted) out[k] = hasOwn(input, k) ? input[k] : null;
      return out;
    }
    case 'omit': {
      checkKnownArgs(args, ['input', 'keys']);
      const input = needObject(requireArg(args, 'input'), 'input');
      const keys = new Set(stringArray(requireArg(args, 'keys'), 'keys'));
      const out = Object.create(null);
      for (const k of Object.keys(input)) {
        if (!keys.has(k)) out[k] = input[k];
      }
      return out;
    }

    /* ---------------- utilities ---------------- */
    case 'constant': {
      checkKnownArgs(args, ['value']);
      return requireArg(args, 'value');
    }
    case 'equals': {
      checkKnownArgs(args, ['a', 'b']);
      return valueEquals(requireArg(args, 'a'), requireArg(args, 'b'));
    }
    case 'compare': {
      checkKnownArgs(args, ['a', 'b', 'op']);
      const a = requireArg(args, 'a');
      const b = requireArg(args, 'b');
      const cmpOp = requireArg(args, 'op');
      if (cmpOp !== 'gt' && cmpOp !== 'gte' && cmpOp !== 'lt' && cmpOp !== 'lte') {
        throw invalidArg('compare.op must be gt, gte, lt or lte');
      }
      const c = totalOrder(a, b);
      switch (cmpOp) {
        case 'gt': return c > 0;
        case 'gte': return c >= 0;
        case 'lt': return c < 0;
        default: return c <= 0;
      }
    }
    case 'and': {
      checkKnownArgs(args, ['a', 'b']);
      return truthy(requireArg(args, 'a')) && truthy(requireArg(args, 'b'));
    }
    case 'or': {
      checkKnownArgs(args, ['a', 'b']);
      return truthy(requireArg(args, 'a')) || truthy(requireArg(args, 'b'));
    }
    case 'not': {
      checkKnownArgs(args, ['a']);
      return !truthy(requireArg(args, 'a'));
    }
    case 'type': {
      checkKnownArgs(args, ['value']);
      return typeName(requireArg(args, 'value'));
    }

    /* ---------------- test op ---------------- */
    case 'fail_until': {
      checkKnownArgs(args, ['failures', 'value']);
      const failures = argI64(args, 'failures');
      if (failures < 0n || failures > 10n) {
        throw invalidArg('failures must be between 0 and 10');
      }
      const value = requireArg(args, 'value');
      const attempt = (failCounters.get(stepId) || 0n) + 1n;
      failCounters.set(stepId, attempt);
      if (attempt <= failures) {
        throw new WErr('TRANSIENT_FAILURE', 'transient failure #' + attempt.toString());
      }
      return value;
    }

    default:
      throw new WErr('INVALID_WORKFLOW', 'unknown op "' + op + '"');
  }
}

/* ------------------------------------------------------------------ */
/* Structural validation                                               */
/* ------------------------------------------------------------------ */

const KNOWN_OPS = new Set([
  'add', 'subtract', 'multiply', 'divide', 'mod', 'sum', 'min', 'max', 'range',
  'concat', 'split', 'join', 'uppercase', 'lowercase', 'trim', 'replace', 'length',
  'map', 'filter', 'reduce', 'sort', 'reverse', 'unique', 'slice', 'flatten',
  'get', 'set', 'keys', 'values', 'merge', 'pick', 'omit',
  'constant', 'equals', 'compare', 'and', 'or', 'not', 'type',
  'fail_until',
]);

const ID_RE = /^[A-Za-z_][A-Za-z0-9_]*$/;

function invalidWorkflow(message, step) {
  return new WErr('INVALID_WORKFLOW', message, step === undefined ? null : step);
}

function isIntegerLiteralInRange(v, lo, hi) {
  if (!(v instanceof Num)) return false;
  if (!v.isInt) return false;
  return v.big >= lo && v.big <= hi;
}

function validateWhenShape(w, stepId) {
  if (!isPlainObject(w)) {
    throw invalidWorkflow('when must be an object', stepId);
  }
  const op = w.op;
  if (typeof op === 'string') {
    switch (op) {
      case 'eq':
      case 'ne':
      case 'gt':
      case 'gte':
      case 'lt':
      case 'lte':
        if (!hasOwn(w, 'left') || !hasOwn(w, 'right')) {
          throw invalidWorkflow('condition "' + op + '" needs left and right', stepId);
        }
        return;
      case 'all':
      case 'any':
        if (!Array.isArray(w.conditions)) {
          throw invalidWorkflow('condition "' + op + '" needs a conditions array', stepId);
        }
        for (const c of w.conditions) validateWhenShape(c, stepId);
        return;
      case 'not':
        if (!isPlainObject(w.condition)) {
          throw invalidWorkflow('condition "not" needs a condition object', stepId);
        }
        validateWhenShape(w.condition, stepId);
        return;
      default:
        // Unknown condition op: left for runtime evaluation (INVALID_CONDITION).
        return;
    }
  }
  // No op: treated as a literal/ref expression, checked at runtime.
}

function validateWorkflow(doc) {
  if (!isPlainObject(doc)) {
    throw invalidWorkflow('workflow document must be a JSON object');
  }
  for (const k of Object.keys(doc)) {
    if (k !== 'version' && k !== 'steps') {
      throw invalidWorkflow('unknown top-level field "' + k + '"');
    }
  }
  if (!hasOwn(doc, 'version')) {
    throw invalidWorkflow('missing required field "version"');
  }
  if (!isIntegerLiteralInRange(doc.version, 1n, 1n)) {
    throw invalidWorkflow('version must be the integer literal 1');
  }
  if (!hasOwn(doc, 'steps')) {
    throw invalidWorkflow('missing required field "steps"');
  }
  if (!Array.isArray(doc.steps)) {
    throw invalidWorkflow('"steps" must be an array');
  }

  const ids = new Set();
  const steps = [];

  for (const raw of doc.steps) {
    if (!isPlainObject(raw)) {
      throw invalidWorkflow('each step must be an object');
    }
    // Determine the step id first for error attribution.
    let stepId = null;
    if (hasOwn(raw, 'id') && typeof raw.id === 'string') stepId = raw.id;

    for (const k of Object.keys(raw)) {
      if (!['id', 'op', 'args', 'depends_on', 'when', 'retry'].includes(k)) {
        throw invalidWorkflow('unknown step field "' + k + '"', stepId);
      }
    }
    if (!hasOwn(raw, 'id')) {
      throw invalidWorkflow('step is missing required field "id"', null);
    }
    if (typeof raw.id !== 'string') {
      throw invalidWorkflow('step id must be a string', null);
    }
    if (!ID_RE.test(raw.id)) {
      throw invalidWorkflow('invalid step id "' + raw.id + '"', raw.id);
    }
    if (ids.has(raw.id)) {
      throw invalidWorkflow('duplicate step id "' + raw.id + '"', raw.id);
    }

    if (!hasOwn(raw, 'op')) {
      throw invalidWorkflow('step "' + raw.id + '" is missing required field "op"', raw.id);
    }
    if (typeof raw.op !== 'string') {
      throw invalidWorkflow('step "op" must be a string', raw.id);
    }
    if (!KNOWN_OPS.has(raw.op)) {
      throw invalidWorkflow('unknown op "' + raw.op + '"', raw.id);
    }

    let args = Object.create(null);
    if (hasOwn(raw, 'args')) {
      if (!isPlainObject(raw.args)) {
        throw invalidWorkflow('step "args" must be an object', raw.id);
      }
      args = raw.args;
    }

    let dependsOn = [];
    if (hasOwn(raw, 'depends_on')) {
      if (!Array.isArray(raw.depends_on)) {
        throw invalidWorkflow('step "depends_on" must be an array', raw.id);
      }
      for (const d of raw.depends_on) {
        if (typeof d !== 'string') {
          throw invalidWorkflow('"depends_on" entries must be step ids', raw.id);
        }
      }
      dependsOn = raw.depends_on;
    }

    let retry = null;
    if (hasOwn(raw, 'retry')) {
      if (!isPlainObject(raw.retry)) {
        throw invalidWorkflow('step "retry" must be an object', raw.id);
      }
      for (const k of Object.keys(raw.retry)) {
        if (k !== 'max_attempts') {
          throw invalidWorkflow('unknown retry field "' + k + '"', raw.id);
        }
      }
      if (!hasOwn(raw.retry, 'max_attempts')) {
        throw invalidWorkflow('retry is missing "max_attempts"', raw.id);
      }
      const ma = raw.retry.max_attempts;
      if (!isIntegerLiteralInRange(ma, 1n, 5n)) {
        throw invalidWorkflow('retry.max_attempts must be an integer in 1..5', raw.id);
      }
      retry = { max_attempts: Number(ma.big) };
    }

    if (hasOwn(raw, 'when')) {
      validateWhenShape(raw.when, raw.id);
    }

    ids.add(raw.id);
    steps.push({
      id: raw.id,
      op: raw.op,
      args,
      depends_on: dependsOn,
      hasWhen: hasOwn(raw, 'when'),
      when: raw.when,
      retry,
      index: steps.length,
    });
  }

  // Unknown dependencies.
  for (const s of steps) {
    for (const d of s.depends_on) {
      if (!ids.has(d)) {
        throw new WErr('UNKNOWN_DEPENDENCY', 'unknown dependency "' + d + '"', s.id);
      }
    }
  }

  // Cycle detection (iterative DFS with colors).
  const byId = new Map(steps.map((s) => [s.id, s]));
  const color = new Map(); // 0 = unvisited, 1 = visiting, 2 = done
  for (const s of steps) color.set(s.id, 0);
  for (const root of steps) {
    if (color.get(root.id) !== 0) continue;
    const stack = [{ node: root, idx: 0 }];
    color.set(root.id, 1);
    while (stack.length) {
      const frame = stack[stack.length - 1];
      if (frame.idx < frame.node.depends_on.length) {
        const dep = frame.node.depends_on[frame.idx++];
        const dc = color.get(dep);
        if (dc === 1) {
          throw new WErr('CYCLIC_DEPENDENCY', 'dependency cycle detected');
        }
        if (dc === 0) {
          color.set(dep, 1);
          stack.push({ node: byId.get(dep), idx: 0 });
        }
      } else {
        color.set(frame.node.id, 2);
        stack.pop();
      }
    }
  }

  return { steps, byId };
}

/* ------------------------------------------------------------------ */
/* References and conditions                                           */
/* ------------------------------------------------------------------ */

function isRefObject(v) {
  if (!isPlainObject(v)) return false;
  const keys = Object.keys(v);
  return keys.length === 1 && keys[0] === '$ref' && typeof v.$ref === 'string';
}

function allowedDependencies(step, byId, cache) {
  let set = cache.get(step.id);
  if (set) return set;
  set = new Set();
  const stack = step.depends_on.slice();
  while (stack.length) {
    const id = stack.pop();
    if (set.has(id)) continue;
    set.add(id);
    const dep = byId.get(id);
    if (dep) for (const d of dep.depends_on) stack.push(d);
  }
  cache.set(step.id, set);
  return set;
}

function resolveReference(refStr, step, allowed, results, skippedSet) {
  const segments = refStr.split('.');
  const targetId = segments[0];
  if (!allowed.has(targetId)) {
    throw new WErr('INVALID_REFERENCE', 'reference to non-dependency "' + targetId + '"');
  }
  if (skippedSet.has(targetId)) {
    throw new WErr('SKIPPED_REFERENCE', 'reference to skipped step "' + targetId + '"');
  }
  if (!hasOwn(results, targetId)) {
    throw new WErr('INVALID_REFERENCE', 'reference to unfinshed step "' + targetId + '"');
  }
  let cur = results[targetId];
  for (let k = 1; k < segments.length; k++) {
    const seg = segments[k];
    if (Array.isArray(cur)) {
      if (!/^[0-9]+$/.test(seg)) {
        throw new WErr('INVALID_REFERENCE', 'invalid array index "' + seg + '"');
      }
      const idx = Number(seg);
      if (idx >= cur.length) {
        throw new WErr('INVALID_REFERENCE', 'array index out of range "' + seg + '"');
      }
      cur = cur[idx];
    } else if (isPlainObject(cur)) {
      if (!hasOwn(cur, seg)) {
        throw new WErr('INVALID_REFERENCE', 'missing object key "' + seg + '"');
      }
      cur = cur[seg];
    } else {
      throw new WErr('INVALID_REFERENCE', 'cannot descend into non-container value');
    }
  }
  return cur;
}

function resolveValue(value, step, allowed, results, skippedSet) {
  if (isRefObject(value)) {
    return resolveReference(value.$ref, step, allowed, results, skippedSet);
  }
  if (Array.isArray(value)) {
    return value.map((v) => resolveValue(v, step, allowed, results, skippedSet));
  }
  if (isPlainObject(value)) {
    const out = Object.create(null);
    for (const k of Object.keys(value)) {
      out[k] = resolveValue(value[k], step, allowed, results, skippedSet);
    }
    return out;
  }
  return value;
}

function evalCondition(cond, step, allowed, results, skippedSet) {
  if (!isPlainObject(cond)) {
    throw new WErr('INVALID_CONDITION', 'condition must be an object');
  }
  if (typeof cond.op === 'string') {
    switch (cond.op) {
      case 'eq':
      case 'ne':
      case 'gt':
      case 'gte':
      case 'lt':
      case 'lte': {
        if (!hasOwn(cond, 'left') || !hasOwn(cond, 'right')) {
          throw new WErr('INVALID_CONDITION', 'condition needs left and right');
        }
        const left = resolveValue(cond.left, step, allowed, results, skippedSet);
        const right = resolveValue(cond.right, step, allowed, results, skippedSet);
        if (cond.op === 'eq') return valueEquals(left, right);
        if (cond.op === 'ne') return !valueEquals(left, right);
        const c = totalOrder(left, right);
        switch (cond.op) {
          case 'gt': return c > 0;
          case 'gte': return c >= 0;
          case 'lt': return c < 0;
          default: return c <= 0;
        }
      }
      case 'all': {
        if (!Array.isArray(cond.conditions)) {
          throw new WErr('INVALID_CONDITION', '"all" needs a conditions array');
        }
        for (const sub of cond.conditions) {
          const v = evalCondition(sub, step, allowed, results, skippedSet);
          if (typeof v !== 'boolean') {
            throw new WErr('INVALID_CONDITION', 'sub-condition is not boolean');
          }
          if (!v) return false;
        }
        return true;
      }
      case 'any': {
        if (!Array.isArray(cond.conditions)) {
          throw new WErr('INVALID_CONDITION', '"any" needs a conditions array');
        }
        for (const sub of cond.conditions) {
          const v = evalCondition(sub, step, allowed, results, skippedSet);
          if (typeof v !== 'boolean') {
            throw new WErr('INVALID_CONDITION', 'sub-condition is not boolean');
          }
          if (v) return true;
        }
        return false;
      }
      case 'not': {
        const v = evalCondition(cond.condition, step, allowed, results, skippedSet);
        if (typeof v !== 'boolean') {
          throw new WErr('INVALID_CONDITION', 'sub-condition is not boolean');
        }
        return !v;
      }
      default:
        throw new WErr('INVALID_CONDITION', 'unknown condition op "' + cond.op + '"');
    }
  }
  // No op: evaluate as a directive / literal and require a boolean.
  const v = resolveValue(cond, step, allowed, results, skippedSet);
  if (typeof v !== 'boolean') {
    throw new WErr('INVALID_CONDITION', 'condition did not evaluate to a boolean');
  }
  return v;
}

/* ------------------------------------------------------------------ */
/* Execution                                                           */
/* ------------------------------------------------------------------ */

function makeResponse(status, results, executionOrder, skipped, error) {
  return {
    status,
    results,
    execution_order: executionOrder,
    skipped,
    error,
  };
}

function execute(validated) {
  const steps = validated.steps;
  const byId = validated.byId;

  const results = Object.create(null);
  const executionOrder = [];
  const skippedSteps = [];
  const skippedSet = new Set();
  const done = new Set();
  const failCounters = new Map();
  const depCache = new Map();

  function fail(code, message, stepId) {
    return {
      response: makeResponse('failed', results, executionOrder, skippedSteps, {
        code,
        step: stepId === undefined ? null : stepId,
        message,
      }),
      exitCode: exitFor(code),
    };
  }

  for (let iter = 0; iter < steps.length; iter++) {
    let step = null;
    for (const candidate of steps) {
      if (done.has(candidate.id)) continue;
      if (candidate.depends_on.every((d) => done.has(d))) {
        step = candidate;
        break;
      }
    }
    if (!step) {
      // Should be impossible after validation.
      return fail('CYCLIC_DEPENDENCY', 'dependency cycle detected');
    }

    const allowed = allowedDependencies(step, byId, depCache);

    try {
      if (step.hasWhen) {
        const condition = evalCondition(step.when, step, allowed, results, skippedSet);
        if (typeof condition !== 'boolean') {
          throw new WErr('INVALID_CONDITION', 'condition is not a boolean');
        }
        if (condition === false) {
          results[step.id] = null;
          executionOrder.push(step.id);
          skippedSteps.push(step.id);
          skippedSet.add(step.id);
          done.add(step.id);
          continue;
        }
      }

      const resolvedArgs = resolveValue(step.args, step, allowed, results, skippedSet);

      const maxAttempts = step.retry ? step.retry.max_attempts : 1;
      let value;
      let ok = false;
      for (let attempt = 0; attempt < maxAttempts; attempt++) {
        try {
          value = dispatch(step.op, resolvedArgs, step.id, failCounters);
          ok = true;
          break;
        } catch (e) {
          if (e instanceof WErr && e.code === 'TRANSIENT_FAILURE') {
            continue;
          }
          throw e;
        }
      }
      if (!ok) {
        throw new WErr('STEP_FAILED', 'step failed after retries');
      }

      results[step.id] = value;
      executionOrder.push(step.id);
      done.add(step.id);
    } catch (e) {
      if (e instanceof WErr) {
        return fail(e.code, e.message, typeof e.step === 'string' ? e.step : step.id);
      }
      throw e;
    }
  }

  return {
    response: makeResponse('completed', results, executionOrder, skippedSteps, null),
    exitCode: 0,
  };
}

/* ------------------------------------------------------------------ */
/* CLI                                                                 */
/* ------------------------------------------------------------------ */

function emit(obj) {
  process.stdout.write(serialize(obj) + '\n');
}

function inputError(message) {
  emit(makeResponse('failed', Object.create(null), [], [], {
    code: 'INVALID_WORKFLOW',
    step: null,
    message,
  }));
  process.exitCode = 2;
}

function main() {
  const argv = process.argv.slice(2);

  if (argv.length !== 2 || argv[0] !== 'run') {
    inputError('usage: workflow run <workflow.json>  (or: workflow run -)');
    return;
  }

  let text;
  try {
    if (argv[1] === '-') {
      text = fs.readFileSync(0, 'utf8');
    } else {
      text = fs.readFileSync(argv[1], 'utf8');
    }
  } catch (e) {
    inputError('cannot read input document: ' + (e && e.message ? e.message : String(e)));
    return;
  }

  // Tolerate a UTF-8 BOM at the start of the input document.
  if (text.charCodeAt(0) === 0xfeff) text = text.slice(1);

  let doc;
  try {
    doc = parseJson(text);
  } catch (e) {
    if (e instanceof WErr) {
      emit(makeResponse('failed', Object.create(null), [], [], {
        code: e.code,
        step: e.step === undefined ? null : e.step,
        message: e.message,
      }));
      process.exitCode = exitFor(e.code);
      return;
    }
    throw e;
  }

  let validated;
  try {
    validated = validateWorkflow(doc);
  } catch (e) {
    if (e instanceof WErr) {
      emit(makeResponse('failed', Object.create(null), [], [], {
        code: e.code,
        step: e.step === undefined ? null : e.step,
        message: e.message,
      }));
      process.exitCode = exitFor(e.code);
      return;
    }
    throw e;
  }

  const outcome = execute(validated);
  emit(outcome.response);
  process.exitCode = outcome.exitCode;
}

try {
  main();
} catch (e) {
  // Internal error: best-effort JSON on stdout, exit 3.
  try {
    emit(makeResponse('failed', Object.create(null), [], [], {
      code: 'INTERNAL_ERROR',
      step: null,
      message: e && e.message ? String(e.message) : String(e),
    }));
  } catch (_) {
    process.stdout.write('{"status":"failed","results":{},"execution_order":[],"skipped":[],"error":{"code":"INTERNAL_ERROR","step":null,"message":"internal error"}}\n');
  }
  process.exitCode = 3;
}
