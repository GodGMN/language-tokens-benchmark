//! Declarative workflow engine CLI.
//!
//! Implements the contract described in SPEC.md: a small, deterministic,
//! closed set of operations over JSON values with i64 arithmetic, dependency
//! ordered execution, conditions, references and retries.

use serde_json::{Map, Number, Value};
use std::cmp::Ordering;
use std::collections::{HashMap, HashSet};
use std::io::Read;
use std::process::exit;

type JMap = Map<String, Value>;

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

/// Workflow level error (validation or a step failure). `code` is one of the
/// codes from SPEC section 7.
#[derive(Debug, Clone)]
struct WfError {
    code: &'static str,
    step: Option<String>,
    message: String,
}

impl WfError {
    fn new(code: &'static str, step: Option<String>, message: impl Into<String>) -> Self {
        WfError {
            code,
            step,
            message: message.into(),
        }
    }
}

/// Runtime (per-operation / per-reference) error. The step id is attached by
/// the caller.
#[derive(Debug, Clone)]
struct RErr {
    code: &'static str,
    message: String,
}

impl RErr {
    fn new(code: &'static str, message: impl Into<String>) -> Self {
        RErr {
            code,
            message: message.into(),
        }
    }
}

fn inv_arg<M: Into<String>>(m: M) -> RErr {
    RErr::new("INVALID_ARG", m)
}
fn invalid_ref<M: Into<String>>(m: M) -> RErr {
    RErr::new("INVALID_REFERENCE", m)
}
fn skipped_ref<M: Into<String>>(m: M) -> RErr {
    RErr::new("SKIPPED_REFERENCE", m)
}
fn invalid_cond<M: Into<String>>(m: M) -> RErr {
    RErr::new("INVALID_CONDITION", m)
}
fn overflow() -> RErr {
    RErr::new("ARITHMETIC_OVERFLOW", "integer overflow")
}
fn div_zero() -> RErr {
    RErr::new("DIVISION_BY_ZERO", "division by zero")
}
fn empty_array() -> RErr {
    RErr::new("EMPTY_ARRAY", "operation on empty array")
}
fn transient() -> RErr {
    RErr::new("TRANSIENT_FAILURE", "transient failure")
}

// ---------------------------------------------------------------------------
// Known operations
// ---------------------------------------------------------------------------

const OPS: &[&str] = &[
    "add",
    "subtract",
    "multiply",
    "divide",
    "mod",
    "sum",
    "min",
    "max",
    "range",
    "concat",
    "split",
    "join",
    "uppercase",
    "lowercase",
    "trim",
    "replace",
    "length",
    "map",
    "filter",
    "reduce",
    "sort",
    "reverse",
    "unique",
    "slice",
    "flatten",
    "get",
    "set",
    "keys",
    "values",
    "merge",
    "pick",
    "omit",
    "constant",
    "equals",
    "compare",
    "and",
    "or",
    "not",
    "type",
    "fail_until",
];

// ---------------------------------------------------------------------------
// Workflow model
// ---------------------------------------------------------------------------

#[derive(Debug, Clone)]
struct Step {
    id: String,
    op: String,
    args: JMap,
    depends_on: Vec<String>,
    when: Option<Value>,
    max_attempts: u32,
}

fn valid_id(s: &str) -> bool {
    let mut it = s.bytes();
    match it.next() {
        Some(b) if b.is_ascii_alphabetic() || b == b'_' => {}
        _ => return false,
    }
    it.all(|b| b.is_ascii_alphanumeric() || b == b'_')
}

const STEP_FIELDS: &[&str] = &["id", "op", "args", "depends_on", "when", "retry"];
const TOP_FIELDS: &[&str] = &["version", "steps"];

fn wf(code: &'static str, step: Option<String>, msg: impl Into<String>) -> WfError {
    WfError::new(code, step, msg)
}

