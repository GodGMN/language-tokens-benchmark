//! Exact handling of JSON numbers.
//!
//! serde_json is built with `arbitrary_precision`, so the original decimal text
//! of every number is available.  This lets us decide exactly whether a number
//! is an integer that fits in i64, and to compare numbers without loss of
//! precision for integers outside the f64 range.

use std::cmp::Ordering;

use serde_json::Number;

#[derive(Clone, Debug)]
struct Dec {
    negative: bool,
    /// Canonical decimal digits: no leading zeros, and (except for zero) no
    /// trailing zeros.
    digits: String,
    /// value = (negative ? -1 : 1) * digits * 10^exp
    exp: i64,
}

fn parse_exponent(s: &str) -> Option<i64> {
    if s.is_empty() {
        return None;
    }
    let (negative, digits) = if let Some(rest) = s.strip_prefix('-') {
        (true, rest)
    } else if let Some(rest) = s.strip_prefix('+') {
        (false, rest)
    } else {
        (false, s)
    };
    if digits.is_empty() || !digits.bytes().all(|b| b.is_ascii_digit()) {
        return None;
    }
    let mut value: i64 = 0;
    for b in digits.bytes() {
        value = value.saturating_mul(10).saturating_add((b - b'0') as i64);
    }
    Some(if negative { -value } else { value })
}

fn parse_dec(text: &str) -> Option<Dec> {
    let (negative, rest) = if let Some(r) = text.strip_prefix('-') {
        (true, r)
    } else {
        (false, text)
    };
    let (mantissa, exp_text) = match rest.find(['e', 'E']) {
        Some(pos) => (&rest[..pos], &rest[pos + 1..]),
        None => (rest, "0"),
    };
    let (int_part, frac_part) = match mantissa.find('.') {
        Some(pos) => (&mantissa[..pos], &mantissa[pos + 1..]),
        None => (mantissa, ""),
    };
    if int_part.is_empty() && frac_part.is_empty() {
        return None;
    }
    if !int_part.bytes().all(|b| b.is_ascii_digit())
        || !frac_part.bytes().all(|b| b.is_ascii_digit())
    {
        return None;
    }
    let exp_base = parse_exponent(exp_text)?;
    let mut digits = String::with_capacity(int_part.len() + frac_part.len());
    digits.push_str(int_part);
    digits.push_str(frac_part);
    let mut exp = exp_base.saturating_sub(frac_part.len() as i64);

    let first_nonzero = digits.bytes().position(|b| b != b'0');
    let Some(first) = first_nonzero else {
        return Some(Dec {
            negative: false,
            digits: "0".to_string(),
            exp: 0,
        });
    };
    if first > 0 {
        digits.drain(..first);
    }
    while digits.ends_with('0') {
        digits.pop();
        exp = exp.saturating_add(1);
    }
    Some(Dec {
        negative,
        digits,
        exp,
    })
}

fn dec(number: &Number) -> Option<Dec> {
    parse_dec(&number.to_string())
}

pub fn number_to_i64(number: &Number) -> Option<i64> {
    let d = dec(number)?;
    if d.digits == "0" {
        return Some(0);
    }
    if d.exp < 0 || d.exp > 18 {
        return None;
    }
    let total_len = d.digits.len() + d.exp as usize;
    if total_len > 19 {
        return None;
    }
    let mut text = String::with_capacity(total_len);
    text.push_str(&d.digits);
    for _ in 0..d.exp {
        text.push('0');
    }
    let magnitude: u128 = text.parse().ok()?;
    if d.negative {
        if magnitude == (i64::MAX as u128) + 1 {
            Some(i64::MIN)
        } else if magnitude <= i64::MAX as u128 {
            Some(-(magnitude as i64))
        } else {
            None
        }
    } else if magnitude <= i64::MAX as u128 {
        Some(magnitude as i64)
    } else {
        None
    }
}

fn cmp_abs(a: &Dec, b: &Dec) -> Ordering {
    // Number of decimal digits before the decimal point (for magnitudes >= 1).
    let len_a = a.digits.len() as i128 + a.exp as i128;
    let len_b = b.digits.len() as i128 + b.exp as i128;
    if len_a != len_b {
        return len_a.cmp(&len_b);
    }
    // Equal total length. Compare digit strings, treating missing positions as
    // zeros. Since both have the same total length, any remaining positions are
    // zeros in both numbers.
    let max_len = a.digits.len().max(b.digits.len());
    let ab = a.digits.as_bytes();
    let bb = b.digits.as_bytes();
    for i in 0..max_len {
        let ca = *ab.get(i).unwrap_or(&b'0');
        let cb = *bb.get(i).unwrap_or(&b'0');
        if ca != cb {
            return ca.cmp(&cb);
        }
    }
    Ordering::Equal
}

fn cmp_dec(a: &Dec, b: &Dec) -> Ordering {
    let a_zero = a.digits == "0";
    let b_zero = b.digits == "0";
    if a_zero && b_zero {
        return Ordering::Equal;
    }
    if a_zero {
        return if b.negative {
            Ordering::Greater
        } else {
            Ordering::Less
        };
    }
    if b_zero {
        return if a.negative {
            Ordering::Less
        } else {
            Ordering::Greater
        };
    }
    if a.negative != b.negative {
        return if a.negative {
            Ordering::Less
        } else {
            Ordering::Greater
        };
    }
    let ord = cmp_abs(a, b);
    if a.negative {
        ord.reverse()
    } else {
        ord
    }
}

pub fn number_cmp(a: &Number, b: &Number) -> Ordering {
    match (dec(a), dec(b)) {
        (Some(da), Some(db)) => cmp_dec(&da, &db),
        _ => {
            // The parser only produces valid JSON numbers; this fallback keeps
            // behaviour total even if a Number was constructed programmatically.
            let fa = a.as_f64().unwrap_or(0.0);
            let fb = b.as_f64().unwrap_or(0.0);
            fa.partial_cmp(&fb).unwrap_or(Ordering::Equal)
        }
    }
}

pub fn number_is_zero(number: &Number) -> bool {
    dec(number).map(|d| d.digits == "0").unwrap_or(false)
}

/// Canonical text for a number under structural equality.  Numbers that are
/// numerically equal (for example `1`, `1.0` and `1e0`) produce the same key.
pub fn number_key(number: &Number) -> String {
    match dec(number) {
        Some(d) => {
            if d.digits == "0" {
                "0".to_string()
            } else {
                let sign = if d.negative { "-" } else { "" };
                format!("{sign}{}e{}", d.digits, d.exp)
            }
        }
        None => number.to_string(),
    }
}
