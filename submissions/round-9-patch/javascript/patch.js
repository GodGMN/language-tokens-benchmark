#!/usr/bin/env node
'use strict';

/*
 * JSON Patch engine (RFC 6902 style) with RFC 6901 JSON Pointer paths.
 * See ../../../tasks/patch/SPEC.md.
 *
 * The only dependency is the Node.js standard library.
 */

const fs = require('fs');

/* ------------------------------------------------------------------ */
/* Errors                                                              */
/* ------------------------------------------------------------------ */

class PatchError extends Error {
  constructor(code, message) {
    super(message || code);
    this.name = 'PatchError';
    this.code = code;
  }
}

function badRequest(message) {
  return new PatchError('INVALID_REQUEST', message);
}

/* ------------------------------------------------------------------ */
/* JSON values                                                         */
/*                                                                     */
/* Objects are represented as Map, so that keys such as "__proto__"    */
/* are ordinary keys.  Numbers are represented by Num, preserving the  */
/* original JSON lexeme (important for integers beyond 2^53 and for    */
/* exact round-tripping).                                               */
/* ------------------------------------------------------------------ */

class Num {
  constructor(raw) {
    this.raw = raw;
  }
}

function parseJson(text) {
  return JSON.parse(text, function reviver(key, value, context) {
    if (Array.isArray(value)) return value;
    if (value !== null && typeof value === 'object') {
      return new Map(Object.entries(value));
    }
    if (typeof value === 'number') {
      const raw =
        context && typeof context.source === 'string'
          ? context.source
          : String(value);
      return new Num(raw);
    }
    return value;
  });
}

function serialize(value) {
  if (value === null) return 'null';
  if (value instanceof Num) return value.raw;

  const t = typeof value;
  if (t === 'number') {
    if (!Number.isFinite(value)) {
      throw new PatchError('INTERNAL_ERROR', 'non-finite number');
    }
    return JSON.stringify(value);
  }
  if (t === 'string' || t === 'boolean') return JSON.stringify(value);

  if (Array.isArray(value)) {
    const parts = new Array(value.length);
    for (let i = 0; i < value.length; i++) parts[i] = serialize(value[i]);
    return '[' + parts.join(',') + ']';
  }

  if (value instanceof Map) {
    const parts = [];
    for (const [k, v] of value) {
      parts.push(JSON.stringify(k) + ':' + serialize(v));
    }
    return '{' + parts.join(',') + '}';
  }

  throw new PatchError('INTERNAL_ERROR', 'cannot serialize value');
}

/* ------------------------------------------------------------------ */
/* Structural equality                                                 */
/* ------------------------------------------------------------------ */

// Normalize a JSON number lexeme to (sign, coefficient, decimal exponent).
// value = sign * coefficient * 10^exponent, with trailing zeroes removed
// from the coefficient.  Zero is canonicalized regardless of its sign.
function normalizeNumber(raw) {
  let s = raw;
  let negative = false;
  if (s.charAt(0) === '-') {
    negative = true;
    s = s.slice(1);
  }

  let exponent = 0n;
  const ePos = s.search(/[eE]/);
  if (ePos >= 0) {
    let expText = s.slice(ePos + 1);
    s = s.slice(0, ePos);
    let expNegative = false;
    if (expText.charAt(0) === '+') {
      expText = expText.slice(1);
    } else if (expText.charAt(0) === '-') {
      expNegative = true;
      expText = expText.slice(1);
    }
    exponent = BigInt(expText);
    if (expNegative) exponent = -exponent;
  }

  const dot = s.indexOf('.');
  let digits;
  if (dot >= 0) {
    digits = s.slice(0, dot) + s.slice(dot + 1);
    exponent -= BigInt(s.length - dot - 1);
  } else {
    digits = s;
  }

  let start = 0;
  while (start < digits.length && digits.charAt(start) === '0') start++;
  digits = digits.slice(start);
  if (digits === '') return { negative: false, coefficient: 0n, exponent: 0n };

  let trailing = 0;
  while (digits.charAt(digits.length - 1) === '0') {
    digits = digits.slice(0, -1);
    trailing++;
  }
  exponent += BigInt(trailing);

  return { negative, coefficient: BigInt(digits), exponent };
}

function numbersEqual(a, b) {
  const x = normalizeNumber(a.raw);
  const y = normalizeNumber(b.raw);
  if (x.coefficient === 0n || y.coefficient === 0n) {
    return x.coefficient === y.coefficient;
  }
  return (
    x.negative === y.negative &&
    x.coefficient === y.coefficient &&
    x.exponent === y.exponent
  );
}