fn validate(doc: &Value) -> Result<Vec<Step>, WfError> {
    let obj = doc
        .as_object()
        .ok_or_else(|| wf("INVALID_WORKFLOW", None, "workflow must be a JSON object"))?;

    for k in obj.keys() {
        if !TOP_FIELDS.contains(&k.as_str()) {
            return Err(wf(
                "INVALID_WORKFLOW",
                None,
                format!("unknown top-level field '{}'", k),
            ));
        }
    }

    // version: required integer literal 1 (1.0 / 1e0 are rejected).
    let version = obj
        .get("version")
        .ok_or_else(|| wf("INVALID_WORKFLOW", None, "missing 'version'"))?;
    if !(version.is_i64() && version.as_i64() == Some(1)) {
        return Err(wf(
            "INVALID_WORKFLOW",
            None,
            "'version' must be the integer literal 1",
        ));
    }

    let steps_val = obj
        .get("steps")
        .ok_or_else(|| wf("INVALID_WORKFLOW", None, "missing 'steps'"))?;
    let steps_arr = steps_val
        .as_array()
        .ok_or_else(|| wf("INVALID_WORKFLOW", None, "'steps' must be an array"))?;

    let mut steps: Vec<Step> = Vec::with_capacity(steps_arr.len());
    let mut seen: HashSet<String> = HashSet::new();

    for sv in steps_arr {
        let so = match sv.as_object() {
            Some(o) => o,
            None => {
                return Err(wf(
                    "INVALID_WORKFLOW",
                    None,
                    "each step must be a JSON object",
                ))
            }
        };

        // Id used for error reporting: only when it is a string.
        let id_hint: Option<String> = match so.get("id") {
            Some(Value::String(s)) => Some(s.clone()),
            _ => None,
        };

        for k in so.keys() {
            if !STEP_FIELDS.contains(&k.as_str()) {
                return Err(wf(
                    "INVALID_WORKFLOW",
                    id_hint,
                    format!("unknown step field '{}'", k),
                ));
            }
        }

        let id = match so.get("id") {
            Some(Value::String(s)) => s.clone(),
            _ => {
                return Err(wf(
                    "INVALID_WORKFLOW",
                    id_hint,
                    "step 'id' must be a string",
                ))
            }
        };

        if !valid_id(&id) {
            return Err(wf(
                "INVALID_WORKFLOW",
                Some(id),
                "step id does not match [A-Za-z_][A-Za-z0-9_]*",
            ));
        }
        if !seen.insert(id.clone()) {
            return Err(wf(
                "INVALID_WORKFLOW",
                Some(id),
                "duplicate step id",
            ));
        }

        let op = match so.get("op") {
            Some(Value::String(s)) => s.clone(),
            _ => {
                return Err(wf(
                    "INVALID_WORKFLOW",
                    Some(id),
                    "step 'op' must be a string",
                ))
            }
        };
        if !OPS.contains(&op.as_str()) {
            return Err(wf(
                "INVALID_WORKFLOW",
                Some(id),
                format!("unknown operation '{}'", op),
            ));
        }

        let args = match so.get("args") {
            None => JMap::new(),
            Some(Value::Object(m)) => m.clone(),
            Some(_) => {
                return Err(wf(
                    "INVALID_WORKFLOW",
                    Some(id),
                    "step 'args' must be an object",
                ))
            }
        };

        let depends_on = match so.get("depends_on") {
            None => Vec::new(),
            Some(Value::Array(a)) => {
                let mut v = Vec::with_capacity(a.len());
                for d in a {
                    match d.as_str() {
                        Some(s) => v.push(s.to_string()),
                        None => {
                            return Err(wf(
                                "INVALID_WORKFLOW",
                                Some(id),
                                "'depends_on' must be an array of strings",
                            ))
                        }
                    }
                }
                v
            }
            Some(_) => {
                return Err(wf(
                    "INVALID_WORKFLOW",
                    Some(id),
                    "'depends_on' must be an array",
                ))
            }
        };

        // Condition shape: must be an object, either a reference object or one
        // carrying a string 'op'. Deeper problems surface as INVALID_CONDITION
        // at evaluation time.
        let when = match so.get("when") {
            None => None,
            Some(w) => {
                let wo = match w.as_object() {
                    Some(o) => o,
                    None => {
                        return Err(wf(
                            "INVALID_WORKFLOW",
                            Some(id),
                            "'when' must be an object",
                        ))
                    }
                };
                let is_ref = wo.len() == 1
                    && matches!(wo.get("$ref"), Some(Value::String(_)));
                if !is_ref {
                    match wo.get("op") {
                        Some(Value::String(_)) => {}
                        _ => {
                            return Err(wf(
                                "INVALID_WORKFLOW",
                                Some(id),
                                "'when' must contain a string 'op'",
                            ))
                        }
                    }
                }
                Some(w.clone())
            }
        };

        let max_attempts = match so.get("retry") {
            None => 1u32,
            Some(Value::Object(r)) => {
                for k in r.keys() {
                    if k != "max_attempts" {
                        return Err(wf(
                            "INVALID_WORKFLOW",
                            Some(id),
                            format!("unknown retry field '{}'", k),
                        ));
                    }
                }
                match r.get("max_attempts") {
                    None => 1,
                    Some(v) => {
                        if v.is_i64() {
                            if let Some(n) = v.as_i64() {
                                if (1..=5).contains(&n) {
                                    n as u32
                                } else {
                                    return Err(wf(
                                        "INVALID_WORKFLOW",
                                        Some(id),
                                        "'retry.max_attempts' must be in 1..5",
                                    ));
                                }
                            } else {
                                return Err(wf(
                                    "INVALID_WORKFLOW",
                                    Some(id),
                                    "'retry.max_attempts' must be an integer",
                                ));
                            }
                        } else {
                            return Err(wf(
                                "INVALID_WORKFLOW",
                                Some(id),
                                "'retry.max_attempts' must be an integer literal",
                            ));
                        }
                    }
                }
            }
            Some(_) => {
                return Err(wf(
                    "INVALID_WORKFLOW",
                    Some(id),
                    "'retry' must be an object",
                ))
            }
        };

        steps.push(Step {
            id,
            op,
            args,
            depends_on,
            when,
            max_attempts,
        });
    }

    let index: HashMap<String, usize> = steps
        .iter()
        .enumerate()
        .map(|(i, s)| (s.id.clone(), i))
        .collect();

    // Existing dependencies.
    for s in &steps {
        for d in &s.depends_on {
            if !index.contains_key(d) {
                return Err(wf(
                    "UNKNOWN_DEPENDENCY",
                    Some(s.id.clone()),
                    format!("unknown dependency '{}'", d),
                ));
            }
        }
    }

    // Cycle detection (Kahn).
    let n = steps.len();
    let mut indeg = vec![0usize; n];
    let mut adj: Vec<Vec<usize>> = vec![Vec::new(); n];
    for (i, s) in steps.iter().enumerate() {
        let mut deps: HashSet<usize> = HashSet::new();
        for d in &s.depends_on {
            deps.insert(index[d]);
        }
        indeg[i] = deps.len();
        for d in deps {
            adj[d].push(i);
        }
    }
    let mut queue: Vec<usize> = (0..n).filter(|&i| indeg[i] == 0).collect();
    let mut qi = 0usize;
    let mut processed = 0usize;
    while qi < queue.len() {
        let u = queue[qi];
        qi += 1;
        processed += 1;
        for &v in &adj[u] {
            indeg[v] -= 1;
            if indeg[v] == 0 {
                queue.push(v);
            }
        }
    }
    if processed != n {
        return Err(wf(
            "CYCLIC_DEPENDENCY",
            None,
            "workflow contains a dependency cycle",
        ));
    }

    Ok(steps)
}

fn transitive_deps(
    steps: &[Step],
    index: &HashMap<String, usize>,
    i: usize,
) -> HashSet<String> {
    let mut out: HashSet<String> = HashSet::new();
    let mut stack: Vec<usize> = steps[i]
        .depends_on
        .iter()
        .filter_map(|d| index.get(d).copied())
        .collect();
    while let Some(u) = stack.pop() {
        if out.insert(steps[u].id.clone()) {
            for d in &steps[u].depends_on {
                if let Some(&j) = index.get(d) {
                    stack.push(j);
                }
            }
        }
    }
    out
}

// ---------------------------------------------------------------------------
// Value semantics
// ---------------------------------------------------------------------------

fn value_i64(v: &Value) -> Option<i64> {
    // Only JSON integer literals are accepted for numeric parameters:
    // 1.0 / 1e3 are floats in serde_json and therefore rejected here.
    if v.is_i64() {
        v.as_i64()
    } else {
        None
    }
}

fn type_rank(v: &Value) -> u8 {
    match v {
        Value::Number(_) => 0,
        Value::String(_) => 1,
        Value::Bool(_) => 2,
        Value::Null => 3,
        Value::Array(_) => 4,
        Value::Object(_) => 5,
    }
}

fn is_type_name(s: &str) -> bool {
    matches!(s, "null" | "boolean" | "number" | "string" | "array" | "object")
}

fn type_name(v: &Value) -> &'static str {
    match v {
        Value::Null => "null",
        Value::Bool(_) => "boolean",
        Value::Number(_) => "number",
        Value::String(_) => "string",
        Value::Array(_) => "array",
        Value::Object(_) => "object",
    }
}

fn cmp_numbers(a: &Number, b: &Number) -> Ordering {
    if let (Some(x), Some(y)) = (a.as_i64(), b.as_i64()) {
        return x.cmp(&y);
    }
    if let (Some(x), Some(y)) = (a.as_u64(), b.as_u64()) {
        return x.cmp(&y);
    }
    let x = a.as_f64().unwrap_or(0.0);
    let y = b.as_f64().unwrap_or(0.0);
    x.partial_cmp(&y).unwrap_or(Ordering::Equal)
}

fn cmp_key_seqs(a: &[&String], b: &[&String]) -> Ordering {
    let n = a.len().min(b.len());
    for i in 0..n {
        let c = a[i].as_bytes().cmp(b[i].as_bytes());
        if c != Ordering::Equal {
            return c;
        }
    }
    a.len().cmp(&b.len())
}

