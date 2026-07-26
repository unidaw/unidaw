//! Reproduce the command-volume host death: send many WriteNote commands, one at
//! a time, and report when the engine stops publishing. Mirrors the frontend's
//! repro-hang.mjs. Run against a live engine:
//!   DAW_UI_SHM_NAME=/daw_cmd_ui cargo run -p daw-agent --example spam -- 80 120
use daw_agent::AgentSession;
use daw_bridge::layout::{UiCommandPayload, UiCommandType};

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let count: usize = args.get(1).and_then(|s| s.parse().ok()).unwrap_or(80);
    let delay_ms: u64 = args.get(2).and_then(|s| s.parse().ok()).unwrap_or(120);
    let name = std::env::var("DAW_UI_SHM_NAME")
        .unwrap_or_else(|_| daw_bridge::control::default_shm_name());
    let session = AgentSession::attach(&name).expect("attach");
    let h = session.handle();
    let q: u64 = 960_000;

    let mut last_version = h.clip_version();
    for i in 0..count {
        // One WriteNote, base_version = current clip version (optimistic).
        let base = h.clip_version();
        let nanotick = (i as u64) * (q / 4);
        let payload = UiCommandPayload {
            command_type: UiCommandType::WriteNote as u16,
            flags: 0,
            track_id: 0,
            plugin_index: 0,
            note_pitch: 60 + (i % 12) as u32,
            value0: 100,
            note_nanotick_lo: (nanotick & 0xffff_ffff) as u32,
            note_nanotick_hi: (nanotick >> 32) as u32,
            note_duration_lo: (q / 4) as u32,
            note_duration_hi: 0,
            base_version: base,
        };
        if let Err(e) = h.send_command(payload) {
            println!("send failed at note {i}: {e}");
            return;
        }
        std::thread::sleep(std::time::Duration::from_millis(delay_ms));
        let v = h.clip_version();
        // If the version hasn't moved in a while, the engine has stopped draining.
        if i > 3 && v == last_version {
            // give it one more grace period
            std::thread::sleep(std::time::Duration::from_millis(500));
            let v2 = h.clip_version();
            if v2 == last_version {
                println!("ENGINE STOPPED PUBLISHING after {i} commands (clipVersion stuck at {v2})");
                return;
            }
        }
        last_version = v;
    }
    println!("sent all {count} commands; final clipVersion {}", h.clip_version());
}
