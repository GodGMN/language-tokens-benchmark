#!/usr/bin/env node
'use strict';

/*
 * workflow.js — declarative workflow engine.
 *
 * Implements ../../../SPEC.md.  No dependencies outside the Node.js standard
 * library.  Numbers are kept as BigInt internally so that i64 arithmetic can
 * be checked exactly; integer literals are parsed as BigInt, fraction/exponent
 * literals are parsed as JS doubles (and are rejected wherever an i64 argument
 * is required).
 */

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

class CodedError extends Error {
  constructor(code, message, transient, step) {
    super(message || code);
    this.code = code;
    this.transient = !!transient;
    this.step = step === undefined ? null : step;
  }
}

class ParseError extends Error {}

function opFail(code, message) {
  throw new CodedError(code, message || code);
}

function transientFail(message) {
  throw new CodedError('TRANSIENT_FAILURE', message || 'transient failure', true);
}

// ---------------------------------------------------------------------------
// i64 helpers
// ---------------------------------------------------------------------------

const I64_MIN = -(1n << 63n);
const I64_MAX = (1n << 63n) - 1n;

function i64(x) {
  if (x < I64_MIN || x > I64_MAX) opFail('ARITHMETIC_OVERFLOW', 'i64 overflow');
  return x;
}

// Arithmetic on two i64 values, with all overflow / division rules.
function arith(op, a, b) {
  switch (op) {
    case 'add':
      return i64(a + b);
    case 'subtract':
      return i64(a - b);
    case 'multiply':
      return i64(a * b);
    case 'divide':
      if (b === 0n) opFail('DIVISION_BY_ZERO', 'division by zero');
      if (a === I64_MIN && b === -1n) opFail('ARITHMETIC_OVERFLOW', 'i64 overflow');
      return a / b; // BigInt division truncates toward zero
    case 'mod':
      if (b === 0n) opFail('DIVISION_BY_ZERO', 'division by zero');
      if (a === I64_MIN && b === -1n) opFail('ARITHMETIC_OVERFLOW', 'i64 overflow');
      return a % b; // sign of the dividend, like C99
    default:
      opFail('INVALID_ARG', 'unsupported arithmetic operator ' + op);
  }
}

// ---------------------------------------------------------------------------
// Value helpers
// ---------------------------------------------------------------------------

function isNum(v) {
  return typeof v === 'bigint' || typeof v === 'number';
}

function isObj(v) {
  return v !== null && typeof v === 'object' && !Array.isArray(v);
}

function typeName(v) {
  if (v === null) return 'null';
  const t = typeof v;
  if (t === 'bigint' || t === 'number') return 'number';
  if (t === 'string' || t === 'boolean') return t;
  if (Array.isArray(v)) return 'array';
  return 'object';
}

function typeRank(v) {
  if (isNum(v)) return 0;
  if (typeof v === 'string') return 1;
  if (typeof v === 'boolean') return 2;
  if (v === null) return 3;
  if (Array.isArray(v)) return 4;
  return 5;
}