function deepEqual(a, b) {
  if (a instanceof Num || b instanceof Num) {
    return a instanceof Num && b instanceof Num && numbersEqual(a, b);
  }

  if (a === null || b === null) return a === b;

  const ta = typeof a;
  const tb = typeof b;
  if (ta !== tb) return false;

  if (ta === 'string' || ta === 'boolean') return a === b;
  if (ta === 'number') return a === b; // defensive fallback

  if (Array.isArray(a)) {
    if (!Array.isArray(b) || a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) {
      if (!deepEqual(a[i], b[i])) return false;
    }
    return true;
  }

  if (a instanceof Map) {
    if (!(b instanceof Map) || a.size !== b.size) return false;
    for (const [key, value] of a) {
      if (!b.has(key)) return false;
      if (!deepEqual(value, b.get(key))) return false;
    }
    return true;
  }

  return false;
}

function deepClone(value) {
  if (value === null || typeof value !== 'object' || value instanceof Num) {
    return value;
  }
  if (Array.isArray(value)) {
    const out = new Array(value.length);
    for (let i = 0; i < value.length; i++) out[i] = deepClone(value[i]);
    return out;
  }
  if (value instanceof Map) {
    const out = new Map();
    for (const [key, val] of value) out.set(key, deepClone(val));
    return out;
  }
  return value;
}

/* ------------------------------------------------------------------ */
/* JSON Pointer (RFC 6901)                                             */
/* ------------------------------------------------------------------ */

function parsePointer(raw) {
  if (typeof raw !== 'string') {
    throw badRequest('pointer must be a string');
  }
  if (raw === '') return [];
  if (raw.charAt(0) !== '/') {
    throw new PatchError('INVALID_PATH', 'pointer must be empty or start with /');
  }

  // A '~' must be followed by '0' or '1'.
  for (let i = 0; i < raw.length; i++) {
    if (raw.charAt(i) === '~') {
      const next = raw.charAt(i + 1);
      if (next !== '0' && next !== '1') {
        throw new PatchError('INVALID_PATH', 'invalid ~ escape in pointer');
      }
    }
  }

  const rawSegments = raw.split('/').slice(1);
  const segments = new Array(rawSegments.length);
  for (let i = 0; i < rawSegments.length; i++) {
    // Order matters: ~1 first, then ~0.
    segments[i] = rawSegments[i].replace(/~1/g, '/').replace(/~0/g, '~');
  }
  return segments;
}

function segmentsEqual(a, b) {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) {
    if (a[i] !== b[i]) return false;
  }
  return true;
}

function isStrictPrefix(prefix, full) {
  if (prefix.length >= full.length) return false;
  for (let i = 0; i < prefix.length; i++) {
    if (prefix[i] !== full[i]) return false;
  }
  return true;
}

// Array index grammar: "0" or [1-9][0-9]*.  "-" is handled by callers that
// allow append.  Anything else is INDEX_ERROR.
function parseArrayIndex(segment) {
  if (segment === '0') return 0;
  if (!/^[1-9][0-9]*$/.test(segment)) {
    throw new PatchError('INDEX_ERROR', 'invalid array index');
  }
  return Number(segment);
}

/* ------------------------------------------------------------------ */
/* Path resolution and operations                                      */
/* ------------------------------------------------------------------ */

function resolveParent(document, segments) {
  let current = document;
  for (let i = 0; i < segments.length - 1; i++) {
    const segment = segments[i];
    if (Array.isArray(current)) {
      const index = parseArrayIndex(segment);
      if (index >= current.length) {
        throw new PatchError('INDEX_ERROR', 'array index out of range');
      }
      current = current[index];
    } else if (current instanceof Map) {
      if (!current.has(segment)) {
        throw new PatchError('PATH_NOT_FOUND', 'missing object member');
      }
      current = current.get(segment);
    } else {
      throw new PatchError('PATH_NOT_FOUND', 'cannot traverse a scalar');
    }
  }
  return current;
}

