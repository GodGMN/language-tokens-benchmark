use serde_json::{Map, Number, Value};
use std::cmp::Ordering;
use std::collections::{HashMap, HashSet};
use std::env;
use std::io::{self, Read, Write};
use std::process;

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum ErrorCode {
    InvalidWorkflow,
    UnknownDependency,
    CyclicDependency,
    InvalidReference,
    SkippedReference,
    InvalidArg,
    InvalidCondition,
    ArithmeticOverflow,
    DivisionByZero,
    EmptyArray,
    TransientFailure,
    StepFailed,
}

impl ErrorCode {
    fn as_str(self) -> &'static str {
        match self {
            ErrorCode::InvalidWorkflow => "INVALID_WORKFLOW",
            ErrorCode::UnknownDependency => "UNKNOWN_DEPENDENCY",
            ErrorCode::CyclicDependency => "CYCLIC_DEPENDENCY",
            ErrorCode::InvalidReference => "INVALID_REFERENCE",
            ErrorCode::SkippedReference => "SKIPPED_REFERENCE",
            ErrorCode::InvalidArg => "INVALID_ARG",
            ErrorCode::InvalidCondition => "INVALID_CONDITION",
            ErrorCode::ArithmeticOverflow => "ARITHMETIC_OVERFLOW",
            ErrorCode::DivisionByZero => "DIVISION_BY_ZERO",
            ErrorCode::EmptyArray => "EMPTY_ARRAY",
            ErrorCode::TransientFailure => "TRANSIENT_FAILURE",
            ErrorCode::StepFailed => "STEP_FAILED",
        }
    }

    fn exit_code(self) -> i32 {
        match self {
            ErrorCode::InvalidWorkflow
            | ErrorCode::UnknownDependency
            | ErrorCode::CyclicDependency => 2,
            _ => 1,
        }
    }
}

#[derive(Clone, Debug)]
struct EngineError {
    code: ErrorCode,
    message: String,
}

impl EngineError {
    fn new(code: ErrorCode, message: impl Into<String>) -> Self {
        EngineError {
            code,
            message: message.into(),
        }
    }
}

fn err_arg(msg: impl Into<String>) -> EngineError {
    EngineError::new(ErrorCode::InvalidArg, msg)
}

fn err_ref(msg: impl Into<String>) -> EngineError {
    EngineError::new(ErrorCode::InvalidReference, msg)
}

#[derive(Clone, Debug)]
struct ValidationError {
    code: ErrorCode,
    step: Option<String>,
    message: String,
}

impl ValidationError {
    fn new(code: ErrorCode, step: Option<String>, message: impl Into<String>) -> Self {
        ValidationError {
            code,
            step,
            message: message.into(),
        }
    }
}

// ---------------------------------------------------------------------------
// Workflow model / structural validation
// ---------------------------------------------------------------------------

#[derive(Clone, Debug)]
struct Step {
    id: String,
    op: String,
    args: Value, // always an object
    depends_on: Vec<String>,
    when: Option<Value>,
    max_attempts: usize,
}

fn is_known_op(op: &str) -> bool {
    matches!(
        op,
        "add"
            | "subtract"
            | "multiply"
            | "divide"
            | "mod"
            | "sum"
            | "min"
            | "max"
            | "range"
            | "concat"
            | "split"
            | "join"
            | "uppercase"
            | "lowercase"
            | "trim"
            | "replace"
            | "length"
            | "map"
            | "filter"
            | "reduce"
            | "sort"
            | "reverse"
            | "unique"
            | "slice"
            | "flatten"
            | "get"
            | "set"
            | "keys"
            | "values"
            | "merge"
            | "pick"
            | "omit"
            | "constant"
            | "equals"
            | "compare"
            | "and"
            | "or"
            | "not"
            | "type"
            | "fail_until"
    )
}

fn valid_id(s: &str) -> bool {
    let mut chars = s.chars();
    match chars.next() {
        Some(c) if c.is_ascii_alphabetic() || c == '_' => {}
        _ => return false,
    }
    chars.all(|c| c.is_ascii_alphanumeric() || c == '_')
}

fn step_error(step: Option<&str>, msg: impl Into<String>) -> ValidationError {
    ValidationError::new(ErrorCode::InvalidWorkflow, step.map(|s| s.to_string()), msg)
}

fn validate_condition_shape(v: &Value) -> Result<(), String> {
    let obj = match v.as_object() {
        Some(o) => o,
        None => return Err("condition must be an object".into()),
    };
    let op = match obj.get("op").and_then(Value::as_str) {
        Some(s) => s,
        None => return Err("condition 'op' must be a string".into()),
    };
    match op {
        "eq" | "ne" | "gt" | "gte" | "lt" | "lte" => {
            for k in obj.keys() {
                if k != "op" && k != "left" && k != "right" {
                    return Err(format!("unknown condition field '{}'", k));
                }
            }
            if !obj.contains_key("left") {
                return Err("condition missing 'left'".into());
            }
            if !obj.contains_key("right") {
                return Err("condition missing 'right'".into());
            }
        }
        "all" | "any" => {
            for k in obj.keys() {
                if k != "op" && k != "conditions" {
                    return Err(format!("unknown condition field '{}'", k));
                }
            }
            let arr = match obj.get("conditions").and_then(Value::as_array) {
                Some(a) => a,
                None => return Err("compound condition requires a 'conditions' array".into()),
            };
            for c in arr {
                validate_condition_shape(c)?;
            }
        }
        "not" => {
            for k in obj.keys() {
                if k != "op" && k != "condition" {
                    return Err(format!("unknown condition field '{}'", k));
                }
            }
            match obj.get("condition") {
                Some(c) => validate_condition_shape(c)?,
                None => return Err("'not' condition requires a 'condition'".into()),
            }
        }
        other => return Err(format!("unknown condition operator '{}'", other)),
    }
    Ok(())
}

