//! Error type and error-code constants shared across the engine.

pub const INVALID_WORKFLOW: &str = "INVALID_WORKFLOW";
pub const UNKNOWN_DEPENDENCY: &str = "UNKNOWN_DEPENDENCY";
pub const CYCLIC_DEPENDENCY: &str = "CYCLIC_DEPENDENCY";
pub const INVALID_REFERENCE: &str = "INVALID_REFERENCE";
pub const SKIPPED_REFERENCE: &str = "SKIPPED_REFERENCE";
pub const INVALID_ARG: &str = "INVALID_ARG";
pub const INVALID_CONDITION: &str = "INVALID_CONDITION";
pub const ARITHMETIC_OVERFLOW: &str = "ARITHMETIC_OVERFLOW";
pub const DIVISION_BY_ZERO: &str = "DIVISION_BY_ZERO";
pub const EMPTY_ARRAY: &str = "EMPTY_ARRAY";
pub const TRANSIENT_FAILURE: &str = "TRANSIENT_FAILURE";
pub const STEP_FAILED: &str = "STEP_FAILED";
pub const INTERNAL_ERROR: &str = "INTERNAL_ERROR";

/// A workflow error. `step` is filled in by the engine for runtime failures.
#[derive(Debug, Clone)]
pub struct WfError {
    pub code: &'static str,
    pub step: Option<String>,
    pub message: String,
}

impl WfError {
    pub fn new(code: &'static str, step: Option<String>, message: impl Into<String>) -> Self {
        WfError {
            code,
            step,
            message: message.into(),
        }
    }

    /// An operation/argument error; the engine attaches the step id.
    pub fn op(code: &'static str, message: impl Into<String>) -> Self {
        WfError::new(code, None, message)
    }

    pub fn at_step(mut self, id: &str) -> Self {
        self.step = Some(id.to_string());
        self
    }
}

pub fn invalid_workflow(step: Option<String>, message: impl Into<String>) -> WfError {
    WfError::new(INVALID_WORKFLOW, step, message)
}

pub fn invalid_arg(message: impl Into<String>) -> WfError {
    WfError::op(INVALID_ARG, message)
}

pub fn invalid_reference(message: impl Into<String>) -> WfError {
    WfError::op(INVALID_REFERENCE, message)
}

pub fn skipped_reference(message: impl Into<String>) -> WfError {
    WfError::op(SKIPPED_REFERENCE, message)
}

pub fn invalid_condition(message: impl Into<String>) -> WfError {
    WfError::op(INVALID_CONDITION, message)
}

pub fn overflow(message: impl Into<String>) -> WfError {
    WfError::op(ARITHMETIC_OVERFLOW, message)
}

pub fn division_by_zero(message: impl Into<String>) -> WfError {
    WfError::op(DIVISION_BY_ZERO, message)
}

pub fn empty_array(message: impl Into<String>) -> WfError {
    WfError::op(EMPTY_ARRAY, message)
}
