#!/usr/bin/env node
'use strict';

/*
 * Declarative workflow engine (SPEC.md, v1).
 *
 * No third-party dependencies.  JSON parsing is implemented locally so that
 * integer literals can be represented exactly as BigInt (needed for the full
 * signed 64-bit range) and so integer literals can be distinguished from
 * floating-point literals (`1` vs `1.0`).
 */

const fs = require('fs');

// ---------------------------------------------------------------------------
// i64 constants
// ---------------------------------------------------------------------------

const I64_MIN = -(1n << 63n);
const I64_MAX = (1n << 63n) - 1n;

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

class WFError extends Error {
  constructor(code, step, message) {
    super(message || code);
    this.name = 'WFError';
    this.code = code;
    this.step = step === undefined ? null : step;
  }
}

// ---------------------------------------------------------------------------
// JSON parsing (exact integers)
// ---------------------------------------------------------------------------

// Records duplicate keys for objects.  This is only used to be forgiving about
// the SPEC's `type_is` predicate example, which shows a duplicated `type` key.
const DUPLICATE_KEYS = new WeakMap();

function parseJSON(text) {
  let i = 0;
  const n = text.length;

  function err(msg) {
    throw new SyntaxError(msg + ' at offset ' + i);
  }

  function skipWs() {
    while (i < n) {
      const c = text.charCodeAt(i);
      if (c === 0x20 || c === 0x09 || c === 0x0a || c === 0x0d) i++;
      else break;
    }
  }

  function expectWord(word) {
    if (text.slice(i, i + word.length) !== word) err('invalid literal');
    i += word.length;
  }

  function parseString() {
    if (text[i] !== '"') err('expected string');
    i++;
    let out = '';
    while (i < n) {
      const c = text[i++];
      if (c === '"') return out;
      if (c === '\\') {
        if (i >= n) err('unterminated escape');
        const e = text[i++];
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
            if (i + 4 > n) err('bad unicode escape');
            const hex = text.slice(i, i + 4);
            if (!/^[0-9a-fA-F]{4}$/.test(hex)) err('bad unicode escape');
            out += String.fromCharCode(parseInt(hex, 16));
            i += 4;
            break;
          }
          default: err('bad escape');
        }
      } else {
        if (c.charCodeAt(0) < 0x20) err('control character in string');
        out += c;
      }
    }
    err('unterminated string');
  }

  function parseNumber() {
    const start = i;
    let isInteger = true;
    if (text[i] === '-') i++;
    if (i >= n) err('bad number');
    if (text[i] === '0') {
      i++;
    } else if (text[i] >= '1' && text[i] <= '9') {
      i++;
      while (i < n && text[i] >= '0' && text[i] <= '9') i++;
    } else {
      err('bad number');
    }
    if (i < n && text[i] === '.') {
      isInteger = false;
      i++;
      if (i >= n || text[i] < '0' || text[i] > '9') err('bad number fraction');
      while (i < n && text[i] >= '0' && text[i] <= '9') i++;
    }
    if (i < n && (text[i] === 'e' || text[i] === 'E')) {
      isInteger = false;
      i++;
      if (i < n && (text[i] === '+' || text[i] === '-')) i++;
      if (i >= n || text[i] < '0' || text[i] > '9') err('bad number exponent');
      while (i < n && text[i] >= '0' && text[i] <= '9') i++;
    }
    const token = text.slice(start, i);
    if (isInteger) return BigInt(token);
    return Number(token);
  }

  function parseArray() {
    i++; // [
    const out = [];
    skipWs();
    if (i < n && text[i] === ']') { i++; return out; }
    while (true) {
      out.push(parseValue());
      skipWs();
      if (i >= n) err('unterminated array');
      if (text[i] === ',') { i++; continue; }
      if (text[i] === ']') { i++; return out; }
      err('expected , or ]');
    }
  }

  function parseObject() {
    i++; // {
    const out = Object.create(null);
    const seen = Object.create(null);
    let duplicates = null;
    skipWs();
    if (i < n && text[i] === '}') { i++; return out; }
    while (true) {
      skipWs();
      const key = parseString();
      skipWs();
      if (i >= n || text[i] !== ':') err('expected :');
      i++;
      const val = parseValue();
      if (hasOwn(seen, key)) {
        if (!duplicates) duplicates = Object.create(null);
        if (!hasOwn(duplicates, key)) duplicates[key] = [seen[key]];
        duplicates[key].push(val);
      } else {
        seen[key] = val;
      }
      out[key] = val;
      skipWs();
      if (i >= n) err('unterminated object');
      if (text[i] === ',') { i++; continue; }
      if (text[i] === '}') {
        i++;
        if (duplicates) DUPLICATE_KEYS.set(out, duplicates);
        return out;
      }
      err('expected , or }');
    }
  }

  function parseValue() {
    skipWs();
    if (i >= n) err('unexpected end of input');
    const c = text[i];
    if (c === '{') return parseObject();
    if (c === '[') return parseArray();
    if (c === '"') return parseString();
    if (c === 't') { expectWord('true'); return true; }
    if (c === 'f') { expectWord('false'); return false; }
    if (c === 'n') { expectWord('null'); return null; }
    if (c === '-' || (c >= '0' && c <= '9')) return parseNumber();
    err('unexpected character');
  }

  const value = parseValue();
  skipWs();
  if (i !== n) err('trailing data');
  return value;
}

// ---------------------------------------------------------------------------
// JSON serialization (BigInt aware, compact)
// ---------------------------------------------------------------------------

function serialize(value) {
  if (value === null) return 'null';
  const t = typeof value;
  if (t === 'boolean') return value ? 'true' : 'false';
  if (t === 'bigint') return value.toString();
  if (t === 'number') {
    if (!Number.isFinite(value)) return 'null';
    return JSON.stringify(value);
  }
  if (t === 'string') return JSON.stringify(value);
  if (Array.isArray(value)) return '[' + value.map(serialize).join(',') + ']';
  if (t === 'object') {
    const keys = Object.keys(value);
    return '{' + keys.map((k) => JSON.stringify(k) + ':' + serialize(value[k])).join(',') + '}';
  }
  return 'null';
}

// ---------------------------------------------------------------------------
// Value helpers
// ---------------------------------------------------------------------------

function isObject(v) {
  return v !== null && typeof v === 'object' && !Array.isArray(v);
}

function hasOwn(o, k) {
  return Object.prototype.hasOwnProperty.call(o, k);
}

function isNumberValue(v) {
  return typeof v === 'bigint' || typeof v === 'number';
}

function typeName(v) {
  if (v === null) return 'null';
  const t = typeof v;
  if (t === 'bigint' || t === 'number') return 'number';
  if (t === 'string') return 'string';
  if (t === 'boolean') return 'boolean';
  if (Array.isArray(v)) return 'array';
  if (t === 'object') return 'object';
  return 'null';
}

