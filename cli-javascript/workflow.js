#!/usr/bin/env node
'use strict';

/*
 * workflow — declarative workflow engine (SPEC.md)
 *
 * Node.js standard library only. No dependencies, no eval, no network,
 * no filesystem writes. Deterministic: same input -> same output.
 */

const fs = require('fs');

/* ------------------------------------------------------------------ */
/* Errors                                                              */
/* ------------------------------------------------------------------ */

class EngineError extends Error {
  constructor(code, message, step) {
    super(message || code);
    this.code = code;
    this.step = step === undefined ? null : step;
  }
}

function err(code, message, step) {
  return new EngineError(code, message, step);
}

function argErr(message) {
  return err('INVALID_ARG', message);
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

const hasOwn = (o, k) => Object.prototype.hasOwnProperty.call(o, k);

const isPlainObj = (v) =>
  typeof v === 'object' && v !== null && !Array.isArray(v);

const isNum = (v) => typeof v === 'bigint' || typeof v === 'number';

const I64_MIN = -(1n << 63n);
const I64_MAX = (1n << 63n) - 1n;

const ID_RE = /^[A-Za-z_][A-Za-z0-9_]*$/;

const BINARY_MATH = new Set([
  'add',
  'subtract',
  'multiply',
  'divide',
  'mod',
]);

const OPS = new Set([
  // math
  'add',
  'subtract',
  'multiply',
  'divide',
  'mod',
  'sum',
  'min',
  'max',
  'range',
  // strings
  'concat',
  'split',
  'join',
  'uppercase',
  'lowercase',
  'trim',
  'replace',
  'length',
  // arrays
  'map',
  'filter',
  'reduce',
  'sort',
  'reverse',
  'unique',
  'slice',
  'flatten',
  // objects
  'get',
  'set',
  'keys',
  'values',
  'merge',
  'pick',
  'omit',
  // utilities
  'constant',
  'equals',
  'compare',
  'and',
  'or',
  'not',
  'type',
  // test op
  'fail_until',
]);

const COND_BINARY = new Set(['eq', 'ne', 'gt', 'gte', 'lt', 'lte']);

const TYPE_NAMES = new Set([
  'null',
  'boolean',
  'number',
  'string',
  'array',
  'object',
]);

/* ------------------------------------------------------------------ */
/* JSON parser (integers preserved as BigInt, i64-safe)                */
/* ------------------------------------------------------------------ */

const NUMBER_RE = /-?(?:0|[1-9]\d*)(?:\.\d+)?(?:[eE][+-]?\d+)?/y;

function parseJSONText(text) {
  if (typeof text !== 'string') {
    throw err('INVALID_WORKFLOW', 'input is not text');
  }
  if (text.charCodeAt(0) === 0xfeff) text = text.slice(1);

  const len = text.length;
  let i = 0;

  const fail = (msg) => {
    throw err('INVALID_WORKFLOW', `invalid JSON: ${msg} at offset ${i}`);
  };

  function skipWs() {
    while (i < len) {
      const c = text.charCodeAt(i);
      if (c === 0x20 || c === 0x09 || c === 0x0a || c === 0x0d) i++;
      else break;
    }
  }

  function parseString() {
    const start = i; // pointing at opening quote
    i++;
    while (i < len) {
      const c = text.charCodeAt(i);
      if (c === 0x22) {
        i++;
        const raw = text.slice(start, i);
        try {
          return JSON.parse(raw);
        } catch (e) {
          fail('bad string literal');
        }
      }
      if (c === 0x5c) {
        i += 2;
        continue;
      }
      i++;
    }
    fail('unterminated string');
  }

  function parseNumber() {
    NUMBER_RE.lastIndex = i;
    const m = NUMBER_RE.exec(text);
    if (!m) fail('bad number');
    const lit = m[0];
    i = NUMBER_RE.lastIndex;
    if (/[.eE]/.test(lit)) {
      const n = Number(lit);
      if (!Number.isFinite(n)) fail('number out of range');
      return n;
    }
    return BigInt(lit);
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
      if (text[i] !== ':') fail('expected colon');
      i++;
      obj[key] = parseValue();
      skipWs();
      if (text[i] === ',') {
        i++;
        continue;
      }
      if (text[i] === '}') {
        i++;
        return obj;
      }
      fail('expected comma or closing brace');
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
      fail('expected comma or closing bracket');
    }
  }

  function parseValue() {
    skipWs();
    if (i >= len) fail('unexpected end of input');
    const c = text[i];
    if (c === '"') return parseString();
    if (c === '{') return parseObject();
    if (c === '[') return parseArray();
    if (c === 't') {
      if (text.startsWith('true', i)) {
        i += 4;
        return true;
      }
      fail('bad literal');
    }
    if (c === 'f') {
      if (text.startsWith('false', i)) {
        i += 5;
        return false;
      }
      fail('bad literal');
    }
    if (c === 'n') {
      if (text.startsWith('null', i)) {
        i += 4;
        return null;
      }
      fail('bad literal');
    }
    if (c === '-' || (c >= '0' && c <= '9')) return parseNumber();
    fail(`unexpected character ${JSON.stringify(c)}`);
  }

  const value = parseValue();
  skipWs();
  if (i !== len) fail('trailing content');
  return value;
}

/* ------------------------------------------------------------------ */
/* JSON serializer (emits BigInt as a JSON number)                     */
/* ------------------------------------------------------------------ */

