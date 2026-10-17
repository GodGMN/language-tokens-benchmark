// Workflow validation, reference/condition evaluation and execution.
use crate::error::{Code, StepErr, ValidationErr};
use crate::ops;
use crate::value_util::{cmp_values, split_ref, structural_eq};
use serde_json::{Map, Value};
use std::cmp::Ordering;
use std::collections::{HashMap, HashSet};

const KNOWN_OPS: &[&str] = &[
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

pub struct StepDef {
    pub id: String,
    pub op: String,
    pub args: Map<String, Value>,
    pub depends: Vec<String>,
    pub when: Option<Value>,
    pub max_attempts: u32,
}

pub struct Workflow {
    pub steps: Vec<StepDef>,
}

#[derive(Clone)]
enum Outcome {
    Value(Value),
    Skipped,
}

fn invalid(step: Option<String>, message: impl Into<String>) -> ValidationErr {
    ValidationErr::new(Code::InvalidWorkflow, step, message)
}

fn valid_id(s: &str) -> bool {
    let bytes = s.as_bytes();
    if bytes.is_empty() {
        return false;
    }
    let first = bytes[0];
    if !(first.is_ascii_alphabetic() || first == b'_') {
        return false;
    }
    bytes[1..]
        .iter()
        .all(|b| b.is_ascii_alphanumeric() || *b == b'_')
}

fn is_ref_object(v: &Value) -> bool {
    match v.as_object() {
        Some(m) => m.len() == 1 && m.get("$ref").map(|x| x.is_string()).unwrap_or(false),
        None => false,
    }
}

fn validate_condition_shape(v: &Value) -> Result<(), String> {
    let m = match v.as_object() {
        Some(m) => m,
        None => {
            return if v.is_boolean() {
                Ok(())
            } else {
                Err("condition must be an object or boolean".to_string())
            };
        }
    };
    let opv = match m.get("op") {
        Some(opv) => opv,
        None => {
            // A bare reference is a value expression whose result must be a
            // boolean at runtime.
            return if is_ref_object(v) {
                Ok(())
            } else {
                Err("condition must have an op".to_string())
            };
        }
    };
    let op = opv
        .as_str()
        .ok_or_else(|| "condition op must be a string".to_string())?;
    match op {
        "eq" | "ne" | "gt" | "gte" | "lt" | "lte" => {
            for k in m.keys() {
                if !matches!(k.as_str(), "op" | "left" | "right") {
                    return Err(format!("unknown condition field: {k}"));
                }
            }
            if !m.contains_key("left") || !m.contains_key("right") {
                return Err(format!("condition {op} requires left and right"));
            }
            Ok(())
        }
        "all" | "any" => {
            for k in m.keys() {
                if !matches!(k.as_str(), "op" | "conditions") {
                    return Err(format!("unknown condition field: {k}"));
                }
            }
            let conds = m
                .get("conditions")
                .and_then(|x| x.as_array())
                .ok_or_else(|| format!("condition {op} requires a conditions array"))?;
            for c in conds {
                validate_condition_shape(c)?;
            }
            Ok(())
        }
        "not" => {
            for k in m.keys() {
                if !matches!(k.as_str(), "op" | "condition") {
                    return Err(format!("unknown condition field: {k}"));
                }
            }
            let c = m
                .get("condition")
                .ok_or_else(|| "condition not requires condition".to_string())?;
            validate_condition_shape(c)
        }
        _ => Err(format!("unknown condition op: {op}")),
    }
}

pub fn validate(root: &Value) -> Result<Workflow, ValidationErr> {
    let obj = root
        .as_object()
        .ok_or_else(|| invalid(None, "workflow must be a JSON object"))?;

    // Unknown top-level fields are rejected.
    for k in obj.keys() {
        if k != "version" && k != "steps" {
            return Err(invalid(None, format!("unknown top-level field: {k}")));
        }
    }

    let version = obj
        .get("version")
        .ok_or_else(|| invalid(None, "missing required field: version"))?;
    // Must be the integer literal 1; 1.0 and 1e0 are rejected because their
    // Number representation is a float.
    if version.as_i64() != Some(1) {
        return Err(invalid(None, "version must be the integer 1"));
    }

    let raw_steps = obj
        .get("steps")
        .ok_or_else(|| invalid(None, "missing required field: steps"))?;
    let raw_steps = raw_steps
        .as_array()
        .ok_or_else(|| invalid(None, "steps must be an array"))?;

    let mut steps: Vec<StepDef> = Vec::with_capacity(raw_steps.len());
    let mut seen: HashMap<String, usize> = HashMap::new();

    for raw in raw_steps {
        let sobj = raw
            .as_object()
            .ok_or_else(|| invalid(None, "each step must be an object"))?;
        let label = sobj
            .get("id")
            .and_then(|v| v.as_str())
            .map(|s| s.to_string());

        // Unknown step fields.
        for k in sobj.keys() {
            if !matches!(
                k.as_str(),
                "id" | "op" | "args" | "depends_on" | "when" | "retry"
            ) {
                return Err(invalid(label.clone(), format!("unknown step field: {k}")));
            }
        }

        let id = sobj
            .get("id")
            .and_then(|v| v.as_str())
            .ok_or_else(|| invalid(label.clone(), "step id must be a string"))?
            .to_string();
        if !valid_id(&id) {
            return Err(invalid(Some(id), "invalid step id"));
        }
        if seen.contains_key(&id) {
            return Err(invalid(Some(id.clone()), format!("duplicate step id: {id}")));
        }

        let op = sobj
            .get("op")
            .and_then(|v| v.as_str())
            .ok_or_else(|| invalid(Some(id.clone()), "step op must be a string"))?
            .to_string();
        if !KNOWN_OPS.contains(&op.as_str()) {
            return Err(invalid(Some(id.clone()), format!("unknown op: {op}")));
        }

        let args = match sobj.get("args") {
            None => Map::new(),
            Some(v) => v
                .as_object()
                .ok_or_else(|| invalid(Some(id.clone()), "step args must be an object"))?
                .clone(),
        };

        let depends = match sobj.get("depends_on") {
            None => Vec::new(),
            Some(v) => {
                let arr = v
                    .as_array()
                    .ok_or_else(|| invalid(Some(id.clone()), "depends_on must be an array"))?;
                let mut deps = Vec::with_capacity(arr.len());
                for d in arr {
                    let ds = d.as_str().ok_or_else(|| {
                        invalid(Some(id.clone()), "depends_on entries must be strings")
                    })?;
                    deps.push(ds.to_string());
                }
                deps
            }
        };

        let when = match sobj.get("when") {
            None => None,
            Some(v) => {
                if v.is_boolean() {
                    // A literal boolean is already a strict condition result.
                } else if v.is_object() {
                    validate_condition_shape(v)
                        .map_err(|m| invalid(Some(id.clone()), format!("invalid when: {m}")))?;
                } else {
                    return Err(invalid(
                        Some(id.clone()),
                        "when must be a condition object or boolean",
                    ));
                }
                Some(v.clone())
            }
        };

        let max_attempts = match sobj.get("retry") {
            None => 1,
            Some(v) => {
                let robj = v
                    .as_object()
                    .ok_or_else(|| invalid(Some(id.clone()), "retry must be an object"))?;
                for k in robj.keys() {
                    if k != "max_attempts" {
                        return Err(invalid(
                            Some(id.clone()),
                            format!("unknown retry field: {k}"),
                        ));
                    }
                }
                match robj.get("max_attempts") {
                    None => 1,
                    Some(n) => match n.as_i64() {
                        Some(x) if (1..=5).contains(&x) => x as u32,
                        _ => {
                            return Err(invalid(
                                Some(id.clone()),
                                "max_attempts must be an integer between 1 and 5",
                            ))
                        }
                    },
                }
            }
        };

        seen.insert(id.clone(), steps.len());
        steps.push(StepDef {
            id,
            op,
            args,
            depends,
            when,
            max_attempts,
        });
    }

    // All dependencies must exist.
    for st in &steps {
        for dep in &st.depends {
            if !seen.contains_key(dep) {
                return Err(ValidationErr::new(
                    Code::UnknownDependency,
                    Some(st.id.clone()),
                    format!("unknown dependency: {dep}"),
                ));
            }
        }
    }

    // No cycles.
    if has_cycle(&steps, &seen) {
        return Err(ValidationErr::new(
            Code::CyclicDependency,
            None,
            "dependency cycle detected",
        ));
    }

    Ok(Workflow { steps })
}

fn has_cycle(steps: &[StepDef], index: &HashMap<String, usize>) -> bool {
    fn visit(
        i: usize,
        steps: &[StepDef],
        index: &HashMap<String, usize>,
        color: &mut Vec<u8>,
    ) -> bool {
        color[i] = 1;
        for dep in &steps[i].depends {
            if let Some(&j) = index.get(dep) {
                if color[j] == 1 {
                    return true;
                }
                if color[j] == 0 && visit(j, steps, index, color) {
                    return true;
                }
            }
        }
        color[i] = 2;
        false
    }

    let mut color = vec![0u8; steps.len()];
    for i in 0..steps.len() {
        if color[i] == 0 && visit(i, steps, index, &mut color) {
            return true;
        }
    }
    false
}

fn compute_closure(
    i: usize,
    steps: &[StepDef],
    index: &HashMap<String, usize>,
    memo: &mut Vec<Option<HashSet<String>>>,
) -> HashSet<String> {
    if let Some(s) = &memo[i] {
        return s.clone();
    }
    let mut set: HashSet<String> = HashSet::new();
    for dep in &steps[i].depends {
        set.insert(dep.clone());
        if let Some(&j) = index.get(dep) {
            for x in compute_closure(j, steps, index, memo) {
                set.insert(x);
            }
        }
    }
    memo[i] = Some(set.clone());
    set
}

struct Ctx<'a> {
    allowed: &'a HashSet<String>,
    outcomes: &'a HashMap<String, Outcome>,
}

