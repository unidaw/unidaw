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
    pub song: Song,
    pub tracks: Vec<TrackView>,
}

/// What the song IS, as opposed to where it is playing: tempo, meter, key.
///
/// Separate from `Transport` because these change when the music changes and
/// the transport changes several times a second. A reader that wants to know
/// what it is looking at should not have to filter that out of a playhead.
#[derive(Debug, Clone, Serialize)]
pub struct Song {
    /// Tempo at the playhead, in milli-BPM: 120000 is 120 BPM. Integer, so two
    /// readers can compare it exactly instead of chasing float jitter.
    pub tempo_milli_bpm: u32,
    /// 1 means the whole song is this tempo; more means it varies and this is
    /// only the tempo HERE.
    pub tempo_points: u32,
    pub time_sig_numerator: u32,
    pub time_sig_denominator: u32,
    /// The key in force at the playhead, if the harmony timeline has one.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub key: Option<Key>,
    /// How many key changes the song has, so "there is more harmony than this"
    /// is visible without listing it.
    pub key_changes: usize,
}

#[derive(Debug, Clone, Serialize)]
pub struct Key {
    /// Pitch class, 0 = C.
    pub root: u32,
    pub root_name: String,
    pub scale_id: u32,
    /// The engine's own name for the scale, when its registry has one.
    #[serde(skip_serializing_if = "String::is_empty")]
    pub scale_name: String,
    pub from_beat: f64,
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
    /// What the track is called. Blank for a slot the engine has not published
    /// a name for; a reader should fall back to "Track {id+1}", which is what
    /// the tracker shows.
    pub name: String,
    pub peak_rms: f32,
    /// Where the fader is. Asked for by name the first time this observation was
    /// tried on a real request — "turn the bass down 6 dB" needs to know what it
    /// is being turned down FROM, and the model said so itself.
    pub gain_db: f64,
    /// -1 hard left to 1 hard right.
    pub pan: f64,
    #[serde(skip_serializing_if = "is_false")]
    pub mute: bool,
    #[serde(skip_serializing_if = "is_false")]
    pub solo: bool,
    /// Per-lane tracker subdivision (4 = 16ths, 3 = triplets, 6 = sextuplets).
    pub lines_per_beat: u8,
    pub note_count: usize,
    /// THE SHAPE OF THE PART, so a reader does not need the notes to know what
    /// the track is. `pitch_low`/`pitch_high` answer "which one is the bass"
    /// directly, and better than twenty thousand note objects do.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub span: Option<Span>,
    /// The engine's published clip region stops at 4096 notes per track and says
    /// nothing about it. A reader told "the song right now" and handed a prefix
    /// will conclude the track ends early and edit the gap it thinks it found —
    /// so when notes are cut, the cut is stated.
    #[serde(skip_serializing_if = "is_false")]
    pub truncated: bool,
    /// Notes are omitted unless a window was asked for: enumerating every note of
    /// every track is how this observation reached 2.2 MB on a large song, which
    /// is more than a model can be given at all.
    #[serde(skip_serializing_if = "Vec::is_empty")]
    pub notes: Vec<NoteView>,
}

