use serde_json::{Map, Number, Value};
use std::cmp::Ordering;
use std::collections::{HashMap, HashSet};
use std::io::Read;

const OPS: &[&str] = &[
    "add", "subtract", "multiply", "divide", "mod", "sum", "min", "max", "range",
    "concat", "split", "join", "uppercase", "lowercase", "trim", "replace", "length",
    "map", "filter", "reduce", "sort", "reverse", "unique", "slice", "flatten",
    "get", "set", "keys", "values", "merge", "pick", "omit",
    "constant", "equals", "compare", "and", "or", "not", "type",
    "fail_until",
];

#[derive(Debug, Clone)]
struct WfError {
    code: String,
    step: Option<String>,
    message: String,
}

impl WfError {
    fn new(code: &str, message: impl Into<String>) -> Self {
        WfError {
            code: code.to_string(),
            step: None,
            message: message.into(),
        }
    }

    fn at(mut self, step: &str) -> Self {
        if self.step.is_none() {
            self.step = Some(step.to_string());
        }
        self
    }
}

type WResult<T> = Result<T, WfError>;

fn invalid(step: Option<&str>, message: impl Into<String>) -> WfError {
    WfError {
        code: "INVALID_WORKFLOW".to_string(),
        step: step.map(|s| s.to_string()),
        message: message.into(),
    }
}

fn invalid_arg(message: impl Into<String>) -> WfError {
    WfError::new("INVALID_ARG", message)
}

fn invalid_ref(message: impl Into<String>) -> WfError {
    WfError::new("INVALID_REFERENCE", message)
}

fn overflow() -> WfError {
    WfError::new("ARITHMETIC_OVERFLOW", "i64 arithmetic overflow")
}

fn division_by_zero() -> WfError {
    WfError::new("DIVISION_BY_ZERO", "division by zero")
}

#[derive(Debug, Clone)]
struct Step {
    id: String,
    op: String,
    args: Map<String, Value>,
    depends_on: Vec<String>,
    when: Option<Value>,
    max_attempts: u32,
}

fn valid_id(id: &str) -> bool {
    let bytes = id.as_bytes();
    if bytes.is_empty() {
        return false;
    }
    let first = bytes[0];
    if !(first.is_ascii_alphabetic() || first == b'_') {
        return false;
    }
    bytes[1..]
        .iter()
        .all(|b| b.is_ascii_alphanumeric() || *b == b'_')
}

fn check_allowed_keys(
    obj: &Map<String, Value>,
    allowed: &[&str],
    step: Option<&str>,
) -> WResult<()> {
    for key in obj.keys() {
        if !allowed.iter().any(|a| *a == key.as_str()) {
            return Err(invalid(step, format!("unknown field '{}'", key)));
        }
    }
    Ok(())
}

fn validate_condition(cond: &Value, step: Option<&str>) -> WResult<()> {
    let obj = match cond.as_object() {
        Some(o) => o,
        None => return Err(invalid(step, "condition must be an object")),
    };
    let op = match obj.get("op").and_then(|v| v.as_str()) {
        Some(o) => o,
        None => return Err(invalid(step, "condition.op must be a string")),
    };
    match op {
        "eq" | "ne" | "gt" | "gte" | "lt" | "lte" => {
            check_allowed_keys(obj, &["op", "left", "right"], step)?;
            if !obj.contains_key("left") || !obj.contains_key("right") {
                return Err(invalid(step, "comparison condition requires left and right"));
            }
        }
        "all" | "any" => {
            check_allowed_keys(obj, &["op", "conditions"], step)?;
            let arr = match obj.get("conditions").and_then(|v| v.as_array()) {
                Some(a) => a,
                None => return Err(invalid(step, "conditions must be an array")),
            };
            for sub in arr {
                validate_condition(sub, step)?;
            }
        }
        "not" => {
            check_allowed_keys(obj, &["op", "condition"], step)?;
            let sub = match obj.get("condition") {
                Some(c) => c,
                None => return Err(invalid(step, "not condition requires condition")),
            };
            validate_condition(sub, step)?;
        }
        other => {
            return Err(invalid(
                step,
                format!("unknown condition operator '{}'", other),
            ))
        }
    }
    Ok(())
}

fn detect_cycle(steps: &[Step]) -> WResult<()> {
    let index: HashMap<&str, usize> = steps
        .iter()
        .enumerate()
        .map(|(i, s)| (s.id.as_str(), i))
        .collect();
    let mut color = vec![0u8; steps.len()]; // 0 white, 1 gray, 2 black

    fn visit(
        i: usize,
        steps: &[Step],
        index: &HashMap<&str, usize>,
        color: &mut [u8],
    ) -> WResult<()> {
        if color[i] == 1 {
            return Err(WfError::new(
                "CYCLIC_DEPENDENCY",
                "dependency cycle detected",
            ));
        }
        if color[i] == 2 {
            return Ok(());
        }
        color[i] = 1;
        for dep in &steps[i].depends_on {
            if let Some(&j) = index.get(dep.as_str()) {
                visit(j, steps, index, color)?;
            }
        }
        color[i] = 2;
        Ok(())
    }

    for i in 0..steps.len() {
        visit(i, steps, &index, &mut color)?;
    }
    Ok(())
}

