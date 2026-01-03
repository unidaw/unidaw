use std::collections::HashMap;
use std::sync::{Arc, OnceLock};

use gpui::{div, px, rgb, Context, FontWeight, IntoElement, MouseButton, SharedString};
use gpui::prelude::*;

use crate::app::{
    chord_token_text, pitch_to_note, EngineView, DEFAULT_ZOOM_INDEX,
    NANOTICKS_PER_QUARTER, TRACK_COUNT, ZOOM_LEVELS, TrackerCache, TrackerCacheKey,
    TrackerRowCache,
};
use crate::harmony::{harmony_root_name, harmony_scale_name};
use crate::state::{AggregateCell, AggregateSingle, ClipChord, HarmonyAggregate};
use crate::tracker::{
    COLUMN_WIDTH, HEADER_HEIGHT, HARMONY_COLUMN_WIDTH, ROW_HEIGHT, TIME_COLUMN_WIDTH,
    VISIBLE_ROWS,
};

fn empty_label() -> SharedString {
    static EMPTY: OnceLock<SharedString> = OnceLock::new();
    EMPTY.get_or_init(|| SharedString::from("")).clone()
}

fn off_label() -> SharedString {
    static OFF: OnceLock<SharedString> = OnceLock::new();
    OFF.get_or_init(|| SharedString::from("OFF")).clone()
}

fn cached_note_label(pitch: u8) -> SharedString {
    static LABELS: OnceLock<Vec<SharedString>> = OnceLock::new();
    let labels = LABELS.get_or_init(|| {
        (0..128)
            .map(|value| SharedString::from(pitch_to_note(value as u8)))
            .collect()
    });
    labels[pitch.min(127) as usize].clone()
}

fn time_label_for_nanotick(nanotick: u64) -> SharedString {
    let total_beats = nanotick / crate::app::NANOTICKS_PER_QUARTER;
    let bar = total_beats / crate::app::BEATS_PER_BAR + 1;
    let beat = total_beats % crate::app::BEATS_PER_BAR + 1;
    let tick = (nanotick % crate::app::NANOTICKS_PER_QUARTER) / 10_000;
    SharedString::from(format!("{}:{}:{}", bar, beat, tick))
}

fn set_cell_label(
    rows: &mut [TrackerRowCache],
    row_index: usize,
    track_index: usize,
    column: usize,
    label: SharedString,
) {
    let Some(row) = rows.get_mut(row_index) else {
        return;
    };
    let Some(track_cells) = row.cell_labels.get_mut(track_index) else {
        return;
    };
    if column >= track_cells.len() {
        return;
    }
    if track_cells[column].is_none() {
        track_cells[column] = Some(label);
    }
}

fn cell_id(row_index: usize, track_index: usize, column: usize) -> usize {
    row_index * 1_000_000 + track_index * 1000 + column
}

struct PendingOverlay {
    labels: HashMap<usize, SharedString>,
}

impl PendingOverlay {
    fn label_for(&self, cell_id: usize) -> Option<SharedString> {
        self.labels.get(&cell_id).cloned()
    }
}

impl EngineView {
    fn tracker_cache_key(&self) -> TrackerCacheKey {
        TrackerCacheKey {
            window_start: self.scroll_nanotick_offset.max(0) as u64,
            row_nanoticks: self.row_nanoticks(),
            clip_render_version: self.clip_render_version,
            harmony_render_version: self.harmony_render_version,
            track_columns: self.track_columns.clone(),
        }
    }

