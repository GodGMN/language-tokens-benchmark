#!/usr/bin/env node
'use strict';

/*
 * Declarative workflow engine.
 *
 * CLI:
 *   node workflow.js run <workflow.json>
 *   node workflow.js run -          (document from stdin)
 *
 * Only the Node.js standard library is used.
 */

const fs = require('fs');

/* ------------------------------------------------------------------ */
/* JSON value representation                                           */
/*                                                                     */
/* Objects are represented as JObj (Map-backed) so that insertion      */
/* order is preserved and keys such as "__proto__" are safe.           */
/* Integer JSON literals are represented as BigInt so i64 arithmetic   */
/* is exact. Fraction/exponent literals become JS numbers, which lets  */
/* us distinguish `1` (valid i64 parameter) from `1.0` (invalid).      */
/* ------------------------------------------------------------------ */

class JObj {
  constructor(entries) {
    this.map = new Map();
    if (entries) {
      for (const [k, v] of entries) this.map.set(k, v);
    }
  }
  get(k) { return this.map.get(k); }
  has(k) { return this.map.has(k); }
  set(k, v) { this.map.set(k, v); return this; }
  get size() { return this.map.size; }
  keys() { return Array.from(this.map.keys()); }
  values() { return Array.from(this.map.values()); }
  entries() { return Array.from(this.map.entries()); }
}

class JsonParseError extends Error {}
class StopError extends Error {
  constructor(code, step, message) {
    super(message);
    this.code = code;
    this.step = step === undefined ? null : step;
  }
}

/* ------------------------------------------------------------------ */
/* JSON parser                                                         */
/* ------------------------------------------------------------------ */

const MAX_JSON_DEPTH = 1000;

function parseJson(text) {
  let i = 0;
  const n = text.length;

  function fail(msg) {
    throw new JsonParseError(msg + ' at offset ' + i);
  }
  function isDigit(c) { return c >= '0' && c <= '9'; }
  function skipWs() {
    while (i < n) {
      const c = text.charCodeAt(i);
      if (c === 0x20 || c === 0x09 || c === 0x0a || c === 0x0d) i++;
      else break;
    }
  }

  function parseString() {
    // assumes text[i] === '"'
    i++;
    let out = '';
    while (i < n) {
      const c = text[i++];
      if (c === '"') return out;
      if (c === '\\') {
        if (i >= n) fail('unterminated escape');
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
        if (c.charCodeAt(0) < 0x20) fail('unescaped control character');
        out += c;
      }
    }
    fail('unterminated string');
  }

  function parseNumber() {
    const start = i;
    if (text[i] === '-') i++;
    if (i >= n) fail('bad number');
    if (text[i] === '0') {
      i++;
      if (i < n && isDigit(text[i])) fail('leading zero');
    } else if (text[i] >= '1' && text[i] <= '9') {
      i++;
      while (i < n && isDigit(text[i])) i++;
    } else {
      fail('bad number');
    }
    let isIntegerLiteral = true;
    if (i < n && text[i] === '.') {
      isIntegerLiteral = false;
      i++;
      if (i >= n || !isDigit(text[i])) fail('bad fraction');
      while (i < n && isDigit(text[i])) i++;
    }
    if (i < n && (text[i] === 'e' || text[i] === 'E')) {
      isIntegerLiteral = false;
      i++;
      if (i < n && (text[i] === '+' || text[i] === '-')) i++;
      if (i >= n || !isDigit(text[i])) fail('bad exponent');
      while (i < n && isDigit(text[i])) i++;
    }
    const raw = text.slice(start, i);
    if (isIntegerLiteral) return BigInt(raw);
    return Number(raw);
  }

  function parseArray(depth) {
    i++; // [
    skipWs();
    const arr = [];
    if (i < n && text[i] === ']') { i++; return arr; }
    while (true) {
      arr.push(parseValue(depth + 1));
      skipWs();
      if (i >= n) fail('unterminated array');
      if (text[i] === ',') { i++; continue; }
      if (text[i] === ']') { i++; return arr; }
      fail('expected , or ]');
    }
  }

  function parseObject(depth) {
    i++; // {
    skipWs();
    const obj = new JObj();
    if (i < n && text[i] === '}') { i++; return obj; }
    while (true) {
      skipWs();
      if (i >= n || text[i] !== '"') fail('expected object key');
      const key = parseString();
      skipWs();
      if (i >= n || text[i] !== ':') fail('expected :');
      i++;
      const value = parseValue(depth + 1);
      obj.set(key, value);
      skipWs();
      if (i >= n) fail('unterminated object');
      if (text[i] === ',') { i++; continue; }
      if (text[i] === '}') { i++; return obj; }
      fail('expected , or }');
    }
  }

  function parseValue(depth) {
    if (depth === undefined) depth = 0;
    if (depth > MAX_JSON_DEPTH) fail('JSON nesting too deep');
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
    if (c === '-' || isDigit(c)) return parseNumber();
    fail('unexpected token');
  }

  const value = parseValue();
  skipWs();
  if (i !== n) fail('trailing content');
  return value;
}

/* ------------------------------------------------------------------ */
/* JSON serialization                                                  */
/* ------------------------------------------------------------------ */

function serialize(v) {
  if (v === null) return 'null';
  const t = typeof v;
  if (t === 'boolean') return v ? 'true' : 'false';
  if (t === 'bigint') return v.toString();
  if (t === 'number') {
    if (!Number.isFinite(v)) return 'null';
    return JSON.stringify(v);
  }
  if (t === 'string') return JSON.stringify(v);
  if (Array.isArray(v)) {
    let out = '[';
    for (let i = 0; i < v.length; i++) {
      if (i) out += ',';
      out += serialize(v[i]);
    }
    return out + ']';
  }
  if (v instanceof JObj) {
    let out = '{';
    let first = true;
    for (const [k, val] of v.entries()) {
      if (!first) out += ',';
      first = false;
      out += JSON.stringify(k) + ':' + serialize(val);
    }
    return out + '}';
  }
  return 'null';
}

/* ------------------------------------------------------------------ */
/* i64 helpers                                                         */
/* ------------------------------------------------------------------ */

const I64_MIN = -9223372036854775808n;
const I64_MAX = 9223372036854775807n;