fn sorted_keys(m: &JMap) -> Vec<&String> {
    let mut ks: Vec<&String> = m.keys().collect();
    ks.sort_by(|a, b| a.as_bytes().cmp(b.as_bytes()));
    ks
}

fn cmp_values(a: &Value, b: &Value) -> Ordering {
    let ra = type_rank(a);
    let rb = type_rank(b);
    if ra != rb {
        return ra.cmp(&rb);
    }
    match (a, b) {
        (Value::Number(x), Value::Number(y)) => cmp_numbers(x, y),
        (Value::String(x), Value::String(y)) => x.as_bytes().cmp(y.as_bytes()),
        (Value::Bool(x), Value::Bool(y)) => x.cmp(y),
        (Value::Null, Value::Null) => Ordering::Equal,
        (Value::Array(x), Value::Array(y)) => {
            let n = x.len().min(y.len());
            for i in 0..n {
                let c = cmp_values(&x[i], &y[i]);
                if c != Ordering::Equal {
                    return c;
                }
            }
            x.len().cmp(&y.len())
        }
        (Value::Object(x), Value::Object(y)) => {
            let kx = sorted_keys(x);
            let ky = sorted_keys(y);
            let c = cmp_key_seqs(&kx, &ky);
            if c != Ordering::Equal {
                return c;
            }
            for k in kx {
                let c = cmp_values(&x[k], &y[k]);
                if c != Ordering::Equal {
                    return c;
                }
            }
            Ordering::Equal
        }
        _ => Ordering::Equal,
    }
}

fn eq_values(a: &Value, b: &Value) -> bool {
    match (a, b) {
        (Value::Null, Value::Null) => true,
        (Value::Bool(x), Value::Bool(y)) => x == y,
        (Value::Number(x), Value::Number(y)) => cmp_numbers(x, y) == Ordering::Equal,
        (Value::String(x), Value::String(y)) => x == y,
        (Value::Array(x), Value::Array(y)) => {
            x.len() == y.len() && x.iter().zip(y.iter()).all(|(p, q)| eq_values(p, q))
        }
        (Value::Object(x), Value::Object(y)) => {
            x.len() == y.len()
                && x.iter()
                    .all(|(k, v)| y.get(k).map(|w| eq_values(v, w)).unwrap_or(false))
        }
        _ => false,
    }
}

const SAFE_INT: i64 = 9_007_199_254_740_992; // 2^53, exactly representable in f64

/// Hash key for values whose structural equality can be decided by an exact
/// key. Only integers with magnitude <= 2^53 (where f64 conversion is exact)
/// and the non-numeric scalar types qualify; floats, large integers, arrays
/// and objects go through exact pairwise comparison instead.
fn fast_value_key(v: &Value) -> Option<String> {
    match v {
        Value::Null => Some("z".to_string()),
        Value::Bool(b) => Some(if *b { "b1" } else { "b0" }.to_string()),
        Value::String(s) => Some(format!("s{}", s)),
        Value::Number(n) => n
            .as_i64()
            .filter(|i| *i >= -SAFE_INT && *i <= SAFE_INT)
            .map(|i| format!("n{}", i)),
        _ => None,
    }
}

/// Deduplicate by structural equality, keeping the first occurrence.
///
/// The fast path keeps the common cases (small integers, strings, booleans,
/// null) linear; everything else is compared pairwise so the exact numeric
/// equality rules (1 == 1.0, exact i64 comparison for large integers) are
/// preserved.
fn unique_values(arr: &[Value]) -> Vec<Value> {
    let mut out: Vec<Value> = Vec::new();
    let mut fast: HashSet<String> = HashSet::new();
    let mut slow: Vec<&Value> = Vec::new();
    for e in arr {
        if let Some(key) = fast_value_key(e) {
            if fast.contains(&key) {
                continue;
            }
            // A safe integer can equal an earlier float (1 == 1.0).
            if e.is_number() && slow.iter().any(|s| eq_values(*s, e)) {
                continue;
            }
            fast.insert(key);
            out.push(e.clone());
        } else {
            if slow.iter().any(|s| eq_values(*s, e)) {
                continue;
            }
            // A float can equal an earlier safe integer.
            if let Value::Number(n) = e {
                if n.is_f64() {
                    if let Some(f) = n.as_f64() {
                        if f.fract() == 0.0 && f.abs() <= SAFE_INT as f64 {
                            if fast.contains(&format!("n{}", f as i64)) {
                                continue;
                            }
                        }
                    }
                }
            }
            slow.push(e);
            out.push(e.clone());
        }
    }
    out
}

fn truthy(v: &Value) -> bool {
    match v {
        Value::Null => false,
        Value::Bool(b) => *b,
        Value::Number(n) => {
            if let Some(i) = n.as_i64() {
                i != 0
            } else if let Some(u) = n.as_u64() {
                u != 0
            } else {
                n.as_f64().map(|f| f != 0.0).unwrap_or(false)
            }
        }
        Value::String(s) => !s.is_empty(),
        Value::Array(a) => !a.is_empty(),
        Value::Object(o) => !o.is_empty(),
    }
}

// ---------------------------------------------------------------------------
// Argument helpers
// ---------------------------------------------------------------------------

fn check_keys(args: &JMap, allowed: &[&str]) -> Result<(), RErr> {
    for k in args.keys() {
        if !allowed.contains(&k.as_str()) {
            return Err(inv_arg(format!("unknown argument '{}'", k)));
        }
    }
    Ok(())
}

fn get_arg<'a>(args: &'a JMap, name: &str) -> Result<&'a Value, RErr> {
    args.get(name)
        .ok_or_else(|| inv_arg(format!("missing argument '{}'", name)))
}

fn get_i64(args: &JMap, name: &str) -> Result<i64, RErr> {
    value_i64(get_arg(args, name)?)
        .ok_or_else(|| inv_arg(format!("argument '{}' must be an integer literal", name)))
}

fn get_str<'a>(args: &'a JMap, name: &str) -> Result<&'a str, RErr> {
    get_arg(args, name)?
        .as_str()
        .ok_or_else(|| inv_arg(format!("argument '{}' must be a string", name)))
}

fn get_arr<'a>(args: &'a JMap, name: &str) -> Result<&'a Vec<Value>, RErr> {
    get_arg(args, name)?
        .as_array()
        .ok_or_else(|| inv_arg(format!("argument '{}' must be an array", name)))
}

fn get_obj<'a>(args: &'a JMap, name: &str) -> Result<&'a JMap, RErr> {
    get_arg(args, name)?
        .as_object()
        .ok_or_else(|| inv_arg(format!("argument '{}' must be an object", name)))
}

