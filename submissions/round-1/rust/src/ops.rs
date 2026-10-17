use std::cmp::Ordering;

use serde_json::{Map, Number, Value};

use crate::decimal::number_to_i64;
use crate::value_util::{
    canonical_key, compare_values, structural_eq, truthy, type_name,
};

#[derive(Debug)]
pub struct OpError {
    pub code: &'static str,
    pub message: String,
}

impl OpError {
    pub fn new(code: &'static str, message: impl Into<String>) -> Self {
        Self {
            code,
            message: message.into(),
        }
    }
}

fn invalid_arg(message: impl Into<String>) -> OpError {
    OpError::new("INVALID_ARG", message)
}

fn overflow(message: impl Into<String>) -> OpError {
    OpError::new("ARITHMETIC_OVERFLOW", message)
}

fn div_zero() -> OpError {
    OpError::new("DIVISION_BY_ZERO", "division by zero")
}

fn empty_array() -> OpError {
    OpError::new("EMPTY_ARRAY", "operation requires a non-empty array")
}

fn check_args(args: &Map<String, Value>, allowed: &[&str]) -> Result<(), OpError> {
    for key in args.keys() {
        if !allowed.contains(&key.as_str()) {
            return Err(invalid_arg(format!("unknown argument: {key}")));
        }
    }
    Ok(())
}

fn required<'a>(args: &'a Map<String, Value>, name: &str) -> Result<&'a Value, OpError> {
    args.get(name)
        .ok_or_else(|| invalid_arg(format!("missing argument: {name}")))
}

fn value_i64(value: &Value) -> Result<i64, OpError> {
    value
        .as_number()
        .and_then(number_to_i64)
        .ok_or_else(|| invalid_arg("argument must be an integer fitting in i64"))
}

fn req_i64(args: &Map<String, Value>, name: &str) -> Result<i64, OpError> {
    value_i64(required(args, name)?)
}

fn req_str<'a>(args: &'a Map<String, Value>, name: &str) -> Result<&'a str, OpError> {
    required(args, name)?
        .as_str()
        .ok_or_else(|| invalid_arg(format!("argument {name} must be a string")))
}

fn req_array<'a>(args: &'a Map<String, Value>, name: &str) -> Result<&'a Vec<Value>, OpError> {
    required(args, name)?
        .as_array()
        .ok_or_else(|| invalid_arg(format!("argument {name} must be an array")))
}

fn req_object<'a>(
    args: &'a Map<String, Value>,
    name: &str,
) -> Result<&'a Map<String, Value>, OpError> {
    required(args, name)?
        .as_object()
        .ok_or_else(|| invalid_arg(format!("argument {name} must be an object")))
}

fn binary_num(op: &str, a: i64, b: i64) -> Result<i64, OpError> {
    match op {
        "add" => a
            .checked_add(b)
            .ok_or_else(|| overflow("integer addition overflow")),
        "subtract" => a
            .checked_sub(b)
            .ok_or_else(|| overflow("integer subtraction overflow")),
        "multiply" => a
            .checked_mul(b)
            .ok_or_else(|| overflow("integer multiplication overflow")),
        "divide" => {
            if b == 0 {
                Err(div_zero())
            } else {
                a.checked_div(b)
                    .ok_or_else(|| overflow("integer division overflow"))
            }
        }
        "mod" => {
            if b == 0 {
                Err(div_zero())
            } else if b == -1 {
                // i64::MIN % -1 is mathematically zero, although the hardware
                // operation overflows.  The specification asks for the true
                // mathematical result.
                Ok(0)
            } else {
                a.checked_rem(b)
                    .ok_or_else(|| overflow("integer remainder overflow"))
            }
        }
        _ => Err(invalid_arg(format!("unsupported numeric operation: {op}"))),
    }
}

fn binary_is_supported(op: &str) -> bool {
    matches!(op, "add" | "subtract" | "multiply" | "divide" | "mod")
}

fn run_binary_args(op: &str, args: &Map<String, Value>) -> Result<Value, OpError> {
    check_args(args, &["a", "b"])?;
    let a = req_i64(args, "a")?;
    let b = req_i64(args, "b")?;
    Ok(Value::Number(Number::from(binary_num(op, a, b)?)))
}

