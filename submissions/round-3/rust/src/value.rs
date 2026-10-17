//! Value semantics: JSON types, structural equality, total ordering,
//! truthiness, integer coercion and object key ordering.

use serde_json::{Map, Number, Value};
use std::cmp::Ordering;

pub fn type_name(v: &Value) -> &'static str {
    match v {
        Value::Null => "null",
        Value::Bool(_) => "boolean",
        Value::Number(_) => "number",
        Value::String(_) => "string",
        Value::Array(_) => "array",
        Value::Object(_) => "object",
    }
}

/// JSON truthiness: false, null, "", 0, [], {} are falsy.
pub fn is_truthy(v: &Value) -> bool {
    match v {
        Value::Null => false,
        Value::Bool(b) => *b,
        Value::Number(n) => !is_zero(n),
        Value::String(s) => !s.is_empty(),
        Value::Array(a) => !a.is_empty(),
        Value::Object(o) => !o.is_empty(),
    }
}

fn is_zero(n: &Number) -> bool {
    match num_repr(n) {
        NumRepr::I(i) => i == 0,
        NumRepr::U(u) => u == 0,
        NumRepr::F(f) => f == 0.0,
    }
}

/// Returns the value as an i64 only if it is a JSON integer literal that fits
/// in i64. Numbers written with a fraction or exponent are rejected.
pub fn integer_literal(v: &Value) -> Option<i64> {
    match v {
        Value::Number(n) if n.is_i64() => n.as_i64(),
        _ => None,
    }
}

#[derive(Clone, Copy)]
enum NumRepr {
    I(i64),
    U(u64),
    F(f64),
}

fn num_repr(n: &Number) -> NumRepr {
    if n.is_i64() {
        NumRepr::I(n.as_i64().unwrap())
    } else if n.is_u64() {
        NumRepr::U(n.as_u64().unwrap())
    } else {
        NumRepr::F(n.as_f64().unwrap_or(0.0))
    }
}

fn cmp_i64_f64(i: i64, f: f64) -> Ordering {
    if f.is_nan() {
        return Ordering::Equal;
    }
    // -2^63 is exactly representable; 2^63 is the first value above i64::MAX.
    const MIN_F: f64 = -9223372036854775808.0;
    const MAX_PLUS_ONE_F: f64 = 9223372036854775808.0;
    if f < MIN_F {
        return Ordering::Greater;
    }
    if f >= MAX_PLUS_ONE_F {
        return Ordering::Less;
    }
    let trunc = f.trunc();
    let trunc_i = trunc as i64;
    match i.cmp(&trunc_i) {
        Ordering::Equal => {
            if f > trunc {
                Ordering::Less
            } else {
                Ordering::Equal
            }
        }
        other => other,
    }
}

fn cmp_u64_f64(u: u64, f: f64) -> Ordering {
    if f.is_nan() {
        return Ordering::Equal;
    }
    if f < 0.0 {
        return Ordering::Greater;
    }
    // 2^64 is the first value above u64::MAX.
    const MAX_PLUS_ONE_F: f64 = 18446744073709551616.0;
    if f >= MAX_PLUS_ONE_F {
        return Ordering::Less;
    }
    let trunc = f.trunc();
    let trunc_u = trunc as u64;
    match u.cmp(&trunc_u) {
        Ordering::Equal => {
            if f > trunc {
                Ordering::Less
            } else {
                Ordering::Equal
            }
        }
        other => other,
    }
}

pub fn cmp_numbers(a: &Number, b: &Number) -> Ordering {
    match (num_repr(a), num_repr(b)) {
        (NumRepr::I(x), NumRepr::I(y)) => x.cmp(&y),
        (NumRepr::U(x), NumRepr::U(y)) => x.cmp(&y),
        (NumRepr::I(_), NumRepr::U(_)) => Ordering::Less,
        (NumRepr::U(_), NumRepr::I(_)) => Ordering::Greater,
        (NumRepr::I(x), NumRepr::F(y)) => cmp_i64_f64(x, y),
        (NumRepr::F(x), NumRepr::I(y)) => cmp_i64_f64(y, x).reverse(),
        (NumRepr::U(x), NumRepr::F(y)) => cmp_u64_f64(x, y),
        (NumRepr::F(x), NumRepr::U(y)) => cmp_u64_f64(y, x).reverse(),
        (NumRepr::F(x), NumRepr::F(y)) => x.partial_cmp(&y).unwrap_or(Ordering::Equal),
    }
}

/// Structural equality. Numbers compare by numeric value (1 == 1.0),
/// object key order is irrelevant, arrays compare element-wise.
pub fn equals(a: &Value, b: &Value) -> bool {
    match (a, b) {
        (Value::Null, Value::Null) => true,
        (Value::Bool(x), Value::Bool(y)) => x == y,
        (Value::Number(x), Value::Number(y)) => cmp_numbers(x, y) == Ordering::Equal,
        (Value::String(x), Value::String(y)) => x == y,
        (Value::Array(x), Value::Array(y)) => {
            x.len() == y.len() && x.iter().zip(y.iter()).all(|(p, q)| equals(p, q))
        }
        (Value::Object(x), Value::Object(y)) => {
            x.len() == y.len()
                && x.iter()
                    .all(|(k, v)| y.get(k).map(|w| equals(v, w)).unwrap_or(false))
        }
        _ => false,
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

pub fn sorted_entries(o: &Map<String, Value>) -> Vec<(&String, &Value)> {
    let mut entries: Vec<(&String, &Value)> = o.iter().collect();
    entries.sort_by(|a, b| a.0.as_bytes().cmp(b.0.as_bytes()));
    entries
}

/// Total order over all JSON values, as defined by SPEC §3.
pub fn cmp_values(a: &Value, b: &Value) -> Ordering {
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
            let ex = sorted_entries(x);
            let ey = sorted_entries(y);
            let n = ex.len().min(ey.len());
            for i in 0..n {
                let c = ex[i].0.as_bytes().cmp(ey[i].0.as_bytes());
                if c != Ordering::Equal {
                    return c;
                }
            }
            if ex.len() != ey.len() {
                return ex.len().cmp(&ey.len());
            }
            for i in 0..n {
                let c = cmp_values(ex[i].1, ey[i].1);
                if c != Ordering::Equal {
                    return c;
                }
            }
            Ordering::Equal
        }
        _ => Ordering::Equal,
    }
}