fn bin_i64(a: i64, b: i64, op: &str) -> Result<i64, RErr> {
    let r = match op {
        "add" => a.checked_add(b),
        "subtract" => a.checked_sub(b),
        "multiply" => a.checked_mul(b),
        "divide" => {
            if b == 0 {
                return Err(div_zero());
            }
            if a == i64::MIN && b == -1 {
                return Err(overflow());
            }
            Some(a / b)
        }
        "mod" => {
            if b == 0 {
                return Err(div_zero());
            }
            if a == i64::MIN && b == -1 {
                return Err(overflow());
            }
            Some(a % b)
        }
        _ => return Err(inv_arg(format!("unsupported arithmetic op '{}'", op))),
    };
    r.ok_or_else(overflow)
}

// ---------------------------------------------------------------------------
// References
// ---------------------------------------------------------------------------

struct RefCtx<'a> {
    results: &'a HashMap<String, Value>,
    skipped: &'a HashSet<String>,
    allowed: &'a HashSet<String>,
}

fn resolve_ref(path: &str, ctx: &RefCtx<'_>) -> Result<Value, RErr> {
    let mut parts = path.split('.');
    let sid = parts.next().unwrap_or("");
    if sid.is_empty() {
        return Err(invalid_ref("empty reference"));
    }
    if !ctx.allowed.contains(sid) {
        return Err(invalid_ref(format!(
            "reference to '{}' which is not a dependency",
            sid
        )));
    }
    if ctx.skipped.contains(sid) {
        return Err(skipped_ref(format!("reference to skipped step '{}'", sid)));
    }
    let mut cur = ctx
        .results
        .get(sid)
        .cloned()
        .ok_or_else(|| invalid_ref(format!("reference to unknown step '{}'", sid)))?;

    for seg in parts {
        match cur {
            Value::Object(ref m) => {
                let next = m
                    .get(seg)
                    .cloned()
                    .ok_or_else(|| invalid_ref(format!("missing key '{}'", seg)))?;
                cur = next;
            }
            Value::Array(ref a) => {
                if seg.is_empty() || !seg.bytes().all(|b| b.is_ascii_digit()) {
                    return Err(invalid_ref(format!("invalid array index '{}'", seg)));
                }
                let idx: usize = seg
                    .parse()
                    .map_err(|_| invalid_ref(format!("invalid array index '{}'", seg)))?;
                let next = a
                    .get(idx)
                    .cloned()
                    .ok_or_else(|| invalid_ref(format!("array index {} out of range", idx)))?;
                cur = next;
            }
            _ => {
                return Err(invalid_ref(format!(
                    "cannot descend into {} value with segment '{}'",
                    type_name(&cur),
                    seg
                )))
            }
        }
    }
    Ok(cur)
}

fn resolve_value(v: &Value, ctx: &RefCtx<'_>) -> Result<Value, RErr> {
    match v {
        Value::Object(m) => {
            if m.len() == 1 {
                if let Some(Value::String(p)) = m.get("$ref") {
                    return resolve_ref(p, ctx);
                }
            }
            let mut out = JMap::new();
            for (k, val) in m {
                out.insert(k.clone(), resolve_value(val, ctx)?);
            }
            Ok(Value::Object(out))
        }
        Value::Array(a) => {
            let mut out = Vec::with_capacity(a.len());
            for val in a {
                out.push(resolve_value(val, ctx)?);
            }
            Ok(Value::Array(out))
        }
        _ => Ok(v.clone()),
    }
}

// ---------------------------------------------------------------------------
// Conditions
// ---------------------------------------------------------------------------

fn eval_condition(v: &Value, ctx: &RefCtx<'_>) -> Result<bool, RErr> {
    if let Value::Object(m) = v {
        if m.len() == 1 {
            if let Some(Value::String(p)) = m.get("$ref") {
                let val = resolve_ref(p, ctx)?;
                return match val {
                    Value::Bool(b) => Ok(b),
                    _ => Err(invalid_cond("condition reference is not a boolean")),
                };
            }
        }
        let op = match m.get("op") {
            Some(Value::String(s)) => s.as_str(),
            _ => return Err(invalid_cond("condition must have a string 'op'")),
        };
        match op {
            "eq" | "ne" | "gt" | "gte" | "lt" | "lte" => {
                let l = m
                    .get("left")
                    .ok_or_else(|| invalid_cond("condition missing 'left'"))?;
                let r = m
                    .get("right")
                    .ok_or_else(|| invalid_cond("condition missing 'right'"))?;
                let lv = resolve_value(l, ctx)?;
                let rv = resolve_value(r, ctx)?;
                let ord = cmp_values(&lv, &rv);
                Ok(match op {
                    "eq" => eq_values(&lv, &rv),
                    "ne" => !eq_values(&lv, &rv),
                    "gt" => ord == Ordering::Greater,
                    "gte" => ord != Ordering::Less,
                    "lt" => ord == Ordering::Less,
                    "lte" => ord != Ordering::Greater,
                    _ => unreachable!(),
                })
            }
            "all" => {
                let arr = match m.get("conditions") {
                    Some(Value::Array(a)) => a,
                    _ => return Err(invalid_cond("'all' requires a 'conditions' array")),
                };
                for c in arr {
                    if !eval_condition(c, ctx)? {
                        return Ok(false);
                    }
                }
                Ok(true)
            }
            "any" => {
                let arr = match m.get("conditions") {
                    Some(Value::Array(a)) => a,
                    _ => return Err(invalid_cond("'any' requires a 'conditions' array")),
                };
                for c in arr {
                    if eval_condition(c, ctx)? {
                        return Ok(true);
                    }
                }
                Ok(false)
            }
            "not" => {
                let c = m
                    .get("condition")
                    .ok_or_else(|| invalid_cond("'not' requires a 'condition'"))?;
                Ok(!eval_condition(c, ctx)?)
            }
            other => Err(invalid_cond(format!(
                "unknown condition operator '{}'",
                other
            ))),
        }
    } else if let Value::Bool(b) = v {
        Ok(*b)
    } else {
        Err(invalid_cond("condition must be a JSON object"))
    }
}

// ---------------------------------------------------------------------------
// Transforms / predicates
// ---------------------------------------------------------------------------

fn spec_type(spec: &Value) -> Result<String, RErr> {
    match spec {
        Value::String(s) => Ok(s.clone()),
        Value::Object(m) => match m.get("type") {
            Some(Value::String(s)) => Ok(s.clone()),
            _ => Err(inv_arg("transform/predicate requires a string 'type'")),
        },
        _ => Err(inv_arg("transform/predicate must be an object")),
    }
}

fn spec_get<'a>(spec: &'a Value, key: &str) -> Option<&'a Value> {
    spec.as_object().and_then(|m| m.get(key))
}

fn substitute_self(v: &Value, elem: &Value) -> Value {
    match v {
        Value::Object(m) => {
            if m.len() == 1 && m.contains_key("$self") {
                return elem.clone();
            }
            let mut out = JMap::new();
            for (k, val) in m {
                out.insert(k.clone(), substitute_self(val, elem));
            }
            Value::Object(out)
        }
        Value::Array(a) => {
            Value::Array(a.iter().map(|x| substitute_self(x, elem)).collect())
        }
        other => other.clone(),
    }
}