fn validate_workflow(doc: &Value) -> Result<Vec<Step>, ValidationError> {
    let top = match doc.as_object() {
        Some(o) => o,
        None => return Err(step_error(None, "workflow document must be a JSON object")),
    };

    for k in top.keys() {
        if k != "version" && k != "steps" {
            return Err(step_error(None, format!("unknown top-level field '{}'", k)));
        }
    }

    match top.get("version") {
        Some(Value::Number(n)) if n.is_i64() && n.as_i64() == Some(1) => {}
        Some(_) => {
            return Err(step_error(
                None,
                "workflow version must be the integer literal 1",
            ))
        }
        None => return Err(step_error(None, "missing workflow 'version'")),
    }

    let step_values = match top.get("steps") {
        Some(Value::Array(a)) => a,
        Some(_) => return Err(step_error(None, "'steps' must be an array")),
        None => return Err(step_error(None, "missing workflow 'steps'")),
    };

    let mut steps: Vec<Step> = Vec::new();
    let mut ids: HashMap<String, usize> = HashMap::new();

    for sv in step_values {
        let obj = match sv.as_object() {
            Some(o) => o,
            None => return Err(step_error(None, "each step must be an object")),
        };

        let id_for_error: Option<String> = obj
            .get("id")
            .and_then(Value::as_str)
            .map(|s| s.to_string());

        for k in obj.keys() {
            if !matches!(
                k.as_str(),
                "id" | "op" | "args" | "depends_on" | "when" | "retry"
            ) {
                return Err(step_error(
                    id_for_error.as_deref(),
                    format!("unknown step field '{}'", k),
                ));
            }
        }

        let id = match obj.get("id").and_then(Value::as_str) {
            Some(s) => s.to_string(),
            None => return Err(step_error(None, "step 'id' must be a string")),
        };
        if !valid_id(&id) {
            return Err(step_error(
                Some(&id),
                format!("invalid step id '{}'", id),
            ));
        }
        if ids.contains_key(&id) {
            return Err(step_error(Some(&id), format!("duplicate step id '{}'", id)));
        }

        let op = match obj.get("op").and_then(Value::as_str) {
            Some(s) => s.to_string(),
            None => return Err(step_error(Some(&id), "step 'op' must be a string")),
        };
        if !is_known_op(&op) {
            return Err(step_error(
                Some(&id),
                format!("unknown operation '{}'", op),
            ));
        }

        let args = match obj.get("args") {
            None => Value::Object(Map::new()),
            Some(v) if v.is_object() => v.clone(),
            Some(_) => return Err(step_error(Some(&id), "step 'args' must be an object")),
        };

        let depends_on = match obj.get("depends_on") {
            None => Vec::new(),
            Some(Value::Array(a)) => {
                let mut deps = Vec::new();
                for d in a {
                    match d.as_str() {
                        Some(s) => deps.push(s.to_string()),
                        None => {
                            return Err(step_error(
                                Some(&id),
                                "'depends_on' must contain only step ids",
                            ))
                        }
                    }
                }
                deps
            }
            Some(_) => {
                return Err(step_error(
                    Some(&id),
                    "step 'depends_on' must be an array",
                ))
            }
        };

        let when = match obj.get("when") {
            None => None,
            Some(v) => {
                validate_condition_shape(v)
                    .map_err(|m| step_error(Some(&id), format!("invalid 'when': {}", m)))?;
                Some(v.clone())
            }
        };

        let max_attempts = match obj.get("retry") {
            None => 1usize,
            Some(Value::Object(r)) => {
                for k in r.keys() {
                    if k != "max_attempts" {
                        return Err(step_error(
                            Some(&id),
                            format!("unknown retry field '{}'", k),
                        ));
                    }
                }
                match r.get("max_attempts") {
                    None => 1usize,
                    Some(v) => match int_literal(v) {
                        Ok(n) if (1..=5).contains(&n) => n as usize,
                        _ => {
                            return Err(step_error(
                                Some(&id),
                                "retry.max_attempts must be an integer from 1 to 5",
                            ))
                        }
                    },
                }
            }
            Some(_) => return Err(step_error(Some(&id), "step 'retry' must be an object")),
        };

        let idx = steps.len();
        ids.insert(id.clone(), idx);
        steps.push(Step {
            id,
            op,
            args,
            depends_on,
            when,
            max_attempts,
        });
    }

    // Dependencies must name existing steps.
    for s in &steps {
        for d in &s.depends_on {
            if !ids.contains_key(d) {
                return Err(ValidationError::new(
                    ErrorCode::UnknownDependency,
                    Some(s.id.clone()),
                    format!("step '{}' depends on unknown step '{}'", s.id, d),
                ));
            }
        }
    }

    // Cycle detection (iterative-ish recursive DFS with colors).
    let n = steps.len();
    let mut color = vec![0u8; n];
    for start in 0..n {
        if color[start] != 0 {
            continue;
        }
        // Explicit stack: (node, next dependency index)
        let mut stack: Vec<(usize, usize)> = vec![(start, 0)];
        color[start] = 1;
        while let Some((node, dep_i)) = stack.pop() {
            if dep_i < steps[node].depends_on.len() {
                stack.push((node, dep_i + 1));
                let dep_id = &steps[node].depends_on[dep_i];
                let dep = ids[dep_id];
                if color[dep] == 1 {
                    return Err(ValidationError::new(
                        ErrorCode::CyclicDependency,
                        None,
                        "dependency cycle detected",
                    ));
                }
                if color[dep] == 0 {
                    color[dep] = 1;
                    stack.push((dep, 0));
                }
            } else {
                color[node] = 2;
            }
        }
    }

    Ok(steps)
}

// ---------------------------------------------------------------------------
// Generic value helpers
// ---------------------------------------------------------------------------