function isTruthy(v) {
  if (v === null || v === false) return false;
  if (v === true) return true;
  if (typeof v === 'string') return v.length > 0;
  if (typeof v === 'bigint') return v !== 0n;
  if (typeof v === 'number') return v !== 0;
  if (Array.isArray(v)) return v.length > 0;
  if (typeof v === 'object') return Object.keys(v).length > 0;
  return Boolean(v);
}

function copyObject(o) {
  const out = Object.create(null);
  for (const k of Object.keys(o)) out[k] = o[k];
  return out;
}

// Numeric comparison between BigInt (integer literals / results) and JS
// numbers (floating-point JSON literals).
function compareNumbers(a, b) {
  const ab = typeof a === 'bigint';
  const bb = typeof b === 'bigint';
  if (ab && bb) return a < b ? -1 : a > b ? 1 : 0;
  if (!ab && !bb) return a < b ? -1 : a > b ? 1 : 0;

  if (typeof a === 'number') {
    const r = compareNumbers(b, a);
    return r === 0 ? 0 : r === -1 ? 1 : -1;
  }

  // a is BigInt, b is number
  if (b === Infinity) return -1;
  if (b === -Infinity) return 1;
  if (Number.isInteger(b)) {
    const bbv = BigInt(b);
    return a < bbv ? -1 : a > bbv ? 1 : 0;
  }
  if (b > 0) {
    const fl = BigInt(Math.floor(b));
    if (a < fl) return -1;
    if (a > fl) return 1;
    return -1; // a === floor(b) < b
  }
  const ce = BigInt(Math.ceil(b));
  if (a < ce) return -1;
  if (a > ce) return 1;
  return 1; // a === ceil(b) > b
}

function compareUtf8(a, b) {
  const r = Buffer.compare(Buffer.from(a, 'utf8'), Buffer.from(b, 'utf8'));
  return r < 0 ? -1 : r > 0 ? 1 : 0;
}

const TYPE_RANK = {
  number: 0,
  string: 1,
  boolean: 2,
  null: 3,
  array: 4,
  object: 5,
};

function compareValues(a, b) {
  const ta = typeName(a);
  const tb = typeName(b);
  if (ta !== tb) return TYPE_RANK[ta] - TYPE_RANK[tb];

  switch (ta) {
    case 'null':
      return 0;
    case 'number':
      return compareNumbers(a, b);
    case 'string':
      return compareUtf8(a, b);
    case 'boolean':
      return a === b ? 0 : a ? 1 : -1;
    case 'array': {
      const len = Math.min(a.length, b.length);
      for (let i = 0; i < len; i++) {
        const c = compareValues(a[i], b[i]);
        if (c !== 0) return c;
      }
      return a.length - b.length;
    }
    case 'object': {
      const ka = Object.keys(a).sort(compareUtf8);
      const kb = Object.keys(b).sort(compareUtf8);
      const len = Math.min(ka.length, kb.length);
      for (let i = 0; i < len; i++) {
        const c = compareUtf8(ka[i], kb[i]);
        if (c !== 0) return c;
      }
      if (ka.length !== kb.length) return ka.length - kb.length;
      for (let i = 0; i < ka.length; i++) {
        const c = compareValues(a[ka[i]], b[kb[i]]);
        if (c !== 0) return c;
      }
      return 0;
    }
  }
  return 0;
}

