//! `$ref` resolution.

use crate::error::{invalid_reference, skipped_reference, WfError};
use crate::validate::Workflow;
use serde_json::{Map, Value};
use std::collections::{BTreeMap, BTreeSet};

pub struct RefContext<'a> {
    pub wf: &'a Workflow,
    pub current: usize,
    pub results: &'a BTreeMap<String, Value>,
    pub skipped: &'a BTreeSet<String>,
}

impl<'a> RefContext<'a> {
    /// Recursively resolve references inside a value.
    pub fn resolve(&self, v: &Value) -> Result<Value, WfError> {
        match v {
            Value::Object(o) => {
                if o.len() == 1 {
                    if let Some(Value::String(s)) = o.get("$ref") {
                        return self.resolve_ref(s);
                    }
                }
                let mut out = Map::new();
                for (k, val) in o {
                    out.insert(k.clone(), self.resolve(val)?);
                }
                Ok(Value::Object(out))
            }
            Value::Array(a) => {
                let mut out = Vec::with_capacity(a.len());
                for val in a {
                    out.push(self.resolve(val)?);
                }
                Ok(Value::Array(out))
            }
            other => Ok(other.clone()),
        }
    }

    fn resolve_ref(&self, reference: &str) -> Result<Value, WfError> {
        let mut parts = reference.splitn(2, '.');
        let target = parts.next().unwrap_or("");
        let path = parts.next();

        if target.is_empty() {
            return Err(invalid_reference("empty reference target"));
        }
        if target == self.wf.steps[self.current].id {
            return Err(invalid_reference(format!(
                "step '{}' cannot reference itself",
                target
            )));
        }
        if !self.wf.ancestors[self.current].contains(target) {
            return Err(invalid_reference(format!(
                "'{}' is not a dependency of step '{}'",
                reference, self.wf.steps[self.current].id
            )));
        }
        if self.skipped.contains(target) {
            return Err(skipped_reference(format!(
                "step '{}' was skipped and cannot be referenced",
                target
            )));
        }
        let mut current = match self.results.get(target) {
            Some(v) => v.clone(),
            None => {
                return Err(invalid_reference(format!(
                    "result of step '{}' is not available",
                    target
                )))
            }
        };

        if let Some(path) = path {
            for segment in path.split('.') {
                match current {
                    Value::Array(arr) => {
                        if segment.is_empty()
                            || !segment.bytes().all(|b| b.is_ascii_digit())
                        {
                            return Err(invalid_reference(format!(
                                "invalid array index '{}' in reference '{}'",
                                segment, reference
                            )));
                        }
                        let index: usize = segment.parse().map_err(|_| {
                            invalid_reference(format!(
                                "array index '{}' out of range in reference '{}'",
                                segment, reference
                            ))
                        })?;
                        current = arr.get(index).cloned().ok_or_else(|| {
                            invalid_reference(format!(
                                "array index '{}' out of bounds in reference '{}'",
                                segment, reference
                            ))
                        })?;
                    }
                    Value::Object(obj) => {
                        current = obj.get(segment).cloned().ok_or_else(|| {
                            invalid_reference(format!(
                                "missing key '{}' in reference '{}'",
                                segment, reference
                            ))
                        })?;
                    }
                    _ => {
                        return Err(invalid_reference(format!(
                            "cannot descend into a scalar in reference '{}'",
                            reference
                        )))
                    }
                }
            }
        }

        Ok(current)
    }
}
