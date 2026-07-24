use std::collections::BTreeMap;

use daw_bridge::layout::{
    UiChordDiffPayload, UiChordDiffType, UiClipWindowSnapshot, UiDiffPayload, UiDiffType,
};

use crate::state::{ClipChord, ClipNote, PendingChord, PendingNote};

#[derive(Clone, Debug)]
pub struct ClipTrack {
    notes: Vec<BTreeMap<u64, ClipNote>>,
    chords: Vec<BTreeMap<u64, ClipChord>>,
    window_start: u64,
    window_end: u64,
}

#[derive(Clone, Debug)]
pub struct ClipStore {
    tracks: Vec<ClipTrack>,
}

impl ClipStore {
    pub fn new(track_count: usize, max_columns: usize) -> Self {
        let mut tracks = Vec::with_capacity(track_count);
        for _ in 0..track_count {
            tracks.push(ClipTrack::new(max_columns));
        }
        Self { tracks }
    }

    pub fn tracks(&self) -> &[ClipTrack] {
        &self.tracks
    }

    pub fn track(&self, track: usize) -> Option<&ClipTrack> {
        self.tracks.get(track)
    }

    pub fn track_mut(&mut self, track: usize) -> Option<&mut ClipTrack> {
        self.tracks.get_mut(track)
    }

    pub fn note_at(&self, track: usize, column: u8, nanotick: u64) -> Option<&ClipNote> {
        let track = self.tracks.get(track)?;
        let map = track.notes.get(column as usize)?;
        map.get(&nanotick)
    }

    pub fn chord_at(&self, track: usize, column: u8, nanotick: u64) -> Option<&ClipChord> {
        let track = self.tracks.get(track)?;
        let map = track.chords.get(column as usize)?;
        map.get(&nanotick)
    }

    pub fn prev_note_on_nanotick(&self, track: usize, column: u8, nanotick: u64) -> Option<u64> {
        self.tracks.get(track)?.prev_note_on_nanotick(column, nanotick)
    }

    pub fn next_note_on_nanotick(&self, track: usize, column: u8, nanotick: u64) -> Option<u64> {
        self.tracks.get(track)?.next_note_on_nanotick(column, nanotick)
    }

    pub fn prev_chord_nanotick(&self, track: usize, column: u8, nanotick: u64) -> Option<u64> {
        self.tracks.get(track)?.prev_chord_nanotick(column, nanotick)
    }

    pub fn next_chord_nanotick(&self, track: usize, column: u8, nanotick: u64) -> Option<u64> {
        self.tracks.get(track)?.next_chord_nanotick(column, nanotick)
    }

    pub fn remove_note_at(&mut self, track: usize, column: u8, nanotick: u64) -> bool {
        let Some(track) = self.tracks.get_mut(track) else {
            return false;
        };
        let Some(map) = track.notes.get_mut(column as usize) else {
            return false;
        };
        map.remove(&nanotick).is_some()
    }

    pub fn insert_note(&mut self, track: usize, note: ClipNote) -> bool {
        let Some(track) = self.tracks.get_mut(track) else {
            return false;
        };
        let Some(map) = track.notes.get_mut(note.column as usize) else {
            return false;
        };
        map.insert(note.nanotick, note).is_some()
    }

    pub fn remove_chord_at(&mut self, track: usize, column: u8, nanotick: u64) -> bool {
        let Some(track) = self.tracks.get_mut(track) else {
            return false;
        };
        let Some(map) = track.chords.get_mut(column as usize) else {
            return false;
        };
        map.remove(&nanotick).is_some()
    }

    pub fn insert_chord(&mut self, track: usize, chord: ClipChord) -> bool {
        let Some(track) = self.tracks.get_mut(track) else {
            return false;
        };
        let Some(map) = track.chords.get_mut(chord.column as usize) else {
            return false;
        };
        map.insert(chord.nanotick, chord).is_some()
    }

    pub fn remove_chord_by_id(&mut self, track: usize, chord_id: u32) -> bool {
        let Some(track) = self.tracks.get_mut(track) else {
            return false;
        };
        let mut removed = false;
        for map in track.chords.iter_mut() {
            let before = map.len();
            map.retain(|_, chord| chord.chord_id != chord_id);
            if map.len() != before {
                removed = true;
            }
        }
        removed
    }