function invalidArg(msg) { return new StopError('INVALID_ARG', null, msg); }
function overflowError(msg) { return new StopError('ARITHMETIC_OVERFLOW', null, msg || 'i64 overflow'); }

function inI64(v) { return v >= I64_MIN && v <= I64_MAX; }

function requireI64(v, name) {
  if (typeof v !== 'bigint' || !inI64(v)) {
    throw invalidArg('argument ' + name + ' must be an i64 integer literal');
  }
  return v;
}
function requireI64Value(v, name) {
  if (typeof v !== 'bigint' || !inI64(v)) {
    throw invalidArg('value ' + name + ' must be an i64 integer');
  }
  return v;
}
function checkOverflow(v) {
  if (v < I64_MIN || v > I64_MAX) throw overflowError();
  return v;
}
function requireString(v, name) {
  if (typeof v !== 'string') throw invalidArg('argument ' + name + ' must be a string');
  return v;
}
function requireArray(v, name) {
  if (!Array.isArray(v)) throw invalidArg('argument ' + name + ' must be an array');
  return v;
}
function requireObject(v, name) {
  if (!(v instanceof JObj)) throw invalidArg('argument ' + name + ' must be an object');
  return v;
}
function requireBoolOp(v, allowed, name) {
  if (typeof v !== 'string' || !allowed.includes(v)) {
    throw invalidArg('argument ' + name + ' has invalid value');
  }
  return v;
}

function checkKeys(args, allowed) {
  for (const k of args.keys()) {
    if (!allowed.includes(k)) throw invalidArg('unknown argument "' + k + '"');
  }
}
function need(args, key) {
  if (!args.has(key)) throw invalidArg('missing argument "' + key + '"');
  return args.get(key);
}
function optionalI64(args, key) {
  if (!args.has(key)) return undefined;
  return requireI64(args.get(key), key);
}

/* ------------------------------------------------------------------ */
/* Ordering / equality                                                 */
/* ------------------------------------------------------------------ */

function utf8Compare(a, b) {
  if (a === b) return 0;
  return Buffer.compare(Buffer.from(a, 'utf8'), Buffer.from(b, 'utf8'));
}

function sortedKeys(obj) {
  return obj.keys().sort(utf8Compare);
}

function typeName(v) {
  if (v === null) return 'null';
  const t = typeof v;
  if (t === 'boolean') return 'boolean';
  if (t === 'bigint' || t === 'number') return 'number';
  if (t === 'string') return 'string';
  if (Array.isArray(v)) return 'array';
  if (v instanceof JObj) return 'object';
  return 'null';
}

function typeRank(v) {
  if (typeof v === 'bigint' || typeof v === 'number') return 0;
  if (typeof v === 'string') return 1;
  if (typeof v === 'boolean') return 2;
  if (v === null) return 3;
  if (Array.isArray(v)) return 4;
  if (v instanceof JObj) return 5;
  return 3;
}

function cmpNumbers(a, b) {
  if (typeof a === 'bigint' && typeof b === 'bigint') {
    return a < b ? -1 : a > b ? 1 : 0;
  }
  // Relational operators implement mathematical comparison between
  // BigInt and Number as well.
  if (a < b) return -1;
  if (a > b) return 1;
  return 0;
}

function compareValues(a, b) {
  const ra = typeRank(a);
  const rb = typeRank(b);
  if (ra !== rb) return ra < rb ? -1 : 1;

  switch (ra) {
    case 0:
      return cmpNumbers(a, b);
    case 1:
      return utf8Compare(a, b);
    case 2:
      return a === b ? 0 : (a ? 1 : -1);
    case 3:
      return 0;
    case 4: {
      const len = Math.min(a.length, b.length);
      for (let i = 0; i < len; i++) {
        const c = compareValues(a[i], b[i]);
        if (c !== 0) return c;
      }
      return a.length < b.length ? -1 : a.length > b.length ? 1 : 0;
    }
    case 5: {
      const ka = sortedKeys(a);
      const kb = sortedKeys(b);
      const len = Math.min(ka.length, kb.length);
      for (let i = 0; i < len; i++) {
        const c = utf8Compare(ka[i], kb[i]);
        if (c !== 0) return c;
      }
      if (ka.length !== kb.length) return ka.length < kb.length ? -1 : 1;
      for (let i = 0; i < ka.length; i++) {
        const c = compareValues(a.get(ka[i]), b.get(kb[i]));
        if (c !== 0) return c;
      }
      return 0;
    }
    default:
      return 0;
  }
}

function canonical(v) {
  if (v === null) return 'N';
  const t = typeof v;
  if (t === 'boolean') return v ? 'T' : 'F';
  if (t === 'bigint') return 'd' + v.toString();
  if (t === 'number') {
    if (Number.isInteger(v)) {
      // Exact numeric value; 1 and 1.0 canonicalize identically.
      return 'd' + BigInt(v).toString();
    }
    return 'f' + String(v);
  }
  if (t === 'string') return 's' + JSON.stringify(v);
  if (Array.isArray(v)) {
    let out = 'a' + v.length + '[';
    for (const x of v) {
      const c = canonical(x);
      out += c.length + ':' + c;
    }
    return out + ']';
  }
  if (v instanceof JObj) {
    const ks = sortedKeys(v);
    let out = 'o' + ks.length + '{';
    for (const k of ks) {
      const kc = JSON.stringify(k);
      const vc = canonical(v.get(k));
      out += kc.length + ':' + kc + vc.length + ':' + vc;
    }
    return out + '}';
  }
  return '?';
}

function structuralEquals(a, b) {
  return canonical(a) === canonical(b);
}

function isTruthy(v) {
  if (v === null || v === false) return false;
  const t = typeof v;
  if (t === 'string') return v.length > 0;
  if (t === 'bigint') return v !== 0n;
  if (t === 'number') return v !== 0;
  if (Array.isArray(v)) return v.length > 0;
  if (v instanceof JObj) return v.size > 0;
  return true;
}

/* ------------------------------------------------------------------ */
/* Operation names / structural validation                             */
/* ------------------------------------------------------------------ */

const OPERATIONS = new Set([
  'add', 'subtract', 'multiply', 'divide', 'mod', 'sum', 'min', 'max', 'range',
  'concat', 'split', 'join', 'uppercase', 'lowercase', 'trim', 'replace', 'length',
  'map', 'filter', 'reduce', 'sort', 'reverse', 'unique', 'slice', 'flatten',
  'get', 'set', 'keys', 'values', 'merge', 'pick', 'omit',
  'constant', 'equals', 'compare', 'and', 'or', 'not', 'type',
  'fail_until'
]);

