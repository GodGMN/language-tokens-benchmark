use std::collections::{HashMap, HashSet};

use serde_json::{Map, Value};

#[derive(Clone, Copy, Debug)]
pub enum CmpOp {
    Eq,
    Ne,
    Gt,
    Gte,
    Lt,
    Lte,
}

#[derive(Debug)]
pub enum Condition {
    Compare {
        op: CmpOp,
        left: Value,
        right: Value,
    },
    All(Vec<Condition>),
    Any(Vec<Condition>),
    Not(Box<Condition>),
    /// A reference used directly as a condition.  These are not part of the
    /// documented comparison grammar, but accepting them gives a well-defined
    /// path to INVALID_CONDITION when the value is not a strict boolean.
    Ref(String),
}

#[derive(Debug)]
pub struct StepDef {
    pub id: String,
    pub op: String,
    pub args: Map<String, Value>,
    pub deps: Vec<String>,
    pub when: Option<Condition>,
    pub max_attempts: u32,
}

#[derive(Debug)]
pub struct Workflow {
    pub steps: Vec<StepDef>,
}

#[derive(Debug)]
pub struct ValidationError {
    pub code: &'static str,
    pub step: Option<String>,
    pub message: String,
}

fn err(code: &'static str, step: Option<String>, message: impl Into<String>) -> ValidationError {
    ValidationError {
        code,
        step,
        message: message.into(),
    }
}

fn valid_id(id: &str) -> bool {
    let mut bytes = id.bytes();
    let Some(first) = bytes.next() else {
        return false;
    };
    if !(first.is_ascii_alphabetic() || first == b'_') {
        return false;
    }
    bytes.all(|b| b.is_ascii_alphanumeric() || b == b'_')
}

