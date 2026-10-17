//! The closed set of operations (SPEC §4) plus map transforms and filter
//! predicates.

use crate::error::{
    division_by_zero, empty_array, invalid_arg, overflow, WfError, TRANSIENT_FAILURE,
};
use crate::value::{
    cmp_values, equals, integer_literal, is_truthy, sorted_entries, type_name,
};
use serde_json::{json, Map, Value};
use std::cmp::Ordering;

const TYPE_NAMES: &[&str] = &["null", "boolean", "number", "string", "array", "object"];

fn as_object(args: &Value) -> Result<&Map<String, Value>, WfError> {
    args.as_object()
        .ok_or_else(|| invalid_arg("operation arguments must be an object"))
}

fn check_keys(map: &Map<String, Value>, allowed: &[&str]) -> Result<(), WfError> {
    for key in map.keys() {
        if !allowed.contains(&key.as_str()) {
            return Err(invalid_arg(format!("unknown argument '{}'", key)));
        }
    }
    Ok(())
}

fn required<'a>(map: &'a Map<String, Value>, key: &str) -> Result<&'a Value, WfError> {
    map.get(key)
        .ok_or_else(|| invalid_arg(format!("missing required argument '{}'", key)))
}

fn required_i64(map: &Map<String, Value>, key: &str) -> Result<i64, WfError> {
    let v = required(map, key)?;
    integer_literal(v).ok_or_else(|| {
        invalid_arg(format!(
            "argument '{}' must be an integer literal that fits in i64",
            key
        ))
    })
}

fn optional_i64(map: &Map<String, Value>, key: &str) -> Result<Option<i64>, WfError> {
    match map.get(key) {
        None => Ok(None),
        Some(v) => integer_literal(v).map(Some).ok_or_else(|| {
            invalid_arg(format!(
                "argument '{}' must be an integer literal that fits in i64",
                key
            ))
        }),
    }
}

fn required_str<'a>(map: &'a Map<String, Value>, key: &str) -> Result<&'a str, WfError> {
    required(map, key)?
        .as_str()
        .ok_or_else(|| invalid_arg(format!("argument '{}' must be a string", key)))
}

fn required_array<'a>(map: &'a Map<String, Value>, key: &str) -> Result<&'a Vec<Value>, WfError> {
    required(map, key)?
        .as_array()
        .ok_or_else(|| invalid_arg(format!("argument '{}' must be an array", key)))
}

fn required_object<'a>(
    map: &'a Map<String, Value>,
    key: &str,
) -> Result<&'a Map<String, Value>, WfError> {
    required(map, key)?
        .as_object()
        .ok_or_else(|| invalid_arg(format!("argument '{}' must be an object", key)))
}

fn i64_array(map: &Map<String, Value>, key: &str) -> Result<Vec<i64>, WfError> {
    let arr = required_array(map, key)?;
    let mut out = Vec::with_capacity(arr.len());
    for item in arr {
        out.push(integer_literal(item).ok_or_else(|| {
            invalid_arg(format!("'{}' must be an array of integers", key))
        })?);
    }
    Ok(out)
}

fn string_array(map: &Map<String, Value>, key: &str) -> Result<Vec<String>, WfError> {
    let arr = required_array(map, key)?;
    let mut out = Vec::with_capacity(arr.len());
    for item in arr {
        out.push(
            item.as_str()
                .ok_or_else(|| invalid_arg(format!("'{}' must be an array of strings", key)))?
                .to_string(),
        );
    }
    Ok(out)
}

