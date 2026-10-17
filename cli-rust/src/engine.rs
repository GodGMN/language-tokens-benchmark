use std::cmp::Ordering;
use std::collections::{HashMap, HashSet};

use serde_json::{json, Map, Value};

use crate::ops::{execute_op, OpError};
use crate::value_util::{compare_values, structural_eq};
use crate::workflow::{self, CmpOp, Condition, StepDef, Workflow};

#[derive(Debug, Clone)]
pub struct ErrorInfo {
    pub code: &'static str,
    pub step: Option<String>,
    pub message: String,
}

#[derive(Debug)]
pub struct Response {
    pub status: &'static str,
    pub results: Map<String, Value>,
    pub execution_order: Vec<String>,
    pub skipped: Vec<String>,
    pub error: Option<ErrorInfo>,
}

impl Response {
    pub fn failed_empty(
        code: &'static str,
        step: Option<String>,
        message: String,
    ) -> Self {
        Self {
            status: "failed",
            results: Map::new(),
            execution_order: Vec::new(),
            skipped: Vec::new(),
            error: Some(ErrorInfo {
                code,
                step,
                message,
            }),
        }
    }

    pub fn to_json(&self) -> Value {
        let mut results = Map::new();
        for (key, value) in &self.results {
            results.insert(key.clone(), value.clone());
        }
        let error = match &self.error {
            None => Value::Null,
            Some(e) => json!({
                "code": e.code,
                "step": e.step,
                "message": e.message,
            }),
        };
        json!({
            "status": self.status,
            "results": Value::Object(results),
            "execution_order": self.execution_order.clone(),
            "skipped": self.skipped.clone(),
            "error": error,
        })
    }
}

pub fn run_text(text: &str) -> (Response, i32) {
    let doc: Value = match serde_json::from_str(text) {
        Ok(v) => v,
        Err(e) => {
            return (
                Response::failed_empty(
                    "INVALID_WORKFLOW",
                    None,
                    format!("invalid JSON: {e}"),
                ),
                2,
            )
        }
    };
    match workflow::validate(&doc) {
        Ok(wf) => execute(&wf),
        Err(e) => (
            Response::failed_empty(e.code, e.step, e.message),
            2,
        ),
    }
}

struct Exec<'a> {
    wf: &'a Workflow,
    index: HashMap<String, usize>,
    closure: Vec<HashSet<usize>>,
    executed: Vec<bool>,
    results: Vec<Option<Value>>,
    skipped: Vec<bool>,
    execution_order: Vec<String>,
    skipped_order: Vec<String>,
}

fn build_closure(wf: &Workflow, index: &HashMap<String, usize>) -> Vec<HashSet<usize>> {
    let mut closures = Vec::with_capacity(wf.steps.len());
    for step in &wf.steps {
        let mut set = HashSet::new();
        let mut stack: Vec<String> = step.deps.clone();
        while let Some(dep) = stack.pop() {
            let Some(&j) = index.get(&dep) else {
                continue;
            };
            if set.insert(j) {
                stack.extend(wf.steps[j].deps.iter().cloned());
            }
        }
        closures.push(set);
    }
    closures
}

