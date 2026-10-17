use serde_json::{Map, Number, Value};
use std::cmp::Ordering;
use std::collections::{HashMap, HashSet};
use std::io::{Read, Write};

// ---------------------------------------------------------------------------
// Error type
// ---------------------------------------------------------------------------

#[derive(Debug, Clone)]
struct WfError {
    code: String,
    step: Option<String>,
    message: String,
}

impl WfError {
    fn new(code: &str, step: Option<String>, message: impl Into<String>) -> Self {
        WfError {
            code: code.to_string(),
            step,
            message: message.into(),
        }
    }
}

fn err(code: &str, step: Option<String>, message: impl Into<String>) -> WfError {
    WfError::new(code, step, message)
}

fn invalid_wf(step: Option<&str>, message: impl Into<String>) -> WfError {
    err("INVALID_WORKFLOW", step.map(|s| s.to_string()), message)
}

fn at_step(mut e: WfError, id: &str) -> WfError {
    if e.step.is_none() {
        e.step = Some(id.to_string());
    }
    e
}

// ---------------------------------------------------------------------------
// Validation / workflow model
// ---------------------------------------------------------------------------

const OPS: &[&str] = &[
    "add", "subtract", "multiply", "divide", "mod", "sum", "min", "max", "range", "concat",
    "split", "join", "uppercase", "lowercase", "trim", "replace", "length", "map", "filter",
    "reduce", "sort", "reverse", "unique", "slice", "flatten", "get", "set", "keys", "values",
    "merge", "pick", "omit", "constant", "equals", "compare", "and", "or", "not", "type",
    "fail_until",
];

#[derive(Debug, Clone)]
struct StepDef {
    id: String,
    op: String,
    args: Map<String, Value>,
    depends_on: Vec<String>,
    when: Option<Value>,
    max_attempts: u32,
    transitive: HashSet<String>,
}

struct Workflow {
    steps: Vec<StepDef>,
}

fn valid_id(s: &str) -> bool {
    let mut chars = s.chars();
    match chars.next() {
        Some(c) if c.is_ascii_alphabetic() || c == '_' => {}
        _ => return false,
    }
    chars.all(|c| c.is_ascii_alphanumeric() || c == '_')
}

fn validate_condition(c: &Value) -> Result<(), String> {
    let obj = c
        .as_object()
        .ok_or_else(|| "condition must be an object".to_string())?;
    let op = obj
        .get("op")
        .and_then(Value::as_str)
        .ok_or_else(|| "condition op must be a string".to_string())?;
    match op {
        "eq" | "ne" | "gt" | "gte" | "lt" | "lte" => {
            for k in obj.keys() {
                if !["op", "left", "right"].contains(&k.as_str()) {
                    return Err(format!("unknown condition field '{}'", k));
                }
            }
            if !obj.contains_key("left") || !obj.contains_key("right") {
                return Err("comparison condition requires left and right".to_string());
            }
            Ok(())
        }
        "all" | "any" => {
            for k in obj.keys() {
                if !["op", "conditions"].contains(&k.as_str()) {
                    return Err(format!("unknown condition field '{}'", k));
                }
            }
            let arr = obj
                .get("conditions")
                .and_then(Value::as_array)
                .ok_or_else(|| "conditions must be an array".to_string())?;
            for ch in arr {
                validate_condition(ch)?;
            }
            Ok(())
        }
        "not" => {
            for k in obj.keys() {
                if !["op", "condition"].contains(&k.as_str()) {
                    return Err(format!("unknown condition field '{}'", k));
                }
            }
            let ch = obj
                .get("condition")
                .ok_or_else(|| "not condition requires condition".to_string())?;
            validate_condition(ch)
        }
        _ => Err(format!("unknown condition op '{}'", op)),
    }
}

fn compute_closure(
    idx: usize,
    steps: &[StepDef],
    index: &HashMap<String, usize>,
    state: &mut Vec<u8>,
    closures: &mut Vec<HashSet<String>>,
) -> Result<HashSet<String>, WfError> {
    if state[idx] == 1 {
        return Err(err("CYCLIC_DEPENDENCY", None, "dependency cycle"));
    }
    if state[idx] == 2 {
        return Ok(closures[idx].clone());
    }
    state[idx] = 1;
    let mut deps = steps[idx].depends_on.clone();
    deps.sort();
    deps.dedup();
    let mut set: HashSet<String> = HashSet::new();
    for d in deps {
        let j = match index.get(&d) {
            Some(j) => *j,
            None => {
                return Err(err(
                    "UNKNOWN_DEPENDENCY",
                    Some(steps[idx].id.clone()),
                    format!("unknown dependency '{}'", d),
                ))
            }
        };
        set.insert(d.clone());
        let sub = compute_closure(j, steps, index, state, closures)?;
        for x in sub {
            set.insert(x);
        }
    }
    state[idx] = 2;
    closures[idx] = set.clone();
    Ok(set)
}