fn known_op(op: &str) -> bool {
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

pub fn validate(doc: &Value) -> Result<Workflow, ValidationError> {
    let obj = doc
        .as_object()
        .ok_or_else(|| err("INVALID_WORKFLOW", None, "workflow must be a JSON object"))?;

    for key in obj.keys() {
        if key != "version" && key != "steps" {
            return Err(err(
                "INVALID_WORKFLOW",
                None,
                format!("unknown top-level field: {key}"),
            ));
        }
    }

    let version = obj
        .get("version")
        .ok_or_else(|| err("INVALID_WORKFLOW", None, "missing version"))?;
    let version_ok = version
        .as_number()
        .and_then(|n| n.as_i64())
        .map(|v| v == 1)
        .unwrap_or(false);
    if !version_ok {
        return Err(err(
            "INVALID_WORKFLOW",
            None,
            "version must be the integer 1",
        ));
    }

    let raw_steps = obj
        .get("steps")
        .ok_or_else(|| err("INVALID_WORKFLOW", None, "missing steps"))?;
    let raw_steps = raw_steps
        .as_array()
        .ok_or_else(|| err("INVALID_WORKFLOW", None, "steps must be an array"))?;

    let mut steps: Vec<StepDef> = Vec::with_capacity(raw_steps.len());
    let mut seen_ids: HashSet<String> = HashSet::new();

    for raw in raw_steps {
        let step_obj = match raw.as_object() {
            Some(o) => o,
            None => {
                return Err(err(
                    "INVALID_WORKFLOW",
                    None,
                    "each step must be an object",
                ))
            }
        };

        let id_value = step_obj.get("id");
        let id_string = id_value.and_then(Value::as_str);
        let step_hint = id_string.map(|s| s.to_string());

        for key in step_obj.keys() {
            if !matches!(
                key.as_str(),
                "id" | "op" | "args" | "depends_on" | "when" | "retry"
            ) {
                return Err(err(
                    "INVALID_WORKFLOW",
                    step_hint,
                    format!("unknown step field: {key}"),
                ));
            }
        }

        let id = match id_string {
            Some(s) if valid_id(s) => s.to_string(),
            Some(_) => {
                return Err(err(
                    "INVALID_WORKFLOW",
                    step_hint,
                    "invalid step id",
                ))
            }
            None => {
                return Err(err(
                    "INVALID_WORKFLOW",
                    None,
                    "step id must be a string",
                ))
            }
        };
        if !seen_ids.insert(id.clone()) {
            return Err(err(
                "INVALID_WORKFLOW",
                Some(id),
                "duplicate step id",
            ));
        }

        let op = match step_obj.get("op").and_then(Value::as_str) {
            Some(op) if known_op(op) => op.to_string(),
            Some(op) => {
                return Err(err(
                    "INVALID_WORKFLOW",
                    Some(id),
                    format!("unknown operation: {op}"),
                ))
            }
            None => {
                return Err(err(
                    "INVALID_WORKFLOW",
                    Some(id),
                    "step op must be a string",
                ))
            }
        };

        let args = match step_obj.get("args") {
            None => Map::new(),
            Some(v) => match v.as_object() {
                Some(m) => m.clone(),
                None => {
                    return Err(err(
                        "INVALID_WORKFLOW",
                        Some(id),
                        "args must be an object",
                    ))
                }
            },
        };

        let deps = match step_obj.get("depends_on") {
            None => Vec::new(),
            Some(v) => {
                let arr = match v.as_array() {
                    Some(a) => a,
                    None => {
                        return Err(err(
                            "INVALID_WORKFLOW",
                            Some(id),
                            "depends_on must be an array",
                        ))
                    }
                };
                let mut out = Vec::with_capacity(arr.len());
                for item in arr {
                    let dep = match item.as_str() {
                        Some(s) => s.to_string(),
                        None => {
                            return Err(err(
                                "INVALID_WORKFLOW",
                                Some(id),
                                "depends_on entries must be strings",
                            ))
                        }
                    };
                    out.push(dep);
                }
                out
            }
        };

        let max_attempts = match step_obj.get("retry") {
            None => 1,
            Some(v) => {
                let r = match v.as_object() {
                    Some(o) => o,
                    None => {
                        return Err(err(
                            "INVALID_WORKFLOW",
                            Some(id),
                            "retry must be an object",
                        ))
                    }
                };
                for key in r.keys() {
                    if key != "max_attempts" {
                        return Err(err(
                            "INVALID_WORKFLOW",
                            Some(id),
                            format!("unknown retry field: {key}"),
                        ));
                    }
                }
                match r.get("max_attempts") {
                    None => 1,
                    Some(n) => {
                        let attempts = n
                            .as_number()
                            .and_then(|number| number.as_i64())
                            .ok_or_else(|| {
                                err(
                                    "INVALID_WORKFLOW",
                                    Some(id.clone()),
                                    "max_attempts must be an integer",
                                )
                            })?;
                        if !(1..=5).contains(&attempts) {
                            return Err(err(
                                "INVALID_WORKFLOW",
                                Some(id),
                                "max_attempts must be between 1 and 5",
                            ));
                        }
                        attempts as u32
                    }
                }
            }
        };

        let when = match step_obj.get("when") {
            None => None,
            Some(v) => Some(parse_condition(v, &id)?),
        };

        steps.push(StepDef {
            id,
            op,
            args,
            deps,
            when,
            max_attempts,
        });
    }

    let index: HashMap<String, usize> = steps
        .iter()
        .enumerate()
        .map(|(i, s)| (s.id.clone(), i))
        .collect();

    for step in &steps {
        for dep in &step.deps {
            if !index.contains_key(dep) {
                return Err(err(
                    "UNKNOWN_DEPENDENCY",
                    Some(step.id.clone()),
                    format!("unknown dependency: {dep}"),
                ));
            }
        }
    }

    let mut colors = vec![0u8; steps.len()];
    for i in 0..steps.len() {
        if colors[i] == 0 && visit_cycle(i, &steps, &index, &mut colors).is_err() {
            return Err(err(
                "CYCLIC_DEPENDENCY",
                None,
                "dependency cycle detected",
            ));
        }
    }

    Ok(Workflow { steps })
}

fn visit_cycle(
    i: usize,
    steps: &[StepDef],
    index: &HashMap<String, usize>,
    colors: &mut [u8],
) -> Result<(), ()> {
    colors[i] = 1;
    for dep in &steps[i].deps {
        let j = index[dep];
        match colors[j] {
            0 => visit_cycle(j, steps, index, colors)?,
            1 => return Err(()),
            _ => {}
        }
    }
    colors[i] = 2;
    Ok(())
}

fn parse_condition(value: &Value, step_id: &str) -> Result<Condition, ValidationError> {
    let obj = match value.as_object() {
        Some(o) => o,
        None => {
            return Err(err(
                "INVALID_WORKFLOW",
                Some(step_id.to_string()),
                "when must be an object",
            ))
        }
    };

    if obj.len() == 1 {
        if let Some(target) = obj.get("$ref").and_then(Value::as_str) {
            return Ok(Condition::Ref(target.to_string()));
        }
    }

    let op = match obj.get("op").and_then(Value::as_str) {
        Some(op) => op,
        None => {
            return Err(err(
                "INVALID_WORKFLOW",
                Some(step_id.to_string()),
                "condition op must be a string",
            ))
        }
    };

    let cmp = match op {
        "eq" => Some(CmpOp::Eq),
        "ne" => Some(CmpOp::Ne),
        "gt" => Some(CmpOp::Gt),
        "gte" => Some(CmpOp::Gte),
        "lt" => Some(CmpOp::Lt),
        "lte" => Some(CmpOp::Lte),
        _ => None,
    };

    if let Some(cmp) = cmp {
        for key in obj.keys() {
            if key != "op" && key != "left" && key != "right" {
                return Err(err(
                    "INVALID_WORKFLOW",
                    Some(step_id.to_string()),
                    format!("unknown condition field: {key}"),
                ));
            }
        }
        let left = obj.get("left").cloned().ok_or_else(|| {
            err(
                "INVALID_WORKFLOW",
                Some(step_id.to_string()),
                "condition missing left",
            )
        })?;
        let right = obj.get("right").cloned().ok_or_else(|| {
            err(
                "INVALID_WORKFLOW",
                Some(step_id.to_string()),
                "condition missing right",
            )
        })?;
        return Ok(Condition::Compare {
            op: cmp,
            left,
            right,
        });
    }

    match op {
        "all" | "any" => {
            for key in obj.keys() {
                if key != "op" && key != "conditions" {
                    return Err(err(
                        "INVALID_WORKFLOW",
                        Some(step_id.to_string()),
                        format!("unknown condition field: {key}"),
                    ));
                }
            }
            let list = obj
                .get("conditions")
                .and_then(Value::as_array)
                .ok_or_else(|| {
                    err(
                        "INVALID_WORKFLOW",
                        Some(step_id.to_string()),
                        "conditions must be an array",
                    )
                })?;
            let mut parsed = Vec::with_capacity(list.len());
            for item in list {
                parsed.push(parse_condition(item, step_id)?);
            }
            if op == "all" {
                Ok(Condition::All(parsed))
            } else {
                Ok(Condition::Any(parsed))
            }
        }
        "not" => {
            for key in obj.keys() {
                if key != "op" && key != "condition" {
                    return Err(err(
                        "INVALID_WORKFLOW",
                        Some(step_id.to_string()),
                        format!("unknown condition field: {key}"),
                    ));
                }
            }
            let inner = obj.get("condition").ok_or_else(|| {
                err(
                    "INVALID_WORKFLOW",
                    Some(step_id.to_string()),
                    "not condition missing condition",
                )
            })?;
            Ok(Condition::Not(Box::new(parse_condition(inner, step_id)?)))
        }
        _ => Err(err(
            "INVALID_WORKFLOW",
            Some(step_id.to_string()),
            format!("unknown condition op: {op}"),
        )),
    }
}