fn validate(doc: &Value) -> WResult<Vec<Step>> {
    let obj = match doc.as_object() {
        Some(o) => o,
        None => return Err(invalid(None, "workflow document must be an object")),
    };

    for key in obj.keys() {
        if key != "version" && key != "steps" {
            return Err(invalid(
                None,
                format!("unknown top-level field '{}'", key),
            ));
        }
    }

    match obj.get("version") {
        Some(Value::Number(n)) if n.as_i64() == Some(1) => {}
        Some(_) => return Err(invalid(None, "version must be the integer 1")),
        None => return Err(invalid(None, "missing version")),
    }

    let steps_val = match obj.get("steps") {
        Some(v) => v,
        None => return Err(invalid(None, "missing steps array")),
    };
    let step_values = match steps_val.as_array() {
        Some(a) => a,
        None => return Err(invalid(None, "steps must be an array")),
    };

    let mut steps: Vec<Step> = Vec::new();
    let mut seen: HashSet<String> = HashSet::new();

    for sv in step_values {
        let so = match sv.as_object() {
            Some(o) => o,
            None => return Err(invalid(None, "each step must be an object")),
        };

        let id_from_field: Option<String> = so
            .get("id")
            .and_then(|v| v.as_str())
            .map(|s| s.to_string());

        for key in so.keys() {
            if !["id", "op", "args", "depends_on", "when", "retry"]
                .iter()
                .any(|a| *a == key.as_str())
            {
                return Err(invalid(
                    id_from_field.as_deref(),
                    format!("unknown step field '{}'", key),
                ));
            }
        }

        let id = match so.get("id") {
            Some(Value::String(s)) => s.clone(),
            _ => return Err(invalid(None, "step id must be a string")),
        };
        if !valid_id(&id) {
            return Err(invalid(Some(&id), "invalid step id"));
        }
        if !seen.insert(id.clone()) {
            return Err(invalid(Some(&id), "duplicate step id"));
        }

        let op = match so.get("op") {
            Some(Value::String(s)) => {
                if OPS.iter().any(|o| *o == s.as_str()) {
                    s.clone()
                } else {
                    return Err(invalid(Some(&id), format!("unknown operation '{}'", s)));
                }
            }
            Some(_) => return Err(invalid(Some(&id), "step op must be a string")),
            None => return Err(invalid(Some(&id), "missing step op")),
        };

        let args = match so.get("args") {
            None => Map::new(),
            Some(Value::Object(m)) => m.clone(),
            Some(_) => return Err(invalid(Some(&id), "step args must be an object")),
        };

        let depends_on = match so.get("depends_on") {
            None => Vec::new(),
            Some(Value::Array(a)) => {
                let mut deps = Vec::new();
                for d in a {
                    match d {
                        Value::String(s) => deps.push(s.clone()),
                        _ => {
                            return Err(invalid(
                                Some(&id),
                                "depends_on entries must be strings",
                            ))
                        }
                    }
                }
                deps
            }
            Some(_) => return Err(invalid(Some(&id), "depends_on must be an array")),
        };

        let when = match so.get("when") {
            None => None,
            Some(w) => {
                validate_condition(w, Some(&id))?;
                Some(w.clone())
            }
        };

        let max_attempts = match so.get("retry") {
            None => 1u32,
            Some(Value::Object(r)) => {
                for key in r.keys() {
                    if key != "max_attempts" {
                        return Err(invalid(
                            Some(&id),
                            format!("unknown retry field '{}'", key),
                        ));
                    }
                }
                match r.get("max_attempts") {
                    None => 1,
                    Some(Value::Number(n)) => match n.as_i64() {
                        Some(x) if (1..=5).contains(&x) => x as u32,
                        _ => {
                            return Err(invalid(
                                Some(&id),
                                "retry.max_attempts must be an integer 1..5",
                            ))
                        }
                    },
                    Some(_) => {
                        return Err(invalid(
                            Some(&id),
                            "retry.max_attempts must be an integer 1..5",
                        ))
                    }
                }
            }
            Some(_) => return Err(invalid(Some(&id), "retry must be an object")),
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

    let ids: HashSet<&str> = steps.iter().map(|s| s.id.as_str()).collect();
    for step in &steps {
        for dep in &step.depends_on {
            if !ids.contains(dep.as_str()) {
                return Err(WfError {
                    code: "UNKNOWN_DEPENDENCY".to_string(),
                    step: Some(step.id.clone()),
                    message: format!("unknown dependency '{}'", dep),
                });
            }
        }
    }

    detect_cycle(&steps)?;

    Ok(steps)
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

fn num_as_i128(n: &Number) -> Option<i128> {
    if let Some(i) = n.as_i64() {
        Some(i as i128)
    } else {
        n.as_u64().map(|u| u as i128)
    }
}

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
    // Compare against trunc(f) exactly (as an integer), then use the sign of
    // the fractional part. This avoids losing precision by converting a large
    // integer to f64. Out-of-range casts saturate, which still yields the
    // correct ordering for our i64/u64 integer range.
    let truncated = f.trunc();
    let as_int = truncated as i128;
    match i.cmp(&as_int) {
        Ordering::Equal => {
            if f > truncated {
                Ordering::Less
            } else if f < truncated {
                Ordering::Greater
            } else {
                Ordering::Equal
            }
        }
        ord => ord,
    }
}

fn num_cmp(a: &Number, b: &Number) -> Ordering {
    let ia = num_as_i128(a);
    let ib = num_as_i128(b);
    match (ia, ib) {
        (Some(x), Some(y)) => x.cmp(&y),
        (Some(x), None) => cmp_int_float(x, b.as_f64().unwrap_or(0.0)),
        (None, Some(y)) => cmp_int_float(y, a.as_f64().unwrap_or(0.0)).reverse(),
        (None, None) => {
            let x = a.as_f64().unwrap_or(0.0);
            let y = b.as_f64().unwrap_or(0.0);
            x.partial_cmp(&y).unwrap_or(Ordering::Equal)
        }
    }
}

fn num_eq(a: &Number, b: &Number) -> bool {
    num_cmp(a, b) == Ordering::Equal
}

fn structural_eq(a: &Value, b: &Value) -> bool {
    match (a, b) {
        (Value::Null, Value::Null) => true,
        (Value::Bool(x), Value::Bool(y)) => x == y,
        (Value::Number(x), Value::Number(y)) => num_eq(x, y),
        (Value::String(x), Value::String(y)) => x == y,
        (Value::Array(x), Value::Array(y)) => {
            x.len() == y.len() && x.iter().zip(y.iter()).all(|(p, q)| structural_eq(p, q))
        }
        (Value::Object(x), Value::Object(y)) => {
            x.len() == y.len()
                && x.iter()
                    .all(|(k, v)| y.get(k).map(|w| structural_eq(v, w)).unwrap_or(false))
        }
        _ => false,
    }
}

fn sorted_keys(m: &Map<String, Value>) -> Vec<&String> {
    let mut keys: Vec<&String> = m.keys().collect();
    keys.sort_by(|a, b| a.as_bytes().cmp(b.as_bytes()));
    keys
}

fn total_cmp(a: &Value, b: &Value) -> Ordering {
    let ra = type_rank(a);
    let rb = type_rank(b);
    if ra != rb {
        return ra.cmp(&rb);
    }
    match (a, b) {
        (Value::Null, Value::Null) => Ordering::Equal,
        (Value::Bool(x), Value::Bool(y)) => x.cmp(y),
        (Value::Number(x), Value::Number(y)) => num_cmp(x, y),
        (Value::String(x), Value::String(y)) => x.as_bytes().cmp(y.as_bytes()),
        (Value::Array(x), Value::Array(y)) => {
            let n = x.len().min(y.len());
            for i in 0..n {
                let c = total_cmp(&x[i], &y[i]);
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
            for key in kx {
                if let (Some(vx), Some(vy)) = (x.get(key), y.get(key)) {
                    let c = total_cmp(vx, vy);
                    if c != Ordering::Equal {
                        return c;
                    }
                }
            }
            Ordering::Equal
        }
        _ => Ordering::Equal,
    }
}

fn truthy(v: &Value) -> bool {
    match v {
        Value::Null => false,
        Value::Bool(b) => *b,
        Value::Number(n) => {
            if let Some(i) = num_as_i128(n) {
                i != 0
            } else {
                n.as_f64().map(|f| f != 0.0).unwrap_or(false)
            }
        }
        Value::String(s) => !s.is_empty(),
        Value::Array(a) => !a.is_empty(),
        Value::Object(o) => !o.is_empty(),
    }
}

fn int_value(v: &Value) -> WResult<i64> {
    match v {
        Value::Number(n) => n
            .as_i64()
            .ok_or_else(|| invalid_arg("numeric argument must be an integer literal fitting i64")),
        _ => Err(invalid_arg("expected an integer")),
    }
}

fn req_arg<'a>(args: &'a Map<String, Value>, name: &str) -> WResult<&'a Value> {
    args.get(name)
        .ok_or_else(|| invalid_arg(format!("missing required argument '{}'", name)))
}

fn req_i64(args: &Map<String, Value>, name: &str) -> WResult<i64> {
    int_value(req_arg(args, name)?)
}

fn opt_i64(args: &Map<String, Value>, name: &str) -> WResult<Option<i64>> {
    match args.get(name) {
        None => Ok(None),
        Some(v) => Ok(Some(int_value(v)?)),
    }
}

fn req_str<'a>(args: &'a Map<String, Value>, name: &str) -> WResult<&'a str> {
    match req_arg(args, name)? {
        Value::String(s) => Ok(s),
        _ => Err(invalid_arg(format!("argument '{}' must be a string", name))),
    }
}