function stringify(v) {
  if (v === null) return 'null';
  switch (typeof v) {
    case 'boolean':
      return v ? 'true' : 'false';
    case 'bigint':
      return v.toString();
    case 'number':
      if (!Number.isFinite(v)) return 'null';
      return JSON.stringify(v);
    case 'string':
      return JSON.stringify(v);
    default:
      break;
  }
  if (Array.isArray(v)) {
    return '[' + v.map(stringify).join(',') + ']';
  }
  if (typeof v === 'object') {
    const parts = [];
    for (const k of Object.keys(v)) {
      parts.push(JSON.stringify(k) + ':' + stringify(v[k]));
    }
    return '{' + parts.join(',') + '}';
  }
  return 'null';
}

/* ------------------------------------------------------------------ */
/* Numeric helpers (i64)                                               */
/* ------------------------------------------------------------------ */

function toI64(v, name) {
  let b;
  if (typeof v === 'bigint') {
    b = v;
  } else if (typeof v === 'number') {
    if (!Number.isFinite(v) || !Number.isInteger(v)) {
      throw argErr(`argument ${name || 'number'} must be an integer`);
    }
    b = BigInt(v);
  } else {
    throw argErr(`argument ${name || 'number'} must be an integer`);
  }
  if (b < I64_MIN || b > I64_MAX) {
    throw argErr(`argument ${name || 'number'} is outside i64 range`);
  }
  return b;
}

function checkOverflow(v) {
  if (v < I64_MIN || v > I64_MAX) {
    throw err('ARITHMETIC_OVERFLOW', 'i64 arithmetic overflow');
  }
  return v;
}

function arith(kind, a, b) {
  const x = toI64(a, 'a');
  const y = toI64(b, 'b');
  switch (kind) {
    case 'add':
      return checkOverflow(x + y);
    case 'subtract':
      return checkOverflow(x - y);
    case 'multiply':
      return checkOverflow(x * y);
    case 'divide':
      if (y === 0n) throw err('DIVISION_BY_ZERO', 'division by zero');
      return checkOverflow(x / y); // BigInt division truncates toward zero
    case 'mod':
      if (y === 0n) throw err('DIVISION_BY_ZERO', 'modulo by zero');
      return checkOverflow(x % y); // sign of the dividend
    default:
      throw err('INTERNAL_ERROR', `unknown arithmetic op ${kind}`);
  }
}

function negateI64(v) {
  return checkOverflow(-toI64(v, 'value'));
}

function absI64(v) {
  const x = toI64(v, 'value');
  return checkOverflow(x < 0n ? -x : x);
}

function cmpNum(a, b) {
  if (typeof a === 'bigint' && typeof b === 'bigint') {
    return a < b ? -1 : a > b ? 1 : 0;
  }
  if (typeof a === 'bigint') {
    if (typeof b === 'number' && Number.isInteger(b)) {
      const bb = BigInt(b);
      return a < bb ? -1 : a > bb ? 1 : 0;
    }
    const an = Number(a);
    return an < b ? -1 : an > b ? 1 : 0;
  }
  if (typeof b === 'bigint') return -cmpNum(b, a);
  return a < b ? -1 : a > b ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Value semantics                                                     */
/* ------------------------------------------------------------------ */

function typeName(v) {
  if (v === null) return 'null';
  switch (typeof v) {
    case 'boolean':
      return 'boolean';
    case 'bigint':
    case 'number':
      return 'number';
    case 'string':
      return 'string';
    default:
      break;
  }
  if (Array.isArray(v)) return 'array';
  return 'object';
}

function typeRank(v) {
  switch (typeName(v)) {
    case 'number':
      return 0;
    case 'string':
      return 1;
    case 'boolean':
      return 2;
    case 'null':
      return 3;
    case 'array':
      return 4;
    default:
      return 5;
  }
}

// UTF-8 byte-wise lexicographic order (equivalently: code point order).
function cmpStr(a, b) {
  return Buffer.compare(Buffer.from(a, 'utf8'), Buffer.from(b, 'utf8'));
}

function cmpValues(a, b) {
  const ra = typeRank(a);
  const rb = typeRank(b);
  if (ra !== rb) return ra < rb ? -1 : 1;
  switch (ra) {
    case 0:
      return cmpNum(a, b);
    case 1:
      return cmpStr(a, b);
    case 2:
      return a === b ? 0 : a === false ? -1 : 1;
    case 3:
      return 0;
    case 4: {
      const n = Math.min(a.length, b.length);
      for (let i = 0; i < n; i++) {
        const c = cmpValues(a[i], b[i]);
        if (c !== 0) return c;
      }
      return a.length < b.length ? -1 : a.length > b.length ? 1 : 0;
    }
    default: {
      const ka = Object.keys(a).sort(cmpStr);
      const kb = Object.keys(b).sort(cmpStr);
      const n = Math.min(ka.length, kb.length);
      for (let i = 0; i < n; i++) {
        const c = cmpStr(ka[i], kb[i]);
        if (c !== 0) return c;
      }
      if (ka.length !== kb.length) return ka.length < kb.length ? -1 : 1;
      for (let i = 0; i < ka.length; i++) {
        const c = cmpValues(a[ka[i]], b[ka[i]]);
        if (c !== 0) return c;
      }
      return 0;
    }
  }
}

function structuralEquals(a, b) {
  if (isNum(a) && isNum(b)) return cmpNum(a, b) === 0;
  if (a === null || b === null) return a === b;
  if (typeof a !== typeof b) return false;
  if (typeof a !== 'object') return a === b;
  const aArr = Array.isArray(a);
  const bArr = Array.isArray(b);
  if (aArr !== bArr) return false;
  if (aArr) {
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) {
      if (!structuralEquals(a[i], b[i])) return false;
    }
    return true;
  }
  const ka = Object.keys(a);
  const kb = Object.keys(b);
  if (ka.length !== kb.length) return false;
  for (const k of ka) {
    if (!hasOwn(b, k)) return false;
    if (!structuralEquals(a[k], b[k])) return false;
  }
  return true;
}

