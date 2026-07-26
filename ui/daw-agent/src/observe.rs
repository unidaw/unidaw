//! Perception: turn the live engine's shared state into a structured, legible
//! observation an agent can reason over. Action already existed (the command
//! ring); this is the eyes. Pure read — no command is sent, and reads never
//! touch the write ring.
//!
//! Two sources are combined: the SHM header snapshot (transport, playhead, live
//! per-track peak) and the v9 all-tracks published clip region, read per track
//! with `read_track_clip` — a seqlock read, no request. Row ops
//! (retrigger/probability/delay) ride along in the note now.

use daw_bridge::control::EngineHandle;
use daw_bridge::grid::NANOTICKS_PER_QUARTER;
use serde::Serialize;

/// A whole-song observation at one instant.
#[derive(Debug, Clone, Serialize)]
pub struct Observation {
    /// The seqlock version the snapshot was read at (monotonic; dedup on it).
    pub version: u64,
    pub transport: Transport,
    pub tracks: Vec<TrackView>,
}

#[derive(Debug, Clone, Serialize)]
pub struct Transport {
    pub playing: bool,
    pub playhead_nanotick: u64,
    pub playhead_beats: f64,
    pub clip_version: u32,
    pub harmony_version: u32,
}

#[derive(Debug, Clone, Serialize)]
pub struct TrackView {
    pub track_id: u32,
    pub peak_rms: f32,
    pub note_count: usize,
    pub notes: Vec<NoteView>,
}

#[derive(Debug, Clone, Serialize)]
pub struct NoteView {
    pub nanotick: u64,
    pub beat: f64,
    pub duration: u64,
    pub pitch: u8,
    pub name: String,
    pub velocity: u8,
    pub column: u8,
    /// Row ops, omitted from JSON when inert so an op-free note reads clean.
    #[serde(skip_serializing_if = "is_zero_u8")]
    pub retrigger: u8,
    #[serde(skip_serializing_if = "is_zero_u8")]
    pub probability: u8,
    #[serde(skip_serializing_if = "is_zero_u32")]
    pub delay_nanoticks: u32,
}

fn is_zero_u8(v: &u8) -> bool {
    *v == 0
}
fn is_zero_u32(v: &u32) -> bool {
    *v == 0
}

const NOTE_NAMES: [&str; 12] = [
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B",
];

/// Scientific pitch name for a MIDI note, e.g. 60 -> "C4".
pub fn pitch_name(pitch: u8) -> String {
    let octave = (pitch as i32 / 12) - 1;
    format!("{}{}", NOTE_NAMES[(pitch % 12) as usize], octave)
}

fn beats(nanotick: u64) -> f64 {
    nanotick as f64 / NANOTICKS_PER_QUARTER as f64
}

/// Reads one track's notes from the v9 all-tracks published region. A seqlock
/// read, no request, no write ring — so any number of observers can call it.
/// Returns an empty vec if the region is absent (older engine) or the track has
/// no notes.
pub fn observe_track(handle: &EngineHandle, track_id: u32) -> Vec<NoteView> {
    let Some(snap) = handle.read_track_clip(track_id) else {
        return Vec::new();
    };
    let count = (snap.note_count as usize).min(snap.notes.len());
    let mut notes = Vec::with_capacity(count);
    for note in snap.notes.iter().take(count) {
        notes.push(NoteView {
            nanotick: note.t_on,
            beat: beats(note.t_on),
            duration: note.t_off.saturating_sub(note.t_on),
            pitch: note.pitch,
            name: pitch_name(note.pitch),
            velocity: note.velocity,
            column: note.column,
            retrigger: note.retrigger,
            probability: note.probability,
            delay_nanoticks: note.delay_nanoticks,
        });
    }
    notes
}

/// Observe the whole song: transport plus every track's notes.
pub fn observe(handle: &EngineHandle, _bars: u64) -> Observation {
    let snapshot = handle.snapshot();
    let (version, playing, playhead, clip_version, harmony_version, track_count) =
        match &snapshot {
            Some(s) => (
                s.version,
                s.ui_transport_state != 0,
                s.ui_global_nanotick_playhead,
                s.ui_clip_version,
                s.ui_harmony_version,
                s.ui_track_count,
            ),
            None => (0, false, 0, 0, 0, handle.track_count()),
        };

    let mut tracks = Vec::new();
    for track_id in 0..track_count {
        let notes = observe_track(handle, track_id);
        let peak_rms = snapshot
            .as_ref()
            .and_then(|s| s.ui_track_peak_rms.get(track_id as usize).copied())
            .unwrap_or(0.0);
        tracks.push(TrackView {
            track_id,
            peak_rms,
            note_count: notes.len(),
            notes,
        });
    }

    Observation {
        version,
        transport: Transport {
            playing,
            playhead_nanotick: playhead,
            playhead_beats: beats(playhead),
            clip_version,
            harmony_version,
        },
        tracks,
    }
}

impl Observation {
    pub fn to_json(&self) -> String {
        serde_json::to_string_pretty(self).unwrap_or_else(|e| format!("{{\"error\":\"{e}\"}}"))
    }

    /// A compact human/agent-readable summary: transport, then each non-empty
    /// track as `beat pitch` lines. Empty tracks collapse to one line so a large
    /// empty session does not bury the signal.
    pub fn to_text(&self) -> String {
        let mut out = String::new();
        out.push_str(&format!(
            "transport: {} @ beat {:.3} (clipv {}, harmv {})\n",
            if self.transport.playing { "playing" } else { "stopped" },
            self.transport.playhead_beats,
            self.transport.clip_version,
            self.transport.harmony_version,
        ));
        for t in &self.tracks {
            if t.notes.is_empty() {
                out.push_str(&format!("track {}: (empty)\n", t.track_id));
                continue;
            }
            out.push_str(&format!("track {}: {} notes\n", t.track_id, t.note_count));
            for n in &t.notes {
                out.push_str(&format!(
                    "  beat {:>7.3}  {:<3} vel {:>3} dur {} col {}\n",
                    n.beat, n.name, n.velocity, n.duration, n.column
                ));
            }
        }
        out
    }
}