fn validate(doc: &Value) -> Result<Workflow, WfError> {
    let root = doc
        .as_object()
        .ok_or_else(|| invalid_wf(None, "workflow must be an object"))?;
    for k in root.keys() {
        if k != "version" && k != "steps" {
            return Err(invalid_wf(None, format!("unknown top-level field '{}'", k)));
        }
    }
    let version = root
        .get("version")
        .ok_or_else(|| invalid_wf(None, "missing workflow version"))?;
    let version_ok = match version {
        Value::Number(n) => !n.is_f64() && n.as_i64() == Some(1),
        _ => false,
    };
    if !version_ok {
        return Err(invalid_wf(None, "version must be the integer 1"));
    }
    let steps_val = root
        .get("steps")
        .ok_or_else(|| invalid_wf(None, "missing steps"))?;
    let steps_arr = steps_val
        .as_array()
        .ok_or_else(|| invalid_wf(None, "steps must be an array"))?;

    let mut steps: Vec<StepDef> = Vec::new();
    let mut ids: HashSet<String> = HashSet::new();

    for sv in steps_arr {
        let obj = match sv.as_object() {
            Some(o) => o,
            None => return Err(invalid_wf(None, "step must be an object")),
        };
        let id_opt = obj.get("id").and_then(Value::as_str).map(|s| s.to_string());
        for k in obj.keys() {
            if !["id", "op", "args", "depends_on", "when", "retry"].contains(&k.as_str()) {
                return Err(invalid_wf(
                    id_opt.as_deref(),
                    format!("unknown step field '{}'", k),
                ));
            }
        }
        let id = match obj.get("id") {
            Some(Value::String(s)) => s.clone(),
            _ => return Err(invalid_wf(None, "step id must be a string")),
        };
        if !valid_id(&id) {
            return Err(invalid_wf(Some(&id), "invalid step id"));
        }
        if !ids.insert(id.clone()) {
            return Err(invalid_wf(Some(&id), "duplicate step id"));
        }
        let op = match obj.get("op") {
            Some(Value::String(s)) => s.clone(),
            _ => return Err(invalid_wf(Some(&id), "op must be a string")),
        };
        if !OPS.contains(&op.as_str()) {
            return Err(invalid_wf(Some(&id), format!("unknown op '{}'", op)));
        }
        let args = match obj.get("args") {
            None => Map::new(),
            Some(Value::Object(m)) => m.clone(),
            Some(_) => return Err(invalid_wf(Some(&id), "args must be an object")),
        };
        let depends_on = match obj.get("depends_on") {
            None => Vec::new(),
            Some(Value::Array(a)) => {
                let mut v = Vec::new();
                for x in a {
                    match x {
                        Value::String(s) => v.push(s.clone()),
                        _ => {
                            return Err(invalid_wf(
                                Some(&id),
                                "depends_on must be an array of strings",
                            ))
                        }
                    }
                }
                v
            }
            Some(_) => return Err(invalid_wf(Some(&id), "depends_on must be an array")),
        };
        let when = match obj.get("when") {
            None => None,
            Some(w) => {
                validate_condition(w).map_err(|m| invalid_wf(Some(&id), m))?;
                Some(w.clone())
            }
        };
        let max_attempts = match obj.get("retry") {
            None => 1u32,
            Some(Value::Object(r)) => {
                for k in r.keys() {
                    if k != "max_attempts" {
                        return Err(invalid_wf(Some(&id), "unknown retry field"));
                    }
                }
                match r.get("max_attempts") {
                    None => 1,
                    Some(Value::Number(n)) => {
                        let x = if n.is_f64() { None } else { n.as_i64() };
                        match x {
                            Some(v) if (1..=5).contains(&v) => v as u32,
                            _ => {
                                return Err(invalid_wf(
                                    Some(&id),
                                    "max_attempts must be an integer in 1..5",
                                ))
                            }
                        }
                    }
                    Some(_) => {
                        return Err(invalid_wf(Some(&id), "max_attempts must be an integer"))
                    }
                }
            }
            Some(_) => return Err(invalid_wf(Some(&id), "retry must be an object")),
        };
        steps.push(StepDef {
            id,
            op,
            args,
            depends_on,
            when,
            max_attempts,
            transitive: HashSet::new(),
        });
    }

    // Dependency existence check.
    for s in &steps {
        for d in &s.depends_on {
            if !ids.contains(d) {
                return Err(err(
                    "UNKNOWN_DEPENDENCY",
                    Some(s.id.clone()),
                    format!("unknown dependency '{}'", d),
                ));
            }
        }
    }

    // Cycle detection and transitive dependency sets.
    let index: HashMap<String, usize> = steps
        .iter()
        .enumerate()
        .map(|(i, s)| (s.id.clone(), i))
        .collect();
    let mut state = vec![0u8; steps.len()];
    let mut closures: Vec<HashSet<String>> = vec![HashSet::new(); steps.len()];
    for i in 0..steps.len() {
        let set = compute_closure(i, &steps, &index, &mut state, &mut closures)?;
        steps[i].transitive = set;
    }

    Ok(Workflow { steps })
}

// ---------------------------------------------------------------------------
// Comparators / equality / truthiness
// ---------------------------------------------------------------------------

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

fn num_i128(n: &Number) -> Option<i128> {
    if let Some(i) = n.as_i64() {
        Some(i as i128)
    } else {
        n.as_u64().map(|u| u as i128)
    }
}

fn num_f64(n: &Number) -> f64 {
    n.as_f64().unwrap_or(0.0)
}

// Compare an integer with a finite f64 exactly (without losing integer
// precision for values larger than 2^53).
fn cmp_int_float(i: i128, f: f64) -> Ordering {
    if f.is_nan() {
        return Ordering::Equal;
    }
    if f == f64::INFINITY {
        return Ordering::Less;
    }
    if f == f64::NEG_INFINITY {
        return Ordering::Greater;
    }
    let two_127: f64 = 170141183460469231731687303715884105728.0; // 2^127
    let t = f.trunc();
    if t >= two_127 {
        return Ordering::Less;
    }
    if t < -two_127 {
        return Ordering::Greater;
    }
    let ti = t as i128;
    match i.cmp(&ti) {
        Ordering::Equal => {
            if f > t {
                Ordering::Less
            } else if f < t {
                Ordering::Greater
            } else {
                Ordering::Equal
            }
        }
        other => other,
    }
}

fn cmp_numbers(a: &Number, b: &Number) -> Ordering {
    match (num_i128(a), num_i128(b)) {
        (Some(x), Some(y)) => x.cmp(&y),
        (Some(x), None) => cmp_int_float(x, num_f64(b)),
        (None, Some(y)) => cmp_int_float(y, num_f64(a)).reverse(),
        (None, None) => {
            let x = num_f64(a);
            let y = num_f64(b);
            x.partial_cmp(&y).unwrap_or(Ordering::Equal)
        }
    }
}

fn eq_numbers(a: &Number, b: &Number) -> bool {
    cmp_numbers(a, b) == Ordering::Equal
}

fn sorted_keys(map: &Map<String, Value>) -> Vec<&String> {
    let mut keys: Vec<&String> = map.keys().collect();
    keys.sort_by(|a, b| a.as_bytes().cmp(b.as_bytes()));
    keys
}