fn checked_binary(op: &str, a: i64, b: i64) -> Result<i64, WfError> {
    match op {
        "add" => a
            .checked_add(b)
            .ok_or_else(|| overflow("addition overflowed i64")),
        "subtract" => a
            .checked_sub(b)
            .ok_or_else(|| overflow("subtraction overflowed i64")),
        "multiply" => a
            .checked_mul(b)
            .ok_or_else(|| overflow("multiplication overflowed i64")),
        "divide" => {
            if b == 0 {
                Err(division_by_zero("division by zero"))
            } else if a == i64::MIN && b == -1 {
                Err(overflow("division overflowed i64"))
            } else {
                Ok(a / b)
            }
        }
        "mod" => {
            if b == 0 {
                Err(division_by_zero("modulo by zero"))
            } else if a == i64::MIN && b == -1 {
                Err(overflow("modulo overflowed i64"))
            } else {
                Ok(a % b)
            }
        }
        _ => Err(invalid_arg(format!("unsupported binary operation '{}'", op))),
    }
}

pub fn run_op(op: &str, args: &Value, attempt_no: u32) -> Result<Value, WfError> {
    let map = as_object(args)?;
    match op {
        // ---------------------------------------------------------------- math
        "add" | "subtract" | "multiply" | "divide" | "mod" => {
            check_keys(map, &["a", "b"])?;
            let a = required_i64(map, "a")?;
            let b = required_i64(map, "b")?;
            Ok(json!(checked_binary(op, a, b)?))
        }
        "sum" => {
            check_keys(map, &["input"])?;
            let values = i64_array(map, "input")?;
            let mut acc: i64 = 0;
            for v in values {
                acc = acc
                    .checked_add(v)
                    .ok_or_else(|| overflow("sum overflowed i64"))?;
            }
            Ok(json!(acc))
        }
        "min" => {
            check_keys(map, &["input"])?;
            let values = i64_array(map, "input")?;
            if values.is_empty() {
                return Err(empty_array("min of an empty array"));
            }
            let mut m = values[0];
            for v in values.into_iter().skip(1) {
                if v < m {
                    m = v;
                }
            }
            Ok(json!(m))
        }
        "max" => {
            check_keys(map, &["input"])?;
            let values = i64_array(map, "input")?;
            if values.is_empty() {
                return Err(empty_array("max of an empty array"));
            }
            let mut m = values[0];
            for v in values.into_iter().skip(1) {
                if v > m {
                    m = v;
                }
            }
            Ok(json!(m))
        }
        "range" => {
            check_keys(map, &["start", "end"])?;
            let start = required_i64(map, "start")?;
            let end = required_i64(map, "end")?;
            let mut out = Vec::new();
            if start <= end {
                let mut cur = start;
                loop {
                    out.push(json!(cur));
                    if cur == end {
                        break;
                    }
                    cur = cur
                        .checked_add(1)
                        .ok_or_else(|| overflow("range overflowed i64"))?;
                }
            }
            Ok(Value::Array(out))
        }

        // ------------------------------------------------------------- strings
        "concat" => {
            check_keys(map, &["a", "b"])?;
            let a = required_str(map, "a")?;
            let b = required_str(map, "b")?;
            Ok(Value::String(format!("{}{}", a, b)))
        }
        "split" => {
            check_keys(map, &["input", "sep"])?;
            let input = required_str(map, "input")?;
            let sep = required_str(map, "sep")?;
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
            check_keys(map, &["input", "sep"])?;
            let parts = string_array(map, "input")?;
            let sep = required_str(map, "sep")?;
            Ok(Value::String(parts.join(sep)))
        }
        "uppercase" => {
            check_keys(map, &["input"])?;
            let input = required_str(map, "input")?;
            Ok(Value::String(input.to_ascii_uppercase()))
        }
        "lowercase" => {
            check_keys(map, &["input"])?;
            let input = required_str(map, "input")?;
            Ok(Value::String(input.to_ascii_lowercase()))
        }
        "trim" => {
            check_keys(map, &["input"])?;
            let input = required_str(map, "input")?;
            let trimmed = input.trim_matches(|c: char| {
                matches!(c, ' ' | '\t' | '\n' | '\r' | '\u{0C}' | '\u{0B}')
            });
            Ok(Value::String(trimmed.to_string()))
        }
        "replace" => {
            check_keys(map, &["input", "search", "replace"])?;
            let input = required_str(map, "input")?;
            let search = required_str(map, "search")?;
            let replacement = required_str(map, "replace")?;
            if search.is_empty() {
                return Err(invalid_arg("replace 'search' cannot be empty"));
            }
            Ok(Value::String(input.replace(search, replacement)))
        }
        "length" => {
            check_keys(map, &["input"])?;
            let input = required_str(map, "input")?;
            Ok(json!(input.chars().count() as i64))
        }

        // -------------------------------------------------------------- arrays
        "map" => {
            check_keys(map, &["input", "transform"])?;
            let input = required_array(map, "input")?;
            let transform = required(map, "transform")?;
            validate_transform(transform)?;
            let mut out = Vec::with_capacity(input.len());
            for elem in input {
                out.push(apply_transform(transform, elem)?);
            }
            Ok(Value::Array(out))
        }
        "filter" => {
            check_keys(map, &["input", "predicate"])?;
            let input = required_array(map, "input")?;
            let predicate = required(map, "predicate")?;
            validate_predicate(predicate)?;
            let mut out = Vec::new();
            for elem in input {
                if eval_predicate(predicate, elem)? {
                    out.push(elem.clone());
                }
            }
            Ok(Value::Array(out))
        }
        "reduce" => {
            check_keys(map, &["input", "op", "initial"])?;
            let input = i64_array(map, "input")?;
            let sub_op = required_str(map, "op")?;
            let mut acc = match optional_i64(map, "initial")? {
                Some(v) => v,
                None => match sub_op {
                    "add" | "subtract" => 0,
                    "multiply" => 1,
                    _ => {
                        return Err(invalid_arg(format!(
                            "reduce 'op' must be add, subtract or multiply (got '{}')",
                            sub_op
                        )))
                    }
                },
            };
            if !matches!(sub_op, "add" | "subtract" | "multiply") {
                return Err(invalid_arg(format!(
                    "reduce 'op' must be add, subtract or multiply (got '{}')",
                    sub_op
                )));
            }
            for v in input {
                acc = checked_binary(sub_op, acc, v)?;
            }
            Ok(json!(acc))
        }
        "sort" => {
            check_keys(map, &["input"])?;
            let input = required_array(map, "input")?;
            let mut out = input.clone();
            out.sort_by(cmp_values);
            Ok(Value::Array(out))
        }
        "reverse" => {
            check_keys(map, &["input"])?;
            let input = required_array(map, "input")?;
            let mut out = input.clone();
            out.reverse();
            Ok(Value::Array(out))
        }
        "unique" => {
            check_keys(map, &["input"])?;
            let input = required_array(map, "input")?;
            let mut out: Vec<Value> = Vec::new();
            for elem in input {
                if !out.iter().any(|seen| equals(seen, elem)) {
                    out.push(elem.clone());
                }
            }
            Ok(Value::Array(out))
        }
        "slice" => {
            check_keys(map, &["input", "start", "end"])?;
            let input = required_array(map, "input")?;
            let start = required_i64(map, "start")?;
            let end = optional_i64(map, "end")?;
            let len = input.len() as i64;
            let normalize = |x: i64| -> i64 {
                let shifted = if x < 0 {
                    // Adding a non-negative length to almost any i64 cannot
                    // overflow i64::MIN.
                    x.saturating_add(len)
                } else {
                    x
                };
                if shifted < 0 {
                    0
                } else if shifted > len {
                    len
                } else {
                    shifted
                }
            };
            let s = normalize(start);
            let e = match end {
                Some(x) => normalize(x),
                None => len,
            };
            if s >= e {
                Ok(Value::Array(Vec::new()))
            } else {
                Ok(Value::Array(input[s as usize..e as usize].to_vec()))
            }
        }
        "flatten" => {
            check_keys(map, &["input"])?;
            let input = required_array(map, "input")?;
            let mut out = Vec::new();
            for elem in input {
                match elem {
                    Value::Array(inner) => out.extend(inner.iter().cloned()),
                    other => out.push(other.clone()),
                }
            }
            Ok(Value::Array(out))
        }

        // ------------------------------------------------------------- objects
        "get" => {
            check_keys(map, &["input", "key"])?;
            let input = required_object(map, "input")?;
            let key = required_str(map, "key")?;
            Ok(input.get(key).cloned().unwrap_or(Value::Null))
        }
        "set" => {
            check_keys(map, &["input", "key", "value"])?;
            let input = required_object(map, "input")?;
            let key = required_str(map, "key")?;
            let value = required(map, "value")?;
            let mut out = input.clone();
            out.insert(key.to_string(), value.clone());
            Ok(Value::Object(out))
        }
        "keys" => {
            check_keys(map, &["input"])?;
            let input = required_object(map, "input")?;
            let mut keys: Vec<&String> = input.keys().collect();
            keys.sort_by(|a, b| a.as_bytes().cmp(b.as_bytes()));
            Ok(Value::Array(
                keys.into_iter().map(|k| Value::String(k.clone())).collect(),
            ))
        }
        "values" => {
            check_keys(map, &["input"])?;
            let input = required_object(map, "input")?;
            let values = sorted_entries(input)
                .into_iter()
                .map(|(_, v)| v.clone())
                .collect();
            Ok(Value::Array(values))
        }
        "merge" => {
            check_keys(map, &["a", "b"])?;
            let a = required_object(map, "a")?;
            let b = required_object(map, "b")?;
            let mut out = a.clone();
            for (k, v) in b {
                out.insert(k.clone(), v.clone());
            }
            Ok(Value::Object(out))
        }
        "pick" => {
            check_keys(map, &["input", "keys"])?;
            let input = required_object(map, "input")?;
            let keys = string_array(map, "keys")?;
            let mut out = Map::new();
            for key in keys {
                let value = input.get(&key).cloned().unwrap_or(Value::Null);
                out.insert(key, value);
            }
            Ok(Value::Object(out))
        }
        "omit" => {
            check_keys(map, &["input", "keys"])?;
            let input = required_object(map, "input")?;
            let keys = string_array(map, "keys")?;
            let mut out = Map::new();
            for (k, v) in input {
                if !keys.iter().any(|key| key == k) {
                    out.insert(k.clone(), v.clone());
                }
            }
            Ok(Value::Object(out))
        }

        // ----------------------------------------------------------- utilities
        "constant" => {
            check_keys(map, &["value"])?;
            Ok(required(map, "value")?.clone())
        }
        "equals" => {
            check_keys(map, &["a", "b"])?;
            let a = required(map, "a")?;
            let b = required(map, "b")?;
            Ok(Value::Bool(equals(a, b)))
        }
        "compare" => {
            check_keys(map, &["a", "b", "op"])?;
            let a = required(map, "a")?;
            let b = required(map, "b")?;
            let cmp_op = required_str(map, "op")?;
            let ordering = cmp_values(a, b);
            let result = match cmp_op {
                "gt" => ordering == Ordering::Greater,
                "gte" => ordering != Ordering::Less,
                "lt" => ordering == Ordering::Less,
                "lte" => ordering != Ordering::Greater,
                _ => {
                    return Err(invalid_arg(format!(
                        "compare 'op' must be gt, gte, lt or lte (got '{}')",
                        cmp_op
                    )))
                }
            };
            Ok(Value::Bool(result))
        }
        "and" => {
            check_keys(map, &["a", "b"])?;
            let a = required(map, "a")?;
            let b = required(map, "b")?;
            Ok(Value::Bool(is_truthy(a) && is_truthy(b)))
        }
        "or" => {
            check_keys(map, &["a", "b"])?;
            let a = required(map, "a")?;
            let b = required(map, "b")?;
            Ok(Value::Bool(is_truthy(a) || is_truthy(b)))
        }
        "not" => {
            check_keys(map, &["a"])?;
            let a = required(map, "a")?;
            Ok(Value::Bool(!is_truthy(a)))
        }
        "type" => {
            check_keys(map, &["value"])?;
            Ok(Value::String(type_name(required(map, "value")?).to_string()))
        }

        // -------------------------------------------------------- test op
        "fail_until" => {
            check_keys(map, &["failures", "value"])?;
            let failures = required_i64(map, "failures")?;
            if !(0..=10).contains(&failures) {
                return Err(invalid_arg(
                    "fail_until 'failures' must be an integer from 0 to 10",
                ));
            }
            let value = required(map, "value")?;
            if (attempt_no as i64) <= failures {
                Err(WfError::op(
                    TRANSIENT_FAILURE,
                    format!(
                        "transient failure on attempt {} of {}",
                        attempt_no, failures
                    ),
                ))
            } else {
                Ok(value.clone())
            }
        }

        other => Err(invalid_arg(format!("unknown operation '{}'", other))),
    }
}

