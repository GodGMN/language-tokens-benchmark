#!/usr/bin/env node
'use strict';

/*
 * Self-test runner for the JavaScript workflow CLI.
 * Runs every case through `node workflow.js run <file>` and compares the
 * JSON response (and exit code) with the expectations below.
 *
 * Usage: node selftest/run.js
 */

const fs = require('fs');
const path = require('path');
const { spawnSync } = require('child_process');

const CLI = path.join(__dirname, '..', 'workflow.js');

/* ---------- a tiny BigInt-preserving JSON parser ---------- */

function parseJSON(text) {
  let i = 0;
  const n = text.length;
  const ws = () => {
    while (i < n && ' \t\n\r'.includes(text[i])) i++;
  };

  function str() {
    const start = i;
    i++;
    while (i < n) {
      if (text[i] === '\\') {
        i += 2;
        continue;
      }
      if (text[i] === '"') {
        i++;
        return JSON.parse(text.slice(start, i));
      }
      i++;
    }
    throw new Error('unterminated string');
  }

  function value() {
    ws();
    const c = text[i];
    if (c === '"') return str();
    if (c === '{') {
      i++;
      const o = Object.create(null);
      ws();
      if (text[i] === '}') {
        i++;
        return o;
      }
      for (;;) {
        ws();
        const k = str();
        ws();
        if (text[i] !== ':') throw new Error('expected :');
        i++;
        o[k] = value();
        ws();
        if (text[i] === ',') {
          i++;
          continue;
        }
        if (text[i] === '}') {
          i++;
          return o;
        }
        throw new Error('expected }');
      }
    }
    if (c === '[') {
      i++;
      const a = [];
      ws();
      if (text[i] === ']') {
        i++;
        return a;
      }
      for (;;) {
        a.push(value());
        ws();
        if (text[i] === ',') {
          i++;
          continue;
        }
        if (text[i] === ']') {
          i++;
          return a;
        }
        throw new Error('expected ]');
      }
    }
    if (text.startsWith('true', i)) {
      i += 4;
      return true;
    }
    if (text.startsWith('false', i)) {
      i += 5;
      return false;
    }
    if (text.startsWith('null', i)) {
      i += 4;
      return null;
    }
    const m = /-?(?:0|[1-9]\d*)(?:\.\d+)?(?:[eE][+-]?\d+)?/y;
    m.lastIndex = i;
    const r = m.exec(text);
    if (!r) throw new Error(`bad token at ${i}`);
    i = m.lastIndex;
    return /[.eE]/.test(r[0]) ? Number(r[0]) : BigInt(r[0]);
  }

  const v = value();
  ws();
  if (i !== n) throw new Error('trailing text');
  return v;
}

/* ---------- structural comparison ---------- */

function isNum(v) {
  return typeof v === 'bigint' || typeof v === 'number';
}

function deepEqual(a, b) {
  if (isNum(a) && isNum(b)) return Number(a) === Number(b);
  if (a === null || b === null) return a === b;
  if (typeof a !== typeof b) return false;
  if (typeof a !== 'object') return a === b;
  const aa = Array.isArray(a);
  const ba = Array.isArray(b);
  if (aa !== ba) return false;
  if (aa) {
    if (a.length !== b.length) return false;
    return a.every((x, idx) => deepEqual(x, b[idx]));
  }
  const ka = Object.keys(a);
  const kb = Object.keys(b);
  if (ka.length !== kb.length) return false;
  return ka.every(
    (k) => Object.prototype.hasOwnProperty.call(b, k) && deepEqual(a[k], b[k]),
  );
}

/* ---------- cases ---------- */

