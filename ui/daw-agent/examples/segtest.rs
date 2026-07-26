//! Enter two note clusters far apart on a fresh track and save — to verify the
//! engine segments live-entered notes into separate clips ("no notes outside
//! clips"). Run against a live engine started with DAW_PROJECT_DIR set:
//!   DAW_UI_SHM_NAME=/daw_seg_ui cargo run -p daw-agent --example segtest -- segout
use daw_agent::{AgentSession, ToolCall};
use serde_json::json;

fn main() {
    let dst = std::env::args().nth(1).unwrap_or_else(|| "segout".into());
    let name = std::env::var("DAW_UI_SHM_NAME")
        .unwrap_or_else(|_| daw_bridge::control::default_shm_name());
    let session = AgentSession::attach(&name).expect("attach to engine");
    let q: u64 = 960_000;
    let four_bars = 16 * q;

    let a = session.execute(&ToolCall {
        tool: "add_notes".into(),
        args: json!({ "track": 0, "pitches": [60, 62, 64], "start": 0, "step": q, "duration": q }),
    });
    println!("cluster A: {}", serde_json::to_string(&a).unwrap());
    // No delay needed: add_notes waits for the engine to apply the batch before
    // returning, so B reads a settled clip version.
    let b = session.execute(&ToolCall {
        tool: "add_notes".into(),
        args: json!({ "track": 0, "pitches": [67, 69], "start": four_bars, "step": q, "duration": q }),
    });
    println!("cluster B: {}", serde_json::to_string(&b).unwrap());

    // Rails as the UI would see them (already refreshed — add_notes waited for
    // the publish that carries the new clip version).
    let extents = session.handle().read_clip_extents();
    println!("== clip extents (rails): {} ==", extents.len());
    for e in &extents {
        println!("  track {} placement {} [{}..{}]", e.track_id, e.placement_id, e.start_tick, e.end_tick);
    }

    let s = session.execute(&ToolCall { tool: "save".into(), args: json!({ "name": dst }) });
    println!("save: {}", serde_json::to_string(&s).unwrap());
    std::thread::sleep(std::time::Duration::from_millis(400));
}