fn apply_transform(
    elem: &Value,
    spec: &Value,
    step_id: &str,
    fail_counts: &mut HashMap<String, u32>,
    depth: u32,
) -> Result<Value, RErr> {
    if depth > 64 {
        return Err(inv_arg("transform nesting too deep"));
    }
    let ty = spec_type(spec)?;
    match ty.as_str() {
        "constant" => spec_get(spec, "value")
            .cloned()
            .ok_or_else(|| inv_arg("constant transform requires 'value'")),
        "add" | "subtract" | "multiply" | "divide" | "mod" => {
            let v = spec_get(spec, "value")
                .ok_or_else(|| inv_arg("transform requires 'value'"))?;
            let n = value_i64(v)
                .ok_or_else(|| inv_arg("transform 'value' must be an integer literal"))?;
            let e = value_i64(elem)
                .ok_or_else(|| inv_arg("transform element must be an integer literal"))?;
            Ok(Value::from(bin_i64(e, n, &ty)?))
        }
        "negate" => {
            let e = value_i64(elem)
                .ok_or_else(|| inv_arg("negate element must be an integer literal"))?;
            e.checked_neg().map(Value::from).ok_or_else(overflow)
        }
        "abs" => {
            let e = value_i64(elem)
                .ok_or_else(|| inv_arg("abs element must be an integer literal"))?;
            e.checked_abs().map(Value::from).ok_or_else(overflow)
        }
        "stringify" => Ok(Value::String(
            serde_json::to_string(elem).unwrap_or_else(|_| "null".to_string()),
        )),
        "type" => Ok(Value::String(type_name(elem).to_string())),
        "op" => {
            let opname = spec_get(spec, "op")
                .and_then(|v| v.as_str())
                .ok_or_else(|| inv_arg("op transform requires a string 'op'"))?;
            let raw_args = spec_get(spec, "args")
                .ok_or_else(|| inv_arg("op transform requires 'args'"))?;
            let substituted = substitute_self(raw_args, elem);
            let m = substituted
                .as_object()
                .ok_or_else(|| inv_arg("op transform 'args' must be an object"))?;
            exec_op(step_id, opname, m, fail_counts, depth + 1)
        }
        other => Err(inv_arg(format!("unknown transform type '{}'", other))),
    }
}

fn apply_predicate(elem: &Value, spec: &Value) -> Result<bool, RErr> {
    let ty = spec_type(spec)?;
    match ty.as_str() {
        "even" | "odd" => {
            let e = value_i64(elem)
                .ok_or_else(|| inv_arg("even/odd predicate requires an integer element"))?;
            Ok(if ty == "even" { e % 2 == 0 } else { e % 2 != 0 })
        }
        "gt" | "gte" | "lt" | "lte" => {
            let v = spec_get(spec, "value")
                .ok_or_else(|| inv_arg("comparison predicate requires 'value'"))?;
            let n = value_i64(v).ok_or_else(|| {
                inv_arg("comparison predicate 'value' must be an integer literal")
            })?;
            let ord = cmp_values(elem, &Value::from(n));
            Ok(match ty.as_str() {
                "gt" => ord == Ordering::Greater,
                "gte" => ord != Ordering::Less,
                "lt" => ord == Ordering::Less,
                _ => ord != Ordering::Greater,
            })
        }
        "eq" | "ne" => {
            let v = spec_get(spec, "value")
                .ok_or_else(|| inv_arg("equality predicate requires 'value'"))?;
            let n = value_i64(v).ok_or_else(|| {
                inv_arg("equality predicate 'value' must be an integer literal")
            })?;
            let eq = eq_values(elem, &Value::from(n));
            Ok(if ty == "eq" { eq } else { !eq })
        }
        "truthy" => Ok(truthy(elem)),
        "falsy" => Ok(!truthy(elem)),
        "type_is" => {
            let target = spec_get(spec, "value")
                .and_then(|v| v.as_str())
                .or_else(|| spec_get(spec, "target").and_then(|v| v.as_str()))
                .or_else(|| spec_get(spec, "type_name").and_then(|v| v.as_str()))
                .or_else(|| spec_get(spec, "expected").and_then(|v| v.as_str()))
                .or_else(|| spec_get(spec, "of").and_then(|v| v.as_str()))
                .or_else(|| {
                    // The SPEC table spelling has a duplicate "type" key, so
                    // accept whatever key carries a JSON type name.
                    spec.as_object().and_then(|m| {
                        m.iter().find_map(|(k, v)| {
                            if k == "type" {
                                return None;
                            }
                            v.as_str().filter(|s| is_type_name(s))
                        })
                    })
                })
                .ok_or_else(|| inv_arg("type_is predicate requires a target type"))?;
            Ok(type_name(elem) == target)
        }
        "string_contains" => {
            let needle = spec_get(spec, "value")
                .and_then(|v| v.as_str())
                .ok_or_else(|| inv_arg("string_contains requires a string 'value'"))?;
            let hay = elem
                .as_str()
                .ok_or_else(|| inv_arg("string_contains requires a string element"))?;
            Ok(hay.contains(needle))
        }
        // Tolerate the compact spelling {"type":"number"} for type_is
        // (the spec table spelling {"type":"type_is","type":...} cannot be
        // represented as a JSON object with two identical keys).
        "null" | "boolean" | "number" | "string" | "array" | "object" => {
            Ok(type_name(elem) == ty)
        }
        other => Err(inv_arg(format!("unknown predicate type '{}'", other))),
    }
}

// ---------------------------------------------------------------------------
// Operation execution
// ---------------------------------------------------------------------------

