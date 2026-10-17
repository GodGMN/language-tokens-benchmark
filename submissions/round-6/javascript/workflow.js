#!/usr/bin/env node
'use strict'

/*
 * workflow.js - declarative workflow engine (JavaScript, Node.js, no deps).
 *
 * Usage:
 *   node workflow.js run <workflow.json>
 *   node workflow.js run -        (read JSON document from stdin)
 *
 * stdout: exactly one JSON document.  stderr: diagnostics only.
 * Exit: 0 completed, 1 runtime failure, 2 validation/input error, 3 internal.
 */

const fs = require('fs')
const { Buffer } = require('buffer')

// ---------------------------------------------------------------------------
// Value model
//
//   null                     -> null
//   boolean                  -> true / false
//   number                   -> I64 (integer literal, exact BigInt) or F64
//   string                   -> JS string
//   array                    -> JS Array
//   object                   -> Map<string, value>
//
// I64/F64 keep the distinction between JSON integer literals and literals with
// a fraction/exponent, which the spec requires for numeric parameters.
// ---------------------------------------------------------------------------

const I64_MIN = -(2n ** 63n)
const I64_MAX = (2n ** 63n) - 1n

class I64 {
  constructor (b) { this.b = b }
}
class F64 {
  constructor (n, raw) { this.n = n; this.raw = raw }
}

class OpError extends Error {
  constructor (code, message) { super(message || code); this.name = 'OpError'; this.code = code }
}
class StepError extends Error {
  constructor (code, message) { super(message || code); this.name = 'StepError'; this.code = code }
}
class WfError extends Error {
  constructor (code, step, message) { super(message || code); this.name = 'WfError'; this.code = code; this.step = step === undefined ? null : step }
}
class JsonParseError extends Error {}

// ---------------------------------------------------------------------------
// JSON parser (strict, preserves integer/float distinction and duplicates)
// ---------------------------------------------------------------------------

function parseJson (text) {
  if (text.charCodeAt(0) === 0xfeff) text = text.slice(1) // tolerate a UTF-8 BOM
  const n = text.length
  let i = 0

  function fail (msg) { throw new JsonParseError(msg + ' at offset ' + i) }

  function skipWs () {
    while (i < n) {
      const c = text.charCodeAt(i)
      if (c === 0x20 || c === 0x09 || c === 0x0a || c === 0x0d) i++
      else break
    }
  }

  function isDigit (c) { return c >= 0x30 && c <= 0x39 }

  function parseValue (depth) {
    if (depth > 512) fail('nesting too deep')
    skipWs()
    if (i >= n) fail('unexpected end of input')
    const c = text[i]
    if (c === '{') return parseObject(depth)
    if (c === '[') return parseArray(depth)
    if (c === '"') return parseString()
    if (c === 't') { expect('true'); return true }
    if (c === 'f') { expect('false'); return false }
    if (c === 'n') { expect('null'); return null }
    if (c === '-' || (c >= '0' && c <= '9')) return parseNumber()
    fail('unexpected character ' + JSON.stringify(c))
  }

  function expect (word) {
    if (text.slice(i, i + word.length) !== word) fail('expected ' + word)
    i += word.length
  }

  function parseObject (depth) {
    i++ // {
    const map = new Map()
    let dups = null
    skipWs()
    if (i < n && text[i] === '}') { i++; return map }
    for (;;) {
      skipWs()
      if (i >= n) fail('unterminated object')
      if (text[i] !== '"') fail('object keys must be strings')
      const key = parseString()
      skipWs()
      if (i >= n || text[i] !== ':') fail("expected ':'")
      i++
      const value = parseValue(depth + 1)
      if (map.has(key)) {
        if (dups === null) dups = []
        dups.push([key, map.get(key)])
      }
      map.set(key, value)
      skipWs()
      if (i >= n) fail('unterminated object')
      if (text[i] === ',') { i++; continue }
      if (text[i] === '}') { i++; break }
      fail("expected ',' or '}'")
    }
    if (dups !== null) {
      Object.defineProperty(map, '__dups', { value: dups, enumerable: false, writable: false, configurable: false })
    }
    return map
  }

  function parseArray (depth) {
    i++ // [
    const arr = []
    skipWs()
    if (i < n && text[i] === ']') { i++; return arr }
    for (;;) {
      arr.push(parseValue(depth + 1))
      skipWs()
      if (i >= n) fail('unterminated array')
      if (text[i] === ',') { i++; continue }
      if (text[i] === ']') { i++; break }
      fail("expected ',' or ']'")
    }
    return arr
  }

  function parseString () {
    i++ // opening quote
    let out = ''
    let start = i
    for (;;) {
      if (i >= n) fail('unterminated string')
      const c = text.charCodeAt(i)
      if (c === 0x22) { // '"'
        out += text.slice(start, i)
        i++
        return out
      }
      if (c === 0x5c) { // '\\'
        out += text.slice(start, i)
        i++
        if (i >= n) fail('unterminated escape')
        const e = text[i]
        i++
        switch (e) {
          case '"': out += '"'; break
          case '\\': out += '\\'; break
          case '/': out += '/'; break
          case 'b': out += '\b'; break
          case 'f': out += '\f'; break
          case 'n': out += '\n'; break
          case 'r': out += '\r'; break
          case 't': out += '\t'; break
          case 'u': {
            if (i + 4 > n) fail('bad unicode escape')
            const hex = text.slice(i, i + 4)
            if (!/^[0-9A-Fa-f]{4}$/.test(hex)) fail('bad unicode escape')
            out += String.fromCharCode(parseInt(hex, 16))
            i += 4
            break
          }
          default: fail('bad escape')
        }
        start = i
        continue
      }
      if (c < 0x20) fail('control character in string')
      i++
    }
  }

  function parseNumber () {
    const start = i
    if (text[i] === '-') i++
    if (i >= n) fail('bad number')
    if (text[i] === '0') {
      i++
    } else if (text[i] >= '1' && text[i] <= '9') {
      while (i < n && isDigit(text.charCodeAt(i))) i++
    } else {
      fail('bad number')
    }
    let isFloat = false
    if (i < n && text[i] === '.') {
      isFloat = true
      i++
      if (i >= n || !isDigit(text.charCodeAt(i))) fail('bad number')
      while (i < n && isDigit(text.charCodeAt(i))) i++
    }
    if (i < n && (text[i] === 'e' || text[i] === 'E')) {
      isFloat = true
      i++
      if (i < n && (text[i] === '+' || text[i] === '-')) i++
      if (i >= n || !isDigit(text.charCodeAt(i))) fail('bad number')
      while (i < n && isDigit(text.charCodeAt(i))) i++
    }
    const raw = text.slice(start, i)
    if (isFloat) return new F64(Number(raw), raw)
    return new I64(BigInt(raw))
  }

  const value = parseValue(0)
  skipWs()
  if (i !== n) fail('trailing content after JSON value')
  return value
}