/// Where a track's notes are, and how high.
#[derive(Debug, Clone, Serialize)]
pub struct Span {
    pub first_beat: f64,
    pub last_beat: f64,
    pub pitch_low: u8,
    pub pitch_high: u8,
    pub pitch_low_name: String,
    pub pitch_high_name: String,
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

fn is_false(v: &bool) -> bool {
    !*v
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

/// How many beats of notes a caller gets when it asks for a window but does not
/// say how much. Four bars of 4/4 — enough to see a phrase.
pub const DEFAULT_WINDOW_BEATS: f64 = 16.0;

/// A window of the song, in beats. `None` means "no notes at all, just the
/// shape" — which is what a whole-song observation should be.
#[derive(Debug, Clone, Copy)]
pub struct Window {
    pub from_beat: f64,
    pub to_beat: f64,
    /// Only this track, or every track when None.
    pub track: Option<u32>,
}

impl Window {
    pub fn beats(from_beat: f64, len: f64) -> Self {
        Self { from_beat, to_beat: from_beat + len.max(0.0), track: None }
    }
    pub fn on_track(mut self, track: u32) -> Self {
        self.track = Some(track);
        self
    }
    fn wants(&self, track_id: u32) -> bool {
        self.track.map_or(true, |t| t == track_id)
    }
}

/// The whole song's SHAPE — every track named and measured, no notes.
///
/// This is what a reader should be given by default, and the reason is
/// arithmetic: enumerating every note costs ~114 bytes each, so a large session
/// reaches 2.2 MB — past what a model can be handed at all, and past it silently.
/// The same song summarised is under a kilobyte, and answers the questions that
/// are actually asked of it ("which track is the bass") better than the notes do.
pub fn observe(handle: &EngineHandle, _bars: u64) -> Observation {
    observe_window(handle, None)
}

/// The shape, plus the notes inside `window` when one is given.
pub fn observe_window(handle: &EngineHandle, window: Option<Window>) -> Observation {
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

    // ONCE, outside the loop. Each of these is its own seqlock retry, and calling
    // them per track would turn one observation into 3n reads. They are separate
    // frames from the snapshot, so they can be a publish newer — harmless for
    // names and key, which change when the music does, not every block.
    //
    // Never cached: there is no version counter for a track NAME, so a rename
    // moves the name and bumps nothing. Caching this on clip_version is exactly
    // how the sidecar once made renames invisible.
    let names = handle.read_track_names();
    let harmony = handle.read_harmony();
    let mixer = handle.read_mixer();

    let mut tracks = Vec::new();
    for track_id in 0..track_count {
        // The full note list is read either way — the shape is measured from it —
        // but it is only KEPT when a window asked for notes.
        let all = observe_track(handle, track_id);
        let peak_rms = snapshot
            .as_ref()
            .and_then(|s| s.ui_track_peak_rms.get(track_id as usize).copied())
            .unwrap_or(0.0);
        let lines_per_beat = snapshot
            .as_ref()
            .and_then(|s| s.ui_lines_per_beat.get(track_id as usize).copied())
            .filter(|&v| v > 0)
            .unwrap_or(4);
        let span = span_of(&all);
        let notes = match window {
            Some(w) if w.wants(track_id) => all
                .iter()
                .filter(|n| n.beat >= w.from_beat && n.beat < w.to_beat)
                .cloned()
                .collect(),
            _ => Vec::new(),
        };
        tracks.push(TrackView {
            track_id,
            // Slot-indexed, like the peaks and the lines-per-beat above. `.get`
            // rather than an index: read_track_names re-reads the track count in
            // its own frame, so its length need not match this loop's bound.
            name: names.get(track_id as usize).cloned().unwrap_or_default(),
            peak_rms,
            // Millibels and thousandths are the engine's units; dB and -1..1 are
            // what anyone reading this means by "how loud" and "how far left".
            gain_db: mixer.get(track_id as usize).map_or(0.0, |m| m.gain_millibels as f64 / 100.0),
            pan: mixer.get(track_id as usize).map_or(0.0, |m| m.pan_thousandths as f64 / 1000.0),
            // Mute and solo ride in `flags`, not as fields — MIXER_FLAG_* in layout.rs.
            mute: mixer.get(track_id as usize)
                .is_some_and(|m| m.flags as u16 & daw_bridge::layout::MIXER_FLAG_MUTE != 0),
            solo: mixer.get(track_id as usize)
                .is_some_and(|m| m.flags as u16 & daw_bridge::layout::MIXER_FLAG_SOLO != 0),
            lines_per_beat,
            note_count: all.len(),
            span,
            truncated: all.len() >= daw_bridge::layout::K_UI_MAX_CLIP_NOTES,
            notes,
        });
    }

    let (tempo_milli_bpm, tempo_points, num, den) = match &snapshot {
        Some(s) => (
            s.ui_tempo_milli_bpm,
            s.ui_tempo_point_count,
            s.ui_song_time_sig_num,
            s.ui_song_time_sig_den,
        ),
        None => (120_000, 1, 4, 4),
    };

    Observation {
        version,
        transport: Transport {
            playing,
            playhead_nanotick: playhead,
            playhead_beats: beats(playhead),
            clip_version,
            harmony_version,
        },
        song: Song {
            tempo_milli_bpm,
            tempo_points,
            time_sig_numerator: if num == 0 { 4 } else { num },
            time_sig_denominator: if den == 0 { 4 } else { den },
            key: key_at(handle, &harmony, playhead),
            key_changes: harmony.len(),
        },
        tracks,
    }
}

/// The fader, but only when it has been touched.
///
/// A line that says "0.0 dB, pan C" on every track of every song is noise: it
/// costs a reader attention to learn nothing. What is worth saying is what is
/// NOT at its default.
fn mix_text(t: &TrackView) -> String {
    let mut s = String::new();
    if t.gain_db.abs() >= 0.05 { s.push_str(&format!("  {:+.1} dB", t.gain_db)); }
    if t.pan.abs() >= 0.005 {
        s.push_str(&format!("  pan {}{:.0}%",
                            if t.pan < 0.0 { "L" } else { "R" }, t.pan.abs() * 100.0));
    }
    if t.mute { s.push_str("  MUTED"); }
    if t.solo { s.push_str("  SOLO"); }
    s
}

/// The extent of a part: where it starts and ends, and how high it sits.
fn span_of(notes: &[NoteView]) -> Option<Span> {
    let first = notes.first()?;
    let mut lo = first.pitch;
    let mut hi = first.pitch;
    let mut first_beat = first.beat;
    let mut last_beat = first.beat;
    for n in notes {
        lo = lo.min(n.pitch);
        hi = hi.max(n.pitch);
        first_beat = first_beat.min(n.beat);
        last_beat = last_beat.max(n.beat);
    }
    Some(Span {
        first_beat,
        last_beat,
        pitch_low: lo,
        pitch_high: hi,
        pitch_low_name: pitch_name(lo),
        pitch_high_name: pitch_name(hi),
    })
}

/// The key in force at `playhead`.
///
/// Chosen by MAX TICK at or before the playhead rather than by walking until the
/// first one past it: the engine inserts sorted, but a project load rebuilds the
/// timeline and nothing in the published region promises order. Picking the
/// latest qualifying event is right either way, and costs one pass.
fn key_at(handle: &EngineHandle, harmony: &[daw_bridge::layout::UiHarmonyEvent],
          playhead: u64) -> Option<Key> {
    let ev = harmony.iter().filter(|e| e.nanotick <= playhead).max_by_key(|e| e.nanotick)?;
    // The scale registry is static, so this is read only when there IS a key to
    // name — no point paying for it on a song with no harmony at all.
    let scale_name = handle
        .read_scales()
        .into_iter()
        .find(|s| s.id == ev.scale_id)
        .map(|s| s.name)
        .unwrap_or_default();
    Some(Key {
        root: ev.root % 12,
        root_name: NOTE_NAMES[(ev.root % 12) as usize].to_string(),
        scale_id: ev.scale_id,
        scale_name,
        from_beat: beats(ev.nanotick),
    })
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
        out.push_str(&format!(
            "song: {:.3} BPM{}  {}/{}",
            self.song.tempo_milli_bpm as f64 / 1000.0,
            if self.song.tempo_points > 1 { " (varies)" } else { "" },
            self.song.time_sig_numerator,
            self.song.time_sig_denominator,
        ));
        if let Some(k) = &self.song.key {
            out.push_str(&format!("  key {} {}", k.root_name,
                                  if k.scale_name.is_empty() { "?" } else { &k.scale_name }));
            if self.song.key_changes > 1 {
                out.push_str(&format!(" (+{} more changes)", self.song.key_changes - 1));
            }
        }
        out.push('\n');
        for t in &self.tracks {
            // The name a person sees. A slot the engine has not published a name
            // for reads blank, and the tracker shows "Track n+1" for it.
            let name = if t.name.is_empty() {
                format!("Track {}", t.track_id + 1)
            } else {
                t.name.clone()
            };
            match &t.span {
                None => out.push_str(&format!(
                    "track {} \"{}\": (empty)  [lpb {}]{}\n",
                    t.track_id, name, t.lines_per_beat, mix_text(t))),
                Some(sp) => out.push_str(&format!(
                    "track {} \"{}\": {} notes, beats {:.2}-{:.2}, {}..{}  [lpb {}]{}{}\n",
                    t.track_id, name, t.note_count, sp.first_beat, sp.last_beat,
                    sp.pitch_low_name, sp.pitch_high_name, t.lines_per_beat,
                    mix_text(t),
                    if t.truncated { "  TRUNCATED" } else { "" })),
            }
            // Notes only when a window asked for them.
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
