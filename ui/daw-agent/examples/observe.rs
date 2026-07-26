//! Print a live observation of whatever the engine currently holds — the same
//! all-tracks published region the web UI reads. Run with the engine live:
//!   DAW_UI_SHM_NAME=/daw_engine_ui cargo run -p daw-agent --example observe
use daw_agent::AgentSession;

fn main() {
    let name = std::env::var("DAW_UI_SHM_NAME")
        .unwrap_or_else(|_| daw_bridge::control::default_shm_name());
    let session = AgentSession::attach(&name).expect("attach to engine");
    let obs = session.observe();
    println!("{}", obs.to_text());
    let notes: usize = obs.tracks.iter().map(|t| t.note_count).sum();
    println!("total notes across {} tracks: {notes}", obs.tracks.len());

    // M3.4: clip extents (rails).
    let extents = session.handle().read_clip_extents();
    println!("== clip extents (rails): {} ==", extents.len());
    for e in &extents {
        let end = e.name.iter().position(|&b| b == 0).unwrap_or(e.name.len());
        let name = String::from_utf8_lossy(&e.name[..end]);
        println!(
            "  track {} placement {} clip {}  [{}..{}]  \"{}\"",
            e.track_id, e.placement_id, e.clip_id, e.start_tick, e.end_tick, name
        );
    }
}
