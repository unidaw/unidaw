use daw_bridge::layout::{UiChordCommandPayload, UiCommandPayload};

#[derive(Clone, Debug)]
pub enum QueuedCommand {
    Ui(UiCommandPayload),
    Chord(UiChordCommandPayload),
}

#[derive(Clone, Debug)]
pub struct PendingNote {
    pub track_id: u32,
    pub nanotick: u64,
    pub duration: u64,
    pub pitch: u8,
    pub velocity: u8,
    pub column: u8,
}

#[derive(Clone, Debug)]
pub struct PendingChord {
    pub track_id: u32,
    pub nanotick: u64,
    pub duration: u64,
    pub spread: u32,
    pub humanize_timing: u16,
    pub humanize_velocity: u16,
    pub degree: u8,
    pub quality: u8,
    pub inversion: u8,
    pub base_octave: u8,
    pub column: u8,
}

#[derive(Clone, Debug)]
pub struct ClipNote {
    pub nanotick: u64,
    pub duration: u64,
    pub pitch: u8,
    pub velocity: u8,
    pub column: u8,
}

#[derive(Clone, Debug)]
pub struct HarmonyEntry {
    pub nanotick: u64,
    pub root: u32,
    pub scale_id: u32,
}

#[derive(Clone, Debug)]
pub struct ClipChord {
    pub chord_id: u32,
    pub nanotick: u64,
    pub duration: u64,
    pub spread: u32,
    pub humanize_timing: u16,
    pub humanize_velocity: u16,
    pub degree: u8,
    pub quality: u8,
    pub inversion: u8,
    pub base_octave: u8,
    pub column: u8,
}

#[derive(Clone, Debug)]
pub enum CellKind {
    Note,
    Chord,
}

#[derive(Clone, Debug)]
#[allow(dead_code)]
pub struct CellEntry {
    pub kind: CellKind,
    pub text: String,
    pub nanotick: u64,
    pub note_pitch: Option<u8>,
    pub chord_id: Option<u32>,
    pub column: usize,
    pub note_off: bool,
}

#[derive(Clone, Debug)]
pub struct AggregateCell {
    pub count: usize,
    pub notes_only: bool,
    pub note_off_only: bool,
    pub unique_pitch: Option<u8>,
    pub chord_only: bool,
    pub single: Option<AggregateSingle>,
}

impl AggregateCell {
    pub fn new() -> Self {
        Self {
            count: 0,
            notes_only: true,
            note_off_only: true,
            unique_pitch: None,
            chord_only: true,
            single: None,
        }
    }

    pub fn add_note(&mut self, pitch: u8, is_note_off: bool) {
        self.count += 1;
        if self.count == 1 {
            self.single = Some(AggregateSingle::Note { pitch, note_off: is_note_off });
        } else {
            self.single = None;
        }
        self.chord_only = false;
        if !is_note_off {
            self.note_off_only = false;
        }
        if self.unique_pitch.map_or(true, |prev| prev == pitch) {
            self.unique_pitch = Some(pitch);
        } else {
            self.unique_pitch = None;
        }
    }

    pub fn add_chord(&mut self, chord: ClipChord) {
        self.count += 1;
        if self.count == 1 {
            self.single = Some(AggregateSingle::Chord(chord));
        } else {
            self.single = None;
        }
        self.notes_only = false;
        self.note_off_only = false;
        self.unique_pitch = None;
    }
}

#[derive(Clone, Debug)]
pub enum AggregateSingle {
    Note { pitch: u8, note_off: bool },
    Chord(ClipChord),
}

#[derive(Clone, Debug)]
pub struct HarmonyAggregate {
    pub count: usize,
    pub labels: Vec<String>,
}