    pub fn remove_note_offs_in_span(
        &mut self,
        track: usize,
        column: u8,
        nanotick: u64,
        prev_boundary: Option<u64>,
        next_boundary: Option<u64>,
    ) -> Vec<u64> {
        let Some(track) = self.tracks.get_mut(track) else {
            return Vec::new();
        };
        let Some(map) = track.notes.get_mut(column as usize) else {
            return Vec::new();
        };
        let lower = prev_boundary.unwrap_or(0);
        let upper = next_boundary.unwrap_or(u64::MAX);
        let start = lower.saturating_add(1);
        let keys: Vec<u64> = map
            .range(start..upper)
            .filter(|(&tick, note)| {
                if tick == nanotick {
                    return false;
                }
                note.velocity == 0 && note.duration == 0
            })
            .map(|(&tick, _)| tick)
            .collect();
        for tick in &keys {
            map.remove(tick);
        }
        keys
    }

    pub fn apply_clip_window_page(&mut self, snapshot: UiClipWindowSnapshot, reset: bool) {
        let track_index = snapshot.track_id as usize;
        let Some(track) = self.tracks.get_mut(track_index) else {
            return;
        };
        if reset {
            for map in track.notes.iter_mut() {
                map.clear();
            }
            for map in track.chords.iter_mut() {
                map.clear();
            }
        }
        track.window_start = snapshot.window_start_nanotick;
        track.window_end = snapshot.window_end_nanotick;

        // Counts come from shared memory and must be treated as untrusted: a
        // stale or corrupt writer must not index past the fixed-size arrays.
        let note_count = (snapshot.note_count as usize).min(snapshot.notes.len());
        let chord_count = (snapshot.chord_count as usize).min(snapshot.chords.len());

        for note_index in 0..note_count {
            let note = snapshot.notes[note_index];
            let column = note.column as usize;
            if column >= track.notes.len() {
                continue;
            }
            let entry = ClipNote {
                nanotick: note.t_on,
                duration: note.t_off.saturating_sub(note.t_on),
                pitch: note.pitch,
                velocity: note.velocity,
                column: note.column,
            };
            track.notes[column].insert(entry.nanotick, entry);
        }

        for chord_index in 0..chord_count {
            let chord = snapshot.chords[chord_index];
            let column = (chord.flags & 0xff) as usize;
            if column >= track.chords.len() {
                continue;
            }
            let entry = ClipChord {
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
                column: column as u8,
            };
            track.chords[column].insert(entry.nanotick, entry);
        }
    }

    pub fn apply_note_diff(&mut self, diff: &UiDiffPayload) -> bool {
        let track_index = diff.track_id as usize;
        let Some(track) = self.tracks.get_mut(track_index) else {
            return false;
        };
        let nanotick =
            (diff.note_nanotick_lo as u64) | ((diff.note_nanotick_hi as u64) << 32);
        if !track.window_contains(nanotick) {
            return false;
        }
        let duration =
            (diff.note_duration_lo as u64) | ((diff.note_duration_hi as u64) << 32);
        let pitch = diff.note_pitch.min(127) as u8;
        let velocity = diff.note_velocity.min(127) as u8;
        let column = diff.note_column.min(255) as u8;

        match diff.diff_type {
            x if x == UiDiffType::AddNote as u16 || x == UiDiffType::UpdateNote as u16 => {
                if let Some(chords) = track.chords.get_mut(column as usize) {
                    chords.remove(&nanotick);
                }
                {
                    let Some(notes) = track.notes.get_mut(column as usize) else {
                        return false;
                    };
                    notes.remove(&nanotick);
                }
                let is_note_off = velocity == 0 && duration == 0;
                let (prev_boundary, next_boundary) = if is_note_off {
                    (
                        track.prev_boundary_nanotick(column, nanotick),
                        track.next_boundary_nanotick(column, nanotick),
                    )
                } else {
                    (None, None)
                };
                let Some(notes) = track.notes.get_mut(column as usize) else {
                    return false;
                };
                if is_note_off {
                    let lower = prev_boundary.unwrap_or(0);
                    let upper = next_boundary.unwrap_or(u64::MAX);
                    let start = lower.saturating_add(1);
                    let keys: Vec<u64> = notes
                        .range(start..upper)
                        .filter(|(&tick, note)| {
                            if tick == nanotick {
                                return false;
                            }
                            note.velocity == 0 && note.duration == 0
                        })
                        .map(|(&tick, _)| tick)
                        .collect();
                    for tick in keys {
                        notes.remove(&tick);
                    }
                }
                notes.insert(
                    nanotick,
                    ClipNote {
                        nanotick,
                        duration,
                        pitch,
                        velocity,
                        column,
                    },
                );
                true
            }
            x if x == UiDiffType::RemoveNote as u16 => {
                let Some(notes) = track.notes.get_mut(column as usize) else {
                    return false;
                };
                notes.remove(&nanotick).is_some()
            }
            _ => false,
        }
    }

