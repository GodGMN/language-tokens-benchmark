//! `workflow` — declarative workflow engine CLI.
//!
//! Usage:
//!   workflow run <workflow.json>
//!   workflow run -            (read the document from stdin)

mod engine;
mod error;
mod ops;
mod refs;
mod validate;
mod value;

use engine::Execution;
use error::{invalid_workflow, WfError, INTERNAL_ERROR};
use serde_json::{Map, Value};
use std::collections::BTreeMap;
use std::io::{Read, Write};

fn response_value(exec: &Execution) -> Value {
    let mut results = Map::new();
    for (k, v) in &exec.results {
        results.insert(k.clone(), v.clone());
    }
    let order = Value::Array(
        exec.order
            .iter()
            .map(|s| Value::String(s.clone()))
            .collect(),
    );
    let skipped = Value::Array(
        exec.skipped
            .iter()
            .map(|s| Value::String(s.clone()))
            .collect(),
    );
    let error = match &exec.error {
        None => Value::Null,
        Some(e) => {
            let mut obj = Map::new();
            obj.insert("code".to_string(), Value::String(e.code.to_string()));
            obj.insert(
                "step".to_string(),
                match &e.step {
                    Some(s) => Value::String(s.clone()),
                    None => Value::Null,
                },
            );
            obj.insert("message".to_string(), Value::String(e.message.clone()));
            Value::Object(obj)
        }
    };
    let mut root = Map::new();
    root.insert(
        "status".to_string(),
        Value::String(if exec.error.is_none() {
            "completed".to_string()
        } else {
            "failed".to_string()
        }),
    );
    root.insert("results".to_string(), Value::Object(results));
    root.insert("execution_order".to_string(), order);
    root.insert("skipped".to_string(), skipped);
    root.insert("error".to_string(), error);
    Value::Object(root)
}

fn failure_after_validation(err: WfError) -> Value {
    let exec = Execution {
        results: BTreeMap::new(),
        order: Vec::new(),
        skipped: Vec::new(),
        error: Some(err),
    };
    response_value(&exec)
}

fn emit(value: &Value) {
    match serde_json::to_string(value) {
        Ok(text) => {
            let stdout = std::io::stdout();
            let mut lock = stdout.lock();
            let _ = writeln!(lock, "{}", text);
            let _ = lock.flush();
        }
        Err(_) => {
            println!(
                "{{\"status\":\"failed\",\"results\":{{}},\"execution_order\":[],\"skipped\":[],\"error\":{{\"code\":\"INTERNAL_ERROR\",\"step\":null,\"message\":\"serialization failed\"}}}}"
            );
        }
    }
}

fn read_input(path: &str) -> Result<String, String> {
    if path == "-" {
        let mut buf = String::new();
        std::io::stdin()
            .read_to_string(&mut buf)
            .map_err(|e| format!("could not read stdin: {}", e))?;
        Ok(buf)
    } else {
        std::fs::read_to_string(path).map_err(|e| format!("could not read '{}': {}", path, e))
    }
}

fn run_cli() -> i32 {
    let args: Vec<String> = std::env::args().collect();
    let path = match (args.get(1), args.get(2), args.get(3)) {
        (Some(cmd), Some(path), None) if cmd == "run" => path.clone(),
        _ => {
            emit(&failure_after_validation(invalid_workflow(
                None,
                "usage: workflow run <workflow.json> | workflow run -",
            )));
            return 2;
        }
    };

    let raw = match read_input(&path) {
        Ok(text) => text,
        Err(msg) => {
            emit(&failure_after_validation(invalid_workflow(None, msg)));
            return 2;
        }
    };

    let document: Value = match serde_json::from_str(&raw) {
        Ok(v) => v,
        Err(e) => {
            emit(&failure_after_validation(invalid_workflow(
                None,
                format!("invalid JSON input: {}", e),
            )));
            return 2;
        }
    };

    let workflow = match validate::validate(&document) {
        Ok(wf) => wf,
        Err(e) => {
            emit(&failure_after_validation(e));
            return 2;
        }
    };

    let execution = engine::run(&workflow);
    let failed = execution.error.is_some();
    emit(&response_value(&execution));
    if failed {
        1
    } else {
        0
    }
}

fn main() {
    let outcome = std::panic::catch_unwind(std::panic::AssertUnwindSafe(run_cli));
    let code = match outcome {
        Ok(code) => code,
        Err(_) => {
            let exec = Execution {
                results: BTreeMap::new(),
                order: Vec::new(),
                skipped: Vec::new(),
                error: Some(WfError::new(
                    INTERNAL_ERROR,
                    None,
                    "internal program error",
                )),
            };
            emit(&response_value(&exec));
            3
        }
    };
    let _ = std::io::stdout().flush();
    std::process::exit(code);
}