// ---------------------------------------------------------------------------
// Value helpers
// ---------------------------------------------------------------------------

function kindOf (v) {
  if (v === null) return 'null'
  if (v === true || v === false) return 'boolean'
  if (v instanceof I64 || v instanceof F64) return 'number'
  if (typeof v === 'string') return 'string'
  if (Array.isArray(v)) return 'array'
  if (v instanceof Map) return 'object'
  throw new Error('unknown internal value')
}

const TYPE_RANK = { number: 0, string: 1, boolean: 2, null: 3, array: 4, object: 5 }

function numPrim (v) { return v instanceof I64 ? v.b : v.n } // BigInt | Number

function numEq (a, b) {
  const x = numPrim(a)
  const y = numPrim(b)
  if (typeof x === 'bigint' && typeof y === 'bigint') return x === y
  if (typeof x === 'number' && typeof y === 'number') return x === y
  // eslint-disable-next-line eqeqeq
  return x == y // mixed BigInt/Number: exact mathematical comparison
}

function numCmp (a, b) {
  if (numEq(a, b)) return 0
  const x = numPrim(a)
  const y = numPrim(b)
  return x < y ? -1 : 1
}

function cmpBytes (a, b) {
  if (a === b) return 0
  return Buffer.compare(Buffer.from(a, 'utf8'), Buffer.from(b, 'utf8'))
}

function sortedKeys (obj) {
  return Array.from(obj.keys()).sort(cmpBytes)
}

function deepEqual (a, b) {
  const ka = kindOf(a)
  if (ka !== kindOf(b)) return false
  switch (ka) {
    case 'null': return true
    case 'boolean': return a === b
    case 'number': return numEq(a, b)
    case 'string': return a === b
    case 'array': {
      if (a.length !== b.length) return false
      for (let i = 0; i < a.length; i++) if (!deepEqual(a[i], b[i])) return false
      return true
    }
    case 'object': {
      if (a.size !== b.size) return false
      for (const [k, v] of a) {
        if (!b.has(k)) return false
        if (!deepEqual(v, b.get(k))) return false
      }
      return true
    }
  }
  return false
}

function cmpValues (a, b) {
  const ka = kindOf(a)
  const kb = kindOf(b)
  if (ka !== kb) return TYPE_RANK[ka] - TYPE_RANK[kb]
  switch (ka) {
    case 'null': return 0
    case 'boolean': return a === b ? 0 : (a ? 1 : -1)
    case 'number': return numCmp(a, b)
    case 'string': return cmpBytes(a, b)
    case 'array': {
      const len = Math.min(a.length, b.length)
      for (let i = 0; i < len; i++) {
        const c = cmpValues(a[i], b[i])
        if (c !== 0) return c
      }
      return a.length - b.length
    }
    case 'object': {
      const ka2 = sortedKeys(a)
      const kb2 = sortedKeys(b)
      const len = Math.min(ka2.length, kb2.length)
      for (let i = 0; i < len; i++) {
        const c = cmpBytes(ka2[i], kb2[i])
        if (c !== 0) return c
      }
      if (ka2.length !== kb2.length) return ka2.length - kb2.length
      for (let i = 0; i < ka2.length; i++) {
        const c = cmpValues(a.get(ka2[i]), b.get(kb2[i]))
        if (c !== 0) return c
      }
      return 0
    }
  }
  return 0
}

function isTruthy (v) {
  switch (kindOf(v)) {
    case 'null': return false
    case 'boolean': return v
    case 'number': return v instanceof I64 ? v.b !== 0n : v.n !== 0
    case 'string': return v.length > 0
    case 'array': return v.length > 0
    case 'object': return v.size > 0
  }
  return false
}

function codePointLength (s) {
  let n = 0
  for (const _ of s) n++ // eslint-disable-line no-unused-vars
  return n
}

// Canonical key used by `unique` (respects structural/numeric equality).
function canon (v) {
  switch (kindOf(v)) {
    case 'null': return 'z'
    case 'boolean': return v ? 'b1' : 'b0'
    case 'number': {
      if (v instanceof I64) return 'n' + v.b.toString()
      const x = v.n
      if (Number.isInteger(x)) return 'n' + BigInt(x).toString()
      return 'f' + String(x)
    }
    case 'string': return 's' + JSON.stringify(v)
    case 'array': return 'a[' + v.map(canon).join(',') + ']'
    case 'object': {
      const ks = sortedKeys(v)
      return 'o{' + ks.map(k => JSON.stringify(k) + ':' + canon(v.get(k))).join(',') + '}'
    }
  }
  return ''
}

