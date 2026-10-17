mod decimal;
mod engine;
mod ops;
mod value_util;
mod workflow;

use std::io::{Read, Write};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::process::ExitCode;

use engine::Response;

fn print_json(value: &serde_json::Value) {
    let text = serde_json::to_string(value).unwrap_or_else(|_| "{}".to_string());
    let mut stdout = std::io::stdout().lock();
    let _ = writeln!(stdout, "{text}");
    let _ = stdout.flush();
}

fn cli_error(message: &str) -> (Response, i32) {
    (
        Response::failed_empty("INVALID_WORKFLOW", None, message.to_string()),
        2,
    )
}

fn read_input(args: &[String]) -> Result<String, (Response, i32)> {
    if args.len() != 3 || args[1] != "run" {
        return Err(cli_error("usage: workflow run <workflow.json|->"));
    }
    let source = &args[2];
    if source == "-" {
        let mut buf = String::new();
        std::io::stdin()
            .read_to_string(&mut buf)
            .map_err(|e| cli_error(&format!("cannot read stdin: {e}")))?;
        Ok(buf)
    } else {
        std::fs::read_to_string(source)
            .map_err(|e| cli_error(&format!("cannot read input file: {e}")))
    }
}

fn real_main() -> i32 {
    let args: Vec<String> = std::env::args().collect();
    let text = match read_input(&args) {
        Ok(t) => t,
        Err((resp, code)) => {
            print_json(&resp.to_json());
            return code;
        }
    };
    let (resp, code) = engine::run_text(&text);
    print_json(&resp.to_json());
    code
}

fn main() -> ExitCode {
    let code = catch_unwind(AssertUnwindSafe(real_main)).unwrap_or_else(|_| {
        let resp = Response::failed_empty("INTERNAL_ERROR", None, "internal error".to_string());
        print_json(&resp.to_json());
        3
    });
    ExitCode::from(code as u8)
}