fn execute(wf: &Workflow) -> (Response, i32) {
    let index: HashMap<String, usize> = wf
        .steps
        .iter()
        .enumerate()
        .map(|(i, s)| (s.id.clone(), i))
        .collect();
    let closure = build_closure(wf, &index);
    let n = wf.steps.len();
    let mut exec = Exec {
        wf,
        index,
        closure,
        executed: vec![false; n],
        results: vec![None; n],
        skipped: vec![false; n],
        execution_order: Vec::with_capacity(n),
        skipped_order: Vec::new(),
    };

    for _ in 0..n {
        let Some(i) = exec.next_ready() else {
            let mut resp = exec.response();
            resp.status = "failed";
            resp.error = Some(ErrorInfo {
                code: "INTERNAL_ERROR",
                step: None,
                message: "no executable step remains".to_string(),
            });
            return (resp, 3);
        };

        let step = &wf.steps[i];

        if let Some(condition) = &step.when {
            match exec.eval_condition(i, condition) {
                Ok(true) => {}
                Ok(false) => {
                    exec.record_skipped(i);
                    continue;
                }
                Err(e) => return exec.step_failure(step, e),
            }
        }

        let args_value = Value::Object(step.args.clone());
        let resolved = match exec.resolve_value(i, &args_value) {
            Ok(v) => v,
            Err(e) => return exec.step_failure(step, e),
        };
        let args_map = match resolved.as_object() {
            Some(m) => m.clone(),
            None => {
                return exec.step_failure(
                    step,
                    OpError::new("INVALID_ARG", "args must resolve to an object"),
                )
            }
        };

        let mut attempt: u32 = 1;
        let value = loop {
            match execute_op(&step.op, &args_map, attempt) {
                Ok(v) => break v,
                Err(e) => {
                    if e.code == "TRANSIENT_FAILURE" {
                        if attempt < step.max_attempts {
                            attempt += 1;
                            continue;
                        }
                        return exec.step_failure(
                            step,
                            OpError::new("STEP_FAILED", "retries exhausted"),
                        );
                    }
                    return exec.step_failure(step, e);
                }
            }
        };

        exec.record_completed(i, value);
    }

    (exec.response(), 0)
}

impl<'a> Exec<'a> {
    fn next_ready(&self) -> Option<usize> {
        for (i, step) in self.wf.steps.iter().enumerate() {
            if !self.executed[i]
                && step
                    .deps
                    .iter()
                    .all(|dep| self.executed[self.index[dep]])
            {
                return Some(i);
            }
        }
        None
    }

    fn record_completed(&mut self, i: usize, value: Value) {
        self.executed[i] = true;
        self.results[i] = Some(value);
        self.execution_order.push(self.wf.steps[i].id.clone());
    }

    fn record_skipped(&mut self, i: usize) {
        self.executed[i] = true;
        self.results[i] = Some(Value::Null);
        self.skipped[i] = true;
        let id = self.wf.steps[i].id.clone();
        self.execution_order.push(id.clone());
        self.skipped_order.push(id);
    }

    fn response(&self) -> Response {
        let mut results = Map::new();
        for (i, step) in self.wf.steps.iter().enumerate() {
            if self.executed[i] {
                results.insert(
                    step.id.clone(),
                    self.results[i].clone().unwrap_or(Value::Null),
                );
            }
        }
        Response {
            status: "completed",
            results,
            execution_order: self.execution_order.clone(),
            skipped: self.skipped_order.clone(),
            error: None,
        }
    }

    fn step_failure(&self, step: &StepDef, error: OpError) -> (Response, i32) {
        let mut resp = self.response();
        resp.status = "failed";
        resp.error = Some(ErrorInfo {
            code: error.code,
            step: Some(step.id.clone()),
            message: error.message,
        });
        (resp, 1)
    }

    fn resolve_value(&self, current: usize, value: &Value) -> Result<Value, OpError> {
        if let Value::Object(map) = value {
            if map.len() == 1 {
                if let Some(target) = map.get("$ref").and_then(Value::as_str) {
                    return self.resolve_ref(current, target);
                }
            }
            let mut out = Map::new();
            for (key, item) in map {
                out.insert(key.clone(), self.resolve_value(current, item)?);
            }
            return Ok(Value::Object(out));
        }
        if let Value::Array(items) = value {
            let mut out = Vec::with_capacity(items.len());
            for item in items {
                out.push(self.resolve_value(current, item)?);
            }
            return Ok(Value::Array(out));
        }
        Ok(value.clone())
    }