// Byte-wise lexicographic order of the UTF-8 encoding; for valid strings this
// is exactly code point order.
function cmpStr(a, b) {
  if (a === b) return 0;
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

// Numeric comparison between i64 (BigInt) and/or JSON doubles.
function numCmp(a, b) {
  if (typeof a === 'bigint' && typeof b === 'bigint') {
    return a < b ? -1 : a > b ? 1 : 0;
  }
  if (typeof a === 'number' && typeof b === 'number') {
    return a < b ? -1 : a > b ? 1 : 0;
  }
  const big = typeof a === 'bigint' ? a : b;
  const dbl = typeof a === 'number' ? a : b;
  let c;
  if (Number.isInteger(dbl)) {
    const bd = BigInt(dbl);
    c = big < bd ? -1 : big > bd ? 1 : 0;
  } else {
    const floor = BigInt(Math.floor(dbl));
    if (big < floor) c = -1;
    else if (big > floor) c = 1;
    else c = -1; // equal to floor, double is floor + positive fraction
  }
  return typeof a === 'bigint' ? c : -c;
}

// Total order used by sort / compare / relational conditions.
function cmpValues(a, b) {
  const ra = typeRank(a);
  const rb = typeRank(b);
  if (ra !== rb) return ra < rb ? -1 : 1;
  switch (ra) {
    case 0:
      return numCmp(a, b);
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
      return a.length === b.length ? 0 : a.length < b.length ? -1 : 1;
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

// Structural equality.
function deepEqual(a, b) {
  if (isNum(a) && isNum(b)) return numCmp(a, b) === 0;
  if (a === null || b === null) return a === b;
  const ta = typeof a;
  const tb = typeof b;
  if (ta !== tb) return false;
  if (ta === 'string' || ta === 'boolean') return a === b;
  const aa = Array.isArray(a);
  const ab = Array.isArray(b);
  if (aa || ab) {
    if (!aa || !ab) return false;
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) {
      if (!deepEqual(a[i], b[i])) return false;
    }
    return true;
  }
  if (ta === 'object') {
    const ka = Object.keys(a);
    const kb = Object.keys(b);
    if (ka.length !== kb.length) return false;
    for (const k of ka) {
      if (!Object.hasOwn(b, k)) return false;
      if (!deepEqual(a[k], b[k])) return false;
    }
    return true;
  }
  return false;
}

// JSON truthiness: false, null, "", 0, [], {} are falsy.
function truthy(v) {
  if (v === null || v === undefined) return false;
  if (v === false) return false;
  if (typeof v === 'string') return v.length > 0;
  if (isNum(v)) return numCmp(v, 0n) !== 0;
  if (Array.isArray(v)) return v.length > 0;
  if (typeof v === 'object') return Object.keys(v).length > 0;
  return true;
}

// Canonical key for structural equality (used by `unique` so that dedup is
// O(n) rather than O(n^2)).  Numeric value equality is canonicalised: 1 and
// 1.0 map to the same key.
function valueKey(v) {
  if (v === null) return 'z';
  const t = typeof v;
  if (t === 'boolean') return v ? 'b1' : 'b0';
  if (t === 'bigint') return 'n' + v.toString();
  if (t === 'number') {
    if (Number.isInteger(v)) return 'n' + BigInt(v).toString();
    return 'n' + String(v);
  }
  if (t === 'string') return 's' + JSON.stringify(v);
  if (Array.isArray(v)) {
    let s = 'a[';
    for (let i = 0; i < v.length; i++) {
      if (i) s += ',';
      s += valueKey(v[i]);
    }
    return s + ']';
  }
  const keys = Object.keys(v).sort(cmpStr);
  let s = 'o{';
  for (let i = 0; i < keys.length; i++) {
    if (i) s += ',';
    s += JSON.stringify(keys[i]) + ':' + valueKey(v[keys[i]]);
  }
  return s + '}';
}

// Compact JSON serialization.  BigInt is emitted as a plain integer.
function jsonStringify(v) {
  if (v === undefined || v === null) return 'null';
  const t = typeof v;
  if (t === 'bigint') return v.toString();
  if (t === 'number') {
    if (!Number.isFinite(v)) return 'null';
    return JSON.stringify(v);
  }
  if (t === 'string' || t === 'boolean') return JSON.stringify(v);
  if (Array.isArray(v)) {
    let s = '[';
    for (let i = 0; i < v.length; i++) {
      if (i) s += ',';
      s += jsonStringify(v[i]);
    }
    return s + ']';
  }
  const keys = Object.keys(v);
  let s = '{';
  for (let i = 0; i < keys.length; i++) {
    if (i) s += ',';
    s += JSON.stringify(keys[i]) + ':' + jsonStringify(v[keys[i]]);
  }
  return s + '}';
}

// ---------------------------------------------------------------------------
// JSON parser (preserves the distinction between integer and float literals)
// ---------------------------------------------------------------------------

const MAX_PARSE_DEPTH = 2000;

function parseJSON(text) {
  let i = 0;
  const n = text.length;
  let depth = 0;

  function err(msg) {
    throw new ParseError(msg + ' at offset ' + i);
  }

  function ws() {
    while (i < n) {
      const c = text.charCodeAt(i);
      if (c === 32 || c === 9 || c === 10 || c === 13) i++;
      else break;
    }
  }

  function literal(word, value) {
    if (text.startsWith(word, i)) {
      i += word.length;
      return value;
    }
    err('invalid literal');
  }

  function parseString() {
    if (text[i] !== '"') err('expected string');
    i++;
    let out = '';
    for (;;) {
      if (i >= n) err('unterminated string');
      const c = text.charCodeAt(i);
      if (c === 34) {
        i++;
        return out;
      }
      if (c === 92) {
        i++;
        if (i >= n) err('bad escape');
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
            if (i + 4 > n) err('bad unicode escape');
            const hex = text.slice(i, i + 4);
            if (!/^[0-9A-Fa-f]{4}$/.test(hex)) err('bad unicode escape');
            out += String.fromCharCode(parseInt(hex, 16));
            i += 4;
            break;
          }
          default:
            err('bad escape');
        }
        continue;
      }
      if (c < 0x20) err('control character in string');
      out += text[i];
      i++;
    }
  }

  const NUM_RE = /-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?/y;

  function parseNumber() {
    NUM_RE.lastIndex = i;
    const m = NUM_RE.exec(text);
    if (!m) err('invalid number');
    const raw = m[0];
    i += raw.length;
    if (raw.indexOf('.') === -1 && raw.indexOf('e') === -1 && raw.indexOf('E') === -1) {
      return BigInt(raw);
    }
    const num = Number(raw);
    if (!Number.isFinite(num)) err('non-finite number');
    return num;
  }

  function parseValue() {
    ws();
    if (i >= n) err('unexpected end of input');
    const c = text[i];
    if (c === '{') return parseObject();
    if (c === '[') return parseArray();
    if (c === '"') return parseString();
    if (c === 't') return literal('true', true);
    if (c === 'f') return literal('false', false);
    if (c === 'n') return literal('null', null);
    if (c === '-' || (c >= '0' && c <= '9')) return parseNumber();
    err('unexpected character ' + JSON.stringify(c));
  }

  function parseObject() {
    if (depth >= MAX_PARSE_DEPTH) err('nesting too deep');
    depth++;
    i++; // {
    const obj = Object.create(null);
    ws();
    if (i < n && text[i] === '}') {
      i++;
      depth--;
      return obj;
    }
    for (;;) {
      ws();
      if (i >= n || text[i] !== '"') err('expected object key');
      const key = parseString();
      ws();
      if (i >= n || text[i] !== ':') err('expected colon');
      i++;
      obj[key] = parseValue();
      ws();
      if (i >= n) err('unterminated object');
      if (text[i] === ',') {
        i++;
        continue;
      }
      if (text[i] === '}') {
        i++;
        depth--;
        return obj;
      }
      err('expected comma or closing brace');
    }
  }

  function parseArray() {
    if (depth >= MAX_PARSE_DEPTH) err('nesting too deep');
    depth++;
    i++; // [
    const arr = [];
    ws();
    if (i < n && text[i] === ']') {
      i++;
      depth--;
      return arr;
    }
    for (;;) {
      arr.push(parseValue());
      ws();
      if (i >= n) err('unterminated array');
      if (text[i] === ',') {
        i++;
        continue;
      }
      if (text[i] === ']') {
        i++;
        depth--;
        return arr;
      }
      err('expected comma or closing bracket');
    }
  }

  const value = parseValue();
  ws();
  if (i !== n) err('trailing content');
  return value;
}

// ---------------------------------------------------------------------------
// Workflow validation
// ---------------------------------------------------------------------------