const STEP_FIELDS = new Set(['id', 'op', 'args', 'depends_on', 'when', 'retry']);
const ID_RE = /^[A-Za-z_][A-Za-z0-9_]*$/;
const CONDITION_OPS = new Set(['eq', 'ne', 'gt', 'gte', 'lt', 'lte', 'all', 'any', 'not']);

function validateCondition(cond, stepId) {
  if (!(cond instanceof JObj)) {
    throw new StopError('INVALID_WORKFLOW', stepId, 'condition must be an object');
  }
  const op = cond.has('op') ? cond.get('op') : undefined;
  if (typeof op !== 'string' || !CONDITION_OPS.has(op)) {
    throw new StopError('INVALID_WORKFLOW', stepId, 'condition has invalid op');
  }
  if (op === 'all' || op === 'any') {
    for (const k of cond.keys()) {
      if (k !== 'op' && k !== 'conditions') {
        throw new StopError('INVALID_WORKFLOW', stepId, 'unknown condition field');
      }
    }
    if (!cond.has('conditions') || !Array.isArray(cond.get('conditions'))) {
      throw new StopError('INVALID_WORKFLOW', stepId, 'conditions must be an array');
    }
    for (const c of cond.get('conditions')) validateCondition(c, stepId);
  } else if (op === 'not') {
    for (const k of cond.keys()) {
      if (k !== 'op' && k !== 'condition') {
        throw new StopError('INVALID_WORKFLOW', stepId, 'unknown condition field');
      }
    }
    if (!cond.has('condition')) {
      throw new StopError('INVALID_WORKFLOW', stepId, 'condition required');
    }
    validateCondition(cond.get('condition'), stepId);
  } else {
    for (const k of cond.keys()) {
      if (k !== 'op' && k !== 'left' && k !== 'right') {
        throw new StopError('INVALID_WORKFLOW', stepId, 'unknown condition field');
      }
    }
    if (!cond.has('left') || !cond.has('right')) {
      throw new StopError('INVALID_WORKFLOW', stepId, 'comparison requires left and right');
    }
  }
}

function validateWorkflow(doc) {
  if (!(doc instanceof JObj)) {
    throw new StopError('INVALID_WORKFLOW', null, 'workflow must be a JSON object');
  }

  // Unknown top-level fields.
  for (const k of doc.keys()) {
    if (k !== 'version' && k !== 'steps') {
      throw new StopError('INVALID_WORKFLOW', null, 'unknown top-level field "' + k + '"');
    }
  }

  if (!doc.has('version')) {
    throw new StopError('INVALID_WORKFLOW', null, 'missing version');
  }
  const version = doc.get('version');
  if (typeof version !== 'bigint' || version !== 1n) {
    throw new StopError('INVALID_WORKFLOW', null, 'version must be the integer literal 1');
  }

  if (!doc.has('steps')) {
    throw new StopError('INVALID_WORKFLOW', null, 'missing steps');
  }
  const rawSteps = doc.get('steps');
  if (!Array.isArray(rawSteps)) {
    throw new StopError('INVALID_WORKFLOW', null, 'steps must be an array');
  }

  const idToIndex = new Map();
  const steps = [];

  for (let idx = 0; idx < rawSteps.length; idx++) {
    const raw = rawSteps[idx];
    if (!(raw instanceof JObj)) {
      throw new StopError('INVALID_WORKFLOW', null, 'step must be an object');
    }

    const idValue = raw.has('id') ? raw.get('id') : undefined;
    const idForError = typeof idValue === 'string' ? idValue : null;

    for (const k of raw.keys()) {
      if (!STEP_FIELDS.has(k)) {
        throw new StopError('INVALID_WORKFLOW', idForError, 'unknown step field "' + k + '"');
      }
    }

    if (typeof idValue !== 'string') {
      throw new StopError('INVALID_WORKFLOW', idForError, 'step id must be a string');
    }
    const id = idValue;
    if (!ID_RE.test(id)) {
      throw new StopError('INVALID_WORKFLOW', id, 'invalid step id');
    }
    if (idToIndex.has(id)) {
      throw new StopError('INVALID_WORKFLOW', id, 'duplicate step id');
    }

    const opValue = raw.has('op') ? raw.get('op') : undefined;
    if (typeof opValue !== 'string') {
      throw new StopError('INVALID_WORKFLOW', id, 'step op must be a string');
    }
    if (!OPERATIONS.has(opValue)) {
      throw new StopError('INVALID_WORKFLOW', id, 'unknown operation "' + opValue + '"');
    }

    let args = new JObj();
    if (raw.has('args')) {
      const a = raw.get('args');
      if (!(a instanceof JObj)) {
        throw new StopError('INVALID_WORKFLOW', id, 'args must be an object');
      }
      args = a;
    }

    let deps = [];
    if (raw.has('depends_on')) {
      const d = raw.get('depends_on');
      if (!Array.isArray(d)) {
        throw new StopError('INVALID_WORKFLOW', id, 'depends_on must be an array');
      }
      for (const x of d) {
        if (typeof x !== 'string') {
          throw new StopError('INVALID_WORKFLOW', id, 'depends_on entries must be strings');
        }
        deps.push(x);
      }
    }

    let when = null;
    if (raw.has('when')) {
      when = raw.get('when');
      validateCondition(when, id);
    }

    let maxAttempts = 1;
    if (raw.has('retry')) {
      const r = raw.get('retry');
      if (!(r instanceof JObj)) {
        throw new StopError('INVALID_WORKFLOW', id, 'retry must be an object');
      }
      for (const k of r.keys()) {
        if (k !== 'max_attempts') {
          throw new StopError('INVALID_WORKFLOW', id, 'unknown retry field');
        }
      }
      if (r.has('max_attempts')) {
        const m = r.get('max_attempts');
        if (typeof m !== 'bigint' || m < 1n || m > 5n) {
          throw new StopError('INVALID_WORKFLOW', id, 'max_attempts must be 1..5');
        }
        maxAttempts = Number(m);
      }
    }

    const step = {
      id,
      op: opValue,
      args,
      deps,
      when,
      maxAttempts,
      index: idx,
      ancestors: null
    };
    idToIndex.set(id, idx);
    steps.push(step);
  }

  const stepById = new Map();
  for (const s of steps) stepById.set(s.id, s);

  // Unknown dependencies.
  for (const s of steps) {
    for (const d of s.deps) {
      if (!stepById.has(d)) {
        throw new StopError('UNKNOWN_DEPENDENCY', s.id,
          'step "' + s.id + '" depends on unknown step "' + d + '"');
      }
    }
  }

  // Cycle detection (DFS colours).
  const colour = new Map();
  function visit(id) {
    colour.set(id, 1);
    const s = stepById.get(id);
    for (const d of s.deps) {
      const c = colour.get(d) || 0;
      if (c === 1) {
        throw new StopError('CYCLIC_DEPENDENCY', null, 'dependency cycle involving "' + d + '"');
      }
      if (c === 0) visit(d);
    }
    colour.set(id, 2);
  }
  for (const s of steps) {
    if ((colour.get(s.id) || 0) === 0) visit(s.id);
  }

  // Transitive dependency sets (used for reference validity).
  function collectAncestors(id, acc) {
    const s = stepById.get(id);
    for (const d of s.deps) {
      if (!acc.has(d)) {
        acc.add(d);
        collectAncestors(d, acc);
      }
    }
  }
  for (const s of steps) {
    const acc = new Set();
    collectAncestors(s.id, acc);
    s.ancestors = acc;
  }

  return { steps, stepById };
}