fn exec_op(
    step_id: &str,
    op: &str,
    args: &JMap,
    fail_counts: &mut HashMap<String, u32>,
    depth: u32,
) -> Result<Value, RErr> {
    if depth > 64 {
        return Err(inv_arg("operation nesting too deep"));
    }
    match op {
        "add" | "subtract" | "multiply" | "divide" | "mod" => {
            check_keys(args, &["a", "b"])?;
            let a = get_i64(args, "a")?;
            let b = get_i64(args, "b")?;
            Ok(Value::from(bin_i64(a, b, op)?))
        }
        "sum" => {
            check_keys(args, &["input"])?;
            let arr = get_arr(args, "input")?;
            let mut acc: i64 = 0;
            for v in arr {
                let n = value_i64(v)
                    .ok_or_else(|| inv_arg("'sum' input must be an array of integers"))?;
                acc = acc.checked_add(n).ok_or_else(overflow)?;
            }
            Ok(Value::from(acc))
        }
        "min" | "max" => {
            check_keys(args, &["input"])?;
            let arr = get_arr(args, "input")?;
            if arr.is_empty() {
                return Err(empty_array());
            }
            let mut best = value_i64(&arr[0])
                .ok_or_else(|| inv_arg("'min'/'max' input must be an array of integers"))?;
            for v in &arr[1..] {
                let n = value_i64(v)
                    .ok_or_else(|| inv_arg("'min'/'max' input must be an array of integers"))?;
                if (op == "min" && n < best) || (op == "max" && n > best) {
                    best = n;
                }
            }
            Ok(Value::from(best))
        }
        "range" => {
            check_keys(args, &["start", "end"])?;
            let start = get_i64(args, "start")?;
            let end = get_i64(args, "end")?;
            let mut out: Vec<Value> = Vec::new();
            if start <= end {
                let mut v = start;
                loop {
                    out.push(Value::from(v));
                    if v == end || v == i64::MAX {
                        break;
                    }
                    v += 1;
                }
            }
            Ok(Value::Array(out))
        }
        "concat" => {
            check_keys(args, &["a", "b"])?;
            let a = get_str(args, "a")?;
            let b = get_str(args, "b")?;
            let mut s = String::with_capacity(a.len() + b.len());
            s.push_str(a);
            s.push_str(b);
            Ok(Value::String(s))
        }
        "split" => {
            check_keys(args, &["input", "sep"])?;
            let input = get_str(args, "input")?;
            let sep = get_str(args, "sep")?;
            let parts: Vec<Value> = if sep.is_empty() {
                input
                    .chars()
                    .map(|c| Value::String(c.to_string()))
                    .collect()
            } else {
                input
                    .split(sep)
                    .map(|s| Value::String(s.to_string()))
                    .collect()
            };
            Ok(Value::Array(parts))
        }
        "join" => {
            check_keys(args, &["input", "sep"])?;
            let arr = get_arr(args, "input")?;
            let sep = get_str(args, "sep")?;
            let mut out = String::new();
            for (i, v) in arr.iter().enumerate() {
                let s = v
                    .as_str()
                    .ok_or_else(|| inv_arg("'join' input must be an array of strings"))?;
                if i > 0 {
                    out.push_str(sep);
                }
                out.push_str(s);
            }
            Ok(Value::String(out))
        }
        "uppercase" => {
            check_keys(args, &["input"])?;
            let s = get_str(args, "input")?;
            Ok(Value::String(s.chars().map(|c| c.to_ascii_uppercase()).collect()))
        }
        "lowercase" => {
            check_keys(args, &["input"])?;
            let s = get_str(args, "input")?;
            Ok(Value::String(s.chars().map(|c| c.to_ascii_lowercase()).collect()))
        }
        "trim" => {
            check_keys(args, &["input"])?;
            let s = get_str(args, "input")?;
            Ok(Value::String(
                s.trim_matches(|c: char| {
                    matches!(c, ' ' | '\t' | '\n' | '\r' | '\u{0C}' | '\u{0B}')
                })
                .to_string(),
            ))
        }
        "replace" => {
            check_keys(args, &["input", "search", "replace"])?;
            let input = get_str(args, "input")?;
            let search = get_str(args, "search")?;
            let rep = get_str(args, "replace")?;
            if search.is_empty() {
                return Err(inv_arg("'replace' search string must not be empty"));
            }
            Ok(Value::String(input.replace(search, rep)))
        }
        "length" => {
            check_keys(args, &["input"])?;
            let s = get_str(args, "input")?;
            Ok(Value::from(s.chars().count() as i64))
        }
        "map" => {
            check_keys(args, &["input", "transform"])?;
            let input = get_arr(args, "input")?;
            let transform = get_arg(args, "transform")?;
            let mut out = Vec::with_capacity(input.len());
            for e in input {
                out.push(apply_transform(
                    e, transform, step_id, fail_counts, depth,
                )?);
            }
            Ok(Value::Array(out))
        }
        "filter" => {
            check_keys(args, &["input", "predicate"])?;
            let input = get_arr(args, "input")?;
            let pred = get_arg(args, "predicate")?;
            let mut out = Vec::new();
            for e in input {
                if apply_predicate(e, pred)? {
                    out.push(e.clone());
                }
            }
            Ok(Value::Array(out))
        }
        "reduce" => {
            check_keys(args, &["input", "op", "initial"])?;
            let input = get_arr(args, "input")?;
            let opname = get_str(args, "op")?;
            if !matches!(opname, "add" | "subtract" | "multiply") {
                return Err(inv_arg(
                    "'reduce' op must be one of add, subtract, multiply",
                ));
            }
            let mut acc = match args.get("initial") {
                Some(v) => value_i64(v).ok_or_else(|| {
                    inv_arg("'reduce' initial must be an integer literal")
                })?,
                None => {
                    if opname == "multiply" {
                        1
                    } else {
                        0
                    }
                }
            };
            for e in input {
                let n = value_i64(e)
                    .ok_or_else(|| inv_arg("'reduce' input must be integers"))?;
                acc = bin_i64(acc, n, opname)?;
            }
            Ok(Value::from(acc))
        }
        "sort" => {
            check_keys(args, &["input"])?;
            let arr = get_arr(args, "input")?;
            let mut out = arr.clone();
            out.sort_by(|a, b| cmp_values(a, b));
            Ok(Value::Array(out))
        }
        "reverse" => {
            check_keys(args, &["input"])?;
            let arr = get_arr(args, "input")?;
            let mut out = arr.clone();
            out.reverse();
            Ok(Value::Array(out))
        }
        "unique" => {
            check_keys(args, &["input"])?;
            let arr = get_arr(args, "input")?;
            Ok(Value::Array(unique_values(arr)))
        }
        "slice" => {
            check_keys(args, &["input", "start", "end"])?;
            let arr = get_arr(args, "input")?;
            let start = get_i64(args, "start")?;
            let end: Option<i64> = match args.get("end") {
                Some(v) => Some(
                    value_i64(v)
                        .ok_or_else(|| inv_arg("'end' must be an integer literal"))?,
                ),
                None => None,
            };
            let len = arr.len() as i128;
            let norm = |i: i64| -> i128 {
                let i = i as i128;
                if i < 0 {
                    let x = len + i;
                    if x < 0 {
                        0
                    } else if x > len {
                        len
                    } else {
                        x
                    }
                } else if i > len {
                    len
                } else {
                    i
                }
            };
            let s = norm(start);
            let e = end.map(norm).unwrap_or(len);
            if s >= e {
                Ok(Value::Array(Vec::new()))
            } else {
                Ok(Value::Array(
                    arr[s as usize..e as usize].to_vec(),
                ))
            }
        }
        "flatten" => {
            check_keys(args, &["input"])?;
            let arr = get_arr(args, "input")?;
            let mut out: Vec<Value> = Vec::new();
            for e in arr {
                match e {
                    Value::Array(inner) => out.extend(inner.iter().cloned()),
                    other => out.push(other.clone()),
                }
            }
            Ok(Value::Array(out))
        }
        "get" => {
            check_keys(args, &["input", "key"])?;
            let obj = get_obj(args, "input")?;
            let key = get_str(args, "key")?;
            Ok(obj.get(key).cloned().unwrap_or(Value::Null))
        }
        "set" => {
            check_keys(args, &["input", "key", "value"])?;
            let obj = get_obj(args, "input")?;
            let key = get_str(args, "key")?.to_string();
            let value = get_arg(args, "value")?.clone();
            let mut out = obj.clone();
            out.insert(key, value);
            Ok(Value::Object(out))
        }
        "keys" => {
            check_keys(args, &["input"])?;
            let obj = get_obj(args, "input")?;
            let keys: Vec<Value> = sorted_keys(obj)
                .into_iter()
                .map(|k| Value::String(k.clone()))
                .collect();
            Ok(Value::Array(keys))
        }
        "values" => {
            check_keys(args, &["input"])?;
            let obj = get_obj(args, "input")?;
            let vals: Vec<Value> = sorted_keys(obj)
                .into_iter()
                .map(|k| obj[k].clone())
                .collect();
            Ok(Value::Array(vals))
        }
        "merge" => {
            check_keys(args, &["a", "b"])?;
            let a = get_obj(args, "a")?;
            let b = get_obj(args, "b")?;
            let mut out = a.clone();
            for (k, v) in b {
                out.insert(k.clone(), v.clone());
            }
            Ok(Value::Object(out))
        }
        "pick" => {
            check_keys(args, &["input", "keys"])?;
            let obj = get_obj(args, "input")?;
            let keys = get_arr(args, "keys")?;
            let mut out = JMap::new();
            for k in keys {
                let key = k
                    .as_str()
                    .ok_or_else(|| inv_arg("'pick' keys must be strings"))?;
                out.insert(
                    key.to_string(),
                    obj.get(key).cloned().unwrap_or(Value::Null),
                );
            }
            Ok(Value::Object(out))
        }
        "omit" => {
            check_keys(args, &["input", "keys"])?;
            let obj = get_obj(args, "input")?;
            let keys = get_arr(args, "keys")?;
            let mut out = obj.clone();
            for k in keys {
                let key = k
                    .as_str()
                    .ok_or_else(|| inv_arg("'omit' keys must be strings"))?;
                out.remove(key);
            }
            Ok(Value::Object(out))
        }
        "constant" => {
            check_keys(args, &["value"])?;
            Ok(get_arg(args, "value")?.clone())
        }
        "equals" => {
            check_keys(args, &["a", "b"])?;
            let a = get_arg(args, "a")?;
            let b = get_arg(args, "b")?;
            Ok(Value::Bool(eq_values(a, b)))
        }
        "compare" => {
            check_keys(args, &["a", "b", "op"])?;
            let a = get_arg(args, "a")?;
            let b = get_arg(args, "b")?;
            let opname = get_str(args, "op")?;
            let ord = cmp_values(a, b);
            let res = match opname {
                "gt" => ord == Ordering::Greater,
                "gte" => ord != Ordering::Less,
                "lt" => ord == Ordering::Less,
                "lte" => ord != Ordering::Greater,
                _ => {
                    return Err(inv_arg(
                        "'compare' op must be one of gt, gte, lt, lte",
                    ))
                }
            };
            Ok(Value::Bool(res))
        }
        "and" => {
            check_keys(args, &["a", "b"])?;
            let a = get_arg(args, "a")?;
            let b = get_arg(args, "b")?;
            Ok(Value::Bool(truthy(a) && truthy(b)))
        }
        "or" => {
            check_keys(args, &["a", "b"])?;
            let a = get_arg(args, "a")?;
            let b = get_arg(args, "b")?;
            Ok(Value::Bool(truthy(a) || truthy(b)))
        }
        "not" => {
            check_keys(args, &["a"])?;
            let a = get_arg(args, "a")?;
            Ok(Value::Bool(!truthy(a)))
        }
        "type" => {
            check_keys(args, &["value"])?;
            let v = get_arg(args, "value")?;
            Ok(Value::String(type_name(v).to_string()))
        }
        "fail_until" => {
            check_keys(args, &["failures", "value"])?;
            let failures = get_i64(args, "failures")?;
            if !(0..=10).contains(&failures) {
                return Err(inv_arg("'failures' must be in 0..10"));
            }
            let value = get_arg(args, "value")?.clone();
            let count = fail_counts.entry(step_id.to_string()).or_insert(0);
            *count += 1;
            if (*count as i64) <= failures {
                Err(transient())
            } else {
                Ok(value)
            }
        }
        other => Err(inv_arg(format!("unknown operation '{}'", other))),
    }
}

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------