fn req_array<'a>(args: &'a Map<String, Value>, name: &str) -> WResult<&'a Vec<Value>> {
    match req_arg(args, name)? {
        Value::Array(a) => Ok(a),
        _ => Err(invalid_arg(format!("argument '{}' must be an array", name))),
    }
}

fn req_obj<'a>(args: &'a Map<String, Value>, name: &str) -> WResult<&'a Map<String, Value>> {
    match req_arg(args, name)? {
        Value::Object(o) => Ok(o),
        _ => Err(invalid_arg(format!("argument '{}' must be an object", name))),
    }
}

fn check_args(args: &Map<String, Value>, allowed: &[&str]) -> WResult<()> {
    for key in args.keys() {
        if !allowed.iter().any(|a| *a == key.as_str()) {
            return Err(invalid_arg(format!("unknown argument '{}'", key)));
        }
    }
    Ok(())
}

fn num_value(v: i64) -> Value {
    Value::Number(Number::from(v))
}

fn binary_i64(op: &str, a: i64, b: i64) -> WResult<Value> {
    let result = match op {
        "add" => a.checked_add(b),
        "subtract" => a.checked_sub(b),
        "multiply" => a.checked_mul(b),
        "divide" => {
            if b == 0 {
                return Err(division_by_zero());
            }
            if a == i64::MIN && b == -1 {
                return Err(overflow());
            }
            Some(a / b)
        }
        "mod" => {
            if b == 0 {
                return Err(division_by_zero());
            }
            if a == i64::MIN && b == -1 {
                return Err(overflow());
            }
            Some(a % b)
        }
        _ => return Err(invalid_arg(format!("unsupported binary operator '{}'", op))),
    };
    match result {
        Some(v) => Ok(num_value(v)),
        None => Err(overflow()),
    }
}

fn binary_args(op: &str, args: &Map<String, Value>) -> WResult<Value> {
    check_args(args, &["a", "b"])?;
    let a = req_i64(args, "a")?;
    let b = req_i64(args, "b")?;
    binary_i64(op, a, b)
}

#[derive(Debug, Clone)]
enum MapTransform {
    Constant(Value),
    Binary(String, i64),
    Negate,
    Abs,
    Stringify,
    TypeName,
    OpForm(String, Map<String, Value>),
}

fn is_self_marker(v: &Value) -> bool {
    match v {
        Value::Object(m) => {
            m.len() == 1 && matches!(m.get("$self"), Some(Value::Bool(true)))
        }
        _ => false,
    }
}

fn check_transform_keys(obj: &Map<String, Value>, allowed: &[&str]) -> WResult<()> {
    for key in obj.keys() {
        if !allowed.iter().any(|a| *a == key.as_str()) {
            return Err(invalid_arg(format!(
                "unknown transform field '{}'",
                key
            )));
        }
    }
    Ok(())
}

fn parse_transform(v: &Value) -> WResult<MapTransform> {
    let obj = match v.as_object() {
        Some(o) => o,
        None => return Err(invalid_arg("map.transform must be an object")),
    };
    let ty = match obj.get("type").and_then(|v| v.as_str()) {
        Some(t) => t,
        None => return Err(invalid_arg("map.transform.type must be a string")),
    };
    match ty {
        "constant" => {
            check_transform_keys(obj, &["type", "value"])?;
            let value = req_arg(obj, "value")?.clone();
            Ok(MapTransform::Constant(value))
        }
        "add" | "subtract" | "multiply" | "divide" | "mod" => {
            check_transform_keys(obj, &["type", "value"])?;
            let value = req_i64(obj, "value")?;
            Ok(MapTransform::Binary(ty.to_string(), value))
        }
        "negate" => {
            check_transform_keys(obj, &["type"])?;
            Ok(MapTransform::Negate)
        }
        "abs" => {
            check_transform_keys(obj, &["type"])?;
            Ok(MapTransform::Abs)
        }
        "stringify" => {
            check_transform_keys(obj, &["type"])?;
            Ok(MapTransform::Stringify)
        }
        "type" => {
            check_transform_keys(obj, &["type"])?;
            Ok(MapTransform::TypeName)
        }
        "op" => {
            check_transform_keys(obj, &["type", "op", "args"])?;
            let op = match obj.get("op").and_then(|v| v.as_str()) {
                Some(o)
                    if matches!(
                        o,
                        "add" | "subtract" | "multiply" | "divide" | "mod"
                    ) =>
                {
                    o.to_string()
                }
                _ => {
                    return Err(invalid_arg(
                        "map.transform.op must be a supported 2-number operation",
                    ))
                }
            };
            let targs = match obj.get("args") {
                Some(Value::Object(m)) => m,
                Some(_) => return Err(invalid_arg("map.transform.args must be an object")),
                None => return Err(invalid_arg("map.transform.args is required")),
            };
            for key in targs.keys() {
                if key != "a" && key != "b" {
                    return Err(invalid_arg(format!(
                        "unknown transform argument '{}'",
                        key
                    )));
                }
            }
            if !targs.contains_key("a") || !targs.contains_key("b") {
                return Err(invalid_arg(
                    "map.transform.args requires a and b",
                ));
            }
            for key in ["a", "b"] {
                let val = &targs[key];
                if !is_self_marker(val) {
                    if int_value(val).is_err() {
                        return Err(invalid_arg(
                            "map.transform.args values must be $self or integers",
                        ));
                    }
                }
            }
            Ok(MapTransform::OpForm(op, targs.clone()))
        }
        other => Err(invalid_arg(format!("unknown transform type '{}'", other))),
    }
}