// Compact JSON serialization (deterministic, handles BigInt / raw floats).
function ser (v) {
  if (v === null) return 'null'
  if (v === true) return 'true'
  if (v === false) return 'false'
  if (typeof v === 'string') return JSON.stringify(v)
  if (v instanceof I64) return v.b.toString()
  if (v instanceof F64) return v.raw
  if (Array.isArray(v)) {
    const parts = new Array(v.length)
    for (let i = 0; i < v.length; i++) parts[i] = ser(v[i])
    return '[' + parts.join(',') + ']'
  }
  if (v instanceof Map) {
    const parts = []
    for (const [k, val] of v) parts.push(JSON.stringify(k) + ':' + ser(val))
    return '{' + parts.join(',') + '}'
  }
  throw new Error('cannot serialize internal value')
}

// ---------------------------------------------------------------------------
// Structural validation
// ---------------------------------------------------------------------------

const OP_NAMES = new Set([
  'add', 'subtract', 'multiply', 'divide', 'mod', 'sum', 'min', 'max', 'range',
  'concat', 'split', 'join', 'uppercase', 'lowercase', 'trim', 'replace', 'length',
  'map', 'filter', 'reduce', 'sort', 'reverse', 'unique', 'slice', 'flatten',
  'get', 'set', 'keys', 'values', 'merge', 'pick', 'omit',
  'constant', 'equals', 'compare', 'and', 'or', 'not', 'type', 'fail_until'
])

const STEP_FIELDS = new Set(['id', 'op', 'args', 'depends_on', 'when', 'retry'])
const ID_PATTERN = /^[A-Za-z_][A-Za-z0-9_]*$/
const CMP_CONDS = new Set(['eq', 'ne', 'gt', 'gte', 'lt', 'lte'])

function invalid (step, msg) { return new WfError('INVALID_WORKFLOW', step === undefined ? null : step, msg) }

function validateConditionShape (cond, stepId) {
  if (!(cond instanceof Map)) throw invalid(stepId, 'condition must be an object')
  const op = cond.get('op')
  if (typeof op !== 'string') throw invalid(stepId, "condition requires a string 'op'")
  if (op === 'all' || op === 'any') {
    for (const k of cond.keys()) if (k !== 'op' && k !== 'conditions') throw invalid(stepId, `unknown condition field '${k}'`)
    const arr = cond.get('conditions')
    if (!Array.isArray(arr)) throw invalid(stepId, "'conditions' must be an array")
    for (const sub of arr) validateConditionShape(sub, stepId)
    return
  }
  if (op === 'not') {
    for (const k of cond.keys()) if (k !== 'op' && k !== 'condition') throw invalid(stepId, `unknown condition field '${k}'`)
    if (!cond.has('condition')) throw invalid(stepId, "'not' requires 'condition'")
    validateConditionShape(cond.get('condition'), stepId)
    return
  }
  if (CMP_CONDS.has(op)) {
    for (const k of cond.keys()) if (k !== 'op' && k !== 'left' && k !== 'right') throw invalid(stepId, `unknown condition field '${k}'`)
    if (!cond.has('left')) throw invalid(stepId, "condition requires 'left'")
    if (!cond.has('right')) throw invalid(stepId, "condition requires 'right'")
    return
  }
  throw invalid(stepId, `unknown condition op '${op}'`)
}

function validateDocument (doc) {
  if (!(doc instanceof Map)) throw invalid(null, 'workflow document must be a JSON object')
  for (const k of doc.keys()) {
    if (k !== 'version' && k !== 'steps') throw invalid(null, `unknown top-level field '${k}'`)
  }
  const version = doc.get('version')
  if (!(version instanceof I64) || version.b !== 1n) {
    throw invalid(null, "version must be the integer literal 1")
  }
  if (!doc.has('steps')) throw invalid(null, "missing 'steps'")
  const rawSteps = doc.get('steps')
  if (!Array.isArray(rawSteps)) throw invalid(null, "'steps' must be an array")

  const steps = []
  const ids = new Set()

  for (const raw of rawSteps) {
    if (!(raw instanceof Map)) throw invalid(null, 'each step must be an object')
    const idv = raw.get('id')
    const stepId = typeof idv === 'string' ? idv : null

    for (const k of raw.keys()) {
      if (!STEP_FIELDS.has(k)) throw invalid(stepId, `unknown step field '${k}'`)
    }
    if (typeof idv !== 'string') throw invalid(stepId, "step requires a string 'id'")
    if (!ID_PATTERN.test(idv)) throw invalid(stepId, `invalid step id '${idv}'`)
    if (ids.has(idv)) throw invalid(idv, `duplicate step id '${idv}'`)
    ids.add(idv)

    const opv = raw.get('op')
    if (typeof opv !== 'string') throw invalid(idv, "step requires a string 'op'")
    if (!OP_NAMES.has(opv)) throw invalid(idv, `unknown op '${opv}'`)

    let args = new Map()
    if (raw.has('args')) {
      const a = raw.get('args')
      if (!(a instanceof Map)) throw invalid(idv, "'args' must be an object")
      args = a
    }

    let dependsOn = []
    if (raw.has('depends_on')) {
      const d = raw.get('depends_on')
      if (!Array.isArray(d)) throw invalid(idv, "'depends_on' must be an array")
      const seen = new Set()
      for (const x of d) {
        if (typeof x !== 'string') throw invalid(idv, "'depends_on' entries must be strings")
        if (!seen.has(x)) { seen.add(x); dependsOn.push(x) }
      }
    }

    let retryMax = 1
    if (raw.has('retry')) {
      const r = raw.get('retry')
      if (!(r instanceof Map)) throw invalid(idv, "'retry' must be an object")
      for (const k of r.keys()) {
        if (k !== 'max_attempts') throw invalid(idv, `unknown retry field '${k}'`)
      }
      if (r.has('max_attempts')) {
        const ma = r.get('max_attempts')
        if (!(ma instanceof I64) || ma.b < 1n || ma.b > 5n) {
          throw invalid(idv, "'retry.max_attempts' must be an integer 1..5")
        }
        retryMax = Number(ma.b)
      }
    }

    let when = null
    if (raw.has('when')) {
      when = raw.get('when')
      validateConditionShape(when, idv)
    }

    steps.push({ id: idv, op: opv, args, dependsOn, retryMax, when })
  }

  // Dependencies must exist.
  for (const st of steps) {
    for (const d of st.dependsOn) {
      if (!ids.has(d)) throw new WfError('UNKNOWN_DEPENDENCY', st.id, `step '${st.id}' depends on unknown step '${d}'`)
    }
  }

  // Cycles (Kahn).
  const remaining = new Map()
  const dependents = new Map()
  for (const st of steps) { remaining.set(st.id, new Set(st.dependsOn)); dependents.set(st.id, new Set()) }
  for (const st of steps) for (const d of st.dependsOn) dependents.get(d).add(st.id)
  const queue = []
  for (const st of steps) if (remaining.get(st.id).size === 0) queue.push(st.id)
  let done = 0
  while (queue.length) {
    const id = queue.pop()
    done++
    for (const dep of dependents.get(id)) {
      const r = remaining.get(dep)
      r.delete(id)
      if (r.size === 0) queue.push(dep)
    }
  }
  if (done !== steps.length) throw new WfError('CYCLIC_DEPENDENCY', null, 'dependency cycle detected')

  return steps
}

