#!/usr/bin/env node
'use strict';

/*
 * Self-test for patch.js.
 *
 * Every case is hand-checked against ../../../tasks/patch/SPEC.md.
 * Run with:  node selftest/run.js
 */

const { spawnSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

const PATCH = path.join(__dirname, '..', 'patch.js');

function deepEqual(a, b) {
  if (a === b) return true;
  if (typeof a !== typeof b) return false;
  if (a === null || b === null) return false;
  if (typeof a !== 'object') return false;

  if (Array.isArray(a)) {
    if (!Array.isArray(b) || a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) {
      if (!deepEqual(a[i], b[i])) return false;
    }
    return true;
  }
  if (Array.isArray(b)) return false;

  const ak = Object.keys(a);
  const bk = Object.keys(b);
  if (ak.length !== bk.length) return false;
  for (const k of ak) {
    if (!Object.prototype.hasOwnProperty.call(b, k)) return false;
    if (!deepEqual(a[k], b[k])) return false;
  }
  return true;
}

const cases = [];
function addCase(name, doc, patch, expected) {
  cases.push({ name, requestText: JSON.stringify({ document: doc, patch }), expected });
}
function addRawCase(name, requestText, expected) {
  cases.push({ name, requestText, expected });
}

/* ---------------- add ---------------- */

addCase('add object new key', { a: 1 }, [{ op: 'add', path: '/b', value: 2 }], {
  exit: 0,
  status: 'ok',
  document: { a: 1, b: 2 },
});
addCase('add object overwrite', { a: 1 }, [{ op: 'add', path: '/a', value: 9 }], {
  exit: 0,
  status: 'ok',
  document: { a: 9 },
});
addCase('add array middle', [1, 2, 3], [{ op: 'add', path: '/1', value: 9 }], {
  exit: 0,
  status: 'ok',
  document: [1, 9, 2, 3],
});
addCase('add array dash append', [1, 2, 3], [{ op: 'add', path: '/-', value: 4 }], {
  exit: 0,
  status: 'ok',
  document: [1, 2, 3, 4],
});
addCase('add array at length', [1, 2], [{ op: 'add', path: '/2', value: 3 }], {
  exit: 0,
  status: 'ok',
  document: [1, 2, 3],
});
addCase('add array at zero', [1, 2], [{ op: 'add', path: '/0', value: 0 }], {
  exit: 0,
  status: 'ok',
  document: [0, 1, 2],
});
addCase('add replaces whole document', { a: 1 }, [{ op: 'add', path: '', value: [1] }], {
  exit: 0,
  status: 'ok',
  document: [1],
});
addCase('add empty-string object key', {}, [{ op: 'add', path: '/', value: 1 }], {
  exit: 0,
  status: 'ok',
  document: { '': 1 },
});
addCase('add dash is a normal object key', {}, [{ op: 'add', path: '/-', value: 1 }], {
  exit: 0,
  status: 'ok',
  document: { '-': 1 },
});
addCase(
  'add nested new member',
  { a: { b: 1 } },
  [{ op: 'add', path: '/a/c', value: 2 }],
  { exit: 0, status: 'ok', document: { a: { b: 1, c: 2 } } }
);
addCase('add missing parent', { a: 1 }, [{ op: 'add', path: '/b/c', value: 1 }], {
  exit: 1,
  status: 'error',
  code: 'PATH_NOT_FOUND',
  index: 0,
});
addCase('add below scalar', { a: 1 }, [{ op: 'add', path: '/a/b', value: 1 }], {
  exit: 1,
  status: 'error',
  code: 'PATH_NOT_FOUND',
  index: 0,
});
addCase('add index beyond length', [1], [{ op: 'add', path: '/5', value: 0 }], {
  exit: 1,
  status: 'error',
  code: 'INDEX_ERROR',
  index: 0,
});
addCase('add index with leading zero', [1], [{ op: 'add', path: '/01', value: 0 }], {
  exit: 1,
  status: 'error',
  code: 'INDEX_ERROR',
  index: 0,
});
addCase('add dash as intermediate segment', [1], [{ op: 'add', path: '/-/x', value: 0 }], {
  exit: 1,
  status: 'error',
  code: 'INDEX_ERROR',
  index: 0,
});

/* ---------------- remove ---------------- */

addCase('remove object member', { a: 1, b: 2 }, [{ op: 'remove', path: '/a' }], {
  exit: 0,
  status: 'ok',
  document: { b: 2 },
});
addCase('remove array element', [1, 2, 3], [{ op: 'remove', path: '/1' }], {
  exit: 0,
  status: 'ok',
  document: [1, 3],
});
addCase('remove root is invalid', { a: 1 }, [{ op: 'remove', path: '' }], {
  exit: 1,
  status: 'error',
  code: 'INVALID_PATH',
  index: 0,
});
addCase('remove missing member', { a: 1 }, [{ op: 'remove', path: '/x' }], {
  exit: 1,
  status: 'error',
  code: 'PATH_NOT_FOUND',
  index: 0,
});
addCase('remove array index equal to length', [1], [{ op: 'remove', path: '/1' }], {
  exit: 1,
  status: 'error',
  code: 'INDEX_ERROR',
  index: 0,
});
addCase('remove dash from array', [1], [{ op: 'remove', path: '/-' }], {
  exit: 1,
  status: 'error',
  code: 'INDEX_ERROR',
  index: 0,
});
addCase('remove negative index', [1], [{ op: 'remove', path: '/-1' }], {
  exit: 1,
  status: 'error',
  code: 'INDEX_ERROR',
  index: 0,
});
addCase('remove empty-string key', { '': 1 }, [{ op: 'remove', path: '/' }], {
  exit: 0,
  status: 'ok',
  document: {},
});

/* ---------------- replace ---------------- */

addCase(
  'replace object member',
  { a: 1, b: 2 },
  [{ op: 'replace', path: '/a', value: 9 }],
  { exit: 0, status: 'ok', document: { a: 9, b: 2 } }
);
addCase('replace array element', [1, 2, 3], [{ op: 'replace', path: '/1', value: 9 }], {
  exit: 0,
  status: 'ok',
  document: [1, 9, 3],
});
addCase('replace whole document', { a: 1 }, [{ op: 'replace', path: '', value: 5 }], {
  exit: 0,
  status: 'ok',
  document: 5,
});
addCase('replace missing member', { a: 1 }, [{ op: 'replace', path: '/x', value: 2 }], {
  exit: 1,
  status: 'error',
  code: 'PATH_NOT_FOUND',
  index: 0,
});
addCase('replace array out of range', [1], [{ op: 'replace', path: '/1', value: 2 }], {
  exit: 1,
  status: 'error',
  code: 'INDEX_ERROR',
  index: 0,
});
addCase('replace below scalar', { a: 1 }, [{ op: 'replace', path: '/a/b', value: 2 }], {
  exit: 1,
  status: 'error',
  code: 'PATH_NOT_FOUND',
  index: 0,
});

/* ---------------- move ---------------- */

addCase('move object member', { a: 1 }, [{ op: 'move', from: '/a', path: '/b' }], {
  exit: 0,
  status: 'ok',
  document: { b: 1 },
});
addCase('move within array', [1, 2, 3], [{ op: 'move', from: '/0', path: '/2' }], {
  exit: 0,
  status: 'ok',
  document: [2, 3, 1],
});
addCase('move onto itself', { a: 1 }, [{ op: 'move', from: '/a', path: '/a' }], {
  exit: 1,
  status: 'error',
  code: 'INVALID_MOVE',
  index: 0,
});
addCase(
  'move into descendant',
  { a: { b: 1 } },
  [{ op: 'move', from: '/a', path: '/a/c' }],
  { exit: 1, status: 'error', code: 'INVALID_MOVE', index: 0 }
);
addCase('move missing from', { a: 1 }, [{ op: 'move', from: '/x', path: '/y' }], {
  exit: 1,
  status: 'error',
  code: 'PATH_NOT_FOUND',
  index: 0,
});
addCase('move missing from onto itself', { a: 1 }, [{ op: 'move', from: '/x', path: '/x' }], {
  exit: 1,
  status: 'error',
  code: 'PATH_NOT_FOUND',
  index: 0,
});
addCase('move root into child', { a: 1 }, [{ op: 'move', from: '', path: '/b' }], {
  exit: 1,
  status: 'error',
  code: 'INVALID_MOVE',
  index: 0,
});
addCase('move child onto ancestor', { a: { b: 1 } }, [{ op: 'move', from: '/a/b', path: '/a' }], {
  exit: 0,
  status: 'ok',
  document: { a: 1 },
});
addCase('move member to root', { a: 1, b: 2 }, [{ op: 'move', from: '/a', path: '' }], {
  exit: 0,
  status: 'ok',
  document: 1,
});
addCase('move array element to root', [1, 2], [{ op: 'move', from: '/1', path: '' }], {
  exit: 0,
  status: 'ok',
  document: 2,
});

/* ---------------- copy ---------------- */

addCase(
  'copy object member',
  { a: { b: 1 } },
  [{ op: 'copy', from: '/a', path: '/c' }],
  { exit: 0, status: 'ok', document: { a: { b: 1 }, c: { b: 1 } } }
);
addCase(
  'copy into descendant',
  { a: { b: 1 } },
  [{ op: 'copy', from: '/a', path: '/a/c' }],
  { exit: 0, status: 'ok', document: { a: { b: 1, c: { b: 1 } } } }
);
addCase('copy root into member', { a: 1 }, [{ op: 'copy', from: '', path: '/b' }], {
  exit: 0,
  status: 'ok',
  document: { a: 1, b: { a: 1 } },
});
addCase('copy missing from', { a: 1 }, [{ op: 'copy', from: '/x', path: '/y' }], {
  exit: 1,
  status: 'error',
  code: 'PATH_NOT_FOUND',
  index: 0,
});
addCase('copy array element to end', [1, 2], [{ op: 'copy', from: '/0', path: '/-' }], {
  exit: 0,
  status: 'ok',
  document: [1, 2, 1],
});

/* ---------------- test ---------------- */

addCase(
  'test nested equal',
  { a: { b: [1, { c: null }] } },
  [{ op: 'test', path: '/a', value: { b: [1, { c: null }] } }],
  { exit: 0, status: 'ok', document: { a: { b: [1, { c: null }] } } }
);
addCase('test whole document', { a: 1 }, [{ op: 'test', path: '', value: { a: 1 } }], {
  exit: 0,
  status: 'ok',
  document: { a: 1 },
});
addCase('test mismatch', { a: 1 }, [{ op: 'test', path: '/a', value: 2 }], {
  exit: 1,
  status: 'error',
  code: 'TEST_FAILED',
  index: 0,
});
addCase('test type mismatch', { a: 1 }, [{ op: 'test', path: '/a', value: '1' }], {
  exit: 1,
  status: 'error',
  code: 'TEST_FAILED',
  index: 0,
});
addCase('test array order matters', { a: [1, 2] }, [{ op: 'test', path: '/a', value: [2, 1] }], {
  exit: 1,
  status: 'error',
  code: 'TEST_FAILED',
  index: 0,
});
addCase(
  'test object key set matters',
  { a: { x: 1, y: 2 } },
  [{ op: 'test', path: '/a', value: { x: 1, z: 2 } }],
  { exit: 1, status: 'error', code: 'TEST_FAILED', index: 0 }
);
addCase('test missing path', { a: 1 }, [{ op: 'test', path: '/x', value: 1 }], {
  exit: 1,
  status: 'error',
  code: 'PATH_NOT_FOUND',
  index: 0,
});
addCase('test whole document mismatch', { a: 1 }, [{ op: 'test', path: '', value: { a: 2 } }], {
  exit: 1,
  status: 'error',
  code: 'TEST_FAILED',
  index: 0,
});
addCase('test null member', { a: null }, [{ op: 'test', path: '/a', value: null }], {
  exit: 0,
  status: 'ok',
  document: { a: null },
});
addCase('test missing is not null', { a: null }, [{ op: 'test', path: '/b', value: null }], {
  exit: 1,
  status: 'error',
  code: 'PATH_NOT_FOUND',
  index: 0,
});
addCase('test leading-zero array index', [1], [{ op: 'test', path: '/01', value: 1 }], {
  exit: 1,
  status: 'error',
  code: 'INDEX_ERROR',
  index: 0,
});

/* ---------------- pointers / escapes ---------------- */

addCase(
  'pointer escapes ~1 and ~0',
  { 'a/b': { '~key': 1 } },
  [{ op: 'remove', path: '/a~1b/~0key' }],
  { exit: 0, status: 'ok', document: { 'a/b': {} } }
);
addCase(
  'pointer ~01 decodes to ~1',
  { '~1': 1 },
  [{ op: 'test', path: '/~01', value: 1 }],
  { exit: 0, status: 'ok', document: { '~1': 1 } }
);
addCase('invalid ~ escape', { a: 1 }, [{ op: 'remove', path: '/a~2b' }], {
  exit: 1,
  status: 'error',
  code: 'INVALID_PATH',
  index: 0,
});
addCase('trailing ~ escape', {}, [{ op: 'add', path: '/a~', value: 1 }], {
  exit: 1,
  status: 'error',
  code: 'INVALID_PATH',
  index: 0,
});
addCase('pointer without leading slash', { a: 1 }, [{ op: 'remove', path: 'a' }], {
  exit: 1,
  status: 'error',
  code: 'INVALID_PATH',
  index: 0,
});
addCase(
  'nested pointer through arrays and objects',
  { a: [{ b: { c: 1 } }] },
  [
    { op: 'test', path: '/a/0/b/c', value: 1 },
    { op: 'add', path: '/a/0/b/d', value: 2 },
    { op: 'replace', path: '/a/0/b/c', value: 3 },
  ],
  { exit: 0, status: 'ok', document: { a: [{ b: { c: 3, d: 2 } }] } }
);
addCase('array intermediate index out of range', { a: [1] }, [{ op: 'remove', path: '/a/2/x' }], {
  exit: 1,
  status: 'error',
  code: 'INDEX_ERROR',
  index: 0,
});

/* ---------------- envelope / op shape ---------------- */

addRawCase('unknown op', '{"document":{"a":1},"patch":[{"op":"bogus","path":"/a"}]}', {
  exit: 1,
  status: 'error',
  code: 'UNKNOWN_OP',
  index: 0,
});
addRawCase('missing patch', '{"document":{}}', {
  exit: 2,
  status: 'error',
  code: 'INVALID_REQUEST',
  index: null,
});
addRawCase('missing document', '{"patch":[]}', {
  exit: 2,
  status: 'error',
  code: 'INVALID_REQUEST',
  index: null,
});
addRawCase('unknown top-level field', '{"document":{},"patch":[],"x":1}', {
  exit: 2,
  status: 'error',
  code: 'INVALID_REQUEST',
  index: null,
});
addRawCase('patch not an array', '{"document":{},"patch":{}}', {
  exit: 2,
  status: 'error',
  code: 'INVALID_REQUEST',
  index: null,
});
addRawCase('request is not an object', '[]', {
  exit: 2,
  status: 'error',
  code: 'INVALID_REQUEST',
  index: null,
});
addRawCase('malformed JSON', '{"document":', {
  exit: 2,
  status: 'error',
  code: 'INVALID_REQUEST',
  index: null,
});
addRawCase('operation is not an object', '{"document":{},"patch":[1]}', {
  exit: 2,
  status: 'error',
  code: 'INVALID_REQUEST',
  index: 0,
});
addRawCase('operation missing op', '{"document":{},"patch":[{"path":"/a"}]}', {
  exit: 2,
  status: 'error',
  code: 'INVALID_REQUEST',
  index: 0,
});
addRawCase('operation missing path', '{"document":{},"patch":[{"op":"add","value":1}]}', {
  exit: 2,
  status: 'error',
  code: 'INVALID_REQUEST',
  index: 0,
});
addRawCase(
  'unexpected field for remove',
  '{"document":{"a":1},"patch":[{"op":"remove","path":"/a","value":1}]}',
  { exit: 2, status: 'error', code: 'INVALID_REQUEST', index: 0 }
);
addRawCase('add missing value', '{"document":{},"patch":[{"op":"add","path":"/a"}]}', {
  exit: 2,
  status: 'error',
  code: 'INVALID_REQUEST',
  index: 0,
});
addRawCase('move missing from', '{"document":{},"patch":[{"op":"move","path":"/a"}]}', {
  exit: 2,
  status: 'error',
  code: 'INVALID_REQUEST',
  index: 0,
});
addRawCase(
  'move from is not a string',
  '{"document":{},"patch":[{"op":"move","path":"/a","from":5}]}',
  { exit: 2, status: 'error', code: 'INVALID_REQUEST', index: 0 }
);
addRawCase(
  'invalid op shape at index 1',
  '{"document":{"a":1},"patch":[{"op":"add","path":"/b","value":2},' +
    '{"op":"remove","path":"/a","value":1}]}',
  { exit: 2, status: 'error', code: 'INVALID_REQUEST', index: 1 }
);
addRawCase(
  'unknown op at index 1',
  '{"document":{"a":1},"patch":[{"op":"add","path":"/b","value":2},' +
    '{"op":"bogus","path":"/a"}]}',
  { exit: 1, status: 'error', code: 'UNKNOWN_OP', index: 1 }
);

/* ---------------- indices, atomicity, misc ---------------- */

addCase(
  'second operation fails: index 1',
  { a: 1 },
  [
    { op: 'add', path: '/b', value: 2 },
    { op: 'remove', path: '/x' },
  ],
  { exit: 1, status: 'error', code: 'PATH_NOT_FOUND', index: 1 }
);
addCase(
  'third operation fails: index 2',
  { a: 1 },
  [
    { op: 'add', path: '/b', value: 2 },
    { op: 'replace', path: '/a', value: 3 },
    { op: 'test', path: '/a', value: 4 },
  ],
  { exit: 1, status: 'error', code: 'TEST_FAILED', index: 2 }
);
addCase(
  'patch is atomic (no document on error)',
  { a: 1 },
  [
    { op: 'remove', path: '/a' },
    { op: 'remove', path: '/x' },
  ],
  { exit: 1, status: 'error', code: 'PATH_NOT_FOUND', index: 1, noDocument: true }
);
addCase('empty patch', { a: 1 }, [], { exit: 0, status: 'ok', document: { a: 1 } });
addCase('null document replaced at root', null, [{ op: 'add', path: '', value: { x: 1 } }], {
  exit: 0,
  status: 'ok',
  document: { x: 1 },
});
addCase('scalar document replaced at root', 5, [{ op: 'replace', path: '', value: 6 }], {
  exit: 0,
  status: 'ok',
  document: 6,
});
addCase('huge array index', [1], [{ op: 'add', path: '/999999999999999999999', value: 1 }], {
  exit: 1,
  status: 'error',
  code: 'INDEX_ERROR',
  index: 0,
});
addCase('unicode object key', { 'é': 1 }, [{ op: 'test', path: '/é', value: 1 }], {
  exit: 0,
  status: 'ok',
  document: { 'é': 1 },
});

/* Number equality is by value, not by lexeme. */
addRawCase(
  'number equality across lexemes',
  '{"document":{"a":1.0,"b":1e2,"c":1E+2,"d":0,"e":9007199254740993},' +
    '"patch":[{"op":"test","path":"/a","value":1},{"op":"test","path":"/b","value":100},' +
    '{"op":"test","path":"/c","value":100},{"op":"test","path":"/d","value":-0},' +
    '{"op":"test","path":"/e","value":9007199254740993}]}',
  { exit: 0, status: 'ok' }
);
addRawCase(
  'large integers are not conflated',
  '{"document":{"a":9007199254740993},"patch":[{"op":"test","path":"/a","value":9007199254740992}]}',
  { exit: 1, status: 'error', code: 'TEST_FAILED', index: 0 }
);
addRawCase(
  'add preserves large integer lexeme',
  '{"document":{},"patch":[{"op":"add","path":"/a","value":9007199254740993}]}',
  { exit: 0, status: 'ok', stdoutRaw: '{"status":"ok","document":{"a":9007199254740993}}' }
);
addRawCase(
  '__proto__ is an ordinary key',
  '{"document":{},"patch":[{"op":"add","path":"/__proto__","value":{"x":1}}]}',
  { exit: 0, status: 'ok', stdoutRaw: '{"status":"ok","document":{"__proto__":{"x":1}}}' }
);

/* ---------------- run ---------------- */

let passed = 0;
const failures = [];

for (const testCase of cases) {
  const result = spawnSync(process.execPath, [PATCH, 'apply', '-'], {
    input: testCase.requestText,
    encoding: 'utf8',
  });

  const problems = [];
  if (result.status !== testCase.expected.exit) {
    problems.push(
      'exit: expected ' + testCase.expected.exit + ', got ' + result.status
    );
  }

  let parsed = null;
  try {
    parsed = JSON.parse(result.stdout);
  } catch (err) {
    problems.push('stdout is not a single JSON document: ' + JSON.stringify(result.stdout));
  }

  if (parsed) {
    if (parsed.status !== testCase.expected.status) {
      problems.push('status: expected ' + testCase.expected.status + ', got ' + parsed.status);
    }

    if (testCase.expected.status === 'ok') {
      if (
        Object.prototype.hasOwnProperty.call(testCase.expected, 'document') &&
        !deepEqual(parsed.document, testCase.expected.document)
      ) {
        problems.push(
          'document: expected ' +
            JSON.stringify(testCase.expected.document) +
            ', got ' +
            JSON.stringify(parsed.document)
        );
      }
      if (
        Object.prototype.hasOwnProperty.call(testCase.expected, 'stdoutRaw') &&
        result.stdout.trim() !== testCase.expected.stdoutRaw
      ) {
        problems.push(
          'stdout raw: expected ' +
            testCase.expected.stdoutRaw +
            ', got ' +
            result.stdout.trim()
        );
      }
    } else {
      if (parsed.code !== testCase.expected.code) {
        problems.push('code: expected ' + testCase.expected.code + ', got ' + parsed.code);
      }
      if (parsed.index !== testCase.expected.index) {
        problems.push('index: expected ' + testCase.expected.index + ', got ' + parsed.index);
      }
      if (typeof parsed.message !== 'string') {
        problems.push('message is not a string');
      }
      if (testCase.expected.noDocument && Object.prototype.hasOwnProperty.call(parsed, 'document')) {
        problems.push('error response unexpectedly contains a document');
      }
    }
  }

  if (problems.length === 0) {
    passed++;
  } else {
    failures.push({ name: testCase.name, problems, stdout: result.stdout, stderr: result.stderr });
  }
}

/* File-argument mode (as opposed to stdin). */
{
  const tmp = path.join(os.tmpdir(), 'patch-selftest-' + process.pid + '.json');
  fs.writeFileSync(tmp, '{"document":{"a":1},"patch":[{"op":"add","path":"/b","value":2}]}');
  const result = spawnSync(process.execPath, [PATCH, 'apply', tmp], { encoding: 'utf8' });
  fs.unlinkSync(tmp);

  const problems = [];
  if (result.status !== 0) problems.push('exit: expected 0, got ' + result.status);
  try {
    const parsed = JSON.parse(result.stdout);
    if (!deepEqual(parsed, { status: 'ok', document: { a: 1, b: 2 } })) {
      problems.push('unexpected output: ' + result.stdout);
    }
  } catch (err) {
    problems.push('stdout is not JSON: ' + JSON.stringify(result.stdout));
  }

  if (problems.length === 0) passed++;
  else failures.push({ name: 'file argument mode', problems, stdout: result.stdout, stderr: result.stderr });
}

/* Missing file -> INVALID_REQUEST, exit 2. */
{
  const result = spawnSync(
    process.execPath,
    [PATCH, 'apply', '/nonexistent/definitely-not-here.json'],
    { encoding: 'utf8' }
  );
  const problems = [];
  if (result.status !== 2) problems.push('exit: expected 2, got ' + result.status);
  try {
    const parsed = JSON.parse(result.stdout);
    if (parsed.code !== 'INVALID_REQUEST' || parsed.index !== null) {
      problems.push('unexpected output: ' + result.stdout);
    }
  } catch (err) {
    problems.push('stdout is not JSON: ' + JSON.stringify(result.stdout));
  }
  if (problems.length === 0) passed++;
  else failures.push({ name: 'missing request file', problems, stdout: result.stdout, stderr: result.stderr });
}

const total = cases.length + 2;
if (failures.length === 0) {
  console.log(`selftest: ${passed}/${total} passed`);
  process.exitCode = 0;
} else {
  console.log(`selftest: ${passed}/${total} passed, ${failures.length} failed`);
  for (const failure of failures) {
    console.log(`\nFAIL: ${failure.name}`);
    for (const problem of failure.problems) console.log('  - ' + problem);
    if (failure.stdout) console.log('  stdout: ' + failure.stdout.trim());
    if (failure.stderr) console.log('  stderr: ' + failure.stderr.trim());
  }
  process.exitCode = 1;
}