fn cmp_values(a: &Value, b: &Value) -> Ordering {
    let ra = type_rank(a);
    let rb = type_rank(b);
    if ra != rb {
        return ra.cmp(&rb);
    }
    match (a, b) {
        (Value::Null, Value::Null) => Ordering::Equal,
        (Value::Bool(x), Value::Bool(y)) => x.cmp(y),
        (Value::Number(x), Value::Number(y)) => cmp_numbers(x, y),
        (Value::String(x), Value::String(y)) => x.as_bytes().cmp(y.as_bytes()),
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
            let n = kx.len().min(ky.len());
            for i in 0..n {
                let c = kx[i].as_bytes().cmp(ky[i].as_bytes());
                if c != Ordering::Equal {
                    return c;
                }
            }
            if kx.len() != ky.len() {
                return kx.len().cmp(&ky.len());
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
    if type_rank(a) != type_rank(b) {
        return false;
    }
    match (a, b) {
        (Value::Null, Value::Null) => true,
        (Value::Bool(x), Value::Bool(y)) => x == y,
        (Value::Number(x), Value::Number(y)) => eq_numbers(x, y),
        (Value::String(x), Value::String(y)) => x == y,
        (Value::Array(x), Value::Array(y)) => {
            x.len() == y.len() && x.iter().zip(y.iter()).all(|(p, q)| eq_values(p, q))
        }
        (Value::Object(x), Value::Object(y)) => {
            x.len() == y.len()
                && x.iter().all(|(k, v)| match y.get(k) {
                    Some(w) => eq_values(v, w),
                    None => false,
                })
        }
        _ => false,
    }
}

fn truthy(v: &Value) -> bool {
    match v {
        Value::Null => false,
        Value::Bool(b) => *b,
        Value::Number(n) => num_f64(n) != 0.0,
        Value::String(s) => !s.is_empty(),
        Value::Array(a) => !a.is_empty(),
        Value::Object(o) => !o.is_empty(),
    }
}

// Canonical, collision-free key for structural equality.  Integral floats are
// normalized to the same key as the corresponding integer, matching the
// spec's `1 == 1.0` rule for numbers.
fn canonical_key(v: &Value) -> String {
    match v {
        Value::Null => "N".to_string(),
        Value::Bool(b) => format!("B{}", b),
        Value::Number(n) => match num_i128(n) {
            Some(i) => format!("I{}", i),
            None => {
                let f = num_f64(n);
                let two_127: f64 = 170141183460469231731687303715884105728.0;
                if f.fract() == 0.0 && f >= -two_127 && f < two_127 {
                    format!("I{}", f as i128)
                } else {
                    format!("F{:016x}", f.to_bits())
                }
            }
        },
        Value::String(s) => format!("S{}:{}", s.len(), s),
        Value::Array(a) => {
            let mut s = String::from("A");
            for x in a {
                let k = canonical_key(x);
                s.push_str(&format!("{}:{}", k.len(), k));
            }
            s.push(']');
            s
        }
        Value::Object(o) => {
            let mut s = String::from("O");
            for k in sorted_keys(o) {
                let vk = canonical_key(o.get(k).unwrap());
                s.push_str(&format!("{}:{}={}:{}", k.len(), k, vk.len(), vk));
            }
            s.push('}');
            s
        }
    }
}

// ---------------------------------------------------------------------------
// Argument access helpers
// ---------------------------------------------------------------------------

fn check_keys(args: &Map<String, Value>, allowed: &[&str]) -> Result<(), WfError> {
    for k in args.keys() {
        if !allowed.contains(&k.as_str()) {
            return Err(err(
                "INVALID_ARG",
                None,
                format!("unknown argument '{}'", k),
            ));
        }
    }
    Ok(())
}

fn req<'a>(args: &'a Map<String, Value>, name: &str) -> Result<&'a Value, WfError> {
    args.get(name)
        .ok_or_else(|| err("INVALID_ARG", None, format!("missing argument '{}'", name)))
}

fn req_i64(args: &Map<String, Value>, name: &str) -> Result<i64, WfError> {
    let v = req(args, name)?;
    i64_of(v, name)
}

fn i64_of(v: &Value, name: &str) -> Result<i64, WfError> {
    v.as_i64().ok_or_else(|| {
        err(
            "INVALID_ARG",
            None,
            format!("argument '{}' must be an integer", name),
        )
    })
}

fn req_str<'a>(args: &'a Map<String, Value>, name: &str) -> Result<&'a str, WfError> {
    req(args, name)?
        .as_str()
        .ok_or_else(|| err("INVALID_ARG", None, format!("argument '{}' must be a string", name)))
}

fn req_array<'a>(args: &'a Map<String, Value>, name: &str) -> Result<&'a Vec<Value>, WfError> {
    req(args, name)?
        .as_array()
        .ok_or_else(|| err("INVALID_ARG", None, format!("argument '{}' must be an array", name)))
}

fn req_obj<'a>(args: &'a Map<String, Value>, name: &str) -> Result<&'a Map<String, Value>, WfError> {
    req(args, name)?
        .as_object()
        .ok_or_else(|| err("INVALID_ARG", None, format!("argument '{}' must be an object", name)))
}

fn req_bool(args: &Map<String, Value>, name: &str) -> Result<bool, WfError> {
    req(args, name)?
        .as_bool()
        .ok_or_else(|| err("INVALID_ARG", None, format!("argument '{}' must be a boolean", name)))
}

// ---------------------------------------------------------------------------
// Integer arithmetic
// ---------------------------------------------------------------------------

fn arith(op: &str, a: i64, b: i64) -> Result<i64, WfError> {
    let overflow = || err("ARITHMETIC_OVERFLOW", None, "i64 overflow");
    match op {
        "add" => a.checked_add(b).ok_or_else(overflow),
        "subtract" => a.checked_sub(b).ok_or_else(overflow),
        "multiply" => a.checked_mul(b).ok_or_else(overflow),
        "divide" => {
            if b == 0 {
                Err(err("DIVISION_BY_ZERO", None, "division by zero"))
            } else if a == i64::MIN && b == -1 {
                Err(overflow())
            } else {
                Ok(a / b)
            }
        }
        "mod" => {
            if b == 0 {
                Err(err("DIVISION_BY_ZERO", None, "modulo by zero"))
            } else if a == i64::MIN && b == -1 {
                Err(overflow())
            } else {
                Ok(a % b)
            }
        }
        _ => Err(err("INVALID_ARG", None, format!("unsupported operator '{}'", op))),
    }
}

// ---------------------------------------------------------------------------
// map transform / filter predicate
// ---------------------------------------------------------------------------

