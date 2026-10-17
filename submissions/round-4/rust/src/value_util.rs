// JSON value helpers: type names, truthiness, structural equality and the
// total ordering used by sort/compare.
use serde_json::{Number, Value};
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

enum Num {
    Int(i128),
    Float(f64),
}

fn num(n: &Number) -> Num {
    if n.is_i64() {
        return Num::Int(n.as_i64().unwrap() as i128);
    }
    if n.is_u64() {
        return Num::Int(n.as_u64().unwrap() as i128);
    }
    // With arbitrary_precision, integer literals beyond u64 are preserved as
    // text. Recover exact integer values whenever they fit in i128.
    let text = n.to_string();
    let looks_integer = !text.is_empty()
        && text
            .bytes()
            .enumerate()
            .all(|(i, b)| (i == 0 && b == b'-') || b.is_ascii_digit());
    if looks_integer {
        if let Ok(v) = text.parse::<i128>() {
            return Num::Int(v);
        }
    }
    Num::Float(n.as_f64().unwrap_or(0.0))
}

/// Compare an integer (which fits in i128) with a finite/non-finite f64
/// without losing precision for large integers.
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
    // JSON integers accepted here are within [-2^63, 2^64); use generous
    // i128 bounds for safety.
    if f >= i128::MAX as f64 {
        return Ordering::Less;
    }
    if f < i128::MIN as f64 {
        return Ordering::Greater;
    }
    if f == i128::MIN as f64 {
        return i.cmp(&i128::MIN);
    }
    if f.fract() == 0.0 {
        let fi = f as i128;
        return i.cmp(&fi);
    }
    let fl = f.floor();
    let fi = fl as i128;
    match i.cmp(&fi) {
        Ordering::Equal => Ordering::Less, // i == floor(f) < f
        other => other,
    }
}

/// Numeric comparison. Integer values are compared exactly; mixed
/// integer/float comparisons preserve exact integer precision where possible.
pub fn cmp_numbers(a: &Number, b: &Number) -> Ordering {
    match (num(a), num(b)) {
        (Num::Int(x), Num::Int(y)) => x.cmp(&y),
        (Num::Float(x), Num::Float(y)) => x.partial_cmp(&y).unwrap_or(Ordering::Equal),
        (Num::Int(x), Num::Float(y)) => cmp_int_float(x, y),
        (Num::Float(x), Num::Int(y)) => cmp_int_float(y, x).reverse(),
    }
}

pub fn structural_eq(a: &Value, b: &Value) -> bool {
    match (a, b) {
        (Value::Null, Value::Null) => true,
        (Value::Bool(x), Value::Bool(y)) => x == y,
        (Value::Number(x), Value::Number(y)) => cmp_numbers(x, y) == Ordering::Equal,
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

/// Total order over all JSON values, as required by SPEC section 3.
pub fn cmp_values(a: &Value, b: &Value) -> Ordering {
    let (ra, rb) = (type_rank(a), type_rank(b));
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
            let mut kx: Vec<&String> = x.keys().collect();
            let mut ky: Vec<&String> = y.keys().collect();
            kx.sort_by(|p, q| p.as_bytes().cmp(q.as_bytes()));
            ky.sort_by(|p, q| p.as_bytes().cmp(q.as_bytes()));
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

/// JSON truthiness: false, null, "", 0, [], {} are falsy.
pub fn truthy(v: &Value) -> bool {
    match v {
        Value::Null => false,
        Value::Bool(b) => *b,
        Value::Number(n) => n.as_f64().map(|f| f != 0.0).unwrap_or(false),
        Value::String(s) => !s.is_empty(),
        Value::Array(a) => !a.is_empty(),
        Value::Object(o) => !o.is_empty(),
    }
}

/// Split a dotted reference path. The first segment is the step id; the
/// remaining segments navigate through objects and arrays.
pub fn split_ref(s: &str) -> (String, Vec<String>) {
    let mut it = s.split('.');
    let first = it.next().unwrap_or("").to_string();
    let rest = it.map(|p| p.to_string()).collect();
    (first, rest)
}
