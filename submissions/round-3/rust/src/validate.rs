//! Structural validation of a workflow document and dependency analysis.

use crate::error::{WfError, CYCLIC_DEPENDENCY, INVALID_WORKFLOW, UNKNOWN_DEPENDENCY};
use crate::value::integer_literal;
use serde_json::{Map, Value};
use std::collections::{HashMap, HashSet};

pub const OP_NAMES: &[&str] = &[
    "add",
    "subtract",
    "multiply",
    "divide",
    "mod",
    "sum",
    "min",
    "max",
    "range",
    "concat",
    "split",
    "join",
    "uppercase",
    "lowercase",
    "trim",
    "replace",
    "length",
    "map",
    "filter",
    "reduce",
    "sort",
    "reverse",
    "unique",
    "slice",
    "flatten",
    "get",
    "set",
    "keys",
    "values",
    "merge",
    "pick",
    "omit",
    "constant",
    "equals",
    "compare",
    "and",
    "or",
    "not",
    "type",
    "fail_until",
];

#[derive(Debug, Clone)]
pub struct Step {
    pub id: String,
    pub op: String,
    pub args: Value,
    pub depends_on: Vec<String>,
    pub when: Option<Value>,
    pub max_attempts: i64,
}

#[derive(Debug)]
pub struct Workflow {
    pub steps: Vec<Step>,
    pub id_to_index: HashMap<String, usize>,
    /// For each step, the set of its direct and transitive dependencies.
    pub ancestors: Vec<HashSet<String>>,
}

fn is_valid_id(id: &str) -> bool {
    let mut chars = id.chars();
    match chars.next() {
        Some(c) if c.is_ascii_alphabetic() || c == '_' => {}
        _ => return false,
    }
    chars.all(|c| c.is_ascii_alphanumeric() || c == '_')
}

fn step_error(step: Option<String>, message: impl Into<String>) -> WfError {
    WfError::new(INVALID_WORKFLOW, step, message)
}

fn validate_condition_shape(c: &Value) -> Result<(), String> {
    let o = c
        .as_object()
        .ok_or_else(|| "condition must be an object".to_string())?;
    let op = o
        .get("op")
        .and_then(|v| v.as_str())
        .ok_or_else(|| "condition 'op' must be a string".to_string())?;
    match op {
        "eq" | "ne" | "gt" | "gte" | "lt" | "lte" => {
            for key in o.keys() {
                if key != "op" && key != "left" && key != "right" {
                    return Err(format!("unknown condition field '{}'", key));
                }
            }
            if !o.contains_key("left") {
                return Err("condition is missing 'left'".to_string());
            }
            if !o.contains_key("right") {
                return Err("condition is missing 'right'".to_string());
            }
            Ok(())
        }
        "all" | "any" => {
            for key in o.keys() {
                if key != "op" && key != "conditions" {
                    return Err(format!("unknown condition field '{}'", key));
                }
            }
            let arr = o
                .get("conditions")
                .and_then(|v| v.as_array())
                .ok_or_else(|| "compound condition requires a 'conditions' array".to_string())?;
            for sub in arr {
                validate_condition_shape(sub)?;
            }
            Ok(())
        }
        "not" => {
            for key in o.keys() {
                if key != "op" && key != "condition" {
                    return Err(format!("unknown condition field '{}'", key));
                }
            }
            let sub = o
                .get("condition")
                .ok_or_else(|| "'not' condition requires 'condition'".to_string())?;
            validate_condition_shape(sub)
        }
        other => Err(format!("unknown condition op '{}'", other)),
    }
}

fn validate_step(v: &Value) -> Result<Step, WfError> {
    let obj: &Map<String, Value> = match v {
        Value::Object(o) => o,
        _ => return Err(step_error(None, "each step must be an object")),
    };

    // The id is used for error attribution whenever it is a string, even if it
    // does not match the id pattern.
    let id_hint: Option<String> = match obj.get("id") {
        Some(Value::String(s)) => Some(s.clone()),
        _ => None,
    };

    for key in obj.keys() {
        if !matches!(
            key.as_str(),
            "id" | "op" | "args" | "depends_on" | "when" | "retry"
        ) {
            return Err(step_error(
                id_hint.clone(),
                format!("unknown step field '{}'", key),
            ));
        }
    }

    let id = match obj.get("id") {
        Some(Value::String(s)) => {
            if !is_valid_id(s) {
                return Err(step_error(
                    Some(s.clone()),
                    format!("invalid step id '{}'", s),
                ));
            }
            s.clone()
        }
        Some(_) => return Err(step_error(None, "step 'id' must be a string")),
        None => return Err(step_error(None, "step is missing 'id'")),
    };

    let op = match obj.get("op") {
        Some(Value::String(s)) if OP_NAMES.contains(&s.as_str()) => s.clone(),
        Some(Value::String(s)) => {
            return Err(step_error(
                Some(id.clone()),
                format!("unknown operation '{}'", s),
            ))
        }
        Some(_) => return Err(step_error(Some(id.clone()), "step 'op' must be a string")),
        None => return Err(step_error(Some(id.clone()), "step is missing 'op'")),
    };

    let args = match obj.get("args") {
        None => Value::Object(Map::new()),
        Some(Value::Object(_)) => obj.get("args").unwrap().clone(),
        Some(_) => {
            return Err(step_error(
                Some(id.clone()),
                "step 'args' must be an object",
            ))
        }
    };

    let depends_on = match obj.get("depends_on") {
        None => Vec::new(),
        Some(Value::Array(a)) => {
            let mut deps = Vec::with_capacity(a.len());
            for e in a {
                match e {
                    Value::String(s) => deps.push(s.clone()),
                    _ => {
                        return Err(step_error(
                            Some(id.clone()),
                            "'depends_on' must be an array of strings",
                        ))
                    }
                }
            }
            deps
        }
        Some(_) => {
            return Err(step_error(
                Some(id.clone()),
                "'depends_on' must be an array of strings",
            ))
        }
    };

    let when = match obj.get("when") {
        None => None,
        Some(c) => {
            validate_condition_shape(c)
                .map_err(|msg| step_error(Some(id.clone()), msg))?;
            Some(c.clone())
        }
    };

    let max_attempts = match obj.get("retry") {
        None => 1,
        Some(Value::Object(r)) => {
            for key in r.keys() {
                if key != "max_attempts" {
                    return Err(step_error(
                        Some(id.clone()),
                        format!("unknown retry field '{}'", key),
                    ));
                }
            }
            match r.get("max_attempts") {
                None => 1,
                Some(v) => match integer_literal(v) {
                    Some(n) if (1..=5).contains(&n) => n,
                    _ => {
                        return Err(step_error(
                            Some(id.clone()),
                            "retry 'max_attempts' must be an integer from 1 to 5",
                        ))
                    }
                },
            }
        }
        Some(_) => {
            return Err(step_error(
                Some(id.clone()),
                "'retry' must be an object",
            ))
        }
    };

    Ok(Step {
        id,
        op,
        args,
        depends_on,
        when,
        max_attempts,
    })
}

