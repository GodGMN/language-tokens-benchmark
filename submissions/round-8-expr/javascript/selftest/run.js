#!/usr/bin/env node
'use strict';

/*
 * Self-test driver for expr.js.
 *
 * Runs every case in selftest/cases.js through the real CLI (spawning
 * `node expr.js eval <file>` or `node expr.js eval -` for stdin cases) and
 * compares:
 *   - exit code
 *   - stdout is exactly one valid JSON document (no extra output)
 *   - the response document (message excluded, because SPEC.md says the
 *     message text is free-form and never compared)
 *
 * Usage: node selftest/run.js [--verbose]
 */

const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawnSync } = require('child_process');
const { parseJson, jsonSerialize } = require('../expr.js');

const cases = require('./cases.js');
const verbose = process.argv.includes('--verbose');
const cliPath = path.join(__dirname, '..', 'expr.js');

function canon(value) {
  if (value === null) return 'null';
  const t = typeof value;
  if (t === 'bigint') return value.toString();
  if (t === 'boolean') return value ? 'true' : 'false';
  if (t === 'string') return JSON.stringify(value);
  if (t === 'number') return JSON.stringify(value);
  if (Array.isArray(value)) return '[' + value.map(canon).join(',') + ']';
  const keys = Object.keys(value).sort();
  return '{' + keys.map((k) => JSON.stringify(k) + ':' + canon(value[k])).join(',') + '}';
}

function stripMeta(value) {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) return value;
  if (!Object.prototype.hasOwnProperty.call(value, 'message')) return value;
  const copy = Object.create(null);
  for (const k of Object.keys(value)) {
    if (k !== 'message') copy[k] = value[k];
  }
  return copy;
}

const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'expr-selftest-'));

let passed = 0;
const failures = [];

cases.forEach((test, index) => {
  const requestText = test.request_text !== undefined
    ? test.request_text
    : jsonSerialize(test.request);

  const args = ['eval'];
  let input;
  if (test.stdin) {
    args.push('-');
    input = requestText;
  } else {
    const file = path.join(tmpDir, 'request-' + index + '.json');
    fs.writeFileSync(file, requestText);
    args.push(file);
  }

  const result = spawnSync(process.execPath, [cliPath].concat(args), {
    input,
    encoding: 'utf8',
  });

  const problems = [];

  if (result.status !== test.exit) {
    problems.push('exit code ' + result.status + ' != expected ' + test.exit);
  }
  if (result.stderr !== '') {
    problems.push('unexpected stderr: ' + JSON.stringify(result.stderr));
  }

  let actual;
  try {
    actual = parseJson(result.stdout);
  } catch (e) {
    problems.push('stdout is not a single JSON document: ' + JSON.stringify(result.stdout));
  }

  if (actual !== undefined) {
    try {
      JSON.parse(result.stdout);
    } catch (e) {
      problems.push('stdout is not accepted by strict JSON.parse');
    }
    if (actual !== null && typeof actual === 'object' && !Array.isArray(actual)) {
      if (actual.status === 'error' && typeof actual.message !== 'string') {
        problems.push('error response without a string message');
      }
    }
    const expectedCanonical = canon(stripMeta(test.expected));
    const actualCanonical = canon(stripMeta(actual));
    if (expectedCanonical !== actualCanonical) {
      problems.push(
        'response mismatch\n    expected: ' + expectedCanonical +
        '\n    actual:   ' + actualCanonical
      );
    }
  }

  if (problems.length === 0) {
    passed++;
    if (verbose) {
      console.log('ok   ' + test.name + ' -> ' + result.stdout.trim());
    }
  } else {
    failures.push({ test, problems, result });
  }
});

try {
  fs.rmSync(tmpDir, { recursive: true, force: true });
} catch (e) {
  /* best effort */
}

for (const failure of failures) {
  console.log('FAIL ' + failure.test.name);
  for (const problem of failure.problems) {
    console.log('  - ' + problem);
  }
}

console.log('');
console.log('selftest: ' + passed + '/' + cases.length + ' passed, ' + failures.length + ' failed');
process.exitCode = failures.length === 0 ? 0 : 1;