fn substitute_self(value: &Value, elem: &Value) -> Value {
    match value {
        Value::Object(o) => {
            if o.len() == 1 && matches!(o.get("$self"), Some(Value::Bool(true))) {
                return elem.clone();
            }
            let mut out = Map::new();
            for (k, v) in o {
                out.insert(k.clone(), substitute_self(v, elem));
            }
            Value::Object(out)
        }
        Value::Array(a) => Value::Array(a.iter().map(|v| substitute_self(v, elem)).collect()),
        other => other.clone(),
    }
}

/// Shape validation for a transform, independent of any elements. This makes
/// `map` on an empty array still reject a malformed transform.
fn validate_transform(transform: &Value) -> Result<(), WfError> {
    let o = transform
        .as_object()
        .ok_or_else(|| invalid_arg("map 'transform' must be an object"))?;
    let kind = o
        .get("type")
        .and_then(|v| v.as_str())
        .ok_or_else(|| invalid_arg("map 'transform' requires a string 'type'"))?;
    match kind {
        "constant" => {
            check_keys(o, &["type", "value"])?;
            if !o.contains_key("value") {
                return Err(invalid_arg("'constant' transform requires 'value'"));
            }
        }
        "add" | "subtract" | "multiply" | "divide" | "mod" => {
            check_keys(o, &["type", "value"])?;
            let value = o
                .get("value")
                .ok_or_else(|| invalid_arg(format!("'{}' transform requires 'value'", kind)))?;
            if integer_literal(value).is_none() {
                return Err(invalid_arg(format!(
                    "'{}' transform 'value' must be an integer",
                    kind
                )));
            }
        }
        "negate" | "abs" | "stringify" | "type" => {
            check_keys(o, &["type"])?;
        }
        "op" => {
            check_keys(o, &["type", "op", "args"])?;
            let sub_op = o
                .get("op")
                .and_then(|v| v.as_str())
                .ok_or_else(|| invalid_arg("'op' transform requires a string 'op'"))?;
            if !matches!(sub_op, "add" | "subtract" | "multiply" | "divide" | "mod") {
                return Err(invalid_arg(format!(
                    "'op' transform only supports add/subtract/multiply/divide/mod (got '{}')",
                    sub_op
                )));
            }
            if !o.get("args").map(|v| v.is_object()).unwrap_or(false) {
                return Err(invalid_arg("'op' transform requires an object 'args'"));
            }
        }
        other => return Err(invalid_arg(format!("unknown transform type '{}'", other))),
    }
    Ok(())
}