fn apply_transform(t: &MapTransform, elem: &Value) -> WResult<Value> {
    match t {
        MapTransform::Constant(v) => Ok(v.clone()),
        MapTransform::Binary(op, x) => {
            let a = int_value(elem)?;
            binary_i64(op, a, *x)
        }
        MapTransform::Negate => {
            let a = int_value(elem)?;
            a.checked_neg().map(num_value).ok_or_else(overflow)
        }
        MapTransform::Abs => {
            let a = int_value(elem)?;
            a.checked_abs().map(num_value).ok_or_else(overflow)
        }
        MapTransform::Stringify => serde_json::to_string(elem)
            .map(Value::String)
            .map_err(|e| WfError::new("INVALID_ARG", format!("cannot stringify: {}", e))),
        MapTransform::TypeName => Ok(Value::String(type_name(elem).to_string())),
        MapTransform::OpForm(op, targs) => {
            let mut resolved = Map::new();
            for (key, val) in targs {
                if is_self_marker(val) {
                    resolved.insert(key.clone(), elem.clone());
                } else {
                    resolved.insert(key.clone(), val.clone());
                }
            }
            let a = req_i64(&resolved, "a")?;
            let b = req_i64(&resolved, "b")?;
            binary_i64(op, a, b)
        }
    }
}

#[derive(Debug, Clone)]
enum Predicate {
    Even,
    Odd,
    Compare(String, i64),
    Truthy,
    Falsy,
    TypeIs(String),
    StringContains(String),
}

fn check_predicate_keys(obj: &Map<String, Value>, allowed: &[&str]) -> WResult<()> {
    for key in obj.keys() {
        if !allowed.iter().any(|a| *a == key.as_str()) {
            return Err(invalid_arg(format!("unknown predicate field '{}'", key)));
        }
    }
    Ok(())
}

fn parse_predicate(v: &Value) -> WResult<Predicate> {
    if let Value::String(s) = v {
        return match s.as_str() {
            "even" => Ok(Predicate::Even),
            "odd" => Ok(Predicate::Odd),
            "truthy" => Ok(Predicate::Truthy),
            "falsy" => Ok(Predicate::Falsy),
            _ => Err(invalid_arg(format!("unknown predicate '{}'", s))),
        };
    }
    let obj = match v.as_object() {
        Some(o) => o,
        None => return Err(invalid_arg("filter.predicate must be an object")),
    };
    let ty = match obj.get("type").and_then(|v| v.as_str()) {
        Some(t) => t,
        None => return Err(invalid_arg("filter.predicate.type must be a string")),
    };
    match ty {
        "even" => {
            check_predicate_keys(obj, &["type"])?;
            Ok(Predicate::Even)
        }
        "odd" => {
            check_predicate_keys(obj, &["type"])?;
            Ok(Predicate::Odd)
        }
        "gt" | "gte" | "lt" | "lte" | "eq" | "ne" => {
            check_predicate_keys(obj, &["type", "value"])?;
            let value = req_i64(obj, "value")?;
            Ok(Predicate::Compare(ty.to_string(), value))
        }
        "truthy" => {
            check_predicate_keys(obj, &["type"])?;
            Ok(Predicate::Truthy)
        }
        "falsy" => {
            check_predicate_keys(obj, &["type"])?;
            Ok(Predicate::Falsy)
        }
        "type_is" => {
            // SPEC writes this form with a duplicated "type" key, which no JSON
            // parser can represent. Accept the obvious explicit spellings.
            let aliases = [
                "value",
                "type_name",
                "expected",
                "expected_type",
                "target",
                "name",
            ];
            let mut allowed: Vec<&str> = vec!["type"];
            allowed.extend(aliases.iter().copied());
            check_predicate_keys(obj, &allowed)?;
            let target = aliases.iter().find_map(|key| match obj.get(*key) {
                Some(Value::String(s)) => Some(s.clone()),
                _ => None,
            });
            match target {
                Some(t) => Ok(Predicate::TypeIs(t)),
                None => Err(invalid_arg(
                    "predicate type_is requires a type string in value",
                )),
            }
        }
        "string_contains" => {
            check_predicate_keys(obj, &["type", "value"])?;
            let needle = match obj.get("value") {
                Some(Value::String(s)) => s.clone(),
                _ => {
                    return Err(invalid_arg(
                        "predicate string_contains requires a string value",
                    ))
                }
            };
            Ok(Predicate::StringContains(needle))
        }
        other => Err(invalid_arg(format!("unknown predicate type '{}'", other))),
    }
}

fn apply_predicate(p: &Predicate, elem: &Value) -> WResult<bool> {
    match p {
        Predicate::Even => {
            let n = int_value(elem)?;
            Ok(n % 2 == 0)
        }
        Predicate::Odd => {
            let n = int_value(elem)?;
            Ok(n % 2 != 0)
        }
        Predicate::Compare(op, value) => {
            let n = int_value(elem)?;
            let result = match op.as_str() {
                "gt" => n > *value,
                "gte" => n >= *value,
                "lt" => n < *value,
                "lte" => n <= *value,
                "eq" => n == *value,
                "ne" => n != *value,
                _ => return Err(invalid_arg("unknown comparison predicate")),
            };
            Ok(result)
        }
        Predicate::Truthy => Ok(truthy(elem)),
        Predicate::Falsy => Ok(!truthy(elem)),
        Predicate::TypeIs(target) => Ok(type_name(elem) == target.as_str()),
        Predicate::StringContains(needle) => match elem {
            Value::String(s) => Ok(s.contains(needle.as_str())),
            _ => Err(invalid_arg(
                "string_contains predicate requires string elements",
            )),
        },
    }
}