function truthy(v) {
  if (v === null || v === false) return false;
  if (typeof v === 'string') return v.length > 0;
  if (typeof v === 'bigint') return v !== 0n;
  if (typeof v === 'number') return v !== 0;
  if (Array.isArray(v)) return v.length > 0;
  if (typeof v === 'object') return Object.keys(v).length > 0;
  return true;
}

/* ------------------------------------------------------------------ */
/* Argument checking                                                   */
/* ------------------------------------------------------------------ */

function checkKeys(obj, allowed, required) {
  for (const k of Object.keys(obj)) {
    if (!allowed.includes(k)) throw argErr(`unknown argument "${k}"`);
  }
  for (const k of required) {
    if (!hasOwn(obj, k)) throw argErr(`missing argument "${k}"`);
  }
}

function needString(v, name) {
  if (typeof v !== 'string') throw argErr(`argument "${name}" must be a string`);
  return v;
}

function needNumber(v, name) {
  return toI64(v, name);
}

function needArray(v, name) {
  if (!Array.isArray(v)) throw argErr(`argument "${name}" must be an array`);
  return v;
}

function needObject(v, name) {
  if (!isPlainObj(v)) throw argErr(`argument "${name}" must be an object`);
  return v;
}

/* ------------------------------------------------------------------ */
/* Objects / ordering helpers                                          */
/* ------------------------------------------------------------------ */

function sortObject(obj) {
  const out = Object.create(null);
  for (const k of Object.keys(obj).sort(cmpStr)) out[k] = obj[k];
  return out;
}

function makeObject(entries) {
  const out = Object.create(null);
  for (const [k, v] of entries) out[k] = v;
  return out;
}

/* ------------------------------------------------------------------ */
/* map.transform                                                       */
/* ------------------------------------------------------------------ */

function isSelfMarker(v) {
  return (
    isPlainObj(v) &&
    Object.keys(v).length === 1 &&
    hasOwn(v, '$self') &&
    v.$self === true
  );
}

function applyTransform(t, el) {
  // Accept the PRD-style shorthand ("stringify", "negate", ...).
  if (typeof t === 'string') t = makeObject([['type', t]]);
  if (!isPlainObj(t)) throw argErr('transform must be an object');
  if (!hasOwn(t, 'type') || typeof t.type !== 'string') {
    throw argErr('transform needs a string "type"');
  }
  switch (t.type) {
    case 'constant':
      checkKeys(t, ['type', 'value'], ['type', 'value']);
      return t.value;
    case 'add':
    case 'subtract':
    case 'multiply':
    case 'divide':
    case 'mod':
      checkKeys(t, ['type', 'value'], ['type', 'value']);
      return arith(t.type, el, t.value);
    case 'negate':
      checkKeys(t, ['type'], ['type']);
      return negateI64(el);
    case 'abs':
      checkKeys(t, ['type'], ['type']);
      return absI64(el);
    case 'stringify':
      checkKeys(t, ['type'], ['type']);
      return stringify(el);
    case 'type':
      checkKeys(t, ['type'], ['type']);
      return typeName(el);
    case 'op': {
      checkKeys(t, ['type', 'op', 'args'], ['type', 'op', 'args']);
      if (typeof t.op !== 'string' || !BINARY_MATH.has(t.op)) {
        throw argErr('transform op must be one of add/subtract/multiply/divide/mod');
      }
      const a = needObject(t.args, 'args');
      checkKeys(a, ['a', 'b'], ['a', 'b']);
      const left = isSelfMarker(a.a) ? el : a.a;
      const right = isSelfMarker(a.b) ? el : a.b;
      return arith(t.op, left, right);
    }
    default:
      throw argErr(`unknown transform type "${t.type}"`);
  }
}

/* ------------------------------------------------------------------ */
/* filter.predicate                                                    */
/* ------------------------------------------------------------------ */

function applyPredicate(p, el) {
  // Accept the PRD-style shorthand ("even", "odd", ...).
  if (typeof p === 'string') p = makeObject([['type', p]]);
  if (!isPlainObj(p)) throw argErr('predicate must be an object');
  if (!hasOwn(p, 'type') || typeof p.type !== 'string') {
    throw argErr('predicate needs a string "type"');
  }
  switch (p.type) {
    case 'even':
      checkKeys(p, ['type'], ['type']);
      return toI64(el, 'element') % 2n === 0n;
    case 'odd':
      checkKeys(p, ['type'], ['type']);
      return toI64(el, 'element') % 2n !== 0n;
    case 'gt':
    case 'gte':
    case 'lt':
    case 'lte':
    case 'eq':
    case 'ne': {
      checkKeys(p, ['type', 'value'], ['type', 'value']);
      if (p.type === 'eq') return structuralEquals(el, p.value);
      if (p.type === 'ne') return !structuralEquals(el, p.value);
      const c = cmpValues(el, p.value);
      if (p.type === 'gt') return c > 0;
      if (p.type === 'gte') return c >= 0;
      if (p.type === 'lt') return c < 0;
      return c <= 0;
    }
    case 'truthy':
      checkKeys(p, ['type'], ['type']);
      return truthy(el);
    case 'falsy':
      checkKeys(p, ['type'], ['type']);
      return !truthy(el);
    case 'type_is': {
      // SPEC shows a duplicate "type" key; accept the plausible spellings.
      checkKeys(
        p,
        ['type', 'value', 'type_name', 'name', 'target', 'expected', 'typeName'],
        ['type'],
      );
      let target;
      if (hasOwn(p, 'value')) target = p.value;
      else if (hasOwn(p, 'type_name')) target = p.type_name;
      else if (hasOwn(p, 'name')) target = p.name;
      else if (hasOwn(p, 'target')) target = p.target;
      else if (hasOwn(p, 'expected')) target = p.expected;
      else if (hasOwn(p, 'typeName')) target = p.typeName;
      else throw argErr('type_is predicate needs a target type');
      if (typeof target !== 'string' || !TYPE_NAMES.has(target)) {
        throw argErr('type_is predicate has an invalid type name');
      }
      return typeName(el) === target;
    }
    case 'string_contains':
      checkKeys(p, ['type', 'value'], ['type', 'value']);
      if (typeof p.value !== 'string') {
        throw argErr('string_contains predicate needs a string value');
      }
      return typeof el === 'string' && el.includes(p.value);
    default:
      throw argErr(`unknown predicate type "${p.type}"`);
  }
}

