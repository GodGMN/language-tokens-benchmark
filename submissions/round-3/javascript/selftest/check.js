#!/usr/bin/env node
'use strict';

/*
 * Self-test driver: runs every workflow in this folder through the CLI and
 * compares the parsed response (and exit code) with the expected value.
 *
 *   node selftest/check.js        (run from the submission folder)
 */

const path = require('path');
const { spawnSync } = require('child_process');
const util = require('util');

const ROOT = path.resolve(__dirname, '..');
const CLI = path.join(ROOT, 'workflow.js');

const cases = [
  {
    file: '01_math_pipeline.json',
    exit: 0,
    expect: {
      status: 'completed',
      results: { r: [1, 2, 3, 4, 5], s: 15, d: 3, m: 3, p: 45, mn: 3, mx: 45, sub: 42 },
      execution_order: ['r', 's', 'd', 'm', 'p', 'mn', 'mx', 'sub'],
      skipped: [],
      error: null,
    },
  },
  {
    file: '02_string_pipeline.json',
    exit: 0,
    expect: {
      status: 'completed',
      results: {
        s: '  Hello, World  ',
        t: 'Hello, World',
        l: 'hello, world',
        r: 'hello! world',
        sp: ['hello!', 'world'],
        j: 'hello!-world',
        u: 'HELLO!-WORLD',
        len: 12,
        cc: 'HELLO!-WORLD!',
      },
      execution_order: ['s', 't', 'l', 'r', 'sp', 'j', 'u', 'len', 'cc'],
      skipped: [],
      error: null,
    },
  },
  {
    file: '03_object_ops.json',
    exit: 0,
    expect: {
      status: 'completed',
      results: {
        o: { b: 2, a: 1 },
        k: ['a', 'b'],
        v: [1, 2],
        s: { b: 2, a: 1, c: 3 },
        m: { b: 2, a: 1, c: 3 },
        g: 2,
        gm: null,
        om: { a: 1, c: 3 },
        pk: { a: 1, z: null },
      },
      execution_order: ['o', 'k', 'v', 's', 'm', 'g', 'gm', 'om', 'pk'],
      skipped: [],
      error: null,
    },
  },
  {
    file: '04_skipped_step.json',
    exit: 0,
    expect: {
      status: 'completed',
      results: { x: 3, y: null },
      execution_order: ['x', 'y'],
      skipped: ['y'],
      error: null,
    },
  },
  {
    file: '05_retry.json',
    exit: 0,
    expect: {
      status: 'completed',
      results: { flaky: 'ok' },
      execution_order: ['flaky'],
      skipped: [],
      error: null,
    },
  },
  {
    file: '06_arrays.json',
    exit: 0,
    expect: {
      status: 'completed',
      results: {
        a: [5, 3, 1, 4, 2],
        srt: [1, 2, 3, 4, 5],
        rev: [5, 4, 3, 2, 1],
        u: [1, 2, 3],
        fl: [1, 2, 3, 4],
        sl: [2, 3, 4],
        mp: [11, 12, 13, 14, 15],
        flt: [1, 3, 5],
        rd: 15,
      },
      execution_order: ['a', 'srt', 'rev', 'u', 'fl', 'sl', 'mp', 'flt', 'rd'],
      skipped: [],
      error: null,
    },
  },
  {
    file: '07_references.json',
    exit: 0,
    expect: {
      status: 'completed',
      results: { o: { user: { name: 'ann', tags: ['x', 'y'] } }, n: 'ann', t: 'y' },
      execution_order: ['o', 'n', 't'],
      skipped: [],
      error: null,
    },
  },
  {
    file: '08_conditions.json',
    exit: 0,
    expect: {
      status: 'completed',
      results: { x: 10, y: 'big', z: null, w: 'again' },
      execution_order: ['x', 'y', 'z', 'w'],
      skipped: ['z'],
      error: null,
    },
  },
  {
    file: '09_error_overflow.json',
    exit: 1,
    expect: {
      status: 'failed',
      results: { ok: 'before' },
      execution_order: ['ok'],
      skipped: [],
      error: { code: 'ARITHMETIC_OVERFLOW', step: 'boom' },
    },
  },
  {
    file: '10_retry_exhausted.json',
    exit: 1,
    expect: {
      status: 'failed',
      results: {},
      execution_order: [],
      skipped: [],
      error: { code: 'STEP_FAILED', step: 'flaky' },
    },
  },
];

let failures = 0;

function normalise(resp) {
  if (resp && resp.error) {
    return {
      status: resp.status,
      results: resp.results,
      execution_order: resp.execution_order,
      skipped: resp.skipped,
      error: { code: resp.error.code, step: resp.error.step },
    };
  }
  return resp;
}

for (const c of cases) {
  const file = path.join(__dirname, c.file);
  const res = spawnSync(process.execPath, [CLI, 'run', file], { encoding: 'utf8' });
  const problems = [];
  if (res.status !== c.exit) {
    problems.push('exit code ' + res.status + ' (expected ' + c.exit + ')');
  }
  let parsed;
  try {
    parsed = JSON.parse(res.stdout);
  } catch (e) {
    problems.push('stdout is not valid JSON: ' + JSON.stringify(res.stdout));
  }
  if (parsed && !util.isDeepStrictEqual(normalise(parsed), normalise(c.expect))) {
    problems.push(
      'response mismatch\n    actual:   ' + JSON.stringify(normalise(parsed)) +
      '\n    expected: ' + JSON.stringify(normalise(c.expect))
    );
  }
  if (problems.length === 0) {
    console.log('PASS  ' + c.file);
  } else {
    failures++;
    console.log('FAIL  ' + c.file);
    for (const p of problems) console.log('      ' + p);
  }
}

// stdin smoke test: the SPEC example A document via `run -`.
const exampleA = {
  version: 1,
  steps: [
    { id: 'n', op: 'range', args: { start: 1, end: 5 } },
    { id: 'evens', op: 'filter', depends_on: ['n'], args: { input: { $ref: 'n' }, predicate: { type: 'even' } } },
    { id: 'total', op: 'sum', depends_on: ['evens'], args: { input: { $ref: 'evens' } } },
  ],
};
const stdinRes = spawnSync(process.execPath, [CLI, 'run', '-'], {
  encoding: 'utf8',
  input: JSON.stringify(exampleA),
});
const stdinExpected = {
  status: 'completed',
  results: { n: [1, 2, 3, 4, 5], evens: [2, 4], total: 6 },
  execution_order: ['n', 'evens', 'total'],
  skipped: [],
  error: null,
};
let stdinOk = stdinRes.status === 0;
try {
  stdinOk = stdinOk && util.isDeepStrictEqual(JSON.parse(stdinRes.stdout), stdinExpected);
} catch (e) {
  stdinOk = false;
}
if (stdinOk) {
  console.log('PASS  stdin (SPEC example A)');
} else {
  failures++;
  console.log('FAIL  stdin (SPEC example A)');
  console.log('      exit=' + stdinRes.status + ' stdout=' + JSON.stringify(stdinRes.stdout));
}

if (failures > 0) {
  console.log('\n' + failures + ' self-test(s) failed');
  process.exitCode = 1;
} else {
  console.log('\nAll self-tests passed');
}
