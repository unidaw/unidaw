//! End-to-end smoke check for the in-app agent against a running engine.
//! Attaches on the agent's own ring, writes a phrase, then observes it back
//! through the published all-tracks region. Run with the engine live:
//!   DAW_UI_SHM_NAME=/daw_engine_ui cargo run -p daw-agent --example smoke
use daw_agent::{AgentSession, ToolCall};
use serde_json::json;

fn main() {
    let name = std::env::var("DAW_UI_SHM_NAME")
        .unwrap_or_else(|_| daw_bridge::control::default_shm_name());
    let session = AgentSession::attach(&name).expect("attach to engine");

    println!("== manifest ==");
    println!("{}", daw_agent::manifest_json());

    let add = session.execute(&ToolCall {
        tool: "add_notes".into(),
        args: json!({
            "track": 0, "pitches": [60, 64, 67, 72],
            "step": 240000, "duration": 240000, "velocity": 110
        }),
    });
    println!("== add_notes ==\n{}", serde_json::to_string(&add).unwrap());

    // Give the engine a beat to drain the agent ring and republish.
    std::thread::sleep(std::time::Duration::from_millis(400));

    let obs = session.observe();
    println!("== observe (text) ==\n{}", obs.to_text());
    let notes: usize = obs.tracks.iter().map(|t| t.note_count).sum();
    println!("total notes observed via published region: {notes}");
    std::process::exit(if notes >= 4 { 0 } else { 1 });
}