fn substitute_self(v: &Value, elem: &Value) -> Value {
    match v {
        Value::Object(m) => {
            if m.len() == 1 {
                if let Some(Value::Bool(true)) = m.get("$self") {
                    return elem.clone();
                }
            }
            let mut out = Map::new();
            for (k, val) in m {
                out.insert(k.clone(), substitute_self(val, elem));
            }
            Value::Object(out)
        }
        Value::Array(a) => Value::Array(a.iter().map(|x| substitute_self(x, elem)).collect()),
        _ => v.clone(),
    }
}

fn apply_transform(elem: &Value, t: &Value) -> Result<Value, WfError> {
    let obj = t
        .as_object()
        .ok_or_else(|| err("INVALID_ARG", None, "transform must be an object"))?;
    let ty = obj
        .get("type")
        .and_then(Value::as_str)
        .ok_or_else(|| err("INVALID_ARG", None, "transform type must be a string"))?;
    match ty {
        "op" => {
            check_keys(obj, &["type", "op", "args"])?;
            let op = obj
                .get("op")
                .and_then(Value::as_str)
                .ok_or_else(|| err("INVALID_ARG", None, "transform op must be a string"))?;
            if !["add", "subtract", "multiply", "divide", "mod"].contains(&op) {
                return Err(err(
                    "INVALID_ARG",
                    None,
                    format!("unsupported transform op '{}'", op),
                ));
            }
            let targs = obj
                .get("args")
                .and_then(Value::as_object)
                .ok_or_else(|| err("INVALID_ARG", None, "transform args must be an object"))?;
            let mut resolved = Map::new();
            for (k, v) in targs {
                resolved.insert(k.clone(), substitute_self(v, elem));
            }
            eval_op(op, &resolved, 0)
        }
        "constant" => {
            check_keys(obj, &["type", "value"])?;
            Ok(req(obj, "value")?.clone())
        }
        "add" | "subtract" | "multiply" | "divide" | "mod" => {
            check_keys(obj, &["type", "value"])?;
            let n = i64_of(req(obj, "value")?, "value")?;
            let e = i64_of(elem, "input element")?;
            arith(ty, e, n).map(Value::from)
        }
        "negate" => {
            check_keys(obj, &["type"])?;
            let e = i64_of(elem, "input element")?;
            e.checked_neg().map(Value::from).ok_or_else(|| {
                err("ARITHMETIC_OVERFLOW", None, "i64 overflow in negate")
            })
        }
        "abs" => {
            check_keys(obj, &["type"])?;
            let e = i64_of(elem, "input element")?;
            e.checked_abs().map(Value::from).ok_or_else(|| {
                err("ARITHMETIC_OVERFLOW", None, "i64 overflow in abs")
            })
        }
        "stringify" => {
            check_keys(obj, &["type"])?;
            let s = serde_json::to_string(elem)
                .map_err(|e| err("INVALID_ARG", None, e.to_string()))?;
            Ok(Value::String(s))
        }
        "type" => {
            check_keys(obj, &["type"])?;
            Ok(Value::String(type_name(elem).to_string()))
        }
        _ => Err(err(
            "INVALID_ARG",
            None,
            format!("unknown transform type '{}'", ty),
        )),
    }
}

fn type_is_target(obj: &Map<String, Value>) -> Result<String, WfError> {
    // The spec writes {"type":"type_is","type":"<type>"} which has a duplicate
    // key in raw JSON.  Support the natural "value" spelling, the explicit
    // "type_name" spelling, and the duplicate-key collapse handled by the caller.
    for key in ["value", "type_name", "type_is"] {
        if let Some(v) = obj.get(key) {
            return v
                .as_str()
                .map(|s| s.to_string())
                .ok_or_else(|| err("INVALID_ARG", None, "type_is target must be a string"));
        }
    }
    Err(err("INVALID_ARG", None, "type_is requires a target type"))
}

fn apply_predicate(elem: &Value, p: &Value) -> Result<bool, WfError> {
    let obj = p
        .as_object()
        .ok_or_else(|| err("INVALID_ARG", None, "predicate must be an object"))?;
    let ty = obj
        .get("type")
        .and_then(Value::as_str)
        .ok_or_else(|| err("INVALID_ARG", None, "predicate type must be a string"))?;
    match ty {
        "even" | "odd" => {
            check_keys(obj, &["type"])?;
            let e = i64_of(elem, "input element")?;
            let even = e % 2 == 0;
            Ok(if ty == "even" { even } else { !even })
        }
        "gt" | "gte" | "lt" | "lte" | "eq" | "ne" => {
            check_keys(obj, &["type", "value"])?;
            let n = i64_of(req(obj, "value")?, "value")?;
            let target = Value::from(n);
            Ok(match ty {
                "eq" => eq_values(elem, &target),
                "ne" => !eq_values(elem, &target),
                "gt" => cmp_values(elem, &target) == Ordering::Greater,
                "gte" => cmp_values(elem, &target) != Ordering::Less,
                "lt" => cmp_values(elem, &target) == Ordering::Less,
                "lte" => cmp_values(elem, &target) != Ordering::Greater,
                _ => unreachable!(),
            })
        }
        "truthy" => {
            check_keys(obj, &["type"])?;
            Ok(truthy(elem))
        }
        "falsy" => {
            check_keys(obj, &["type"])?;
            Ok(!truthy(elem))
        }
        "type_is" => {
            for k in obj.keys() {
                if !["type", "value", "type_name", "type_is"].contains(&k.as_str()) {
                    return Err(err(
                        "INVALID_ARG",
                        None,
                        format!("unknown predicate field '{}'", k),
                    ));
                }
            }
            let target = type_is_target(obj)?;
            Ok(type_name(elem) == target)
        }
        // Raw duplicate-key JSON such as {"type":"type_is","type":"string"}
        // collapses to {"type":"string"} in most parsers.
        _ if is_type_name(ty) && obj.len() == 1 => Ok(type_name(elem) == ty),
        "string_contains" => {
            check_keys(obj, &["type", "value"])?;
            let needle = req(obj, "value")?
                .as_str()
                .ok_or_else(|| err("INVALID_ARG", None, "value must be a string"))?;
            let s = elem
                .as_str()
                .ok_or_else(|| err("INVALID_ARG", None, "input element must be a string"))?;
            Ok(s.contains(needle))
        }
        _ => Err(err(
            "INVALID_ARG",
            None,
            format!("unknown predicate type '{}'", ty),
        )),
    }
}