fn int_literal(v: &Value) -> Result<i64, EngineError> {
    match v {
        Value::Number(n) if n.is_i64() => Ok(n.as_i64().unwrap()),
        _ => Err(err_arg(
            "numeric parameter must be a JSON integer literal that fits in i64",
        )),
    }
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

fn cmp_numbers(a: &Number, b: &Number) -> Ordering {
    if let (Some(x), Some(y)) = (a.as_i64(), b.as_i64()) {
        return x.cmp(&y);
    }
    if let (Some(x), Some(y)) = (a.as_u64(), b.as_u64()) {
        return x.cmp(&y);
    }
    let fa = a.as_f64().unwrap_or(0.0);
    let fb = b.as_f64().unwrap_or(0.0);
    fa.partial_cmp(&fb).unwrap_or(Ordering::Equal)
}

fn sorted_keys(obj: &Map<String, Value>) -> Vec<&String> {
    let mut keys: Vec<&String> = obj.keys().collect();
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
            let common = x.len().min(y.len());
            for i in 0..common {
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
            let common = kx.len().min(ky.len());
            for i in 0..common {
                let c = kx[i].as_bytes().cmp(ky[i].as_bytes());
                if c != Ordering::Equal {
                    return c;
                }
            }
            let c = kx.len().cmp(&ky.len());
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

fn equals_values(a: &Value, b: &Value) -> bool {
    if type_rank(a) != type_rank(b) {
        return false;
    }
    match (a, b) {
        (Value::Null, Value::Null) => true,
        (Value::Bool(x), Value::Bool(y)) => x == y,
        (Value::Number(x), Value::Number(y)) => cmp_numbers(x, y) == Ordering::Equal,
        (Value::String(x), Value::String(y)) => x == y,
        (Value::Array(x), Value::Array(y)) => {
            x.len() == y.len() && x.iter().zip(y.iter()).all(|(u, v)| equals_values(u, v))
        }
        (Value::Object(x), Value::Object(y)) => {
            if x.len() != y.len() {
                return false;
            }
            for (k, v) in x {
                match y.get(k) {
                    Some(w) if equals_values(v, w) => {}
                    _ => return false,
                }
            }
            true
        }
        _ => false,
    }
}

fn is_truthy(v: &Value) -> bool {
    match v {
        Value::Null => false,
        Value::Bool(b) => *b,
        Value::Number(n) => !matches!(n.as_f64(), Some(f) if f == 0.0),
        Value::String(s) => !s.is_empty(),
        Value::Array(a) => !a.is_empty(),
        Value::Object(o) => !o.is_empty(),
    }
}

fn checked_binop(op: &str, a: i64, b: i64) -> Result<i64, EngineError> {
    let overflow = || EngineError::new(ErrorCode::ArithmeticOverflow, "i64 overflow");
    match op {
        "add" => a.checked_add(b).ok_or_else(overflow),
        "subtract" => a.checked_sub(b).ok_or_else(overflow),
        "multiply" => a.checked_mul(b).ok_or_else(overflow),
        "divide" => {
            if b == 0 {
                Err(EngineError::new(
                    ErrorCode::DivisionByZero,
                    "division by zero",
                ))
            } else {
                a.checked_div(b).ok_or_else(overflow)
            }
        }
        "mod" => {
            if b == 0 {
                Err(EngineError::new(
                    ErrorCode::DivisionByZero,
                    "modulo by zero",
                ))
            } else {
                a.checked_rem(b).ok_or_else(overflow)
            }
        }
        _ => Err(err_arg(format!("unsupported arithmetic operator '{}'", op))),
    }
}

fn relation_holds(op: &str, a: &Value, b: &Value) -> Result<bool, EngineError> {
    match op {
        "eq" => Ok(equals_values(a, b)),
        "ne" => Ok(!equals_values(a, b)),
        "gt" => Ok(cmp_values(a, b) == Ordering::Greater),
        "gte" => Ok(cmp_values(a, b) != Ordering::Less),
        "lt" => Ok(cmp_values(a, b) == Ordering::Less),
        "lte" => Ok(cmp_values(a, b) != Ordering::Greater),
        _ => Err(err_arg(format!("unknown comparison operator '{}'", op))),
    }
}

// ---------------------------------------------------------------------------
// Operation helpers
// ---------------------------------------------------------------------------

fn check_keys(args: &Map<String, Value>, allowed: &[&str]) -> Result<(), EngineError> {
    for k in args.keys() {
        if !allowed.contains(&k.as_str()) {
            return Err(err_arg(format!("unknown argument '{}'", k)));
        }
    }
    Ok(())
}

fn need<'a>(args: &'a Map<String, Value>, key: &str) -> Result<&'a Value, EngineError> {
    args.get(key)
        .ok_or_else(|| err_arg(format!("missing argument '{}'", key)))
}

fn arg_i64(args: &Map<String, Value>, key: &str) -> Result<i64, EngineError> {
    int_literal(need(args, key)?)
}

fn arg_string<'a>(args: &'a Map<String, Value>, key: &str) -> Result<&'a str, EngineError> {
    need(args, key)?
        .as_str()
        .ok_or_else(|| err_arg(format!("argument '{}' must be a string", key)))
}

fn arg_bool(args: &Map<String, Value>, key: &str) -> Result<bool, EngineError> {
    need(args, key)?
        .as_bool()
        .ok_or_else(|| err_arg(format!("argument '{}' must be a boolean", key)))
}

fn arg_array<'a>(
    args: &'a Map<String, Value>,
    key: &str,
) -> Result<&'a Vec<Value>, EngineError> {
    need(args, key)?
        .as_array()
        .ok_or_else(|| err_arg(format!("argument '{}' must be an array", key)))
}

fn arg_object<'a>(
    args: &'a Map<String, Value>,
    key: &str,
) -> Result<&'a Map<String, Value>, EngineError> {
    need(args, key)?
        .as_object()
        .ok_or_else(|| err_arg(format!("argument '{}' must be an object", key)))
}

fn is_self_marker(v: &Value) -> bool {
    match v.as_object() {
        Some(o) if o.len() == 1 => matches!(o.get("$self"), Some(Value::Bool(true))),
        _ => false,
    }
}

fn apply_transform(t: &Value, elem: &Value) -> Result<Value, EngineError> {
    let obj = t
        .as_object()
        .ok_or_else(|| err_arg("transform must be an object"))?;
    let typ = obj
        .get("type")
        .and_then(Value::as_str)
        .ok_or_else(|| err_arg("transform 'type' must be a string"))?;

    match typ {
        "constant" => {
            check_keys(obj, &["type", "value"])?;
            let val = obj
                .get("value")
                .ok_or_else(|| err_arg("constant transform requires 'value'"))?;
            Ok(val.clone())
        }
        "add" | "subtract" | "multiply" | "divide" | "mod" => {
            check_keys(obj, &["type", "value"])?;
            let rhs = int_literal(
                obj.get("value")
                    .ok_or_else(|| err_arg("transform requires 'value'"))?,
            )?;
            let lhs = int_literal(elem)?;
            checked_binop(typ, lhs, rhs).map(Value::from)
        }
        "negate" => {
            check_keys(obj, &["type"])?;
            let e = int_literal(elem)?;
            e.checked_neg()
                .map(Value::from)
                .ok_or_else(|| EngineError::new(ErrorCode::ArithmeticOverflow, "negate overflow"))
        }
        "abs" => {
            check_keys(obj, &["type"])?;
            let e = int_literal(elem)?;
            e.checked_abs()
                .map(Value::from)
                .ok_or_else(|| EngineError::new(ErrorCode::ArithmeticOverflow, "abs overflow"))
        }
        "stringify" => {
            check_keys(obj, &["type"])?;
            let s = serde_json::to_string(elem)
                .map_err(|e| err_arg(format!("cannot stringify value: {}", e)))?;
            Ok(Value::String(s))
        }
        "type" => {
            check_keys(obj, &["type"])?;
            Ok(Value::String(type_name(elem).to_string()))
        }
        "op" => {
            check_keys(obj, &["type", "op", "args"])?;
            let op = obj
                .get("op")
                .and_then(Value::as_str)
                .ok_or_else(|| err_arg("transform 'op' must be a string"))?;
            if !matches!(op, "add" | "subtract" | "multiply" | "divide" | "mod") {
                return Err(err_arg(format!(
                    "unsupported transform operation '{}'",
                    op
                )));
            }
            let targs = obj
                .get("args")
                .and_then(Value::as_object)
                .ok_or_else(|| err_arg("transform 'args' must be an object"))?;
            check_keys(targs, &["a", "b"])?;
            let a_val = targs
                .get("a")
                .ok_or_else(|| err_arg("transform args missing 'a'"))?;
            let b_val = targs
                .get("b")
                .ok_or_else(|| err_arg("transform args missing 'b'"))?;
            let a = if is_self_marker(a_val) {
                int_literal(elem)?
            } else {
                int_literal(a_val)?
            };
            let b = if is_self_marker(b_val) {
                int_literal(elem)?
            } else {
                int_literal(b_val)?
            };
            checked_binop(op, a, b).map(Value::from)
        }
        other => Err(err_arg(format!("unsupported transform type '{}'", other))),
    }
}