/* ------------------------------------------------------------------ */
/* References                                                          */
/* ------------------------------------------------------------------ */

function resolveRef(ref, ctx) {
  const parts = ref.split('.');
  const targetId = parts[0];

  if (!ctx.stepById.has(targetId)) {
    throw new StopError('INVALID_REFERENCE', ctx.step.id,
      'reference to unknown step "' + targetId + '"');
  }
  if (!ctx.step.ancestors.has(targetId)) {
    throw new StopError('INVALID_REFERENCE', ctx.step.id,
      'step "' + ctx.step.id + '" may not reference "' + targetId + '"');
  }
  if (ctx.skipped.has(targetId)) {
    throw new StopError('SKIPPED_REFERENCE', ctx.step.id,
      'step "' + targetId + '" was skipped');
  }

  let cur = ctx.results.get(targetId);

  for (let p = 1; p < parts.length; p++) {
    const seg = parts[p];
    if (Array.isArray(cur)) {
      if (!/^[0-9]+$/.test(seg)) {
        throw new StopError('INVALID_REFERENCE', ctx.step.id,
          'invalid array index "' + seg + '"');
      }
      const idx = BigInt(seg);
      if (idx < 0n || idx >= BigInt(cur.length)) {
        throw new StopError('INVALID_REFERENCE', ctx.step.id,
          'array index out of range "' + seg + '"');
      }
      cur = cur[Number(idx)];
    } else if (cur instanceof JObj) {
      if (!cur.has(seg)) {
        throw new StopError('INVALID_REFERENCE', ctx.step.id,
          'missing key "' + seg + '"');
      }
      cur = cur.get(seg);
    } else {
      throw new StopError('INVALID_REFERENCE', ctx.step.id,
        'cannot traverse into non-container value');
    }
  }
  return cur;
}

function resolveValue(value, ctx) {
  if (value instanceof JObj) {
    if (value.size === 1 && value.has('$ref') && typeof value.get('$ref') === 'string') {
      return resolveRef(value.get('$ref'), ctx);
    }
    const out = new JObj();
    for (const [k, v] of value.entries()) {
      out.set(k, resolveValue(v, ctx));
    }
    return out;
  }
  if (Array.isArray(value)) {
    return value.map((v) => resolveValue(v, ctx));
  }
  return value;
}

/* ------------------------------------------------------------------ */
/* Conditions                                                          */
/* ------------------------------------------------------------------ */

function evalCondition(cond, ctx) {
  const op = cond.get('op');

  if (op === 'all') {
    let result = true;
    for (const c of cond.get('conditions')) {
      const r = evalCondition(c, ctx);
      if (typeof r !== 'boolean') {
        throw new StopError('INVALID_CONDITION', ctx.step.id, 'condition is not boolean');
      }
      if (!r) { result = false; break; }
    }
    return result;
  }
  if (op === 'any') {
    let result = false;
    for (const c of cond.get('conditions')) {
      const r = evalCondition(c, ctx);
      if (typeof r !== 'boolean') {
        throw new StopError('INVALID_CONDITION', ctx.step.id, 'condition is not boolean');
      }
      if (r) { result = true; break; }
    }
    return result;
  }
  if (op === 'not') {
    const r = evalCondition(cond.get('condition'), ctx);
    if (typeof r !== 'boolean') {
      throw new StopError('INVALID_CONDITION', ctx.step.id, 'condition is not boolean');
    }
    return !r;
  }

  const left = resolveValue(cond.get('left'), ctx);
  const right = resolveValue(cond.get('right'), ctx);

  switch (op) {
    case 'eq': return structuralEquals(left, right);
    case 'ne': return !structuralEquals(left, right);
    case 'gt': return compareValues(left, right) > 0;
    case 'gte': return compareValues(left, right) >= 0;
    case 'lt': return compareValues(left, right) < 0;
    case 'lte': return compareValues(left, right) <= 0;
    default:
      throw new StopError('INVALID_CONDITION', ctx.step.id, 'unknown condition op');
  }
}

/* ------------------------------------------------------------------ */
/* Operation implementation                                            */
/* ------------------------------------------------------------------ */

function binaryArith(op, a, b) {
  switch (op) {
    case 'add': return checkOverflow(a + b);
    case 'subtract': return checkOverflow(a - b);
    case 'multiply': return checkOverflow(a * b);
    case 'divide': {
      if (b === 0n) throw new StopError('DIVISION_BY_ZERO', null, 'division by zero');
      if (a === I64_MIN && b === -1n) throw overflowError();
      return checkOverflow(a / b); // BigInt division truncates toward zero
    }
    case 'mod': {
      if (b === 0n) throw new StopError('DIVISION_BY_ZERO', null, 'modulo by zero');
      if (a === I64_MIN && b === -1n) throw overflowError();
      return a % b;
    }
    default:
      throw invalidArg('unsupported arithmetic op');
  }
}

