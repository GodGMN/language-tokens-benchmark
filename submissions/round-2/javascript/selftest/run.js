#!/usr/bin/env node
'use strict';

/*
 * Self-test runner.
 *
 * Each case below runs:
 *   node ../workflow.js run selftest/<file>
 * and compares the parsed JSON response (plus exit code) against a
 * hand-checked expectation.  Object key order is ignored; array order is not.
 *
 * Usage:  node selftest/run.js
 */

const { spawnSync } = require('child_process');
const path = require('path');

const ROOT = path.resolve(__dirname, '..');

const cases = [
  {
    file: 'example_a.json',
    exit: 0,
    expected: {
      status: 'completed',
      results: { n: [1, 2, 3, 4, 5], evens: [2, 4], total: 6 },
      execution_order: ['n', 'evens', 'total'],
      skipped: [],
      error: null,
    },
  },
  {
    file: 'example_b.json',
    exit: 0,
    expected: {
      status: 'completed',
      results: { x: 3, y: null },
      execution_order: ['x', 'y'],
      skipped: ['y'],
      error: null,
    },
  },
  {
    file: 'example_c.json',
    exit: 0,
    expected: {
      status: 'completed',
      results: { flaky: 'ok' },
      execution_order: ['flaky'],
      skipped: [],
      error: null,
    },
  },
  {
    file: 'math_pipeline.json',
    exit: 0,
    expected: {
      status: 'completed',
      results: {
        n: [1, 2, 3, 4, 5, 6],
        scaled: [3, 6, 9, 12, 15, 18],
        evens: [6, 12, 18],
        total: 36,
        peak: 18,
        low: 6,
        folded: 163,
      },
      execution_order: ['n', 'scaled', 'evens', 'total', 'peak', 'low', 'folded'],
      skipped: [],
      error: null,
    },
  },
  {
    file: 'string_pipeline.json',
    exit: 0,
    expected: {
      status: 'completed',
      results: {
        raw: '  Hello World  ',
        clean: 'Hello World',
        up: 'HELLO WORLD',
        parts: ['HELLO', 'WORLD'],
        dashed: 'HELLO-WORLD',
        colon: 'HELLO:WORLD',
        down: 'hello:world',
        size: 11,
        shout: 'hello:world!',
      },
      execution_order: ['raw', 'clean', 'up', 'parts', 'dashed', 'colon', 'down', 'size', 'shout'],
      skipped: [],
      error: null,
    },
  },
  {
    file: 'object_ops.json',
    exit: 0,
    expected: {
      status: 'completed',
      results: {
        base: { name: 'Ada', age: 36, tags: ['math'] },
        withCity: { name: 'Ada', age: 36, tags: ['math'], city: 'London' },
        ks: ['age', 'city', 'name', 'tags'],
        vs: [36, 'London', 'Ada', ['math']],
        age: 36,
        missing: null,
        merged: { name: 'Ada', age: 37, tags: ['math'], country: 'UK' },
        picked: { country: 'UK', name: 'Ada', nope: null },
        omitted: { name: 'Ada', age: 37, country: 'UK' },
      },
      execution_order: ['base', 'withCity', 'ks', 'vs', 'age', 'missing', 'merged', 'picked', 'omitted'],
      skipped: [],
      error: null,
    },
  },
  {
    file: 'skipped_step.json',
    exit: 0,
    expected: {
      status: 'completed',
      results: { threshold: 10, value: 3, high: null, low: 'kept' },
      execution_order: ['threshold', 'value', 'high', 'low'],
      skipped: ['high'],
      error: null,
    },
  },
  {
    file: 'retry_transient.json',
    exit: 0,
    expected: {
      status: 'completed',
      results: { flaky: 'ok' },
      execution_order: ['flaky'],
      skipped: [],
      error: null,
    },
  },
  {
    file: 'retry_exhausted.json',
    exit: 1,
    expected: {
      status: 'failed',
      results: {},
      execution_order: [],
      skipped: [],
      error: { code: 'STEP_FAILED', step: 'flaky' },
    },
  },
  {
    file: 'references.json',
    exit: 0,
    expected: {
      status: 'completed',
      results: {
        data: {
          user: { name: 'Ada', scores: [10, 20, 30] },
          items: [{ id: 'a' }, { id: 'b' }],
        },
        name: 'Ada',
        score: 20,
        item: 'b',
        nameLen: 3,
      },
      execution_order: ['data', 'name', 'score', 'item', 'nameLen'],
      skipped: [],
      error: null,
    },
  },
  {
    file: 'errors_overflow.json',
    exit: 1,
    expected: {
      status: 'failed',
      results: { maxv: 9223372036854775807 },
      execution_order: ['maxv'],
      skipped: [],
      error: { code: 'ARITHMETIC_OVERFLOW', step: 'boom' },
    },
  },
  {
    file: 'conditions_compound.json',
    exit: 0,
    expected: {
      status: 'completed',
      results: { base: 7, result: 'seven', other: null },
      execution_order: ['base', 'result', 'other'],
      skipped: ['other'],
      error: null,
    },
  },
  {
    file: 'validation_cycle.json',
    exit: 2,
    expected: {
      status: 'failed',
      results: {},
      execution_order: [],
      skipped: [],
      error: { code: 'CYCLIC_DEPENDENCY', step: null },
    },
  },
];

