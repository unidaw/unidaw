//! What the model is actually handed.
//!
//! The observation is embedded in a system prompt on every ask, so its size is
//! a correctness property and not a performance note: past the context window
//! the feature does not run at all, and the failure is a 400 rather than
//! anything a person could read. These numbers are the reason `observe` returns
//! a shape and not a note list.

use daw_agent::observe::{observe, observe_window, Window};
use daw_bridge::control::EngineHandle;

/// A live engine is not required to check the SHAPE of the output — only that
/// the summary form stays small and the windowed form stays bounded. With no
/// engine to attach to, both are empty and this asserts nothing useful, so it
/// is skipped rather than passing vacuously.
fn handle() -> Option<EngineHandle> {
    let shm = std::env::var("DAW_UI_SHM_NAME").unwrap_or_else(|_| "/daw_web_ui".into());
    EngineHandle::attach(&shm, false).ok()
}

#[test]
fn the_shape_is_small_and_the_window_is_bounded() {
    let Some(h) = handle() else {
        eprintln!("no engine on the segment — skipping");
        return;
    };
    let shape = observe(&h, 0);
    let shape_json = serde_json::to_string(&shape).unwrap();
    let shape_text = shape.to_text();

    // No notes at all in the default form: that is the whole point.
    assert!(shape.tracks.iter().all(|t| t.notes.is_empty()),
            "the default observation must carry no notes");

    // A kilobyte per track is generous — measured, the real figure is a few
    // hundred bytes for a whole song. This is a ceiling that would catch a
    // regression back to enumerating notes, not a tight bound.
    let budget = 1024 * (shape.tracks.len().max(1) + 2);
    assert!(shape_json.len() < budget,
            "shape JSON {} bytes for {} tracks, budget {}",
            shape_json.len(), shape.tracks.len(), budget);
    assert!(shape_text.len() < budget, "shape text {} bytes", shape_text.len());

    // A window returns notes, and only from inside itself.
    let win = observe_window(&h, Some(Window::beats(0.0, 8.0)));
    for t in &win.tracks {
        for n in &t.notes {
            assert!(n.beat >= 0.0 && n.beat < 8.0,
                    "note at beat {} escaped the 0..8 window", n.beat);
        }
    }

    // ...and a per-track window returns only that track's notes.
    if let Some(id) = win.tracks.first().map(|t| t.track_id) {
        let one = observe_window(&h, Some(Window::beats(0.0, 64.0).on_track(id)));
        for t in &one.tracks {
            if t.track_id != id {
                assert!(t.notes.is_empty(),
                        "track {} returned notes for a window scoped to track {}", t.track_id, id);
            }
        }
    }

    eprintln!("shape: {} bytes json, {} bytes text, {} tracks",
              shape_json.len(), shape_text.len(), shape.tracks.len());
}
