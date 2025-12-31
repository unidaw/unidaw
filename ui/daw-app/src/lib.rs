pub fn run_app() -> anyhow::Result<()> {
    crate::app::run_app()
}

pub use crate::app::{
    EngineView, UiNotify, BEATS_PER_BAR, DEFAULT_ZOOM_INDEX, NANOTICKS_PER_QUARTER, TRACK_COUNT,
    ZOOM_LEVELS,
};
pub use crate::state::{ClipChord, ClipNote, HarmonyEntry, PendingChord, PendingNote};
pub use crate::engine::bridge::{
    decode_chord_diff, decode_harmony_diff, decode_ui_diff, log_last_ui_command, ring_pop,
    ring_view, EngineBridge, RingView,
};
pub use crate::engine::bridge::{reset_ui_counters, ui_cmd_counters};
pub use crate::engine::supervisor::{default_engine_path, spawn_engine_process, stop_engine_process};

mod app;
mod clipboard;
mod commands;
mod harmony;
mod palette;
mod plugins;
mod scale_browser;
mod selection;
mod state;
mod util;
mod engine;
mod tracker;
mod ui;