struct RunState {
    results: Map<String, Value>,
    execution_order: Vec<String>,
    skipped: Vec<String>,
    skipped_set: HashSet<String>,
    finished: HashSet<String>,
    closures: HashMap<String, HashSet<String>>,
}

fn compute_closures(steps: &[Step]) -> HashMap<String, HashSet<String>> {
    let by_id: HashMap<&str, &Step> = steps.iter().map(|s| (s.id.as_str(), s)).collect();
    let mut closures = HashMap::new();
    for step in steps {
        let mut set: HashSet<String> = HashSet::new();
        let mut stack: Vec<String> = step.depends_on.clone();
        while let Some(id) = stack.pop() {
            if set.insert(id.clone()) {
                if let Some(dep) = by_id.get(id.as_str()) {
                    for d in &dep.depends_on {
                        if !set.contains(d) {
                            stack.push(d.clone());
                        }
                    }
                }
            }
        }
        closures.insert(step.id.clone(), set);
    }
    closures
}

fn parse_index(seg: &str) -> WResult<usize> {
    if seg.is_empty() || !seg.bytes().all(|b| b.is_ascii_digit()) {
        return Err(invalid_ref("array index must be a non-negative decimal integer"));
    }
    seg.parse::<usize>()
        .map_err(|_| invalid_ref("array index out of range"))
}

fn resolve_ref(path: &str, step: &Step, st: &RunState) -> WResult<Value> {
    let mut parts = path.split('.');
    let target = parts.next().unwrap_or("");
    if target.is_empty() {
        return Err(invalid_ref("empty reference target"));
    }
    if target == step.id {
        return Err(invalid_ref("self reference"));
    }
    let allowed = st
        .closures
        .get(&step.id)
        .map(|c| c.contains(target))
        .unwrap_or(false);
    if !allowed {
        return Err(invalid_ref(format!(
            "'{}' is not a dependency of '{}'",
            target, step.id
        )));
    }
    if st.skipped_set.contains(target) {
        return Err(WfError::new(
            "SKIPPED_REFERENCE",
            format!("referenced step '{}' was skipped", target),
        ));
    }
    let mut current = match st.results.get(target) {
        Some(v) => v.clone(),
        None => {
            return Err(invalid_ref(format!(
                "referenced step '{}' has no result",
                target
            )))
        }
    };
    for seg in parts {
        current = match current {
            Value::Object(m) => match m.get(seg) {
                Some(v) => v.clone(),
                None => {
                    return Err(invalid_ref(format!(
                        "missing key '{}' in reference '{}'",
                        seg, path
                    )))
                }
            },
            Value::Array(a) => {
                let idx = parse_index(seg)?;
                match a.get(idx) {
                    Some(v) => v.clone(),
                    None => {
                        return Err(invalid_ref(format!(
                            "index {} out of bounds in reference '{}'",
                            idx, path
                        )))
                    }
                }
            }
            _ => {
                return Err(invalid_ref(format!(
                    "cannot descend into scalar in reference '{}'",
                    path
                )))
            }
        };
    }
    Ok(current)
}

fn resolve_value(v: &Value, step: &Step, st: &RunState) -> WResult<Value> {
    match v {
        Value::Object(m) => {
            if m.len() == 1 {
                if let Some(Value::String(path)) = m.get("$ref") {
                    return resolve_ref(path, step, st);
                }
            }
            let mut out = Map::new();
            for (key, val) in m {
                out.insert(key.clone(), resolve_value(val, step, st)?);
            }
            Ok(Value::Object(out))
        }
        Value::Array(a) => {
            let mut out = Vec::with_capacity(a.len());
            for val in a {
                out.push(resolve_value(val, step, st)?);
            }
            Ok(Value::Array(out))
        }
        _ => Ok(v.clone()),
    }
}

fn eval_condition(cond: &Value, step: &Step, st: &RunState) -> WResult<bool> {
    let obj = match cond.as_object() {
        Some(o) => o,
        None => {
            return Err(WfError::new(
                "INVALID_CONDITION",
                "condition must be an object",
            ))
        }
    };
    let op = match obj.get("op").and_then(|v| v.as_str()) {
        Some(o) => o,
        None => {
            return Err(WfError::new(
                "INVALID_CONDITION",
                "condition.op must be a string",
            ))
        }
    };
    match op {
        "eq" | "ne" | "gt" | "gte" | "lt" | "lte" => {
            let left = match obj.get("left") {
                Some(v) => resolve_value(v, step, st)?,
                None => {
                    return Err(WfError::new(
                        "INVALID_CONDITION",
                        "missing left operand",
                    ))
                }
            };
            let right = match obj.get("right") {
                Some(v) => resolve_value(v, step, st)?,
                None => {
                    return Err(WfError::new(
                        "INVALID_CONDITION",
                        "missing right operand",
                    ))
                }
            };
            let result = match op {
                "eq" => structural_eq(&left, &right),
                "ne" => !structural_eq(&left, &right),
                "gt" => total_cmp(&left, &right) == Ordering::Greater,
                "gte" => total_cmp(&left, &right) != Ordering::Less,
                "lt" => total_cmp(&left, &right) == Ordering::Less,
                "lte" => total_cmp(&left, &right) != Ordering::Greater,
                _ => unreachable!(),
            };
            Ok(result)
        }
        "all" => {
            let arr = match obj.get("conditions").and_then(|v| v.as_array()) {
                Some(a) => a,
                None => {
                    return Err(WfError::new(
                        "INVALID_CONDITION",
                        "all requires a conditions array",
                    ))
                }
            };
            for sub in arr {
                if !eval_condition(sub, step, st)? {
                    return Ok(false);
                }
            }
            Ok(true)
        }
        "any" => {
            let arr = match obj.get("conditions").and_then(|v| v.as_array()) {
                Some(a) => a,
                None => {
                    return Err(WfError::new(
                        "INVALID_CONDITION",
                        "any requires a conditions array",
                    ))
                }
            };
            for sub in arr {
                if eval_condition(sub, step, st)? {
                    return Ok(true);
                }
            }
            Ok(false)
        }
        "not" => {
            let sub = match obj.get("condition") {
                Some(c) => c,
                None => {
                    return Err(WfError::new(
                        "INVALID_CONDITION",
                        "not requires a condition",
                    ))
                }
            };
            Ok(!eval_condition(sub, step, st)?)
        }
        other => Err(WfError::new(
            "INVALID_CONDITION",
            format!("unknown condition operator '{}'", other),
        )),
    }
}