function getValue(document, segments) {
  if (segments.length === 0) return document;

  const parent = resolveParent(document, segments);
  const last = segments[segments.length - 1];

  if (Array.isArray(parent)) {
    const index = parseArrayIndex(last);
    if (index >= parent.length) {
      throw new PatchError('INDEX_ERROR', 'array index out of range');
    }
    return parent[index];
  }
  if (parent instanceof Map) {
    if (!parent.has(last)) {
      throw new PatchError('PATH_NOT_FOUND', 'missing object member');
    }
    return parent.get(last);
  }
  throw new PatchError('PATH_NOT_FOUND', 'cannot traverse a scalar');
}

function addAt(document, segments, value) {
  if (segments.length === 0) return value;

  const parent = resolveParent(document, segments);
  const last = segments[segments.length - 1];

  if (Array.isArray(parent)) {
    if (last === '-') {
      parent.push(value);
      return document;
    }
    const index = parseArrayIndex(last);
    if (index > parent.length) {
      throw new PatchError('INDEX_ERROR', 'array index out of range');
    }
    parent.splice(index, 0, value);
    return document;
  }
  if (parent instanceof Map) {
    parent.set(last, value);
    return document;
  }
  throw new PatchError('PATH_NOT_FOUND', 'cannot add below a scalar');
}

function removeAt(document, segments) {
  if (segments.length === 0) {
    throw new PatchError('INVALID_PATH', 'cannot remove the document root');
  }

  const parent = resolveParent(document, segments);
  const last = segments[segments.length - 1];

  if (Array.isArray(parent)) {
    const index = parseArrayIndex(last);
    if (index >= parent.length) {
      throw new PatchError('INDEX_ERROR', 'array index out of range');
    }
    parent.splice(index, 1);
    return document;
  }
  if (parent instanceof Map) {
    if (!parent.has(last)) {
      throw new PatchError('PATH_NOT_FOUND', 'missing object member');
    }
    parent.delete(last);
    return document;
  }
  throw new PatchError('PATH_NOT_FOUND', 'cannot remove below a scalar');
}

function replaceAt(document, segments, value) {
  if (segments.length === 0) return value;

  const parent = resolveParent(document, segments);
  const last = segments[segments.length - 1];

  if (Array.isArray(parent)) {
    const index = parseArrayIndex(last);
    if (index >= parent.length) {
      throw new PatchError('INDEX_ERROR', 'array index out of range');
    }
    parent[index] = value;
    return document;
  }
  if (parent instanceof Map) {
    if (!parent.has(last)) {
      throw new PatchError('PATH_NOT_FOUND', 'missing object member');
    }
    parent.set(last, value);
    return document;
  }
  throw new PatchError('PATH_NOT_FOUND', 'cannot replace below a scalar');
}

/* ------------------------------------------------------------------ */
/* Operation validation and application                                */
/* ------------------------------------------------------------------ */

const OP_EXTRA_FIELDS = {
  add: ['value'],
  remove: [],
  replace: ['value'],
  move: ['from'],
  copy: ['from'],
  test: ['value'],
};

const KNOWN_OPS = new Set(Object.keys(OP_EXTRA_FIELDS));

function applyOperation(document, operation) {
  if (!(operation instanceof Map)) {
    throw badRequest('operation must be an object');
  }
  if (!operation.has('op') || typeof operation.get('op') !== 'string') {
    throw badRequest('operation requires a string "op"');
  }
  if (!operation.has('path') || typeof operation.get('path') !== 'string') {
    throw badRequest('operation requires a string "path"');
  }

  const name = operation.get('op');
  if (!KNOWN_OPS.has(name)) {
    throw new PatchError('UNKNOWN_OP', 'unknown operation: ' + name);
  }

  const extras = OP_EXTRA_FIELDS[name];
  const allowed = new Set(['op', 'path']);
  for (const field of extras) allowed.add(field);

  for (const key of operation.keys()) {
    if (!allowed.has(key)) {
      throw badRequest('unexpected field "' + key + '" for op "' + name + '"');
    }
  }
  for (const field of extras) {
    if (!operation.has(field)) {
      throw badRequest('missing field "' + field + '" for op "' + name + '"');
    }
  }

  if (
    (name === 'move' || name === 'copy') &&
    typeof operation.get('from') !== 'string'
  ) {
    throw badRequest('"from" must be a string');
  }

  const pathText = operation.get('path');

  switch (name) {
    case 'add':
      return addAt(document, parsePointer(pathText), operation.get('value'));

    case 'remove':
      return removeAt(document, parsePointer(pathText));

    case 'replace':
      return replaceAt(
        document,
        parsePointer(pathText),
        operation.get('value')
      );

    case 'move': {
      const pathSegments = parsePointer(pathText);
      const fromSegments = parsePointer(operation.get('from'));

      // "from" must exist before the self/descendant checks (SPEC order).
      const value = getValue(document, fromSegments);

      if (segmentsEqual(fromSegments, pathSegments)) {
        throw new PatchError('INVALID_MOVE', 'cannot move a value onto itself');
      }
      if (isStrictPrefix(fromSegments, pathSegments)) {
        throw new PatchError(
          'INVALID_MOVE',
          'cannot move a value into one of its descendants'
        );
      }

      removeAt(document, fromSegments);
      return addAt(document, pathSegments, value);
    }

    case 'copy': {
      const pathSegments = parsePointer(pathText);
      const fromSegments = parsePointer(operation.get('from'));
      const value = getValue(document, fromSegments);
      return addAt(document, pathSegments, deepClone(value));
    }

    case 'test': {
      const segments = parsePointer(pathText);
      const actual = getValue(document, segments);
      if (!deepEqual(actual, operation.get('value'))) {
        throw new PatchError('TEST_FAILED', 'test value does not match');
      }
      return document;
    }
  }

  throw badRequest('unhandled operation');
}

