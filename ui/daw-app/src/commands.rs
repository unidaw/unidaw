use daw_bridge::layout::{
    UiChordCommandPayload, UiChordDiffPayload, UiChordDiffType, UiClipSnapshot, UiCommandPayload,
    UiCommandType, UiDiffPayload, UiDiffType, UiHarmonyDiffPayload, UiHarmonyDiffType,
    UiHarmonySnapshot,
};

use crate::app::{EngineView, TRACK_COUNT};
use crate::engine::bridge::{
    bump_ui_enqueued, bump_ui_send_fail, bump_ui_sent, log_ui_send_fail,
};
use crate::state::{ClipChord, ClipNote, HarmonyEntry, QueuedCommand};
use crate::util::{unpack_chord_packed, unpack_chord_spread};

impl EngineView {
    fn note_key(payload: &UiCommandPayload) -> Option<(u32, u64, u8)> {
        let command = payload.command_type;
        if command == UiCommandType::WriteNote as u16 ||
            command == UiCommandType::DeleteNote as u16 {
            let nanotick = (payload.note_nanotick_lo as u64) |
                ((payload.note_nanotick_hi as u64) << 32);
            return Some((payload.track_id, nanotick, payload.flags as u8));
        }
        None
    }

    fn chord_key(payload: &UiChordCommandPayload) -> Option<(u32, u64, u8)> {
        let command = payload.command_type;
        if command == UiCommandType::WriteChord as u16 ||
            command == UiCommandType::DeleteChord as u16 {
            let nanotick = (payload.nanotick_lo as u64) |
                ((payload.nanotick_hi as u64) << 32);
            return Some((payload.track_id, nanotick, payload.flags as u8));
        }
        None
    }

    pub(crate) fn enqueue_ui_command(&mut self, payload: UiCommandPayload) {
        if let Some(last) = self.queued_commands.back() {
            if let QueuedCommand::Ui(prev) = last {
                if let (Some(prev_key), Some(next_key)) =
                    (Self::note_key(prev), Self::note_key(&payload)) {
                    if prev_key == next_key {
                        self.queued_commands.pop_back();
                    }
                }
            }
        }
        self.queued_commands.push_back(QueuedCommand::Ui(payload));
        bump_ui_enqueued();
    }

    pub(crate) fn enqueue_chord_command(&mut self, payload: UiChordCommandPayload) {
        if let Some(last) = self.queued_commands.back() {
            if let QueuedCommand::Chord(prev) = last {
                if let (Some(prev_key), Some(next_key)) =
                    (Self::chord_key(prev), Self::chord_key(&payload)) {
                    if prev_key == next_key {
                        self.queued_commands.pop_back();
                    }
                }
            }
        }
        self.queued_commands.push_back(QueuedCommand::Chord(payload));
        bump_ui_enqueued();
    }

    pub fn flush_queued_commands(&mut self) {
        let Some(bridge) = &self.bridge else {
            return;
        };
        while let Some(entry) = self.queued_commands.front() {
            if self.clip_resync_pending || self.harmony_resync_pending {
                let should_pause = match entry {
                    QueuedCommand::Ui(payload) => {
                        let cmd = payload.command_type;
                        if self.clip_resync_pending {
                            matches!(
                                cmd,
                                x if x == UiCommandType::WriteNote as u16 ||
                                    x == UiCommandType::DeleteNote as u16 ||
                                    x == UiCommandType::Undo as u16 ||
                                    x == UiCommandType::Redo as u16
                            )
                        } else if self.harmony_resync_pending {
                            matches!(
                                cmd,
                                x if x == UiCommandType::WriteHarmony as u16 ||
                                    x == UiCommandType::DeleteHarmony as u16
                            )
                        } else {
                            false
                        }
                    }
                    QueuedCommand::Chord(_) => self.clip_resync_pending,
                };
                if should_pause {
                    break;
                }
            }
            let sent = match entry {
                QueuedCommand::Ui(payload) => bridge.try_send_ui_command(*payload),
                QueuedCommand::Chord(payload) => bridge.try_send_ui_chord_command(*payload),
            };
            if sent {
                bump_ui_sent();
                self.queued_commands.pop_front();
            } else {
                bump_ui_send_fail();
                log_ui_send_fail();
                break;
            }
        }
    }

    pub(crate) fn rebase_clip_queue(&mut self, base_version: u32) {
        let mut next = base_version;
        for entry in self.queued_commands.iter_mut() {
            match entry {
                QueuedCommand::Ui(payload) => {
                    let cmd = payload.command_type;
                    if cmd == UiCommandType::WriteNote as u16 ||
                        cmd == UiCommandType::DeleteNote as u16 ||
                        cmd == UiCommandType::Undo as u16 ||
                        cmd == UiCommandType::Redo as u16 {
                        payload.base_version = next;
                        next = next.saturating_add(1);
                    }
                }
                QueuedCommand::Chord(payload) => {
                    let cmd = payload.command_type;
                    if cmd == UiCommandType::WriteChord as u16 ||
                        cmd == UiCommandType::DeleteChord as u16 {
                        payload.base_version = next;
                        next = next.saturating_add(1);
                    }
                }
            }
        }
        self.clip_version_local = next;
    }

