//! JSON Patch (RFC 6902) engine with RFC 6901 JSON Pointers.
//!
//! The implementation intentionally performs pointer parsing, escaping,
//! navigation, insertion and error classification itself.  `serde_json` is
//! used only as a general JSON parser/serializer.

use serde_json::{Map, Number, Value};
use std::io::Read;

const INVALID_REQUEST: &str = "INVALID_REQUEST";
const INVALID_PATH: &str = "INVALID_PATH";
const PATH_NOT_FOUND: &str = "PATH_NOT_FOUND";
const INDEX_ERROR: &str = "INDEX_ERROR";
const UNKNOWN_OP: &str = "UNKNOWN_OP";
const TEST_FAILED: &str = "TEST_FAILED";
const INVALID_MOVE: &str = "INVALID_MOVE";

#[derive(Debug)]
struct PatchError {
    code: &'static str,
    index: Option<usize>,
    message: String,
}

impl PatchError {
    fn new(code: &'static str, index: Option<usize>, message: impl Into<String>) -> Self {
        PatchError {
            code,
            index,
            message: message.into(),
        }
    }

    fn request(message: impl Into<String>) -> Self {
        Self::new(INVALID_REQUEST, None, message)
    }

    fn at(code: &'static str, index: usize, message: impl Into<String>) -> Self {
        Self::new(code, Some(index), message)
    }

    fn exit_code(&self) -> i32 {
        match self.code {
            INVALID_REQUEST => 2,
            _ => 1,
        }
    }
}

/// A parsed JSON Pointer.  `segments` are already unescaped.
#[derive(Debug, Clone)]
struct Pointer {
    segments: Vec<String>,
}

impl Pointer {
    fn parse(path: &str, index: usize) -> Result<Self, PatchError> {
        if path.is_empty() {
            return Ok(Pointer {
                segments: Vec::new(),
            });
        }
        if !path.starts_with('/') {
            return Err(PatchError::at(
                INVALID_PATH,
                index,
                "a non-empty JSON pointer must start with '/'",
            ));
        }

        let mut segments = Vec::new();
        for raw in path[1..].split('/') {
            segments.push(unescape_segment(raw, index)?);
        }
        Ok(Pointer { segments })
    }

    fn is_root(&self) -> bool {
        self.segments.is_empty()
    }

    /// True when `self` is a strict descendant of `ancestor`.
    fn is_strict_descendant_of(&self, ancestor: &Pointer) -> bool {
        self.segments.len() > ancestor.segments.len()
            && self.segments[..ancestor.segments.len()] == ancestor.segments[..]
    }
}

/// RFC 6901 unescaping: `~1` -> `/` and `~0` -> `~`.
///
/// This is the same as applying the two replacements in that order, but a
/// single left-to-right scan also lets us reject malformed escapes.
fn unescape_segment(raw: &str, index: usize) -> Result<String, PatchError> {
    let mut out = String::with_capacity(raw.len());
    let mut chars = raw.chars();
    while let Some(ch) = chars.next() {
        if ch == '~' {
            match chars.next() {
                Some('0') => out.push('~'),
                Some('1') => out.push('/'),
                _ => {
                    return Err(PatchError::at(
                        INVALID_PATH,
                        index,
                        "invalid '~' escape in JSON pointer",
                    ))
                }
            }
        } else {
            out.push(ch);
        }
    }
    Ok(out)
}

#[derive(Debug, Clone, Copy)]
enum ArrayRef {
    Index(usize),
    Append,
}

/// Parse an RFC 6902 array reference.  `-` is accepted only for `add`
/// (including the implicit add performed by move/copy).
fn parse_array_ref(raw: &str, allow_append: bool, index: usize) -> Result<ArrayRef, PatchError> {
    if raw == "-" {
        if allow_append {
            return Ok(ArrayRef::Append);
        }
        return Err(PatchError::at(
            INDEX_ERROR,
            index,
            "'-' is only valid for an add operation",
        ));
    }

    let bytes = raw.as_bytes();
    let valid_shape = !bytes.is_empty()
        && (bytes[0] == b'0' || (b'1'..=b'9').contains(&bytes[0]))
        && bytes.iter().all(|b| b.is_ascii_digit());
    if !valid_shape {
        return Err(PatchError::at(
            INDEX_ERROR,
            index,
            format!("invalid array index '{raw}'"),
        ));
    }
    if bytes.len() > 1 && bytes[0] == b'0' {
        return Err(PatchError::at(
            INDEX_ERROR,
            index,
            format!("array index '{raw}' has a leading zero"),
        ));
    }

    match raw.parse::<usize>() {
        Ok(n) => Ok(ArrayRef::Index(n)),
        Err(_) => Err(PatchError::at(
            INDEX_ERROR,
            index,
            format!("array index '{raw}' is out of range"),
        )),
    }
}