function deepEqual(a, b) {
  if (a === b) return true;
  if (Array.isArray(a) || Array.isArray(b)) {
    if (!Array.isArray(a) || !Array.isArray(b)) return false;
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) if (!deepEqual(a[i], b[i])) return false;
    return true;
  }
  if (a !== null && b !== null && typeof a === 'object' && typeof b === 'object') {
    const ka = Object.keys(a);
    const kb = Object.keys(b);
    if (ka.length !== kb.length) return false;
    for (const k of ka) {
      if (!Object.prototype.hasOwnProperty.call(b, k)) return false;
      if (!deepEqual(a[k], b[k])) return false;
    }
    return true;
  }
  return false;
}

let failures = 0;

for (const tc of cases) {
  const inputPath = path.join(__dirname, tc.file);
  const proc = spawnSync(process.execPath, [path.join(ROOT, 'workflow.js'), 'run', inputPath], {
    encoding: 'utf8',
  });

  let actual = null;
  let parseError = null;
  try {
    actual = JSON.parse(proc.stdout);
  } catch (e) {
    parseError = e.message;
  }

  const problems = [];
  if (parseError) problems.push('stdout is not valid JSON: ' + parseError);
  if (proc.status !== tc.exit) problems.push('exit code ' + proc.status + ' != ' + tc.exit);

  if (actual) {
    if (actual.status !== tc.expected.status) problems.push('status mismatch');
    if (!deepEqual(actual.results, tc.expected.results)) problems.push('results mismatch');
    if (!deepEqual(actual.execution_order, tc.expected.execution_order)) problems.push('execution_order mismatch');
    if (!deepEqual(actual.skipped, tc.expected.skipped)) problems.push('skipped mismatch');
    const err = actual.error === null ? null : { code: actual.error.code, step: actual.error.step };
    if (!deepEqual(err, tc.expected.error)) problems.push('error mismatch: ' + JSON.stringify(err));
  }

  if (problems.length === 0) {
    console.log('PASS ' + tc.file);
  } else {
    failures++;
    console.log('FAIL ' + tc.file);
    for (const p of problems) console.log('     ' + p);
    console.log('     stdout: ' + proc.stdout.trim());
  }
}

if (failures > 0) {
  console.log('\n' + failures + ' self-test case(s) failed');
  process.exitCode = 1;
} else {
  console.log('\nAll ' + cases.length + ' self-test cases passed');
}