fn is_json_type_name(s: &str) -> bool {
    matches!(s, "null" | "boolean" | "number" | "string" | "array" | "object")
}

fn is_predicate_kind(s: &str) -> bool {
    matches!(
        s,
        "even"
            | "odd"
            | "gt"
            | "gte"
            | "lt"
            | "lte"
            | "eq"
            | "ne"
            | "truthy"
            | "falsy"
            | "type_is"
            | "string_contains"
    )
}

fn apply_predicate(p: &Value, elem: &Value) -> Result<bool, EngineError> {
    // Accept the string shorthand used by some examples (e.g. "even").
    if let Some(s) = p.as_str() {
        return match s {
            "even" | "odd" => {
                let e = int_literal(elem)?;
                Ok(if s == "even" { e % 2 == 0 } else { e % 2 != 0 })
            }
            "truthy" => Ok(is_truthy(elem)),
            "falsy" => Ok(!is_truthy(elem)),
            _ => Err(err_arg(format!("unsupported predicate '{}'", s))),
        };
    }
    let obj = p
        .as_object()
        .ok_or_else(|| err_arg("predicate must be an object"))?;
    let raw_type = obj.get("type").and_then(Value::as_str);
    let raw_op = obj.get("op").and_then(Value::as_str);
    let raw_kind = obj.get("kind").and_then(Value::as_str);
    let raw_pred = obj.get("predicate").and_then(Value::as_str);
    let typ = match raw_type {
        Some(t) if is_predicate_kind(t) => t,
        Some(t) => raw_op.or(raw_kind).or(raw_pred).unwrap_or(t),
        None => raw_op
            .or(raw_kind)
            .or(raw_pred)
            .ok_or_else(|| err_arg("predicate 'type' must be a string"))?,
    };

    match typ {
        "even" | "odd" => {
            check_keys(obj, &["type", "op"])?;
            let e = int_literal(elem)?;
            Ok(if typ == "even" {
                e % 2 == 0
            } else {
                e % 2 != 0
            })
        }
        "gt" | "gte" | "lt" | "lte" | "eq" | "ne" => {
            check_keys(obj, &["type", "op", "value"])?;
            let val = int_literal(
                obj.get("value")
                    .ok_or_else(|| err_arg("comparison predicate requires 'value'"))?,
            )?;
            relation_holds(typ, elem, &Value::from(val))
        }
        "truthy" | "falsy" => {
            check_keys(obj, &["type", "op"])?;
            let truthy = is_truthy(elem);
            Ok(if typ == "truthy" { truthy } else { !truthy })
        }
        "type_is" => {
            // The spec's spelling is ambiguous (it shows two 'type' keys), so
            // accept the common field names for the target type.
            let mut target: Option<&str> = None;
            for k in [
                "value",
                "type_name",
                "target_type",
                "expected_type",
                "json_type",
                "name",
                "of",
                "expected",
                "target",
                "type_value",
            ] {
                if let Some(v) = obj.get(k) {
                    target = v.as_str();
                    if target.is_some() {
                        break;
                    }
                }
            }
            // Also support `{"op":"type_is","type":"number"}` and the
            // duplicate-key form `{"type":"type_is","type":"number"}`
            // (whose parsed representation keeps only the last key).
            if target.is_none() {
                if let Some(t) = raw_type {
                    if is_json_type_name(t) {
                        target = Some(t);
                    }
                }
            }
            if target.is_none() {
                if let Some(k) = raw_kind {
                    if is_json_type_name(k) {
                        target = Some(k);
                    }
                }
            }
            let target = target.ok_or_else(|| {
                err_arg("type_is predicate requires the target JSON type")
            })?;
            if !is_json_type_name(target) {
                return Err(err_arg(format!("unknown JSON type '{}'", target)));
            }
            Ok(type_name(elem) == target)
        }
        other if is_json_type_name(other) && obj.len() == 1 => {
            // Salvage `{"type":"number"}` (the result of duplicate keys in the
            // spec's type_is example).
            Ok(type_name(elem) == other)
        }
        "string_contains" => {
            check_keys(obj, &["type", "op", "value"])?;
            let needle = obj
                .get("value")
                .and_then(Value::as_str)
                .ok_or_else(|| err_arg("string_contains requires a string 'value'"))?;
            let hay = elem
                .as_str()
                .ok_or_else(|| err_arg("string_contains requires a string element"))?;
            Ok(hay.contains(needle))
        }
        other => Err(err_arg(format!("unsupported predicate type '{}'", other))),
    }
}

fn normalize_index(idx: i64, len: usize) -> usize {
    let len_i = len as i128;
    let mut x = idx as i128;
    if x < 0 {
        x += len_i;
    }
    if x < 0 {
        0
    } else if x > len_i {
        len
    } else {
        x as usize
    }
}