function substituteSelf(value, element) {
  if (value instanceof JObj) {
    if (value.size === 1 && value.has('$self') && value.get('$self') === true) {
      return element;
    }
    const out = new JObj();
    for (const [k, v] of value.entries()) out.set(k, substituteSelf(v, element));
    return out;
  }
  if (Array.isArray(value)) return value.map((v) => substituteSelf(v, element));
  return value;
}

const TYPE_NAMES = ['null', 'boolean', 'number', 'string', 'array', 'object'];

function validateMapTransformShape(transform) {
  if (!(transform instanceof JObj)) throw invalidArg('transform must be an object');
  const t = transform.has('type') ? transform.get('type') : undefined;
  if (typeof t !== 'string') throw invalidArg('transform type must be a string');

  switch (t) {
    case 'constant':
      checkKeys(transform, ['type', 'value']);
      need(transform, 'value');
      return;
    case 'add':
    case 'subtract':
    case 'multiply':
    case 'divide':
    case 'mod':
      checkKeys(transform, ['type', 'value']);
      requireI64(need(transform, 'value'), 'value');
      return;
    case 'negate':
    case 'abs':
    case 'stringify':
    case 'type':
      checkKeys(transform, ['type']);
      return;
    case 'op': {
      checkKeys(transform, ['type', 'op', 'args']);
      const op = requireString(need(transform, 'op'), 'op');
      if (!['add', 'subtract', 'multiply', 'divide', 'mod'].includes(op)) {
        throw invalidArg('unsupported transform op');
      }
      const rawArgs = need(transform, 'args');
      if (!(rawArgs instanceof JObj)) throw invalidArg('transform args must be an object');
      checkKeys(rawArgs, ['a', 'b']);
      const av = need(rawArgs, 'a');
      const bv = need(rawArgs, 'b');
      const isSelf = (v) => v instanceof JObj && v.size === 1 &&
        v.has('$self') && v.get('$self') === true;
      if (!isSelf(av)) requireI64(av, 'a');
      if (!isSelf(bv)) requireI64(bv, 'b');
      return;
    }
    default:
      throw invalidArg('unknown transform type "' + t + '"');
  }
}

function applyMapTransform(transform, element) {
  if (!(transform instanceof JObj)) throw invalidArg('transform must be an object');
  const t = transform.has('type') ? transform.get('type') : undefined;
  if (typeof t !== 'string') throw invalidArg('transform type must be a string');

  switch (t) {
    case 'constant': {
      checkKeys(transform, ['type', 'value']);
      return need(transform, 'value');
    }
    case 'add':
    case 'subtract':
    case 'multiply':
    case 'divide':
    case 'mod': {
      checkKeys(transform, ['type', 'value']);
      const v = requireI64(need(transform, 'value'), 'value');
      const e = requireI64Value(element, 'element');
      return binaryArith(t, e, v);
    }
    case 'negate': {
      checkKeys(transform, ['type']);
      const e = requireI64Value(element, 'element');
      if (e === I64_MIN) throw overflowError();
      return -e;
    }
    case 'abs': {
      checkKeys(transform, ['type']);
      const e = requireI64Value(element, 'element');
      if (e === I64_MIN) throw overflowError();
      return e < 0n ? -e : e;
    }
    case 'stringify': {
      checkKeys(transform, ['type']);
      return serialize(element);
    }
    case 'type': {
      checkKeys(transform, ['type']);
      return typeName(element);
    }
    case 'op': {
      // General two-number operation form:
      // {"type":"op","op":"add","args":{"a":{"$self":true},"b":2}}
      checkKeys(transform, ['type', 'op', 'args']);
      const op = requireString(need(transform, 'op'), 'op');
      const allowed = ['add', 'subtract', 'multiply', 'divide', 'mod'];
      if (!allowed.includes(op)) throw invalidArg('unsupported transform op');
      const rawArgs = need(transform, 'args');
      if (!(rawArgs instanceof JObj)) throw invalidArg('transform args must be an object');
      const args = substituteSelf(rawArgs, element);
      return runMathBinaryOp(op, args);
    }
    default:
      throw invalidArg('unknown transform type "' + t + '"');
  }
}

function runMathBinaryOp(op, args) {
  checkKeys(args, ['a', 'b']);
  const a = requireI64(need(args, 'a'), 'a');
  const b = requireI64(need(args, 'b'), 'b');
  return binaryArith(op, a, b);
}

function validateFilterPredicateShape(predicate) {
  if (!(predicate instanceof JObj)) throw invalidArg('predicate must be an object');
  const t = predicate.has('type') ? predicate.get('type') : undefined;
  if (typeof t !== 'string') throw invalidArg('predicate type must be a string');

  switch (t) {
    case 'even':
    case 'odd':
    case 'truthy':
    case 'falsy':
      checkKeys(predicate, ['type']);
      return;
    case 'gt':
    case 'gte':
    case 'lt':
    case 'lte':
    case 'eq':
    case 'ne':
      checkKeys(predicate, ['type', 'value']);
      requireI64(need(predicate, 'value'), 'value');
      return;
    case 'type_is': {
      checkKeys(predicate, ['type', 'value', 'type_name']);
      let target;
      if (predicate.has('value')) target = predicate.get('value');
      else if (predicate.has('type_name')) target = predicate.get('type_name');
      else throw invalidArg('type_is requires a type name');
      if (typeof target !== 'string' || !TYPE_NAMES.includes(target)) {
        throw invalidArg('invalid type name');
      }
      return;
    }
    case 'string_contains':
      checkKeys(predicate, ['type', 'value']);
      requireString(need(predicate, 'value'), 'value');
      return;
    default:
      if (TYPE_NAMES.includes(t)) {
        checkKeys(predicate, ['type']);
        return;
      }
      throw invalidArg('unknown predicate type "' + t + '"');
  }
}