    pub fn apply_chord_diff(&mut self, diff: &UiChordDiffPayload) -> bool {
        let track_index = diff.track_id as usize;
        let Some(track) = self.tracks.get_mut(track_index) else {
            return false;
        };
        let nanotick = (diff.nanotick_lo as u64) | ((diff.nanotick_hi as u64) << 32);
        if !track.window_contains(nanotick) {
            return false;
        }
        let duration = (diff.duration_lo as u64) | ((diff.duration_hi as u64) << 32);
        let (degree, quality, inversion, base_octave) = crate::util::unpack_chord_packed(diff.packed);
        let (spread, column) = crate::util::unpack_chord_spread(diff.spread_nanoticks);
        let humanize_timing = (diff.flags & 0xff) as u16;
        let humanize_velocity = ((diff.flags >> 8) & 0xff) as u16;
        let Some(chords) = track.chords.get_mut(column as usize) else {
            return false;
        };

        match diff.diff_type {
            x if x == UiChordDiffType::AddChord as u16 || x == UiChordDiffType::UpdateChord as u16 => {
                if let Some(notes) = track.notes.get_mut(column as usize) {
                    notes.remove(&nanotick);
                }
                chords.retain(|_, chord| chord.chord_id != diff.chord_id);
                chords.remove(&nanotick);
                chords.insert(
                    nanotick,
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
                true
            }
            x if x == UiChordDiffType::RemoveChord as u16 => {
                if diff.chord_id != 0 {
                    let before = chords.len();
                    chords.retain(|_, chord| chord.chord_id != diff.chord_id);
                    before != chords.len()
                } else {
                    chords.remove(&nanotick).is_some()
                }
            }
            _ => false,
        }
    }
}

impl ClipTrack {
    fn new(max_columns: usize) -> Self {
        let mut notes = Vec::with_capacity(max_columns);
        let mut chords = Vec::with_capacity(max_columns);
        for _ in 0..max_columns {
            notes.push(BTreeMap::new());
            chords.push(BTreeMap::new());
        }
        Self {
            notes,
            chords,
            window_start: 0,
            window_end: 0,
        }
    }

    pub fn window_start(&self) -> u64 {
        self.window_start
    }

    pub fn window_end(&self) -> u64 {
        self.window_end
    }

    pub fn notes(&self) -> &Vec<BTreeMap<u64, ClipNote>> {
        &self.notes
    }

    /// The tick at which an OFF marker should render in `column` within
    /// `[start, end)`, if any. A note that is cut on next ends exactly where
    /// the following event begins, so an OFF is shown only at a genuine gap:
    /// a note ends in this span and nothing else starts at that tick.
    pub fn note_off_tick_in(&self, column: usize, start: u64, end: u64) -> Option<u64> {
        let notes = self.notes.get(column)?;
        for note in notes.values() {
            if note.duration == 0 {
                continue;
            }
            let off = note.nanotick.saturating_add(note.duration);
            if off < start || off >= end {
                continue;
            }
            let occupied = self.notes.get(column).is_some_and(|m| m.contains_key(&off))
                || self.chords.get(column).is_some_and(|m| m.contains_key(&off));
            if !occupied {
                return Some(off);
            }
        }
        None
    }