fn resolve_ref(path: &str, ctx: &Ctx) -> Result<Value, StepErr> {
    let (step_id, segments) = split_ref(path);
    if step_id.is_empty() {
        return Err(StepErr::new(Code::InvalidReference, "empty reference"));
    }
    if !ctx.allowed.contains(&step_id) {
        return Err(StepErr::new(
            Code::InvalidReference,
            format!("reference to non-dependency step: {step_id}"),
        ));
    }
    let base = match ctx.outcomes.get(&step_id) {
        None => {
            return Err(StepErr::new(
                Code::InvalidReference,
                format!("referenced step not executed: {step_id}"),
            ))
        }
        Some(Outcome::Skipped) => {
            return Err(StepErr::new(
                Code::SkippedReference,
                format!("reference to skipped step: {step_id}"),
            ))
        }
        Some(Outcome::Value(v)) => v.clone(),
    };

    let mut cur = base;
    for seg in &segments {
        cur = match cur {
            Value::Object(m) => match m.get(seg) {
                Some(v) => v.clone(),
                None => {
                    return Err(StepErr::new(
                        Code::InvalidReference,
                        format!("missing object key in reference: {seg}"),
                    ))
                }
            },
            Value::Array(a) => {
                if seg.is_empty() || !seg.bytes().all(|b| b.is_ascii_digit()) {
                    return Err(StepErr::new(
                        Code::InvalidReference,
                        format!("invalid array index in reference: {seg}"),
                    ));
                }
                let idx: usize = seg.parse().map_err(|_| {
                    StepErr::new(Code::InvalidReference, "array index out of range in reference")
                })?;
                match a.get(idx) {
                    Some(v) => v.clone(),
                    None => {
                        return Err(StepErr::new(
                            Code::InvalidReference,
                            format!("array index out of range in reference: {idx}"),
                        ))
                    }
                }
            }
            _ => {
                return Err(StepErr::new(
                    Code::InvalidReference,
                    "cannot index into a scalar in reference",
                ))
            }
        };
    }
    Ok(cur)
}