const OPS = new Set([
  'add', 'subtract', 'multiply', 'divide', 'mod', 'sum', 'min', 'max', 'range',
  'concat', 'split', 'join', 'uppercase', 'lowercase', 'trim', 'replace', 'length',
  'map', 'filter', 'reduce', 'sort', 'reverse', 'unique', 'slice', 'flatten',
  'get', 'set', 'keys', 'values', 'merge', 'pick', 'omit',
  'constant', 'equals', 'compare', 'and', 'or', 'not', 'type', 'fail_until',
]);

const STEP_FIELDS = new Set(['id', 'op', 'args', 'depends_on', 'when', 'retry']);
const CMP_OPS = new Set(['eq', 'ne', 'gt', 'gte', 'lt', 'lte']);
const ID_RE = /^[A-Za-z_][A-Za-z0-9_]*$/;

function wfError(step, message) {
  throw new CodedError('INVALID_WORKFLOW', message, false, step);
}

function validateConditionShape(c, stepId) {
  if (!isObj(c)) wfError(stepId, 'condition must be an object');
  const op = c.op;
  if (typeof op !== 'string') wfError(stepId, 'condition op must be a string');
  if (op === 'all' || op === 'any') {
    for (const k of Object.keys(c)) {
      if (k !== 'op' && k !== 'conditions') wfError(stepId, 'unknown condition field ' + k);
    }
    if (!Array.isArray(c.conditions)) wfError(stepId, 'condition ' + op + ' needs a conditions array');
    for (const sub of c.conditions) validateConditionShape(sub, stepId);
    return;
  }
  if (op === 'not') {
    for (const k of Object.keys(c)) {
      if (k !== 'op' && k !== 'condition') wfError(stepId, 'unknown condition field ' + k);
    }
    if (!isObj(c.condition)) wfError(stepId, 'condition not needs a condition object');
    validateConditionShape(c.condition, stepId);
    return;
  }
  if (CMP_OPS.has(op)) {
    for (const k of Object.keys(c)) {
      if (k !== 'op' && k !== 'left' && k !== 'right') wfError(stepId, 'unknown condition field ' + k);
    }
    if (!Object.hasOwn(c, 'left')) wfError(stepId, 'condition ' + op + ' missing left');
    if (!Object.hasOwn(c, 'right')) wfError(stepId, 'condition ' + op + ' missing right');
    return;
  }
  wfError(stepId, 'unknown condition op ' + op);
}

function validateWorkflow(doc) {
  if (!isObj(doc)) wfError(null, 'workflow must be a JSON object');
  for (const k of Object.keys(doc)) {
    if (k !== 'version' && k !== 'steps') wfError(null, 'unknown top-level field ' + k);
  }
  if (!Object.hasOwn(doc, 'version')) wfError(null, 'missing version');
  const version = doc.version;
  if (!(typeof version === 'bigint' && version === 1n)) {
    wfError(null, 'version must be the integer literal 1');
  }
  if (!Object.hasOwn(doc, 'steps')) wfError(null, 'missing steps');
  if (!Array.isArray(doc.steps)) wfError(null, 'steps must be an array');

  const raw = doc.steps;
  const seenIds = new Set();
  const idList = [];

  // Pass 1: ids.
  for (let idx = 0; idx < raw.length; idx++) {
    const s = raw[idx];
    if (!isObj(s)) wfError(null, 'step must be an object');
    const id = s.id;
    const errStep = typeof id === 'string' ? id : null;
    if (!Object.hasOwn(s, 'id') || typeof id !== 'string') {
      wfError(errStep, 'step id must be a string');
    }
    if (!ID_RE.test(id)) wfError(id, 'invalid step id ' + JSON.stringify(id));
    if (seenIds.has(id)) wfError(id, 'duplicate step id ' + id);
    seenIds.add(id);
    idList.push(id);
  }

  // Pass 2: every other field.
  const steps = [];
  for (let idx = 0; idx < raw.length; idx++) {
    const s = raw[idx];
    const id = idList[idx];
    for (const k of Object.keys(s)) {
      if (!STEP_FIELDS.has(k)) wfError(id, 'unknown step field ' + k);
    }
    if (typeof s.op !== 'string') wfError(id, 'step op must be a string');
    if (!OPS.has(s.op)) wfError(id, 'unknown operation ' + s.op);

    let args = Object.create(null);
    if (Object.hasOwn(s, 'args')) {
      if (!isObj(s.args)) wfError(id, 'args must be an object');
      args = s.args;
    }

    let deps = [];
    if (Object.hasOwn(s, 'depends_on')) {
      if (!Array.isArray(s.depends_on)) wfError(id, 'depends_on must be an array');
      for (const d of s.depends_on) {
        if (typeof d !== 'string') wfError(id, 'depends_on entries must be strings');
      }
      deps = s.depends_on.slice();
    }

    let when = null;
    if (Object.hasOwn(s, 'when')) {
      when = s.when;
      validateConditionShape(when, id);
    }

    let maxAttempts = 1;
    if (Object.hasOwn(s, 'retry')) {
      const r = s.retry;
      if (!isObj(r)) wfError(id, 'retry must be an object');
      for (const k of Object.keys(r)) {
        if (k !== 'max_attempts') wfError(id, 'unknown retry field ' + k);
      }
      if (!Object.hasOwn(r, 'max_attempts')) wfError(id, 'retry missing max_attempts');
      const m = r.max_attempts;
      if (typeof m !== 'bigint' || m < 1n || m > 5n) {
        wfError(id, 'retry.max_attempts must be an integer in 1..5');
      }
      maxAttempts = Number(m);
    }

    steps.push({
      id,
      op: s.op,
      args,
      deps,
      when,
      maxAttempts,
      index: idx,
      ancestors: new Set(),
    });
  }

  // Pass 3: dependencies exist.
  for (const step of steps) {
    for (const d of step.deps) {
      if (!seenIds.has(d)) {
        throw new CodedError('UNKNOWN_DEPENDENCY', 'unknown dependency ' + d, false, step.id);
      }
    }
  }

  // Pass 4: cycles (Kahn's algorithm).
  const indeg = new Map();
  const dependents = new Map();
  for (const step of steps) {
    indeg.set(step.id, 0);
    dependents.set(step.id, []);
  }
  for (const step of steps) {
    for (const d of step.deps) {
      indeg.set(step.id, indeg.get(step.id) + 1);
      dependents.get(d).push(step.id);
    }
  }
  const queue = [];
  for (const step of steps) {
    if (indeg.get(step.id) === 0) queue.push(step.id);
  }
  let processed = 0;
  for (let qi = 0; qi < queue.length; qi++) {
    const id = queue[qi];
    processed++;
    for (const dep of dependents.get(id)) {
      const v = indeg.get(dep) - 1;
      indeg.set(dep, v);
      if (v === 0) queue.push(dep);
    }
  }
  if (processed !== steps.length) {
    throw new CodedError('CYCLIC_DEPENDENCY', 'dependency cycle detected', false, null);
  }

  // Transitive dependency sets (used by reference resolution).
  const byId = new Map();
  for (const step of steps) byId.set(step.id, step);
  const memo = new Map();
  function ancestorsOf(id) {
    if (memo.has(id)) return memo.get(id);
    const set = new Set();
    memo.set(id, set); // graph is acyclic; guard is harmless
    for (const d of byId.get(id).deps) {
      set.add(d);
      for (const a of ancestorsOf(d)) set.add(a);
    }
    return set;
  }
  for (const step of steps) step.ancestors = ancestorsOf(step.id);

  return { steps, byId };
}