    pub fn chords(&self) -> &Vec<BTreeMap<u64, ClipChord>> {
        &self.chords
    }

    fn window_contains(&self, nanotick: u64) -> bool {
        if self.window_end <= self.window_start {
            return false;
        }
        nanotick >= self.window_start && nanotick < self.window_end
    }

    fn prev_note_on_nanotick(&self, column: u8, nanotick: u64) -> Option<u64> {
        let map = self.notes.get(column as usize)?;
        map.range(..nanotick)
            .rev()
            .find(|(_, note)| note.velocity > 0)
            .map(|(&tick, _)| tick)
    }

    fn next_note_on_nanotick(&self, column: u8, nanotick: u64) -> Option<u64> {
        let map = self.notes.get(column as usize)?;
        map.range(nanotick.saturating_add(1)..)
            .find(|(_, note)| note.velocity > 0)
            .map(|(&tick, _)| tick)
    }

    fn prev_chord_nanotick(&self, column: u8, nanotick: u64) -> Option<u64> {
        let map = self.chords.get(column as usize)?;
        map.range(..nanotick).next_back().map(|(&tick, _)| tick)
    }

    fn next_chord_nanotick(&self, column: u8, nanotick: u64) -> Option<u64> {
        let map = self.chords.get(column as usize)?;
        map.range(nanotick.saturating_add(1)..).next().map(|(&tick, _)| tick)
    }

    fn prev_boundary_nanotick(&self, column: u8, nanotick: u64) -> Option<u64> {
        match (
            self.prev_note_on_nanotick(column, nanotick),
            self.prev_chord_nanotick(column, nanotick),
        ) {
            (Some(a), Some(b)) => Some(a.max(b)),
            (Some(a), None) => Some(a),
            (None, Some(b)) => Some(b),
            (None, None) => None,
        }
    }

    fn next_boundary_nanotick(&self, column: u8, nanotick: u64) -> Option<u64> {
        match (
            self.next_note_on_nanotick(column, nanotick),
            self.next_chord_nanotick(column, nanotick),
        ) {
            (Some(a), Some(b)) => Some(a.min(b)),
            (Some(a), None) => Some(a),
            (None, Some(b)) => Some(b),
            (None, None) => None,
        }
    }

}

#[derive(Clone, Debug)]
pub struct PendingOverlay {
    notes: Vec<Vec<BTreeMap<u64, PendingNote>>>,
    chords: Vec<Vec<BTreeMap<u64, PendingChord>>>,
    note_count: usize,
    chord_count: usize,
}

impl PendingOverlay {
    pub fn new(track_count: usize, max_columns: usize) -> Self {
        let mut notes = Vec::with_capacity(track_count);
        let mut chords = Vec::with_capacity(track_count);
        for _ in 0..track_count {
            let mut track_notes = Vec::with_capacity(max_columns);
            let mut track_chords = Vec::with_capacity(max_columns);
            for _ in 0..max_columns {
                track_notes.push(BTreeMap::new());
                track_chords.push(BTreeMap::new());
            }
            notes.push(track_notes);
            chords.push(track_chords);
        }
        Self {
            notes,
            chords,
            note_count: 0,
            chord_count: 0,
        }
    }

    /// Drops every unconfirmed edit for a track. Pending entries are normally
    /// retired one by one as the engine confirms them, so an edit the engine
    /// rejected would otherwise linger as a ghost cell forever. A resync
    /// replaces the track's confirmed state wholesale, which is the one moment
    /// we know the surviving pending entries are stale.
    pub fn clear_track(&mut self, track: usize) -> bool {
        let mut removed = false;
        if let Some(columns) = self.notes.get_mut(track) {
            for map in columns.iter_mut() {
                if !map.is_empty() {
                    self.note_count = self.note_count.saturating_sub(map.len());
                    map.clear();
                    removed = true;
                }
            }
        }
        if let Some(columns) = self.chords.get_mut(track) {
            for map in columns.iter_mut() {
                if !map.is_empty() {
                    self.chord_count = self.chord_count.saturating_sub(map.len());
                    map.clear();
                    removed = true;
                }
            }
        }
        removed
    }

