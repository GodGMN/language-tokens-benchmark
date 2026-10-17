#!/usr/bin/env node
'use strict';

/*
 * Declarative workflow engine (SPEC.md implementation).
 *
 * Numbers are represented as BigInt when they came from an integer JSON
 * literal (and are produced by all math operations) and as JS `number` when
 * they came from a fractional/exponent JSON literal.  This lets us enforce
 * the "integer literal only" rule for numeric parameters without losing
 * precision, while still being able to carry `constant` payloads through.
 */

const fs = require('fs');

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

class WfError extends Error {
  constructor(code, step, message) {
    super(message || code);
    this.name = 'WfError';
    this.code = code;
    this.step = step === undefined ? null : step;
  }
}

function invalidWorkflow(step, msg) { return new WfError('INVALID_WORKFLOW', step, msg); }
function runtimeError(code, msg) { return new WfError(code, null, msg); }
function argError(msg) { return new WfError('INVALID_ARG', null, msg); }

// ---------------------------------------------------------------------------
// Strict JSON parser (tracks integer literals as BigInt)
// ---------------------------------------------------------------------------

function parseJSON(text) {
  let pos = 0;
  const len = text.length;

  function fail(msg) {
    throw new SyntaxError(msg + ' at offset ' + pos);
  }

  function skipWs() {
    while (pos < len) {
      const c = text.charCodeAt(pos);
      if (c === 0x20 || c === 0x09 || c === 0x0a || c === 0x0d) pos++;
      else break;
    }
  }

  function expectWord(w) {
    if (text.slice(pos, pos + w.length) !== w) fail('expected ' + w);
    pos += w.length;
  }

  function parseValue() {
    skipWs();
    if (pos >= len) fail('unexpected end of input');
    const c = text[pos];
    if (c === '{') return parseObject();
    if (c === '[') return parseArray();
    if (c === '"') return parseString();
    if (c === 't') { expectWord('true'); return true; }
    if (c === 'f') { expectWord('false'); return false; }
    if (c === 'n') { expectWord('null'); return null; }
    if (c === '-' || (c >= '0' && c <= '9')) return parseNumber();
    fail('unexpected character');
  }

  function parseObject() {
    pos++; // {
    const obj = Object.create(null);
    skipWs();
    if (pos < len && text[pos] === '}') { pos++; return obj; }
    for (;;) {
      skipWs();
      if (pos >= len || text[pos] !== '"') fail('expected object key');
      const key = parseString();
      skipWs();
      if (pos >= len || text[pos] !== ':') fail('expected ":"');
      pos++;
      const value = parseValue();
      obj[key] = value;
      skipWs();
      if (pos < len && text[pos] === ',') { pos++; continue; }
      if (pos < len && text[pos] === '}') { pos++; break; }
      fail('expected "," or "}"');
    }
    return obj;
  }

  function parseArray() {
    pos++; // [
    const arr = [];
    skipWs();
    if (pos < len && text[pos] === ']') { pos++; return arr; }
    for (;;) {
      arr.push(parseValue());
      skipWs();
      if (pos < len && text[pos] === ',') { pos++; continue; }
      if (pos < len && text[pos] === ']') { pos++; break; }
      fail('expected "," or "]"');
    }
    return arr;
  }

  function hasLoneSurrogate(s) {
    for (let i = 0; i < s.length; i++) {
      const c = s.charCodeAt(i);
      if (c >= 0xd800 && c <= 0xdbff) {
        const n = s.charCodeAt(i + 1);
        if (!(n >= 0xdc00 && n <= 0xdfff)) return true;
        i++;
      } else if (c >= 0xdc00 && c <= 0xdfff) {
        return true;
      }
    }
    return false;
  }

  function parseString() {
    const start = pos;
    pos++; // opening quote
    while (pos < len) {
      const c = text.charCodeAt(pos);
      if (c === 0x22) { // closing quote
        pos++;
        const raw = text.slice(start, pos);
        let s;
        try {
          s = JSON.parse(raw);
        } catch (e) {
          fail('invalid string literal');
        }
        if (hasLoneSurrogate(s)) fail('invalid unicode surrogate pair');
        return s;
      }
      if (c === 0x5c) { // backslash: skip escaped char
        pos += 2;
        continue;
      }
      if (c < 0x20) fail('unescaped control character in string');
      pos++;
    }
    fail('unterminated string');
  }

  function isDigit(ch) { return ch >= '0' && ch <= '9'; }

  function parseNumber() {
    const start = pos;
    if (text[pos] === '-') pos++;
    if (pos >= len) fail('invalid number');
    if (text[pos] === '0') {
      pos++;
    } else if (text[pos] >= '1' && text[pos] <= '9') {
      pos++;
      while (pos < len && isDigit(text[pos])) pos++;
    } else {
      fail('invalid number');
    }
    let isFloat = false;
    if (pos < len && text[pos] === '.') {
      isFloat = true;
      pos++;
      if (pos >= len || !isDigit(text[pos])) fail('invalid fraction');
      while (pos < len && isDigit(text[pos])) pos++;
    }
    if (pos < len && (text[pos] === 'e' || text[pos] === 'E')) {
      isFloat = true;
      pos++;
      if (pos < len && (text[pos] === '+' || text[pos] === '-')) pos++;
      if (pos >= len || !isDigit(text[pos])) fail('invalid exponent');
      while (pos < len && isDigit(text[pos])) pos++;
    }
    const tok = text.slice(start, pos);
    if (!isFloat) {
      return BigInt(tok);
    }
    const n = Number(tok);
    if (!Number.isFinite(n)) fail('number out of range');
    return n;
  }

  skipWs();
  const value = parseValue();
  skipWs();
  if (pos !== len) fail('trailing data after JSON document');
  return value;
}

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