function applyFilterPredicate(predicate, element) {
  if (!(predicate instanceof JObj)) throw invalidArg('predicate must be an object');
  const t = predicate.has('type') ? predicate.get('type') : undefined;
  if (typeof t !== 'string') throw invalidArg('predicate type must be a string');

  switch (t) {
    case 'even': {
      checkKeys(predicate, ['type']);
      const e = requireI64Value(element, 'element');
      return e % 2n === 0n;
    }
    case 'odd': {
      checkKeys(predicate, ['type']);
      const e = requireI64Value(element, 'element');
      return e % 2n !== 0n;
    }
    case 'gt':
    case 'gte':
    case 'lt':
    case 'lte': {
      checkKeys(predicate, ['type', 'value']);
      const v = requireI64(need(predicate, 'value'), 'value');
      const c = compareValues(element, v);
      if (t === 'gt') return c > 0;
      if (t === 'gte') return c >= 0;
      if (t === 'lt') return c < 0;
      return c <= 0;
    }
    case 'eq':
    case 'ne': {
      checkKeys(predicate, ['type', 'value']);
      const v = requireI64(need(predicate, 'value'), 'value');
      const eq = structuralEquals(element, v);
      return t === 'eq' ? eq : !eq;
    }
    case 'truthy': {
      checkKeys(predicate, ['type']);
      return isTruthy(element);
    }
    case 'falsy': {
      checkKeys(predicate, ['type']);
      return !isTruthy(element);
    }
    case 'type_is': {
      // The spec sketches {"type":"type_is","type":"<type>"}, which is
      // impossible to express as a normal JSON object (duplicate key), so
      // accept the natural spellings as well.
      const allowed = ['type', 'value', 'type_name'];
      checkKeys(predicate, allowed);
      let target;
      if (predicate.has('value')) target = predicate.get('value');
      else if (predicate.has('type_name')) target = predicate.get('type_name');
      else throw invalidArg('type_is requires a type name');
      if (typeof target !== 'string' ||
          !['null', 'boolean', 'number', 'string', 'array', 'object'].includes(target)) {
        throw invalidArg('invalid type name');
      }
      return typeName(element) === target;
    }
    case 'string_contains': {
      checkKeys(predicate, ['type', 'value']);
      const v = requireString(need(predicate, 'value'), 'value');
      if (typeof element !== 'string') throw invalidArg('element must be a string');
      return element.includes(v);
    }
    default: {
      // Tolerate the literal duplicate-key spelling: {"type":"<typename>"}
      if (['null', 'boolean', 'number', 'string', 'array', 'object'].includes(t)) {
        checkKeys(predicate, ['type']);
        return typeName(element) === t;
      }
      throw invalidArg('unknown predicate type "' + t + '"');
    }
  }
}