fn is_type_name(s: &str) -> bool {
    matches!(s, "null" | "boolean" | "number" | "string" | "array" | "object")
}

// ---------------------------------------------------------------------------
// Operation evaluation
// ---------------------------------------------------------------------------

fn eval_op(op: &str, args: &Map<String, Value>, attempt: u32) -> Result<Value, WfError> {
    match op {
        "add" | "subtract" | "multiply" | "divide" | "mod" => {
            check_keys(args, &["a", "b"])?;
            let a = req_i64(args, "a")?;
            let b = req_i64(args, "b")?;
            arith(op, a, b).map(Value::from)
        }
        "sum" => {
            check_keys(args, &["input"])?;
            let arr = req_array(args, "input")?;
            let mut total: i64 = 0;
            for v in arr {
                let n = i64_of(v, "input element")?;
                total = total.checked_add(n).ok_or_else(|| {
                    err("ARITHMETIC_OVERFLOW", None, "i64 overflow in sum")
                })?;
            }
            Ok(Value::from(total))
        }
        "min" | "max" => {
            check_keys(args, &["input"])?;
            let arr = req_array(args, "input")?;
            if arr.is_empty() {
                return Err(err(
                    "EMPTY_ARRAY",
                    None,
                    format!("{} requires a non-empty array", op),
                ));
            }
            let mut best: Option<i64> = None;
            for v in arr {
                let n = i64_of(v, "input element")?;
                best = Some(match best {
                    None => n,
                    Some(cur) => {
                        if op == "min" {
                            cur.min(n)
                        } else {
                            cur.max(n)
                        }
                    }
                });
            }
            Ok(Value::from(best.unwrap()))
        }
        "range" => {
            check_keys(args, &["start", "end"])?;
            let start = req_i64(args, "start")?;
            let end = req_i64(args, "end")?;
            let mut out = Vec::new();
            if start <= end {
                let mut cur = start;
                loop {
                    out.push(Value::from(cur));
                    if cur == end {
                        break;
                    }
                    cur += 1;
                }
            }
            Ok(Value::Array(out))
        }
        "concat" => {
            check_keys(args, &["a", "b"])?;
            let a = req_str(args, "a")?;
            let b = req_str(args, "b")?;
            Ok(Value::String(format!("{}{}", a, b)))
        }
        "split" => {
            check_keys(args, &["input", "sep"])?;
            let input = req_str(args, "input")?;
            let sep = req_str(args, "sep")?;
            let parts: Vec<Value> = if sep.is_empty() {
                input
                    .chars()
                    .map(|c| Value::String(c.to_string()))
                    .collect()
            } else {
                input.split(sep).map(|s| Value::String(s.to_string())).collect()
            };
            Ok(Value::Array(parts))
        }
        "join" => {
            check_keys(args, &["input", "sep"])?;
            let arr = req_array(args, "input")?;
            let sep = req_str(args, "sep")?;
            let mut parts: Vec<&str> = Vec::with_capacity(arr.len());
            for v in arr {
                let s = v
                    .as_str()
                    .ok_or_else(|| err("INVALID_ARG", None, "input element must be a string"))?;
                parts.push(s);
            }
            Ok(Value::String(parts.join(sep)))
        }
        "uppercase" => {
            check_keys(args, &["input"])?;
            let s = req_str(args, "input")?;
            Ok(Value::String(
                s.chars()
                    .map(|c| {
                        if c.is_ascii_lowercase() {
                            c.to_ascii_uppercase()
                        } else {
                            c
                        }
                    })
                    .collect(),
            ))
        }
        "lowercase" => {
            check_keys(args, &["input"])?;
            let s = req_str(args, "input")?;
            Ok(Value::String(
                s.chars()
                    .map(|c| {
                        if c.is_ascii_uppercase() {
                            c.to_ascii_lowercase()
                        } else {
                            c
                        }
                    })
                    .collect(),
            ))
        }
        "trim" => {
            check_keys(args, &["input"])?;
            let s = req_str(args, "input")?;
            let trimmed = s.trim_matches(|c: char| {
                matches!(c, ' ' | '\t' | '\n' | '\r' | '\u{000C}' | '\u{000B}')
            });
            Ok(Value::String(trimmed.to_string()))
        }
        "replace" => {
            check_keys(args, &["input", "search", "replace"])?;
            let input = req_str(args, "input")?;
            let search = req_str(args, "search")?;
            let replacement = req_str(args, "replace")?;
            if search.is_empty() {
                return Err(err(
                    "INVALID_ARG",
                    None,
                    "replace search string must not be empty",
                ));
            }
            Ok(Value::String(input.replace(search, replacement)))
        }
        "length" => {
            check_keys(args, &["input"])?;
            let s = req_str(args, "input")?;
            Ok(Value::from(s.chars().count() as i64))
        }
        "map" => {
            check_keys(args, &["input", "transform"])?;
            let arr = req_array(args, "input")?;
            let transform = req(args, "transform")?;
            let mut out = Vec::with_capacity(arr.len());
            for v in arr {
                out.push(apply_transform(v, transform)?);
            }
            Ok(Value::Array(out))
        }
        "filter" => {
            check_keys(args, &["input", "predicate"])?;
            let arr = req_array(args, "input")?;
            let predicate = req(args, "predicate")?;
            let mut out = Vec::new();
            for v in arr {
                if apply_predicate(v, predicate)? {
                    out.push(v.clone());
                }
            }
            Ok(Value::Array(out))
        }
        "reduce" => {
            check_keys(args, &["input", "op", "initial"])?;
            let arr = req_array(args, "input")?;
            let rop = req_str(args, "op")?;
            if !["add", "subtract", "multiply"].contains(&rop) {
                return Err(err(
                    "INVALID_ARG",
                    None,
                    format!("unsupported reduce op '{}'", rop),
                ));
            }
            let mut acc: i64 = if args.contains_key("initial") {
                req_i64(args, "initial")?
            } else if rop == "multiply" {
                1
            } else {
                0
            };
            for v in arr {
                let n = i64_of(v, "input element")?;
                acc = arith(rop, acc, n)?;
            }
            Ok(Value::from(acc))
        }
        "sort" => {
            check_keys(args, &["input"])?;
            let arr = req_array(args, "input")?;
            let mut out = arr.clone();
            out.sort_by(cmp_values);
            Ok(Value::Array(out))
        }
        "reverse" => {
            check_keys(args, &["input"])?;
            let arr = req_array(args, "input")?;
            let mut out = arr.clone();
            out.reverse();
            Ok(Value::Array(out))
        }
        "unique" => {
            check_keys(args, &["input"])?;
            let arr = req_array(args, "input")?;
            let mut out: Vec<Value> = Vec::new();
            let mut seen: HashSet<String> = HashSet::new();
            for v in arr {
                if seen.insert(canonical_key(v)) {
                    out.push(v.clone());
                }
            }
            Ok(Value::Array(out))
        }
        "slice" => {
            check_keys(args, &["input", "start", "end"])?;
            let arr = req_array(args, "input")?;
            let start = req_i64(args, "start")?;
            let end = if args.contains_key("end") {
                Some(req_i64(args, "end")?)
            } else {
                None
            };
            let len = arr.len() as i128;
            let norm = |idx: i64, len: i128| -> i128 {
                let i = idx as i128;
                if i < 0 {
                    let x = len + i;
                    if x < 0 {
                        0
                    } else {
                        x
                    }
                } else if i > len {
                    len
                } else {
                    i
                }
            };
            let s = norm(start, len);
            let e = match end {
                Some(x) => norm(x, len),
                None => len,
            };
            if s >= e {
                return Ok(Value::Array(Vec::new()));
            }
            let out: Vec<Value> = arr[s as usize..e as usize].to_vec();
            Ok(Value::Array(out))
        }
        "flatten" => {
            check_keys(args, &["input"])?;
            let arr = req_array(args, "input")?;
            let mut out = Vec::new();
            for v in arr {
                match v {
                    Value::Array(inner) => {
                        for x in inner {
                            out.push(x.clone());
                        }
                    }
                    other => out.push(other.clone()),
                }
            }
            Ok(Value::Array(out))
        }
        "get" => {
            check_keys(args, &["input", "key"])?;
            let obj = req_obj(args, "input")?;
            let key = req_str(args, "key")?;
            Ok(obj.get(key).cloned().unwrap_or(Value::Null))
        }
        "set" => {
            check_keys(args, &["input", "key", "value"])?;
            let obj = req_obj(args, "input")?;
            let key = req_str(args, "key")?;
            let value = req(args, "value")?.clone();
            let mut out = obj.clone();
            out.insert(key.to_string(), value);
            Ok(Value::Object(out))
        }
        "keys" => {
            check_keys(args, &["input"])?;
            let obj = req_obj(args, "input")?;
            let keys = sorted_keys(obj);
            Ok(Value::Array(
                keys.into_iter()
                    .map(|k| Value::String(k.clone()))
                    .collect(),
            ))
        }
        "values" => {
            check_keys(args, &["input"])?;
            let obj = req_obj(args, "input")?;
            let keys = sorted_keys(obj);
            Ok(Value::Array(
                keys.into_iter()
                    .map(|k| obj.get(k).cloned().unwrap_or(Value::Null))
                    .collect(),
            ))
        }
        "merge" => {
            check_keys(args, &["a", "b"])?;
            let a = req_obj(args, "a")?;
            let b = req_obj(args, "b")?;
            let mut out = a.clone();
            for (k, v) in b {
                out.insert(k.clone(), v.clone());
            }
            Ok(Value::Object(out))
        }
        "pick" => {
            check_keys(args, &["input", "keys"])?;
            let obj = req_obj(args, "input")?;
            let keys = req_array(args, "keys")?;
            let mut out = Map::new();
            for k in keys {
                let key = k
                    .as_str()
                    .ok_or_else(|| err("INVALID_ARG", None, "keys must be strings"))?;
                out.insert(
                    key.to_string(),
                    obj.get(key).cloned().unwrap_or(Value::Null),
                );
            }
            Ok(Value::Object(out))
        }
        "omit" => {
            check_keys(args, &["input", "keys"])?;
            let obj = req_obj(args, "input")?;
            let keys = req_array(args, "keys")?;
            let mut out = obj.clone();
            for k in keys {
                let key = k
                    .as_str()
                    .ok_or_else(|| err("INVALID_ARG", None, "keys must be strings"))?;
                out.remove(key);
            }
            Ok(Value::Object(out))
        }
        "constant" => {
            check_keys(args, &["value"])?;
            Ok(req(args, "value")?.clone())
        }
        "equals" => {
            check_keys(args, &["a", "b"])?;
            let a = req(args, "a")?;
            let b = req(args, "b")?;
            Ok(Value::Bool(eq_values(a, b)))
        }
        "compare" => {
            check_keys(args, &["a", "b", "op"])?;
            let a = req(args, "a")?;
            let b = req(args, "b")?;
            let cop = req_str(args, "op")?;
            let c = cmp_values(a, b);
            let result = match cop {
                "gt" => c == Ordering::Greater,
                "gte" => c != Ordering::Less,
                "lt" => c == Ordering::Less,
                "lte" => c != Ordering::Greater,
                _ => {
                    return Err(err(
                        "INVALID_ARG",
                        None,
                        format!("unsupported comparison op '{}'", cop),
                    ))
                }
            };
            Ok(Value::Bool(result))
        }
        "and" => {
            check_keys(args, &["a", "b"])?;
            let a = req_bool(args, "a")?;
            let b = req_bool(args, "b")?;
            Ok(Value::Bool(a && b))
        }
        "or" => {
            check_keys(args, &["a", "b"])?;
            let a = req_bool(args, "a")?;
            let b = req_bool(args, "b")?;
            Ok(Value::Bool(a || b))
        }
        "not" => {
            check_keys(args, &["a"])?;
            let a = req_bool(args, "a")?;
            Ok(Value::Bool(!a))
        }
        "type" => {
            check_keys(args, &["value"])?;
            let v = req(args, "value")?;
            Ok(Value::String(type_name(v).to_string()))
        }
        "fail_until" => {
            check_keys(args, &["failures", "value"])?;
            let failures = req_i64(args, "failures")?;
            if !(0..=10).contains(&failures) {
                return Err(err(
                    "INVALID_ARG",
                    None,
                    "failures must be an integer in 0..10",
                ));
            }
            let value = req(args, "value")?.clone();
            if (attempt as i64) <= failures {
                Err(err("TRANSIENT_FAILURE", None, "transient failure"))
            } else {
                Ok(value)
            }
        }
        _ => Err(invalid_wf(None, format!("unknown op '{}'", op))),
    }
}