/* ------------------------------------------------------------------ */
/* Request handling                                                    */
/* ------------------------------------------------------------------ */

function errorOutput(code, index, message) {
  return JSON.stringify({
    status: 'error',
    code,
    index,
    message: typeof message === 'string' ? message : String(message),
  });
}

function readRequest(argv) {
  if (argv.length !== 2 || argv[0] !== 'apply') {
    return { error: badRequest('usage: patch apply <request.json|->') };
  }

  let text;
  try {
    if (argv[1] === '-') {
      text = fs.readFileSync(0, 'utf8');
    } else {
      text = fs.readFileSync(argv[1], 'utf8');
    }
  } catch (err) {
    return { error: badRequest('cannot read request: ' + err.message) };
  }

  try {
    return { request: parseJson(text) };
  } catch (err) {
    return { error: badRequest('invalid JSON request: ' + err.message) };
  }
}

function run(argv) {
  const loaded = readRequest(argv);
  if (loaded.error) {
    return {
      exitCode: 2,
      output: errorOutput('INVALID_REQUEST', null, loaded.error.message),
    };
  }

  const request = loaded.request;

  // Envelope validation: top-level errors have index null.
  try {
    if (!(request instanceof Map)) {
      throw badRequest('request must be a JSON object');
    }
    for (const key of request.keys()) {
      if (key !== 'document' && key !== 'patch') {
        throw badRequest('unexpected top-level field "' + key + '"');
      }
    }
    if (!request.has('document')) throw badRequest('missing "document"');
    if (!request.has('patch')) throw badRequest('missing "patch"');

    const patch = request.get('patch');
    if (!Array.isArray(patch)) throw badRequest('"patch" must be an array');

    let document = request.get('document');

    for (let i = 0; i < patch.length; i++) {
      try {
        document = applyOperation(document, patch[i]);
      } catch (err) {
        if (err instanceof PatchError) {
          const exitCode = err.code === 'INVALID_REQUEST' ? 2 : 1;
          return {
            exitCode,
            output: errorOutput(err.code, i, err.message),
          };
        }
        throw err;
      }
    }

    return {
      exitCode: 0,
      output: '{"status":"ok","document":' + serialize(document) + '}',
    };
  } catch (err) {
    if (err instanceof PatchError) {
      if (err.code === 'INTERNAL_ERROR') throw err;
      const exitCode = err.code === 'INVALID_REQUEST' ? 2 : 1;
      return {
        exitCode,
        output: errorOutput(err.code, null, err.message),
      };
    }
    throw err;
  }
}

function main() {
  let result;
  try {
    result = run(process.argv.slice(2));
  } catch (err) {
    process.stderr.write(
      'internal error: ' + (err && err.stack ? err.stack : String(err)) + '\n'
    );
    result = {
      exitCode: 3,
      output: errorOutput(
        'INTERNAL_ERROR',
        null,
        err && err.message ? err.message : String(err)
      ),
    };
  }

  process.stdout.write(result.output + '\n');
  process.exitCode = result.exitCode;
}

if (require.main === module) {
  main();
}

module.exports = {
  PatchError,
  Num,
  parseJson,
  serialize,
  deepEqual,
  deepClone,
  parsePointer,
  applyOperation,
  run,
};