function runOp(op, args, attempt) {
  switch (op) {
    /* ---------------- math ---------------- */
    case 'add':
    case 'subtract':
    case 'multiply':
    case 'divide':
    case 'mod':
      return runMathBinaryOp(op, args);

    case 'sum': {
      checkKeys(args, ['input']);
      const arr = requireArray(need(args, 'input'), 'input');
      let total = 0n;
      for (const x of arr) total += requireI64Value(x, 'array element');
      return checkOverflow(total);
    }

    case 'min': {
      checkKeys(args, ['input']);
      const arr = requireArray(need(args, 'input'), 'input');
      if (arr.length === 0) throw new StopError('EMPTY_ARRAY', null, 'min of empty array');
      let best = requireI64Value(arr[0], 'array element');
      for (let i = 1; i < arr.length; i++) {
        const x = requireI64Value(arr[i], 'array element');
        if (x < best) best = x;
      }
      return best;
    }

    case 'max': {
      checkKeys(args, ['input']);
      const arr = requireArray(need(args, 'input'), 'input');
      if (arr.length === 0) throw new StopError('EMPTY_ARRAY', null, 'max of empty array');
      let best = requireI64Value(arr[0], 'array element');
      for (let i = 1; i < arr.length; i++) {
        const x = requireI64Value(arr[i], 'array element');
        if (x > best) best = x;
      }
      return best;
    }

    case 'range': {
      checkKeys(args, ['start', 'end']);
      const start = requireI64(need(args, 'start'), 'start');
      const end = requireI64(need(args, 'end'), 'end');
      const out = [];
      if (start > end) return out;
      for (let x = start; x <= end; x++) out.push(x);
      return out;
    }

    /* ---------------- strings ---------------- */
    case 'concat': {
      checkKeys(args, ['a', 'b']);
      return requireString(need(args, 'a'), 'a') + requireString(need(args, 'b'), 'b');
    }

    case 'split': {
      checkKeys(args, ['input', 'sep']);
      const input = requireString(need(args, 'input'), 'input');
      const sep = requireString(need(args, 'sep'), 'sep');
      if (sep === '') return Array.from(input);
      return input.split(sep);
    }

    case 'join': {
      checkKeys(args, ['input', 'sep']);
      const arr = requireArray(need(args, 'input'), 'input');
      const sep = requireString(need(args, 'sep'), 'sep');
      const parts = [];
      for (const x of arr) parts.push(requireString(x, 'array element'));
      return parts.join(sep);
    }

    case 'uppercase': {
      checkKeys(args, ['input']);
      const input = requireString(need(args, 'input'), 'input');
      return input.replace(/[a-z]/g, (c) => c.toUpperCase());
    }

    case 'lowercase': {
      checkKeys(args, ['input']);
      const input = requireString(need(args, 'input'), 'input');
      return input.replace(/[A-Z]/g, (c) => c.toLowerCase());
    }

    case 'trim': {
      checkKeys(args, ['input']);
      const input = requireString(need(args, 'input'), 'input');
      return input.replace(/^[ \t\n\r\f\v]+/, '').replace(/[ \t\n\r\f\v]+$/, '');
    }

    case 'replace': {
      checkKeys(args, ['input', 'search', 'replace']);
      const input = requireString(need(args, 'input'), 'input');
      const search = requireString(need(args, 'search'), 'search');
      const repl = requireString(need(args, 'replace'), 'replace');
      if (search === '') throw invalidArg('search must not be empty');
      return input.split(search).join(repl);
    }

    case 'length': {
      checkKeys(args, ['input']);
      const input = requireString(need(args, 'input'), 'input');
      return BigInt(Array.from(input).length);
    }

    /* ---------------- arrays ---------------- */
    case 'map': {
      checkKeys(args, ['input', 'transform']);
      const arr = requireArray(need(args, 'input'), 'input');
      const transform = need(args, 'transform');
      validateMapTransformShape(transform);
      return arr.map((el) => applyMapTransform(transform, el));
    }

    case 'filter': {
      checkKeys(args, ['input', 'predicate']);
      const arr = requireArray(need(args, 'input'), 'input');
      const pred = need(args, 'predicate');
      validateFilterPredicateShape(pred);
      return arr.filter((el) => applyFilterPredicate(pred, el));
    }

    case 'reduce': {
      checkKeys(args, ['input', 'op', 'initial']);
      const arr = requireArray(need(args, 'input'), 'input');
      const rop = requireBoolOp(need(args, 'op'), ['add', 'subtract', 'multiply'], 'op');
      let acc;
      if (args.has('initial')) acc = requireI64(args.get('initial'), 'initial');
      else acc = rop === 'multiply' ? 1n : 0n;
      for (const x of arr) {
        const v = requireI64Value(x, 'array element');
        acc = binaryArith(rop, acc, v);
      }
      return acc;
    }

    case 'sort': {
      checkKeys(args, ['input']);
      const arr = requireArray(need(args, 'input'), 'input');
      return arr.slice().sort(compareValues);
    }

    case 'reverse': {
      checkKeys(args, ['input']);
      const arr = requireArray(need(args, 'input'), 'input');
      return arr.slice().reverse();
    }

    case 'unique': {
      checkKeys(args, ['input']);
      const arr = requireArray(need(args, 'input'), 'input');
      const seen = new Set();
      const out = [];
      for (const x of arr) {
        const c = canonical(x);
        if (!seen.has(c)) {
          seen.add(c);
          out.push(x);
        }
      }
      return out;
    }

    case 'slice': {
      checkKeys(args, ['input', 'start', 'end']);
      const arr = requireArray(need(args, 'input'), 'input');
      const start = requireI64(need(args, 'start'), 'start');
      const end = optionalI64(args, 'end');
      const N = BigInt(arr.length);
      let s = start < 0n ? N + start : start;
      if (s < 0n) s = 0n;
      if (s > N) s = N;
      let e;
      if (end === undefined) e = N;
      else {
        e = end < 0n ? N + end : end;
        if (e < 0n) e = 0n;
        if (e > N) e = N;
      }
      if (s > e) return [];
      return arr.slice(Number(s), Number(e));
    }

    case 'flatten': {
      checkKeys(args, ['input']);
      const arr = requireArray(need(args, 'input'), 'input');
      const out = [];
      for (const x of arr) {
        if (Array.isArray(x)) {
          for (const y of x) out.push(y);
        } else {
          out.push(x);
        }
      }
      return out;
    }

    /* ---------------- objects ---------------- */
    case 'get': {
      checkKeys(args, ['input', 'key']);
      const obj = requireObject(need(args, 'input'), 'input');
      const key = requireString(need(args, 'key'), 'key');
      return obj.has(key) ? obj.get(key) : null;
    }

    case 'set': {
      checkKeys(args, ['input', 'key', 'value']);
      const obj = requireObject(need(args, 'input'), 'input');
      const key = requireString(need(args, 'key'), 'key');
      const value = need(args, 'value');
      const out = new JObj(obj.entries());
      out.set(key, value);
      return out;
    }

    case 'keys': {
      checkKeys(args, ['input']);
      const obj = requireObject(need(args, 'input'), 'input');
      return sortedKeys(obj);
    }

    case 'values': {
      checkKeys(args, ['input']);
      const obj = requireObject(need(args, 'input'), 'input');
      return sortedKeys(obj).map((k) => obj.get(k));
    }

    case 'merge': {
      checkKeys(args, ['a', 'b']);
      const a = requireObject(need(args, 'a'), 'a');
      const b = requireObject(need(args, 'b'), 'b');
      const out = new JObj(a.entries());
      for (const [k, v] of b.entries()) out.set(k, v);
      return out;
    }

    case 'pick': {
      checkKeys(args, ['input', 'keys']);
      const obj = requireObject(need(args, 'input'), 'input');
      const keyArr = requireArray(need(args, 'keys'), 'keys');
      const requested = new Set();
      for (const k of keyArr) requested.add(requireString(k, 'key'));
      const out = new JObj();
      for (const k of Array.from(requested).sort(utf8Compare)) {
        out.set(k, obj.has(k) ? obj.get(k) : null);
      }
      return out;
    }

    case 'omit': {
      checkKeys(args, ['input', 'keys']);
      const obj = requireObject(need(args, 'input'), 'input');
      const keyArr = requireArray(need(args, 'keys'), 'keys');
      const omitted = new Set();
      for (const k of keyArr) omitted.add(requireString(k, 'key'));
      const out = new JObj();
      for (const [k, v] of obj.entries()) {
        if (!omitted.has(k)) out.set(k, v);
      }
      return out;
    }

    /* ---------------- utilities ---------------- */
    case 'constant': {
      checkKeys(args, ['value']);
      return need(args, 'value');
    }

    case 'equals': {
      checkKeys(args, ['a', 'b']);
      return structuralEquals(need(args, 'a'), need(args, 'b'));
    }

    case 'compare': {
      checkKeys(args, ['a', 'b', 'op']);
      const a = need(args, 'a');
      const b = need(args, 'b');
      const cop = requireBoolOp(need(args, 'op'), ['gt', 'gte', 'lt', 'lte'], 'op');
      const c = compareValues(a, b);
      if (cop === 'gt') return c > 0;
      if (cop === 'gte') return c >= 0;
      if (cop === 'lt') return c < 0;
      return c <= 0;
    }

    case 'and': {
      checkKeys(args, ['a', 'b']);
      const a = need(args, 'a');
      const b = need(args, 'b');
      return isTruthy(a) && isTruthy(b);
    }

    case 'or': {
      checkKeys(args, ['a', 'b']);
      const a = need(args, 'a');
      const b = need(args, 'b');
      return isTruthy(a) || isTruthy(b);
    }

    case 'not': {
      checkKeys(args, ['a']);
      return !isTruthy(need(args, 'a'));
    }

    case 'type': {
      checkKeys(args, ['value']);
      return typeName(need(args, 'value'));
    }

    case 'fail_until': {
      checkKeys(args, ['failures', 'value']);
      const failures = requireI64(need(args, 'failures'), 'failures');
      if (failures < 0n || failures > 10n) {
        throw invalidArg('failures must be between 0 and 10');
      }
      const value = need(args, 'value');
      if (BigInt(attempt) <= failures) {
        throw new StopError('TRANSIENT_FAILURE', null, 'transient failure');
      }
      return value;
    }

    default:
      throw invalidArg('unknown operation');
  }
}