function deepEqual(a, b) {
  if (isNumberValue(a) && isNumberValue(b)) return compareNumbers(a, b) === 0;
  if (a === null || b === null) return a === b;
  const ta = typeof a;
  const tb = typeof b;
  if (ta !== tb) return false;
  if (ta === 'string' || ta === 'boolean') return a === b;
  if (Array.isArray(a)) {
    if (!Array.isArray(b) || a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) {
      if (!deepEqual(a[i], b[i])) return false;
    }
    return true;
  }
  if (ta === 'object') {
    if (!isObject(b)) return false;
    const ka = Object.keys(a);
    const kb = Object.keys(b);
    if (ka.length !== kb.length) return false;
    for (const k of ka) {
      if (!hasOwn(b, k) || !deepEqual(a[k], b[k])) return false;
    }
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// References
// ---------------------------------------------------------------------------

function isRefObject(v) {
  if (!isObject(v)) return false;
  const keys = Object.keys(v);
  return keys.length === 1 && keys[0] === '$ref' && typeof v.$ref === 'string';
}

function resolveRef(ref, ctx) {
  const bad = (code) => {
    throw new WFError(code, ctx.stepId, 'invalid reference "' + ref + '"');
  };

  const parts = ref.split('.');
  const target = parts[0];
  if (!ctx.byId.has(target)) bad('INVALID_REFERENCE');
  if (!ctx.ancestors.get(ctx.stepId).has(target)) bad('INVALID_REFERENCE');
  if (ctx.skippedSet.has(target)) bad('SKIPPED_REFERENCE');
  if (!hasOwn(ctx.results, target)) bad('INVALID_REFERENCE');

  let cur = ctx.results[target];
  for (let p = 1; p < parts.length; p++) {
    const seg = parts[p];
    if (Array.isArray(cur)) {
      if (!/^[0-9]+$/.test(seg)) bad('INVALID_REFERENCE');
      const idx = Number(seg);
      if (!Number.isSafeInteger(idx) || idx >= cur.length) bad('INVALID_REFERENCE');
      cur = cur[idx];
    } else if (isObject(cur)) {
      if (!hasOwn(cur, seg)) bad('INVALID_REFERENCE');
      cur = cur[seg];
    } else {
      bad('INVALID_REFERENCE');
    }
  }
  return cur;
}

function resolveValue(v, ctx) {
  if (isRefObject(v)) return resolveRef(v.$ref, ctx);
  if (Array.isArray(v)) {
    const out = new Array(v.length);
    for (let i = 0; i < v.length; i++) out[i] = resolveValue(v[i], ctx);
    return out;
  }
  if (isObject(v)) {
    const out = Object.create(null);
    for (const k of Object.keys(v)) out[k] = resolveValue(v[k], ctx);
    if (DUPLICATE_KEYS.has(v)) DUPLICATE_KEYS.set(out, DUPLICATE_KEYS.get(v));
    return out;
  }
  return v;
}

// ---------------------------------------------------------------------------
// Operation set and structural validation
// ---------------------------------------------------------------------------

const OPS = new Set([
  'add', 'subtract', 'multiply', 'divide', 'mod', 'sum', 'min', 'max', 'range',
  'concat', 'split', 'join', 'uppercase', 'lowercase', 'trim', 'replace', 'length',
  'map', 'filter', 'reduce', 'sort', 'reverse', 'unique', 'slice', 'flatten',
  'get', 'set', 'keys', 'values', 'merge', 'pick', 'omit',
  'constant', 'equals', 'compare', 'and', 'or', 'not', 'type',
  'fail_until',
]);

const BINARY_CONDITION_OPS = new Set(['eq', 'ne', 'gt', 'gte', 'lt', 'lte']);
const STEP_FIELDS = new Set(['id', 'op', 'args', 'depends_on', 'when', 'retry']);
const ID_RE = /^[A-Za-z_][A-Za-z0-9_]*$/;

function invalidWorkflow(step, message) {
  throw new WFError('INVALID_WORKFLOW', step, message);
}

function validateConditionShape(c, stepId) {
  if (!isObject(c)) invalidWorkflow(stepId, 'condition must be an object');
  const op = c.op;
  if (typeof op !== 'string') invalidWorkflow(stepId, 'condition op must be a string');

  if (BINARY_CONDITION_OPS.has(op)) {
    for (const k of Object.keys(c)) {
      if (k !== 'op' && k !== 'left' && k !== 'right') {
        invalidWorkflow(stepId, 'unknown condition field "' + k + '"');
      }
    }
    if (!hasOwn(c, 'left') || !hasOwn(c, 'right')) {
      invalidWorkflow(stepId, 'condition requires left and right');
    }
    return;
  }

  if (op === 'all' || op === 'any') {
    for (const k of Object.keys(c)) {
      if (k !== 'op' && k !== 'conditions') {
        invalidWorkflow(stepId, 'unknown condition field "' + k + '"');
      }
    }
    if (!Array.isArray(c.conditions)) {
      invalidWorkflow(stepId, 'condition requires a conditions array');
    }
    for (const sub of c.conditions) validateConditionShape(sub, stepId);
    return;
  }

  if (op === 'not') {
    for (const k of Object.keys(c)) {
      if (k !== 'op' && k !== 'condition') {
        invalidWorkflow(stepId, 'unknown condition field "' + k + '"');
      }
    }
    if (!hasOwn(c, 'condition')) invalidWorkflow(stepId, 'not requires condition');
    validateConditionShape(c.condition, stepId);
    return;
  }

  invalidWorkflow(stepId, 'unknown condition op "' + op + '"');
}

function validateWorkflow(doc) {
  if (!isObject(doc)) invalidWorkflow(null, 'workflow must be a JSON object');

  for (const k of Object.keys(doc)) {
    if (k !== 'version' && k !== 'steps') {
      invalidWorkflow(null, 'unknown top-level field "' + k + '"');
    }
  }

  if (!hasOwn(doc, 'version')) invalidWorkflow(null, 'missing version');
  if (typeof doc.version !== 'bigint' || doc.version !== 1n) {
    invalidWorkflow(null, 'version must be the integer literal 1');
  }

  if (!hasOwn(doc, 'steps')) invalidWorkflow(null, 'missing steps');
  if (!Array.isArray(doc.steps)) invalidWorkflow(null, 'steps must be an array');

  const steps = [];
  const seenIds = new Set();

  for (const raw of doc.steps) {
    const idForError = isObject(raw) && typeof raw.id === 'string' ? raw.id : null;

    if (!isObject(raw)) invalidWorkflow(idForError, 'step must be an object');

    for (const k of Object.keys(raw)) {
      if (!STEP_FIELDS.has(k)) {
        invalidWorkflow(idForError, 'unknown step field "' + k + '"');
      }
    }

    if (typeof raw.id !== 'string') invalidWorkflow(null, 'step id must be a string');
    if (!ID_RE.test(raw.id)) invalidWorkflow(raw.id, 'invalid step id');
    if (seenIds.has(raw.id)) invalidWorkflow(raw.id, 'duplicate step id');
    seenIds.add(raw.id);

    if (typeof raw.op !== 'string') invalidWorkflow(raw.id, 'step op must be a string');
    if (!OPS.has(raw.op)) invalidWorkflow(raw.id, 'unknown operation "' + raw.op + '"');

    let args = Object.create(null);
    if (hasOwn(raw, 'args')) {
      if (!isObject(raw.args)) invalidWorkflow(raw.id, 'args must be an object');
      args = raw.args;
    }

    let depends = [];
    if (hasOwn(raw, 'depends_on')) {
      if (!Array.isArray(raw.depends_on)) {
        invalidWorkflow(raw.id, 'depends_on must be an array');
      }
      for (const d of raw.depends_on) {
        if (typeof d !== 'string') {
          invalidWorkflow(raw.id, 'depends_on entries must be strings');
        }
      }
      depends = raw.depends_on.slice();
    }

    const when = hasOwn(raw, 'when') ? raw.when : null;
    if (hasOwn(raw, 'when')) validateConditionShape(when, raw.id);

    let retryMax = 1;
    if (hasOwn(raw, 'retry')) {
      const r = raw.retry;
      if (!isObject(r)) invalidWorkflow(raw.id, 'retry must be an object');
      for (const k of Object.keys(r)) {
        if (k !== 'max_attempts') {
          invalidWorkflow(raw.id, 'unknown retry field "' + k + '"');
        }
      }
      if (hasOwn(r, 'max_attempts')) {
        const m = r.max_attempts;
        if (typeof m !== 'bigint' || m < 1n || m > 5n) {
          invalidWorkflow(raw.id, 'max_attempts must be an integer in 1..5');
        }
        retryMax = Number(m);
      }
    }

    steps.push({
      id: raw.id,
      op: raw.op,
      args,
      deps: depends,
      when,
      retryMax,
    });
  }

  const byId = new Map();
  for (const s of steps) byId.set(s.id, s);

  for (const s of steps) {
    for (const d of s.deps) {
      if (!byId.has(d)) {
        throw new WFError('UNKNOWN_DEPENDENCY', s.id, 'unknown dependency "' + d + '"');
      }
    }
  }

  // Compute transitive ancestors.  This also detects cycles.
  const ancestors = new Map();
  const visiting = new Set();

  function visit(id) {
    if (ancestors.has(id)) return ancestors.get(id);
    if (visiting.has(id)) {
      throw new WFError('CYCLIC_DEPENDENCY', null, 'dependency cycle at "' + id + '"');
    }
    visiting.add(id);
    const step = byId.get(id);
    const anc = new Set();
    for (const d of step.deps) {
      anc.add(d);
      for (const a of visit(d)) anc.add(a);
    }
    visiting.delete(id);
    ancestors.set(id, anc);
    return anc;
  }

  for (const s of steps) visit(s.id);

  return { steps, byId, ancestors };
}

// ---------------------------------------------------------------------------
// Operations
// ---------------------------------------------------------------------------

function checkArgs(args, ctx, allowed, required) {
  for (const k of Object.keys(args)) {
    if (!allowed.includes(k)) {
      throw new WFError('INVALID_ARG', ctx.stepId, 'unknown argument "' + k + '"');
    }
  }
  for (const k of required) {
    if (!hasOwn(args, k)) {
      throw new WFError('INVALID_ARG', ctx.stepId, 'missing argument "' + k + '"');
    }
  }
}

function requireInt(v, ctx, name) {
  if (typeof v !== 'bigint') {
    throw new WFError('INVALID_ARG', ctx.stepId, name + ' must be an integer literal');
  }
  if (v < I64_MIN || v > I64_MAX) {
    throw new WFError('INVALID_ARG', ctx.stepId, name + ' is outside the i64 range');
  }
  return v;
}

function requireString(v, ctx, name) {
  if (typeof v !== 'string') {
    throw new WFError('INVALID_ARG', ctx.stepId, name + ' must be a string');
  }
  return v;
}

function requireArray(v, ctx, name) {
  if (!Array.isArray(v)) {
    throw new WFError('INVALID_ARG', ctx.stepId, name + ' must be an array');
  }
  return v;
}

function requireObject(v, ctx, name) {
  if (!isObject(v)) {
    throw new WFError('INVALID_ARG', ctx.stepId, name + ' must be an object');
  }
  return v;
}

function checkI64(v, ctx) {
  if (v < I64_MIN || v > I64_MAX) {
    throw new WFError('ARITHMETIC_OVERFLOW', ctx.stepId, 'arithmetic overflow');
  }
  return v;
}

function binaryMath(op, a, b, ctx) {
  switch (op) {
    case 'add':
      return checkI64(a + b, ctx);
    case 'subtract':
      return checkI64(a - b, ctx);
    case 'multiply':
      return checkI64(a * b, ctx);
    case 'divide':
      if (b === 0n) {
        throw new WFError('DIVISION_BY_ZERO', ctx.stepId, 'division by zero');
      }
      if (a === I64_MIN && b === -1n) {
        throw new WFError('ARITHMETIC_OVERFLOW', ctx.stepId, 'division overflow');
      }
      return a / b;
    case 'mod':
      if (b === 0n) {
        throw new WFError('DIVISION_BY_ZERO', ctx.stepId, 'modulo by zero');
      }
      if (a === I64_MIN && b === -1n) {
        throw new WFError('ARITHMETIC_OVERFLOW', ctx.stepId, 'modulo overflow');
      }
      return a % b;
  }
  throw new WFError('INVALID_ARG', ctx.stepId, 'unknown binary op');
}

function isSelfMarker(v) {
  return (
    isObject(v) &&
    Object.keys(v).length === 1 &&
    hasOwn(v, '$self') &&
    v.$self === true
  );
}

function replaceSelf(v, element) {
  if (isSelfMarker(v)) return element;
  if (Array.isArray(v)) return v.map((x) => replaceSelf(x, element));
  if (isObject(v)) {
    const out = Object.create(null);
    for (const k of Object.keys(v)) out[k] = replaceSelf(v[k], element);
    return out;
  }
  return v;
}

const TRANSFORM_TYPES = new Set([
  'op', 'constant', 'add', 'subtract', 'multiply', 'divide', 'mod',
  'negate', 'abs', 'stringify', 'type',
]);

// Validate the transform independently of the input elements, so that an
// invalid transform is still rejected for an empty input array.
function validateTransformShape(transform, ctx) {
  if (!isObject(transform)) {
    throw new WFError('INVALID_ARG', ctx.stepId, 'transform must be an object');
  }
  const t = transform.type;
  if (typeof t !== 'string' || !TRANSFORM_TYPES.has(t)) {
    throw new WFError('INVALID_ARG', ctx.stepId, 'unknown transform type');
  }

  if (t === 'op') {
    for (const k of Object.keys(transform)) {
      if (k !== 'type' && k !== 'op' && k !== 'args') {
        throw new WFError('INVALID_ARG', ctx.stepId, 'unknown transform field');
      }
    }
    const op = transform.op;
    if (typeof op !== 'string' || !['add', 'subtract', 'multiply', 'divide', 'mod'].includes(op)) {
      throw new WFError('INVALID_ARG', ctx.stepId, 'unsupported transform op');
    }
    if (!isObject(transform.args)) {
      throw new WFError('INVALID_ARG', ctx.stepId, 'transform args must be an object');
    }
    for (const k of Object.keys(transform.args)) {
      if (k !== 'a' && k !== 'b') {
        throw new WFError('INVALID_ARG', ctx.stepId, 'unknown transform arg');
      }
    }
    if (!hasOwn(transform.args, 'a') || !hasOwn(transform.args, 'b')) {
      throw new WFError('INVALID_ARG', ctx.stepId, 'transform requires a and b');
    }
    for (const k of ['a', 'b']) {
      const v = transform.args[k];
      if (!isSelfMarker(v)) requireInt(v, ctx, k);
    }
    return;
  }

  if (t === 'constant') {
    for (const k of Object.keys(transform)) {
      if (k !== 'type' && k !== 'value') {
        throw new WFError('INVALID_ARG', ctx.stepId, 'unknown transform field');
      }
    }
    if (!hasOwn(transform, 'value')) {
      throw new WFError('INVALID_ARG', ctx.stepId, 'constant transform requires value');
    }
    return;
  }

  if (t === 'add' || t === 'subtract' || t === 'multiply' || t === 'divide' || t === 'mod') {
    for (const k of Object.keys(transform)) {
      if (k !== 'type' && k !== 'value') {
        throw new WFError('INVALID_ARG', ctx.stepId, 'unknown transform field');
      }
    }
    if (!hasOwn(transform, 'value')) {
      throw new WFError('INVALID_ARG', ctx.stepId, t + ' transform requires value');
    }
    requireInt(transform.value, ctx, 'value');
    return;
  }

  // negate / abs / stringify / type take no extra fields
  for (const k of Object.keys(transform)) {
    if (k !== 'type') {
      throw new WFError('INVALID_ARG', ctx.stepId, 'unknown transform field');
    }
  }
}

function applyTransform(element, transform, ctx) {
  const t = transform.type;

  if (t === 'op') {
    for (const k of Object.keys(transform)) {
      if (k !== 'type' && k !== 'op' && k !== 'args') {
        throw new WFError('INVALID_ARG', ctx.stepId, 'unknown transform field');
      }
    }
    const op = transform.op;
    if (typeof op !== 'string' || !['add', 'subtract', 'multiply', 'divide', 'mod'].includes(op)) {
      throw new WFError('INVALID_ARG', ctx.stepId, 'unsupported transform op');
    }
    if (!isObject(transform.args)) {
      throw new WFError('INVALID_ARG', ctx.stepId, 'transform args must be an object');
    }
    const sub = replaceSelf(transform.args, element);
    for (const k of Object.keys(sub)) {
      if (k !== 'a' && k !== 'b') {
        throw new WFError('INVALID_ARG', ctx.stepId, 'unknown transform arg');
      }
    }
    if (!hasOwn(sub, 'a') || !hasOwn(sub, 'b')) {
      throw new WFError('INVALID_ARG', ctx.stepId, 'transform requires a and b');
    }
    const a = requireInt(sub.a, ctx, 'a');
    const b = requireInt(sub.b, ctx, 'b');
    return binaryMath(op, a, b, ctx);
  }

  if (t === 'constant') {
    for (const k of Object.keys(transform)) {
      if (k !== 'type' && k !== 'value') {
        throw new WFError('INVALID_ARG', ctx.stepId, 'unknown transform field');
      }
    }
    if (!hasOwn(transform, 'value')) {
      throw new WFError('INVALID_ARG', ctx.stepId, 'constant transform requires value');
    }
    return transform.value;
  }

  if (t === 'add' || t === 'subtract' || t === 'multiply' || t === 'divide' || t === 'mod') {
    for (const k of Object.keys(transform)) {
      if (k !== 'type' && k !== 'value') {
        throw new WFError('INVALID_ARG', ctx.stepId, 'unknown transform field');
      }
    }
    if (!hasOwn(transform, 'value')) {
      throw new WFError('INVALID_ARG', ctx.stepId, t + ' transform requires value');
    }
    const a = requireInt(element, ctx, 'element');
    const b = requireInt(transform.value, ctx, 'value');
    return binaryMath(t, a, b, ctx);
  }

  if (t === 'negate' || t === 'abs') {
    for (const k of Object.keys(transform)) {
      if (k !== 'type') {
        throw new WFError('INVALID_ARG', ctx.stepId, 'unknown transform field');
      }
    }
    const v = requireInt(element, ctx, 'element');
    if (v === I64_MIN) {
      throw new WFError('ARITHMETIC_OVERFLOW', ctx.stepId, 'unary overflow');
    }
    return t === 'negate' ? -v : v < 0n ? -v : v;
  }

  if (t === 'stringify') {
    for (const k of Object.keys(transform)) {
      if (k !== 'type') {
        throw new WFError('INVALID_ARG', ctx.stepId, 'unknown transform field');
      }
    }
    return serialize(element);
  }

  if (t === 'type') {
    for (const k of Object.keys(transform)) {
      if (k !== 'type') {
        throw new WFError('INVALID_ARG', ctx.stepId, 'unknown transform field');
      }
    }
    return typeName(element);
  }

  throw new WFError('INVALID_ARG', ctx.stepId, 'unknown transform type "' + t + '"');
}

function effectivePredicateType(predicate) {
  const duplicated = DUPLICATE_KEYS.get(predicate);
  if (duplicated && duplicated.type && duplicated.type.indexOf('type_is') !== -1) {
    // Recover the SPEC's literal (duplicate-key) type_is example.
    return 'type_is';
  }
  return predicate.type;
}

function typeIsTarget(predicate) {
  // The SPEC shows `{"type":"type_is","type":"<type>"}`, which is impossible
  // to express as a normal JSON object (duplicate key).  Be liberal: accept
  // any additional string-valued field, and also recover the duplicated-key
  // form when the parser saw it literally.
  const dups = DUPLICATE_KEYS.get(predicate);
  if (dups && dups.type) {
    const vals = dups.type.slice();
    vals.push(predicate.type);
    const marker = vals.indexOf('type_is');
    if (marker !== -1) {
      for (let i = vals.length - 1; i >= 0; i--) {
        if (i !== marker && typeof vals[i] === 'string') return vals[i];
      }
    }
  }
  for (const k of Object.keys(predicate)) {
    if (k !== 'type' && typeof predicate[k] === 'string') return predicate[k];
  }
  return null;
}

const PREDICATE_TYPES = new Set([
  'even', 'odd', 'gt', 'gte', 'lt', 'lte', 'eq', 'ne',
  'truthy', 'falsy', 'type_is', 'string_contains',
]);

// Validate the predicate independently of the input elements.
function validatePredicateShape(predicate, ctx) {
  if (!isObject(predicate)) {
    throw new WFError('INVALID_ARG', ctx.stepId, 'predicate must be an object');
  }
  const t = effectivePredicateType(predicate);
  if (typeof t !== 'string' || !PREDICATE_TYPES.has(t)) {
    throw new WFError('INVALID_ARG', ctx.stepId, 'unknown predicate type');
  }

  if (t === 'even' || t === 'odd' || t === 'truthy' || t === 'falsy') {
    for (const k of Object.keys(predicate)) {
      if (k !== 'type') {
        throw new WFError('INVALID_ARG', ctx.stepId, 'unknown predicate field');
      }
    }
    return;
  }

  if (t === 'gt' || t === 'gte' || t === 'lt' || t === 'lte' || t === 'eq' || t === 'ne') {
    for (const k of Object.keys(predicate)) {
      if (k !== 'type' && k !== 'value') {
        throw new WFError('INVALID_ARG', ctx.stepId, 'unknown predicate field');
      }
    }
    if (!hasOwn(predicate, 'value')) {
      throw new WFError('INVALID_ARG', ctx.stepId, 'predicate requires value');
    }
    requireInt(predicate.value, ctx, 'value');
    return;
  }

  if (t === 'type_is') {
    if (typeIsTarget(predicate) === null) {
      throw new WFError('INVALID_ARG', ctx.stepId, 'type_is requires a type name');
    }
    return;
  }

  // string_contains
  for (const k of Object.keys(predicate)) {
    if (k !== 'type' && k !== 'value') {
      throw new WFError('INVALID_ARG', ctx.stepId, 'unknown predicate field');
    }
  }
  if (!hasOwn(predicate, 'value')) {
    throw new WFError('INVALID_ARG', ctx.stepId, 'string_contains requires value');
  }
  requireString(predicate.value, ctx, 'value');
}

function applyPredicate(element, predicate, ctx) {
  const t = effectivePredicateType(predicate);
  if (typeof t !== 'string') {
    throw new WFError('INVALID_ARG', ctx.stepId, 'predicate type must be a string');
  }

  if (t === 'even' || t === 'odd') {
    for (const k of Object.keys(predicate)) {
      if (k !== 'type') {
        throw new WFError('INVALID_ARG', ctx.stepId, 'unknown predicate field');
      }
    }
    const v = requireInt(element, ctx, 'element');
    const r = v % 2n;
    return t === 'even' ? r === 0n : r !== 0n;
  }

  if (t === 'gt' || t === 'gte' || t === 'lt' || t === 'lte' || t === 'eq' || t === 'ne') {
    for (const k of Object.keys(predicate)) {
      if (k !== 'type' && k !== 'value') {
        throw new WFError('INVALID_ARG', ctx.stepId, 'unknown predicate field');
      }
    }
    if (!hasOwn(predicate, 'value')) {
      throw new WFError('INVALID_ARG', ctx.stepId, 'predicate requires value');
    }
    const value = requireInt(predicate.value, ctx, 'value');
    if (t === 'eq') return deepEqual(element, value);
    if (t === 'ne') return !deepEqual(element, value);
    const c = compareValues(element, value);
    if (t === 'gt') return c > 0;
    if (t === 'gte') return c >= 0;
    if (t === 'lt') return c < 0;
    return c <= 0;
  }

  if (t === 'truthy' || t === 'falsy') {
    for (const k of Object.keys(predicate)) {
      if (k !== 'type') {
        throw new WFError('INVALID_ARG', ctx.stepId, 'unknown predicate field');
      }
    }
    const truth = isTruthy(element);
    return t === 'truthy' ? truth : !truth;
  }

  if (t === 'type_is') {
    const target = typeIsTarget(predicate);
    if (target === null) {
      throw new WFError('INVALID_ARG', ctx.stepId, 'type_is requires a type name');
    }
    return typeName(element) === target;
  }

  if (t === 'string_contains') {
    for (const k of Object.keys(predicate)) {
      if (k !== 'type' && k !== 'value') {
        throw new WFError('INVALID_ARG', ctx.stepId, 'unknown predicate field');
      }
    }
    if (!hasOwn(predicate, 'value')) {
      throw new WFError('INVALID_ARG', ctx.stepId, 'string_contains requires value');
    }
    const needle = requireString(predicate.value, ctx, 'value');
    const haystack = requireString(element, ctx, 'element');
    return haystack.includes(needle);
  }

  throw new WFError('INVALID_ARG', ctx.stepId, 'unknown predicate type "' + t + '"');
}

function runOp(op, args, ctx) {
  switch (op) {
    // ----- math ---------------------------------------------------------
    case 'add':
    case 'subtract':
    case 'multiply':
    case 'divide':
    case 'mod': {
      checkArgs(args, ctx, ['a', 'b'], ['a', 'b']);
      const a = requireInt(args.a, ctx, 'a');
      const b = requireInt(args.b, ctx, 'b');
      return binaryMath(op, a, b, ctx);
    }
    case 'sum': {
      checkArgs(args, ctx, ['input'], ['input']);
      const input = requireArray(args.input, ctx, 'input');
      let total = 0n;
      for (const el of input) {
        total = checkI64(total + requireInt(el, ctx, 'input element'), ctx);
      }
      return total;
    }
    case 'min':
    case 'max': {
      checkArgs(args, ctx, ['input'], ['input']);
      const input = requireArray(args.input, ctx, 'input');
      if (input.length === 0) {
        throw new WFError('EMPTY_ARRAY', ctx.stepId, op + ' requires a non-empty array');
      }
      let best = requireInt(input[0], ctx, 'input element');
      for (let i = 1; i < input.length; i++) {
        const v = requireInt(input[i], ctx, 'input element');
        if (op === 'min' ? v < best : v > best) best = v;
      }
      return best;
    }
    case 'range': {
      checkArgs(args, ctx, ['start', 'end'], ['start', 'end']);
      const start = requireInt(args.start, ctx, 'start');
      const end = requireInt(args.end, ctx, 'end');
      const out = [];
      for (let v = start; v <= end; v++) out.push(v);
      return out;
    }

    // ----- strings ------------------------------------------------------
    case 'concat': {
      checkArgs(args, ctx, ['a', 'b'], ['a', 'b']);
      return requireString(args.a, ctx, 'a') + requireString(args.b, ctx, 'b');
    }
    case 'split': {
      checkArgs(args, ctx, ['input', 'sep'], ['input', 'sep']);
      const input = requireString(args.input, ctx, 'input');
      const sep = requireString(args.sep, ctx, 'sep');
      if (sep === '') return Array.from(input);
      return input.split(sep);
    }
    case 'join': {
      checkArgs(args, ctx, ['input', 'sep'], ['input', 'sep']);
      const input = requireArray(args.input, ctx, 'input');
      const sep = requireString(args.sep, ctx, 'sep');
      const parts = input.map((v) => requireString(v, ctx, 'input element'));
      return parts.join(sep);
    }
    case 'uppercase': {
      checkArgs(args, ctx, ['input'], ['input']);
      const input = requireString(args.input, ctx, 'input');
      return input.replace(/[a-z]/g, (c) => String.fromCharCode(c.charCodeAt(0) - 32));
    }
    case 'lowercase': {
      checkArgs(args, ctx, ['input'], ['input']);
      const input = requireString(args.input, ctx, 'input');
      return input.replace(/[A-Z]/g, (c) => String.fromCharCode(c.charCodeAt(0) + 32));
    }
    case 'trim': {
      checkArgs(args, ctx, ['input'], ['input']);
      const input = requireString(args.input, ctx, 'input');
      return input.replace(/^[ \t\n\r\f\v]+|[ \t\n\r\f\v]+$/g, '');
    }
    case 'replace': {
      checkArgs(args, ctx, ['input', 'search', 'replace'], ['input', 'search', 'replace']);
      const input = requireString(args.input, ctx, 'input');
      const search = requireString(args.search, ctx, 'search');
      const replacement = requireString(args.replace, ctx, 'replace');
      if (search === '') {
        throw new WFError('INVALID_ARG', ctx.stepId, 'search must not be empty');
      }
      return input.split(search).join(replacement);
    }
    case 'length': {
      checkArgs(args, ctx, ['input'], ['input']);
      const input = requireString(args.input, ctx, 'input');
      return BigInt(Array.from(input).length);
    }

    // ----- arrays -------------------------------------------------------
    case 'map': {
      checkArgs(args, ctx, ['input', 'transform'], ['input', 'transform']);
      const input = requireArray(args.input, ctx, 'input');
      validateTransformShape(args.transform, ctx);
      return input.map((el) => applyTransform(el, args.transform, ctx));
    }
    case 'filter': {
      checkArgs(args, ctx, ['input', 'predicate'], ['input', 'predicate']);
      const input = requireArray(args.input, ctx, 'input');
      validatePredicateShape(args.predicate, ctx);
      return input.filter((el) => applyPredicate(el, args.predicate, ctx));
    }
    case 'reduce': {
      checkArgs(args, ctx, ['input', 'op', 'initial'], ['input', 'op']);
      const input = requireArray(args.input, ctx, 'input');
      const subOp = requireString(args.op, ctx, 'op');
      if (subOp !== 'add' && subOp !== 'subtract' && subOp !== 'multiply') {
        throw new WFError('INVALID_ARG', ctx.stepId, 'unsupported reduce op');
      }
      let acc;
      if (hasOwn(args, 'initial')) {
        acc = requireInt(args.initial, ctx, 'initial');
      } else {
        acc = subOp === 'multiply' ? 1n : 0n;
      }
      for (const el of input) {
        acc = binaryMath(subOp, acc, requireInt(el, ctx, 'input element'), ctx);
      }
      return acc;
    }
    case 'sort': {
      checkArgs(args, ctx, ['input'], ['input']);
      const input = requireArray(args.input, ctx, 'input');
      const decorated = input.map((v, i) => ({ v, i }));
      decorated.sort((a, b) => {
        const c = compareValues(a.v, b.v);
        return c !== 0 ? c : a.i - b.i;
      });
      return decorated.map((d) => d.v);
    }
    case 'reverse': {
      checkArgs(args, ctx, ['input'], ['input']);
      return requireArray(args.input, ctx, 'input').slice().reverse();
    }
    case 'unique': {
      checkArgs(args, ctx, ['input'], ['input']);
      const input = requireArray(args.input, ctx, 'input');
      const out = [];
      for (const v of input) {
        let found = false;
        for (const u of out) {
          if (deepEqual(v, u)) { found = true; break; }
        }
        if (!found) out.push(v);
      }
      return out;
    }
    case 'slice': {
      checkArgs(args, ctx, ['input', 'start', 'end'], ['input', 'start']);
      const input = requireArray(args.input, ctx, 'input');
      const n = BigInt(input.length);
      let start = requireInt(args.start, ctx, 'start');
      let end = hasOwn(args, 'end') ? requireInt(args.end, ctx, 'end') : n;

      if (start < 0n) start = n + start;
      if (start < 0n) start = 0n;
      if (start > n) start = n;
      if (end < 0n) end = n + end;
      if (end < 0n) end = 0n;
      if (end > n) end = n;
      if (start > end) return [];
      return input.slice(Number(start), Number(end));
    }
    case 'flatten': {
      checkArgs(args, ctx, ['input'], ['input']);
      const input = requireArray(args.input, ctx, 'input');
      const out = [];
      for (const el of input) {
        if (Array.isArray(el)) {
          for (const x of el) out.push(x);
        } else {
          out.push(el);
        }
      }
      return out;
    }

    // ----- objects ------------------------------------------------------
    case 'get': {
      checkArgs(args, ctx, ['input', 'key'], ['input', 'key']);
      const input = requireObject(args.input, ctx, 'input');
      const key = requireString(args.key, ctx, 'key');
      return hasOwn(input, key) ? input[key] : null;
    }
    case 'set': {
      checkArgs(args, ctx, ['input', 'key', 'value'], ['input', 'key', 'value']);
      const input = requireObject(args.input, ctx, 'input');
      const key = requireString(args.key, ctx, 'key');
      const out = copyObject(input);
      out[key] = args.value;
      return out;
    }
    case 'keys': {
      checkArgs(args, ctx, ['input'], ['input']);
      const input = requireObject(args.input, ctx, 'input');
      return Object.keys(input).sort(compareUtf8);
    }
    case 'values': {
      checkArgs(args, ctx, ['input'], ['input']);
      const input = requireObject(args.input, ctx, 'input');
      return Object.keys(input).sort(compareUtf8).map((k) => input[k]);
    }
    case 'merge': {
      checkArgs(args, ctx, ['a', 'b'], ['a', 'b']);
      const a = requireObject(args.a, ctx, 'a');
      const b = requireObject(args.b, ctx, 'b');
      const out = copyObject(a);
      for (const k of Object.keys(b)) out[k] = b[k];
      return out;
    }
    case 'pick': {
      checkArgs(args, ctx, ['input', 'keys'], ['input', 'keys']);
      const input = requireObject(args.input, ctx, 'input');
      const keys = requireArray(args.keys, ctx, 'keys');
      const selected = new Set();
      for (const k of keys) selected.add(requireString(k, ctx, 'keys element'));
      const out = Object.create(null);
      for (const k of Array.from(selected).sort(compareUtf8)) {
        out[k] = hasOwn(input, k) ? input[k] : null;
      }
      return out;
    }
    case 'omit': {
      checkArgs(args, ctx, ['input', 'keys'], ['input', 'keys']);
      const input = requireObject(args.input, ctx, 'input');
      const keys = requireArray(args.keys, ctx, 'keys');
      const omitted = new Set();
      for (const k of keys) omitted.add(requireString(k, ctx, 'keys element'));
      const out = Object.create(null);
      for (const k of Object.keys(input)) {
        if (!omitted.has(k)) out[k] = input[k];
      }
      return out;
    }

    // ----- utilities ----------------------------------------------------
    case 'constant': {
      checkArgs(args, ctx, ['value'], ['value']);
      return args.value;
    }
    case 'equals': {
      checkArgs(args, ctx, ['a', 'b'], ['a', 'b']);
      return deepEqual(args.a, args.b);
    }
    case 'compare': {
      checkArgs(args, ctx, ['a', 'b', 'op'], ['a', 'b', 'op']);
      const cmpOp = requireString(args.op, ctx, 'op');
      if (cmpOp !== 'gt' && cmpOp !== 'gte' && cmpOp !== 'lt' && cmpOp !== 'lte') {
        throw new WFError('INVALID_ARG', ctx.stepId, 'unsupported compare op');
      }
      const c = compareValues(args.a, args.b);
      if (cmpOp === 'gt') return c > 0;
      if (cmpOp === 'gte') return c >= 0;
      if (cmpOp === 'lt') return c < 0;
      return c <= 0;
    }
    case 'and': {
      checkArgs(args, ctx, ['a', 'b'], ['a', 'b']);
      return isTruthy(args.a) && isTruthy(args.b);
    }
    case 'or': {
      checkArgs(args, ctx, ['a', 'b'], ['a', 'b']);
      return isTruthy(args.a) || isTruthy(args.b);
    }
    case 'not': {
      checkArgs(args, ctx, ['a'], ['a']);
      return !isTruthy(args.a);
    }
    case 'type': {
      checkArgs(args, ctx, ['value'], ['value']);
      return typeName(args.value);
    }

    // ----- test operation ----------------------------------------------
    case 'fail_until': {
      checkArgs(args, ctx, ['failures', 'value'], ['failures', 'value']);
      const failures = requireInt(args.failures, ctx, 'failures');
      if (failures < 0n || failures > 10n) {
        throw new WFError('INVALID_ARG', ctx.stepId, 'failures must be in 0..10');
      }
      const attempts = (ctx.failCounts.get(ctx.stepId) || 0) + 1;
      ctx.failCounts.set(ctx.stepId, attempts);
      if (BigInt(attempts) <= failures) {
        throw new WFError('TRANSIENT_FAILURE', ctx.stepId, 'transient failure');
      }
      return args.value;
    }
  }

  throw new WFError('INVALID_WORKFLOW', ctx.stepId, 'unknown operation "' + op + '"');
}

// ---------------------------------------------------------------------------
// Conditions
// ---------------------------------------------------------------------------

function evalCondition(cond, ctx) {
  const op = cond.op;

  if (op === 'all') {
    for (const sub of cond.conditions) {
      const v = evalCondition(sub, ctx);
      if (typeof v !== 'boolean') {
        throw new WFError('INVALID_CONDITION', ctx.stepId, 'condition is not boolean');
      }
      if (!v) return false;
    }
    return true;
  }

  if (op === 'any') {
    for (const sub of cond.conditions) {
      const v = evalCondition(sub, ctx);
      if (typeof v !== 'boolean') {
        throw new WFError('INVALID_CONDITION', ctx.stepId, 'condition is not boolean');
      }
      if (v) return true;
    }
    return false;
  }

  if (op === 'not') {
    const v = evalCondition(cond.condition, ctx);
    if (typeof v !== 'boolean') {
      throw new WFError('INVALID_CONDITION', ctx.stepId, 'condition is not boolean');
    }
    return !v;
  }

  const left = resolveValue(cond.left, ctx);
  const right = resolveValue(cond.right, ctx);

  switch (op) {
    case 'eq':
      return deepEqual(left, right);
    case 'ne':
      return !deepEqual(left, right);
    case 'gt':
      return compareValues(left, right) > 0;
    case 'gte':
      return compareValues(left, right) >= 0;
    case 'lt':
      return compareValues(left, right) < 0;
    case 'lte':
      return compareValues(left, right) <= 0;
  }

  throw new WFError('INVALID_CONDITION', ctx.stepId, 'unknown condition op');
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------

function execute(validated) {
  const { steps, byId, ancestors } = validated;

  const state = {
    results: Object.create(null),
    order: [],
    skipped: [],
  };
  const skippedSet = new Set();
  const finished = new Set();
  const failCounts = new Map();
  const remaining = steps.slice();

  try {
    while (remaining.length > 0) {
      let pick = -1;
      for (let i = 0; i < remaining.length; i++) {
        if (remaining[i].deps.every((d) => finished.has(d))) {
          pick = i;
          break;
        }
      }
      if (pick < 0) {
        // Should be impossible after cycle validation.
        throw new WFError('CYCLIC_DEPENDENCY', null, 'dependency cycle');
      }

      const step = remaining.splice(pick, 1)[0];
      const ctx = {
        stepId: step.id,
        byId,
        ancestors,
        results: state.results,
        skippedSet,
        failCounts,
      };

      if (step.when !== null) {
        const condValue = evalCondition(step.when, ctx);
        if (typeof condValue !== 'boolean') {
          throw new WFError('INVALID_CONDITION', step.id, 'condition is not boolean');
        }
        if (!condValue) {
          state.results[step.id] = null;
          state.order.push(step.id);
          state.skipped.push(step.id);
          skippedSet.add(step.id);
          finished.add(step.id);
          continue;
        }
      }

      // Reference resolution is not retriable.
      const resolvedArgs = resolveValue(step.args, ctx);

      let result;
      let succeeded = false;
      for (let attempt = 0; attempt < step.retryMax; attempt++) {
        try {
          result = runOp(step.op, resolvedArgs, ctx);
          succeeded = true;
          break;
        } catch (e) {
          if (!(e instanceof WFError)) throw e;
          if (e.code === 'TRANSIENT_FAILURE') {
            if (attempt + 1 < step.retryMax) continue;
            throw new WFError(
              'STEP_FAILED',
              step.id,
              'step failed after ' + step.retryMax + ' attempt(s): ' + e.message
            );
          }
          throw e;
        }
      }
      if (!succeeded) {
        throw new WFError('STEP_FAILED', step.id, 'step failed');
      }

      state.results[step.id] = result;
      state.order.push(step.id);
      finished.add(step.id);
    }
  } catch (e) {
    if (e instanceof WFError) {
      return { ok: false, error: e, state };
    }
    throw e;
  }

  return { ok: true, state };
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

function emptyState() {
  return { results: Object.create(null), order: [], skipped: [] };
}

function render(status, state, error) {
  return serialize({
    status,
    results: state.results,
    execution_order: state.order,
    skipped: state.skipped,
    error: error
      ? { code: error.code, step: error.step === undefined ? null : error.step, message: error.message }
      : null,
  });
}

function writeOut(text) {
  fs.writeSync(1, text + '\n');
}

function main() {
  const argv = process.argv.slice(2);

  if (argv.length !== 2 || argv[0] !== 'run') {
    const e = new WFError('INVALID_WORKFLOW', null, 'usage: workflow run <workflow.json>');
    writeOut(render('failed', emptyState(), e));
    process.exit(2);
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
    const err = new WFError('INVALID_WORKFLOW', null, 'cannot read input: ' + e.message);
    writeOut(render('failed', emptyState(), err));
    process.exit(2);
    return;
  }

  let doc;
  try {
    doc = parseJSON(text);
  } catch (e) {
    const err = new WFError('INVALID_WORKFLOW', null, 'invalid JSON: ' + e.message);
    writeOut(render('failed', emptyState(), err));
    process.exit(2);
    return;
  }

  let validated;
  try {
    validated = validateWorkflow(doc);
  } catch (e) {
    if (e instanceof WFError) {
      const exit = e.code === 'INVALID_WORKFLOW' || e.code === 'UNKNOWN_DEPENDENCY' ||
        e.code === 'CYCLIC_DEPENDENCY' ? 2 : 1;
      writeOut(render('failed', emptyState(), e));
      process.exit(exit);
      return;
    }
    const err = new WFError('INVALID_WORKFLOW', null, 'validation error: ' + e.message);
    writeOut(render('failed', emptyState(), err));
    process.exit(2);
    return;
  }

  let outcome;
  try {
    outcome = execute(validated);
  } catch (e) {
    const err = new WFError('INTERNAL_ERROR', null, String(e && e.stack ? e.stack : e));
    writeOut(render('failed', emptyState(), err));
    process.exit(3);
    return;
  }

  if (outcome.ok) {
    writeOut(render('completed', outcome.state, null));
    process.exit(0);
  }

  writeOut(render('failed', outcome.state, outcome.error));
  process.exit(1);
}

main();