/// Navigate to an existing value.  Every segment must address an existing
/// member; malformed/out-of-range array references are `INDEX_ERROR` and
/// missing object members (or scalar parents) are `PATH_NOT_FOUND`.
fn get_value<'a>(
    root: &'a Value,
    pointer: &Pointer,
    index: usize,
) -> Result<&'a Value, PatchError> {
    let mut current = root;
    for segment in &pointer.segments {
        current = match current {
            Value::Object(map) => map.get(segment).ok_or_else(|| {
                PatchError::at(
                    PATH_NOT_FOUND,
                    index,
                    format!("member '{segment}' does not exist"),
                )
            })?,
            Value::Array(array) => {
                let position = match parse_array_ref(segment, false, index)? {
                    ArrayRef::Index(n) => n,
                    ArrayRef::Append => unreachable!(),
                };
                array.get(position).ok_or_else(|| {
                    PatchError::at(
                        INDEX_ERROR,
                        index,
                        format!("array index {position} is out of range"),
                    )
                })?
            }
            _ => {
                return Err(PatchError::at(
                    PATH_NOT_FOUND,
                    index,
                    "cannot descend through a scalar value",
                ))
            }
        };
    }
    Ok(current)
}

/// Navigate to the container that holds the final pointer segment.
fn get_parent_mut<'a>(
    root: &'a mut Value,
    pointer: &Pointer,
    index: usize,
) -> Result<&'a mut Value, PatchError> {
    debug_assert!(!pointer.segments.is_empty());
    let parent_segments = &pointer.segments[..pointer.segments.len() - 1];
    let mut current = root;
    for segment in parent_segments {
        current = match current {
            Value::Object(map) => map.get_mut(segment).ok_or_else(|| {
                PatchError::at(
                    PATH_NOT_FOUND,
                    index,
                    format!("member '{segment}' does not exist"),
                )
            })?,
            Value::Array(array) => {
                let position = match parse_array_ref(segment, false, index)? {
                    ArrayRef::Index(n) => n,
                    ArrayRef::Append => unreachable!(),
                };
                array.get_mut(position).ok_or_else(|| {
                    PatchError::at(
                        INDEX_ERROR,
                        index,
                        format!("array index {position} is out of range"),
                    )
                })?
            }
            _ => {
                return Err(PatchError::at(
                    PATH_NOT_FOUND,
                    index,
                    "cannot descend through a scalar value",
                ))
            }
        };
    }
    Ok(current)
}

fn op_add(
    document: &mut Value,
    pointer: &Pointer,
    value: Value,
    index: usize,
) -> Result<(), PatchError> {
    if pointer.is_root() {
        *document = value;
        return Ok(());
    }

    let last = pointer.segments.last().expect("non-root pointer");
    let parent = get_parent_mut(document, pointer, index)?;
    match parent {
        Value::Object(map) => {
            // Map::insert keeps the position of an existing key and appends a
            // new key, matching the natural insertion-order behaviour.
            map.insert(last.clone(), value);
            Ok(())
        }
        Value::Array(array) => match parse_array_ref(last, true, index)? {
            ArrayRef::Append => {
                array.push(value);
                Ok(())
            }
            ArrayRef::Index(position) => {
                if position <= array.len() {
                    array.insert(position, value);
                    Ok(())
                } else {
                    Err(PatchError::at(
                        INDEX_ERROR,
                        index,
                        format!("array index {position} is out of range"),
                    ))
                }
            }
        },
        _ => Err(PatchError::at(
            PATH_NOT_FOUND,
            index,
            "the target's parent is a scalar value",
        )),
    }
}

