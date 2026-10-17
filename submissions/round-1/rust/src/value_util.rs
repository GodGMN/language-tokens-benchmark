use std::cmp::Ordering;

use serde_json::{Map, Value};

use crate::decimal::{number_cmp, number_is_zero, number_key};

pub fn type_name(value: &Value) -> &'static str {
    match value {
        Value::Null => "null",
        Value::Bool(_) => "boolean",
        Value::Number(_) => "number",
        Value::String(_) => "string",
        Value::Array(_) => "array",
        Value::Object(_) => "object",
    }
}

pub fn type_rank(value: &Value) -> u8 {
    match value {
        Value::Number(_) => 0,
        Value::String(_) => 1,
        Value::Bool(_) => 2,
        Value::Null => 3,
        Value::Array(_) => 4,
        Value::Object(_) => 5,
    }
}

pub fn structural_eq(a: &Value, b: &Value) -> bool {
    match (a, b) {
        (Value::Null, Value::Null) => true,
        (Value::Bool(x), Value::Bool(y)) => x == y,
        (Value::Number(x), Value::Number(y)) => number_cmp(x, y) == Ordering::Equal,
        (Value::String(x), Value::String(y)) => x == y,
        (Value::Array(x), Value::Array(y)) => {
            x.len() == y.len() && x.iter().zip(y.iter()).all(|(u, v)| structural_eq(u, v))
        }
        (Value::Object(x), Value::Object(y)) => {
            x.len() == y.len()
                && x.iter()
                    .all(|(k, v)| y.get(k).map(|w| structural_eq(v, w)).unwrap_or(false))
        }
        _ => false,
    }
}

fn sorted_keys(map: &Map<String, Value>) -> Vec<&str> {
    let mut keys: Vec<&str> = map.keys().map(|k| k.as_str()).collect();
    keys.sort_by(|a, b| a.as_bytes().cmp(b.as_bytes()));
    keys
}

pub fn compare_values(a: &Value, b: &Value) -> Ordering {
    let ra = type_rank(a);
    let rb = type_rank(b);
    if ra != rb {
        return ra.cmp(&rb);
    }
    match (a, b) {
        (Value::Number(x), Value::Number(y)) => number_cmp(x, y),
        (Value::String(x), Value::String(y)) => x.as_bytes().cmp(y.as_bytes()),
        (Value::Bool(x), Value::Bool(y)) => x.cmp(y),
        (Value::Null, Value::Null) => Ordering::Equal,
        (Value::Array(x), Value::Array(y)) => {
            for (u, v) in x.iter().zip(y.iter()) {
                let ord = compare_values(u, v);
                if ord != Ordering::Equal {
                    return ord;
                }
            }
            x.len().cmp(&y.len())
        }
        (Value::Object(x), Value::Object(y)) => {
            let kx = sorted_keys(x);
            let ky = sorted_keys(y);
            for (u, v) in kx.iter().zip(ky.iter()) {
                let ord = u.as_bytes().cmp(v.as_bytes());
                if ord != Ordering::Equal {
                    return ord;
                }
            }
            let key_ord = kx.len().cmp(&ky.len());
            if key_ord != Ordering::Equal {
                return key_ord;
            }
            for key in kx {
                let ord = compare_values(&x[key], &y[key]);
                if ord != Ordering::Equal {
                    return ord;
                }
            }
            Ordering::Equal
        }
        _ => Ordering::Equal,
    }
}

/// Build an unambiguous canonical key for structural equality.  This is used
/// to make `unique` linear-time even for large arrays.
pub fn canonical_key(value: &Value) -> String {
    let mut out = String::new();
    append_canonical(value, &mut out);
    out
}

fn append_canonical(value: &Value, out: &mut String) {
    match value {
        Value::Null => out.push('z'),
        Value::Bool(true) => out.push('T'),
        Value::Bool(false) => out.push('F'),
        Value::Number(n) => {
            out.push('n');
            out.push_str(&number_key(n));
            out.push(';');
        }
        Value::String(s) => {
            out.push('s');
            out.push_str(&s.len().to_string());
            out.push(':');
            out.push_str(s);
        }
        Value::Array(items) => {
            out.push('a');
            out.push_str(&items.len().to_string());
            out.push('[');
            for item in items {
                append_canonical(item, out);
            }
            out.push(']');
        }
        Value::Object(map) => {
            let keys = sorted_keys(map);
            out.push('o');
            out.push_str(&keys.len().to_string());
            out.push('{');
            for key in keys {
                out.push('k');
                out.push_str(&key.len().to_string());
                out.push(':');
                out.push_str(key);
                append_canonical(&map[key], out);
            }
            out.push('}');
        }
    }
}

pub fn truthy(value: &Value) -> bool {
    match value {
        Value::Null => false,
        Value::Bool(b) => *b,
        Value::String(s) => !s.is_empty(),
        Value::Number(n) => !number_is_zero(n),
        Value::Array(a) => !a.is_empty(),
        Value::Object(o) => !o.is_empty(),
    }
}

/// Sort helper that is explicit about byte-wise key ordering for object output.
pub fn sorted_string_keys(map: &Map<String, Value>) -> Vec<String> {
    let mut keys: Vec<String> = map.keys().cloned().collect();
    keys.sort_by(|a, b| a.as_bytes().cmp(b.as_bytes()));
    keys
}