fn resolve_value(v: &Value, ctx: &Ctx) -> Result<Value, StepErr> {
    match v {
        Value::Object(m) => {
            if m.len() == 1 {
                if let Some(Value::String(path)) = m.get("$ref") {
                    return resolve_ref(path, ctx);
                }
            }
            let mut out = Map::new();
            for (k, val) in m {
                out.insert(k.clone(), resolve_value(val, ctx)?);
            }
            Ok(Value::Object(out))
        }
        Value::Array(a) => {
            let mut out = Vec::with_capacity(a.len());
            for x in a {
                out.push(resolve_value(x, ctx)?);
            }
            Ok(Value::Array(out))
        }
        _ => Ok(v.clone()),
    }
}

fn eval_condition(v: &Value, ctx: &Ctx) -> Result<bool, StepErr> {
    if let Value::Object(m) = v {
        if let Some(opv) = m.get("op") {
            let op = opv.as_str().ok_or_else(|| {
                StepErr::new(Code::InvalidCondition, "condition op must be a string")
            })?;
            match op {
                "eq" | "ne" | "gt" | "gte" | "lt" | "lte" => {
                    let left = m.get("left").ok_or_else(|| {
                        StepErr::new(Code::InvalidCondition, "condition missing left")
                    })?;
                    let right = m.get("right").ok_or_else(|| {
                        StepErr::new(Code::InvalidCondition, "condition missing right")
                    })?;
                    let lv = resolve_value(left, ctx)?;
                    let rv = resolve_value(right, ctx)?;
                    let result = match op {
                        "eq" => structural_eq(&lv, &rv),
                        "ne" => !structural_eq(&lv, &rv),
                        "gt" => cmp_values(&lv, &rv) == Ordering::Greater,
                        "gte" => cmp_values(&lv, &rv) != Ordering::Less,
                        "lt" => cmp_values(&lv, &rv) == Ordering::Less,
                        _ => cmp_values(&lv, &rv) != Ordering::Greater,
                    };
                    return Ok(result);
                }
                "all" => {
                    let conds = m.get("conditions").and_then(|x| x.as_array()).ok_or_else(
                        || {
                            StepErr::new(
                                Code::InvalidCondition,
                                "condition all missing conditions array",
                            )
                        },
                    )?;
                    for c in conds {
                        if !eval_condition(c, ctx)? {
                            return Ok(false);
                        }
                    }
                    return Ok(true);
                }
                "any" => {
                    let conds = m.get("conditions").and_then(|x| x.as_array()).ok_or_else(
                        || {
                            StepErr::new(
                                Code::InvalidCondition,
                                "condition any missing conditions array",
                            )
                        },
                    )?;
                    for c in conds {
                        if eval_condition(c, ctx)? {
                            return Ok(true);
                        }
                    }
                    return Ok(false);
                }
                "not" => {
                    let c = m.get("condition").ok_or_else(|| {
                        StepErr::new(Code::InvalidCondition, "condition not missing condition")
                    })?;
                    return Ok(!eval_condition(c, ctx)?);
                }
                _ => {
                    return Err(StepErr::new(
                        Code::InvalidCondition,
                        format!("unknown condition op: {op}"),
                    ))
                }
            }
        }
    }
    let resolved = resolve_value(v, ctx)?;
    resolved.as_bool().ok_or_else(|| {
        StepErr::new(
            Code::InvalidCondition,
            "when expression did not evaluate to a boolean",
        )
    })
}

