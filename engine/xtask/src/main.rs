use std::{
    env,
    path::Path,
    process::{Command, ExitCode},
};

fn main() -> ExitCode {
    let engine = Path::new(env!("CARGO_MANIFEST_DIR")).parent().unwrap();
    let status = Command::new(env::var_os("PYTHON").unwrap_or_else(|| "python".into()))
        .arg(engine.join("tools/task.py"))
        .args(env::args_os().skip(1))
        .status();
    match status {
        Ok(code) if code.success() => ExitCode::SUCCESS,
        Ok(_) => ExitCode::FAILURE,
        Err(error) => {
            eprintln!("Cannot start build tool: {error}");
            ExitCode::FAILURE
        }
    }
}