fn compare_with_op(op: &str, a: &Value, b: &Value) -> Result<bool, OpError> {
    let ord = compare_values(a, b);
    match op {
        "gt" => Ok(ord == Ordering::Greater),
        "gte" => Ok(matches!(ord, Ordering::Greater | Ordering::Equal)),
        "lt" => Ok(ord == Ordering::Less),
        "lte" => Ok(matches!(ord, Ordering::Less | Ordering::Equal)),
        _ => Err(invalid_arg(format!("unsupported comparison: {op}"))),
    }
}

fn ascii_upper(s: &str) -> String {
    s.chars().map(|c| c.to_ascii_uppercase()).collect()
}

fn ascii_lower(s: &str) -> String {
    s.chars().map(|c| c.to_ascii_lowercase()).collect()
}

fn ascii_whitespace(c: char) -> bool {
    matches!(c, ' ' | '\t' | '\n' | '\r' | '\u{000c}' | '\u{000b}')
}

#[derive(Debug)]
enum Transform {
    Constant(Value),
    Binary(String, i64),
    Negate,
    Abs,
    Stringify,
    TypeName,
    OpForm(String, Map<String, Value>),
}

fn parse_transform(value: &Value) -> Result<Transform, OpError> {
    let obj = value
        .as_object()
        .ok_or_else(|| invalid_arg("transform must be an object"))?;
    let kind = obj
        .get("type")
        .and_then(Value::as_str)
        .ok_or_else(|| invalid_arg("transform type must be a string"))?;

    match kind {
        "constant" => {
            check_args(obj, &["type", "value"])?;
            let v = obj
                .get("value")
                .cloned()
                .ok_or_else(|| invalid_arg("constant transform requires value"))?;
            Ok(Transform::Constant(v))
        }
        "add" | "subtract" | "multiply" | "divide" | "mod" => {
            check_args(obj, &["type", "value"])?;
            let n = value_i64(
                obj.get("value")
                    .ok_or_else(|| invalid_arg("numeric transform requires value"))?,
            )?;
            Ok(Transform::Binary(kind.to_string(), n))
        }
        "negate" => {
            check_args(obj, &["type"])?;
            Ok(Transform::Negate)
        }
        "abs" => {
            check_args(obj, &["type"])?;
            Ok(Transform::Abs)
        }
        "stringify" => {
            check_args(obj, &["type"])?;
            Ok(Transform::Stringify)
        }
        "type" => {
            check_args(obj, &["type"])?;
            Ok(Transform::TypeName)
        }
        "op" => {
            check_args(obj, &["type", "op", "args"])?;
            let op = obj
                .get("op")
                .and_then(Value::as_str)
                .ok_or_else(|| invalid_arg("transform op must be a string"))?;
            if !binary_is_supported(op) {
                return Err(invalid_arg(format!(
                    "transform op not supported: {op}"
                )));
            }
            let args = obj
                .get("args")
                .and_then(Value::as_object)
                .ok_or_else(|| invalid_arg("transform args must be an object"))?;
            Ok(Transform::OpForm(op.to_string(), args.clone()))
        }
        _ => Err(invalid_arg(format!("unknown transform type: {kind}"))),
    }
}

fn is_self_marker(value: &Value) -> bool {
    match value.as_object() {
        Some(m) if m.len() == 1 => matches!(m.get("$self"), Some(Value::Bool(true))),
        _ => false,
    }
}

fn substitute_self(value: &Value, element: &Value) -> Value {
    if is_self_marker(value) {
        return element.clone();
    }
    match value {
        Value::Array(items) => Value::Array(
            items
                .iter()
                .map(|item| substitute_self(item, element))
                .collect(),
        ),
        Value::Object(map) => {
            let mut out = Map::new();
            for (key, item) in map {
                out.insert(key.clone(), substitute_self(item, element));
            }
            Value::Object(out)
        }
        _ => value.clone(),
    }
}