// ---------------------------------------------------------------------------
// Reference resolution
// ---------------------------------------------------------------------------

function computeClosures (steps) {
  const byId = new Map(steps.map(s => [s.id, s]))
  const closures = new Map()
  for (const st of steps) {
    const seen = new Set()
    const stack = st.dependsOn.slice()
    while (stack.length) {
      const d = stack.pop()
      if (seen.has(d)) continue
      seen.add(d)
      const ds = byId.get(d)
      if (ds) for (const x of ds.dependsOn) stack.push(x)
    }
    closures.set(st.id, seen)
  }
  return closures
}

function resolveRef (ref, step, ctx, closure) {
  const segs = ref.split('.')
  const targetId = segs[0]
  if (!ctx.byId.has(targetId)) {
    throw new OpError('INVALID_REFERENCE', `reference to unknown step '${targetId}'`)
  }
  if (!closure.has(targetId)) {
    throw new OpError('INVALID_REFERENCE', `step '${targetId}' is not a dependency of '${step.id}'`)
  }
  if (ctx.skipped.has(targetId)) {
    throw new OpError('SKIPPED_REFERENCE', `step '${targetId}' was skipped`)
  }
  let cur = ctx.results.get(targetId)
  for (let i = 1; i < segs.length; i++) {
    const seg = segs[i]
    if (cur instanceof Map) {
      if (!cur.has(seg)) throw new OpError('INVALID_REFERENCE', `missing key '${seg}' in reference '${ref}'`)
      cur = cur.get(seg)
    } else if (Array.isArray(cur)) {
      if (!/^[0-9]+$/.test(seg)) throw new OpError('INVALID_REFERENCE', `invalid array index '${seg}' in reference '${ref}'`)
      const idx = Number(seg)
      if (!Number.isSafeInteger(idx) || idx >= cur.length) {
        throw new OpError('INVALID_REFERENCE', `array index '${seg}' out of range in reference '${ref}'`)
      }
      cur = cur[idx]
    } else {
      throw new OpError('INVALID_REFERENCE', `cannot descend into non-container via '${seg}' in reference '${ref}'`)
    }
  }
  return cur
}

function makeResolver (step, ctx) {
  const closure = ctx.closures.get(step.id)
  function resolve (v) {
    if (v instanceof Map) {
      if (v.size === 1 && v.has('$ref') && typeof v.get('$ref') === 'string') {
        return resolveRef(v.get('$ref'), step, ctx, closure)
      }
      const out = new Map()
      for (const [k, val] of v) out.set(k, resolve(val))
      if (Array.isArray(v.__dups)) {
        Object.defineProperty(out, '__dups', { value: v.__dups, enumerable: false, writable: false, configurable: false })
      }
      return out
    }
    if (Array.isArray(v)) {
      const out = new Array(v.length)
      for (let i = 0; i < v.length; i++) out[i] = resolve(v[i])
      return out
    }
    return v
  }
  return resolve
}

// ---------------------------------------------------------------------------
// Conditions
// ---------------------------------------------------------------------------

function strictBool (v) {
  if (typeof v !== 'boolean') throw new OpError('INVALID_CONDITION', 'condition did not evaluate to a boolean')
  return v
}

function evalCondition (cond, resolve) {
  const op = cond.get('op')
  if (op === 'all') {
    for (const sub of cond.get('conditions')) {
      if (strictBool(evalCondition(sub, resolve)) === false) return false
    }
    return true
  }
  if (op === 'any') {
    for (const sub of cond.get('conditions')) {
      if (strictBool(evalCondition(sub, resolve)) === true) return true
    }
    return false
  }
  if (op === 'not') {
    return !strictBool(evalCondition(cond.get('condition'), resolve))
  }
  const left = resolve(cond.get('left'))
  const right = resolve(cond.get('right'))
  switch (op) {
    case 'eq': return strictBool(deepEqual(left, right))
    case 'ne': return strictBool(!deepEqual(left, right))
    case 'gt': return strictBool(cmpValues(left, right) > 0)
    case 'gte': return strictBool(cmpValues(left, right) >= 0)
    case 'lt': return strictBool(cmpValues(left, right) < 0)
    case 'lte': return strictBool(cmpValues(left, right) <= 0)
  }
  throw new OpError('INVALID_CONDITION', `unknown condition op '${String(op)}'`)
}

// ---------------------------------------------------------------------------
// Op argument helpers
// ---------------------------------------------------------------------------

function ensureOnly (args, allowed) {
  for (const k of args.keys()) {
    if (!allowed.includes(k)) throw new OpError('INVALID_ARG', `unknown argument '${k}'`)
  }
}