fn op_remove(
    document: &mut Value,
    pointer: &Pointer,
    index: usize,
) -> Result<Value, PatchError> {
    if pointer.is_root() {
        return Err(PatchError::at(
            INVALID_PATH,
            index,
            "the document root cannot be removed",
        ));
    }

    let last = pointer.segments.last().expect("non-root pointer");
    let parent = get_parent_mut(document, pointer, index)?;
    match parent {
        Value::Object(map) => map.shift_remove(last).ok_or_else(|| {
            PatchError::at(
                PATH_NOT_FOUND,
                index,
                format!("member '{last}' does not exist"),
            )
        }),
        Value::Array(array) => {
            let position = match parse_array_ref(last, false, index)? {
                ArrayRef::Index(n) => n,
                ArrayRef::Append => unreachable!(),
            };
            if position < array.len() {
                Ok(array.remove(position))
            } else {
                Err(PatchError::at(
                    INDEX_ERROR,
                    index,
                    format!("array index {position} is out of range"),
                ))
            }
        }
        _ => Err(PatchError::at(
            PATH_NOT_FOUND,
            index,
            "the target's parent is a scalar value",
        )),
    }
}

fn op_replace(
    document: &mut Value,
    pointer: &Pointer,
    value: Value,
    index: usize,
) -> Result<(), PatchError> {
    if pointer.is_root() {
        *document = value;
        return Ok(());
    }

    let last = pointer.segments.last().expect("non-root pointer");
    let parent = get_parent_mut(document, pointer, index)?;
    match parent {
        Value::Object(map) => {
            if !map.contains_key(last) {
                return Err(PatchError::at(
                    PATH_NOT_FOUND,
                    index,
                    format!("member '{last}' does not exist"),
                ));
            }
            map.insert(last.clone(), value);
            Ok(())
        }
        Value::Array(array) => {
            let position = match parse_array_ref(last, false, index)? {
                ArrayRef::Index(n) => n,
                ArrayRef::Append => unreachable!(),
            };
            if position < array.len() {
                array[position] = value;
                Ok(())
            } else {
                Err(PatchError::at(
                    INDEX_ERROR,
                    index,
                    format!("array index {position} is out of range"),
                ))
            }
        }
        _ => Err(PatchError::at(
            PATH_NOT_FOUND,
            index,
            "the target's parent is a scalar value",
        )),
    }
}

fn op_test(document: &Value, pointer: &Pointer, expected: &Value, index: usize) -> Result<(), PatchError> {
    let actual = get_value(document, pointer, index)?;
    if json_equal(actual, expected) {
        Ok(())
    } else {
        Err(PatchError::at(
            TEST_FAILED,
            index,
            "the value at the path does not match the test value",
        ))
    }
}

fn op_move(
    document: &mut Value,
    path: &Pointer,
    from: &Pointer,
    index: usize,
) -> Result<(), PatchError> {
    // The source must exist.
    get_value(document, from, index)?;

    if from.segments == path.segments {
        return Err(PatchError::at(
            INVALID_MOVE,
            index,
            "'from' and 'path' refer to the same location",
        ));
    }
    if path.is_strict_descendant_of(from) {
        return Err(PatchError::at(
            INVALID_MOVE,
            index,
            "cannot move a value into one of its own descendants",
        ));
    }

    let value = op_remove(document, from, index)?;
    op_add(document, path, value, index)
}

fn op_copy(
    document: &mut Value,
    path: &Pointer,
    from: &Pointer,
    index: usize,
) -> Result<(), PatchError> {
    let value = get_value(document, from, index)?.clone();
    op_add(document, path, value, index)
}

/// Structural JSON equality.  Numbers compare by numeric value (so `1` and
/// `1.0` are equal); different JSON types are never equal.
fn json_equal(left: &Value, right: &Value) -> bool {
    match (left, right) {
        (Value::Null, Value::Null) => true,
        (Value::Bool(a), Value::Bool(b)) => a == b,
        (Value::Number(a), Value::Number(b)) => numbers_equal(a, b),
        (Value::String(a), Value::String(b)) => a == b,
        (Value::Array(a), Value::Array(b)) => {
            a.len() == b.len() && a.iter().zip(b.iter()).all(|(x, y)| json_equal(x, y))
        }
        (Value::Object(a), Value::Object(b)) => {
            a.len() == b.len()
                && a.iter()
                    .all(|(key, value)| b.get(key).is_some_and(|other| json_equal(value, other)))
        }
        _ => false,
    }
}