    fn resolve_ref(&self, current: usize, target: &str) -> Result<Value, OpError> {
        let mut segments = target.split('.');
        let step_id = match segments.next() {
            Some(s) if !s.is_empty() => s,
            _ => {
                return Err(OpError::new(
                    "INVALID_REFERENCE",
                    format!("invalid reference: {target}"),
                ))
            }
        };
        let Some(&target_index) = self.index.get(step_id) else {
            return Err(OpError::new(
                "INVALID_REFERENCE",
                format!("reference to unknown step: {step_id}"),
            ));
        };
        if target_index == current || !self.closure[current].contains(&target_index) {
            return Err(OpError::new(
                "INVALID_REFERENCE",
                format!("reference is not a dependency: {step_id}"),
            ));
        }
        if !self.executed[target_index] {
            return Err(OpError::new(
                "INVALID_REFERENCE",
                format!("reference target has not finished: {step_id}"),
            ));
        }
        if self.skipped[target_index] {
            return Err(OpError::new(
                "SKIPPED_REFERENCE",
                format!("reference to skipped step: {step_id}"),
            ));
        }

        let mut value = self.results[target_index].clone().unwrap_or(Value::Null);
        for segment in segments {
            value = match value {
                Value::Object(map) => match map.get(segment) {
                    Some(v) => v.clone(),
                    None => {
                        return Err(OpError::new(
                            "INVALID_REFERENCE",
                            format!("missing object key in reference: {segment}"),
                        ))
                    }
                },
                Value::Array(items) => {
                    if segment.is_empty() || !segment.bytes().all(|b| b.is_ascii_digit()) {
                        return Err(OpError::new(
                            "INVALID_REFERENCE",
                            format!("invalid array index in reference: {segment}"),
                        ));
                    }
                    let idx: usize = match segment.parse() {
                        Ok(i) => i,
                        Err(_) => {
                            return Err(OpError::new(
                                "INVALID_REFERENCE",
                                format!("invalid array index in reference: {segment}"),
                            ))
                        }
                    };
                    match items.get(idx) {
                        Some(v) => v.clone(),
                        None => {
                            return Err(OpError::new(
                                "INVALID_REFERENCE",
                                format!("array index out of range in reference: {segment}"),
                            ))
                        }
                    }
                }
                _ => {
                    return Err(OpError::new(
                        "INVALID_REFERENCE",
                        format!("cannot traverse reference segment: {segment}"),
                    ))
                }
            };
        }
        Ok(value)
    }

    fn eval_condition(&self, current: usize, cond: &Condition) -> Result<bool, OpError> {
        match cond {
            Condition::Compare { op, left, right } => {
                let left = self.resolve_value(current, left)?;
                let right = self.resolve_value(current, right)?;
                let result = match op {
                    CmpOp::Eq => structural_eq(&left, &right),
                    CmpOp::Ne => !structural_eq(&left, &right),
                    CmpOp::Gt => compare_values(&left, &right) == Ordering::Greater,
                    CmpOp::Gte => {
                        matches!(
                            compare_values(&left, &right),
                            Ordering::Greater | Ordering::Equal
                        )
                    }
                    CmpOp::Lt => compare_values(&left, &right) == Ordering::Less,
                    CmpOp::Lte => {
                        matches!(
                            compare_values(&left, &right),
                            Ordering::Less | Ordering::Equal
                        )
                    }
                };
                Ok(result)
            }
            Condition::All(list) => {
                for item in list {
                    if !self.eval_condition(current, item)? {
                        return Ok(false);
                    }
                }
                Ok(true)
            }
            Condition::Any(list) => {
                for item in list {
                    if self.eval_condition(current, item)? {
                        return Ok(true);
                    }
                }
                Ok(false)
            }
            Condition::Not(inner) => Ok(!self.eval_condition(current, inner)?),
            Condition::Ref(target) => {
                let value = self.resolve_ref(current, target)?;
                match value.as_bool() {
                    Some(b) => Ok(b),
                    None => Err(OpError::new(
                        "INVALID_CONDITION",
                        "condition did not evaluate to a boolean",
                    )),
                }
            }
        }
    }
}