fn execute_op(op: &str, args: &Map<String, Value>, attempt: u32) -> WResult<Value> {
    match op {
        "add" | "subtract" | "multiply" | "divide" | "mod" => binary_args(op, args),
        "sum" => {
            check_args(args, &["input"])?;
            let arr = req_array(args, "input")?;
            let mut total: i64 = 0;
            for v in arr {
                let n = int_value(v)?;
                total = total.checked_add(n).ok_or_else(overflow)?;
            }
            Ok(num_value(total))
        }
        "min" | "max" => {
            check_args(args, &["input"])?;
            let arr = req_array(args, "input")?;
            if arr.is_empty() {
                return Err(WfError::new(
                    "EMPTY_ARRAY",
                    format!("{} requires a non-empty array", op),
                ));
            }
            let mut best = int_value(&arr[0])?;
            for v in &arr[1..] {
                let n = int_value(v)?;
                if (op == "min" && n < best) || (op == "max" && n > best) {
                    best = n;
                }
            }
            Ok(num_value(best))
        }
        "range" => {
            check_args(args, &["start", "end"])?;
            let start = req_i64(args, "start")?;
            let end = req_i64(args, "end")?;
            let mut out = Vec::new();
            if start <= end {
                let mut current = start;
                loop {
                    out.push(num_value(current));
                    if current == end {
                        break;
                    }
                    current = match current.checked_add(1) {
                        Some(n) => n,
                        None => break,
                    };
                }
            }
            Ok(Value::Array(out))
        }
        "concat" => {
            check_args(args, &["a", "b"])?;
            let a = req_str(args, "a")?;
            let b = req_str(args, "b")?;
            Ok(Value::String(format!("{}{}", a, b)))
        }
        "split" => {
            check_args(args, &["input", "sep"])?;
            let input = req_str(args, "input")?;
            let sep = req_str(args, "sep")?;
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
            check_args(args, &["input", "sep"])?;
            let arr = req_array(args, "input")?;
            let sep = req_str(args, "sep")?;
            let mut pieces = Vec::with_capacity(arr.len());
            for v in arr {
                match v {
                    Value::String(s) => pieces.push(s.as_str()),
                    _ => return Err(invalid_arg("join input must contain only strings")),
                }
            }
            Ok(Value::String(pieces.join(sep)))
        }
        "uppercase" => {
            check_args(args, &["input"])?;
            let s = req_str(args, "input")?;
            Ok(Value::String(
                s.chars().map(|c| c.to_ascii_uppercase()).collect(),
            ))
        }
        "lowercase" => {
            check_args(args, &["input"])?;
            let s = req_str(args, "input")?;
            Ok(Value::String(
                s.chars().map(|c| c.to_ascii_lowercase()).collect(),
            ))
        }
        "trim" => {
            check_args(args, &["input"])?;
            let s = req_str(args, "input")?;
            let trimmed = s.trim_matches(|c| {
                matches!(c, ' ' | '\t' | '\n' | '\r' | '\x0c' | '\x0b')
            });
            Ok(Value::String(trimmed.to_string()))
        }
        "replace" => {
            check_args(args, &["input", "search", "replace"])?;
            let input = req_str(args, "input")?;
            let search = req_str(args, "search")?;
            let replacement = req_str(args, "replace")?;
            if search.is_empty() {
                return Err(invalid_arg("replace search must not be empty"));
            }
            Ok(Value::String(input.replace(search, replacement)))
        }
        "length" => {
            check_args(args, &["input"])?;
            let s = req_str(args, "input")?;
            Ok(num_value(s.chars().count() as i64))
        }
        "map" => {
            check_args(args, &["input", "transform"])?;
            let arr = req_array(args, "input")?;
            let transform = parse_transform(req_arg(args, "transform")?)?;
            let mut out = Vec::with_capacity(arr.len());
            for elem in arr {
                out.push(apply_transform(&transform, elem)?);
            }
            Ok(Value::Array(out))
        }
        "filter" => {
            check_args(args, &["input", "predicate"])?;
            let arr = req_array(args, "input")?;
            let predicate = parse_predicate(req_arg(args, "predicate")?)?;
            let mut out = Vec::new();
            for elem in arr {
                if apply_predicate(&predicate, elem)? {
                    out.push(elem.clone());
                }
            }
            Ok(Value::Array(out))
        }
        "reduce" => {
            check_args(args, &["input", "op", "initial"])?;
            let arr = req_array(args, "input")?;
            let fold_op = req_str(args, "op")?;
            if !matches!(fold_op, "add" | "subtract" | "multiply") {
                return Err(invalid_arg("reduce op must be add, subtract or multiply"));
            }
            let identity = if fold_op == "multiply" { 1 } else { 0 };
            let mut acc = match opt_i64(args, "initial")? {
                Some(v) => v,
                None => identity,
            };
            for v in arr {
                let n = int_value(v)?;
                acc = match fold_op {
                    "add" => acc.checked_add(n),
                    "subtract" => acc.checked_sub(n),
                    "multiply" => acc.checked_mul(n),
                    _ => unreachable!(),
                }
                .ok_or_else(overflow)?;
            }
            Ok(num_value(acc))
        }
        "sort" => {
            check_args(args, &["input"])?;
            let arr = req_array(args, "input")?;
            let mut out = arr.clone();
            out.sort_by(total_cmp);
            Ok(Value::Array(out))
        }
        "reverse" => {
            check_args(args, &["input"])?;
            let arr = req_array(args, "input")?;
            let mut out = arr.clone();
            out.reverse();
            Ok(Value::Array(out))
        }
        "unique" => {
            check_args(args, &["input"])?;
            let arr = req_array(args, "input")?;
            let mut out: Vec<Value> = Vec::new();
            for v in arr {
                if !out.iter().any(|seen| structural_eq(seen, v)) {
                    out.push(v.clone());
                }
            }
            Ok(Value::Array(out))
        }
        "slice" => {
            check_args(args, &["input", "start", "end"])?;
            let arr = req_array(args, "input")?;
            let start_i = req_i64(args, "start")?;
            let end_i = opt_i64(args, "end")?;
            let n = arr.len() as i128;
            let mut start = start_i as i128;
            if start < 0 {
                start += n;
            }
            start = start.clamp(0, n);
            let mut end = match end_i {
                Some(e) => e as i128,
                None => n,
            };
            if end < 0 {
                end += n;
            }
            end = end.clamp(0, n);
            if start >= end {
                Ok(Value::Array(Vec::new()))
            } else {
                Ok(Value::Array(
                    arr[start as usize..end as usize].to_vec(),
                ))
            }
        }
        "flatten" => {
            check_args(args, &["input"])?;
            let arr = req_array(args, "input")?;
            let mut out = Vec::new();
            for v in arr {
                match v {
                    Value::Array(inner) => out.extend(inner.iter().cloned()),
                    _ => out.push(v.clone()),
                }
            }
            Ok(Value::Array(out))
        }
        "get" => {
            check_args(args, &["input", "key"])?;
            let obj = req_obj(args, "input")?;
            let key = req_str(args, "key")?;
            Ok(obj.get(key).cloned().unwrap_or(Value::Null))
        }
        "set" => {
            check_args(args, &["input", "key", "value"])?;
            let obj = req_obj(args, "input")?;
            let key = req_str(args, "key")?;
            let value = req_arg(args, "value")?.clone();
            let mut out = obj.clone();
            out.insert(key.to_string(), value);
            Ok(Value::Object(out))
        }
        "keys" => {
            check_args(args, &["input"])?;
            let obj = req_obj(args, "input")?;
            let out = sorted_keys(obj)
                .into_iter()
                .map(|k| Value::String(k.clone()))
                .collect();
            Ok(Value::Array(out))
        }
        "values" => {
            check_args(args, &["input"])?;
            let obj = req_obj(args, "input")?;
            let out = sorted_keys(obj)
                .into_iter()
                .map(|k| obj.get(k).cloned().unwrap_or(Value::Null))
                .collect();
            Ok(Value::Array(out))
        }
        "merge" => {
            check_args(args, &["a", "b"])?;
            let a = req_obj(args, "a")?;
            let b = req_obj(args, "b")?;
            let mut out = a.clone();
            for (k, v) in b {
                out.insert(k.clone(), v.clone());
            }
            Ok(Value::Object(out))
        }
        "pick" => {
            check_args(args, &["input", "keys"])?;
            let obj = req_obj(args, "input")?;
            let keys = req_array(args, "keys")?;
            let mut wanted: Vec<&str> = Vec::new();
            for k in keys {
                match k {
                    Value::String(s) => wanted.push(s.as_str()),
                    _ => return Err(invalid_arg("pick keys must be strings")),
                }
            }
            wanted.sort_by(|a, b| a.as_bytes().cmp(b.as_bytes()));
            wanted.dedup();
            let mut out = Map::new();
            for key in wanted {
                out.insert(
                    key.to_string(),
                    obj.get(key).cloned().unwrap_or(Value::Null),
                );
            }
            Ok(Value::Object(out))
        }
        "omit" => {
            check_args(args, &["input", "keys"])?;
            let obj = req_obj(args, "input")?;
            let keys = req_array(args, "keys")?;
            let mut removed: HashSet<&str> = HashSet::new();
            for k in keys {
                match k {
                    Value::String(s) => {
                        removed.insert(s.as_str());
                    }
                    _ => return Err(invalid_arg("omit keys must be strings")),
                }
            }
            let mut out = Map::new();
            for (key, value) in obj {
                if !removed.contains(key.as_str()) {
                    out.insert(key.clone(), value.clone());
                }
            }
            Ok(Value::Object(out))
        }
        "constant" => {
            check_args(args, &["value"])?;
            Ok(req_arg(args, "value")?.clone())
        }
        "equals" => {
            check_args(args, &["a", "b"])?;
            let a = req_arg(args, "a")?;
            let b = req_arg(args, "b")?;
            Ok(Value::Bool(structural_eq(a, b)))
        }
        "compare" => {
            check_args(args, &["a", "b", "op"])?;
            let a = req_arg(args, "a")?;
            let b = req_arg(args, "b")?;
            let cmp_op = req_str(args, "op")?;
            let ord = total_cmp(a, b);
            let result = match cmp_op {
                "gt" => ord == Ordering::Greater,
                "gte" => ord != Ordering::Less,
                "lt" => ord == Ordering::Less,
                "lte" => ord != Ordering::Greater,
                _ => {
                    return Err(invalid_arg(
                        "compare op must be gt, gte, lt or lte",
                    ))
                }
            };
            Ok(Value::Bool(result))
        }
        "and" => {
            check_args(args, &["a", "b"])?;
            let a = truthy(req_arg(args, "a")?);
            let b = truthy(req_arg(args, "b")?);
            Ok(Value::Bool(a && b))
        }
        "or" => {
            check_args(args, &["a", "b"])?;
            let a = truthy(req_arg(args, "a")?);
            let b = truthy(req_arg(args, "b")?);
            Ok(Value::Bool(a || b))
        }
        "not" => {
            check_args(args, &["a"])?;
            Ok(Value::Bool(!truthy(req_arg(args, "a")?)))
        }
        "type" => {
            check_args(args, &["value"])?;
            Ok(Value::String(
                type_name(req_arg(args, "value")?).to_string(),
            ))
        }
        "fail_until" => {
            check_args(args, &["failures", "value"])?;
            let failures = req_i64(args, "failures")?;
            if !(0..=10).contains(&failures) {
                return Err(invalid_arg("fail_until failures must be 0..10"));
            }
            let value = req_arg(args, "value")?.clone();
            if (attempt as i64) <= failures {
                Err(WfError::new(
                    "TRANSIENT_FAILURE",
                    format!("transient failure on attempt {}", attempt),
                ))
            } else {
                Ok(value)
            }
        }
        other => Err(invalid_arg(format!("unknown operation '{}'", other))),
    }
}

