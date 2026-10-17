// Error types shared by validation, execution and operations.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Code {
    InvalidWorkflow,
    UnknownDependency,
    CyclicDependency,
    InvalidReference,
    SkippedReference,
    InvalidArg,
    InvalidCondition,
    ArithmeticOverflow,
    DivisionByZero,
    EmptyArray,
    TransientFailure,
    StepFailed,
}

impl Code {
    pub fn as_str(self) -> &'static str {
        match self {
            Code::InvalidWorkflow => "INVALID_WORKFLOW",
            Code::UnknownDependency => "UNKNOWN_DEPENDENCY",
            Code::CyclicDependency => "CYCLIC_DEPENDENCY",
            Code::InvalidReference => "INVALID_REFERENCE",
            Code::SkippedReference => "SKIPPED_REFERENCE",
            Code::InvalidArg => "INVALID_ARG",
            Code::InvalidCondition => "INVALID_CONDITION",
            Code::ArithmeticOverflow => "ARITHMETIC_OVERFLOW",
            Code::DivisionByZero => "DIVISION_BY_ZERO",
            Code::EmptyArray => "EMPTY_ARRAY",
            Code::TransientFailure => "TRANSIENT_FAILURE",
            Code::StepFailed => "STEP_FAILED",
        }
    }
}

#[derive(Clone, Debug)]
pub struct StepErr {
    pub code: Code,
    pub message: String,
}

impl StepErr {
    pub fn new(code: Code, message: impl Into<String>) -> Self {
        StepErr {
            code,
            message: message.into(),
        }
    }
}

pub fn err<T>(code: Code, message: impl Into<String>) -> Result<T, StepErr> {
    Err(StepErr::new(code, message))
}

/// Errors produced while validating the workflow document.
#[derive(Clone, Debug)]
pub struct ValidationErr {
    pub code: Code,
    pub step: Option<String>,
    pub message: String,
}

impl ValidationErr {
    pub fn new(code: Code, step: Option<String>, message: impl Into<String>) -> Self {
        ValidationErr {
            code,
            step,
            message: message.into(),
        }
    }
}
