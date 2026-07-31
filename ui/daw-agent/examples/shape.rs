//! Print the observation a model is given, for eyeballing.
//!
//!   DAW_UI_SHM_NAME=/daw_web_ui cargo run -q --release -p daw-agent --example shape
fn main() {
    let shm = std::env::var("DAW_UI_SHM_NAME").unwrap_or_else(|_| "/daw_web_ui".into());
    let Ok(h) = daw_bridge::control::EngineHandle::attach(&shm, false) else {
        eprintln!("no engine on {shm}");
        return;
    };
    print!("{}", daw_agent::observe::observe(&h, 0).to_text());
}