/// Shape validation for a filter predicate, independent of any elements.
fn validate_predicate(predicate: &Value) -> Result<(), WfError> {
    let o = predicate
        .as_object()
        .ok_or_else(|| invalid_arg("filter 'predicate' must be an object"))?;
    let kind = o
        .get("type")
        .and_then(|v| v.as_str())
        .ok_or_else(|| invalid_arg("filter 'predicate' requires a string 'type'"))?;
    match kind {
        "even" | "odd" | "truthy" | "falsy" => {
            check_keys(o, &["type"])?;
        }
        "gt" | "gte" | "lt" | "lte" | "eq" | "ne" => {
            check_keys(o, &["type", "value"])?;
            let value = o
                .get("value")
                .ok_or_else(|| invalid_arg(format!("'{}' predicate requires 'value'", kind)))?;
            if integer_literal(value).is_none() {
                return Err(invalid_arg(format!(
                    "'{}' predicate 'value' must be an integer",
                    kind
                )));
            }
        }
        "type_is" => {
            check_keys(o, &["type", "value", "type_name", "of"])?;
            if o.get("value")
                .or_else(|| o.get("type_name"))
                .or_else(|| o.get("of"))
                .and_then(|v| v.as_str())
                .is_none()
            {
                return Err(invalid_arg("'type_is' predicate requires a target type name"));
            }
        }
        "string_contains" => {
            check_keys(o, &["type", "value"])?;
            if o.get("value").and_then(|v| v.as_str()).is_none() {
                return Err(invalid_arg(
                    "'string_contains' predicate requires a string 'value'",
                ));
            }
        }
        other if TYPE_NAMES.contains(&other) => {
            check_keys(o, &["type"])?;
        }
        other => return Err(invalid_arg(format!("unknown predicate type '{}'", other))),
    }
    Ok(())
}

