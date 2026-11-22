use kalam_runtime::{content, session};
use std::{env, fs::File, io::Read, path::Path, process::ExitCode};

fn read_file(path: &Path, limit: usize) -> Result<Vec<u8>, String> {
    let read = || -> std::io::Result<Vec<u8>> {
        let mut bytes = Vec::new();
        File::open(path)?
            .take(limit as u64 + 1)
            .read_to_end(&mut bytes)?;
        Ok(bytes)
    };
    let bytes = read().map_err(|error| format!("{}: {error}", path.display()))?;
    if bytes.len() > limit {
        return Err(format!("{}: exceeds {} KiB", path.display(), limit / 1024));
    }
    Ok(bytes)
}

fn execute() -> Result<String, String> {
    let args: Vec<_> = env::args_os().skip(1).collect();
    let [assets, name, script] = args.as_slice() else {
        return Err("usage: kalam-sim ASSETS SCENE SCRIPT".into());
    };
    let name = name.to_str().ok_or("scene name must be UTF-8")?;
    let script: session::Script =
        kalam_runtime::inline_fields::decode(&read_file(Path::new(script), 1024 * 1024)?)
            .map_err(|error| format!("script: {error}"))?;
    let content = content::load_scene(name, |path, limit| {
        read_file(&Path::new(assets).join(path), limit)
    })?;
    let report = session::run(content.scene, script)?;
    serde_json::to_string_pretty(&report).map_err(|error| error.to_string())
}

fn main() -> ExitCode {
    match execute() {
        Ok(json) => {
            println!("{json}");
            ExitCode::SUCCESS
        }
        Err(error) => {
            eprintln!("Kalam sim: {error}");
            ExitCode::FAILURE
        }
    }
}