fn response(
    status: &str,
    results: &Map<String, Value>,
    execution_order: &[String],
    skipped: &[String],
    error: Option<&WfError>,
) -> Value {
    let mut obj = Map::new();
    obj.insert("status".to_string(), Value::String(status.to_string()));
    obj.insert("results".to_string(), Value::Object(results.clone()));
    obj.insert(
        "execution_order".to_string(),
        Value::Array(
            execution_order
                .iter()
                .map(|s| Value::String(s.clone()))
                .collect(),
        ),
    );
    obj.insert(
        "skipped".to_string(),
        Value::Array(skipped.iter().map(|s| Value::String(s.clone())).collect()),
    );
    let error_val = match error {
        None => Value::Null,
        Some(e) => {
            let mut em = Map::new();
            em.insert("code".to_string(), Value::String(e.code.clone()));
            em.insert(
                "step".to_string(),
                match &e.step {
                    Some(s) => Value::String(s.clone()),
                    None => Value::Null,
                },
            );
            em.insert("message".to_string(), Value::String(e.message.clone()));
            Value::Object(em)
        }
    };
    obj.insert("error".to_string(), error_val);
    Value::Object(obj)
}

fn execute_workflow(steps: &[Step]) -> (Value, i32) {
    let closures = compute_closures(steps);
    let mut st = RunState {
        results: Map::new(),
        execution_order: Vec::new(),
        skipped: Vec::new(),
        skipped_set: HashSet::new(),
        finished: HashSet::new(),
        closures,
    };

    for _ in 0..steps.len() {
        let mut chosen: Option<&Step> = None;
        for step in steps {
            if st.finished.contains(&step.id) {
                continue;
            }
            if step
                .depends_on
                .iter()
                .all(|d| st.finished.contains(d))
            {
                chosen = Some(step);
                break;
            }
        }
        let step = match chosen {
            Some(s) => s,
            None => {
                let e = WfError::new(
                    "INTERNAL_ERROR",
                    "no executable step (dependency scheduling failure)",
                );
                return (
                    response(
                        "failed",
                        &st.results,
                        &st.execution_order,
                        &st.skipped,
                        Some(&e),
                    ),
                    3,
                );
            }
        };

        let should_run = match &step.when {
            None => true,
            Some(cond) => match eval_condition(cond, step, &st) {
                Ok(b) => b,
                Err(e) => {
                    let e = e.at(&step.id);
                    return (
                        response(
                            "failed",
                            &st.results,
                            &st.execution_order,
                            &st.skipped,
                            Some(&e),
                        ),
                        1,
                    );
                }
            },
        };

        if !should_run {
            st.results.insert(step.id.clone(), Value::Null);
            st.execution_order.push(step.id.clone());
            st.skipped.push(step.id.clone());
            st.skipped_set.insert(step.id.clone());
            st.finished.insert(step.id.clone());
            continue;
        }

        let resolved_args = match resolve_value(
            &Value::Object(step.args.clone()),
            step,
            &st,
        ) {
            Ok(Value::Object(m)) => m,
            Ok(_) => Map::new(),
            Err(e) => {
                let e = e.at(&step.id);
                return (
                    response(
                        "failed",
                        &st.results,
                        &st.execution_order,
                        &st.skipped,
                        Some(&e),
                    ),
                    1,
                );
            }
        };

        let mut success: Option<Value> = None;
        let mut final_error: Option<WfError> = None;
        for attempt in 1..=step.max_attempts {
            match execute_op(&step.op, &resolved_args, attempt) {
                Ok(v) => {
                    success = Some(v);
                    break;
                }
                Err(e) => {
                    if e.code == "TRANSIENT_FAILURE" {
                        if attempt < step.max_attempts {
                            continue;
                        }
                        final_error = Some(WfError {
                            code: "STEP_FAILED".to_string(),
                            step: Some(step.id.clone()),
                            message: format!(
                                "step '{}' failed after {} attempt(s)",
                                step.id, attempt
                            ),
                        });
                        break;
                    } else {
                        final_error = Some(e.at(&step.id));
                        break;
                    }
                }
            }
        }

        match success {
            Some(v) => {
                st.results.insert(step.id.clone(), v);
                st.execution_order.push(step.id.clone());
                st.finished.insert(step.id.clone());
            }
            None => {
                let e = final_error.unwrap_or_else(|| {
                    WfError {
                        code: "STEP_FAILED".to_string(),
                        step: Some(step.id.clone()),
                        message: "step failed".to_string(),
                    }
                });
                return (
                    response(
                        "failed",
                        &st.results,
                        &st.execution_order,
                        &st.skipped,
                        Some(&e),
                    ),
                    1,
                );
            }
        }
    }

    (
        response(
            "completed",
            &st.results,
            &st.execution_order,
            &st.skipped,
            None,
        ),
        0,
    )
}