/* ------------------------------------------------------------------ */
/* Operations                                                          */
/* ------------------------------------------------------------------ */

function executeOp(op, args, attempt) {
  switch (op) {
    /* ---------------- math ---------------- */
    case 'add':
    case 'subtract':
    case 'multiply':
    case 'divide':
    case 'mod': {
      checkKeys(args, ['a', 'b'], ['a', 'b']);
      return arith(op, args.a, args.b);
    }

    case 'sum': {
      checkKeys(args, ['input'], ['input']);
      const arr = needArray(args.input, 'input');
      let acc = 0n;
      for (const x of arr) acc = checkOverflow(acc + toI64(x, 'input element'));
      return acc;
    }

    case 'min':
    case 'max': {
      checkKeys(args, ['input'], ['input']);
      const arr = needArray(args.input, 'input');
      if (arr.length === 0) {
        throw err('EMPTY_ARRAY', `${op} requires a non-empty array`);
      }
      let best = toI64(arr[0], 'input element');
      for (let i = 1; i < arr.length; i++) {
        const x = toI64(arr[i], 'input element');
        if (op === 'min' ? x < best : x > best) best = x;
      }
      return best;
    }

    case 'range': {
      checkKeys(args, ['start', 'end'], ['start', 'end']);
      const start = needNumber(args.start, 'start');
      const end = needNumber(args.end, 'end');
      const out = [];
      if (start > end) return out;
      for (let v = start; v <= end; v++) out.push(v);
      return out;
    }

    /* ---------------- strings ---------------- */
    case 'concat': {
      checkKeys(args, ['a', 'b'], ['a', 'b']);
      return needString(args.a, 'a') + needString(args.b, 'b');
    }

    case 'split': {
      checkKeys(args, ['input', 'sep'], ['input', 'sep']);
      const input = needString(args.input, 'input');
      const sep = needString(args.sep, 'sep');
      if (sep === '') return Array.from(input);
      return input.split(sep);
    }

    case 'join': {
      checkKeys(args, ['input', 'sep'], ['input', 'sep']);
      const arr = needArray(args.input, 'input');
      const sep = needString(args.sep, 'sep');
      for (const x of arr) {
        if (typeof x !== 'string') {
          throw argErr('join input must be an array of strings');
        }
      }
      return arr.join(sep);
    }

    case 'uppercase': {
      checkKeys(args, ['input'], ['input']);
      return asciiMap(needString(args.input, 'input'), (c) =>
        c >= 'a' && c <= 'z' ? c.toUpperCase() : c,
      );
    }

    case 'lowercase': {
      checkKeys(args, ['input'], ['input']);
      return asciiMap(needString(args.input, 'input'), (c) =>
        c >= 'A' && c <= 'Z' ? c.toLowerCase() : c,
      );
    }

    case 'trim': {
      checkKeys(args, ['input'], ['input']);
      const s = needString(args.input, 'input');
      const ws = new Set([' ', '\t', '\n', '\r', '\f', '\v']);
      let start = 0;
      let end = s.length;
      while (start < end && ws.has(s[start])) start++;
      while (end > start && ws.has(s[end - 1])) end--;
      return s.slice(start, end);
    }

    case 'replace': {
      checkKeys(args, ['input', 'search', 'replace'], [
        'input',
        'search',
        'replace',
      ]);
      const input = needString(args.input, 'input');
      const search = needString(args.search, 'search');
      const replacement = needString(args.replace, 'replace');
      // Literal replacement, left-to-right, non-overlapping. The callback
      // form prevents "$" sequences in the replacement from being special.
      return input.replaceAll(search, () => replacement);
    }

    case 'length': {
      checkKeys(args, ['input'], ['input']);
      return BigInt(Array.from(needString(args.input, 'input')).length);
    }

    /* ---------------- arrays ---------------- */
    case 'map': {
      checkKeys(args, ['input', 'transform'], ['input', 'transform']);
      const arr = needArray(args.input, 'input');
      return arr.map((el) => applyTransform(args.transform, el));
    }

    case 'filter': {
      checkKeys(args, ['input', 'predicate'], ['input', 'predicate']);
      const arr = needArray(args.input, 'input');
      return arr.filter((el) => applyPredicate(args.predicate, el));
    }

    case 'reduce': {
      checkKeys(args, ['input', 'op', 'initial'], ['input', 'op']);
      const arr = needArray(args.input, 'input');
      const kind = args.op;
      if (kind !== 'add' && kind !== 'subtract' && kind !== 'multiply') {
        throw argErr('reduce op must be one of add/subtract/multiply');
      }
      let acc;
      if (hasOwn(args, 'initial')) {
        acc = toI64(args.initial, 'initial');
      } else {
        acc = kind === 'multiply' ? 1n : 0n;
      }
      for (const x of arr) acc = arith(kind, acc, x);
      return acc;
    }

    case 'sort': {
      checkKeys(args, ['input'], ['input']);
      const arr = needArray(args.input, 'input');
      return arr.slice().sort(cmpValues);
    }

    case 'reverse': {
      checkKeys(args, ['input'], ['input']);
      return needArray(args.input, 'input').slice().reverse();
    }

    case 'unique': {
      checkKeys(args, ['input'], ['input']);
      const arr = needArray(args.input, 'input');
      const out = [];
      for (const el of arr) {
        if (!out.some((seen) => structuralEquals(seen, el))) out.push(el);
      }
      return out;
    }

    case 'slice': {
      checkKeys(args, ['input', 'start', 'end'], ['input', 'start']);
      const arr = needArray(args.input, 'input');
      const start = normIndex(needNumber(args.start, 'start'), arr.length);
      const end = hasOwn(args, 'end')
        ? normIndex(needNumber(args.end, 'end'), arr.length)
        : arr.length;
      if (end <= start) return [];
      return arr.slice(start, end);
    }

    case 'flatten': {
      checkKeys(args, ['input'], ['input']);
      const arr = needArray(args.input, 'input');
      const out = [];
      for (const el of arr) {
        if (Array.isArray(el)) {
          for (const x of el) out.push(x);
        } else {
          out.push(el);
        }
      }
      return out;
    }

    /* ---------------- objects ---------------- */
    case 'get': {
      checkKeys(args, ['input', 'key'], ['input', 'key']);
      const obj = needObject(args.input, 'input');
      const key = needString(args.key, 'key');
      return hasOwn(obj, key) ? obj[key] : null;
    }

    case 'set': {
      checkKeys(args, ['input', 'key', 'value'], ['input', 'key', 'value']);
      const obj = needObject(args.input, 'input');
      const key = needString(args.key, 'key');
      const out = Object.create(null);
      for (const k of Object.keys(obj)) out[k] = obj[k];
      out[key] = args.value;
      return sortObject(out);
    }

    case 'keys': {
      checkKeys(args, ['input'], ['input']);
      return Object.keys(needObject(args.input, 'input')).sort(cmpStr);
    }

    case 'values': {
      checkKeys(args, ['input'], ['input']);
      const obj = needObject(args.input, 'input');
      return Object.keys(obj)
        .sort(cmpStr)
        .map((k) => obj[k]);
    }

    case 'merge': {
      checkKeys(args, ['a', 'b'], ['a', 'b']);
      const a = needObject(args.a, 'a');
      const b = needObject(args.b, 'b');
      const out = Object.create(null);
      for (const k of Object.keys(a)) out[k] = a[k];
      for (const k of Object.keys(b)) out[k] = b[k];
      return sortObject(out);
    }

    case 'pick': {
      checkKeys(args, ['input', 'keys'], ['input', 'keys']);
      const obj = needObject(args.input, 'input');
      const keys = needArray(args.keys, 'keys');
      for (const k of keys) {
        if (typeof k !== 'string') {
          throw argErr('pick keys must be strings');
        }
      }
      const wanted = new Set(keys);
      return makeObject(
        [...wanted]
          .sort(cmpStr)
          .map((k) => [k, hasOwn(obj, k) ? obj[k] : null]),
      );
    }

    case 'omit': {
      checkKeys(args, ['input', 'keys'], ['input', 'keys']);
      const obj = needObject(args.input, 'input');
      const keys = needArray(args.keys, 'keys');
      for (const k of keys) {
        if (typeof k !== 'string') {
          throw argErr('omit keys must be strings');
        }
      }
      const drop = new Set(keys);
      return makeObject(
        Object.keys(obj)
          .filter((k) => !drop.has(k))
          .sort(cmpStr)
          .map((k) => [k, obj[k]]),
      );
    }

    /* ---------------- utilities ---------------- */
    case 'constant': {
      checkKeys(args, ['value'], ['value']);
      return args.value;
    }

    case 'equals': {
      checkKeys(args, ['a', 'b'], ['a', 'b']);
      return structuralEquals(args.a, args.b);
    }

    case 'compare': {
      checkKeys(args, ['a', 'b', 'op'], ['a', 'b', 'op']);
      const c = cmpValues(args.a, args.b);
      switch (args.op) {
        case 'gt':
          return c > 0;
        case 'gte':
          return c >= 0;
        case 'lt':
          return c < 0;
        case 'lte':
          return c <= 0;
        default:
          throw argErr('compare op must be one of gt/gte/lt/lte');
      }
    }

    case 'and': {
      checkKeys(args, ['a', 'b'], ['a', 'b']);
      return truthy(args.a) && truthy(args.b);
    }

    case 'or': {
      checkKeys(args, ['a', 'b'], ['a', 'b']);
      return truthy(args.a) || truthy(args.b);
    }

    case 'not': {
      checkKeys(args, ['a'], ['a']);
      return !truthy(args.a);
    }

    case 'type': {
      checkKeys(args, ['value'], ['value']);
      return typeName(args.value);
    }

    /* ---------------- test op ---------------- */
    case 'fail_until': {
      checkKeys(args, ['failures', 'value'], ['failures', 'value']);
      const failures = toI64(args.failures, 'failures');
      if (failures < 0n || failures > 10n) {
        throw argErr('failures must be between 0 and 10');
      }
      if (BigInt(attempt) <= failures) {
        throw err('TRANSIENT_FAILURE', `transient failure on attempt ${attempt}`);
      }
      return args.value;
    }

    default:
      throw err('INTERNAL_ERROR', `unknown op ${op}`);
  }
}