// ---------------------------------------------------------------------------
// References
// ---------------------------------------------------------------------------

function isRefObject(v) {
  if (!isObj(v)) return false;
  const keys = Object.keys(v);
  return keys.length === 1 && keys[0] === '$ref' && typeof v.$ref === 'string';
}

function resolveRef(pathStr, step, ctx) {
  const parts = pathStr.split('.');
  const targetId = parts[0];
  const target = ctx.byId.get(targetId);
  if (!target) opFail('INVALID_REFERENCE', 'unknown step in reference: ' + targetId);
  if (targetId === step.id) opFail('INVALID_REFERENCE', 'self reference: ' + targetId);
  if (!step.ancestors.has(targetId)) {
    opFail('INVALID_REFERENCE', 'reference to non-dependency: ' + targetId);
  }
  if (ctx.skippedSet.has(targetId)) {
    opFail('SKIPPED_REFERENCE', 'reference to skipped step: ' + targetId);
  }
  if (!ctx.finished.has(targetId)) {
    opFail('INVALID_REFERENCE', 'reference to unfinished step: ' + targetId);
  }

  let value = ctx.results[targetId];
  for (let k = 1; k < parts.length; k++) {
    const seg = parts[k];
    if (seg === '') opFail('INVALID_REFERENCE', 'empty path segment');
    if (Array.isArray(value)) {
      if (!/^[0-9]+$/.test(seg)) opFail('INVALID_REFERENCE', 'invalid array index ' + seg);
      const idx = BigInt(seg);
      if (idx >= BigInt(value.length)) opFail('INVALID_REFERENCE', 'array index out of range ' + seg);
      value = value[Number(idx)];
    } else if (isObj(value)) {
      if (!Object.hasOwn(value, seg)) opFail('INVALID_REFERENCE', 'missing key ' + seg);
      value = value[seg];
    } else {
      opFail('INVALID_REFERENCE', 'cannot navigate into ' + typeName(value));
    }
  }
  return value;
}

function resolveValue(v, step, ctx) {
  if (isRefObject(v)) return resolveRef(v.$ref, step, ctx);
  if (Array.isArray(v)) {
    const out = new Array(v.length);
    for (let i = 0; i < v.length; i++) out[i] = resolveValue(v[i], step, ctx);
    return out;
  }
  if (isObj(v)) {
    const out = Object.create(null);
    for (const k of Object.keys(v)) out[k] = resolveValue(v[k], step, ctx);
    return out;
  }
  return v;
}

// ---------------------------------------------------------------------------
// Conditions
// ---------------------------------------------------------------------------

function evalCondition(c, step, ctx) {
  const op = c.op;
  if (op === 'all') {
    for (const sub of c.conditions) {
      const r = evalCondition(sub, step, ctx);
      if (typeof r !== 'boolean') opFail('INVALID_CONDITION', 'condition did not yield a boolean');
      if (!r) return false;
    }
    return true;
  }
  if (op === 'any') {
    for (const sub of c.conditions) {
      const r = evalCondition(sub, step, ctx);
      if (typeof r !== 'boolean') opFail('INVALID_CONDITION', 'condition did not yield a boolean');
      if (r) return true;
    }
    return false;
  }
  if (op === 'not') {
    const r = evalCondition(c.condition, step, ctx);
    if (typeof r !== 'boolean') opFail('INVALID_CONDITION', 'condition did not yield a boolean');
    return !r;
  }
  const left = resolveValue(c.left, step, ctx);
  const right = resolveValue(c.right, step, ctx);
  let result;
  switch (op) {
    case 'eq': result = deepEqual(left, right); break;
    case 'ne': result = !deepEqual(left, right); break;
    case 'gt': result = cmpValues(left, right) > 0; break;
    case 'gte': result = cmpValues(left, right) >= 0; break;
    case 'lt': result = cmpValues(left, right) < 0; break;
    case 'lte': result = cmpValues(left, right) <= 0; break;
    default: opFail('INVALID_CONDITION', 'unknown condition op ' + op);
  }
  if (typeof result !== 'boolean') opFail('INVALID_CONDITION', 'condition did not yield a boolean');
  return result;
}