// ---------------------------------------------------------------------------
// Reference resolution and condition evaluation
// ---------------------------------------------------------------------------

struct Exec {
    steps: Vec<StepDef>,
    results: HashMap<String, Value>,
    skipped: HashSet<String>,
    finished: HashSet<String>,
    order: Vec<String>,
    skipped_order: Vec<String>,
    error: Option<WfError>,
}

struct Execution {
    results: HashMap<String, Value>,
    order: Vec<String>,
    skipped_order: Vec<String>,
    error: Option<WfError>,
}

impl Exec {
    fn resolve_ref(&self, path: &str, step: &StepDef) -> Result<Value, WfError> {
        let parts: Vec<&str> = path.split('.').collect();
        let base = parts[0];
        if base.is_empty() {
            return Err(err("INVALID_REFERENCE", None, "empty reference"));
        }
        if !step.transitive.contains(base) {
            return Err(err(
                "INVALID_REFERENCE",
                None,
                format!("'{}' is not a dependency of '{}'", base, step.id),
            ));
        }
        if self.skipped.contains(base) {
            return Err(err(
                "SKIPPED_REFERENCE",
                None,
                format!("reference to skipped step '{}'", base),
            ));
        }
        let mut current = match self.results.get(base) {
            Some(v) => v.clone(),
            None => {
                return Err(err(
                    "INVALID_REFERENCE",
                    None,
                    format!("step '{}' has no result", base),
                ))
            }
        };
        for seg in &parts[1..] {
            current = match current {
                Value::Object(map) => match map.get(*seg) {
                    Some(v) => v.clone(),
                    None => {
                        return Err(err(
                            "INVALID_REFERENCE",
                            None,
                            format!("missing key '{}' in reference '{}'", seg, path),
                        ))
                    }
                },
                Value::Array(arr) => {
                    if seg.is_empty() || !seg.chars().all(|c| c.is_ascii_digit()) {
                        return Err(err(
                            "INVALID_REFERENCE",
                            None,
                            format!("invalid array index '{}' in reference '{}'", seg, path),
                        ));
                    }
                    let idx: usize = match seg.parse() {
                        Ok(i) => i,
                        Err(_) => {
                            return Err(err(
                                "INVALID_REFERENCE",
                                None,
                                format!("invalid array index '{}' in reference '{}'", seg, path),
                            ))
                        }
                    };
                    match arr.get(idx) {
                        Some(v) => v.clone(),
                        None => {
                            return Err(err(
                                "INVALID_REFERENCE",
                                None,
                                format!("array index {} out of bounds in reference '{}'", idx, path),
                            ))
                        }
                    }
                }
                _ => {
                    return Err(err(
                        "INVALID_REFERENCE",
                        None,
                        format!("cannot descend into scalar at '{}' in reference '{}'", seg, path),
                    ))
                }
            };
        }
        Ok(current)
    }

