//! Load a project, then save it under a new name — to verify the engine
//! preserves the arrangement's clip/placement structure across load->save.
//! Run against a live engine started with DAW_PROJECT_DIR set:
//!   DAW_UI_SHM_NAME=/daw_engine_ui cargo run -p daw-agent --example roundtrip -- rt_in rt_out
use daw_agent::{AgentSession, ToolCall};
use serde_json::json;

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let src = args.get(1).cloned().unwrap_or_else(|| "rt_in".into());
    let dst = args.get(2).cloned().unwrap_or_else(|| "rt_out".into());
    let name = std::env::var("DAW_UI_SHM_NAME")
        .unwrap_or_else(|_| daw_bridge::control::default_shm_name());
    let session = AgentSession::attach(&name).expect("attach to engine");

    let load = session.execute(&ToolCall { tool: "load".into(), args: json!({ "name": src }) });
    println!("== load {src} ==\n{}", serde_json::to_string(&load).unwrap());
    std::thread::sleep(std::time::Duration::from_millis(600));

    // Optional live edit before save, to exercise the dirty->flatten path.
    if args.get(3).map(|s| s == "dirty").unwrap_or(false) {
        let add = session.execute(&ToolCall {
            tool: "add_notes".into(),
            args: json!({ "track": 0, "pitches": [72], "step": 240000, "duration": 240000, "velocity": 100 }),
        });
        println!("== edit (add_notes) ==\n{}", serde_json::to_string(&add).unwrap());
        std::thread::sleep(std::time::Duration::from_millis(400));
    }

    let save = session.execute(&ToolCall { tool: "save".into(), args: json!({ "name": dst }) });
    println!("== save {dst} ==\n{}", serde_json::to_string(&save).unwrap());
    std::thread::sleep(std::time::Duration::from_millis(400));
}