fn apply_transform(transform: &Value, elem: &Value) -> Result<Value, WfError> {
    let o = transform
        .as_object()
        .ok_or_else(|| invalid_arg("map 'transform' must be an object"))?;
    let kind = o
        .get("type")
        .and_then(|v| v.as_str())
        .ok_or_else(|| invalid_arg("map 'transform' requires a string 'type'"))?;

    match kind {
        "constant" => {
            check_keys(o, &["type", "value"])?;
            let value = o
                .get("value")
                .ok_or_else(|| invalid_arg("'constant' transform requires 'value'"))?;
            Ok(value.clone())
        }
        "add" | "subtract" | "multiply" | "divide" | "mod" => {
            check_keys(o, &["type", "value"])?;
            let value = o
                .get("value")
                .ok_or_else(|| invalid_arg(format!("'{}' transform requires 'value'", kind)))?;
            let operand = integer_literal(value).ok_or_else(|| {
                invalid_arg(format!("'{}' transform 'value' must be an integer", kind))
            })?;
            let element = integer_literal(elem).ok_or_else(|| {
                invalid_arg(format!(
                    "'{}' transform requires integer array elements",
                    kind
                ))
            })?;
            Ok(json!(checked_binary(kind, element, operand)?))
        }
        "negate" => {
            check_keys(o, &["type"])?;
            let element = integer_literal(elem)
                .ok_or_else(|| invalid_arg("'negate' transform requires integer elements"))?;
            match element.checked_neg() {
                Some(v) => Ok(json!(v)),
                None => Err(overflow("negation overflowed i64")),
            }
        }
        "abs" => {
            check_keys(o, &["type"])?;
            let element = integer_literal(elem)
                .ok_or_else(|| invalid_arg("'abs' transform requires integer elements"))?;
            match element.checked_abs() {
                Some(v) => Ok(json!(v)),
                None => Err(overflow("absolute value overflowed i64")),
            }
        }
        "stringify" => {
            check_keys(o, &["type"])?;
            let text = serde_json::to_string(elem)
                .map_err(|_| invalid_arg("could not stringify element"))?;
            Ok(Value::String(text))
        }
        "type" => {
            check_keys(o, &["type"])?;
            Ok(Value::String(type_name(elem).to_string()))
        }
        "op" => {
            check_keys(o, &["type", "op", "args"])?;
            let sub_op = o
                .get("op")
                .and_then(|v| v.as_str())
                .ok_or_else(|| invalid_arg("'op' transform requires a string 'op'"))?;
            if !matches!(sub_op, "add" | "subtract" | "multiply" | "divide" | "mod") {
                return Err(invalid_arg(format!(
                    "'op' transform only supports add/subtract/multiply/divide/mod (got '{}')",
                    sub_op
                )));
            }
            let sub_args = o
                .get("args")
                .ok_or_else(|| invalid_arg("'op' transform requires 'args'"))?;
            let substituted = substitute_self(sub_args, elem);
            run_op(sub_op, &substituted, 1)
        }
        other => Err(invalid_arg(format!("unknown transform type '{}'", other))),
    }
}