fn run_op(
    op: &str,
    args: &Value,
    fail_attempts: &mut HashMap<String, i64>,
    step_id: &str,
) -> Result<Value, EngineError> {
    let args = args
        .as_object()
        .ok_or_else(|| err_arg("resolved args must be an object"))?;

    match op {
        // ---------------- math ----------------
        "add" | "subtract" | "multiply" | "divide" | "mod" => {
            check_keys(args, &["a", "b"])?;
            let a = arg_i64(args, "a")?;
            let b = arg_i64(args, "b")?;
            checked_binop(op, a, b).map(Value::from)
        }
        "sum" => {
            check_keys(args, &["input"])?;
            let input = arg_array(args, "input")?;
            let mut total: i64 = 0;
            for v in input {
                let n = int_literal(v)?;
                total = total.checked_add(n).ok_or_else(|| {
                    EngineError::new(ErrorCode::ArithmeticOverflow, "sum overflow")
                })?;
            }
            Ok(Value::from(total))
        }
        "min" => {
            check_keys(args, &["input"])?;
            let input = arg_array(args, "input")?;
            let mut it = input.iter();
            let first = it
                .next()
                .ok_or_else(|| EngineError::new(ErrorCode::EmptyArray, "min of empty array"))?;
            let mut best = int_literal(first)?;
            for v in it {
                let n = int_literal(v)?;
                if n < best {
                    best = n;
                }
            }
            Ok(Value::from(best))
        }
        "max" => {
            check_keys(args, &["input"])?;
            let input = arg_array(args, "input")?;
            let mut it = input.iter();
            let first = it
                .next()
                .ok_or_else(|| EngineError::new(ErrorCode::EmptyArray, "max of empty array"))?;
            let mut best = int_literal(first)?;
            for v in it {
                let n = int_literal(v)?;
                if n > best {
                    best = n;
                }
            }
            Ok(Value::from(best))
        }
        "range" => {
            check_keys(args, &["start", "end"])?;
            let start = arg_i64(args, "start")?;
            let end = arg_i64(args, "end")?;
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

        // ---------------- strings ----------------
        "concat" => {
            check_keys(args, &["a", "b"])?;
            let a = arg_string(args, "a")?;
            let b = arg_string(args, "b")?;
            Ok(Value::String(format!("{}{}", a, b)))
        }
        "split" => {
            check_keys(args, &["input", "sep"])?;
            let input = arg_string(args, "input")?;
            let sep = arg_string(args, "sep")?;
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
            let input = arg_array(args, "input")?;
            let sep = arg_string(args, "sep")?;
            let mut parts: Vec<&str> = Vec::new();
            for v in input {
                let s = v
                    .as_str()
                    .ok_or_else(|| err_arg("join input must be an array of strings"))?;
                parts.push(s);
            }
            Ok(Value::String(parts.join(sep)))
        }
        "uppercase" => {
            check_keys(args, &["input"])?;
            let input = arg_string(args, "input")?;
            let out: String = input
                .chars()
                .map(|c| c.to_ascii_uppercase())
                .collect();
            Ok(Value::String(out))
        }
        "lowercase" => {
            check_keys(args, &["input"])?;
            let input = arg_string(args, "input")?;
            let out: String = input
                .chars()
                .map(|c| c.to_ascii_lowercase())
                .collect();
            Ok(Value::String(out))
        }
        "trim" => {
            check_keys(args, &["input"])?;
            let input = arg_string(args, "input")?;
            let out = input.trim_matches(|c: char| {
                matches!(c, ' ' | '\t' | '\n' | '\r' | '\u{000c}' | '\u{000b}')
            });
            Ok(Value::String(out.to_string()))
        }
        "replace" => {
            check_keys(args, &["input", "search", "replace"])?;
            let input = arg_string(args, "input")?;
            let search = arg_string(args, "search")?;
            let replacement = arg_string(args, "replace")?;
            if search.is_empty() {
                return Err(err_arg("replace search string must not be empty"));
            }
            Ok(Value::String(input.replace(search, replacement)))
        }
        "length" => {
            check_keys(args, &["input"])?;
            let input = arg_string(args, "input")?;
            let n = input.chars().count() as i64;
            Ok(Value::from(n))
        }

        // ---------------- arrays ----------------
        "map" => {
            check_keys(args, &["input", "transform"])?;
            let input = arg_array(args, "input")?;
            let transform = need(args, "transform")?;
            let mut out = Vec::with_capacity(input.len());
            for elem in input {
                out.push(apply_transform(transform, elem)?);
            }
            Ok(Value::Array(out))
        }
        "filter" => {
            check_keys(args, &["input", "predicate"])?;
            let input = arg_array(args, "input")?;
            let predicate = need(args, "predicate")?;
            let mut out = Vec::new();
            for elem in input {
                if apply_predicate(predicate, elem)? {
                    out.push(elem.clone());
                }
            }
            Ok(Value::Array(out))
        }
        "reduce" => {
            check_keys(args, &["input", "op", "initial"])?;
            let input = arg_array(args, "input")?;
            let rop = arg_string(args, "op")?;
            if !matches!(rop, "add" | "subtract" | "multiply") {
                return Err(err_arg(format!(
                    "reduce op must be add, subtract or multiply (got '{}')",
                    rop
                )));
            }
            let mut acc = match args.get("initial") {
                Some(v) => int_literal(v)?,
                None => {
                    if rop == "multiply" {
                        1
                    } else {
                        0
                    }
                }
            };
            for v in input {
                let n = int_literal(v)?;
                acc = checked_binop(rop, acc, n)?;
            }
            Ok(Value::from(acc))
        }
        "sort" => {
            check_keys(args, &["input"])?;
            let input = arg_array(args, "input")?;
            let mut out = input.clone();
            out.sort_by(cmp_values);
            Ok(Value::Array(out))
        }
        "reverse" => {
            check_keys(args, &["input"])?;
            let input = arg_array(args, "input")?;
            let mut out = input.clone();
            out.reverse();
            Ok(Value::Array(out))
        }
        "unique" => {
            check_keys(args, &["input"])?;
            let input = arg_array(args, "input")?;
            let mut out: Vec<Value> = Vec::new();
            for v in input {
                if !out.iter().any(|seen| equals_values(seen, v)) {
                    out.push(v.clone());
                }
            }
            Ok(Value::Array(out))
        }
        "slice" => {
            check_keys(args, &["input", "start", "end"])?;
            let input = arg_array(args, "input")?;
            let start = arg_i64(args, "start")?;
            let len = input.len();
            let s = normalize_index(start, len);
            let e = match args.get("end") {
                Some(v) => normalize_index(int_literal(v)?, len),
                None => len,
            };
            if s >= e {
                Ok(Value::Array(Vec::new()))
            } else {
                Ok(Value::Array(input[s..e].to_vec()))
            }
        }
        "flatten" => {
            check_keys(args, &["input"])?;
            let input = arg_array(args, "input")?;
            let mut out = Vec::new();
            for v in input {
                match v {
                    Value::Array(a) => out.extend(a.iter().cloned()),
                    _ => out.push(v.clone()),
                }
            }
            Ok(Value::Array(out))
        }

        // ---------------- objects ----------------
        "get" => {
            check_keys(args, &["input", "key"])?;
            let input = arg_object(args, "input")?;
            let key = arg_string(args, "key")?;
            Ok(input.get(key).cloned().unwrap_or(Value::Null))
        }
        "set" => {
            check_keys(args, &["input", "key", "value"])?;
            let input = arg_object(args, "input")?;
            let key = arg_string(args, "key")?;
            let value = need(args, "value")?;
            let mut out = input.clone();
            out.insert(key.to_string(), value.clone());
            Ok(Value::Object(out))
        }
        "keys" => {
            check_keys(args, &["input"])?;
            let input = arg_object(args, "input")?;
            let keys = sorted_keys(input);
            Ok(Value::Array(
                keys.into_iter()
                    .map(|k| Value::String(k.clone()))
                    .collect(),
            ))
        }
        "values" => {
            check_keys(args, &["input"])?;
            let input = arg_object(args, "input")?;
            let keys = sorted_keys(input);
            Ok(Value::Array(
                keys.into_iter()
                    .map(|k| input[k].clone())
                    .collect(),
            ))
        }
        "merge" => {
            check_keys(args, &["a", "b"])?;
            let a = arg_object(args, "a")?;
            let b = arg_object(args, "b")?;
            let mut out = a.clone();
            for (k, v) in b {
                out.insert(k.clone(), v.clone());
            }
            Ok(Value::Object(out))
        }
        "pick" => {
            check_keys(args, &["input", "keys"])?;
            let input = arg_object(args, "input")?;
            let keys = arg_array(args, "keys")?;
            let mut out = Map::new();
            for k in keys {
                let key = k
                    .as_str()
                    .ok_or_else(|| err_arg("pick keys must be strings"))?;
                let value = input.get(key).cloned().unwrap_or(Value::Null);
                out.insert(key.to_string(), value);
            }
            Ok(Value::Object(out))
        }
        "omit" => {
            check_keys(args, &["input", "keys"])?;
            let input = arg_object(args, "input")?;
            let keys = arg_array(args, "keys")?;
            let mut out = input.clone();
            for k in keys {
                let key = k
                    .as_str()
                    .ok_or_else(|| err_arg("omit keys must be strings"))?;
                out.remove(key);
            }
            Ok(Value::Object(out))
        }

        // ---------------- utilities ----------------
        "constant" => {
            check_keys(args, &["value"])?;
            Ok(need(args, "value")?.clone())
        }
        "equals" => {
            check_keys(args, &["a", "b"])?;
            let a = need(args, "a")?;
            let b = need(args, "b")?;
            Ok(Value::Bool(equals_values(a, b)))
        }
        "compare" => {
            check_keys(args, &["a", "b", "op"])?;
            let a = need(args, "a")?;
            let b = need(args, "b")?;
            let cop = arg_string(args, "op")?;
            if !matches!(cop, "gt" | "gte" | "lt" | "lte") {
                return Err(err_arg(
                    "compare op must be one of gt, gte, lt, lte",
                ));
            }
            relation_holds(cop, a, b).map(Value::Bool)
        }
        "and" => {
            check_keys(args, &["a", "b"])?;
            let a = arg_bool(args, "a")?;
            let b = arg_bool(args, "b")?;
            Ok(Value::Bool(a && b))
        }
        "or" => {
            check_keys(args, &["a", "b"])?;
            let a = arg_bool(args, "a")?;
            let b = arg_bool(args, "b")?;
            Ok(Value::Bool(a || b))
        }
        "not" => {
            check_keys(args, &["a"])?;
            let a = arg_bool(args, "a")?;
            Ok(Value::Bool(!a))
        }
        "type" => {
            check_keys(args, &["value"])?;
            let value = need(args, "value")?;
            Ok(Value::String(type_name(value).to_string()))
        }

        // ---------------- test operation ----------------
        "fail_until" => {
            check_keys(args, &["failures", "value"])?;
            let failures = arg_i64(args, "failures")?;
            if !(0..=10).contains(&failures) {
                return Err(err_arg("fail_until 'failures' must be between 0 and 10"));
            }
            let value = need(args, "value")?.clone();
            let counter = fail_attempts.entry(step_id.to_string()).or_insert(0);
            *counter += 1;
            if *counter <= failures {
                Err(EngineError::new(
                    ErrorCode::TransientFailure,
                    "transient failure",
                ))
            } else {
                Ok(value)
            }
        }

        other => Err(err_arg(format!("unknown operation '{}'", other))),
    }
}

// ---------------------------------------------------------------------------
// Reference / condition evaluation
// ---------------------------------------------------------------------------

struct ExecCtx<'a> {
    steps: &'a [Step],
    by_id: &'a HashMap<String, usize>,
    closures: &'a [HashSet<usize>],
    results: &'a Map<String, Value>,
    skipped: &'a HashSet<String>,
    current: usize,
}