// ---------------------------------------------------------------------------
// Argument helpers
// ---------------------------------------------------------------------------

function checkArgs(args, allowed, required) {
  if (!isObj(args)) opFail('INVALID_ARG', 'args must be an object');
  for (const k of Object.keys(args)) {
    if (!allowed.includes(k)) opFail('INVALID_ARG', 'unknown argument ' + k);
  }
  for (const k of required) {
    if (!Object.hasOwn(args, k)) opFail('INVALID_ARG', 'missing argument ' + k);
  }
}

function checkExactKeys(obj, allowed, required) {
  if (!isObj(obj)) opFail('INVALID_ARG', 'expected an object');
  for (const k of Object.keys(obj)) {
    if (!allowed.includes(k)) opFail('INVALID_ARG', 'unknown field ' + k);
  }
  for (const k of required || []) {
    if (!Object.hasOwn(obj, k)) opFail('INVALID_ARG', 'missing field ' + k);
  }
}

function getInt(args, name, required) {
  if (required === false && !Object.hasOwn(args, name)) return undefined;
  if (!Object.hasOwn(args, name)) opFail('INVALID_ARG', 'missing integer argument ' + name);
  const v = args[name];
  if (typeof v !== 'bigint' || v < I64_MIN || v > I64_MAX) {
    opFail('INVALID_ARG', 'argument ' + name + ' must be an i64 integer literal');
  }
  return v;
}

function getStr(args, name) {
  const v = args[name];
  if (typeof v !== 'string') opFail('INVALID_ARG', 'argument ' + name + ' must be a string');
  return v;
}

function getArr(args, name) {
  const v = args[name];
  if (!Array.isArray(v)) opFail('INVALID_ARG', 'argument ' + name + ' must be an array');
  return v;
}

function getObjArg(args, name) {
  const v = args[name];
  if (!isObj(v)) opFail('INVALID_ARG', 'argument ' + name + ' must be an object');
  return v;
}

// Convert a value contained in an array / transform to an i64 integer.  Only
// integer literals (and integer results) are accepted; float literals such as
// 1.0 are rejected, per the SPEC's numeric-parameter rule.
function toI64(v) {
  if (typeof v !== 'bigint') {
    opFail('INVALID_ARG', 'expected an i64 integer, got ' + typeName(v));
  }
  if (v < I64_MIN || v > I64_MAX) opFail('INVALID_ARG', 'integer out of i64 range');
  return v;
}

function getStringArray(args, name) {
  const arr = getArr(args, name);
  for (const v of arr) {
    if (typeof v !== 'string') opFail('INVALID_ARG', 'argument ' + name + ' must contain strings');
  }
  return arr;
}

// ---------------------------------------------------------------------------
// map.transform / filter.predicate compilation
// ---------------------------------------------------------------------------

const TYPE_NAMES = new Set(['null', 'boolean', 'number', 'string', 'array', 'object']);

function isSelfMarker(v) {
  return isObj(v) && Object.keys(v).length === 1 && v.$self === true;
}

function compileTransform(t) {
  if (!isObj(t)) opFail('INVALID_ARG', 'transform must be an object');
  const type = t.type;
  if (typeof type !== 'string') opFail('INVALID_ARG', 'transform type must be a string');

  switch (type) {
    case 'constant': {
      checkExactKeys(t, ['type', 'value'], ['value']);
      const val = t.value;
      return () => val;
    }
    case 'add':
    case 'subtract':
    case 'multiply':
    case 'divide':
    case 'mod': {
      checkExactKeys(t, ['type', 'value'], ['value']);
      const n = getInt(t, 'value');
      return (el) => arith(type, toI64(el), n);
    }
    case 'negate': {
      checkExactKeys(t, ['type'], []);
      return (el) => {
        const x = toI64(el);
        if (x === I64_MIN) opFail('ARITHMETIC_OVERFLOW', 'i64 overflow');
        return -x;
      };
    }
    case 'abs': {
      checkExactKeys(t, ['type'], []);
      return (el) => {
        const x = toI64(el);
        if (x === I64_MIN) opFail('ARITHMETIC_OVERFLOW', 'i64 overflow');
        return x < 0n ? -x : x;
      };
    }
    case 'stringify': {
      checkExactKeys(t, ['type'], []);
      return (el) => jsonStringify(el);
    }
    case 'type': {
      checkExactKeys(t, ['type'], []);
      return (el) => typeName(el);
    }
    case 'op': {
      // Extended form: {"type":"op","op":"add","args":{"a":{"$self":true},"b":2}}
      checkExactKeys(t, ['type', 'op', 'args'], ['op', 'args']);
      const opName = t.op;
      if (typeof opName !== 'string' || !['add', 'subtract', 'multiply', 'divide', 'mod'].includes(opName)) {
        opFail('INVALID_ARG', 'unsupported transform op');
      }
      const a = t.args;
      checkExactKeys(a, ['a', 'b'], ['a', 'b']);
      const left = a.a;
      const right = a.b;
      return (el) => {
        const lv = isSelfMarker(left) ? el : left;
        const rv = isSelfMarker(right) ? el : right;
        return arith(opName, toI64(lv), toI64(rv));
      };
    }
    default:
      opFail('INVALID_ARG', 'unsupported transform type ' + type);
  }
}

function getTypeNameField(p) {
  // The SPEC writes {"type": "type_is", "type": "<type>"}, which JSON cannot
  // represent; accept the plausible spellings of the target type field.
  for (const k of ['value', 'type_name', 'typeName', 'target', 'target_type', 'expected', 'name']) {
    if (Object.hasOwn(p, k)) {
      if (typeof p[k] !== 'string') opFail('INVALID_ARG', 'type_is target must be a string');
      return p[k];
    }
  }
  opFail('INVALID_ARG', 'type_is predicate missing its target type');
}