fn print_response(resp: &Value) -> i32 {
    match serde_json::to_string(resp) {
        Ok(text) => {
            println!("{}", text);
            0
        }
        Err(_) => {
            println!(
                "{{\"status\":\"failed\",\"results\":{{}},\"execution_order\":[],\"skipped\":[],\"error\":{{\"code\":\"INTERNAL_ERROR\",\"step\":null,\"message\":\"serialization failure\"}}}}"
            );
            3
        }
    }
}

fn empty_failure(code: &str, step: Option<String>, message: impl Into<String>) -> Value {
    let e = WfError {
        code: code.to_string(),
        step,
        message: message.into(),
    };
    response(
        "failed",
        &Map::new(),
        &[],
        &[],
        Some(&e),
    )
}

fn real_main() -> i32 {
    let argv: Vec<String> = std::env::args().skip(1).collect();
    if argv.len() != 2 || argv[0] != "run" {
        let resp = empty_failure(
            "INVALID_WORKFLOW",
            None,
            "usage: workflow run <workflow.json> | workflow run -",
        );
        print_response(&resp);
        return 2;
    }

    let source = &argv[1];
    let text = if source == "-" {
        let mut buf = String::new();
        match std::io::stdin().read_to_string(&mut buf) {
            Ok(_) => buf,
            Err(e) => {
                let resp = empty_failure(
                    "INVALID_WORKFLOW",
                    None,
                    format!("failed to read stdin: {}", e),
                );
                print_response(&resp);
                return 2;
            }
        }
    } else {
        match std::fs::read_to_string(source) {
            Ok(s) => s,
            Err(e) => {
                let resp = empty_failure(
                    "INVALID_WORKFLOW",
                    None,
                    format!("failed to read '{}': {}", source, e),
                );
                print_response(&resp);
                return 2;
            }
        }
    };

    let doc: Value = match serde_json::from_str(&text) {
        Ok(v) => v,
        Err(e) => {
            let resp = empty_failure(
                "INVALID_WORKFLOW",
                None,
                format!("invalid JSON: {}", e),
            );
            print_response(&resp);
            return 2;
        }
    };

    let steps = match validate(&doc) {
        Ok(s) => s,
        Err(e) => {
            let resp = response(
                "failed",
                &Map::new(),
                &[],
                &[],
                Some(&e),
            );
            print_response(&resp);
            return 2;
        }
    };

    let (resp, exit_code) = execute_workflow(&steps);
    print_response(&resp);
    exit_code
}

fn main() {
    let code = std::panic::catch_unwind(real_main).unwrap_or_else(|_| {
        let resp = empty_failure(
            "INTERNAL_ERROR",
            None,
            "internal panic",
        );
        match serde_json::to_string(&resp) {
            Ok(text) => println!("{}", text),
            Err(_) => println!(
                "{{\"status\":\"failed\",\"results\":{{}},\"execution_order\":[],\"skipped\":[],\"error\":{{\"code\":\"INTERNAL_ERROR\",\"step\":null,\"message\":\"internal panic\"}}}}"
            ),
        }
        3
    });
    std::process::exit(code);
}