fn as_reference(v: &Value) -> Option<&str> {
    if let Value::Object(o) = v {
        if o.len() == 1 {
            if let Some(Value::String(s)) = o.get("$ref") {
                return Some(s);
            }
        }
    }
    None
}

fn resolve_reference(refstr: &str, ctx: &ExecCtx) -> Result<Value, EngineError> {
    let mut parts = refstr.split('.');
    let id = parts.next().unwrap_or("");
    if id.is_empty() {
        return Err(err_ref("empty reference"));
    }
    let target = match ctx.by_id.get(id) {
        Some(i) => *i,
        None => return Err(err_ref(format!("reference to unknown step '{}'", id))),
    };
    if target == ctx.current || !ctx.closures[ctx.current].contains(&target) {
        return Err(err_ref(format!(
            "step '{}' may not reference '{}': not a dependency",
            ctx.steps[ctx.current].id, id
        )));
    }
    if ctx.skipped.contains(id) {
        return Err(EngineError::new(
            ErrorCode::SkippedReference,
            format!("reference to skipped step '{}'", id),
        ));
    }
    let mut value = match ctx.results.get(id) {
        Some(v) => v.clone(),
        None => {
            return Err(err_ref(format!(
                "reference to step '{}' which has not finished",
                id
            )))
        }
    };

    for seg in parts {
        if seg.is_empty() {
            return Err(err_ref(format!("invalid empty path segment in '{}'", refstr)));
        }
        value = match &value {
            Value::Object(o) => match o.get(seg) {
                Some(v) => v.clone(),
                None => {
                    return Err(err_ref(format!(
                        "path segment '{}' not found in reference '{}'",
                        seg, refstr
                    )))
                }
            },
            Value::Array(a) => {
                if !seg.bytes().all(|b| b.is_ascii_digit()) {
                    return Err(err_ref(format!(
                        "invalid array index '{}' in reference '{}'",
                        seg, refstr
                    )));
                }
                let idx: usize = seg.parse().map_err(|_| {
                    err_ref(format!("array index '{}' is out of range", seg))
                })?;
                match a.get(idx) {
                    Some(v) => v.clone(),
                    None => {
                        return Err(err_ref(format!(
                            "array index '{}' out of bounds in reference '{}'",
                            seg, refstr
                        )))
                    }
                }
            }
            _ => {
                return Err(err_ref(format!(
                    "cannot traverse '{}' in reference '{}'",
                    seg, refstr
                )))
            }
        };
    }

    Ok(value)
}