    fn build_tracker_cache(&self, key: TrackerCacheKey) -> TrackerCache {
        let row_nanoticks = key.row_nanoticks.max(1);
        let visible_start = key.window_start;
        let visible_end = visible_start.saturating_add(
            row_nanoticks.saturating_mul(VISIBLE_ROWS as u64),
        );
        let mut rows = Vec::with_capacity(VISIBLE_ROWS);
        for row_index in 0..VISIBLE_ROWS {
            let row_start = visible_start + row_index as u64 * row_nanoticks;
            let time_label = time_label_for_nanotick(row_start);
            let harmony_label = self
                .harmony_label_at_nanotick(row_start)
                .map(SharedString::from);
            let mut cell_labels = Vec::with_capacity(TRACK_COUNT);
            for track in 0..TRACK_COUNT {
                let columns = key.track_columns.get(track).copied().unwrap_or(0);
                cell_labels.push(vec![None; columns]);
            }
            rows.push(TrackerRowCache {
                row_start,
                time_label,
                harmony_label,
                cell_labels,
            });
        }
        let row_index_for = |nanotick: u64| -> Option<usize> {
            if nanotick < visible_start || nanotick >= visible_end {
                return None;
            }
            let delta = nanotick - visible_start;
            if delta % row_nanoticks != 0 {
                return None;
            }
            let row_index = (delta / row_nanoticks) as usize;
            if row_index < VISIBLE_ROWS {
                Some(row_index)
            } else {
                None
            }
        };

        for (track_index, notes) in self.clip_notes.iter().enumerate() {
            let columns = key.track_columns.get(track_index).copied().unwrap_or(0);
            for note in notes.iter() {
                let Some(row_index) = row_index_for(note.nanotick) else {
                    continue;
                };
                let column = note.column as usize;
                if column >= columns {
                    continue;
                }
                let label = if note.velocity == 0 && note.duration == 0 {
                    off_label()
                } else {
                    cached_note_label(note.pitch)
                };
                set_cell_label(&mut rows, row_index, track_index, column, label);
            }
        }
        for (track_index, chords) in self.clip_chords.iter().enumerate() {
            let columns = key.track_columns.get(track_index).copied().unwrap_or(0);
            for chord in chords.iter() {
                let Some(row_index) = row_index_for(chord.nanotick) else {
                    continue;
                };
                let column = chord.column as usize;
                if column >= columns {
                    continue;
                }
                let label = SharedString::from(chord_token_text(chord));
                set_cell_label(&mut rows, row_index, track_index, column, label);
            }
        }

        TrackerCache { key, rows }
    }

    fn tracker_cache(&mut self) -> Option<Arc<TrackerCache>> {
        if self.should_aggregate_rows() {
            return None;
        }
        let key = self.tracker_cache_key();
        let needs_rebuild = self
            .tracker_cache
            .as_ref()
            .map_or(true, |cache| cache.key != key);
        if needs_rebuild {
            self.tracker_cache = Some(Arc::new(self.build_tracker_cache(key)));
        }
        self.tracker_cache.clone()
    }

    fn pending_overlay(&self) -> Option<PendingOverlay> {
        if self.pending_notes.is_empty() && self.pending_chords.is_empty() {
            return None;
        }
        let row_nanoticks = self.row_nanoticks().max(1);
        let visible_start = self.scroll_nanotick_offset.max(0) as u64;
        let visible_end = visible_start.saturating_add(
            row_nanoticks.saturating_mul(VISIBLE_ROWS as u64),
        );
        let row_index_for = |nanotick: u64| -> Option<usize> {
            if nanotick < visible_start || nanotick >= visible_end {
                return None;
            }
            let delta = nanotick - visible_start;
            if delta % row_nanoticks != 0 {
                return None;
            }
            let row_index = (delta / row_nanoticks) as usize;
            if row_index < VISIBLE_ROWS {
                Some(row_index)
            } else {
                None
            }
        };
        let mut labels = HashMap::with_capacity(
            self.pending_notes.len().saturating_add(self.pending_chords.len()),
        );
        for note in self.pending_notes.iter() {
            let track_index = note.track_id as usize;
            let columns = self.track_columns.get(track_index).copied().unwrap_or(0);
            let Some(row_index) = row_index_for(note.nanotick) else {
                continue;
            };
            let column = note.column as usize;
            if column >= columns {
                continue;
            }
            let label = if note.velocity == 0 && note.duration == 0 {
                off_label()
            } else {
                cached_note_label(note.pitch)
            };
            labels.insert(cell_id(row_index, track_index, column), label);
        }
        for chord in self.pending_chords.iter() {
            let track_index = chord.track_id as usize;
            let columns = self.track_columns.get(track_index).copied().unwrap_or(0);
            let Some(row_index) = row_index_for(chord.nanotick) else {
                continue;
            };
            let column = chord.column as usize;
            if column >= columns {
                continue;
            }
            let temp = ClipChord {
                chord_id: 0,
                nanotick: chord.nanotick,
                duration: chord.duration,
                spread: chord.spread,
                humanize_timing: chord.humanize_timing,
                humanize_velocity: chord.humanize_velocity,
                degree: chord.degree,
                quality: chord.quality,
                inversion: chord.inversion,
                base_octave: chord.base_octave,
                column: chord.column,
            };
            let label = SharedString::from(chord_token_text(&temp));
            labels.entry(cell_id(row_index, track_index, column)).or_insert(label);
        }
        if labels.is_empty() {
            None
        } else {
            Some(PendingOverlay { labels })
        }
    }