function compilePredicate(p) {
  if (!isObj(p)) opFail('INVALID_ARG', 'predicate must be an object');
  const type = p.type;
  if (typeof type !== 'string') opFail('INVALID_ARG', 'predicate type must be a string');

  switch (type) {
    case 'even': {
      checkExactKeys(p, ['type'], []);
      return (el) => toI64(el) % 2n === 0n;
    }
    case 'odd': {
      checkExactKeys(p, ['type'], []);
      return (el) => toI64(el) % 2n !== 0n;
    }
    case 'gt':
    case 'gte':
    case 'lt':
    case 'lte': {
      checkExactKeys(p, ['type', 'value'], ['value']);
      const n = getInt(p, 'value');
      return (el) => {
        const x = toI64(el);
        switch (type) {
          case 'gt': return x > n;
          case 'gte': return x >= n;
          case 'lt': return x < n;
          default: return x <= n;
        }
      };
    }
    case 'eq': {
      checkExactKeys(p, ['type', 'value'], ['value']);
      const n = getInt(p, 'value');
      return (el) => deepEqual(el, n);
    }
    case 'ne': {
      checkExactKeys(p, ['type', 'value'], ['value']);
      const n = getInt(p, 'value');
      return (el) => !deepEqual(el, n);
    }
    case 'truthy': {
      checkExactKeys(p, ['type'], []);
      return (el) => truthy(el);
    }
    case 'falsy': {
      checkExactKeys(p, ['type'], []);
      return (el) => !truthy(el);
    }
    case 'type_is': {
      const target = getTypeNameField(p);
      if (!TYPE_NAMES.has(target)) opFail('INVALID_ARG', 'unknown type name ' + target);
      return (el) => typeName(el) === target;
    }
    case 'string_contains': {
      checkExactKeys(p, ['type', 'value'], ['value']);
      const needle = p.value;
      if (typeof needle !== 'string') opFail('INVALID_ARG', 'string_contains value must be a string');
      return (el) => {
        if (typeof el !== 'string') opFail('INVALID_ARG', 'string_contains expects strings');
        return el.includes(needle);
      };
    }
    default:
      // Degenerate spelling of type_is: {"type":"number"} (see above).
      if (TYPE_NAMES.has(type) && Object.keys(p).length === 1) {
        return (el) => typeName(el) === type;
      }
      opFail('INVALID_ARG', 'unsupported predicate type ' + type);
  }
}

// ---------------------------------------------------------------------------
// Operations
// ---------------------------------------------------------------------------