function serialize(v) {
  if (v === null) return 'null';
  const t = typeof v;
  if (t === 'bigint') return v.toString();
  if (t === 'number') {
    if (!Number.isFinite(v)) throw new Error('cannot serialize non-finite number');
    return JSON.stringify(v);
  }
  if (t === 'string') return JSON.stringify(v);
  if (t === 'boolean') return v ? 'true' : 'false';
  if (Array.isArray(v)) {
    const parts = new Array(v.length);
    for (let i = 0; i < v.length; i++) parts[i] = serialize(v[i]);
    return '[' + parts.join(',') + ']';
  }
  if (t === 'object') {
    const keys = Object.keys(v);
    const parts = new Array(keys.length);
    for (let i = 0; i < keys.length; i++) {
      parts[i] = JSON.stringify(keys[i]) + ':' + serialize(v[keys[i]]);
    }
    return '{' + parts.join(',') + '}';
  }
  throw new Error('cannot serialize value of type ' + t);
}

// ---------------------------------------------------------------------------
// Basic helpers
// ---------------------------------------------------------------------------

const I64_MIN = -(1n << 63n);
const I64_MAX = (1n << 63n) - 1n;

function hasOwn(obj, key) {
  return Object.prototype.hasOwnProperty.call(obj, key);
}

function isPlainObject(v) {
  return v !== null && typeof v === 'object' && !Array.isArray(v);
}

function typeName(v) {
  if (v === null) return 'null';
  const t = typeof v;
  if (t === 'boolean') return 'boolean';
  if (t === 'bigint' || t === 'number') return 'number';
  if (t === 'string') return 'string';
  if (Array.isArray(v)) return 'array';
  if (t === 'object') return 'object';
  return 'null';
}

function cmpBytes(a, b) {
  return Buffer.compare(Buffer.from(a, 'utf8'), Buffer.from(b, 'utf8'));
}

function asI64(v, what) {
  if (typeof v === 'bigint' && v >= I64_MIN && v <= I64_MAX) return v;
  throw argError((what || 'argument') + ' must be an i64 integer literal');
}

function asString(v, what) {
  if (typeof v !== 'string') throw argError((what || 'argument') + ' must be a string');
  return v;
}

function asArray(v, what) {
  if (!Array.isArray(v)) throw argError((what || 'argument') + ' must be an array');
  return v;
}

function asObject(v, what) {
  if (!isPlainObject(v)) throw argError((what || 'argument') + ' must be an object');
  return v;
}

function checkArgs(args, required, optional) {
  if (!isPlainObject(args)) throw argError('args must be an object');
  const allowed = new Set(required);
  if (optional) for (const k of optional) allowed.add(k);
  for (const k of Object.keys(args)) {
    if (!allowed.has(k)) throw argError('unknown argument "' + k + '"');
  }
  for (const k of required) {
    if (!hasOwn(args, k)) throw argError('missing argument "' + k + '"');
  }
}

// ---------------------------------------------------------------------------
// Numeric helpers (i64)
// ---------------------------------------------------------------------------

function ensureI64(x) {
  if (x < I64_MIN || x > I64_MAX) throw runtimeError('ARITHMETIC_OVERFLOW', 'i64 overflow');
  return x;
}

function addI64(a, b) { return ensureI64(a + b); }
function subI64(a, b) { return ensureI64(a - b); }
function mulI64(a, b) { return ensureI64(a * b); }

function divI64(a, b) {
  if (b === 0n) throw runtimeError('DIVISION_BY_ZERO', 'division by zero');
  if (a === I64_MIN && b === -1n) throw runtimeError('ARITHMETIC_OVERFLOW', 'i64 overflow');
  return a / b; // BigInt division truncates toward zero
}

function modI64(a, b) {
  if (b === 0n) throw runtimeError('DIVISION_BY_ZERO', 'modulo by zero');
  if (a === I64_MIN && b === -1n) throw runtimeError('ARITHMETIC_OVERFLOW', 'i64 overflow');
  return a % b; // BigInt remainder follows the dividend's sign
}

function negI64(a) {
  if (a === I64_MIN) throw runtimeError('ARITHMETIC_OVERFLOW', 'i64 overflow');
  return -a;
}

function absI64(a) {
  if (a === I64_MIN) throw runtimeError('ARITHMETIC_OVERFLOW', 'i64 overflow');
  return a < 0n ? -a : a;
}

// ---------------------------------------------------------------------------
// Total order / structural equality
// ---------------------------------------------------------------------------

function typeRank(v) {
  const t = typeName(v);
  switch (t) {
    case 'number': return 0;
    case 'string': return 1;
    case 'boolean': return 2;
    case 'null': return 3;
    case 'array': return 4;
    case 'object': return 5;
  }
  return 5;
}

// Compare one BigInt against a finite double exactly.
function cmpBigAndFloat(big, d) {
  if (Number.isInteger(d)) {
    const bd = BigInt(d);
    return big < bd ? -1 : big > bd ? 1 : 0;
  }
  // Non-integral double: it cannot equal an integer.
  return big <= BigInt(Math.floor(d)) ? -1 : 1;
}

function cmpNumbers(a, b) {
  const ta = typeof a;
  const tb = typeof b;
  if (ta === 'bigint' && tb === 'bigint') return a < b ? -1 : a > b ? 1 : 0;
  if (ta === 'number' && tb === 'number') return a < b ? -1 : a > b ? 1 : 0;
  if (ta === 'bigint') return cmpBigAndFloat(a, b);
  return -cmpBigAndFloat(b, a);
}

function sortedKeys(obj) {
  return Object.keys(obj).sort(cmpBytes);
}