    pub(crate) fn should_aggregate_rows(&self) -> bool {
        let base_row = NANOTICKS_PER_QUARTER / ZOOM_LEVELS[DEFAULT_ZOOM_INDEX];
        self.row_nanoticks() > base_row
    }

    pub(crate) fn aggregate_cells_in_range(
        &self,
        start: u64,
        end: u64,
        track_index: usize,
        columns: usize,
    ) -> Vec<AggregateCell> {
        let mut aggregates = vec![AggregateCell::new(); columns];
        if let Some(notes) = self.clip_notes.get(track_index) {
            for note in notes.iter() {
                if note.nanotick < start || note.nanotick >= end {
                    continue;
                }
                let column = note.column as usize;
                if column >= columns {
                    continue;
                }
                let is_note_off = note.velocity == 0 && note.duration == 0;
                aggregates[column].add_note(note.pitch, is_note_off);
            }
        }
        for note in self
            .pending_notes
            .iter()
            .filter(|note| note.track_id as usize == track_index)
        {
            if note.nanotick < start || note.nanotick >= end {
                continue;
            }
            let column = note.column as usize;
            if column >= columns {
                continue;
            }
            let is_note_off = note.velocity == 0 && note.duration == 0;
            aggregates[column].add_note(note.pitch, is_note_off);
        }
        if let Some(chords) = self.clip_chords.get(track_index) {
            for chord in chords.iter() {
                if chord.nanotick < start || chord.nanotick >= end {
                    continue;
                }
                let column = chord.column as usize;
                if column >= columns {
                    continue;
                }
                aggregates[column].add_chord(chord.clone());
            }
        }
        for chord in self
            .pending_chords
            .iter()
            .filter(|chord| chord.track_id as usize == track_index)
        {
            if chord.nanotick < start || chord.nanotick >= end {
                continue;
            }
            let column = chord.column as usize;
            if column >= columns {
                continue;
            }
            aggregates[column].add_chord(ClipChord {
                chord_id: 0,
                nanotick: chord.nanotick,
                duration: chord.duration,
                spread: chord.spread,
                humanize_timing: chord.humanize_timing,
                humanize_velocity: chord.humanize_velocity,
                degree: chord.degree,
                quality: chord.quality,
                inversion: chord.inversion,
                base_octave: chord.base_octave,
                column: chord.column,
            });
        }
        aggregates
    }

    pub(crate) fn aggregate_cell_label(&self, aggregate: &AggregateCell) -> Option<String> {
        if aggregate.count == 0 {
            return None;
        }
        if aggregate.note_off_only && aggregate.count > 1 {
            return Some(format!("[OFFx {}]", aggregate.count));
        }
        if let Some(single) = &aggregate.single {
            match single {
                AggregateSingle::Note { pitch, note_off } => {
                    if *note_off {
                        return None;
                    }
                    return Some(pitch_to_note(*pitch));
                }
                AggregateSingle::Chord(chord) => {
                    return Some(chord_token_text(chord));
                }
            }
        }
        if aggregate.notes_only {
            if let Some(pitch) = aggregate.unique_pitch {
                return Some(format!("[{}x {}]", aggregate.count, pitch_to_note(pitch)));
            }
        }
        Some(format!("[{}]", aggregate.count))
    }

    pub(crate) fn aggregate_harmony_in_range(
        &self,
        start: u64,
        end: u64,
    ) -> HarmonyAggregate {
        let mut labels = Vec::new();
        for event in self.harmony_events.iter() {
            if event.nanotick < start || event.nanotick >= end {
                continue;
            }
            labels.push(format!(
                "{}:{}",
                harmony_root_name(event.root),
                harmony_scale_name(event.scale_id)
            ));
        }
        HarmonyAggregate {
            count: labels.len(),
            labels,
        }
    }

