#!/usr/bin/env node
'use strict';

/*
 * Self-test runner for the workflow CLI.
 *
 * Runs every selftest/workflows/*.json through `node workflow.js run <file>`
 * and compares status / results / execution_order / skipped / error.code /
 * error.step against the matching selftest/expected/*.json snapshot.  It also
 * exercises the `run -` (stdin) mode.  Exit code 0 means everything passed.
 */

const fs = require('fs');
const path = require('path');
const { spawnSync } = require('child_process');

const here = __dirname;
const root = path.resolve(here, '..');
const cli = path.join(root, 'workflow.js');
const workflows = path.join(here, 'workflows');
const expectedDir = path.join(here, 'expected');

function normalize(out) {
  return {
    status: out.status,
    results: out.results,
    execution_order: out.execution_order,
    skipped: out.skipped,
    error: out.error === null || out.error === undefined
      ? null
      : { code: out.error.code, step: out.error.step },
  };
}

function deepEqual(a, b) {
  if (a === b) return true;
  if (typeof a === 'number' && typeof b === 'number') return a === b;
  if (a === null || b === null || typeof a !== 'object' || typeof b !== 'object') return false;
  const aa = Array.isArray(a);
  const ab = Array.isArray(b);
  if (aa !== ab) return false;
  if (aa) {
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) if (!deepEqual(a[i], b[i])) return false;
    return true;
  }
  const ka = Object.keys(a);
  const kb = Object.keys(b);
  if (ka.length !== kb.length) return false;
  for (const k of ka) {
    if (!Object.prototype.hasOwnProperty.call(b, k)) return false;
    if (!deepEqual(a[k], b[k])) return false;
  }
  return true;
}

function runCli(args, stdin) {
  return spawnSync(process.execPath, [cli, 'run', ...args], {
    input: stdin,
    encoding: 'utf8',
    timeout: 15000,
  });
}

let failures = 0;
let count = 0;

for (const file of fs.readdirSync(workflows).sort()) {
  if (!file.endsWith('.json')) continue;
  count++;
  const name = file.replace(/\.json$/, '');
  const expected = JSON.parse(fs.readFileSync(path.join(expectedDir, file), 'utf8'));
  const res = runCli([path.join(workflows, file)], undefined);

  const problems = [];
  if (res.status !== expected.exit) {
    problems.push(`exit ${res.status}, expected ${expected.exit}`);
  }
  let parsed = null;
  try {
    parsed = JSON.parse(res.stdout);
  } catch (e) {
    problems.push('stdout is not valid JSON: ' + JSON.stringify(res.stdout.slice(0, 200)));
  }
  if (parsed) {
    const got = normalize(parsed);
    if (!deepEqual(got, expected.output)) {
      problems.push('output mismatch\n  got:      ' + JSON.stringify(got) +
        '\n  expected: ' + JSON.stringify(expected.output));
    }
  }

  // stdin mode: run the same document through `run -`.
  const stdinRes = runCli(['-'], fs.readFileSync(path.join(workflows, file), 'utf8'));
  if (stdinRes.status !== expected.exit) {
    problems.push(`stdin exit ${stdinRes.status}, expected ${expected.exit}`);
  } else {
    let stdinParsed = null;
    try {
      stdinParsed = JSON.parse(stdinRes.stdout);
    } catch (e) {
      problems.push('stdin stdout is not valid JSON');
    }
    if (stdinParsed && !deepEqual(normalize(stdinParsed), expected.output)) {
      problems.push('stdin output mismatch');
    }
  }

  if (problems.length === 0) {
    process.stdout.write('PASS ' + name + '\n');
  } else {
    failures++;
    process.stdout.write('FAIL ' + name + '\n');
    for (const p of problems) process.stdout.write('     ' + p.replace(/\n/g, '\n     ') + '\n');
  }
}

process.stdout.write(`\n${count - failures}/${count} self-tests passed\n`);
process.exitCode = failures === 0 ? 0 : 1;