    pub fn is_empty(&self) -> bool {
        self.note_count == 0 && self.chord_count == 0
    }

    pub fn note_count(&self) -> usize {
        self.note_count
    }

    pub fn chord_count(&self) -> usize {
        self.chord_count
    }

    pub fn notes_for_track(&self, track: usize) -> Option<&Vec<BTreeMap<u64, PendingNote>>> {
        self.notes.get(track)
    }

    pub fn chords_for_track(&self, track: usize) -> Option<&Vec<BTreeMap<u64, PendingChord>>> {
        self.chords.get(track)
    }

    pub fn chords_for_track_mut(
        &mut self,
        track: usize,
    ) -> Option<&mut Vec<BTreeMap<u64, PendingChord>>> {
        self.chords.get_mut(track)
    }

    pub fn notes_for_track_mut(
        &mut self,
        track: usize,
    ) -> Option<&mut Vec<BTreeMap<u64, PendingNote>>> {
        self.notes.get_mut(track)
    }

    pub fn note_at(&self, track: usize, column: u8, nanotick: u64) -> Option<&PendingNote> {
        let track = self.notes.get(track)?;
        let map = track.get(column as usize)?;
        map.get(&nanotick)
    }

    pub fn chord_at(&self, track: usize, column: u8, nanotick: u64) -> Option<&PendingChord> {
        let track = self.chords.get(track)?;
        let map = track.get(column as usize)?;
        map.get(&nanotick)
    }

    pub fn insert_note(&mut self, track: usize, column: u8, note: PendingNote) {
        let Some(map) = self
            .notes
            .get_mut(track)
            .and_then(|cols| cols.get_mut(column as usize))
        else {
            return;
        };
        if map.insert(note.nanotick, note).is_none() {
            self.note_count = self.note_count.saturating_add(1);
        }
    }

    pub fn insert_chord(&mut self, track: usize, column: u8, chord: PendingChord) {
        let Some(map) = self
            .chords
            .get_mut(track)
            .and_then(|cols| cols.get_mut(column as usize))
        else {
            return;
        };
        if map.insert(chord.nanotick, chord).is_none() {
            self.chord_count = self.chord_count.saturating_add(1);
        }
    }

    pub fn remove_note_at(&mut self, track: usize, column: u8, nanotick: u64) -> bool {
        let Some(map) = self
            .notes
            .get_mut(track)
            .and_then(|cols| cols.get_mut(column as usize))
        else {
            return false;
        };
        if map.remove(&nanotick).is_some() {
            self.note_count = self.note_count.saturating_sub(1);
            return true;
        }
        false
    }

    pub fn remove_chord_at(&mut self, track: usize, column: u8, nanotick: u64) -> bool {
        let Some(map) = self
            .chords
            .get_mut(track)
            .and_then(|cols| cols.get_mut(column as usize))
        else {
            return false;
        };
        if map.remove(&nanotick).is_some() {
            self.chord_count = self.chord_count.saturating_sub(1);
            return true;
        }
        false
    }

    pub fn remove_chords_where<F>(&mut self, mut predicate: F) -> bool
    where
        F: FnMut(&PendingChord) -> bool,
    {
        let mut removed_any = false;
        for track_maps in self.chords.iter_mut() {
            for map in track_maps.iter_mut() {
                let keys: Vec<u64> = map
                    .iter()
                    .filter(|(_, chord)| predicate(chord))
                    .map(|(&tick, _)| tick)
                    .collect();
                if keys.is_empty() {
                    continue;
                }
                for tick in keys {
                    if map.remove(&tick).is_some() {
                        self.chord_count = self.chord_count.saturating_sub(1);
                        removed_any = true;
                    }
                }
            }
        }
        removed_any
    }