/* ------------------------------------------------------------------ */
/* Response helpers                                                    */
/* ------------------------------------------------------------------ */

function makeError(code, step, message) {
  const e = new JObj();
  e.set('code', code);
  e.set('step', step === undefined || step === null ? null : step);
  e.set('message', message || '');
  return e;
}

function makeResponse(status, results, executionOrder, skipped, error) {
  const r = new JObj();
  r.set('status', status);
  r.set('results', results);
  r.set('execution_order', executionOrder);
  r.set('skipped', skipped);
  r.set('error', error);
  return r;
}

function validationFailure(code, step, message) {
  return makeResponse('failed', new JObj(), [], [], makeError(code, step, message));
}

function exitCodeFor(code) {
  if (code === 'INVALID_WORKFLOW' ||
      code === 'UNKNOWN_DEPENDENCY' ||
      code === 'CYCLIC_DEPENDENCY') {
    return 2;
  }
  return 1;
}

/* ------------------------------------------------------------------ */
/* Execution                                                           */
/* ------------------------------------------------------------------ */

function executeWorkflow(doc) {
  let validated;
  try {
    validated = validateWorkflow(doc);
  } catch (e) {
    if (e instanceof StopError) {
      return { response: validationFailure(e.code, e.step, e.message), code: exitCodeFor(e.code) };
    }
    throw e;
  }

  const steps = validated.steps;
  const stepById = validated.stepById;

  const done = new Set();
  const skippedSet = new Set();
  const results = new JObj();
  const executionOrder = [];
  const skippedList = [];

  function runtimeFailure(code, step, message) {
    return {
      response: makeResponse('failed', results, executionOrder, skippedList,
        makeError(code, step, message)),
      code: 1
    };
  }

  for (let completedCount = 0; completedCount < steps.length; completedCount++) {
    // Deterministic topological choice: earliest step in document order whose
    // dependencies have all finished (completed or skipped).
    let step = null;
    for (const s of steps) {
      if (done.has(s.id)) continue;
      let ready = true;
      for (const d of s.deps) {
        if (!done.has(d)) { ready = false; break; }
      }
      if (ready) { step = s; break; }
    }
    if (step === null) {
      // Should have been caught as a cycle during validation.
      throw new StopError('CYCLIC_DEPENDENCY', null, 'no ready step');
    }

    const ctx = {
      step,
      stepById,
      skipped: skippedSet,
      results
    };

    try {
      // 1. Condition.
      if (step.when !== null) {
        const condResult = evalCondition(step.when, ctx);
        if (typeof condResult !== 'boolean') {
          throw new StopError('INVALID_CONDITION', step.id,
            'condition did not evaluate to a boolean');
        }
        if (condResult === false) {
          done.add(step.id);
          skippedSet.add(step.id);
          results.set(step.id, null);
          executionOrder.push(step.id);
          skippedList.push(step.id);
          continue;
        }
      }

      // 2. Resolve args.
      const resolvedArgs = resolveValue(step.args, ctx);
      if (!(resolvedArgs instanceof JObj)) {
        throw new StopError('INVALID_ARG', step.id, 'args must resolve to an object');
      }

      // 3. Run operation with retries.
      let value;
      let failure = null;
      for (let attempt = 1; attempt <= step.maxAttempts; attempt++) {
        try {
          value = runOp(step.op, resolvedArgs, attempt);
          failure = null;
          break;
        } catch (e) {
          if (e instanceof StopError && e.code === 'TRANSIENT_FAILURE') {
            if (attempt < step.maxAttempts) {
              continue; // retry
            }
            failure = new StopError('STEP_FAILED', step.id,
              'step failed after ' + attempt + ' attempt(s)');
            break;
          }
          failure = e;
          break;
        }
      }

      if (failure !== null) {
        if (failure instanceof StopError) {
          return runtimeFailure(failure.code,
            failure.step === null || failure.step === undefined ? step.id : failure.step,
            failure.message);
        }
        throw failure;
      }

      done.add(step.id);
      results.set(step.id, value);
      executionOrder.push(step.id);
    } catch (e) {
      if (e instanceof StopError) {
        return runtimeFailure(e.code,
          e.step === null || e.step === undefined ? step.id : e.step,
          e.message);
      }
      throw e;
    }
  }

  return {
    response: makeResponse('completed', results, executionOrder, skippedList, null),
    code: 0
  };
}

/* ------------------------------------------------------------------ */
/* CLI                                                                 */
/* ------------------------------------------------------------------ */

function readInput(arg) {
  if (arg === '-') return fs.readFileSync(0, 'utf8');
  return fs.readFileSync(arg, 'utf8');
}

function main() {
  const argv = process.argv.slice(2);

  if (argv.length !== 2 || argv[0] !== 'run') {
    const resp = validationFailure('INVALID_WORKFLOW', null,
      'usage: workflow run <workflow.json> | workflow run -');
    fs.writeSync(1, serialize(resp) + '\n');
    return 2;
  }

  let text;
  try {
    text = readInput(argv[1]);
  } catch (e) {
    const resp = validationFailure('INVALID_WORKFLOW', null,
      'could not read input: ' + e.message);
    fs.writeSync(1, serialize(resp) + '\n');
    return 2;
  }

  if (text.charCodeAt(0) === 0xfeff) text = text.slice(1);

  let doc;
  try {
    doc = parseJson(text);
  } catch (e) {
    const resp = validationFailure('INVALID_WORKFLOW', null,
      'invalid JSON: ' + e.message);
    fs.writeSync(1, serialize(resp) + '\n');
    return 2;
  }

  const outcome = executeWorkflow(doc);
  fs.writeSync(1, serialize(outcome.response) + '\n');
  return outcome.code;
}

let exitCode;
try {
  exitCode = main();
} catch (e) {
  try {
    const resp = makeResponse('failed', new JObj(), [], [],
      makeError('INTERNAL_ERROR', null, String(e && e.message ? e.message : e)));
    fs.writeSync(1, serialize(resp) + '\n');
  } catch (_) {
    // Best effort only.
  }
  exitCode = 3;
}
process.exitCode = exitCode;
