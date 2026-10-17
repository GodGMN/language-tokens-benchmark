//! Condition evaluation and deterministic topological execution.

use crate::error::{invalid_condition, WfError, STEP_FAILED, TRANSIENT_FAILURE};
use crate::ops;
use crate::refs::RefContext;
use crate::validate::Workflow;
use crate::value::{cmp_values, equals};
use serde_json::Value;
use std::cmp::Ordering;
use std::collections::{BTreeMap, BTreeSet};

pub struct Execution {
    pub results: BTreeMap<String, Value>,
    pub order: Vec<String>,
    pub skipped: Vec<String>,
    pub error: Option<WfError>,
}

fn eval_condition(ctx: &RefContext, condition: &Value) -> Result<bool, WfError> {
    let o = condition
        .as_object()
        .ok_or_else(|| invalid_condition("condition must be an object"))?;
    let op = o
        .get("op")
        .and_then(|v| v.as_str())
        .ok_or_else(|| invalid_condition("condition 'op' must be a string"))?;

    let result = match op {
        "all" => {
            let conds = o
                .get("conditions")
                .and_then(|v| v.as_array())
                .ok_or_else(|| invalid_condition("'all' requires 'conditions'"))?;
            let mut value = true;
            for sub in conds {
                if !eval_condition(ctx, sub)? {
                    value = false;
                    break;
                }
            }
            Value::Bool(value)
        }
        "any" => {
            let conds = o
                .get("conditions")
                .and_then(|v| v.as_array())
                .ok_or_else(|| invalid_condition("'any' requires 'conditions'"))?;
            let mut value = false;
            for sub in conds {
                if eval_condition(ctx, sub)? {
                    value = true;
                    break;
                }
            }
            Value::Bool(value)
        }
        "not" => {
            let sub = o
                .get("condition")
                .ok_or_else(|| invalid_condition("'not' requires 'condition'"))?;
            let inner = eval_condition(ctx, sub)?;
            Value::Bool(!inner)
        }
        "eq" | "ne" | "gt" | "gte" | "lt" | "lte" => {
            let left = o
                .get("left")
                .ok_or_else(|| invalid_condition("condition is missing 'left'"))?;
            let right = o
                .get("right")
                .ok_or_else(|| invalid_condition("condition is missing 'right'"))?;
            let lv = ctx.resolve(left)?;
            let rv = ctx.resolve(right)?;
            match op {
                "eq" => Value::Bool(equals(&lv, &rv)),
                "ne" => Value::Bool(!equals(&lv, &rv)),
                "gt" => Value::Bool(cmp_values(&lv, &rv) == Ordering::Greater),
                "gte" => Value::Bool(cmp_values(&lv, &rv) != Ordering::Less),
                "lt" => Value::Bool(cmp_values(&lv, &rv) == Ordering::Less),
                "lte" => Value::Bool(cmp_values(&lv, &rv) != Ordering::Greater),
                _ => unreachable!(),
            }
        }
        other => {
            return Err(invalid_condition(format!(
                "unknown condition op '{}'",
                other
            )))
        }
    };

    match result {
        Value::Bool(b) => Ok(b),
        _ => Err(invalid_condition("condition did not evaluate to a boolean")),
    }
}

pub fn run(wf: &Workflow) -> Execution {
    let n = wf.steps.len();
    let mut done = vec![false; n];
    let mut results: BTreeMap<String, Value> = BTreeMap::new();
    let mut skipped_set: BTreeSet<String> = BTreeSet::new();
    let mut order: Vec<String> = Vec::new();
    let mut skipped_order: Vec<String> = Vec::new();

    loop {
        // Earliest step in document order whose dependencies have all finished.
        let mut pick: Option<usize> = None;
        for i in 0..n {
            if done[i] {
                continue;
            }
            let ready = wf.steps[i]
                .depends_on
                .iter()
                .all(|dep| done[wf.id_to_index[dep]]);
            if ready {
                pick = Some(i);
                break;
            }
        }
        let i = match pick {
            Some(i) => i,
            None => break,
        };
        done[i] = true;
        let step = &wf.steps[i];

        // ---------------------------------------------------------- condition
        if let Some(condition) = &step.when {
            let ctx = RefContext {
                wf,
                current: i,
                results: &results,
                skipped: &skipped_set,
            };
            match eval_condition(&ctx, condition) {
                Ok(false) => {
                    results.insert(step.id.clone(), Value::Null);
                    skipped_set.insert(step.id.clone());
                    order.push(step.id.clone());
                    skipped_order.push(step.id.clone());
                    continue;
                }
                Ok(true) => {}
                Err(e) => {
                    return Execution {
                        results,
                        order,
                        skipped: skipped_order,
                        error: Some(e.at_step(&step.id)),
                    };
                }
            }
        }

        // -------------------------------------------------------------- args
        let resolved = {
            let ctx = RefContext {
                wf,
                current: i,
                results: &results,
                skipped: &skipped_set,
            };
            match ctx.resolve(&step.args) {
                Ok(v) => v,
                Err(e) => {
                    return Execution {
                        results,
                        order,
                        skipped: skipped_order,
                        error: Some(e.at_step(&step.id)),
                    };
                }
            }
        };

        // ----------------------------------------------------------- attempts
        let mut success: Option<Value> = None;
        let mut failure: Option<WfError> = None;
        for attempt in 0..step.max_attempts {
            let attempt_no = (attempt + 1) as u32;
            match ops::run_op(&step.op, &resolved, attempt_no) {
                Ok(v) => {
                    success = Some(v);
                    break;
                }
                Err(e) => {
                    if e.code == TRANSIENT_FAILURE {
                        if attempt + 1 < step.max_attempts {
                            continue;
                        }
                        failure = Some(WfError::new(
                            STEP_FAILED,
                            Some(step.id.clone()),
                            format!(
                                "step '{}' failed after {} attempt(s): {}",
                                step.id, step.max_attempts, e.message
                            ),
                        ));
                        break;
                    }
                    failure = Some(e.at_step(&step.id));
                    break;
                }
            }
        }

        match success {
            Some(value) => {
                results.insert(step.id.clone(), value);
                order.push(step.id.clone());
            }
            None => {
                let error = failure.unwrap_or_else(|| {
                    WfError::new(
                        STEP_FAILED,
                        Some(step.id.clone()),
                        format!("step '{}' failed", step.id),
                    )
                });
                return Execution {
                    results,
                    order,
                    skipped: skipped_order,
                    error: Some(error),
                };
            }
        }
    }

    Execution {
        results,
        order,
        skipped: skipped_order,
        error: None,
    }
}