    /// Render a single tracker row inline
    fn render_tracker_row(
        &self,
        row_index: usize,
        cache: Option<&TrackerCache>,
        pending_overlay: Option<&PendingOverlay>,
        cx: &mut Context<Self>,
    ) -> impl IntoElement {
        let row_nanoticks = self.row_nanoticks();
        let row_cache = cache.and_then(|cache| cache.rows.get(row_index));
        let row_start = row_cache
            .map(|row| row.row_start)
            .unwrap_or_else(|| {
                self.scroll_nanotick_offset as u64 + row_index as u64 * row_nanoticks
            });
        let row_end = row_start + row_nanoticks;
        let cursor_row = self.cursor_view_row();
        let is_cursor_row = cursor_row >= 0 && cursor_row as usize == row_index;
        let playhead_pos = self.snapshot.ui_global_nanotick_playhead;
        let is_playhead_row = playhead_pos >= row_start && playhead_pos < row_end;
        let should_aggregate = cache.is_none() && self.should_aggregate_rows();

        let time_label = row_cache
            .map(|row| row.time_label.clone())
            .unwrap_or_else(|| time_label_for_nanotick(row_start));

        // Background color based on row state
        let bg_color = if is_playhead_row {
            rgb(0x1a2535) // Playhead row - subtle blue tint
        } else if is_cursor_row {
            rgb(0x1a2228) // Cursor row - subtle highlight
        } else if row_index % 4 == 0 {
            rgb(0x15191f)
        } else {
            rgb(0x12161b)
        };

        // Time column
        let time_col = div()
            .w(px(TIME_COLUMN_WIDTH))
            .h(px(ROW_HEIGHT))
            .flex()
            .items_center()
            .text_xs()
            .text_color(rgb(0x6a7a8a))
            .px_2()
            .child(time_label);

        // Harmony column
        let harmony_label = if should_aggregate {
            let agg = self.aggregate_harmony_in_range(row_start, row_end);
            if agg.count > 1 {
                SharedString::from(format!("[{}]", agg.count))
            } else {
                agg.labels
                    .first()
                    .map(|label| SharedString::from(label.clone()))
                    .unwrap_or_else(empty_label)
            }
        } else {
            row_cache
                .and_then(|row| row.harmony_label.clone())
                .or_else(|| {
                    self.harmony_label_at_nanotick(row_start)
                        .map(SharedString::from)
                })
                .unwrap_or_else(empty_label)
        };
        let harmony_bg = if is_cursor_row && self.harmony_focus {
            rgb(0x3a4a5a)
        } else {
            rgb(0x151922)
        };
        let harmony_col = div()
            .w(px(HARMONY_COLUMN_WIDTH))
            .h(px(ROW_HEIGHT))
            .flex()
            .items_center()
            .text_xs()
            .text_color(rgb(0x7fa0c0))
            .bg(harmony_bg)
            .border_l_1()
            .border_r_2()
            .border_color(rgb(0x3a4555))
            .px_2()
            .on_mouse_down(
                MouseButton::Left,
                cx.listener(move |view, _, _, cx| {
                    view.focus_harmony_row(row_index, cx);
                }),
            )
            .child(harmony_label);

        // Build row with stable ID for efficient GPUI diffing
        let mut row = div()
            .id(("tracker-row", row_index))
            .flex()
            .gap_0()
            .h(px(ROW_HEIGHT))
            .bg(bg_color)
            .child(time_col)
            .child(harmony_col);

        // Track columns
        for track in 0..TRACK_COUNT {
            let columns = row_cache
                .and_then(|row| row.cell_labels.get(track))
                .map(|cells| cells.len())
                .unwrap_or_else(|| self.track_columns[track]);

            if should_aggregate {
                let aggregates = self.aggregate_cells_in_range(row_start, row_end, track, columns);
                for (col_idx, agg) in aggregates.iter().enumerate() {
                    let is_cursor_cell = is_cursor_row
                        && !self.harmony_focus
                        && track == self.focused_track_index
                        && col_idx == self.cursor_col;
                    let cell_bg = if is_cursor_cell { rgb(0x3a4a5a) } else { bg_color };
                    let label = self.aggregate_cell_label(agg).unwrap_or_default();
                    let cell_id = cell_id(row_index, track, col_idx);
                    let cell = div()
                        .id(("cell", cell_id))
                        .w(px(COLUMN_WIDTH))
                        .h(px(ROW_HEIGHT))
                        .flex()
                        .items_center()
                        .text_xs()
                        .text_color(rgb(0xc0d0e0))
                        .bg(cell_bg)
                        .border_l_1()
                        .border_color(rgb(0x2a3545))
                        .px_1()
                        .on_mouse_down(
                            MouseButton::Left,
                            cx.listener(move |view, _, _, cx| {
                                view.focus_note_cell(row_index, track, col_idx, cx);
                            }),
                        )
                            .child(label);
                    row = row.child(cell);
                }
            } else if let Some(cells) = row_cache.and_then(|row| row.cell_labels.get(track)) {
                for col_idx in 0..columns {
                    let is_cursor_cell = is_cursor_row
                        && !self.harmony_focus
                        && track == self.focused_track_index
                        && col_idx == self.cursor_col;
                    let cell_bg = if is_cursor_cell { rgb(0x3a4a5a) } else { bg_color };
                    let cell_id = cell_id(row_index, track, col_idx);
                    let overlay_label = pending_overlay
                        .and_then(|overlay| overlay.label_for(cell_id));
                    let label = overlay_label.or_else(|| {
                        cells.get(col_idx).and_then(|label| label.clone())
                    }).unwrap_or_else(empty_label);
                    let cell = div()
                        .id(("cell", cell_id))
                        .w(px(COLUMN_WIDTH))
                        .h(px(ROW_HEIGHT))
                        .flex()
                        .items_center()
                        .text_xs()
                        .text_color(rgb(0xc0d0e0))
                        .bg(cell_bg)
                        .border_l_1()
                        .border_color(rgb(0x2a3545))
                        .px_1()
                        .on_mouse_down(
                            MouseButton::Left,
                            cx.listener(move |view, _, _, cx| {
                                view.focus_note_cell(row_index, track, col_idx, cx);
                            }),
                        )
                        .child(label);
                    row = row.child(cell);
                }
            } else {
                // Non-aggregated: show individual notes
                for col_idx in 0..columns {
                    let is_cursor_cell = is_cursor_row
                        && !self.harmony_focus
                        && track == self.focused_track_index
                        && col_idx == self.cursor_col;
                    let cell_bg = if is_cursor_cell { rgb(0x3a4a5a) } else { bg_color };

                    // Find note at this position
                    let note_label = self.clip_notes.get(track).and_then(|notes| {
                        notes.iter().find(|n| n.nanotick == row_start && n.column as usize == col_idx)
                    }).map(|n| {
                        if n.velocity == 0 && n.duration == 0 {
                            "OFF".to_string()
                        } else {
                            pitch_to_note(n.pitch)
                        }
                    }).or_else(|| {
                        // Check pending notes
                        self.pending_notes.iter().find(|n| {
                            n.track_id as usize == track && n.nanotick == row_start && n.column as usize == col_idx
                        }).map(|n| {
                            if n.velocity == 0 && n.duration == 0 {
                                "OFF".to_string()
                            } else {
                                pitch_to_note(n.pitch)
                            }
                        })
                    }).or_else(|| {
                        // Check chords
                        self.clip_chords.get(track).and_then(|chords| {
                            chords.iter().find(|c| c.nanotick == row_start && c.column as usize == col_idx)
                        }).map(|c| chord_token_text(c))
                    }).or_else(|| {
                        // Check pending chords
                        self.pending_chords.iter().find(|c| {
                            c.track_id as usize == track && c.nanotick == row_start && c.column as usize == col_idx
                        }).map(|c| chord_token_text(&crate::state::ClipChord {
                            chord_id: 0,
                            nanotick: c.nanotick,
                            duration: c.duration,
                            spread: c.spread,
                            humanize_timing: c.humanize_timing,
                            humanize_velocity: c.humanize_velocity,
                            degree: c.degree,
                            quality: c.quality,
                            inversion: c.inversion,
                            base_octave: c.base_octave,
                            column: c.column,
                        }))
                    });

                    let cell_id = cell_id(row_index, track, col_idx);
                    let cell = div()
                        .id(("cell", cell_id))
                        .w(px(COLUMN_WIDTH))
                        .h(px(ROW_HEIGHT))
                        .flex()
                        .items_center()
                        .text_xs()
                        .text_color(rgb(0xc0d0e0))
                        .bg(cell_bg)
                        .border_l_1()
                        .border_color(rgb(0x2a3545))
                        .px_1()
                        .on_mouse_down(
                            MouseButton::Left,
                            cx.listener(move |view, _, _, cx| {
                                view.focus_note_cell(row_index, track, col_idx, cx);
                            }),
                        )
                        .child(note_label.unwrap_or_default());
                    row = row.child(cell);
                }
            }
        }

        row
    }