    fn resolve_value(&self, v: &Value, step: &StepDef) -> Result<Value, WfError> {
        match v {
            Value::Object(m) => {
                if m.len() == 1 {
                    if let Some(Value::String(path)) = m.get("$ref") {
                        return self.resolve_ref(path, step);
                    }
                }
                let mut out = Map::new();
                for (k, val) in m {
                    out.insert(k.clone(), self.resolve_value(val, step)?);
                }
                Ok(Value::Object(out))
            }
            Value::Array(a) => {
                let mut out = Vec::with_capacity(a.len());
                for x in a {
                    out.push(self.resolve_value(x, step)?);
                }
                Ok(Value::Array(out))
            }
            _ => Ok(v.clone()),
        }
    }

    fn eval_condition(&self, cond: &Value, step: &StepDef) -> Result<bool, WfError> {
        let obj = cond
            .as_object()
            .ok_or_else(|| err("INVALID_CONDITION", None, "condition must be an object"))?;
        let op = obj
            .get("op")
            .and_then(Value::as_str)
            .ok_or_else(|| err("INVALID_CONDITION", None, "condition op must be a string"))?;
        match op {
            "eq" | "ne" | "gt" | "gte" | "lt" | "lte" => {
                let left = self.resolve_value(
                    obj.get("left")
                        .ok_or_else(|| err("INVALID_CONDITION", None, "missing left"))?,
                    step,
                )?;
                let right = self.resolve_value(
                    obj.get("right")
                        .ok_or_else(|| err("INVALID_CONDITION", None, "missing right"))?,
                    step,
                )?;
                let result = match op {
                    "eq" => eq_values(&left, &right),
                    "ne" => !eq_values(&left, &right),
                    "gt" => cmp_values(&left, &right) == Ordering::Greater,
                    "gte" => cmp_values(&left, &right) != Ordering::Less,
                    "lt" => cmp_values(&left, &right) == Ordering::Less,
                    "lte" => cmp_values(&left, &right) != Ordering::Greater,
                    _ => unreachable!(),
                };
                Ok(result)
            }
            "all" | "any" => {
                let conditions = obj
                    .get("conditions")
                    .and_then(Value::as_array)
                    .ok_or_else(|| {
                        err("INVALID_CONDITION", None, "conditions must be an array")
                    })?;
                if op == "all" {
                    for c in conditions {
                        if !self.eval_condition(c, step)? {
                            return Ok(false);
                        }
                    }
                    Ok(true)
                } else {
                    for c in conditions {
                        if self.eval_condition(c, step)? {
                            return Ok(true);
                        }
                    }
                    Ok(false)
                }
            }
            "not" => {
                let c = obj.get("condition").ok_or_else(|| {
                    err("INVALID_CONDITION", None, "not requires condition")
                })?;
                Ok(!self.eval_condition(c, step)?)
            }
            _ => Err(err(
                "INVALID_CONDITION",
                None,
                format!("unknown condition op '{}'", op),
            )),
        }
    }