function normIndex(v, len) {
  let n = v;
  const L = BigInt(len);
  if (n < 0n) n = L + n;
  if (n < 0n) n = 0n;
  if (n > L) n = L;
  return Number(n);
}

function asciiMap(s, fn) {
  const out = [];
  for (const ch of s) out.push(fn(ch));
  return out.join('');
}

/* ------------------------------------------------------------------ */
/* Conditions and references                                           */
/* ------------------------------------------------------------------ */

function resolveValue(v, ctx) {
  if (
    isPlainObj(v) &&
    Object.keys(v).length === 1 &&
    hasOwn(v, '$ref') &&
    typeof v.$ref === 'string'
  ) {
    return resolveRef(v.$ref, ctx);
  }
  if (Array.isArray(v)) return v.map((x) => resolveValue(x, ctx));
  if (isPlainObj(v)) {
    const out = Object.create(null);
    for (const k of Object.keys(v)) out[k] = resolveValue(v[k], ctx);
    return out;
  }
  return v;
}

function resolveRef(path, ctx) {
  const parts = path.split('.');
  const stepId = parts[0];
  if (stepId === '') throw err('INVALID_REFERENCE', 'empty reference');
  if (stepId === ctx.stepId) {
    throw err('INVALID_REFERENCE', 'self reference');
  }
  if (!ctx.deps.has(stepId)) {
    throw err(
      'INVALID_REFERENCE',
      `reference to "${stepId}" which is not a dependency`,
    );
  }
  if (ctx.skipped.has(stepId)) {
    throw err('SKIPPED_REFERENCE', `reference to skipped step "${stepId}"`);
  }
  let cur = ctx.results[stepId];
  for (let i = 1; i < parts.length; i++) {
    const seg = parts[i];
    if (seg === '') throw err('INVALID_REFERENCE', 'empty path segment');
    if (Array.isArray(cur)) {
      if (!/^\d+$/.test(seg)) {
        throw err('INVALID_REFERENCE', `bad array index "${seg}"`);
      }
      const idx = Number(seg);
      if (!Number.isSafeInteger(idx) || idx >= cur.length) {
        throw err('INVALID_REFERENCE', `index "${seg}" out of range`);
      }
      cur = cur[idx];
    } else if (isPlainObj(cur)) {
      if (!hasOwn(cur, seg)) {
        throw err('INVALID_REFERENCE', `missing key "${seg}"`);
      }
      cur = cur[seg];
    } else {
      throw err('INVALID_REFERENCE', `cannot descend into ${typeName(cur)}`);
    }
  }
  return cur;
}

