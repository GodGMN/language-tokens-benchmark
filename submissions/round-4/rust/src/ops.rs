// All workflow operations (SPEC section 4). Operations receive fully
// `$ref`-resolved arguments and never perform I/O.
use crate::error::{err, Code, StepErr};
use crate::value_util::{cmp_values, structural_eq, truthy, type_name};
use serde_json::{Map, Value};
use std::cmp::Ordering;

type Args = Map<String, Value>;

fn check_keys(args: &Args, allowed: &[&str]) -> Result<(), StepErr> {
    for k in args.keys() {
        if !allowed.contains(&k.as_str()) {
            return err(Code::InvalidArg, format!("unknown argument: {k}"));
        }
    }
    Ok(())
}

fn required<'a>(args: &'a Args, key: &str) -> Result<&'a Value, StepErr> {
    args.get(key)
        .ok_or_else(|| StepErr::new(Code::InvalidArg, format!("missing argument: {key}")))
}

fn as_str<'a>(v: &'a Value, what: &str) -> Result<&'a str, StepErr> {
    v.as_str()
        .ok_or_else(|| StepErr::new(Code::InvalidArg, format!("{what} must be a string")))
}

fn as_arr<'a>(v: &'a Value, what: &str) -> Result<&'a Vec<Value>, StepErr> {
    v.as_array()
        .ok_or_else(|| StepErr::new(Code::InvalidArg, format!("{what} must be an array")))
}

fn as_obj<'a>(v: &'a Value, what: &str) -> Result<&'a Args, StepErr> {
    v.as_object()
        .ok_or_else(|| StepErr::new(Code::InvalidArg, format!("{what} must be an object")))
}

/// Strict integer parameter: JSON integer literal fitting in i64. Floats
/// (even integral ones such as 1.0) are rejected.
fn as_i64(v: &Value, what: &str) -> Result<i64, StepErr> {
    v.as_i64()
        .ok_or_else(|| StepErr::new(Code::InvalidArg, format!("{what} must be an i64 integer")))
}

fn req_str<'a>(args: &'a Args, key: &str) -> Result<&'a str, StepErr> {
    let v = required(args, key)?;
    as_str(v, key)
}

fn req_i64(args: &Args, key: &str) -> Result<i64, StepErr> {
    let v = required(args, key)?;
    as_i64(v, key)
}

fn req_arr<'a>(args: &'a Args, key: &str) -> Result<&'a Vec<Value>, StepErr> {
    let v = required(args, key)?;
    as_arr(v, key)
}

fn req_obj<'a>(args: &'a Args, key: &str) -> Result<&'a Args, StepErr> {
    let v = required(args, key)?;
    as_obj(v, key)
}

fn overflow<T>() -> Result<T, StepErr> {
    err(Code::ArithmeticOverflow, "i64 arithmetic overflow")
}

fn math_bin(op: &str, a: i64, b: i64) -> Result<i64, StepErr> {
    match op {
        "add" => a.checked_add(b).ok_or_else(|| {
            StepErr::new(Code::ArithmeticOverflow, "i64 arithmetic overflow")
        }),
        "subtract" => a.checked_sub(b).ok_or_else(|| {
            StepErr::new(Code::ArithmeticOverflow, "i64 arithmetic overflow")
        }),
        "multiply" => a.checked_mul(b).ok_or_else(|| {
            StepErr::new(Code::ArithmeticOverflow, "i64 arithmetic overflow")
        }),
        "divide" => {
            if b == 0 {
                err(Code::DivisionByZero, "division by zero")
            } else if a == i64::MIN && b == -1 {
                overflow()
            } else {
                Ok(a / b)
            }
        }
        "mod" => {
            if b == 0 {
                err(Code::DivisionByZero, "modulo by zero")
            } else if a == i64::MIN && b == -1 {
                overflow()
            } else {
                Ok(a % b)
            }
        }
        _ => err(Code::InvalidArg, format!("unsupported binary op: {op}")),
    }
}