impl Transform {
    fn apply(&self, element: &Value) -> Result<Value, OpError> {
        match self {
            Transform::Constant(v) => Ok(v.clone()),
            Transform::Binary(op, n) => {
                let a = value_i64(element)?;
                Ok(Value::Number(Number::from(binary_num(op, a, *n)?)))
            }
            Transform::Negate => {
                let n = value_i64(element)?;
                n.checked_neg()
                    .map(Number::from)
                    .map(Value::Number)
                    .ok_or_else(|| overflow("integer negation overflow"))
            }
            Transform::Abs => {
                let n = value_i64(element)?;
                n.checked_abs()
                    .map(Number::from)
                    .map(Value::Number)
                    .ok_or_else(|| overflow("integer absolute value overflow"))
            }
            Transform::Stringify => Ok(Value::String(
                serde_json::to_string(element)
                    .map_err(|e| OpError::new("INVALID_ARG", e.to_string()))?,
            )),
            Transform::TypeName => Ok(Value::String(type_name(element).to_string())),
            Transform::OpForm(op, args) => {
                let mut resolved = Map::new();
                for (key, value) in args {
                    resolved.insert(key.clone(), substitute_self(value, element));
                }
                run_binary_args(op, &resolved)
            }
        }
    }
}

#[derive(Debug)]
enum Predicate {
    Even,
    Odd,
    Gt(i64),
    Gte(i64),
    Lt(i64),
    Lte(i64),
    Eq(i64),
    Ne(i64),
    Truthy,
    Falsy,
    TypeIs(String),
    StringContains(String),
}

fn parse_predicate(value: &Value) -> Result<Predicate, OpError> {
    let obj = value
        .as_object()
        .ok_or_else(|| invalid_arg("predicate must be an object"))?;
    let kind = obj
        .get("type")
        .and_then(Value::as_str)
        .ok_or_else(|| invalid_arg("predicate type must be a string"))?;

    match kind {
        "even" => {
            check_args(obj, &["type"])?;
            Ok(Predicate::Even)
        }
        "odd" => {
            check_args(obj, &["type"])?;
            Ok(Predicate::Odd)
        }
        "gt" | "gte" | "lt" | "lte" | "eq" | "ne" => {
            check_args(obj, &["type", "value"])?;
            let n = value_i64(
                obj.get("value")
                    .ok_or_else(|| invalid_arg("comparison predicate requires value"))?,
            )?;
            Ok(match kind {
                "gt" => Predicate::Gt(n),
                "gte" => Predicate::Gte(n),
                "lt" => Predicate::Lt(n),
                "lte" => Predicate::Lte(n),
                "eq" => Predicate::Eq(n),
                _ => Predicate::Ne(n),
            })
        }
        "truthy" => {
            check_args(obj, &["type"])?;
            Ok(Predicate::Truthy)
        }
        "falsy" => {
            check_args(obj, &["type"])?;
            Ok(Predicate::Falsy)
        }
        "type_is" => {
            // The spec writes the payload as `"type": "<type>"`, which would
            // collide with the discriminator key.  Accept the natural spellings
            // used by real documents.
            let payload_keys = [
                "value",
                "type_name",
                "type_value",
                "value_type",
                "name",
                "target",
                "target_type",
                "expected",
                "expected_type",
            ];
            for key in obj.keys() {
                if key != "type" && !payload_keys.contains(&key.as_str()) {
                    return Err(invalid_arg(format!(
                        "unknown predicate field: {key}"
                    )));
                }
            }
            let name = payload_keys
                .iter()
                .find_map(|key| obj.get(*key).and_then(Value::as_str))
                .ok_or_else(|| invalid_arg("type_is requires a type name"))?;
            Ok(Predicate::TypeIs(name.to_string()))
        }
        "string_contains" => {
            check_args(obj, &["type", "value"])?;
            let needle = obj
                .get("value")
                .and_then(Value::as_str)
                .ok_or_else(|| invalid_arg("string_contains requires a string value"))?;
            Ok(Predicate::StringContains(needle.to_string()))
        }
        // The spec writes the type_is payload as a second `"type"` key.  A
        // normal JSON parser keeps only the last occurrence, leaving an object
        // that looks like {"type": "number"}; treat that unambiguous
        // single-field form as type_is as well.
        "null" | "boolean" | "number" | "string" | "array" | "object" if obj.len() == 1 => {
            Ok(Predicate::TypeIs(kind.to_string()))
        }
        _ => Err(invalid_arg(format!("unknown predicate type: {kind}"))),
    }
}