function evalCondition(c, ctx) {
  if (isPlainObj(c) && hasOwn(c, 'op')) {
    const op = c.op;
    if (op === 'all') {
      for (const sub of c.conditions) {
        if (!evalCondition(sub, ctx)) return false;
      }
      return true;
    }
    if (op === 'any') {
      for (const sub of c.conditions) {
        if (evalCondition(sub, ctx)) return true;
      }
      return false;
    }
    if (op === 'not') {
      return !evalCondition(c.condition, ctx);
    }
    const left = resolveValue(c.left, ctx);
    const right = resolveValue(c.right, ctx);
    switch (op) {
      case 'eq':
        return structuralEquals(left, right);
      case 'ne':
        return !structuralEquals(left, right);
      case 'gt':
        return cmpValues(left, right) > 0;
      case 'gte':
        return cmpValues(left, right) >= 0;
      case 'lt':
        return cmpValues(left, right) < 0;
      case 'lte':
        return cmpValues(left, right) <= 0;
      default:
        throw err('INVALID_CONDITION', `unknown condition op "${op}"`);
    }
  }
  const v = resolveValue(c, ctx);
  if (typeof v !== 'boolean') {
    throw err('INVALID_CONDITION', 'condition did not evaluate to a boolean');
  }
  return v;
}

/* ------------------------------------------------------------------ */
/* Structural validation                                               */
/* ------------------------------------------------------------------ */

function intValue(v) {
  if (typeof v === 'bigint') return v;
  if (typeof v === 'number' && Number.isFinite(v) && Number.isInteger(v)) {
    return BigInt(v);
  }
  return null;
}

function validateConditionShape(c, sid) {
  // A condition is either a condition object (has "op") or a literal/ref
  // expression whose value must be a boolean at runtime.
  if (!isPlainObj(c) || !hasOwn(c, 'op')) return;
  const op = c.op;
  if (typeof op !== 'string') {
    throw err('INVALID_WORKFLOW', 'condition "op" must be a string', sid);
  }
  if (COND_BINARY.has(op)) {
    for (const k of Object.keys(c)) {
      if (k !== 'op' && k !== 'left' && k !== 'right') {
        throw err('INVALID_WORKFLOW', `unknown condition field "${k}"`, sid);
      }
    }
    if (!hasOwn(c, 'left') || !hasOwn(c, 'right')) {
      throw err(
        'INVALID_WORKFLOW',
        `condition "${op}" requires left and right`,
        sid,
      );
    }
    return;
  }
  if (op === 'all' || op === 'any') {
    for (const k of Object.keys(c)) {
      if (k !== 'op' && k !== 'conditions') {
        throw err('INVALID_WORKFLOW', `unknown condition field "${k}"`, sid);
      }
    }
    if (!hasOwn(c, 'conditions') || !Array.isArray(c.conditions)) {
      throw err(
        'INVALID_WORKFLOW',
        `condition "${op}" requires a conditions array`,
        sid,
      );
    }
    for (const sub of c.conditions) validateConditionShape(sub, sid);
    return;
  }
  if (op === 'not') {
    for (const k of Object.keys(c)) {
      if (k !== 'op' && k !== 'condition') {
        throw err('INVALID_WORKFLOW', `unknown condition field "${k}"`, sid);
      }
    }
    if (!hasOwn(c, 'condition')) {
      throw err('INVALID_WORKFLOW', 'condition "not" requires condition', sid);
    }
    validateConditionShape(c.condition, sid);
    return;
  }
  throw err('INVALID_WORKFLOW', `unknown condition op "${op}"`, sid);
}