enum StepOutcome {
    Completed(Value),
    Skipped,
}

struct Engine<'a> {
    steps: &'a [Step],
    index: &'a HashMap<String, usize>,
    transitive: Vec<HashSet<String>>,
    results: HashMap<String, Value>,
    skipped: HashSet<String>,
    skipped_list: Vec<String>,
    execution_order: Vec<String>,
    fail_counts: HashMap<String, u32>,
}

impl<'a> Engine<'a> {
    fn new(steps: &'a [Step], index: &'a HashMap<String, usize>) -> Self {
        let transitive = (0..steps.len())
            .map(|i| transitive_deps(steps, index, i))
            .collect();
        Engine {
            steps,
            index,
            transitive,
            results: HashMap::new(),
            skipped: HashSet::new(),
            skipped_list: Vec::new(),
            execution_order: Vec::new(),
            fail_counts: HashMap::new(),
        }
    }

    fn run(&mut self) -> Result<(), WfError> {
        let n = self.steps.len();
        let mut done = vec![false; n];
        let mut finished = 0usize;
        while finished < n {
            let mut chosen: Option<usize> = None;
            for i in 0..n {
                if done[i] {
                    continue;
                }
                let ready = self.steps[i]
                    .depends_on
                    .iter()
                    .all(|d| done[self.index[d]]);
                if ready {
                    chosen = Some(i);
                    break;
                }
            }
            let i = match chosen {
                Some(i) => i,
                None => {
                    return Err(WfError::new(
                        "INTERNAL_ERROR",
                        None,
                        "no ready step (dependency deadlock)",
                    ))
                }
            };
            match self.run_step(i)? {
                StepOutcome::Completed(v) => {
                    let id = self.steps[i].id.clone();
                    self.results.insert(id.clone(), v);
                    self.execution_order.push(id);
                }
                StepOutcome::Skipped => {
                    let id = self.steps[i].id.clone();
                    self.results.insert(id.clone(), Value::Null);
                    self.skipped.insert(id.clone());
                    self.skipped_list.push(id.clone());
                    self.execution_order.push(id);
                }
            }
            done[i] = true;
            finished += 1;
        }
        Ok(())
    }