function argAny (args, name) {
  if (!args.has(name)) throw new OpError('INVALID_ARG', `missing required argument '${name}'`)
  return args.get(name)
}

function toI64 (v, what) {
  if (!(v instanceof I64)) throw new OpError('INVALID_ARG', `${what} must be an integer`)
  if (v.b < I64_MIN || v.b > I64_MAX) throw new OpError('INVALID_ARG', `${what} is outside the i64 range`)
  return v.b
}

function argI64 (args, name) { return toI64(argAny(args, name), `argument '${name}'`) }

function argStr (args, name) {
  const v = argAny(args, name)
  if (typeof v !== 'string') throw new OpError('INVALID_ARG', `argument '${name}' must be a string`)
  return v
}

function argArr (args, name) {
  const v = argAny(args, name)
  if (!Array.isArray(v)) throw new OpError('INVALID_ARG', `argument '${name}' must be an array`)
  return v
}

function argObj (args, name) {
  const v = argAny(args, name)
  if (!(v instanceof Map)) throw new OpError('INVALID_ARG', `argument '${name}' must be an object`)
  return v
}

function argStrArray (args, name) {
  const arr = argArr(args, name)
  for (const x of arr) if (typeof x !== 'string') throw new OpError('INVALID_ARG', `argument '${name}' must be an array of strings`)
  return arr
}

function chkBig (x) {
  if (x < I64_MIN || x > I64_MAX) throw new OpError('ARITHMETIC_OVERFLOW', 'i64 overflow')
  return x
}

function binI64 (op, a, b) {
  switch (op) {
    case 'add': return chkBig(a + b)
    case 'subtract': return chkBig(a - b)
    case 'multiply': return chkBig(a * b)
    case 'divide': {
      if (b === 0n) throw new OpError('DIVISION_BY_ZERO', 'division by zero')
      if (a === I64_MIN && b === -1n) throw new OpError('ARITHMETIC_OVERFLOW', 'i64 overflow')
      return a / b
    }
    case 'mod': {
      if (b === 0n) throw new OpError('DIVISION_BY_ZERO', 'modulo by zero')
      if (a === I64_MIN && b === -1n) throw new OpError('ARITHMETIC_OVERFLOW', 'i64 overflow')
      return a % b
    }
  }
  throw new Error('bad binary op ' + op)
}

function predicateTypeInfo (p) {
  let type = p.get('type')
  if (typeof type !== 'string') throw new OpError('INVALID_ARG', "predicate requires a string 'type'")
  const dups = Array.isArray(p.__dups) ? p.__dups : []
  const typeTokens = [type, ...dups.filter(d => d[0] === 'type').map(d => d[1])]
  if (type !== 'type_is' && typeTokens.includes('type_is')) {
    const other = typeTokens.find(t => t !== 'type_is')
    if (typeof other === 'string') return { type: 'type_is', typeIsName: other }
  }
  if (type === 'type_is') {
    for (const k of ['value', 'type_name', 'name', 'target_type', 'type_arg']) {
      const v = p.get(k)
      if (typeof v === 'string') return { type, typeIsName: v }
    }
    const other = typeTokens.find(t => t !== 'type_is')
    if (typeof other === 'string') return { type, typeIsName: other }
    throw new OpError('INVALID_ARG', "predicate 'type_is' requires a type name")
  }
  return { type, typeIsName: null }
}

const TYPE_NAMES = new Set(['null', 'boolean', 'number', 'string', 'array', 'object'])

function applyPredicate (el, p, attempt) {
  const info = predicateTypeInfo(p)
  const type = info.type
  switch (type) {
    case 'even':
    case 'odd': {
      ensureOnly(p, ['type'])
      const x = toI64(el, 'element')
      const isEven = (x % 2n) === 0n
      return type === 'even' ? isEven : !isEven
    }
    case 'gt':
    case 'gte':
    case 'lt':
    case 'lte': {
      ensureOnly(p, ['type', 'value'])
      const a = toI64(el, 'element')
      const b = toI64(argAny(p, 'value'), "predicate 'value'")
      if (type === 'gt') return a > b
      if (type === 'gte') return a >= b
      if (type === 'lt') return a < b
      return a <= b
    }
    case 'eq':
    case 'ne': {
      ensureOnly(p, ['type', 'value'])
      const v = argAny(p, 'value')
      const eq = deepEqual(el, v)
      return type === 'eq' ? eq : !eq
    }
    case 'truthy': ensureOnly(p, ['type']); return isTruthy(el)
    case 'falsy': ensureOnly(p, ['type']); return !isTruthy(el)
    case 'string_contains': {
      ensureOnly(p, ['type', 'value'])
      const needle = argAny(p, 'value')
      if (typeof needle !== 'string') throw new OpError('INVALID_ARG', "predicate 'value' must be a string")
      if (typeof el !== 'string') throw new OpError('INVALID_ARG', 'element must be a string')
      return el.includes(needle)
    }
    case 'type_is': {
      ensureOnly(p, ['type', 'value', 'type_name', 'name', 'target_type', 'type_arg'])
      const name = info.typeIsName
      if (!TYPE_NAMES.has(name)) throw new OpError('INVALID_ARG', `unknown type name '${name}'`)
      return kindOf(el) === name
    }
    default:
      throw new OpError('INVALID_ARG', `unknown predicate type '${type}'`)
  }
}

function substSelf (v, el) {
  if (v instanceof Map) {
    if (v.size === 1 && v.has('$self')) return el
    const out = new Map()
    for (const [k, x] of v) out.set(k, substSelf(x, el))
    return out
  }
  if (Array.isArray(v)) return v.map(x => substSelf(x, el))
  return v
}