function validateWorkflow(doc) {
  if (!isPlainObj(doc)) {
    throw err('INVALID_WORKFLOW', 'workflow must be a JSON object');
  }
  for (const k of Object.keys(doc)) {
    if (k !== 'version' && k !== 'steps') {
      throw err('INVALID_WORKFLOW', `unknown top-level field "${k}"`);
    }
  }
  if (!hasOwn(doc, 'version')) {
    throw err('INVALID_WORKFLOW', 'missing "version"');
  }
  if (intValue(doc.version) !== 1n) {
    throw err('INVALID_WORKFLOW', '"version" must be 1');
  }
  if (!hasOwn(doc, 'steps')) {
    throw err('INVALID_WORKFLOW', 'missing "steps"');
  }
  if (!Array.isArray(doc.steps)) {
    throw err('INVALID_WORKFLOW', '"steps" must be an array');
  }

  const steps = [];
  const ids = new Set();

  for (const raw of doc.steps) {
    if (!isPlainObj(raw)) {
      throw err('INVALID_WORKFLOW', 'each step must be an object');
    }
    const sid =
      hasOwn(raw, 'id') && typeof raw.id === 'string' ? raw.id : null;

    const allowedFields = ['id', 'op', 'args', 'depends_on', 'when', 'retry'];
    for (const k of Object.keys(raw)) {
      if (!allowedFields.includes(k)) {
        throw err('INVALID_WORKFLOW', `unknown step field "${k}"`, sid);
      }
    }

    if (!hasOwn(raw, 'id') || typeof raw.id !== 'string') {
      throw err('INVALID_WORKFLOW', 'step "id" must be a string', sid);
    }
    if (!ID_RE.test(raw.id)) {
      throw err('INVALID_WORKFLOW', `invalid step id "${raw.id}"`, sid);
    }
    if (ids.has(raw.id)) {
      throw err('INVALID_WORKFLOW', `duplicate step id "${raw.id}"`, sid);
    }

    if (!hasOwn(raw, 'op') || typeof raw.op !== 'string') {
      throw err('INVALID_WORKFLOW', 'step "op" must be a string', sid);
    }
    if (!OPS.has(raw.op)) {
      throw err('INVALID_WORKFLOW', `unknown op "${raw.op}"`, sid);
    }

    let args = Object.create(null);
    if (hasOwn(raw, 'args')) {
      if (!isPlainObj(raw.args)) {
        throw err('INVALID_WORKFLOW', 'step "args" must be an object', sid);
      }
      args = raw.args;
    }

    let deps = [];
    if (hasOwn(raw, 'depends_on')) {
      if (!Array.isArray(raw.depends_on)) {
        throw err(
          'INVALID_WORKFLOW',
          'step "depends_on" must be an array',
          sid,
        );
      }
      for (const d of raw.depends_on) {
        if (typeof d !== 'string') {
          throw err(
            'INVALID_WORKFLOW',
            'step "depends_on" entries must be strings',
            sid,
          );
        }
      }
      deps = [...new Set(raw.depends_on)];
    }

    let maxAttempts = 1;
    if (hasOwn(raw, 'retry')) {
      const retry = raw.retry;
      if (!isPlainObj(retry)) {
        throw err('INVALID_WORKFLOW', 'step "retry" must be an object', sid);
      }
      for (const k of Object.keys(retry)) {
        if (k !== 'max_attempts') {
          throw err(
            'INVALID_WORKFLOW',
            `unknown retry field "${k}"`,
            sid,
          );
        }
      }
      if (hasOwn(retry, 'max_attempts')) {
        const m = intValue(retry.max_attempts);
        if (m === null || m < 1n || m > 5n) {
          throw err(
            'INVALID_WORKFLOW',
            'retry.max_attempts must be an integer in 1..5',
            sid,
          );
        }
        maxAttempts = Number(m);
      }
    }

    if (hasOwn(raw, 'when')) {
      validateConditionShape(raw.when, sid);
    }

    ids.add(raw.id);
    steps.push({
      id: raw.id,
      op: raw.op,
      args,
      deps,
      hasWhen: hasOwn(raw, 'when'),
      when: hasOwn(raw, 'when') ? raw.when : undefined,
      maxAttempts,
    });
  }

  for (const s of steps) {
    for (const d of s.deps) {
      if (!ids.has(d)) {
        throw err(
          'UNKNOWN_DEPENDENCY',
          `step "${s.id}" depends on unknown step "${d}"`,
          s.id,
        );
      }
    }
  }

  detectCycles(steps);

  return { steps };
}