pub struct FailureInfo {
    pub code: Code,
    pub step: Option<String>,
    pub message: String,
}

pub struct ExecResult {
    pub results: Map<String, Value>,
    pub order: Vec<String>,
    pub skipped: Vec<String>,
    pub error: Option<FailureInfo>,
}

pub fn execute(wf: &Workflow) -> ExecResult {
    let steps = &wf.steps;
    let n = steps.len();
    let mut index: HashMap<String, usize> = HashMap::with_capacity(n);
    for (i, s) in steps.iter().enumerate() {
        index.insert(s.id.clone(), i);
    }

    let mut memo: Vec<Option<HashSet<String>>> = vec![None; n];
    let mut closures: Vec<HashSet<String>> = Vec::with_capacity(n);
    for i in 0..n {
        closures.push(compute_closure(i, steps, &index, &mut memo));
    }

    let mut done = vec![false; n];
    let mut outcomes: HashMap<String, Outcome> = HashMap::new();
    let mut results: Map<String, Value> = Map::new();
    let mut order: Vec<String> = Vec::new();
    let mut skipped: Vec<String> = Vec::new();
    let mut remaining = n;

    while remaining > 0 {
        // Deterministic tie-break: earliest step in the steps array among ready
        // steps.
        let mut pick: Option<usize> = None;
        for i in 0..n {
            if done[i] {
                continue;
            }
            let ready = steps[i].depends.iter().all(|d| {
                index
                    .get(d)
                    .map(|&j| done[j])
                    .unwrap_or(false)
            });
            if ready {
                pick = Some(i);
                break;
            }
        }

        let i = match pick {
            Some(i) => i,
            None => {
                // Should be impossible after cycle validation, but keep the
                // engine total.
                return ExecResult {
                    results,
                    order,
                    skipped,
                    error: Some(FailureInfo {
                        code: Code::CyclicDependency,
                        step: None,
                        message: "no ready step (cycle)".to_string(),
                    }),
                };
            }
        };
        done[i] = true;
        remaining -= 1;

        let st = &steps[i];

        enum StepRun {
            Done(Value),
            Skipped,
        }

        // Evaluate `when`, then resolve args and run the operation while the
        // context borrow is alive; the context drops before we mutate state.
        let step_result: Result<StepRun, StepErr> = {
            let ctx = Ctx {
                allowed: &closures[i],
                outcomes: &outcomes,
            };
            (|| {
                if let Some(cond) = &st.when {
                    if !eval_condition(cond, &ctx)? {
                        return Ok(StepRun::Skipped);
                    }
                }
                let resolved = resolve_value(&Value::Object(st.args.clone()), &ctx)?;
                let arg_map = match resolved {
                    Value::Object(m) => m,
                    _ => Map::new(),
                };
                let mut attempt: u32 = 0;
                loop {
                    attempt += 1;
                    match ops::execute(&st.op, &arg_map, attempt) {
                        Ok(v) => return Ok(StepRun::Done(v)),
                        Err(e) => {
                            if e.code == Code::TransientFailure && attempt < st.max_attempts {
                                continue;
                            }
                            let code = if e.code == Code::TransientFailure {
                                Code::StepFailed
                            } else {
                                e.code
                            };
                            return Err(StepErr::new(code, e.message));
                        }
                    }
                }
            })()
        };

        match step_result {
            Ok(StepRun::Done(v)) => {
                outcomes.insert(st.id.clone(), Outcome::Value(v.clone()));
                results.insert(st.id.clone(), v);
                order.push(st.id.clone());
            }
            Ok(StepRun::Skipped) => {
                outcomes.insert(st.id.clone(), Outcome::Skipped);
                results.insert(st.id.clone(), Value::Null);
                order.push(st.id.clone());
                skipped.push(st.id.clone());
            }
            Err(e) => {
                return ExecResult {
                    results,
                    order,
                    skipped,
                    error: Some(FailureInfo {
                        code: e.code,
                        step: Some(st.id.clone()),
                        message: e.message,
                    }),
                };
            }
        }
    }

    ExecResult {
        results,
        order,
        skipped,
        error: None,
    }
}