const cases = [
  {
    file: 'math.json',
    exit: 0,
    status: 'completed',
    results: {
      nums: [1, 2, 3, 4, 5, 6],
      sum_all: 21,
      total: 22,
      diff: 7,
      prod: -20,
      quot: -3,
      rem: -1,
      lo: 1,
      hi: 6,
      fold: 720,
    },
    order: ['nums', 'sum_all', 'total', 'diff', 'prod', 'quot', 'rem', 'lo', 'hi', 'fold'],
    skipped: [],
    error: null,
  },
  {
    file: 'strings.json',
    exit: 0,
    status: 'completed',
    results: {
      s: ['a', 'bb', '', 'c'],
      joined: 'a-bb--c',
      upper: 'A-BB--C',
      rep: 'one 2 2',
      tr: 'hi',
      len: 5,
      cat: 'xy',
      lo: 'mixed',
      parts: ['a', 'b', 'c'],
      repchar: 'xaxbxcx',
    },
    order: ['s', 'joined', 'upper', 'rep', 'tr', 'len', 'cat', 'lo', 'parts', 'repchar'],
    skipped: [],
    error: null,
  },
  {
    file: 'objects.json',
    exit: 0,
    status: 'completed',
    results: {
      base: { b: 2, a: { x: [10, 20, 30] } },
      got: { x: [10, 20, 30] },
      missing: null,
      keys: ['a', 'b'],
      vals: [{ x: [10, 20, 30] }, 2],
      updated: { a: { x: [10, 20, 30] }, b: 2, c: true },
      merged: { a: 1, b: 2, d: 4 },
      subset: { a: { x: [10, 20, 30] }, missing: null },
      without: { a: { x: [10, 20, 30] } },
    },
    order: ['base', 'got', 'missing', 'keys', 'vals', 'updated', 'merged', 'subset', 'without'],
    skipped: [],
    error: null,
  },
  {
    file: 'arrays.json',
    exit: 0,
    status: 'completed',
    results: {
      data: [3, 1, 2, 1],
      sorted: [1, 1, 2, 3],
      uniq: [3, 1, 2],
      rev: [1, 2, 1, 3],
      part: [1, 2],
      flat: [1, 2, 3, 4, [5]],
      mapped: [13, 11, 12, 11],
      filtered: [2],
      typed: [{}],
      sorted_mixed: [1, 5, 'a', 'b', false, true, null, [1], [2], { a: 2 }, { z: 1 }],
    },
    order: ['data', 'sorted', 'uniq', 'rev', 'part', 'flat', 'mapped', 'filtered', 'typed', 'sorted_mixed'],
    skipped: [],
    error: null,
  },
  {
    file: 'skipped.json',
    exit: 0,
    status: 'completed',
    results: { x: 3, y: null, z: 7, w: null, v: 9 },
    order: ['x', 'y', 'z', 'w', 'v'],
    skipped: ['y', 'w'],
    error: null,
  },
  {
    file: 'retry.json',
    exit: 0,
    status: 'completed',
    results: { flaky: 'ok' },
    order: ['flaky'],
    skipped: [],
    error: null,
  },
  {
    file: 'error_overflow.json',
    exit: 1,
    status: 'failed',
    results: { a: 9223372036854775807n },
    order: ['a'],
    skipped: [],
    error: { code: 'ARITHMETIC_OVERFLOW', step: 'b' },
  },
  {
    file: 'error_divzero.json',
    exit: 1,
    status: 'failed',
    results: { a: 1 },
    order: ['a'],
    skipped: [],
    error: { code: 'DIVISION_BY_ZERO', step: 'b' },
  },
  {
    file: 'error_empty.json',
    exit: 1,
    status: 'failed',
    results: {},
    order: [],
    skipped: [],
    error: { code: 'EMPTY_ARRAY', step: 'm' },
  },
  {
    file: 'error_skipped_ref.json',
    exit: 1,
    status: 'failed',
    results: { x: 1, y: null },
    order: ['x', 'y'],
    skipped: ['y'],
    error: { code: 'SKIPPED_REFERENCE', step: 'z' },
  },
  {
    file: 'error_condition.json',
    exit: 1,
    status: 'failed',
    results: { x: 5 },
    order: ['x'],
    skipped: [],
    error: { code: 'INVALID_CONDITION', step: 'y' },
  },
  {
    file: 'error_invalid_arg.json',
    exit: 1,
    status: 'failed',
    results: {},
    order: [],
    skipped: [],
    error: { code: 'INVALID_ARG', step: 'a' },
  },
  {
    file: 'invalid_workflow.json',
    exit: 2,
    status: 'failed',
    results: {},
    order: [],
    skipped: [],
    error: { code: 'INVALID_WORKFLOW', step: null },
  },
  {
    file: 'unknown_dependency.json',
    exit: 2,
    status: 'failed',
    results: {},
    order: [],
    skipped: [],
    error: { code: 'UNKNOWN_DEPENDENCY', step: 'a' },
  },
  {
    file: 'cycle.json',
    exit: 2,
    status: 'failed',
    results: {},
    order: [],
    skipped: [],
    error: { code: 'CYCLIC_DEPENDENCY', step: null },
  },
];

/* ---------- runner ---------- */

let failures = 0;
let checks = 0;

function check(label, condition, detail) {
  checks++;
  if (!condition) {
    failures++;
    console.error(`  FAIL ${label}${detail ? `: ${detail}` : ''}`);
  }
}

for (const c of cases) {
  const file = path.join(__dirname, c.file);
  const r = spawnSync(process.execPath, [CLI, 'run', file], {
    encoding: 'utf8',
  });
  const name = c.file;
  let resp;
  try {
    resp = parseJSON(r.stdout);
  } catch (e) {
    failures++;
    checks++;
    console.error(`FAIL ${name}: could not parse stdout (${e.message})`);
    console.error(`  stdout: ${JSON.stringify(r.stdout)}`);
    console.error(`  stderr: ${r.stderr}`);
    continue;
  }
  const before = failures;

  check(`${name} exit code`, r.status === c.exit, `got ${r.status}, want ${c.exit}`);
  check(`${name} status`, resp.status === c.status, `got ${resp.status}`);
  check(`${name} results`, deepEqual(resp.results, c.results));
  check(`${name} execution_order`, deepEqual(resp.execution_order, c.order));
  check(`${name} skipped`, deepEqual(resp.skipped, c.skipped));
  if (c.error === null) {
    check(`${name} error null`, resp.error === null);
  } else {
    check(
      `${name} error code`,
      resp.error && resp.error.code === c.error.code,
      `got ${resp.error && resp.error.code}`,
    );
    check(
      `${name} error step`,
      resp.error && resp.error.step === c.error.step,
      `got ${JSON.stringify(resp.error && resp.error.step)}`,
    );
  }

  if (failures === before) console.log(`PASS ${name}`);
}

/* stdin mode */
{
  const input = fs.readFileSync(path.join(__dirname, 'math.json'), 'utf8');
  const r = spawnSync(process.execPath, [CLI, 'run', '-'], {
    encoding: 'utf8',
    input,
  });
  check('stdin run exit code', r.status === 0, `got ${r.status}`);
  try {
    const resp = parseJSON(r.stdout);
    check('stdin run status', resp.status === 'completed');
  } catch (e) {
    check('stdin run stdout parses', false, e.message);
  }
}

/* missing arguments -> exit 2 */
{
  const r = spawnSync(process.execPath, [CLI], { encoding: 'utf8' });
  check('no-argument exit code', r.status === 2, `got ${r.status}`);
}

console.log(`\n${checks - failures}/${checks} checks passed`);
if (failures > 0) {
  console.error(`${failures} failure(s)`);
  process.exit(1);
}
console.log('All self-tests passed.');