function detectCycles(steps) {
  const byId = new Map(steps.map((s) => [s.id, s]));
  const state = new Map(); // 0 = unvisited, 1 = visiting, 2 = done

  // Iterative DFS so long dependency chains do not exhaust the stack.
  for (const s of steps) {
    if ((state.get(s.id) || 0) !== 0) continue;
    state.set(s.id, 1);
    const stack = [[s.id, 0]];
    while (stack.length > 0) {
      const frame = stack[stack.length - 1];
      const id = frame[0];
      const deps = byId.get(id).deps;
      if (frame[1] < deps.length) {
        const d = deps[frame[1]];
        frame[1] += 1;
        const st = state.get(d) || 0;
        if (st === 1) {
          throw err('CYCLIC_DEPENDENCY', 'dependency cycle detected');
        }
        if (st === 0) {
          state.set(d, 1);
          stack.push([d, 0]);
        }
      } else {
        state.set(id, 2);
        stack.pop();
      }
    }
  }
}

/* ------------------------------------------------------------------ */
/* Execution                                                           */
/* ------------------------------------------------------------------ */

function computeClosure(step, byId) {
  const seen = new Set();
  const stack = [...step.deps];
  while (stack.length > 0) {
    const id = stack.pop();
    if (seen.has(id)) continue;
    seen.add(id);
    for (const d of byId.get(id).deps) stack.push(d);
  }
  return seen;
}

function makeResponse(status, results, executionOrder, skipped, error) {
  return {
    status,
    results,
    execution_order: executionOrder,
    skipped,
    error,
  };
}

function execute(steps) {
  const byId = new Map(steps.map((s) => [s.id, s]));
  const closures = new Map(
    steps.map((s) => [s.id, computeClosure(s, byId)]),
  );

  const finished = new Set();
  const skippedSet = new Set();
  const results = Object.create(null);
  const executionOrder = [];
  const skippedList = [];

  const fail = (stepId, e) => {
    const code = e.code;
    return makeResponse('failed', results, executionOrder, skippedList, {
      code,
      step: stepId,
      message: e.message || code,
    });
  };

  while (finished.size < steps.length) {
    // Deterministic tie-break: earliest ready step in document order.
    let step = null;
    for (const s of steps) {
      if (finished.has(s.id)) continue;
      if (s.deps.every((d) => finished.has(d))) {
        step = s;
        break;
      }
    }
    if (step === null) {
      throw err('CYCLIC_DEPENDENCY', 'dependency cycle detected');
    }

    const ctx = {
      stepId: step.id,
      deps: closures.get(step.id),
      results,
      skipped: skippedSet,
    };

    if (step.hasWhen) {
      let run;
      try {
        run = evalCondition(step.when, ctx);
      } catch (e) {
        if (e instanceof EngineError) return fail(step.id, e);
        throw e;
      }
      if (!run) {
        results[step.id] = null;
        executionOrder.push(step.id);
        skippedList.push(step.id);
        skippedSet.add(step.id);
        finished.add(step.id);
        continue;
      }
    }

    let resolvedArgs;
    try {
      resolvedArgs = resolveValue(step.args, ctx);
    } catch (e) {
      if (e instanceof EngineError) return fail(step.id, e);
      throw e;
    }

    let finalError = null;
    for (let attempt = 1; attempt <= step.maxAttempts; attempt++) {
      try {
        const value = executeOp(step.op, resolvedArgs, attempt);
        results[step.id] = value;
        executionOrder.push(step.id);
        finished.add(step.id);
        finalError = null;
        break;
      } catch (e) {
        if (!(e instanceof EngineError)) throw e;
        if (e.code === 'TRANSIENT_FAILURE' && attempt < step.maxAttempts) {
          continue;
        }
        finalError =
          e.code === 'TRANSIENT_FAILURE'
            ? err('STEP_FAILED', e.message)
            : e;
        break;
      }
    }

    if (finalError !== null) return fail(step.id, finalError);
  }

  return makeResponse('completed', results, executionOrder, skippedList, null);
}

/* ------------------------------------------------------------------ */
/* CLI                                                                 */
/* ------------------------------------------------------------------ */

function readInput(argv) {
  let path;
  if (argv.length === 2 && argv[0] === 'run') {
    path = argv[1];
  } else if (argv.length === 1 && argv[0] !== 'run') {
    path = argv[0];
  } else {
    throw err(
      'INVALID_WORKFLOW',
      'usage: workflow run <workflow.json> | workflow run -',
    );
  }

  let text;
  try {
    text = path === '-' ? fs.readFileSync(0, 'utf8') : fs.readFileSync(path, 'utf8');
  } catch (e) {
    throw err(
      'INVALID_WORKFLOW',
      `cannot read input "${path}": ${e.message}`,
    );
  }
  return text;
}

function exitCodeFor(code) {
  if (
    code === 'INVALID_WORKFLOW' ||
    code === 'UNKNOWN_DEPENDENCY' ||
    code === 'CYCLIC_DEPENDENCY'
  ) {
    return 2;
  }
  return 1;
}

function main() {
  const argv = process.argv.slice(2);
  const text = readInput(argv);
  const doc = parseJSONText(text);
  const workflow = validateWorkflow(doc);
  return execute(workflow.steps);
}

let response;
let exitCode;

try {
  response = main();
  exitCode =
    response.status === 'completed'
      ? 0
      : exitCodeFor(response.error ? response.error.code : 'STEP_FAILED');
} catch (e) {
  if (e instanceof EngineError) {
    response = makeResponse('failed', {}, [], [], {
      code: e.code,
      step: e.step === undefined ? null : e.step,
      message: e.message || e.code,
    });
    exitCode = exitCodeFor(e.code);
  } else {
    response = makeResponse('failed', {}, [], [], {
      code: 'INTERNAL_ERROR',
      step: null,
      message: String((e && e.stack) || e),
    });
    exitCode = 3;
  }
}

fs.writeSync(1, stringify(response) + '\n');
process.exit(exitCode);