fn numbers_equal(left: &Number, right: &Number) -> bool {
    fn as_integer(number: &Number) -> Option<i128> {
        if let Some(unsigned) = number.as_u64() {
            Some(unsigned as i128)
        } else {
            number.as_i64().map(|signed| signed as i128)
        }
    }

    // Compare two integral spellings exactly.  This matters for integers above
    // 2^53, which cannot be represented exactly as f64.
    fn float_equals_integer(float: &Number, integer: i128) -> bool {
        match float.as_f64() {
            Some(value) if value.is_finite() && value.fract() == 0.0 => {
                // Truncation is exact here because the fractional part is zero.
                // Out-of-i128 values saturate and cannot equal a JSON integer
                // (JSON integers fit in u64/i64).
                value as i128 == integer
            }
            _ => false,
        }
    }

    match (as_integer(left), as_integer(right)) {
        (Some(a), Some(b)) => a == b,
        (Some(a), None) => float_equals_integer(right, a),
        (None, Some(b)) => float_equals_integer(left, b),
        // Both are non-integral: f64 comparison gives numeric equality across
        // equivalent float spellings (1e2 == 100.0).
        (None, None) => match (left.as_f64(), right.as_f64()) {
            (Some(a), Some(b)) => a == b,
            _ => false,
        },
    }
}

struct Operation {
    kind: String,
    path: String,
    from: Option<String>,
    value: Option<Value>,
}

/// Validate the shape of one operation.  Shape errors are invalid requests
/// but still carry the index of the offending operation.
fn parse_operation(raw: &Value, index: usize) -> Result<Operation, PatchError> {
    let map: &Map<String, Value> = raw.as_object().ok_or_else(|| {
        PatchError::at(INVALID_REQUEST, index, "each patch entry must be an object")
    })?;

    let kind = map
        .get("op")
        .and_then(Value::as_str)
        .ok_or_else(|| PatchError::at(INVALID_REQUEST, index, "each operation needs a string 'op'"))?
        .to_string();

    let allowed: &[&str] = match kind.as_str() {
        "add" => &["op", "path", "value"],
        "remove" => &["op", "path"],
        "replace" => &["op", "path", "value"],
        "move" => &["op", "path", "from"],
        "copy" => &["op", "path", "from"],
        "test" => &["op", "path", "value"],
        _ => {
            return Err(PatchError::at(
                UNKNOWN_OP,
                index,
                format!("unknown operation '{kind}'"),
            ))
        }
    };

    if let Some(extra) = map.keys().find(|key| !allowed.contains(&key.as_str())) {
        return Err(PatchError::at(
            INVALID_REQUEST,
            index,
            format!("field '{extra}' is not valid for operation '{kind}'"),
        ));
    }

    let path = map
        .get("path")
        .and_then(Value::as_str)
        .ok_or_else(|| {
            PatchError::at(
                INVALID_REQUEST,
                index,
                format!("operation '{kind}' needs a string 'path'"),
            )
        })?
        .to_string();

    let mut from = None;
    let mut value = None;

    match kind.as_str() {
        "add" | "replace" | "test" => {
            let raw_value = map.get("value").ok_or_else(|| {
                PatchError::at(
                    INVALID_REQUEST,
                    index,
                    format!("operation '{kind}' needs a 'value' field"),
                )
            })?;
            value = Some(raw_value.clone());
        }
        "move" | "copy" => {
            let raw_from = map
                .get("from")
                .and_then(Value::as_str)
                .ok_or_else(|| {
                    PatchError::at(
                        INVALID_REQUEST,
                        index,
                        format!("operation '{kind}' needs a string 'from' field"),
                    )
                })?;
            from = Some(raw_from.to_string());
        }
        // "remove" has no value/from field.
        _ => {}
    }

    Ok(Operation {
        kind,
        path,
        from,
        value,
    })
}