    pub(crate) fn rebase_harmony_queue(&mut self, base_version: u32) {
        let mut next = base_version;
        for entry in self.queued_commands.iter_mut() {
            if let QueuedCommand::Ui(payload) = entry {
                let cmd = payload.command_type;
                if cmd == UiCommandType::WriteHarmony as u16 ||
                    cmd == UiCommandType::DeleteHarmony as u16 {
                    payload.base_version = next;
                    next = next.saturating_add(1);
                }
            }
        }
        self.harmony_version_local = next;
    }

    pub fn apply_clip_snapshot(&mut self, snapshot: UiClipSnapshot) {
        self.clip_notes = vec![Vec::new(); TRACK_COUNT];
        self.clip_chords = vec![Vec::new(); TRACK_COUNT];
        self.pending_notes.clear();
        self.pending_chords.clear();
        let track_count = snapshot.track_count.min(TRACK_COUNT as u32) as usize;
        for track_index in 0..track_count {
            let track = snapshot.tracks[track_index];
            let note_start = track.note_offset as usize;
            let note_end = note_start + track.note_count as usize;
            let notes = &mut self.clip_notes[track_index];
            notes.reserve(note_end.saturating_sub(note_start));
            for note_index in note_start..note_end {
                let note = snapshot.notes[note_index];
                notes.push(ClipNote {
                    nanotick: note.t_on,
                    duration: note.t_off.saturating_sub(note.t_on),
                    pitch: note.pitch,
                    velocity: note.velocity,
                    column: note.column,
                });
            }
            let chord_start = track.chord_offset as usize;
            let chord_end = chord_start + track.chord_count as usize;
            let chords = &mut self.clip_chords[track_index];
            chords.clear();
            chords.reserve(chord_end.saturating_sub(chord_start));
            for chord_index in chord_start..chord_end {
                let chord = snapshot.chords[chord_index];
                chords.push(ClipChord {
                    chord_id: chord.chord_id,
                    nanotick: chord.nanotick,
                    duration: chord.duration_nanoticks,
                    spread: chord.spread_nanoticks,
                    humanize_timing: chord.humanize_timing,
                    humanize_velocity: chord.humanize_velocity,
                    degree: chord.degree,
                    quality: chord.quality,
                    inversion: chord.inversion,
                    base_octave: chord.base_octave,
                    column: (chord.flags & 0xff) as u8,
                });
            }
        }
        self.clip_version_local = self.snapshot.ui_clip_version;
        self.clip_snapshot = Some(snapshot);
    }

    pub fn apply_harmony_snapshot(&mut self, snapshot: UiHarmonySnapshot) {
        self.harmony_events.clear();
        let count = snapshot.event_count as usize;
        let max = snapshot.events.len();
        let count = count.min(max);
        for idx in 0..count {
            let event = snapshot.events[idx];
            self.harmony_events.push(HarmonyEntry {
                nanotick: event.nanotick,
                root: event.root,
                scale_id: event.scale_id,
            });
        }
        self.harmony_version_local = self.snapshot.ui_harmony_version;
    }

    pub fn apply_diff(&mut self, diff: UiDiffPayload) {
        let track_index = diff.track_id as usize;
        if track_index >= self.clip_notes.len() {
            return;
        }
        let nanotick =
            (diff.note_nanotick_lo as u64) | ((diff.note_nanotick_hi as u64) << 32);
        let duration =
            (diff.note_duration_lo as u64) | ((diff.note_duration_hi as u64) << 32);
        let pitch = diff.note_pitch.min(127) as u8;
        let velocity = diff.note_velocity.min(127) as u8;
        let column = diff.note_column.min(255) as u8;

        match diff.diff_type {
            x if x == UiDiffType::AddNote as u16 => {
                let notes = &mut self.clip_notes[track_index];
                let chords = &mut self.clip_chords[track_index];

                notes.retain(|note| {
                    !(note.nanotick == nanotick && note.column == column)
                });
                chords.retain(|chord| chord.nanotick != nanotick);

                let insert_at = notes
                    .iter()
                    .position(|note| note.nanotick > nanotick)
                    .unwrap_or(notes.len());
                notes.insert(
                    insert_at,
                    ClipNote {
                        nanotick,
                        duration,
                        pitch,
                        velocity,
                        column,
                    },
                );

                self.pending_notes.retain(|note| {
                    !(note.track_id == track_index as u32 &&
                        note.nanotick == nanotick &&
                        note.column == column)
                });
                self.pending_chords.retain(|chord| {
                    !(chord.track_id == track_index as u32 && chord.nanotick == nanotick)
                });
            }
            x if x == UiDiffType::RemoveNote as u16 => {
                let notes = &mut self.clip_notes[track_index];
                if let Some(index) = notes.iter().position(|note| {
                    note.nanotick == nanotick && note.column == column
                }) {
                    notes.remove(index);
                }
            }
            _ => {}
        }
        if self.clip_version_local < diff.clip_version {
            self.clip_version_local = diff.clip_version;
        }

        self.pending_notes.retain(|note| {
            !(note.track_id == diff.track_id &&
                note.nanotick == nanotick &&
                note.column == column)
        });
        self.pending_chords.retain(|chord| {
            !(chord.track_id == diff.track_id && chord.nanotick == nanotick)
        });
    }

