// CLI entry point: `workflow run <workflow.json>` or `workflow run -`.
mod engine;
mod error;
mod ops;
mod value_util;

use error::Code;
use serde_json::{Map, Value};
use std::io::{Read, Write};
use std::process::ExitCode;

fn emit(doc: &Value) -> bool {
    match serde_json::to_string(doc) {
        Ok(s) => {
            let stdout = std::io::stdout();
            let mut lock = stdout.lock();
            lock.write_all(s.as_bytes()).is_ok() && lock.write_all(b"\n").is_ok()
        }
        Err(_) => false,
    }
}

fn input_error(message: impl Into<String>) -> (Value, i32) {
    (
        engine::make_response(
            Map::new(),
            Vec::new(),
            Vec::new(),
            Some((Code::InvalidWorkflow, None, message.into())),
        ),
        2,
    )
}

fn real_main() -> i32 {
    let args: Vec<std::ffi::OsString> = std::env::args_os().skip(1).collect();
    if args.len() != 2 {
        let (doc, code) = input_error("usage: workflow run <workflow.json>");
        emit(&doc);
        return code;
    }
    let command = args[0].to_string_lossy();
    if command != "run" {
        let (doc, code) = input_error(format!("unknown command: {command}"));
        emit(&doc);
        return code;
    }

    let path = args[1].to_string_lossy();
    let text = if path == "-" {
        let mut buf = String::new();
        match std::io::stdin().read_to_string(&mut buf) {
            Ok(_) => buf,
            Err(e) => {
                let (doc, code) = input_error(format!("failed to read stdin: {e}"));
                emit(&doc);
                return code;
            }
        }
    } else {
        match std::fs::read_to_string(path.as_ref()) {
            Ok(s) => s,
            Err(e) => {
                let (doc, code) = input_error(format!("failed to read input file: {e}"));
                emit(&doc);
                return code;
            }
        }
    };

    let root: Value = match serde_json::from_str(&text) {
        Ok(v) => v,
        Err(e) => {
            let (doc, code) = input_error(format!("invalid JSON: {e}"));
            emit(&doc);
            return code;
        }
    };

    let (doc, code) = engine::run(&root);
    emit(&doc);
    code
}

fn main() -> ExitCode {
    let result = std::panic::catch_unwind(real_main);
    match result {
        Ok(code) => ExitCode::from(code as u8),
        Err(_) => {
            let doc = engine::make_response(
                Map::new(),
                Vec::new(),
                Vec::new(),
                Some((
                    Code::StepFailed,
                    None,
                    "internal program error".to_string(),
                )),
            );
            emit(&doc);
            ExitCode::from(3)
        }
    }
}