fn error_json(code: Code, step: Option<String>, message: &str) -> Value {
    let mut m = Map::new();
    m.insert("code".to_string(), Value::String(code.as_str().to_string()));
    m.insert(
        "step".to_string(),
        match step {
            Some(s) => Value::String(s),
            None => Value::Null,
        },
    );
    m.insert("message".to_string(), Value::String(message.to_string()));
    Value::Object(m)
}

pub fn make_response(
    results: Map<String, Value>,
    order: Vec<String>,
    skipped: Vec<String>,
    error: Option<(Code, Option<String>, String)>,
) -> Value {
    let mut m = Map::new();
    let status = if error.is_some() { "failed" } else { "completed" };
    m.insert("status".to_string(), Value::String(status.to_string()));
    m.insert("results".to_string(), Value::Object(results));
    m.insert(
        "execution_order".to_string(),
        Value::Array(order.into_iter().map(Value::String).collect()),
    );
    m.insert(
        "skipped".to_string(),
        Value::Array(skipped.into_iter().map(Value::String).collect()),
    );
    m.insert(
        "error".to_string(),
        match error {
            None => Value::Null,
            Some((code, step, msg)) => error_json(code, step, &msg),
        },
    );
    Value::Object(m)
}

/// Validate and execute a workflow document. Returns the response document and
/// the process exit code.
pub fn run(root: &Value) -> (Value, i32) {
    match validate(root) {
        Err(e) => (
            make_response(
                Map::new(),
                Vec::new(),
                Vec::new(),
                Some((e.code, e.step, e.message)),
            ),
            2,
        ),
        Ok(wf) => {
            let r = execute(&wf);
            let (code, step, msg) = match r.error {
                Some(f) => (Some(f.code), f.step, f.message),
                None => (None, None, String::new()),
            };
            let exit = if code.is_some() { 1 } else { 0 };
            (
                make_response(
                    r.results,
                    r.order,
                    r.skipped,
                    code.map(|c| (c, step, msg)),
                ),
                exit,
            )
        }
    }
}