fn eval_predicate(predicate: &Value, elem: &Value) -> Result<bool, WfError> {
    let o = predicate
        .as_object()
        .ok_or_else(|| invalid_arg("filter 'predicate' must be an object"))?;
    let kind = o
        .get("type")
        .and_then(|v| v.as_str())
        .ok_or_else(|| invalid_arg("filter 'predicate' requires a string 'type'"))?;

    match kind {
        "even" | "odd" => {
            check_keys(o, &["type"])?;
            let element = integer_literal(elem)
                .ok_or_else(|| invalid_arg(format!("'{}' predicate requires integers", kind)))?;
            let remainder = element % 2;
            Ok(if kind == "even" {
                remainder == 0
            } else {
                remainder != 0
            })
        }
        "gt" | "gte" | "lt" | "lte" => {
            check_keys(o, &["type", "value"])?;
            let value = o
                .get("value")
                .ok_or_else(|| invalid_arg(format!("'{}' predicate requires 'value'", kind)))?;
            // `value` is declared as i64 but comparing through the total order
            // makes mixed-type arrays deterministic too.
            if integer_literal(value).is_none() {
                return Err(invalid_arg(format!(
                    "'{}' predicate 'value' must be an integer",
                    kind
                )));
            }
            let ordering = cmp_values(elem, value);
            Ok(match kind {
                "gt" => ordering == Ordering::Greater,
                "gte" => ordering != Ordering::Less,
                "lt" => ordering == Ordering::Less,
                "lte" => ordering != Ordering::Greater,
                _ => unreachable!(),
            })
        }
        "eq" | "ne" => {
            check_keys(o, &["type", "value"])?;
            let value = o
                .get("value")
                .ok_or_else(|| invalid_arg(format!("'{}' predicate requires 'value'", kind)))?;
            if integer_literal(value).is_none() {
                return Err(invalid_arg(format!(
                    "'{}' predicate 'value' must be an integer",
                    kind
                )));
            }
            let same = equals(elem, value);
            Ok(if kind == "eq" { same } else { !same })
        }
        "truthy" => {
            check_keys(o, &["type"])?;
            Ok(is_truthy(elem))
        }
        "falsy" => {
            check_keys(o, &["type"])?;
            Ok(!is_truthy(elem))
        }
        "type_is" => {
            // The spec writes the target as a second "type" key, which is not
            // representable in JSON. Accept the natural spellings so that
            // documents written against the prose still work.
            check_keys(o, &["type", "value", "type_name", "of"])?;
            let target = o
                .get("value")
                .or_else(|| o.get("type_name"))
                .or_else(|| o.get("of"))
                .and_then(|v| v.as_str())
                .ok_or_else(|| {
                    invalid_arg("'type_is' predicate requires a target type name")
                })?;
            Ok(type_name(elem) == target)
        }
        "string_contains" => {
            check_keys(o, &["type", "value"])?;
            let needle = o
                .get("value")
                .and_then(|v| v.as_str())
                .ok_or_else(|| invalid_arg("'string_contains' predicate requires a string 'value'"))?;
            let haystack = elem.as_str().ok_or_else(|| {
                invalid_arg("'string_contains' predicate requires string elements")
            })?;
            Ok(haystack.contains(needle))
        }
        // Convenience shorthand for `type_is` after a duplicate "type" key has
        // been collapsed by a JSON parser: {"type": "number"}.
        other if TYPE_NAMES.contains(&other) => {
            check_keys(o, &["type"])?;
            Ok(type_name(elem) == other)
        }
        other => Err(invalid_arg(format!("unknown predicate type '{}'", other))),
    }
}