    fn run_step(&mut self, i: usize) -> Result<StepOutcome, WfError> {
        let id = self.steps[i].id.clone();
        let op = self.steps[i].op.clone();
        let args_val = Value::Object(self.steps[i].args.clone());
        let when = self.steps[i].when.clone();
        let max_attempts = self.steps[i].max_attempts;
        let allowed = self.transitive[i].clone();

        let resolved = {
            let ctx = RefCtx {
                results: &self.results,
                skipped: &self.skipped,
                allowed: &allowed,
            };

            if let Some(w) = &when {
                let cond = eval_condition(w, &ctx)
                    .map_err(|e| WfError::new(e.code, Some(id.clone()), e.message))?;
                if !cond {
                    return Ok(StepOutcome::Skipped);
                }
            }

            resolve_value(&args_val, &ctx)
                .map_err(|e| WfError::new(e.code, Some(id.clone()), e.message))?
        };

        let args_obj = resolved.as_object().cloned().unwrap_or_default();

        let mut last: Option<RErr> = None;
        for attempt in 1..=max_attempts {
            match exec_op(&id, &op, &args_obj, &mut self.fail_counts, 0) {
                Ok(v) => return Ok(StepOutcome::Completed(v)),
                Err(e) => {
                    if e.code == "TRANSIENT_FAILURE" {
                        last = Some(e);
                        if attempt < max_attempts {
                            continue;
                        }
                        let msg = last
                            .map(|x| x.message)
                            .unwrap_or_else(|| "step failed".to_string());
                        return Err(WfError::new("STEP_FAILED", Some(id), msg));
                    }
                    return Err(WfError::new(e.code, Some(id), e.message));
                }
            }
        }
        // Unreachable when max_attempts >= 1, but keep it total.
        Err(WfError::new(
            "STEP_FAILED",
            Some(id),
            last.map(|e| e.message)
                .unwrap_or_else(|| "step failed".to_string()),
        ))
    }

    fn results_object(&self) -> JMap {
        let mut results = JMap::new();
        for id in &self.execution_order {
            if let Some(v) = self.results.get(id) {
                results.insert(id.clone(), v.clone());
            }
        }
        results
    }

    fn execution_order_json(&self) -> Value {
        Value::Array(
            self.execution_order
                .iter()
                .map(|s| Value::String(s.clone()))
                .collect(),
        )
    }

    fn skipped_json(&self) -> Value {
        Value::Array(
            self.skipped_list
                .iter()
                .map(|s| Value::String(s.clone()))
                .collect(),
        )
    }

    fn response(&self, error: Option<&WfError>) -> Value {
        let mut resp = JMap::new();
        match error {
            None => {
                resp.insert("status".to_string(), Value::String("completed".to_string()));
                resp.insert("results".to_string(), Value::Object(self.results_object()));
                resp.insert("execution_order".to_string(), self.execution_order_json());
                resp.insert("skipped".to_string(), self.skipped_json());
                resp.insert("error".to_string(), Value::Null);
            }
            Some(e) => {
                resp.insert("status".to_string(), Value::String("failed".to_string()));
                resp.insert("results".to_string(), Value::Object(self.results_object()));
                resp.insert("execution_order".to_string(), self.execution_order_json());
                resp.insert("skipped".to_string(), self.skipped_json());
                resp.insert("error".to_string(), error_object(e));
            }
        }
        Value::Object(resp)
    }
}

fn error_object(e: &WfError) -> Value {
    let mut m = JMap::new();
    m.insert("code".to_string(), Value::String(e.code.to_string()));
    m.insert(
        "step".to_string(),
        match &e.step {
            Some(s) => Value::String(s.clone()),
            None => Value::Null,
        },
    );
    m.insert("message".to_string(), Value::String(e.message.clone()));
    Value::Object(m)
}

fn empty_response(
    code: &'static str,
    step: Option<String>,
    message: impl Into<String>,
) -> Value {
    let mut resp = JMap::new();
    resp.insert("status".to_string(), Value::String("failed".to_string()));
    resp.insert("results".to_string(), Value::Object(JMap::new()));
    resp.insert("execution_order".to_string(), Value::Array(Vec::new()));
    resp.insert("skipped".to_string(), Value::Array(Vec::new()));
    resp.insert(
        "error".to_string(),
        error_object(&WfError::new(code, step, message)),
    );
    Value::Object(resp)
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

fn to_json_string(v: &Value) -> String {
    serde_json::to_string(v).unwrap_or_else(|_| {
        "{\"status\":\"failed\",\"results\":{},\"execution_order\":[],\"skipped\":[],\"error\":{\"code\":\"INTERNAL_ERROR\",\"step\":null,\"message\":\"serialization failure\"}}".to_string()
    })
}

fn run_cli(argv: &[String]) -> (String, i32) {
    if argv.len() != 3 || argv[1] != "run" {
        return (
            to_json_string(&empty_response(
                "INVALID_WORKFLOW",
                None,
                "usage: workflow run <workflow.json|->",
            )),
            2,
        );
    }

    let input: String = if argv[2] == "-" {
        let mut buf = String::new();
        match std::io::stdin().read_to_string(&mut buf) {
            Ok(_) => buf,
            Err(e) => {
                return (
                    to_json_string(&empty_response(
                        "INVALID_WORKFLOW",
                        None,
                        format!("failed to read stdin: {}", e),
                    )),
                    2,
                )
            }
        }
    } else {
        match std::fs::read_to_string(&argv[2]) {
            Ok(s) => s,
            Err(e) => {
                return (
                    to_json_string(&empty_response(
                        "INVALID_WORKFLOW",
                        None,
                        format!("failed to read input file: {}", e),
                    )),
                    2,
                )
            }
        }
    };

    let doc: Value = match serde_json::from_str(&input) {
        Ok(v) => v,
        Err(e) => {
            return (
                to_json_string(&empty_response(
                    "INVALID_WORKFLOW",
                    None,
                    format!("invalid JSON document: {}", e),
                )),
                2,
            )
        }
    };

    let steps = match validate(&doc) {
        Ok(s) => s,
        Err(e) => {
            let out = to_json_string(&empty_response(
                match e.code {
                    "UNKNOWN_DEPENDENCY" => "UNKNOWN_DEPENDENCY",
                    "CYCLIC_DEPENDENCY" => "CYCLIC_DEPENDENCY",
                    _ => "INVALID_WORKFLOW",
                },
                e.step,
                e.message,
            ));
            return (out, 2);
        }
    };

    let index: HashMap<String, usize> = steps
        .iter()
        .enumerate()
        .map(|(i, s)| (s.id.clone(), i))
        .collect();

    let mut engine = Engine::new(&steps, &index);
    match engine.run() {
        Ok(()) => (to_json_string(&engine.response(None)), 0),
        Err(e) => {
            let code = if e.code == "INTERNAL_ERROR" { 3 } else { 1 };
            (to_json_string(&engine.response(Some(&e))), code)
        }
    }
}

fn main() {
    let argv: Vec<String> = std::env::args().collect();
    let outcome = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| run_cli(&argv)));
    match outcome {
        Ok((out, code)) => {
            println!("{}", out);
            let _ = std::io::Write::flush(&mut std::io::stdout());
            exit(code);
        }
        Err(_) => {
            let resp = empty_response("INTERNAL_ERROR", None, "internal program error");
            println!("{}", to_json_string(&resp));
            let _ = std::io::Write::flush(&mut std::io::stdout());
            exit(3);
        }
    }
}