    pub fn apply_harmony_diff(&mut self, diff: UiHarmonyDiffPayload) {
        let nanotick =
            (diff.nanotick_lo as u64) | ((diff.nanotick_hi as u64) << 32);
        match diff.diff_type {
            x if x == UiHarmonyDiffType::AddEvent as u16 ||
                x == UiHarmonyDiffType::UpdateEvent as u16 => {
                if let Some(event) = self
                    .harmony_events
                    .iter_mut()
                    .find(|event| event.nanotick == nanotick)
                {
                    event.root = diff.root;
                    event.scale_id = diff.scale_id;
                } else {
                    self.harmony_events.push(HarmonyEntry {
                        nanotick,
                        root: diff.root,
                        scale_id: diff.scale_id,
                    });
                    self.harmony_events
                        .sort_by_key(|event| event.nanotick);
                }
            }
            x if x == UiHarmonyDiffType::RemoveEvent as u16 => {
                if let Some(index) = self
                    .harmony_events
                    .iter()
                    .position(|event| event.nanotick == nanotick)
                {
                    self.harmony_events.remove(index);
                }
            }
            _ => {}
        }
        if self.harmony_version_local < diff.harmony_version {
            self.harmony_version_local = diff.harmony_version;
        }
    }

    pub fn apply_chord_diff(&mut self, diff: UiChordDiffPayload) {
        let track_index = diff.track_id as usize;
        if track_index >= self.clip_chords.len() {
            return;
        }
        let nanotick = (diff.nanotick_lo as u64) | ((diff.nanotick_hi as u64) << 32);
        let duration = (diff.duration_lo as u64) | ((diff.duration_hi as u64) << 32);
        let (degree, quality, inversion, base_octave) = unpack_chord_packed(diff.packed);
        let (spread, column) = unpack_chord_spread(diff.spread_nanoticks);
        let humanize_timing = (diff.flags & 0xff) as u16;
        let humanize_velocity = ((diff.flags >> 8) & 0xff) as u16;

        match diff.diff_type {
            x if x == UiChordDiffType::AddChord as u16 ||
                x == UiChordDiffType::UpdateChord as u16 => {
                let notes = &mut self.clip_notes[track_index];
                let chords = &mut self.clip_chords[track_index];

                notes.retain(|note| !(note.nanotick == nanotick && note.column == column));
                chords.retain(|chord| {
                    chord.chord_id != diff.chord_id &&
                        !(chord.nanotick == nanotick && chord.column == column)
                });

                let insert_at = chords
                    .iter()
                    .position(|chord| chord.nanotick > nanotick)
                    .unwrap_or(chords.len());
                chords.insert(
                    insert_at,
                    ClipChord {
                        chord_id: diff.chord_id,
                        nanotick,
                        duration,
                        spread,
                        humanize_timing,
                        humanize_velocity,
                        degree,
                        quality,
                        inversion,
                        base_octave,
                        column,
                    },
                );

                self.pending_notes.retain(|note| {
                    !(note.track_id == diff.track_id &&
                        note.nanotick == nanotick &&
                        note.column == column)
                });
                self.pending_chords.retain(|chord| {
                    !(chord.track_id == diff.track_id &&
                        chord.nanotick == nanotick &&
                        chord.column == column)
                });
            }
            x if x == UiChordDiffType::RemoveChord as u16 => {
                let chords = &mut self.clip_chords[track_index];
                if let Some(index) = chords
                    .iter()
                    .position(|chord| chord.chord_id == diff.chord_id)
                {
                    chords.remove(index);
                } else if let Some(index) = chords
                    .iter()
                    .position(|chord| chord.nanotick == nanotick && chord.column == column)
                {
                    chords.remove(index);
                }
                self.pending_chords.retain(|chord| {
                    !(chord.track_id == diff.track_id &&
                        chord.nanotick == nanotick &&
                        chord.column == column)
                });
            }
            _ => {}
        }
        if self.clip_version_local < diff.clip_version {
            self.clip_version_local = diff.clip_version;
        }
    }

    pub(crate) fn current_clip_version(&self) -> u32 {
        if self.clip_version_local != 0 {
            self.clip_version_local
        } else {
            self.snapshot.ui_clip_version
        }
    }

    pub(crate) fn bump_clip_version(&mut self) {
        let next = self.current_clip_version().saturating_add(1);
        self.clip_version_local = next;
    }

    pub(crate) fn current_harmony_version(&self) -> u32 {
        if self.harmony_version_local != 0 {
            self.harmony_version_local
        } else {
            self.snapshot.ui_harmony_version
        }
    }

    pub(crate) fn bump_harmony_version(&mut self) {
        let next = self.current_harmony_version().saturating_add(1);
        self.harmony_version_local = next;
    }
}