fn resolve_value(v: &Value, ctx: &ExecCtx) -> Result<Value, EngineError> {
    if let Some(refstr) = as_reference(v) {
        return resolve_reference(refstr, ctx);
    }
    match v {
        Value::Array(a) => {
            let mut out = Vec::with_capacity(a.len());
            for item in a {
                out.push(resolve_value(item, ctx)?);
            }
            Ok(Value::Array(out))
        }
        Value::Object(o) => {
            let mut out = Map::new();
            for (k, item) in o {
                out.insert(k.clone(), resolve_value(item, ctx)?);
            }
            Ok(Value::Object(out))
        }
        _ => Ok(v.clone()),
    }
}

fn eval_condition(cond: &Value, ctx: &ExecCtx) -> Result<bool, EngineError> {
    let obj = cond
        .as_object()
        .ok_or_else(|| EngineError::new(ErrorCode::InvalidCondition, "condition is not an object"))?;
    let op = obj
        .get("op")
        .and_then(Value::as_str)
        .ok_or_else(|| EngineError::new(ErrorCode::InvalidCondition, "condition missing op"))?;

    match op {
        "eq" | "ne" | "gt" | "gte" | "lt" | "lte" => {
            let left = resolve_value(
                obj.get("left").ok_or_else(|| {
                    EngineError::new(ErrorCode::InvalidCondition, "condition missing left")
                })?,
                ctx,
            )?;
            let right = resolve_value(
                obj.get("right").ok_or_else(|| {
                    EngineError::new(ErrorCode::InvalidCondition, "condition missing right")
                })?,
                ctx,
            )?;
            relation_holds(op, &left, &right)
        }
        "all" => {
            let arr = obj
                .get("conditions")
                .and_then(Value::as_array)
                .ok_or_else(|| {
                    EngineError::new(
                        ErrorCode::InvalidCondition,
                        "all condition requires conditions array",
                    )
                })?;
            for c in arr {
                if !eval_condition(c, ctx)? {
                    return Ok(false);
                }
            }
            Ok(true)
        }
        "any" => {
            let arr = obj
                .get("conditions")
                .and_then(Value::as_array)
                .ok_or_else(|| {
                    EngineError::new(
                        ErrorCode::InvalidCondition,
                        "any condition requires conditions array",
                    )
                })?;
            for c in arr {
                if eval_condition(c, ctx)? {
                    return Ok(true);
                }
            }
            Ok(false)
        }
        "not" => {
            let c = obj.get("condition").ok_or_else(|| {
                EngineError::new(ErrorCode::InvalidCondition, "not requires a condition")
            })?;
            Ok(!eval_condition(c, ctx)?)
        }
        _ => Err(EngineError::new(
            ErrorCode::InvalidCondition,
            format!("unknown condition operator '{}'", op),
        )),
    }
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------

fn compute_closures(steps: &[Step], by_id: &HashMap<String, usize>) -> Vec<HashSet<usize>> {
    let n = steps.len();
    let mut closures: Vec<HashSet<usize>> = vec![HashSet::new(); n];
    for i in 0..n {
        let mut seen: HashSet<usize> = HashSet::new();
        let mut stack: Vec<usize> = steps[i]
            .depends_on
            .iter()
            .filter_map(|d| by_id.get(d).copied())
            .collect();
        while let Some(j) = stack.pop() {
            if seen.insert(j) {
                for d in &steps[j].depends_on {
                    if let Some(&k) = by_id.get(d) {
                        stack.push(k);
                    }
                }
            }
        }
        closures[i] = seen;
    }
    closures
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum RunStatus {
    Completed,
    Failed,
}

struct RunOutcome {
    status: RunStatus,
    results: Map<String, Value>,
    execution_order: Vec<String>,
    skipped_order: Vec<String>,
    error_code: Option<ErrorCode>,
    error_step: Option<String>,
    error_message: Option<String>,
}

fn execute(steps: &[Step]) -> RunOutcome {
    let n = steps.len();
    let mut by_id: HashMap<String, usize> = HashMap::new();
    for (i, s) in steps.iter().enumerate() {
        by_id.insert(s.id.clone(), i);
    }
    let closures = compute_closures(steps, &by_id);

    let mut results: Map<String, Value> = Map::new();
    let mut skipped: HashSet<String> = HashSet::new();
    let mut execution_order: Vec<String> = Vec::new();
    let mut skipped_order: Vec<String> = Vec::new();
    let mut fail_attempts: HashMap<String, i64> = HashMap::new();
    let mut done = vec![false; n];

    for _ in 0..n {
        let mut chosen: Option<usize> = None;
        for i in 0..n {
            if done[i] {
                continue;
            }
            let ready = steps[i]
                .depends_on
                .iter()
                .all(|d| by_id.get(d).map(|&j| done[j]).unwrap_or(false));
            if ready {
                chosen = Some(i);
                break;
            }
        }
        let i = match chosen {
            Some(i) => i,
            None => {
                // Should be impossible after cycle validation.
                return RunOutcome {
                    status: RunStatus::Failed,
                    results,
                    execution_order,
                    skipped_order,
                    error_code: Some(ErrorCode::CyclicDependency),
                    error_step: None,
                    error_message: Some("could not schedule remaining steps".into()),
                };
            }
        };

        let step = &steps[i];
        let ctx = ExecCtx {
            steps,
            by_id: &by_id,
            closures: &closures,
            results: &results,
            skipped: &skipped,
            current: i,
        };

        let should_run = match &step.when {
            None => true,
            Some(cond) => match eval_condition(cond, &ctx) {
                Ok(b) => b,
                Err(e) => {
                    return RunOutcome {
                        status: RunStatus::Failed,
                        results,
                        execution_order,
                        skipped_order,
                        error_code: Some(e.code),
                        error_step: Some(step.id.clone()),
                        error_message: Some(e.message),
                    }
                }
            },
        };

        if !should_run {
            results.insert(step.id.clone(), Value::Null);
            execution_order.push(step.id.clone());
            skipped_order.push(step.id.clone());
            skipped.insert(step.id.clone());
            done[i] = true;
            continue;
        }

        let resolved = match resolve_value(&step.args, &ctx) {
            Ok(v) => v,
            Err(e) => {
                return RunOutcome {
                    status: RunStatus::Failed,
                    results,
                    execution_order,
                    skipped_order,
                    error_code: Some(e.code),
                    error_step: Some(step.id.clone()),
                    error_message: Some(e.message),
                }
            }
        };

        let mut attempt: usize = 0;
        let outcome: Result<Value, EngineError> = loop {
            attempt += 1;
            match run_op(&step.op, &resolved, &mut fail_attempts, &step.id) {
                Ok(v) => break Ok(v),
                Err(e) => {
                    if e.code == ErrorCode::TransientFailure {
                        if attempt < step.max_attempts {
                            continue;
                        }
                        break Err(EngineError::new(
                            ErrorCode::StepFailed,
                            format!(
                                "step '{}' failed after {} attempt(s): {}",
                                step.id, attempt, e.message
                            ),
                        ));
                    }
                    break Err(e);
                }
            }
        };

        match outcome {
            Ok(v) => {
                results.insert(step.id.clone(), v);
                execution_order.push(step.id.clone());
                done[i] = true;
            }
            Err(e) => {
                return RunOutcome {
                    status: RunStatus::Failed,
                    results,
                    execution_order,
                    skipped_order,
                    error_code: Some(e.code),
                    error_step: Some(step.id.clone()),
                    error_message: Some(e.message),
                };
            }
        }
    }

    RunOutcome {
        status: RunStatus::Completed,
        results,
        execution_order,
        skipped_order,
        error_code: None,
        error_step: None,
        error_message: None,
    }
}

// ---------------------------------------------------------------------------
// Response serialization / CLI
// ---------------------------------------------------------------------------

fn response_from_outcome(outcome: RunOutcome) -> Value {
    let status = match outcome.status {
        RunStatus::Completed => "completed",
        RunStatus::Failed => "failed",
    };
    let error = match outcome.error_code {
        None => Value::Null,
        Some(code) => {
            let mut e = Map::new();
            e.insert("code".into(), Value::String(code.as_str().into()));
            e.insert(
                "step".into(),
                match outcome.error_step {
                    Some(s) => Value::String(s),
                    None => Value::Null,
                },
            );
            e.insert(
                "message".into(),
                Value::String(outcome.error_message.unwrap_or_default()),
            );
            Value::Object(e)
        }
    };
    let mut resp = Map::new();
    resp.insert("status".into(), Value::String(status.into()));
    resp.insert("results".into(), Value::Object(outcome.results));
    resp.insert(
        "execution_order".into(),
        Value::Array(
            outcome
                .execution_order
                .into_iter()
                .map(Value::String)
                .collect(),
        ),
    );
    resp.insert(
        "skipped".into(),
        Value::Array(
            outcome
                .skipped_order
                .into_iter()
                .map(Value::String)
                .collect(),
        ),
    );
    resp.insert("error".into(), error);
    Value::Object(resp)
}

fn failure_response(
    code: ErrorCode,
    step: Option<String>,
    message: impl Into<String>,
) -> Value {
    response_from_outcome(RunOutcome {
        status: RunStatus::Failed,
        results: Map::new(),
        execution_order: Vec::new(),
        skipped_order: Vec::new(),
        error_code: Some(code),
        error_step: step,
        error_message: Some(message.into()),
    })
}

fn print_json(value: &Value) {
    match serde_json::to_string(value) {
        Ok(s) => println!("{}", s),
        Err(_) => {
            // Extremely unlikely. Keep stdout a JSON document anyway.
            println!(
                "{{\"status\":\"failed\",\"results\":{{}},\"execution_order\":[],\"skipped\":[],\"error\":{{\"code\":\"INVALID_WORKFLOW\",\"step\":null,\"message\":\"serialization failure\"}}}}"
            );
        }
    }
    let _ = io::stdout().flush();
}

fn read_input(path: &str) -> Result<String, String> {
    if path == "-" {
        let mut buf = String::new();
        io::stdin()
            .read_to_string(&mut buf)
            .map_err(|e| format!("failed to read stdin: {}", e))?;
        Ok(buf)
    } else {
        std::fs::read_to_string(path).map_err(|e| format!("failed to read '{}': {}", path, e))
    }
}

fn real_main() -> i32 {
    let args: Vec<String> = env::args().skip(1).collect();

    if args.len() != 2 || args[0] != "run" {
        let resp = failure_response(
            ErrorCode::InvalidWorkflow,
            None,
            "usage: workflow run <workflow.json> | workflow run -",
        );
        print_json(&resp);
        return 2;
    }

    let text = match read_input(&args[1]) {
        Ok(t) => t,
        Err(msg) => {
            let resp = failure_response(ErrorCode::InvalidWorkflow, None, msg);
            print_json(&resp);
            return 2;
        }
    };

    let doc: Value = match serde_json::from_str(&text) {
        Ok(v) => v,
        Err(e) => {
            let resp = failure_response(
                ErrorCode::InvalidWorkflow,
                None,
                format!("invalid JSON input: {}", e),
            );
            print_json(&resp);
            return 2;
        }
    };

    let steps = match validate_workflow(&doc) {
        Ok(s) => s,
        Err(ve) => {
            let resp = failure_response(ve.code, ve.step, ve.message);
            print_json(&resp);
            return ve.code.exit_code();
        }
    };

    let outcome = execute(&steps);
    let exit = match outcome.status {
        RunStatus::Completed => 0,
        RunStatus::Failed => outcome
            .error_code
            .map(|c| c.exit_code())
            .unwrap_or(1),
    };
    let resp = response_from_outcome(outcome);
    print_json(&resp);
    exit
}

fn main() {
    // Best-effort JSON output on internal panics (exit code 3).
    let result = std::panic::catch_unwind(real_main);
    match result {
        Ok(code) => process::exit(code),
        Err(_) => {
            let resp = failure_response(
                ErrorCode::InvalidWorkflow,
                None,
                "internal error",
            );
            print_json(&resp);
            process::exit(3);
        }
    }
}
