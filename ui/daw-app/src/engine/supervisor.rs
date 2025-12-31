use std::path::PathBuf;
use std::process::{Child, Command, Stdio};
use std::time::Instant;

use anyhow::{Context as AnyhowContext, Result};

pub struct EngineSupervisor {
    pub child: Option<Child>,
    pub last_spawn_attempt: Instant,
    pub engine_path: Option<PathBuf>,
    pub engine_missing_logged: bool,
}

pub fn default_engine_path() -> Option<PathBuf> {
    if let Ok(path) = std::env::var("DAW_ENGINE_PATH") {
        let candidate = PathBuf::from(path);
        if candidate.exists() {
            return Some(candidate);
        }
    }
    let mut roots = Vec::new();
    if let Ok(cwd) = std::env::current_dir() {
        roots.push(cwd);
    }
    if let Ok(exe) = std::env::current_exe() {
        if let Some(dir) = exe.parent() {
            roots.push(dir.to_path_buf());
        }
    }
    for root in &roots {
        eprintln!("daw-app: Searching for engine from root: {}", root.display());
        for ancestor in root.ancestors() {
            let candidate = ancestor.join("build").join("daw_engine");
            if candidate.exists() {
                eprintln!("daw-app: Found engine at: {}", candidate.display());
                return Some(candidate);
            }
        }
    }
    eprintln!("daw-app: Engine not found in any search paths");
    None
}

pub fn spawn_engine_process(engine_path: &PathBuf) -> Result<Child> {
    let mut command = Command::new(engine_path);
    eprintln!("daw-app: Spawning engine from: {}", engine_path.display());

    // Run the engine from its own directory (build/) so it can find juce_host_process
    if let Some(engine_dir) = engine_path.parent() {
        eprintln!("daw-app: Setting engine working dir to: {}", engine_dir.display());
        command.current_dir(engine_dir);
    }

    eprintln!("daw-app: Current UI working dir: {:?}", std::env::current_dir());
    command
        .stdout(Stdio::inherit())
        .stderr(Stdio::inherit())
        .spawn()
        .with_context(|| format!("failed to spawn engine at {}", engine_path.display()))
}

pub fn stop_engine_process(supervisor: &mut EngineSupervisor) {
    if let Some(mut child) = supervisor.child.take() {
        let _ = child.kill();
        let _ = child.wait();
    }
}