    fn run_step(&mut self, idx: usize) -> Result<(), WfError> {
        let step = self.steps[idx].clone();

        if let Some(cond) = &step.when {
            let run = self
                .eval_condition(cond, &step)
                .map_err(|e| at_step(e, &step.id))?;
            if !run {
                self.results.insert(step.id.clone(), Value::Null);
                self.skipped.insert(step.id.clone());
                self.finished.insert(step.id.clone());
                self.order.push(step.id.clone());
                self.skipped_order.push(step.id.clone());
                return Ok(());
            }
        }

        let mut resolved = Map::new();
        for (k, v) in &step.args {
            let r = self
                .resolve_value(v, &step)
                .map_err(|e| at_step(e, &step.id))?;
            resolved.insert(k.clone(), r);
        }

        let mut attempt: u32 = 1;
        loop {
            match eval_op(&step.op, &resolved, attempt) {
                Ok(v) => {
                    self.results.insert(step.id.clone(), v);
                    self.finished.insert(step.id.clone());
                    self.order.push(step.id.clone());
                    return Ok(());
                }
                Err(e) => {
                    let transient = e.code == "TRANSIENT_FAILURE";
                    if transient && attempt < step.max_attempts {
                        attempt += 1;
                        continue;
                    }
                    let final_code = if transient {
                        "STEP_FAILED".to_string()
                    } else {
                        e.code
                    };
                    return Err(WfError {
                        code: final_code,
                        step: Some(step.id.clone()),
                        message: e.message,
                    });
                }
            }
        }
    }
}

fn execute(steps: Vec<StepDef>) -> Execution {
    let mut ex = Exec {
        steps,
        results: HashMap::new(),
        skipped: HashSet::new(),
        finished: HashSet::new(),
        order: Vec::new(),
        skipped_order: Vec::new(),
        error: None,
    };
    let mut pending: Vec<usize> = (0..ex.steps.len()).collect();
    while !pending.is_empty() {
        let ready = pending.iter().position(|&i| {
            ex.steps[i]
                .depends_on
                .iter()
                .all(|d| ex.finished.contains(d))
        });
        match ready {
            None => {
                ex.error = Some(err(
                    "CYCLIC_DEPENDENCY",
                    None,
                    "dependency cycle during execution",
                ));
                break;
            }
            Some(pos) => {
                let idx = pending.remove(pos);
                if let Err(e) = ex.run_step(idx) {
                    ex.error = Some(e);
                    break;
                }
            }
        }
    }
    Execution {
        results: ex.results,
        order: ex.order,
        skipped_order: ex.skipped_order,
        error: ex.error,
    }
}

// ---------------------------------------------------------------------------
// Response / CLI
// ---------------------------------------------------------------------------

fn error_value(e: &WfError) -> Value {
    let mut m = Map::new();
    m.insert("code".to_string(), Value::String(e.code.clone()));
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

fn response(
    status: &str,
    results: HashMap<String, Value>,
    order: Vec<String>,
    skipped: Vec<String>,
    error: Option<&WfError>,
) -> Value {
    let mut results_map = Map::new();
    for (k, v) in results {
        results_map.insert(k, v);
    }
    let mut m = Map::new();
    m.insert("status".to_string(), Value::String(status.to_string()));
    m.insert("results".to_string(), Value::Object(results_map));
    m.insert(
        "execution_order".to_string(),
        Value::Array(order.into_iter().map(Value::String).collect()),
    );
    m.insert(
        "skipped".to_string(),
        Value::Array(skipped.into_iter().map(Value::String).collect()),
    );
    m.insert(
        "error".to_string(),
        match error {
            Some(e) => error_value(e),
            None => Value::Null,
        },
    );
    Value::Object(m)
}

fn emit(v: &Value) {
    let text = serde_json::to_string(v).unwrap_or_else(|_| "{}".to_string());
    let stdout = std::io::stdout();
    let mut lock = stdout.lock();
    let _ = writeln!(lock, "{}", text);
    let _ = lock.flush();
}

fn emit_error(e: &WfError) -> i32 {
    let v = response("failed", HashMap::new(), Vec::new(), Vec::new(), Some(e));
    emit(&v);
    match e.code.as_str() {
        "INVALID_WORKFLOW" | "UNKNOWN_DEPENDENCY" | "CYCLIC_DEPENDENCY" => 2,
        _ => 1,
    }
}

fn read_input(path: &str) -> Result<String, WfError> {
    if path == "-" {
        let mut buf = String::new();
        std::io::stdin()
            .read_to_string(&mut buf)
            .map_err(|e| invalid_wf(None, format!("failed to read stdin: {}", e)))?;
        Ok(buf)
    } else {
        std::fs::read_to_string(path)
            .map_err(|e| invalid_wf(None, format!("failed to read '{}': {}", path, e)))
    }
}

fn run_main() -> i32 {
    let argv: Vec<String> = std::env::args().collect();
    if argv.len() != 3 || argv[1] != "run" {
        let e = invalid_wf(None, "usage: workflow run <workflow.json>");
        return emit_error(&e);
    }
    let text = match read_input(&argv[2]) {
        Ok(t) => t,
        Err(e) => return emit_error(&e),
    };
    let doc: Value = match serde_json::from_str(&text) {
        Ok(v) => v,
        Err(e) => {
            let err = invalid_wf(None, format!("invalid JSON: {}", e));
            return emit_error(&err);
        }
    };
    let wf = match validate(&doc) {
        Ok(w) => w,
        Err(e) => return emit_error(&e),
    };
    let result = execute(wf.steps);
    match &result.error {
        Some(e) => {
            let v = response(
                "failed",
                result.results,
                result.order,
                result.skipped_order,
                Some(e),
            );
            emit(&v);
            1
        }
        None => {
            let v = response(
                "completed",
                result.results,
                result.order,
                result.skipped_order,
                None,
            );
            emit(&v);
            0
        }
    }
}

fn main() {
    let code = std::panic::catch_unwind(run_main).unwrap_or_else(|_| {
        let e = err("INTERNAL_ERROR", None, "internal error");
        let v = response("failed", HashMap::new(), Vec::new(), Vec::new(), Some(&e));
        emit(&v);
        3
    });
    std::process::exit(code);
}