function applyTransform (el, tr, attempt) {
  const type = tr.get('type')
  if (typeof type !== 'string') throw new OpError('INVALID_ARG', "transform requires a string 'type'")
  switch (type) {
    case 'op': {
      ensureOnly(tr, ['type', 'op', 'args'])
      const name = tr.get('op')
      if (typeof name !== 'string' || !OP_NAMES.has(name)) {
        throw new OpError('INVALID_ARG', 'transform has an invalid op name')
      }
      let targs = new Map()
      if (tr.has('args')) {
        const a = tr.get('args')
        if (!(a instanceof Map)) throw new OpError('INVALID_ARG', "transform 'args' must be an object")
        targs = a
      }
      return invokeOp(name, substSelf(targs, el), attempt)
    }
    case 'constant': {
      ensureOnly(tr, ['type', 'value'])
      return argAny(tr, 'value')
    }
    case 'add':
    case 'subtract':
    case 'multiply':
    case 'divide':
    case 'mod': {
      ensureOnly(tr, ['type', 'value'])
      const a = toI64(el, 'element')
      const b = toI64(argAny(tr, 'value'), "transform 'value'")
      return new I64(binI64(type, a, b))
    }
    case 'negate': {
      ensureOnly(tr, ['type'])
      const a = toI64(el, 'element')
      if (a === I64_MIN) throw new OpError('ARITHMETIC_OVERFLOW', 'i64 overflow')
      return new I64(-a)
    }
    case 'abs': {
      ensureOnly(tr, ['type'])
      const a = toI64(el, 'element')
      if (a === I64_MIN) throw new OpError('ARITHMETIC_OVERFLOW', 'i64 overflow')
      return new I64(a < 0n ? -a : a)
    }
    case 'stringify': {
      ensureOnly(tr, ['type'])
      return ser(el)
    }
    case 'type': {
      ensureOnly(tr, ['type'])
      return kindOf(el)
    }
    default:
      throw new OpError('INVALID_ARG', `unknown transform type '${type}'`)
  }
}

// ---------------------------------------------------------------------------
// Operations
// ---------------------------------------------------------------------------

function asciiUpper (s) {
  let out = ''
  for (let i = 0; i < s.length; i++) {
    const c = s.charCodeAt(i)
    out += (c >= 0x61 && c <= 0x7a) ? String.fromCharCode(c - 32) : s[i]
  }
  return out
}

function asciiLower (s) {
  let out = ''
  for (let i = 0; i < s.length; i++) {
    const c = s.charCodeAt(i)
    out += (c >= 0x41 && c <= 0x5a) ? String.fromCharCode(c + 32) : s[i]
  }
  return out
}