pub fn validate(doc: &Value) -> Result<Workflow, WfError> {
    let obj = match doc {
        Value::Object(o) => o,
        _ => return Err(step_error(None, "workflow document must be an object")),
    };

    for key in obj.keys() {
        if key != "version" && key != "steps" {
            return Err(step_error(
                None,
                format!("unknown top-level field '{}'", key),
            ));
        }
    }

    let version = match obj.get("version") {
        Some(Value::Number(n)) if n.is_i64() && n.as_i64() == Some(1) => 1i64,
        Some(_) => {
            return Err(step_error(
                None,
                "workflow 'version' must be the integer 1",
            ))
        }
        None => return Err(step_error(None, "workflow is missing 'version'")),
    };
    let _ = version;

    let steps_value = match obj.get("steps") {
        Some(v) => v,
        None => return Err(step_error(None, "workflow is missing 'steps'")),
    };
    let steps_arr = match steps_value {
        Value::Array(a) => a,
        _ => return Err(step_error(None, "workflow 'steps' must be an array")),
    };

    let mut steps: Vec<Step> = Vec::with_capacity(steps_arr.len());
    for raw in steps_arr {
        steps.push(validate_step(raw)?);
    }

    // Unique ids.
    let mut id_to_index: HashMap<String, usize> = HashMap::new();
    for (i, step) in steps.iter().enumerate() {
        if id_to_index.contains_key(&step.id) {
            return Err(step_error(
                Some(step.id.clone()),
                format!("duplicate step id '{}'", step.id),
            ));
        }
        id_to_index.insert(step.id.clone(), i);
    }

    // Dependencies must exist.
    for step in &steps {
        for dep in &step.depends_on {
            if !id_to_index.contains_key(dep) {
                return Err(WfError::new(
                    UNKNOWN_DEPENDENCY,
                    Some(step.id.clone()),
                    format!("step '{}' depends on unknown step '{}'", step.id, dep),
                ));
            }
        }
    }

    // Deterministic topological sort: repeatedly take the earliest step in the
    // document whose dependencies are all already ordered.
    let n = steps.len();
    let mut indegree = vec![0usize; n];
    let mut dependents: Vec<Vec<usize>> = vec![Vec::new(); n];
    for (i, step) in steps.iter().enumerate() {
        for dep in &step.depends_on {
            let j = id_to_index[dep];
            indegree[i] += 1;
            dependents[j].push(i);
        }
    }
    let mut done = vec![false; n];
    let mut topo: Vec<usize> = Vec::with_capacity(n);
    loop {
        let mut pick: Option<usize> = None;
        for i in 0..n {
            if !done[i] && indegree[i] == 0 {
                pick = Some(i);
                break;
            }
        }
        match pick {
            Some(i) => {
                done[i] = true;
                topo.push(i);
                for &d in &dependents[i] {
                    indegree[d] -= 1;
                }
            }
            None => break,
        }
    }
    if topo.len() != n {
        return Err(WfError::new(
            CYCLIC_DEPENDENCY,
            None,
            "workflow contains a dependency cycle",
        ));
    }

    // Transitive dependency sets.
    let mut ancestors: Vec<HashSet<String>> = vec![HashSet::new(); n];
    for &i in &topo {
        for dep in &steps[i].depends_on {
            let j = id_to_index[dep];
            ancestors[i].insert(dep.clone());
            let inherited: Vec<String> = ancestors[j].iter().cloned().collect();
            ancestors[i].extend(inherited);
        }
    }

    Ok(Workflow {
        steps,
        id_to_index,
        ancestors,
    })
}