function applyOp(op, args, ctx, stepId) {
  switch (op) {
    // ---- math -------------------------------------------------------------
    case 'add':
    case 'subtract':
    case 'multiply':
    case 'divide':
    case 'mod': {
      checkArgs(args, ['a', 'b'], ['a', 'b']);
      return arith(op, getInt(args, 'a'), getInt(args, 'b'));
    }
    case 'sum': {
      checkArgs(args, ['input'], ['input']);
      const input = getArr(args, 'input');
      let total = 0n;
      for (const el of input) total = i64(total + toI64(el));
      return total;
    }
    case 'min': {
      checkArgs(args, ['input'], ['input']);
      const input = getArr(args, 'input');
      if (input.length === 0) opFail('EMPTY_ARRAY', 'min of empty array');
      let best = toI64(input[0]);
      for (let i = 1; i < input.length; i++) {
        const x = toI64(input[i]);
        if (x < best) best = x;
      }
      return best;
    }
    case 'max': {
      checkArgs(args, ['input'], ['input']);
      const input = getArr(args, 'input');
      if (input.length === 0) opFail('EMPTY_ARRAY', 'max of empty array');
      let best = toI64(input[0]);
      for (let i = 1; i < input.length; i++) {
        const x = toI64(input[i]);
        if (x > best) best = x;
      }
      return best;
    }
    case 'range': {
      checkArgs(args, ['start', 'end'], ['start', 'end']);
      const start = getInt(args, 'start');
      const end = getInt(args, 'end');
      const out = [];
      if (start > end) return out;
      for (let x = start; x <= end; x++) out.push(x);
      return out;
    }

    // ---- strings ----------------------------------------------------------
    case 'concat': {
      checkArgs(args, ['a', 'b'], ['a', 'b']);
      return getStr(args, 'a') + getStr(args, 'b');
    }
    case 'split': {
      checkArgs(args, ['input', 'sep'], ['input', 'sep']);
      const input = getStr(args, 'input');
      const sep = getStr(args, 'sep');
      if (sep === '') return Array.from(input);
      return input.split(sep);
    }
    case 'join': {
      checkArgs(args, ['input', 'sep'], ['input', 'sep']);
      const input = getStringArray(args, 'input');
      return input.join(getStr(args, 'sep'));
    }
    case 'uppercase': {
      checkArgs(args, ['input'], ['input']);
      return getStr(args, 'input').replace(/[a-z]/g, (c) => String.fromCharCode(c.charCodeAt(0) - 32));
    }
    case 'lowercase': {
      checkArgs(args, ['input'], ['input']);
      return getStr(args, 'input').replace(/[A-Z]/g, (c) => String.fromCharCode(c.charCodeAt(0) + 32));
    }
    case 'trim': {
      checkArgs(args, ['input'], ['input']);
      return getStr(args, 'input').replace(/^[ \t\n\r\f\v]+|[ \t\n\r\f\v]+$/g, '');
    }
    case 'replace': {
      checkArgs(args, ['input', 'search', 'replace'], ['input', 'search', 'replace']);
      const input = getStr(args, 'input');
      const search = getStr(args, 'search');
      const replacement = getStr(args, 'replace');
      if (search === '') opFail('INVALID_ARG', 'replace search must not be empty');
      return input.split(search).join(replacement);
    }
    case 'length': {
      checkArgs(args, ['input'], ['input']);
      const input = getStr(args, 'input');
      return BigInt(Array.from(input).length);
    }

    // ---- arrays -----------------------------------------------------------
    case 'map': {
      checkArgs(args, ['input', 'transform'], ['input', 'transform']);
      const input = getArr(args, 'input');
      const fn = compileTransform(args.transform);
      return input.map((el) => fn(el));
    }
    case 'filter': {
      checkArgs(args, ['input', 'predicate'], ['input', 'predicate']);
      const input = getArr(args, 'input');
      const fn = compilePredicate(args.predicate);
      return input.filter((el) => fn(el));
    }
    case 'reduce': {
      checkArgs(args, ['input', 'op', 'initial'], ['input', 'op']);
      const input = getArr(args, 'input');
      const opName = args.op;
      if (typeof opName !== 'string' || !['add', 'subtract', 'multiply'].includes(opName)) {
        opFail('INVALID_ARG', 'reduce op must be add, subtract or multiply');
      }
      let acc;
      if (Object.hasOwn(args, 'initial')) acc = getInt(args, 'initial');
      else acc = opName === 'multiply' ? 1n : 0n;
      for (const el of input) acc = arith(opName, acc, toI64(el));
      return acc;
    }
    case 'sort': {
      checkArgs(args, ['input'], ['input']);
      const input = getArr(args, 'input');
      return input.slice().sort(cmpValues);
    }
    case 'reverse': {
      checkArgs(args, ['input'], ['input']);
      return getArr(args, 'input').slice().reverse();
    }
    case 'unique': {
      checkArgs(args, ['input'], ['input']);
      const input = getArr(args, 'input');
      const out = [];
      const seen = new Set();
      for (const el of input) {
        const key = valueKey(el);
        if (!seen.has(key)) {
          seen.add(key);
          out.push(el);
        }
      }
      return out;
    }
    case 'slice': {
      checkArgs(args, ['input', 'start', 'end'], ['input', 'start']);
      const input = getArr(args, 'input');
      const n = BigInt(input.length);
      let s = getInt(args, 'start');
      if (s < 0n) s = n + s;
      if (s < 0n) s = 0n;
      if (s > n) s = n;
      let e;
      if (Object.hasOwn(args, 'end')) {
        e = getInt(args, 'end');
        if (e < 0n) e = n + e;
        if (e < 0n) e = 0n;
        if (e > n) e = n;
      } else {
        e = n;
      }
      if (s >= e) return [];
      return input.slice(Number(s), Number(e));
    }
    case 'flatten': {
      checkArgs(args, ['input'], ['input']);
      const input = getArr(args, 'input');
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

    // ---- objects ----------------------------------------------------------
    case 'get': {
      checkArgs(args, ['input', 'key'], ['input', 'key']);
      const input = getObjArg(args, 'input');
      const key = getStr(args, 'key');
      return Object.hasOwn(input, key) ? input[key] : null;
    }
    case 'set': {
      checkArgs(args, ['input', 'key', 'value'], ['input', 'key', 'value']);
      const input = getObjArg(args, 'input');
      const key = getStr(args, 'key');
      const out = Object.create(null);
      for (const k of Object.keys(input)) out[k] = input[k];
      out[key] = args.value;
      return out;
    }
    case 'keys': {
      checkArgs(args, ['input'], ['input']);
      const input = getObjArg(args, 'input');
      return Object.keys(input).sort(cmpStr);
    }
    case 'values': {
      checkArgs(args, ['input'], ['input']);
      const input = getObjArg(args, 'input');
      const keys = Object.keys(input).sort(cmpStr);
      return keys.map((k) => input[k]);
    }
    case 'merge': {
      checkArgs(args, ['a', 'b'], ['a', 'b']);
      const a = getObjArg(args, 'a');
      const b = getObjArg(args, 'b');
      const out = Object.create(null);
      for (const k of Object.keys(a)) out[k] = a[k];
      for (const k of Object.keys(b)) out[k] = b[k];
      return out;
    }
    case 'pick': {
      checkArgs(args, ['input', 'keys'], ['input', 'keys']);
      const input = getObjArg(args, 'input');
      const keys = getStringArray(args, 'keys');
      const out = Object.create(null);
      for (const k of keys.slice().sort(cmpStr)) {
        out[k] = Object.hasOwn(input, k) ? input[k] : null;
      }
      return out;
    }
    case 'omit': {
      checkArgs(args, ['input', 'keys'], ['input', 'keys']);
      const input = getObjArg(args, 'input');
      const keys = getStringArray(args, 'keys');
      const excluded = new Set(keys);
      const out = Object.create(null);
      for (const k of Object.keys(input)) {
        if (!excluded.has(k)) out[k] = input[k];
      }
      return out;
    }

    // ---- utilities --------------------------------------------------------
    case 'constant': {
      checkArgs(args, ['value'], ['value']);
      return args.value;
    }
    case 'equals': {
      checkArgs(args, ['a', 'b'], ['a', 'b']);
      return deepEqual(args.a, args.b);
    }
    case 'compare': {
      checkArgs(args, ['a', 'b', 'op'], ['a', 'b', 'op']);
      const cmpOp = args.op;
      if (typeof cmpOp !== 'string' || !['gt', 'gte', 'lt', 'lte'].includes(cmpOp)) {
        opFail('INVALID_ARG', 'compare op must be gt, gte, lt or lte');
      }
      const c = cmpValues(args.a, args.b);
      if (cmpOp === 'gt') return c > 0;
      if (cmpOp === 'gte') return c >= 0;
      if (cmpOp === 'lt') return c < 0;
      return c <= 0;
    }
    case 'and': {
      checkArgs(args, ['a', 'b'], ['a', 'b']);
      return truthy(args.a) && truthy(args.b);
    }
    case 'or': {
      checkArgs(args, ['a', 'b'], ['a', 'b']);
      return truthy(args.a) || truthy(args.b);
    }
    case 'not': {
      checkArgs(args, ['a'], ['a']);
      return !truthy(args.a);
    }
    case 'type': {
      checkArgs(args, ['value'], ['value']);
      return typeName(args.value);
    }

    // ---- test operation ---------------------------------------------------
    case 'fail_until': {
      checkArgs(args, ['failures', 'value'], ['failures', 'value']);
      const failures = getInt(args, 'failures');
      if (failures < 0n || failures > 10n) {
        opFail('INVALID_ARG', 'failures must be in 0..10');
      }
      const attempt = (ctx.counters.get(stepId) || 0) + 1;
      ctx.counters.set(stepId, attempt);
      if (attempt <= Number(failures)) transientFail('fail_until attempt ' + attempt);
      return args.value;
    }

    default:
      opFail('INVALID_WORKFLOW', 'unknown operation ' + op);
  }
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------

function executeStep(step, ctx) {
  for (let attempt = 1; attempt <= step.maxAttempts; attempt++) {
    try {
      const resolved = resolveValue(step.args, step, ctx);
      return applyOp(step.op, resolved, ctx, step.id);
    } catch (e) {
      if (!(e instanceof CodedError)) throw e;
      if (e.transient) {
        if (attempt < step.maxAttempts) continue;
        throw new CodedError('STEP_FAILED', 'step failed after ' + step.maxAttempts + ' attempt(s)');
      }
      throw e;
    }
  }
  throw new CodedError('STEP_FAILED', 'step failed');
}

function execute(wf) {
  const ctx = {
    byId: wf.byId,
    results: Object.create(null),
    finished: new Set(),
    skippedSet: new Set(),
    counters: new Map(),
  };
  const order = [];
  const skipped = [];
  const pending = wf.steps.map((s) => s);

  while (pending.length > 0) {
    // Earliest step (in document order) whose dependencies have all finished.
    let pick = -1;
    for (let i = 0; i < pending.length; i++) {
      const step = pending[i];
      let ready = true;
      for (const d of step.deps) {
        if (!ctx.finished.has(d)) {
          ready = false;
          break;
        }
      }
      if (ready) {
        pick = i;
        break;
      }
    }
    if (pick === -1) {
      // Should be impossible after cycle validation.
      throw new CodedError('CYCLIC_DEPENDENCY', 'no schedulable step', false, null);
    }
    const step = pending.splice(pick, 1)[0];

    let run = true;
    if (step.when !== null) {
      try {
        run = evalCondition(step.when, step, ctx);
      } catch (e) {
        if (e instanceof CodedError) {
          e.step = step.id;
          return failureResponse(ctx.results, order, skipped, e);
        }
        throw e;
      }
    }

    if (!run) {
      ctx.results[step.id] = null;
      ctx.finished.add(step.id);
      ctx.skippedSet.add(step.id);
      order.push(step.id);
      skipped.push(step.id);
      continue;
    }

    try {
      const value = executeStep(step, ctx);
      ctx.results[step.id] = value;
      ctx.finished.add(step.id);
      order.push(step.id);
    } catch (e) {
      if (e instanceof CodedError) {
        e.step = step.id;
        return failureResponse(ctx.results, order, skipped, e);
      }
      throw e;
    }
  }

  return {
    status: 'completed',
    results: ctx.results,
    execution_order: order,
    skipped,
    error: null,
  };
}

function failureResponse(results, order, skipped, err) {
  return {
    status: 'failed',
    results,
    execution_order: order,
    skipped,
    error: {
      code: err.code,
      step: err.step === undefined ? null : err.step,
      message: err.message || err.code,
    },
  };
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

function emit(response, exitCode) {
  process.stdout.write(jsonStringify(response) + '\n');
  process.exitCode = exitCode;
}

function emitValidationError(code, step, message) {
  emit(
    {
      status: 'failed',
      results: Object.create(null),
      execution_order: [],
      skipped: [],
      error: { code, step: step === undefined ? null : step, message },
    },
    2
  );
}

function main(argv) {
  let text;
  let source;
  try {
    if (argv.length < 2 || argv[0] !== 'run') {
      emitValidationError('INVALID_WORKFLOW', null, 'usage: workflow run <workflow.json>');
      return;
    }
    source = argv[1];
    if (typeof source !== 'string' || source === '') {
      emitValidationError('INVALID_WORKFLOW', null, 'missing input document');
      return;
    }
    if (source === '-') {
      text = require('fs').readFileSync(0, 'utf8');
    } else {
      text = require('fs').readFileSync(source, 'utf8');
    }
  } catch (e) {
    emitValidationError('INVALID_WORKFLOW', null, 'unable to read input: ' + (e && e.message));
    return;
  }

  if (text.charCodeAt(0) === 0xfeff) text = text.slice(1);

  let doc;
  try {
    doc = parseJSON(text);
  } catch (e) {
    emitValidationError('INVALID_WORKFLOW', null, 'invalid JSON: ' + (e && e.message));
    return;
  }

  let wf;
  try {
    wf = validateWorkflow(doc);
  } catch (e) {
    if (e instanceof CodedError) {
      emitValidationError(e.code, e.step, e.message);
      return;
    }
    throw e;
  }

  const response = execute(wf);
  if (response.status === 'completed') {
    emit(response, 0);
  } else {
    emit(response, 1);
  }
}

try {
  main(process.argv.slice(2));
} catch (e) {
  const message = e && e.stack ? e.stack : String(e);
  try {
    process.stderr.write('internal error: ' + message + '\n');
  } catch (_) {
    /* ignore */
  }
  emit(
    {
      status: 'failed',
      results: Object.create(null),
      execution_order: [],
      skipped: [],
      error: { code: 'INTERNAL_ERROR', step: null, message: String(e && e.message ? e.message : e) },
    },
    3
  );
}