impl Predicate {
    fn test(&self, element: &Value) -> Result<bool, OpError> {
        match self {
            Predicate::Even => {
                let n = value_i64(element)?;
                Ok(n % 2 == 0)
            }
            Predicate::Odd => {
                let n = value_i64(element)?;
                Ok(n % 2 != 0)
            }
            Predicate::Gt(n) => Ok(compare_values(element, &Value::Number(Number::from(*n)))
                == Ordering::Greater),
            Predicate::Gte(n) => Ok(matches!(
                compare_values(element, &Value::Number(Number::from(*n))),
                Ordering::Greater | Ordering::Equal
            )),
            Predicate::Lt(n) => {
                Ok(compare_values(element, &Value::Number(Number::from(*n))) == Ordering::Less)
            }
            Predicate::Lte(n) => Ok(matches!(
                compare_values(element, &Value::Number(Number::from(*n))),
                Ordering::Less | Ordering::Equal
            )),
            Predicate::Eq(n) => Ok(structural_eq(
                element,
                &Value::Number(Number::from(*n)),
            )),
            Predicate::Ne(n) => Ok(!structural_eq(
                element,
                &Value::Number(Number::from(*n)),
            )),
            Predicate::Truthy => Ok(truthy(element)),
            Predicate::Falsy => Ok(!truthy(element)),
            Predicate::TypeIs(name) => Ok(type_name(element) == name),
            Predicate::StringContains(needle) => {
                Ok(element.as_str().map(|s| s.contains(needle)).unwrap_or(false))
            }
        }
    }
}

