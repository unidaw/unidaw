//! Demonstrate the agent loop with a scripted decider — the same shape a real LLM
//! harness fills in, minus the model call. Run against a live engine:
//!   DAW_UI_SHM_NAME=/daw_engine_ui cargo run -p daw-agent --example agent_loop
//!
//! A real LLM harness implements `Decider::decide` by handing the observation and
//! manifest to a model and parsing its tool calls back into `ToolCall`s; the loop,
//! transcript, and engine plumbing are exactly this. Nothing model-specific lives
//! in daw-agent — the networked client is a separate crate.
use daw_agent::{run_agent_loop, AgentSession, ScriptedDecider, ToolCall};
use serde_json::json;

fn main() {
    let name = std::env::var("DAW_UI_SHM_NAME")
        .unwrap_or_else(|_| daw_bridge::control::default_shm_name());
    let session = AgentSession::attach(&name).expect("attach to engine");
    let q: u64 = 960_000;

    // A tiny "plan": lay down an arpeggio, start playback, then stop.
    let mut decider = ScriptedDecider::new(vec![
        vec![ToolCall {
            tool: "add_notes".into(),
            args: json!({ "track": 0, "pitches": [60, 64, 67, 72], "start": 0, "step": q, "duration": q }),
        }],
        vec![ToolCall { tool: "transport".into(), args: json!({ "action": "play" }) }],
        vec![ToolCall { tool: "transport".into(), args: json!({ "action": "stop" }) }],
    ]);

    let transcript = run_agent_loop(&session, &mut decider, 16);
    for (i, step) in transcript.iter().enumerate() {
        for c in &step.calls {
            println!("step {i}: {} -> {}", c.call.tool, serde_json::to_string(&c.result).unwrap());
        }
    }
    println!("ran {} steps", transcript.len());
}