function cmpValues(a, b) {
  const ra = typeRank(a);
  const rb = typeRank(b);
  if (ra !== rb) return ra < rb ? -1 : 1;
  switch (ra) {
    case 0: return cmpNumbers(a, b);
    case 1: return cmpBytes(a, b);
    case 2: return a === b ? 0 : (a ? 1 : -1); // false < true
    case 3: return 0;
    case 4: {
      const n = Math.min(a.length, b.length);
      for (let i = 0; i < n; i++) {
        const c = cmpValues(a[i], b[i]);
        if (c !== 0) return c;
      }
      return a.length < b.length ? -1 : a.length > b.length ? 1 : 0;
    }
    case 5: {
      const ka = sortedKeys(a);
      const kb = sortedKeys(b);
      const n = Math.min(ka.length, kb.length);
      for (let i = 0; i < n; i++) {
        const c = cmpBytes(ka[i], kb[i]);
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
  return 0;
}

function structuralEquals(a, b) {
  const ta = typeName(a);
  const tb = typeName(b);
  if (ta !== tb) return false;
  switch (ta) {
    case 'null': return true;
    case 'boolean': return a === b;
    case 'number': return cmpNumbers(a, b) === 0;
    case 'string': return a === b;
    case 'array': {
      if (a.length !== b.length) return false;
      for (let i = 0; i < a.length; i++) {
        if (!structuralEquals(a[i], b[i])) return false;
      }
      return true;
    }
    case 'object': {
      const ka = Object.keys(a);
      const kb = Object.keys(b);
      if (ka.length !== kb.length) return false;
      for (const k of ka) {
        if (!hasOwn(b, k)) return false;
        if (!structuralEquals(a[k], b[k])) return false;
      }
      return true;
    }
  }
  return false;
}

// Canonical key used for dedup (must agree with structuralEquals).
function equalityKey(v) {
  const t = typeName(v);
  switch (t) {
    case 'null': return 'z';
    case 'boolean': return v ? 'b1' : 'b0';
    case 'number': {
      if (typeof v === 'bigint') return 'n' + v.toString();
      if (Number.isFinite(v) && Number.isInteger(v)) return 'n' + BigInt(v).toString();
      return 'n' + String(v);
    }
    case 'string': return 's' + JSON.stringify(v);
    case 'array': {
      const parts = new Array(v.length);
      for (let i = 0; i < v.length; i++) parts[i] = equalityKey(v[i]);
      return 'a[' + parts.join(',') + ']';
    }
    case 'object': {
      const keys = sortedKeys(v);
      const parts = new Array(keys.length);
      for (let i = 0; i < keys.length; i++) {
        parts[i] = JSON.stringify(keys[i]) + ':' + equalityKey(v[keys[i]]);
      }
      return 'o{' + parts.join(',') + '}';
    }
  }
  return 'z';
}

function truthy(v) {
  if (v === null) return false;
  const t = typeof v;
  if (t === 'boolean') return v;
  if (t === 'bigint') return v !== 0n;
  if (t === 'number') return v !== 0;
  if (t === 'string') return v.length > 0;
  if (Array.isArray(v)) return v.length > 0;
  if (t === 'object') return Object.keys(v).length > 0;
  return false;
}

// ---------------------------------------------------------------------------
// Transform / predicate declaration handling
// ---------------------------------------------------------------------------

const BINARY_OPS = new Set(['add', 'subtract', 'multiply', 'divide', 'mod']);
const VALUE_TRANSFORMS = new Set(['constant', 'add', 'subtract', 'multiply', 'divide', 'mod']);
const PLAIN_TRANSFORMS = new Set(['negate', 'abs', 'stringify', 'type']);
const TYPE_NAMES = new Set(['null', 'boolean', 'number', 'string', 'array', 'object']);

function isSelfMarker(v) {
  return isPlainObject(v) && Object.keys(v).length === 1 && hasOwn(v, '$self') && v.$self === true;
}

function substituteSelf(v, element) {
  if (isSelfMarker(v)) return element;
  if (Array.isArray(v)) return v.map((e) => substituteSelf(e, element));
  if (isPlainObject(v)) {
    const out = Object.create(null);
    for (const k of Object.keys(v)) out[k] = substituteSelf(v[k], element);
    return out;
  }
  return v;
}

function validateTransform(tr) {
  if (!isPlainObject(tr)) throw argError('transform must be an object');
  if (typeof tr.type !== 'string') throw argError('transform.type must be a string');
  const kind = tr.type;
  if (kind === 'op') {
    checkArgs(tr, ['type', 'op', 'args']);
    if (typeof tr.op !== 'string' || !BINARY_OPS.has(tr.op)) throw argError('unsupported transform op');
    const targs = asObject(tr.args, 'transform.args');
    checkArgs(targs, ['a', 'b']);
    return { kind: 'op', op: tr.op, args: targs };
  }
  if (VALUE_TRANSFORMS.has(kind)) {
    checkArgs(tr, ['type', 'value']);
    if (kind === 'constant') return { kind, value: tr.value };
    return { kind, value: asI64(tr.value, 'transform.value') };
  }
  if (PLAIN_TRANSFORMS.has(kind)) {
    checkArgs(tr, ['type']);
    return { kind };
  }
  throw argError('unknown transform type "' + kind + '"');
}

function applyTransform(tr, element) {
  switch (tr.kind) {
    case 'constant': return tr.value;
    case 'add': return addI64(asI64(element, 'element'), tr.value);
    case 'subtract': return subI64(asI64(element, 'element'), tr.value);
    case 'multiply': return mulI64(asI64(element, 'element'), tr.value);
    case 'divide': return divI64(asI64(element, 'element'), tr.value);
    case 'mod': return modI64(asI64(element, 'element'), tr.value);
    case 'negate': return negI64(asI64(element, 'element'));
    case 'abs': return absI64(asI64(element, 'element'));
    case 'stringify': return serialize(element);
    case 'type': return typeName(element);
    case 'op': {
      const self = substituteSelf(tr.args, element);
      const a = asI64(self.a, 'transform args.a');
      const b = asI64(self.b, 'transform args.b');
      switch (tr.op) {
        case 'add': return addI64(a, b);
        case 'subtract': return subI64(a, b);
        case 'multiply': return mulI64(a, b);
        case 'divide': return divI64(a, b);
        case 'mod': return modI64(a, b);
      }
      throw argError('unsupported transform op');
    }
  }
  throw argError('invalid transform');
}

function validatePredicate(pred) {
  if (typeof pred === 'string') {
    if (pred === 'even' || pred === 'odd' || pred === 'truthy' || pred === 'falsy') {
      return { kind: pred };
    }
    throw argError('unknown predicate "' + pred + '"');
  }
  if (!isPlainObject(pred)) throw argError('predicate must be an object');
  if (typeof pred.type !== 'string') throw argError('predicate.type must be a string');
  const kind = pred.type;

  if (kind === 'even' || kind === 'odd' || kind === 'truthy' || kind === 'falsy') {
    checkArgs(pred, ['type']);
    return { kind };
  }
  if (kind === 'gt' || kind === 'gte' || kind === 'lt' || kind === 'lte' || kind === 'eq' || kind === 'ne') {
    checkArgs(pred, ['type', 'value']);
    return { kind, value: asI64(pred.value, 'predicate.value') };
  }
  if (kind === 'type_is') {
    // The spec shows a duplicated "type" key; accept the common spellings.
    const candidates = ['value', 'name', 'type_name', 'target', 'expected', 'expected_type'];
    const allowed = ['type'].concat(candidates);
    for (const k of Object.keys(pred)) {
      if (!allowed.includes(k)) throw argError('unknown predicate field "' + k + '"');
    }
    let tval = null;
    for (const k of candidates) {
      if (hasOwn(pred, k)) { tval = pred[k]; break; }
    }
    if (tval === null) throw argError('missing predicate type name');
    if (typeof tval !== 'string' || !TYPE_NAMES.has(tval)) throw argError('invalid type name');
    return { kind, value: tval };
  }
  if (TYPE_NAMES.has(kind)) {
    // Fallback for the spec's duplicate-key spelling ({"type":"type_is","type":"number"}).
    checkArgs(pred, ['type']);
    return { kind: 'type_is', value: kind };
  }
  if (kind === 'string_contains') {
    checkArgs(pred, ['type', 'value']);
    return { kind, value: asString(pred.value, 'predicate.value') };
  }
  throw argError('unknown predicate type "' + kind + '"');
}

function applyPredicate(pred, element) {
  switch (pred.kind) {
    case 'even': {
      const n = asI64(element, 'element');
      return n % 2n === 0n;
    }
    case 'odd': {
      const n = asI64(element, 'element');
      return n % 2n !== 0n;
    }
    case 'gt': return cmpValues(element, pred.value) > 0;
    case 'gte': return cmpValues(element, pred.value) >= 0;
    case 'lt': return cmpValues(element, pred.value) < 0;
    case 'lte': return cmpValues(element, pred.value) <= 0;
    case 'eq': return structuralEquals(element, pred.value);
    case 'ne': return !structuralEquals(element, pred.value);
    case 'truthy': return truthy(element);
    case 'falsy': return !truthy(element);
    case 'type_is': return typeName(element) === pred.value;
    case 'string_contains': {
      if (typeof element !== 'string') throw argError('element must be a string');
      return element.includes(pred.value);
    }
  }
  throw argError('invalid predicate');
}

// ---------------------------------------------------------------------------
// Operations
// ---------------------------------------------------------------------------

function getI64(args, key) {
  return asI64(args[key], key);
}

function runOp(op, args, ctx) {
  switch (op) {
    // ------------------------------ math ------------------------------
    case 'add': {
      checkArgs(args, ['a', 'b']);
      return addI64(getI64(args, 'a'), getI64(args, 'b'));
    }
    case 'subtract': {
      checkArgs(args, ['a', 'b']);
      return subI64(getI64(args, 'a'), getI64(args, 'b'));
    }
    case 'multiply': {
      checkArgs(args, ['a', 'b']);
      return mulI64(getI64(args, 'a'), getI64(args, 'b'));
    }
    case 'divide': {
      checkArgs(args, ['a', 'b']);
      return divI64(getI64(args, 'a'), getI64(args, 'b'));
    }
    case 'mod': {
      checkArgs(args, ['a', 'b']);
      return modI64(getI64(args, 'a'), getI64(args, 'b'));
    }
    case 'sum': {
      checkArgs(args, ['input']);
      const input = asArray(args.input, 'input');
      let acc = 0n;
      for (const el of input) acc = addI64(acc, asI64(el, 'input element'));
      return acc;
    }
    case 'min':
    case 'max': {
      checkArgs(args, ['input']);
      const input = asArray(args.input, 'input');
      if (input.length === 0) throw runtimeError('EMPTY_ARRAY', op + ' of empty array');
      let best = asI64(input[0], 'input element');
      for (let i = 1; i < input.length; i++) {
        const n = asI64(input[i], 'input element');
        if (op === 'min' ? n < best : n > best) best = n;
      }
      return best;
    }
    case 'range': {
      checkArgs(args, ['start', 'end']);
      const start = getI64(args, 'start');
      const end = getI64(args, 'end');
      const out = [];
      for (let x = start; x <= end; x++) out.push(x);
      return out;
    }

    // ----------------------------- strings -----------------------------
    case 'concat': {
      checkArgs(args, ['a', 'b']);
      return asString(args.a, 'a') + asString(args.b, 'b');
    }
    case 'split': {
      checkArgs(args, ['input', 'sep']);
      const input = asString(args.input, 'input');
      const sep = asString(args.sep, 'sep');
      if (sep === '') return Array.from(input);
      return input.split(sep);
    }
    case 'join': {
      checkArgs(args, ['input', 'sep']);
      const input = asArray(args.input, 'input');
      const sep = asString(args.sep, 'sep');
      const parts = new Array(input.length);
      for (let i = 0; i < input.length; i++) parts[i] = asString(input[i], 'input element');
      return parts.join(sep);
    }
    case 'uppercase': {
      checkArgs(args, ['input']);
      return asString(args.input, 'input').replace(/[a-z]/g, (c) => c.toUpperCase());
    }
    case 'lowercase': {
      checkArgs(args, ['input']);
      return asString(args.input, 'input').replace(/[A-Z]/g, (c) => c.toLowerCase());
    }
    case 'trim': {
      checkArgs(args, ['input']);
      const input = asString(args.input, 'input');
      return input.replace(/^[ \t\n\r\f\v]+/, '').replace(/[ \t\n\r\f\v]+$/, '');
    }
    case 'replace': {
      checkArgs(args, ['input', 'search', 'replace']);
      const input = asString(args.input, 'input');
      const search = asString(args.search, 'search');
      const replacement = asString(args.replace, 'replace');
      if (search === '') throw argError('search must not be empty');
      return input.split(search).join(replacement);
    }
    case 'length': {
      checkArgs(args, ['input']);
      const input = asString(args.input, 'input');
      return BigInt(Array.from(input).length);
    }

    // ------------------------------ arrays -----------------------------
    case 'map': {
      checkArgs(args, ['input', 'transform']);
      const input = asArray(args.input, 'input');
      const tr = validateTransform(args.transform);
      const out = new Array(input.length);
      for (let i = 0; i < input.length; i++) out[i] = applyTransform(tr, input[i]);
      return out;
    }
    case 'filter': {
      checkArgs(args, ['input', 'predicate']);
      const input = asArray(args.input, 'input');
      const pred = validatePredicate(args.predicate);
      const out = [];
      for (const el of input) {
        if (applyPredicate(pred, el)) out.push(el);
      }
      return out;
    }
    case 'reduce': {
      checkArgs(args, ['input', 'op'], ['initial']);
      const input = asArray(args.input, 'input');
      const opName = asString(args.op, 'op');
      if (opName !== 'add' && opName !== 'subtract' && opName !== 'multiply') {
        throw argError('unsupported reduce op "' + opName + '"');
      }
      let acc;
      if (hasOwn(args, 'initial')) acc = asI64(args.initial, 'initial');
      else if (opName === 'multiply') acc = 1n;
      else acc = 0n;
      for (const el of input) {
        const n = asI64(el, 'input element');
        if (opName === 'add') acc = addI64(acc, n);
        else if (opName === 'subtract') acc = subI64(acc, n);
        else acc = mulI64(acc, n);
      }
      return acc;
    }
    case 'sort': {
      checkArgs(args, ['input']);
      const input = asArray(args.input, 'input');
      const decorated = input.map((v, i) => [v, i]);
      decorated.sort((x, y) => {
        const c = cmpValues(x[0], y[0]);
        return c !== 0 ? c : x[1] - y[1];
      });
      return decorated.map((d) => d[0]);
    }
    case 'reverse': {
      checkArgs(args, ['input']);
      const input = asArray(args.input, 'input');
      const out = new Array(input.length);
      for (let i = 0; i < input.length; i++) out[i] = input[input.length - 1 - i];
      return out;
    }
    case 'unique': {
      checkArgs(args, ['input']);
      const input = asArray(args.input, 'input');
      const seen = new Set();
      const out = [];
      for (const el of input) {
        const key = equalityKey(el);
        if (!seen.has(key)) {
          seen.add(key);
          out.push(el);
        }
      }
      return out;
    }
    case 'slice': {
      checkArgs(args, ['input', 'start'], ['end']);
      const input = asArray(args.input, 'input');
      const len = BigInt(input.length);
      const norm = (idx) => {
        if (idx < 0n) idx = len + idx;
        if (idx < 0n) return 0n;
        if (idx > len) return len;
        return idx;
      };
      const start = norm(asI64(args.start, 'start'));
      const end = hasOwn(args, 'end') ? norm(asI64(args.end, 'end')) : len;
      if (start > end) return [];
      return input.slice(Number(start), Number(end));
    }
    case 'flatten': {
      checkArgs(args, ['input']);
      const input = asArray(args.input, 'input');
      const out = [];
      for (const el of input) {
        if (Array.isArray(el)) {
          for (const sub of el) out.push(sub);
        } else {
          out.push(el);
        }
      }
      return out;
    }

    // ----------------------------- objects -----------------------------
    case 'get': {
      checkArgs(args, ['input', 'key']);
      const input = asObject(args.input, 'input');
      const key = asString(args.key, 'key');
      return hasOwn(input, key) ? input[key] : null;
    }
    case 'set': {
      checkArgs(args, ['input', 'key', 'value']);
      const input = asObject(args.input, 'input');
      const key = asString(args.key, 'key');
      const out = Object.create(null);
      for (const k of Object.keys(input)) out[k] = input[k];
      out[key] = args.value;
      return out;
    }
    case 'keys': {
      checkArgs(args, ['input']);
      const input = asObject(args.input, 'input');
      return sortedKeys(input);
    }
    case 'values': {
      checkArgs(args, ['input']);
      const input = asObject(args.input, 'input');
      return sortedKeys(input).map((k) => input[k]);
    }
    case 'merge': {
      checkArgs(args, ['a', 'b']);
      const a = asObject(args.a, 'a');
      const b = asObject(args.b, 'b');
      const out = Object.create(null);
      for (const k of Object.keys(a)) out[k] = a[k];
      for (const k of Object.keys(b)) out[k] = b[k];
      return out;
    }
    case 'pick': {
      checkArgs(args, ['input', 'keys']);
      const input = asObject(args.input, 'input');
      const keys = asArray(args.keys, 'keys');
      const wanted = new Set();
      for (const k of keys) wanted.add(asString(k, 'keys element'));
      const out = Object.create(null);
      for (const k of Array.from(wanted).sort(cmpBytes)) {
        out[k] = hasOwn(input, k) ? input[k] : null;
      }
      return out;
    }
    case 'omit': {
      checkArgs(args, ['input', 'keys']);
      const input = asObject(args.input, 'input');
      const keys = asArray(args.keys, 'keys');
      const excluded = new Set();
      for (const k of keys) excluded.add(asString(k, 'keys element'));
      const out = Object.create(null);
      for (const k of Object.keys(input)) {
        if (!excluded.has(k)) out[k] = input[k];
      }
      return out;
    }

    // ---------------------------- utilities ----------------------------
    case 'constant': {
      checkArgs(args, ['value']);
      return args.value;
    }
    case 'equals': {
      checkArgs(args, ['a', 'b']);
      return structuralEquals(args.a, args.b);
    }
    case 'compare': {
      checkArgs(args, ['a', 'b', 'op']);
      const opName = asString(args.op, 'op');
      const c = cmpValues(args.a, args.b);
      switch (opName) {
        case 'gt': return c > 0;
        case 'gte': return c >= 0;
        case 'lt': return c < 0;
        case 'lte': return c <= 0;
        default: throw argError('unsupported compare op "' + opName + '"');
      }
    }
    case 'and': {
      checkArgs(args, ['a', 'b']);
      return truthy(args.a) && truthy(args.b);
    }
    case 'or': {
      checkArgs(args, ['a', 'b']);
      return truthy(args.a) || truthy(args.b);
    }
    case 'not': {
      checkArgs(args, ['a']);
      return !truthy(args.a);
    }
    case 'type': {
      checkArgs(args, ['value']);
      return typeName(args.value);
    }

    // --------------------------- test op -------------------------------
    case 'fail_until': {
      checkArgs(args, ['failures', 'value']);
      const failures = getI64(args, 'failures');
      if (failures < 0n || failures > 10n) throw argError('failures must be in 0..10');
      if (BigInt(ctx.attempt) <= failures) {
        throw runtimeError('TRANSIENT_FAILURE', 'transient failure on attempt ' + ctx.attempt);
      }
      return args.value;
    }
  }
  throw argError('unsupported operation "' + op + '"');
}

// ---------------------------------------------------------------------------
// Structural validation
// ---------------------------------------------------------------------------

const OP_NAMES = new Set([
  'add', 'subtract', 'multiply', 'divide', 'mod', 'sum', 'min', 'max', 'range',
  'concat', 'split', 'join', 'uppercase', 'lowercase', 'trim', 'replace', 'length',
  'map', 'filter', 'reduce', 'sort', 'reverse', 'unique', 'slice', 'flatten',
  'get', 'set', 'keys', 'values', 'merge', 'pick', 'omit',
  'constant', 'equals', 'compare', 'and', 'or', 'not', 'type',
  'fail_until',
]);

const STEP_FIELDS = new Set(['id', 'op', 'args', 'depends_on', 'when', 'retry']);
const ID_RE = /^[A-Za-z_][A-Za-z0-9_]*$/;
const COMPARISON_OPS = new Set(['eq', 'ne', 'gt', 'gte', 'lt', 'lte']);

function validateCondition(c, stepId) {
  if (!isPlainObject(c)) throw invalidWorkflow(stepId, 'when must be a condition object');
  if (!hasOwn(c, 'op') || typeof c.op !== 'string') {
    throw invalidWorkflow(stepId, 'condition.op must be a string');
  }
  const op = c.op;
  if (COMPARISON_OPS.has(op)) {
    for (const k of Object.keys(c)) {
      if (k !== 'op' && k !== 'left' && k !== 'right') {
        throw invalidWorkflow(stepId, 'unknown condition field "' + k + '"');
      }
    }
    if (!hasOwn(c, 'left') || !hasOwn(c, 'right')) {
      throw invalidWorkflow(stepId, 'condition requires left and right');
    }
    return;
  }
  if (op === 'all' || op === 'any') {
    for (const k of Object.keys(c)) {
      if (k !== 'op' && k !== 'conditions') {
        throw invalidWorkflow(stepId, 'unknown condition field "' + k + '"');
      }
    }
    if (!hasOwn(c, 'conditions') || !Array.isArray(c.conditions)) {
      throw invalidWorkflow(stepId, 'condition.conditions must be an array');
    }
    for (const sub of c.conditions) validateCondition(sub, stepId);
    return;
  }
  if (op === 'not') {
    for (const k of Object.keys(c)) {
      if (k !== 'op' && k !== 'condition') {
        throw invalidWorkflow(stepId, 'unknown condition field "' + k + '"');
      }
    }
    if (!hasOwn(c, 'condition')) throw invalidWorkflow(stepId, 'not requires a condition');
    validateCondition(c.condition, stepId);
    return;
  }
  throw invalidWorkflow(stepId, 'unknown condition op "' + op + '"');
}

function validateWorkflow(doc) {
  if (!isPlainObject(doc)) throw invalidWorkflow(null, 'workflow must be an object');
  for (const k of Object.keys(doc)) {
    if (k !== 'version' && k !== 'steps') {
      throw invalidWorkflow(null, 'unknown top-level field "' + k + '"');
    }
  }
  if (!hasOwn(doc, 'version')) throw invalidWorkflow(null, 'missing version');
  if (typeof doc.version !== 'bigint' || doc.version !== 1n) {
    throw invalidWorkflow(null, 'version must be the integer literal 1');
  }
  if (!hasOwn(doc, 'steps')) throw invalidWorkflow(null, 'missing steps');
  if (!Array.isArray(doc.steps)) throw invalidWorkflow(null, 'steps must be an array');

  const steps = [];
  const seen = new Set();

  for (const raw of doc.steps) {
    if (!isPlainObject(raw)) throw invalidWorkflow(null, 'step must be an object');
    const stepId = typeof raw.id === 'string' ? raw.id : null;

    for (const k of Object.keys(raw)) {
      if (!STEP_FIELDS.has(k)) throw invalidWorkflow(stepId, 'unknown step field "' + k + '"');
    }

    if (!hasOwn(raw, 'id') || typeof raw.id !== 'string') {
      throw invalidWorkflow(stepId, 'step id must be a string');
    }
    const id = raw.id;
    if (!ID_RE.test(id)) throw invalidWorkflow(id, 'invalid step id');
    if (seen.has(id)) throw invalidWorkflow(id, 'duplicate step id');
    seen.add(id);

    if (!hasOwn(raw, 'op') || typeof raw.op !== 'string') {
      throw invalidWorkflow(id, 'step op must be a string');
    }
    if (!OP_NAMES.has(raw.op)) throw invalidWorkflow(id, 'unknown op "' + raw.op + '"');

    let args = Object.create(null);
    if (hasOwn(raw, 'args')) {
      if (!isPlainObject(raw.args)) throw invalidWorkflow(id, 'args must be an object');
      args = raw.args;
    }

    let dependsOn = [];
    if (hasOwn(raw, 'depends_on')) {
      if (!Array.isArray(raw.depends_on)) {
        throw invalidWorkflow(id, 'depends_on must be an array');
      }
      for (const dep of raw.depends_on) {
        if (typeof dep !== 'string') {
          throw invalidWorkflow(id, 'depends_on entries must be strings');
        }
        dependsOn.push(dep);
      }
    }

    let when = null;
    if (hasOwn(raw, 'when')) {
      validateCondition(raw.when, id);
      when = raw.when;
    }

    let maxAttempts = 1;
    if (hasOwn(raw, 'retry')) {
      if (!isPlainObject(raw.retry)) throw invalidWorkflow(id, 'retry must be an object');
      for (const k of Object.keys(raw.retry)) {
        if (k !== 'max_attempts') throw invalidWorkflow(id, 'unknown retry field "' + k + '"');
      }
      if (hasOwn(raw.retry, 'max_attempts')) {
        const ma = raw.retry.max_attempts;
        if (typeof ma !== 'bigint' || ma < 1n || ma > 5n) {
          throw invalidWorkflow(id, 'retry.max_attempts must be an integer 1..5');
        }
        maxAttempts = Number(ma);
      }
    }

    // Deduplicate dependencies while preserving order.
    const uniqDeps = [];
    const depSeen = new Set();
    for (const dep of dependsOn) {
      if (!depSeen.has(dep)) { depSeen.add(dep); uniqDeps.push(dep); }
    }

    steps.push({
      id,
      op: raw.op,
      args,
      dependsOn: uniqDeps,
      when,
      maxAttempts,
    });
  }

  // Unknown dependencies.
  for (const step of steps) {
    for (const dep of step.dependsOn) {
      if (!seen.has(dep)) throw new WfError('UNKNOWN_DEPENDENCY', step.id, 'unknown dependency "' + dep + '"');
    }
  }

  // Cycles.
  detectCycles(steps);

  return steps;
}

function detectCycles(steps) {
  const byId = new Map(steps.map((s) => [s.id, s]));
  const color = new Map();
  function dfs(id) {
    color.set(id, 1);
    const step = byId.get(id);
    for (const dep of step.dependsOn) {
      const c = color.get(dep) || 0;
      if (c === 1) throw new WfError('CYCLIC_DEPENDENCY', null, 'dependency cycle involving "' + dep + '"');
      if (c === 0) dfs(dep);
    }
    color.set(id, 2);
  }
  for (const step of steps) {
    if (!color.get(step.id)) dfs(step.id);
  }
}

function computeClosures(steps) {
  const byId = new Map(steps.map((s) => [s.id, s]));
  const memo = new Map();
  function collect(id) {
    if (memo.has(id)) return memo.get(id);
    const set = new Set();
    memo.set(id, set);
    for (const dep of byId.get(id).dependsOn) {
      set.add(dep);
      for (const t of collect(dep)) set.add(t);
    }
    return set;
  }
  const result = new Map();
  for (const step of steps) result.set(step.id, collect(step.id));
  return result;
}

// ---------------------------------------------------------------------------
// References and conditions
// ---------------------------------------------------------------------------

function resolveRef(ref, ctx) {
  const stepId = ctx.step.id;
  const segs = ref.split('.');
  const target = segs[0];

  if (target === stepId || !ctx.reachable.has(target)) {
    throw new WfError('INVALID_REFERENCE', stepId, 'reference "' + ref + '" is not to a dependency');
  }
  if (ctx.skipped.has(target)) {
    throw new WfError('SKIPPED_REFERENCE', stepId, 'reference "' + ref + '" targets a skipped step');
  }
  if (!hasOwn(ctx.results, target)) {
    throw new WfError('INVALID_REFERENCE', stepId, 'reference "' + ref + '" targets an unfinished step');
  }

  let value = ctx.results[target];
  for (let i = 1; i < segs.length; i++) {
    const seg = segs[i];
    if (Array.isArray(value)) {
      if (!/^[0-9]+$/.test(seg)) {
        throw new WfError('INVALID_REFERENCE', stepId, 'invalid array index "' + seg + '"');
      }
      const idx = BigInt(seg);
      if (idx >= BigInt(value.length)) {
        throw new WfError('INVALID_REFERENCE', stepId, 'array index out of bounds');
      }
      value = value[Number(idx)];
    } else if (isPlainObject(value)) {
      if (!hasOwn(value, seg)) {
        throw new WfError('INVALID_REFERENCE', stepId, 'missing key "' + seg + '"');
      }
      value = value[seg];
    } else {
      throw new WfError('INVALID_REFERENCE', stepId, 'cannot descend into non-container');
    }
  }
  return value;
}

function resolveValue(v, ctx) {
  if (Array.isArray(v)) {
    const out = new Array(v.length);
    for (let i = 0; i < v.length; i++) out[i] = resolveValue(v[i], ctx);
    return out;
  }
  if (isPlainObject(v)) {
    const keys = Object.keys(v);
    if (keys.length === 1 && keys[0] === '$ref' && typeof v.$ref === 'string') {
      return resolveRef(v.$ref, ctx);
    }
    const out = Object.create(null);
    for (const k of keys) out[k] = resolveValue(v[k], ctx);
    return out;
  }
  return v;
}

function evalCondition(c, ctx) {
  if (!isPlainObject(c)) {
    throw new WfError('INVALID_CONDITION', ctx.step.id, 'condition must be an object');
  }
  const op = c.op;
  if (op === 'all' || op === 'any') {
    const subs = Array.isArray(c.conditions) ? c.conditions : [];
    if (op === 'all') {
      for (const sub of subs) {
        if (!evalCondition(sub, ctx)) return false;
      }
      return true;
    }
    for (const sub of subs) {
      if (evalCondition(sub, ctx)) return true;
    }
    return false;
  }
  if (op === 'not') return !evalCondition(c.condition, ctx);

  const left = resolveValue(c.left, ctx);
  const right = resolveValue(c.right, ctx);
  switch (op) {
    case 'eq': return structuralEquals(left, right);
    case 'ne': return !structuralEquals(left, right);
    case 'gt': return cmpValues(left, right) > 0;
    case 'gte': return cmpValues(left, right) >= 0;
    case 'lt': return cmpValues(left, right) < 0;
    case 'lte': return cmpValues(left, right) <= 0;
  }
  throw new WfError('INVALID_CONDITION', ctx.step.id, 'unknown condition op');
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------

function execute(steps, state) {
  const closures = computeClosures(steps);
  const remaining = new Set(steps.map((s) => s.id));

  while (remaining.size > 0) {
    let step = null;
    for (const candidate of steps) {
      if (!remaining.has(candidate.id)) continue;
      let ready = true;
      for (const dep of candidate.dependsOn) {
        if (!state.done.has(dep)) { ready = false; break; }
      }
      if (ready) { step = candidate; break; }
    }
    if (step === null) {
      // Should be unreachable: cycles are validated up front.
      throw new WfError('CYCLIC_DEPENDENCY', null, 'no runnable step remains');
    }

    remaining.delete(step.id);

    const ctx = {
      step,
      reachable: closures.get(step.id),
      results: state.results,
      skipped: state.skippedSet,
    };

    // 1. Condition.
    if (step.when !== null) {
      const run = evalCondition(step.when, ctx);
      if (!run) {
        state.results[step.id] = null;
        state.order.push(step.id);
        state.skipped.push(step.id);
        state.skippedSet.add(step.id);
        state.done.add(step.id);
        continue;
      }
    }

    // 2. Resolve arguments.
    const resolvedArgs = resolveValue(step.args, ctx);

    // 3. Run, with retries for transient failures only.
    let completed = false;
    for (let attempt = 1; attempt <= step.maxAttempts; attempt++) {
      try {
        const value = runOp(step.op, resolvedArgs, { step, attempt });
        state.results[step.id] = value;
        state.order.push(step.id);
        state.done.add(step.id);
        completed = true;
        break;
      } catch (err) {
        if (err instanceof WfError) {
          if (err.step === null) err.step = step.id;
          if (err.code === 'TRANSIENT_FAILURE') {
            if (attempt === step.maxAttempts) {
              throw new WfError('STEP_FAILED', step.id, 'step failed after ' + attempt + ' attempt(s)');
            }
            continue;
          }
          throw err;
        }
        throw err;
      }
    }
    if (!completed) {
      throw new WfError('STEP_FAILED', step.id, 'step failed');
    }
  }
}

// ---------------------------------------------------------------------------
// Response construction
// ---------------------------------------------------------------------------

function emptyResponse() {
  return {
    status: 'failed',
    results: Object.create(null),
    execution_order: [],
    skipped: [],
    error: null,
  };
}

function failedResponse(state, code, step, message) {
  return {
    status: 'failed',
    results: state ? state.results : Object.create(null),
    execution_order: state ? state.order : [],
    skipped: state ? state.skipped : [],
    error: { code, step: step === undefined ? null : step, message: message || code },
  };
}

function main(argv) {
  // argv = process.argv.slice(2)
  if (argv.length !== 2 || argv[0] !== 'run') {
    return { response: failedResponse(null, 'INVALID_WORKFLOW', null, 'usage: workflow run <workflow.json>'), exit: 2 };
  }

  const source = argv[1];
  let text;
  try {
    if (source === '-') {
      text = fs.readFileSync(0, 'utf8');
    } else {
      text = fs.readFileSync(source, 'utf8');
    }
  } catch (e) {
    return { response: failedResponse(null, 'INVALID_WORKFLOW', null, 'cannot read input: ' + e.message), exit: 2 };
  }

  if (text.charCodeAt(0) === 0xfeff) text = text.slice(1);

  let doc;
  try {
    doc = parseJSON(text);
  } catch (e) {
    return { response: failedResponse(null, 'INVALID_WORKFLOW', null, 'invalid JSON: ' + e.message), exit: 2 };
  }

  let steps;
  try {
    steps = validateWorkflow(doc);
  } catch (e) {
    if (e instanceof WfError) {
      return { response: failedResponse(null, e.code, e.step, e.message), exit: 2 };
    }
    throw e;
  }

  const state = {
    results: Object.create(null),
    order: [],
    skipped: [],
    skippedSet: new Set(),
    done: new Set(),
  };

  try {
    execute(steps, state);
  } catch (e) {
    if (e instanceof WfError) {
      return { response: failedResponse(state, e.code, e.step, e.message), exit: 1 };
    }
    throw e;
  }

  return {
    response: {
      status: 'completed',
      results: state.results,
      execution_order: state.order,
      skipped: state.skipped,
      error: null,
    },
    exit: 0,
  };
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

(function entry() {
  let out;
  try {
    const result = main(process.argv.slice(2));
    out = { json: serialize(result.response), exit: result.exit };
  } catch (err) {
    // Internal error: best-effort JSON, exit 3.
    if (err && err.message) process.stderr.write('internal error: ' + err.message + '\n');
    let json;
    try {
      json = serialize({
        status: 'failed',
        results: Object.create(null),
        execution_order: [],
        skipped: [],
        error: { code: 'INTERNAL_ERROR', step: null, message: String(err && err.message ? err.message : err) },
      });
    } catch (e) {
      json = '{"status":"failed","results":{},"execution_order":[],"skipped":[],"error":{"code":"INTERNAL_ERROR","step":null,"message":"internal error"}}';
    }
    out = { json, exit: 3 };
  }
  process.stdout.write(out.json + '\n');
  process.exitCode = out.exit;
})();