pub fn execute_op(op: &str, args: &Map<String, Value>, attempt: u32) -> Result<Value, OpError> {
    match op {
        "add" | "subtract" | "multiply" | "divide" | "mod" => {
            check_args(args, &["a", "b"])?;
            run_binary_args(op, args)
        }
        "sum" => {
            check_args(args, &["input"])?;
            let input = req_array(args, "input")?;
            let mut total: i64 = 0;
            for item in input {
                let n = value_i64(item)?;
                total = total
                    .checked_add(n)
                    .ok_or_else(|| overflow("integer sum overflow"))?;
            }
            Ok(Value::Number(Number::from(total)))
        }
        "min" | "max" => {
            check_args(args, &["input"])?;
            let input = req_array(args, "input")?;
            if input.is_empty() {
                return Err(empty_array());
            }
            let mut best = value_i64(&input[0])?;
            for item in &input[1..] {
                let n = value_i64(item)?;
                if (op == "min" && n < best) || (op == "max" && n > best) {
                    best = n;
                }
            }
            Ok(Value::Number(Number::from(best)))
        }
        "range" => {
            check_args(args, &["start", "end"])?;
            let start = req_i64(args, "start")?;
            let end = req_i64(args, "end")?;
            let mut out = Vec::new();
            if start <= end {
                let mut current = start;
                loop {
                    out.push(Value::Number(Number::from(current)));
                    if current == end {
                        break;
                    }
                    current += 1;
                }
            }
            Ok(Value::Array(out))
        }
        "concat" => {
            check_args(args, &["a", "b"])?;
            let a = req_str(args, "a")?;
            let b = req_str(args, "b")?;
            Ok(Value::String(format!("{a}{b}")))
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
                input.split(sep).map(|s| Value::String(s.to_string())).collect()
            };
            Ok(Value::Array(parts))
        }
        "join" => {
            check_args(args, &["input", "sep"])?;
            let input = req_array(args, "input")?;
            let sep = req_str(args, "sep")?;
            let mut parts = Vec::with_capacity(input.len());
            for item in input {
                let s = item
                    .as_str()
                    .ok_or_else(|| invalid_arg("join input must contain only strings"))?;
                parts.push(s);
            }
            Ok(Value::String(parts.join(sep)))
        }
        "uppercase" => {
            check_args(args, &["input"])?;
            Ok(Value::String(ascii_upper(req_str(args, "input")?)))
        }
        "lowercase" => {
            check_args(args, &["input"])?;
            Ok(Value::String(ascii_lower(req_str(args, "input")?)))
        }
        "trim" => {
            check_args(args, &["input"])?;
            Ok(Value::String(
                req_str(args, "input")?
                    .trim_matches(ascii_whitespace)
                    .to_string(),
            ))
        }
        "replace" => {
            check_args(args, &["input", "search", "replace"])?;
            let input = req_str(args, "input")?;
            let search = req_str(args, "search")?;
            let replacement = req_str(args, "replace")?;
            Ok(Value::String(input.replace(search, replacement)))
        }
        "length" => {
            check_args(args, &["input"])?;
            let count = req_str(args, "input")?.chars().count() as i64;
            Ok(Value::Number(Number::from(count)))
        }
        "map" => {
            check_args(args, &["input", "transform"])?;
            let input = req_array(args, "input")?;
            let transform = parse_transform(required(args, "transform")?)?;
            let mut out = Vec::with_capacity(input.len());
            for item in input {
                out.push(transform.apply(item)?);
            }
            Ok(Value::Array(out))
        }
        "filter" => {
            check_args(args, &["input", "predicate"])?;
            let input = req_array(args, "input")?;
            let predicate = parse_predicate(required(args, "predicate")?)?;
            let mut out = Vec::new();
            for item in input {
                if predicate.test(item)? {
                    out.push(item.clone());
                }
            }
            Ok(Value::Array(out))
        }
        "reduce" => {
            check_args(args, &["input", "op", "initial"])?;
            let input = req_array(args, "input")?;
            let op_name = req_str(args, "op")?;
            if !matches!(op_name, "add" | "subtract" | "multiply") {
                return Err(invalid_arg(format!("unsupported reduce op: {op_name}")));
            }
            let mut acc = match args.get("initial") {
                Some(v) => value_i64(v)?,
                None => {
                    if op_name == "multiply" {
                        1
                    } else {
                        0
                    }
                }
            };
            for item in input {
                let n = value_i64(item)?;
                acc = binary_num(op_name, acc, n)?;
            }
            Ok(Value::Number(Number::from(acc)))
        }
        "sort" => {
            check_args(args, &["input"])?;
            let input = req_array(args, "input")?;
            let mut out = input.clone();
            out.sort_by(compare_values);
            Ok(Value::Array(out))
        }
        "reverse" => {
            check_args(args, &["input"])?;
            let input = req_array(args, "input")?;
            let mut out = input.clone();
            out.reverse();
            Ok(Value::Array(out))
        }
        "unique" => {
            check_args(args, &["input"])?;
            let input = req_array(args, "input")?;
            let mut seen = std::collections::HashSet::with_capacity(input.len());
            let mut out: Vec<Value> = Vec::with_capacity(input.len());
            for item in input {
                if seen.insert(canonical_key(item)) {
                    out.push(item.clone());
                }
            }
            Ok(Value::Array(out))
        }
        "slice" => {
            check_args(args, &["input", "start", "end"])?;
            let input = req_array(args, "input")?;
            let start = req_i64(args, "start")?;
            let end = match args.get("end") {
                Some(v) => Some(value_i64(v)?),
                None => None,
            };
            let n = input.len() as i128;
            let normalize = |index: i64| -> i128 {
                let i = index as i128;
                if i < 0 {
                    (n + i).max(0)
                } else {
                    i.min(n)
                }
            };
            let from = normalize(start);
            let to = end.map(normalize).unwrap_or(n);
            if to <= from {
                Ok(Value::Array(Vec::new()))
            } else {
                Ok(Value::Array(
                    input[from as usize..to as usize].to_vec(),
                ))
            }
        }
        "flatten" => {
            check_args(args, &["input"])?;
            let input = req_array(args, "input")?;
            let mut out = Vec::new();
            for item in input {
                match item {
                    Value::Array(inner) => out.extend(inner.iter().cloned()),
                    other => out.push(other.clone()),
                }
            }
            Ok(Value::Array(out))
        }
        "get" => {
            check_args(args, &["input", "key"])?;
            let input = req_object(args, "input")?;
            let key = req_str(args, "key")?;
            Ok(input.get(key).cloned().unwrap_or(Value::Null))
        }
        "set" => {
            check_args(args, &["input", "key", "value"])?;
            let input = req_object(args, "input")?;
            let key = req_str(args, "key")?;
            let value = required(args, "value")?.clone();
            let mut out = input.clone();
            out.insert(key.to_string(), value);
            Ok(Value::Object(out))
        }
        "keys" => {
            check_args(args, &["input"])?;
            let input = req_object(args, "input")?;
            let keys = crate::value_util::sorted_string_keys(input)
                .into_iter()
                .map(Value::String)
                .collect();
            Ok(Value::Array(keys))
        }
        "values" => {
            check_args(args, &["input"])?;
            let input = req_object(args, "input")?;
            let values = crate::value_util::sorted_string_keys(input)
                .into_iter()
                .map(|key| input[&key].clone())
                .collect();
            Ok(Value::Array(values))
        }
        "merge" => {
            check_args(args, &["a", "b"])?;
            let a = req_object(args, "a")?;
            let b = req_object(args, "b")?;
            let mut out = a.clone();
            for (key, value) in b {
                out.insert(key.clone(), value.clone());
            }
            Ok(Value::Object(out))
        }
        "pick" => {
            check_args(args, &["input", "keys"])?;
            let input = req_object(args, "input")?;
            let keys = req_array(args, "keys")?;
            let mut out = Map::new();
            for key in keys {
                let key = key
                    .as_str()
                    .ok_or_else(|| invalid_arg("pick keys must be strings"))?;
                out.insert(
                    key.to_string(),
                    input.get(key).cloned().unwrap_or(Value::Null),
                );
            }
            Ok(Value::Object(out))
        }
        "omit" => {
            check_args(args, &["input", "keys"])?;
            let input = req_object(args, "input")?;
            let keys = req_array(args, "keys")?;
            let mut out = input.clone();
            for key in keys {
                let key = key
                    .as_str()
                    .ok_or_else(|| invalid_arg("omit keys must be strings"))?;
                out.remove(key);
            }
            Ok(Value::Object(out))
        }
        "constant" => {
            check_args(args, &["value"])?;
            Ok(required(args, "value")?.clone())
        }
        "equals" => {
            check_args(args, &["a", "b"])?;
            Ok(Value::Bool(structural_eq(
                required(args, "a")?,
                required(args, "b")?,
            )))
        }
        "compare" => {
            check_args(args, &["a", "b", "op"])?;
            let a = required(args, "a")?;
            let b = required(args, "b")?;
            let op_name = req_str(args, "op")?;
            Ok(Value::Bool(compare_with_op(op_name, a, b)?))
        }
        "and" => {
            check_args(args, &["a", "b"])?;
            let a = required(args, "a")?
                .as_bool()
                .ok_or_else(|| invalid_arg("and requires boolean a"))?;
            let b = required(args, "b")?
                .as_bool()
                .ok_or_else(|| invalid_arg("and requires boolean b"))?;
            Ok(Value::Bool(a && b))
        }
        "or" => {
            check_args(args, &["a", "b"])?;
            let a = required(args, "a")?
                .as_bool()
                .ok_or_else(|| invalid_arg("or requires boolean a"))?;
            let b = required(args, "b")?
                .as_bool()
                .ok_or_else(|| invalid_arg("or requires boolean b"))?;
            Ok(Value::Bool(a || b))
        }
        "not" => {
            check_args(args, &["a"])?;
            let a = required(args, "a")?
                .as_bool()
                .ok_or_else(|| invalid_arg("not requires boolean a"))?;
            Ok(Value::Bool(!a))
        }
        "type" => {
            check_args(args, &["value"])?;
            Ok(Value::String(
                type_name(required(args, "value")?).to_string(),
            ))
        }
        "fail_until" => {
            check_args(args, &["failures", "value"])?;
            let failures = req_i64(args, "failures")?;
            if !(0..=10).contains(&failures) {
                return Err(invalid_arg("failures must be between 0 and 10"));
            }
            let value = required(args, "value")?.clone();
            if (attempt as i64) <= failures {
                Err(OpError::new(
                    "TRANSIENT_FAILURE",
                    "fail_until transient failure",
                ))
            } else {
                Ok(value)
            }
        }
        _ => Err(invalid_arg(format!("unknown operation: {op}"))),
    }
}

