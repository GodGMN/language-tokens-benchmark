#!/usr/bin/env node
'use strict'

/*
 * Self-test runner.
 *
 * For every selftest/<name>.json fixture, run the CLI on stdin and compare the
 * returned JSON document (ignoring object key order, respecting array order)
 * against selftest/expected/<name>.json.
 *
 * Expected exit code is derived from the expected response status:
 *   completed -> 0, failed -> 1.
 *
 * Usage: node selftest/run.js
 */

const fs = require('fs')
const path = require('path')
const { spawnSync } = require('child_process')

const here = __dirname
const cli = path.join(here, '..', 'workflow.js')

function stable (v) {
  if (v === null || typeof v !== 'object') return JSON.stringify(v)
  if (Array.isArray(v)) return '[' + v.map(stable).join(',') + ']'
  const keys = Object.keys(v).sort()
  return '{' + keys.map(k => JSON.stringify(k) + ':' + stable(v[k])).join(',') + '}'
}

let passed = 0
let failed = 0

const fixtures = fs.readdirSync(here).filter(f => f.endsWith('.json')).sort()
for (const name of fixtures) {
  const expectedPath = path.join(here, 'expected', name)
  if (!fs.existsSync(expectedPath)) {
    console.log(`SKIP ${name} (no expected output)`)
    continue
  }
  const expected = JSON.parse(fs.readFileSync(expectedPath, 'utf8'))
  const expectedExit = expected.status === 'completed' ? 0 : 1
  const input = fs.readFileSync(path.join(here, name), 'utf8')

  const res = spawnSync(process.execPath, [cli, 'run', '-'], { input, encoding: 'utf8' })
  const problems = []
  if (res.status !== expectedExit) problems.push(`exit ${res.status}, expected ${expectedExit}`)
  let actual = null
  try {
    actual = JSON.parse(res.stdout)
  } catch (e) {
    problems.push('stdout is not valid JSON: ' + res.stdout.slice(0, 200))
  }
  if (actual !== null && stable(actual) !== stable(expected)) {
    problems.push('output mismatch\n  expected: ' + stable(expected) + '\n  actual:   ' + stable(actual))
  }
  if (res.stderr && res.stderr.trim() !== '') {
    // stderr is allowed; only informative
  }

  if (problems.length === 0) {
    passed++
    console.log(`PASS ${name}`)
  } else {
    failed++
    console.log(`FAIL ${name}`)
    for (const p of problems) console.log('  ' + p)
  }
}

console.log(`\n${passed} passed, ${failed} failed`)
process.exitCode = failed === 0 ? 0 : 1