fn is_binary_math(op: &str) -> bool {
    matches!(op, "add" | "subtract" | "multiply" | "divide" | "mod")
}

fn json_string(v: &Value) -> String {
    serde_json::to_string(v).unwrap_or_else(|_| "null".to_string())
}

fn normalize_index(i: i64, len: i64) -> i64 {
    if i < 0 {
        let x = len.saturating_add(i);
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
}

/// Replace every `{"$self": true}` marker (recursively) with `elem`.
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

fn apply_transform(elem: &Value, transform: &Args) -> Result<Value, StepErr> {
    let t = as_str(required(transform, "type")?, "transform.type")?;
    match t {
        "constant" => {
            check_keys(transform, &["type", "value"])?;
            Ok(required(transform, "value")?.clone())
        }
        "op" => {
            check_keys(transform, &["type", "op", "args"])?;
            let op = as_str(required(transform, "op")?, "transform.op")?;
            if !is_binary_math(op) {
                return err(
                    Code::InvalidArg,
                    format!("map transform op must be a two-number op, got {op}"),
                );
            }
            let raw_args = required(transform, "args")?;
            let raw_args = as_obj(raw_args, "transform.args")?;
            let mut resolved = Map::new();
            for (k, v) in raw_args {
                resolved.insert(k.clone(), substitute_self(v, elem));
            }
            execute(op, &resolved, 1)
        }
        "add" | "subtract" | "multiply" | "divide" | "mod" => {
            check_keys(transform, &["type", "value"])?;
            let value = as_i64(required(transform, "value")?, "transform.value")?;
            let e = as_i64(elem, "element")?;
            Ok(Value::from(math_bin(t, e, value)?))
        }
        "negate" => {
            check_keys(transform, &["type"])?;
            let e = as_i64(elem, "element")?;
            Ok(Value::from(e.checked_neg().ok_or_else(|| {
                StepErr::new(Code::ArithmeticOverflow, "i64 negation overflow")
            })?))
        }
        "abs" => {
            check_keys(transform, &["type"])?;
            let e = as_i64(elem, "element")?;
            Ok(Value::from(e.checked_abs().ok_or_else(|| {
                StepErr::new(Code::ArithmeticOverflow, "i64 abs overflow")
            })?))
        }
        "stringify" => {
            check_keys(transform, &["type"])?;
            Ok(Value::String(json_string(elem)))
        }
        "type" => {
            check_keys(transform, &["type"])?;
            Ok(Value::String(type_name(elem).to_string()))
        }
        _ => err(
            Code::InvalidArg,
            format!("unknown map transform type: {t}"),
        ),
    }
}

fn predicate_target(p: &Args, t: &str) -> Option<String> {
    // `type_is` needs a target type. The spec writes it with a duplicate
    // "type" key; depending on the JSON parser that either collapses to the
    // target type (handled by the caller) or keeps "type_is" and exposes the
    // target via value/type_name. Support all spellings.
    if let Some(Value::String(s)) = p.get("value") {
        return Some(s.clone());
    }
    if let Some(Value::String(s)) = p.get("type_name") {
        return Some(s.clone());
    }
    if let Some(Value::String(s)) = p.get("target") {
        return Some(s.clone());
    }
    if t != "type_is" {
        return Some(t.to_string());
    }
    None
}

fn apply_predicate(elem: &Value, pred: &Args) -> Result<bool, StepErr> {
    let t = as_str(required(pred, "type")?, "predicate.type")?;
    let type_names = ["null", "boolean", "number", "string", "array", "object"];
    if type_names.contains(&t) && pred.len() == 1 {
        // Collapsed duplicate-key spelling of {"type":"type_is","type":T}.
        return Ok(type_name(elem) == t);
    }
    match t {
        "even" | "odd" => {
            check_keys(pred, &["type"])?;
            let e = match elem.as_i64() {
                Some(e) => e,
                None => return Ok(false),
            };
            let r = e % 2;
            Ok(if t == "even" { r == 0 } else { r != 0 })
        }
        "gt" | "gte" | "lt" | "lte" => {
            check_keys(pred, &["type", "value"])?;
            let value = as_i64(required(pred, "value")?, "predicate.value")?;
            if !elem.is_number() {
                return Ok(false);
            }
            let rhs = Value::from(value);
            let c = cmp_values(elem, &rhs);
            Ok(match t {
                "gt" => c == Ordering::Greater,
                "gte" => c != Ordering::Less,
                "lt" => c == Ordering::Less,
                _ => c != Ordering::Greater,
            })
        }
        "eq" | "ne" => {
            check_keys(pred, &["type", "value"])?;
            let value = as_i64(required(pred, "value")?, "predicate.value")?;
            let eq = structural_eq(elem, &Value::from(value));
            Ok(if t == "eq" { eq } else { !eq })
        }
        "truthy" => {
            check_keys(pred, &["type"])?;
            Ok(truthy(elem))
        }
        "falsy" => {
            check_keys(pred, &["type"])?;
            Ok(!truthy(elem))
        }
        "type_is" => {
            check_keys(pred, &["type", "value", "type_name", "target"])?;
            let target = predicate_target(pred, t).ok_or_else(|| {
                StepErr::new(Code::InvalidArg, "type_is requires a target type")
            })?;
            Ok(type_name(elem) == target)
        }
        "string_contains" => {
            check_keys(pred, &["type", "value"])?;
            let needle = as_str(required(pred, "value")?, "predicate.value")?;
            match elem.as_str() {
                Some(s) => Ok(s.contains(needle)),
                None => Ok(false),
            }
        }
        _ => err(
            Code::InvalidArg,
            format!("unknown filter predicate type: {t}"),
        ),
    }
}

/// Execute one operation on already-resolved args. `attempt` is the 1-based
/// invocation count for this step (used only by `fail_until`).
pub fn execute(op: &str, args: &Args, attempt: u32) -> Result<Value, StepErr> {
    match op {
        "add" | "subtract" | "multiply" | "divide" | "mod" => {
            check_keys(args, &["a", "b"])?;
            let a = req_i64(args, "a")?;
            let b = req_i64(args, "b")?;
            Ok(Value::from(math_bin(op, a, b)?))
        }
        "sum" => {
            check_keys(args, &["input"])?;
            let input = req_arr(args, "input")?;
            let mut total: i64 = 0;
            for (i, v) in input.iter().enumerate() {
                let n = as_i64(v, &format!("input[{i}]"))?;
                total = total.checked_add(n).ok_or_else(|| {
                    StepErr::new(Code::ArithmeticOverflow, "i64 sum overflow")
                })?;
            }
            Ok(Value::from(total))
        }
        "min" | "max" => {
            check_keys(args, &["input"])?;
            let input = req_arr(args, "input")?;
            if input.is_empty() {
                return err(Code::EmptyArray, format!("{op} of empty array"));
            }
            let mut best = as_i64(&input[0], "input[0]")?;
            for (i, v) in input.iter().enumerate().skip(1) {
                let n = as_i64(v, &format!("input[{i}]"))?;
                if (op == "min" && n < best) || (op == "max" && n > best) {
                    best = n;
                }
            }
            Ok(Value::from(best))
        }
        "range" => {
            check_keys(args, &["start", "end"])?;
            let start = req_i64(args, "start")?;
            let end = req_i64(args, "end")?;
            let mut out = Vec::new();
            let mut cur = start;
            while cur <= end {
                out.push(Value::from(cur));
                if cur == i64::MAX {
                    break;
                }
                cur += 1;
            }
            Ok(Value::Array(out))
        }
        "concat" => {
            check_keys(args, &["a", "b"])?;
            let a = req_str(args, "a")?;
            let b = req_str(args, "b")?;
            Ok(Value::String(format!("{a}{b}")))
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
            let input = req_arr(args, "input")?;
            let sep = req_str(args, "sep")?;
            let mut parts: Vec<&str> = Vec::with_capacity(input.len());
            for (i, v) in input.iter().enumerate() {
                parts.push(as_str(v, &format!("input[{i}]"))?);
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
            Ok(Value::String(
                s.trim_matches(|c| matches!(c, ' ' | '\t' | '\n' | '\r' | '\u{000c}' | '\u{000b}'))
                    .to_string(),
            ))
        }
        "replace" => {
            check_keys(args, &["input", "search", "replace"])?;
            let input = req_str(args, "input")?;
            let search = req_str(args, "search")?;
            let replace = req_str(args, "replace")?;
            if search.is_empty() {
                return err(Code::InvalidArg, "replace search must not be empty");
            }
            Ok(Value::String(input.replace(search, replace)))
        }
        "length" => {
            check_keys(args, &["input"])?;
            let s = req_str(args, "input")?;
            Ok(Value::from(s.chars().count() as i64))
        }
        "map" => {
            check_keys(args, &["input", "transform"])?;
            let input = req_arr(args, "input")?;
            let transform = req_obj(args, "transform")?;
            let mut out = Vec::with_capacity(input.len());
            for elem in input {
                out.push(apply_transform(elem, transform)?);
            }
            Ok(Value::Array(out))
        }
        "filter" => {
            check_keys(args, &["input", "predicate"])?;
            let input = req_arr(args, "input")?;
            let pred = req_obj(args, "predicate")?;
            let mut out = Vec::new();
            for elem in input {
                if apply_predicate(elem, pred)? {
                    out.push(elem.clone());
                }
            }
            Ok(Value::Array(out))
        }
        "reduce" => {
            check_keys(args, &["input", "op", "initial"])?;
            let input = req_arr(args, "input")?;
            let rop = req_str(args, "op")?;
            if !matches!(rop, "add" | "subtract" | "multiply") {
                return err(Code::InvalidArg, format!("unsupported reduce op: {rop}"));
            }
            let mut acc = match args.get("initial") {
                Some(v) => as_i64(v, "initial")?,
                None => {
                    if rop == "multiply" {
                        1
                    } else {
                        0
                    }
                }
            };
            for (i, v) in input.iter().enumerate() {
                let n = as_i64(v, &format!("input[{i}]"))?;
                acc = math_bin(rop, acc, n)?;
            }
            Ok(Value::from(acc))
        }
        "sort" => {
            check_keys(args, &["input"])?;
            let mut input = req_arr(args, "input")?.clone();
            input.sort_by(cmp_values);
            Ok(Value::Array(input))
        }
        "reverse" => {
            check_keys(args, &["input"])?;
            let mut input = req_arr(args, "input")?.clone();
            input.reverse();
            Ok(Value::Array(input))
        }
        "unique" => {
            check_keys(args, &["input"])?;
            let input = req_arr(args, "input")?;
            let mut out: Vec<Value> = Vec::new();
            for v in input {
                if !out.iter().any(|kept| structural_eq(kept, v)) {
                    out.push(v.clone());
                }
            }
            Ok(Value::Array(out))
        }
        "slice" => {
            check_keys(args, &["input", "start", "end"])?;
            let input = req_arr(args, "input")?;
            let start = req_i64(args, "start")?;
            let len = input.len() as i64;
            let s = normalize_index(start, len);
            let e = match args.get("end") {
                Some(v) => normalize_index(as_i64(v, "end")?, len),
                None => len,
            };
            if s >= e {
                Ok(Value::Array(Vec::new()))
            } else {
                Ok(Value::Array(input[s as usize..e as usize].to_vec()))
            }
        }
        "flatten" => {
            check_keys(args, &["input"])?;
            let input = req_arr(args, "input")?;
            let mut out = Vec::new();
            for v in input {
                match v {
                    Value::Array(inner) => out.extend(inner.iter().cloned()),
                    other => out.push(other.clone()),
                }
            }
            Ok(Value::Array(out))
        }
        "get" => {
            check_keys(args, &["input", "key"])?;
            let input = req_obj(args, "input")?;
            let key = req_str(args, "key")?;
            Ok(input.get(key).cloned().unwrap_or(Value::Null))
        }
        "set" => {
            check_keys(args, &["input", "key", "value"])?;
            let input = req_obj(args, "input")?;
            let key = req_str(args, "key")?;
            let value = required(args, "value")?.clone();
            let mut out = input.clone();
            out.insert(key.to_string(), value);
            Ok(Value::Object(out))
        }
        "keys" => {
            check_keys(args, &["input"])?;
            let input = req_obj(args, "input")?;
            let mut keys: Vec<&String> = input.keys().collect();
            keys.sort_by(|a, b| a.as_bytes().cmp(b.as_bytes()));
            Ok(Value::Array(
                keys.into_iter().map(|k| Value::String(k.clone())).collect(),
            ))
        }
        "values" => {
            check_keys(args, &["input"])?;
            let input = req_obj(args, "input")?;
            let mut keys: Vec<&String> = input.keys().collect();
            keys.sort_by(|a, b| a.as_bytes().cmp(b.as_bytes()));
            Ok(Value::Array(
                keys.into_iter().map(|k| input[k].clone()).collect(),
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
            let input = req_obj(args, "input")?;
            let keys = req_arr(args, "keys")?;
            let mut out = Map::new();
            for (i, k) in keys.iter().enumerate() {
                let key = as_str(k, &format!("keys[{i}]"))?;
                let val = input.get(key).cloned().unwrap_or(Value::Null);
                out.insert(key.to_string(), val);
            }
            Ok(Value::Object(out))
        }
        "omit" => {
            check_keys(args, &["input", "keys"])?;
            let input = req_obj(args, "input")?;
            let keys = req_arr(args, "keys")?;
            let mut out = input.clone();
            for (i, k) in keys.iter().enumerate() {
                let key = as_str(k, &format!("keys[{i}]"))?;
                out.remove(key);
            }
            Ok(Value::Object(out))
        }
        "constant" => {
            check_keys(args, &["value"])?;
            Ok(required(args, "value")?.clone())
        }
        "equals" => {
            check_keys(args, &["a", "b"])?;
            let a = required(args, "a")?;
            let b = required(args, "b")?;
            Ok(Value::Bool(structural_eq(a, b)))
        }
        "compare" => {
            check_keys(args, &["a", "b", "op"])?;
            let a = required(args, "a")?;
            let b = required(args, "b")?;
            let cop = req_str(args, "op")?;
            let c = cmp_values(a, b);
            let result = match cop {
                "gt" => c == Ordering::Greater,
                "gte" => c != Ordering::Less,
                "lt" => c == Ordering::Less,
                "lte" => c != Ordering::Greater,
                _ => return err(Code::InvalidArg, format!("unknown compare op: {cop}")),
            };
            Ok(Value::Bool(result))
        }
        "and" => {
            check_keys(args, &["a", "b"])?;
            let a = required(args, "a")?;
            let b = required(args, "b")?;
            Ok(Value::Bool(truthy(a) && truthy(b)))
        }
        "or" => {
            check_keys(args, &["a", "b"])?;
            let a = required(args, "a")?;
            let b = required(args, "b")?;
            Ok(Value::Bool(truthy(a) || truthy(b)))
        }
        "not" => {
            check_keys(args, &["a"])?;
            let a = required(args, "a")?;
            Ok(Value::Bool(!truthy(a)))
        }
        "type" => {
            check_keys(args, &["value"])?;
            let v = required(args, "value")?;
            Ok(Value::String(type_name(v).to_string()))
        }
        "fail_until" => {
            check_keys(args, &["failures", "value"])?;
            let failures = req_i64(args, "failures")?;
            if !(0..=10).contains(&failures) {
                return err(Code::InvalidArg, "failures must be between 0 and 10");
            }
            let value = required(args, "value")?.clone();
            if (attempt as i64) <= failures {
                err(Code::TransientFailure, "transient failure")
            } else {
                Ok(value)
            }
        }
        _ => err(Code::InvalidArg, format!("unknown op: {op}")),
    }
}