    pub fn remove_note_offs_in_span(
        &mut self,
        track: usize,
        column: u8,
        nanotick: u64,
        prev_boundary: Option<u64>,
        next_boundary: Option<u64>,
    ) -> Vec<u64> {
        let Some(map) = self
            .notes
            .get_mut(track)
            .and_then(|cols| cols.get_mut(column as usize))
        else {
            return Vec::new();
        };
        let lower = prev_boundary.unwrap_or(0);
        let upper = next_boundary.unwrap_or(u64::MAX);
        let start = lower.saturating_add(1);
        let keys: Vec<u64> = map
            .range(start..upper)
            .filter(|(&tick, note)| {
                if tick == nanotick {
                    return false;
                }
                note.velocity == 0 && note.duration == 0
            })
            .map(|(&tick, _)| tick)
            .collect();
        for tick in &keys {
            map.remove(tick);
            self.note_count = self.note_count.saturating_sub(1);
        }
        keys
    }

    pub fn iter_notes(&self) -> impl Iterator<Item = &PendingNote> {
        self.notes
            .iter()
            .flat_map(|cols| cols.iter().flat_map(|map| map.values()))
    }

    pub fn iter_chords(&self) -> impl Iterator<Item = &PendingChord> {
        self.chords
            .iter()
            .flat_map(|cols| cols.iter().flat_map(|map| map.values()))
    }

    pub fn prev_note_on_nanotick(&self, track: usize, column: u8, nanotick: u64) -> Option<u64> {
        let map = self
            .notes
            .get(track)?
            .get(column as usize)?;
        map.range(..nanotick)
            .rev()
            .find(|(_, note)| note.velocity > 0)
            .map(|(&tick, _)| tick)
    }

    pub fn next_note_on_nanotick(&self, track: usize, column: u8, nanotick: u64) -> Option<u64> {
        let map = self
            .notes
            .get(track)?
            .get(column as usize)?;
        map.range(nanotick.saturating_add(1)..)
            .find(|(_, note)| note.velocity > 0)
            .map(|(&tick, _)| tick)
    }

    pub fn prev_chord_nanotick(&self, track: usize, column: u8, nanotick: u64) -> Option<u64> {
        let map = self
            .chords
            .get(track)?
            .get(column as usize)?;
        map.range(..nanotick).next_back().map(|(&tick, _)| tick)
    }

    pub fn next_chord_nanotick(&self, track: usize, column: u8, nanotick: u64) -> Option<u64> {
        let map = self
            .chords
            .get(track)?
            .get(column as usize)?;
        map.range(nanotick.saturating_add(1)..).next().map(|(&tick, _)| tick)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::state::ClipNote;

    fn note(nanotick: u64, duration: u64, pitch: u8, column: u8) -> ClipNote {
        ClipNote { nanotick, duration, pitch, velocity: 100, column }
    }

    #[test]
    fn off_marker_shows_only_at_a_genuine_gap() {
        let mut store = ClipStore::new(1, 4);
        // A note from tick 0 lasting one beat, then a gap, then another note.
        store.insert_note(0, note(0, 960_000, 60, 0));
        store.insert_note(0, note(1_920_000, 960_000, 64, 0));
        let track = store.track(0).unwrap();

        // The first note ends at 960_000 with nothing starting there — OFF.
        assert_eq!(
            track.note_off_tick_in(0, 720_000, 1_200_000),
            Some(960_000),
            "a note ending in a gap should mark OFF at its end"
        );
        // The second note ends at 2_880_000; also a gap after it.
        assert_eq!(track.note_off_tick_in(0, 2_640_000, 3_120_000), Some(2_880_000));
    }

    #[test]
    fn off_marker_is_suppressed_when_next_note_starts_there() {
        let mut store = ClipStore::new(1, 4);
        // Back-to-back notes: the first ends exactly where the second begins,
        // so the second's label shows, not an OFF.
        store.insert_note(0, note(0, 960_000, 60, 0));
        store.insert_note(0, note(960_000, 960_000, 64, 0));
        let track = store.track(0).unwrap();
        assert_eq!(
            track.note_off_tick_in(0, 720_000, 1_200_000),
            None,
            "no OFF when the next note starts at the previous note's end"
        );
    }

    #[test]
    fn off_marker_ignores_zero_duration_notes() {
        let mut store = ClipStore::new(1, 4);
        store.insert_note(0, note(0, 0, 60, 0));
        let track = store.track(0).unwrap();
        assert_eq!(track.note_off_tick_in(0, 0, 3_840_000), None);
    }
}