function invokeOp (name, args, attempt) {
  if (!(args instanceof Map)) throw new OpError('INVALID_ARG', 'operation args must be a JSON object')
  switch (name) {
    // ---- math ----
    case 'add': ensureOnly(args, ['a', 'b']); return new I64(binI64('add', argI64(args, 'a'), argI64(args, 'b')))
    case 'subtract': ensureOnly(args, ['a', 'b']); return new I64(binI64('subtract', argI64(args, 'a'), argI64(args, 'b')))
    case 'multiply': ensureOnly(args, ['a', 'b']); return new I64(binI64('multiply', argI64(args, 'a'), argI64(args, 'b')))
    case 'divide': ensureOnly(args, ['a', 'b']); return new I64(binI64('divide', argI64(args, 'a'), argI64(args, 'b')))
    case 'mod': ensureOnly(args, ['a', 'b']); return new I64(binI64('mod', argI64(args, 'a'), argI64(args, 'b')))

    case 'sum': {
      ensureOnly(args, ['input'])
      const arr = argArr(args, 'input')
      let s = 0n
      for (const el of arr) s += toI64(el, 'input element')
      return new I64(chkBig(s))
    }
    case 'min': {
      ensureOnly(args, ['input'])
      const arr = argArr(args, 'input')
      if (arr.length === 0) throw new OpError('EMPTY_ARRAY', 'min of empty array')
      let m = toI64(arr[0], 'input element')
      for (let i = 1; i < arr.length; i++) {
        const x = toI64(arr[i], 'input element')
        if (x < m) m = x
      }
      return new I64(m)
    }
    case 'max': {
      ensureOnly(args, ['input'])
      const arr = argArr(args, 'input')
      if (arr.length === 0) throw new OpError('EMPTY_ARRAY', 'max of empty array')
      let m = toI64(arr[0], 'input element')
      for (let i = 1; i < arr.length; i++) {
        const x = toI64(arr[i], 'input element')
        if (x > m) m = x
      }
      return new I64(m)
    }
    case 'range': {
      ensureOnly(args, ['start', 'end'])
      const start = argI64(args, 'start')
      const end = argI64(args, 'end')
      if (start > end) return []
      const len = end - start + 1n
      if (len > 1000000n) throw new OpError('ARITHMETIC_OVERFLOW', 'range is too large to materialize')
      const out = new Array(Number(len))
      let x = start
      for (let i = 0; i < out.length; i++, x++) out[i] = new I64(x)
      return out
    }

    // ---- strings ----
    case 'concat': ensureOnly(args, ['a', 'b']); return argStr(args, 'a') + argStr(args, 'b')
    case 'split': {
      ensureOnly(args, ['input', 'sep'])
      const input = argStr(args, 'input')
      const sep = argStr(args, 'sep')
      if (sep === '') return Array.from(input)
      return input.split(sep)
    }
    case 'join': {
      ensureOnly(args, ['input', 'sep'])
      const sep = argStr(args, 'sep')
      const arr = argStrArray(args, 'input')
      return arr.join(sep)
    }
    case 'uppercase': ensureOnly(args, ['input']); return asciiUpper(argStr(args, 'input'))
    case 'lowercase': ensureOnly(args, ['input']); return asciiLower(argStr(args, 'input'))
    case 'trim': {
      ensureOnly(args, ['input'])
      return argStr(args, 'input').replace(/^[ \t\n\r\f\v]+/, '').replace(/[ \t\n\r\f\v]+$/, '')
    }
    case 'replace': {
      ensureOnly(args, ['input', 'search', 'replace'])
      const input = argStr(args, 'input')
      const search = argStr(args, 'search')
      const repl = argStr(args, 'replace')
      if (search === '') throw new OpError('INVALID_ARG', 'search string must not be empty')
      return input.split(search).join(repl)
    }
    case 'length': ensureOnly(args, ['input']); return new I64(BigInt(codePointLength(argStr(args, 'input'))))

    // ---- arrays ----
    case 'map': {
      ensureOnly(args, ['input', 'transform'])
      const input = argArr(args, 'input')
      const tr = argObj(args, 'transform')
      const out = new Array(input.length)
      for (let i = 0; i < input.length; i++) out[i] = applyTransform(input[i], tr, attempt)
      return out
    }
    case 'filter': {
      ensureOnly(args, ['input', 'predicate'])
      const input = argArr(args, 'input')
      const pred = argObj(args, 'predicate')
      const out = []
      for (const el of input) if (applyPredicate(el, pred, attempt)) out.push(el)
      return out
    }
    case 'reduce': {
      ensureOnly(args, ['input', 'op', 'initial'])
      const input = argArr(args, 'input')
      const opn = argStr(args, 'op')
      if (opn !== 'add' && opn !== 'subtract' && opn !== 'multiply') {
        throw new OpError('INVALID_ARG', `reduce op must be add, subtract or multiply`)
      }
      let acc
      if (args.has('initial')) acc = argI64(args, 'initial')
      else acc = opn === 'multiply' ? 1n : 0n
      for (const el of input) {
        const x = toI64(el, 'input element')
        acc = binI64(opn, acc, x)
      }
      return new I64(acc)
    }
    case 'sort': {
      ensureOnly(args, ['input'])
      const input = argArr(args, 'input')
      const idx = new Array(input.length)
      for (let i = 0; i < input.length; i++) idx[i] = i
      idx.sort((i, j) => {
        const c = cmpValues(input[i], input[j])
        return c !== 0 ? c : i - j
      })
      return idx.map(i => input[i])
    }
    case 'reverse': {
      ensureOnly(args, ['input'])
      const input = argArr(args, 'input')
      const out = new Array(input.length)
      for (let i = 0; i < input.length; i++) out[i] = input[input.length - 1 - i]
      return out
    }
    case 'unique': {
      ensureOnly(args, ['input'])
      const input = argArr(args, 'input')
      const seen = new Set()
      const out = []
      for (const el of input) {
        const k = canon(el)
        if (!seen.has(k)) { seen.add(k); out.push(el) }
      }
      return out
    }
    case 'slice': {
      ensureOnly(args, ['input', 'start', 'end'])
      const input = argArr(args, 'input')
      const len = BigInt(input.length)
      let s = argI64(args, 'start')
      let e = args.has('end') ? argI64(args, 'end') : len
      s = s < 0n ? len + s : s
      e = e < 0n ? len + e : e
      if (s < 0n) s = 0n
      if (e < 0n) e = 0n
      if (s > len) s = len
      if (e > len) e = len
      if (s >= e) return []
      return input.slice(Number(s), Number(e))
    }
    case 'flatten': {
      ensureOnly(args, ['input'])
      const input = argArr(args, 'input')
      const out = []
      for (const el of input) {
        if (Array.isArray(el)) { for (const x of el) out.push(x) } else out.push(el)
      }
      return out
    }

    // ---- objects ----
    case 'get': {
      ensureOnly(args, ['input', 'key'])
      const obj = argObj(args, 'input')
      const key = argStr(args, 'key')
      return obj.has(key) ? obj.get(key) : null
    }
    case 'set': {
      ensureOnly(args, ['input', 'key', 'value'])
      const obj = argObj(args, 'input')
      const key = argStr(args, 'key')
      const val = argAny(args, 'value')
      const out = new Map(obj)
      out.set(key, val)
      return out
    }
    case 'keys': {
      ensureOnly(args, ['input'])
      return sortedKeys(argObj(args, 'input'))
    }
    case 'values': {
      ensureOnly(args, ['input'])
      const obj = argObj(args, 'input')
      const out = []
      for (const k of sortedKeys(obj)) out.push(obj.get(k))
      return out
    }
    case 'merge': {
      ensureOnly(args, ['a', 'b'])
      const a = argObj(args, 'a')
      const b = argObj(args, 'b')
      const out = new Map(a)
      for (const [k, v] of b) out.set(k, v)
      return out
    }
    case 'pick': {
      ensureOnly(args, ['input', 'keys'])
      const obj = argObj(args, 'input')
      const keys = argStrArray(args, 'keys')
      const uniq = new Set(keys)
      const out = new Map()
      for (const k of Array.from(uniq).sort(cmpBytes)) {
        out.set(k, obj.has(k) ? obj.get(k) : null)
      }
      return out
    }
    case 'omit': {
      ensureOnly(args, ['input', 'keys'])
      const obj = argObj(args, 'input')
      const drop = new Set(argStrArray(args, 'keys'))
      const out = new Map()
      for (const [k, v] of obj) if (!drop.has(k)) out.set(k, v)
      return out
    }

    // ---- utilities ----
    case 'constant': ensureOnly(args, ['value']); return argAny(args, 'value')
    case 'equals': ensureOnly(args, ['a', 'b']); return deepEqual(argAny(args, 'a'), argAny(args, 'b'))
    case 'compare': {
      ensureOnly(args, ['a', 'b', 'op'])
      const a = argAny(args, 'a')
      const b = argAny(args, 'b')
      const cop = argStr(args, 'op')
      const c = cmpValues(a, b)
      switch (cop) {
        case 'gt': return c > 0
        case 'gte': return c >= 0
        case 'lt': return c < 0
        case 'lte': return c <= 0
        default: throw new OpError('INVALID_ARG', `compare op must be gt, gte, lt or lte`)
      }
    }
    case 'and': ensureOnly(args, ['a', 'b']); return isTruthy(argAny(args, 'a')) && isTruthy(argAny(args, 'b'))
    case 'or': ensureOnly(args, ['a', 'b']); return isTruthy(argAny(args, 'a')) || isTruthy(argAny(args, 'b'))
    case 'not': ensureOnly(args, ['a']); return !isTruthy(argAny(args, 'a'))
    case 'type': ensureOnly(args, ['value']); return kindOf(argAny(args, 'value'))

    // ---- test op ----
    case 'fail_until': {
      ensureOnly(args, ['failures', 'value'])
      const failures = argI64(args, 'failures')
      if (failures < 0n || failures > 10n) throw new OpError('INVALID_ARG', "'failures' must be an integer 0..10")
      const val = argAny(args, 'value')
      if (BigInt(attempt) <= failures) throw new OpError('TRANSIENT_FAILURE', 'transient failure')
      return val
    }
  }
  throw new OpError('INVALID_ARG', `unknown op '${name}'`)
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------

function executeStep (st, ctx) {
  if (st.when !== null) {
    const resolve = makeResolver(st, ctx)
    const cond = evalCondition(st.when, resolve)
    if (cond === false) return { skipped: true, value: null }
  }
  const resolve = makeResolver(st, ctx)
  const args = resolve(st.args)
  let attempt = 0
  for (;;) {
    attempt++
    try {
      return { skipped: false, value: invokeOp(st.op, args, attempt) }
    } catch (e) {
      if (!(e instanceof OpError)) throw e
      if (e.code === 'TRANSIENT_FAILURE' && attempt < st.retryMax) continue
      const code = e.code === 'TRANSIENT_FAILURE' ? 'STEP_FAILED' : e.code
      throw new StepError(code, e.message)
    }
  }
}

function failureResponse (results, order, skippedList, code, step, message) {
  const err = new Map([
    ['code', code],
    ['step', step === undefined ? null : step],
    ['message', message || code]
  ])
  return new Map([
    ['status', 'failed'],
    ['results', results],
    ['execution_order', order],
    ['skipped', skippedList],
    ['error', err]
  ])
}

function successResponse (results, order, skippedList) {
  return new Map([
    ['status', 'completed'],
    ['results', results],
    ['execution_order', order],
    ['skipped', skippedList],
    ['error', null]
  ])
}

function runWorkflow (steps) {
  const ctx = {
    byId: new Map(steps.map(s => [s.id, s])),
    closures: computeClosures(steps),
    results: new Map(),
    finished: new Set(),
    skipped: new Set()
  }
  const order = []
  const pending = steps.slice()

  while (pending.length > 0) {
    let idx = -1
    for (let i = 0; i < pending.length; i++) {
      const st = pending[i]
      let ready = true
      for (const d of st.dependsOn) {
        if (!ctx.finished.has(d)) { ready = false; break }
      }
      if (ready) { idx = i; break }
    }
    if (idx < 0) throw new Error('no ready step although the graph is acyclic')

    const st = pending.splice(idx, 1)[0]
    let outcome
    try {
      outcome = executeStep(st, ctx)
    } catch (e) {
      if (e instanceof StepError) {
        return { response: failureResponse(ctx.results, order, order.filter(id => ctx.skipped.has(id)), e.code, st.id, e.message), exit: 1 }
      }
      if (e instanceof OpError) {
        const code = e.code === 'TRANSIENT_FAILURE' ? 'STEP_FAILED' : e.code
        return { response: failureResponse(ctx.results, order, order.filter(id => ctx.skipped.has(id)), code, st.id, e.message), exit: 1 }
      }
      throw e
    }

    ctx.results.set(st.id, outcome.value)
    ctx.finished.add(st.id)
    if (outcome.skipped) ctx.skipped.add(st.id)
    order.push(st.id)
  }

  const skippedList = order.filter(id => ctx.skipped.has(id))
  return { response: successResponse(ctx.results, order, skippedList), exit: 0 }
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

function emit (response, exitCode) {
  const text = ser(response) + '\n'
  try {
    process.stdout.write(text)
  } catch (e) {
    // stdout closed; nothing sensible we can do
  }
  process.exitCode = exitCode
}

function inputError (message) {
  return failureResponse(new Map(), [], [], 'INVALID_WORKFLOW', null, message)
}

function main (argv) {
  if (argv.length === 0) {
    emit(inputError('missing command; expected: run <workflow.json>'), 2)
    return
  }
  if (argv[0] !== 'run') {
    emit(inputError(`unknown command '${argv[0]}'; expected 'run'`), 2)
    return
  }
  if (argv.length < 2) {
    emit(inputError('missing workflow file argument'), 2)
    return
  }
  if (argv.length > 2) {
    emit(inputError('too many arguments'), 2)
    return
  }

  const source = argv[1]
  let text
  try {
    if (source === '-') {
      text = fs.readFileSync(0, 'utf8')
    } else {
      text = fs.readFileSync(source, 'utf8')
    }
  } catch (e) {
    emit(inputError(`cannot read workflow input: ${e.message}`), 2)
    return
  }

  let doc
  try {
    doc = parseJson(text)
  } catch (e) {
    if (e instanceof JsonParseError) {
      emit(inputError(`invalid JSON: ${e.message}`), 2)
      return
    }
    throw e
  }

  let steps
  try {
    steps = validateDocument(doc)
  } catch (e) {
    if (e instanceof WfError) {
      emit(failureResponse(new Map(), [], [], e.code, e.step, e.message), 2)
      return
    }
    throw e
  }

  const { response, exit } = runWorkflow(steps)
  emit(response, exit)
}

try {
  main(process.argv.slice(2))
} catch (e) {
  emit(failureResponse(new Map(), [], [], 'INTERNAL_ERROR', null, String(e && e.stack ? e.stack : e)), 3)
}