fn apply_patch(document: &Value, patch: &[Value]) -> Result<Value, PatchError> {
    // Work on a copy: a patch is atomic and the original document is never
    // partially modified in an observable way.
    let mut result = document.clone();

    for (index, raw_op) in patch.iter().enumerate() {
        let operation = parse_operation(raw_op, index)?;
        let path = Pointer::parse(&operation.path, index)?;

        match operation.kind.as_str() {
            "add" => {
                op_add(
                    &mut result,
                    &path,
                    operation.value.expect("validated add value"),
                    index,
                )?;
            }
            "remove" => {
                op_remove(&mut result, &path, index)?;
            }
            "replace" => {
                op_replace(
                    &mut result,
                    &path,
                    operation.value.expect("validated replace value"),
                    index,
                )?;
            }
            "move" => {
                let from = Pointer::parse(
                    operation.from.as_deref().expect("validated move from"),
                    index,
                )?;
                op_move(&mut result, &path, &from, index)?;
            }
            "copy" => {
                let from = Pointer::parse(
                    operation.from.as_deref().expect("validated copy from"),
                    index,
                )?;
                op_copy(&mut result, &path, &from, index)?;
            }
            "test" => {
                op_test(
                    &result,
                    &path,
                    operation.value.as_ref().expect("validated test value"),
                    index,
                )?;
            }
            _ => unreachable!("operation kind was validated"),
        }
    }

    Ok(result)
}

fn parse_request(input: &str) -> Result<(Value, Vec<Value>), PatchError> {
    let parsed: Value = serde_json::from_str(input)
        .map_err(|err| PatchError::request(format!("invalid JSON request: {err}")))?;

    let object = parsed
        .as_object()
        .ok_or_else(|| PatchError::request("the request must be a JSON object"))?;

    if let Some(extra) = object
        .keys()
        .find(|key| key.as_str() != "document" && key.as_str() != "patch")
    {
        return Err(PatchError::request(format!(
            "unknown top-level field '{extra}'"
        )));
    }

    let document = object
        .get("document")
        .ok_or_else(|| PatchError::request("missing required field 'document'"))?
        .clone();

    let patch = object
        .get("patch")
        .ok_or_else(|| PatchError::request("missing required field 'patch'"))?
        .as_array()
        .ok_or_else(|| PatchError::request("field 'patch' must be an array"))?
        .clone();

    Ok((document, patch))
}

fn error_response(error: &PatchError) -> Value {
    let index = match error.index {
        Some(index) => Value::Number(Number::from(index as u64)),
        None => Value::Null,
    };
    serde_json::json!({
        "status": "error",
        "code": error.code,
        "index": index,
        "message": error.message,
    })
}

fn read_input(argument: &str) -> Result<String, PatchError> {
    if argument == "-" {
        let mut buffer = String::new();
        std::io::stdin()
            .read_to_string(&mut buffer)
            .map_err(|err| PatchError::request(format!("cannot read stdin: {err}")))?;
        Ok(buffer)
    } else {
        std::fs::read_to_string(argument)
            .map_err(|err| PatchError::request(format!("cannot read request file: {err}")))
    }
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();

    let error = if args.len() != 2 || args[0] != "apply" {
        PatchError::request("usage: patch apply <request.json|->")
    } else {
        match read_input(&args[1]) {
            Ok(input) => match parse_request(&input) {
                Ok((document, patch)) => match apply_patch(&document, &patch) {
                    Ok(result) => {
                        let response = serde_json::json!({
                            "status": "ok",
                            "document": result,
                        });
                        match serde_json::to_string(&response) {
                            Ok(text) => {
                                println!("{text}");
                                std::process::exit(0);
                            }
                            Err(err) => {
                                PatchError::new("INTERNAL", None, format!("serialization failed: {err}"))
                            }
                        }
                    }
                    Err(err) => err,
                },
                Err(err) => err,
            },
            Err(err) => err,
        }
    };

    let response = error_response(&error);
    match serde_json::to_string(&response) {
        Ok(text) => println!("{text}"),
        Err(_) => println!(
            "{{\"status\":\"error\",\"code\":\"INTERNAL\",\"index\":null,\"message\":\"could not serialize error\"}}"
        ),
    }
    let exit = if error.code == "INTERNAL" {
        3
    } else {
        error.exit_code()
    };
    std::process::exit(exit);
}