    /// Render the tracker header row (TIME, HARM, T1, T2, etc.)
    fn render_tracker_header(&self, cx: &mut Context<Self>) -> impl IntoElement {
        let mut header = div()
            .flex()
            .gap_0()
            .items_center()
            .h(px(HEADER_HEIGHT))
            .bg(rgb(0x1a1f2b))
            .border_b_1()
            .border_color(rgb(0x3a4555))
            .child(
                div()
                    .w(px(TIME_COLUMN_WIDTH))
                    .h_full()
                    .flex()
                    .items_center()
                    .text_xs()
                    .font_weight(FontWeight::SEMIBOLD)
                    .text_color(rgb(0xb0bac4))
                    .px_2()
                    .child("TIME"),
            )
            .child(
                div()
                    .w(px(HARMONY_COLUMN_WIDTH))
                    .h_full()
                    .flex()
                    .items_center()
                    .text_sm()
                    .font_weight(FontWeight::SEMIBOLD)
                    .text_color(rgb(0x7fa0c0))
                    .bg(rgb(0x151922))
                    .border_l_1()
                    .border_r_2()
                    .border_color(rgb(0x3a4555))
                    .px_2()
                    .child("HARM"),
            );
        for track in 0..TRACK_COUNT {
            let columns = self.track_columns[track];
            let track_label = format!("T{}", track + 1);
            let plus = div()
                .w(px(12.0))
                .text_xs()
                .text_color(rgb(0x8fb0c7))
                .child("+")
                .on_mouse_down(
                    MouseButton::Left,
                    cx.listener(move |view, _, _, cx| {
                        view.adjust_columns(track, 1, cx);
                    }),
                );
            let minus = div()
                .w(px(12.0))
                .text_xs()
                .text_color(rgb(0x8fb0c7))
                .child("-")
                .on_mouse_down(
                    MouseButton::Left,
                    cx.listener(move |view, _, _, cx| {
                        view.adjust_columns(track, -1, cx);
                    }),
                );
            let header_cell = div()
                .w(px(COLUMN_WIDTH * columns as f32))
                .h_full()
                .flex()
                .items_center()
                .justify_between()
                .px_2()
                .border_l_1()
                .border_color(rgb(0x3a4555))
                .child(
                    div()
                        .text_sm()
                        .font_weight(FontWeight::MEDIUM)
                        .text_color(rgb(0xa0aab4))
                        .child(track_label),
                )
                .child(div().flex().items_center().gap_1().child(plus).child(minus));
            header = header.child(header_cell);
        }
        header
    }

    pub(crate) fn render_tracker_grid(&mut self, cx: &mut Context<Self>) -> impl IntoElement {
        let header = self.render_tracker_header(cx);
        let minimap = self.render_minimap(cx);
        let cache = self.tracker_cache();
        let cache_ref = cache.as_deref();
        let pending_overlay = if cache_ref.is_some() {
            self.pending_overlay()
        } else {
            None
        };

        // Simple inline rendering - uniform_list overhead not worth it for 32 items
        let mut rows = div().flex().flex_col().gap_0();
        for row_index in 0..VISIBLE_ROWS {
            rows = rows.child(self.render_tracker_row(
                row_index,
                cache_ref,
                pending_overlay.as_ref(),
                cx,
            ));
        }

        let grid = div()
            .flex()
            .flex_col()
            .gap_0()
            .on_scroll_wheel(cx.listener(|view, event, _, cx| {
                view.handle_scroll_wheel(event, cx);
            }))
            .child(header)
            .child(rows);

        div()
            .flex()
            .gap_0()
            .h(px(HEADER_HEIGHT + ROW_HEIGHT * VISIBLE_ROWS as f32))
            .child(minimap)
            .child(grid)
    }
}
