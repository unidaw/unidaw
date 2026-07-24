use std::collections::HashMap;
use std::sync::Arc;

use gpui::{
    canvas, div, fill, px, rgb, App, Bounds, Context, FontWeight, IntoElement,
    MouseButton, MouseDownEvent, Pixels, Point, SharedString, ShapedLine,
    Window,
};
use gpui::prelude::*;

use crate::app::{
    chord_token_text, note_label_shared, off_label_shared, pitch_to_note, EngineView,
    FastCell, FastOverlayGrid, DEFAULT_ZOOM_INDEX, NANOTICKS_PER_QUARTER, TRACK_COUNT,
    ZOOM_LEVELS, TrackerCache, TrackerCacheKey, TrackerRowCache, TrackerTextCache,
};
use crate::harmony::{harmony_root_name, harmony_scale_name};
use crate::state::{AggregateCell, AggregateSingle, ClipChord, HarmonyEntry};
#[cfg(test)]
use crate::state::HarmonyAggregate;
use crate::tracker::{
    COLUMN_WIDTH, HEADER_HEIGHT, HARMONY_COLUMN_WIDTH, ROW_HEIGHT, TIME_COLUMN_WIDTH,
    TRACKER_FONT_SIZE_PX, TRACKER_LINE_HEIGHT_PX, VISIBLE_ROWS,
};

const CELL_TEXT_PADDING: f32 = 4.0;
const TIME_TEXT_PADDING: f32 = 6.0;
const HARMONY_TEXT_PADDING: f32 = 6.0;

struct TrackerCanvasSnapshot {
    cache: Arc<TrackerCache>,
    cursor_row: i64,
    cursor_col: usize,
    focused_track_index: usize,
    harmony_focus: bool,
    playhead_pos: u64,
    fast_overlay: FastOverlayGrid,
}

fn tracker_grid_width(track_columns: &[usize]) -> f32 {
    let tracks_width: f32 = track_columns
        .iter()
        .map(|columns| *columns as f32 * COLUMN_WIDTH)
        .sum();
    TIME_COLUMN_WIDTH + HARMONY_COLUMN_WIDTH + tracks_width
}

fn shaped_line_for_label(
    cache: &mut HashMap<SharedString, ShapedLine>,
    label: &SharedString,
    color: gpui::Hsla,
    font_size: Pixels,
    window: &mut Window,
) -> ShapedLine {
    if let Some(line) = cache.get(label) {
        return line.clone();
    }
    let mut style = window.text_style();
    style.color = color;
    let run = style.to_run(label.len());
    let line = window
        .text_system()
        .shape_line(label.clone(), font_size, &[run], None);
    cache.insert(label.clone(), line.clone());
    line
}

fn paint_tracker_canvas(
    bounds: Bounds<Pixels>,
    snapshot: &TrackerCanvasSnapshot,
    text_cache: &mut TrackerTextCache,
    window: &mut Window,
    cx: &mut App,
) {
    let origin_x = f32::from(bounds.origin.x);
    let origin_y = f32::from(bounds.origin.y);
    let row_height = ROW_HEIGHT;
    let font_size = px(TRACKER_FONT_SIZE_PX);
    let line_height = px(TRACKER_LINE_HEIGHT_PX);
    let line_height_f = f32::from(line_height);
    let text_y_offset = ((row_height - line_height_f) * 0.5).max(0.0);
    let track_columns = &snapshot.cache.key.track_columns;
    let total_width = tracker_grid_width(track_columns);
    let body_height = row_height * VISIBLE_ROWS as f32;

    let time_color: gpui::Hsla = rgb(0x6a7a8a).into();
    let harmony_color: gpui::Hsla = rgb(0x7fa0c0).into();
    let cell_color: gpui::Hsla = rgb(0xc0d0e0).into();
    let border_color = rgb(0x2a3545);
    let harmony_border_color = rgb(0x3a4555);

    let cursor_row = snapshot.cursor_row;
    let cursor_col = snapshot.cursor_col;
    let focused_track = snapshot.focused_track_index;
    let playhead_pos = snapshot.playhead_pos;

    for row_index in 0..VISIBLE_ROWS {
        let row_top = origin_y + row_index as f32 * row_height;
        let row_cache = snapshot.cache.rows.get(row_index);
        let overlay_row = snapshot.fast_overlay.get(row_index);
        let row_start = row_cache
            .map(|row| row.row_start)
            .unwrap_or_else(|| snapshot.cache.key.window_start + row_index as u64 * snapshot.cache.key.row_nanoticks);
        let row_end = row_start.saturating_add(snapshot.cache.key.row_nanoticks);

        let is_cursor_row = cursor_row >= 0 && cursor_row as usize == row_index;
        let is_playhead_row = playhead_pos >= row_start && playhead_pos < row_end;
        let row_bg = if is_playhead_row {
            rgb(0x1a2535)
        } else if is_cursor_row {
            rgb(0x1a2228)
        } else if row_index % 4 == 0 {
            rgb(0x15191f)
        } else {
            rgb(0x12161b)
        };

        let row_bounds = Bounds::new(
            Point::new(px(origin_x), px(row_top)),
            gpui::size(px(total_width), px(row_height)),
        );
        window.paint_quad(fill(row_bounds, row_bg));

        let harmony_bg = if is_cursor_row && snapshot.harmony_focus {
            rgb(0x3a4a5a)
        } else {
            rgb(0x151922)
        };
        let harmony_bounds = Bounds::new(
            Point::new(px(origin_x + TIME_COLUMN_WIDTH), px(row_top)),
            gpui::size(px(HARMONY_COLUMN_WIDTH), px(row_height)),
        );
        window.paint_quad(fill(harmony_bounds, harmony_bg));

        if is_cursor_row && !snapshot.harmony_focus {
            let mut cursor_x = origin_x + TIME_COLUMN_WIDTH + HARMONY_COLUMN_WIDTH;
            for (track_index, columns) in track_columns.iter().enumerate() {
                if track_index == focused_track {
                    let col = cursor_col.min(columns.saturating_sub(1));
                    cursor_x += col as f32 * COLUMN_WIDTH;
                    let cell_bounds = Bounds::new(
                        Point::new(px(cursor_x), px(row_top)),
                        gpui::size(px(COLUMN_WIDTH), px(row_height)),
                    );
                    window.paint_quad(fill(cell_bounds, rgb(0x3a4a5a)));
                    break;
                }
                cursor_x += *columns as f32 * COLUMN_WIDTH;
            }
        }

        if let Some(row_cache) = row_cache {
            let time_origin = Point::new(
                px(origin_x + TIME_TEXT_PADDING),
                px(row_top + text_y_offset),
            );
            let time_line = shaped_line_for_label(
                &mut text_cache.time,
                &row_cache.time_label,
                time_color,
                font_size,
                window,
            );
            let _ = time_line.paint(time_origin, line_height, window, cx);

            if let Some(harmony_label) = row_cache.harmony_label.as_ref() {
                let harmony_origin = Point::new(
                    px(origin_x + TIME_COLUMN_WIDTH + HARMONY_TEXT_PADDING),
                    px(row_top + text_y_offset),
                );
                let harmony_line = shaped_line_for_label(
                    &mut text_cache.harmony,
                    harmony_label,
                    harmony_color,
                    font_size,
                    window,
                );
                let _ = harmony_line.paint(harmony_origin, line_height, window, cx);
            }

            let mut cell_x = origin_x + TIME_COLUMN_WIDTH + HARMONY_COLUMN_WIDTH;
            for (track_index, columns) in track_columns.iter().enumerate() {
                let overlay_track = overlay_row.and_then(|row| row.get(track_index));
                if let Some(cells) = row_cache.cell_labels.get(track_index) {
                    let column_count = (*columns).min(cells.len());
                    for col_idx in 0..column_count {
                        let overlay_cell = overlay_track
                            .and_then(|track| track.get(col_idx))
                            .and_then(|cell| cell.as_ref());
                        let mut drew_overlay = false;
                        if let Some(FastCell::Label(label)) = overlay_cell {
                            let cell_origin = Point::new(
                                px(cell_x + CELL_TEXT_PADDING),
                                px(row_top + text_y_offset),
                            );
                            let line = shaped_line_for_label(
                                &mut text_cache.cell,
                                label,
                                cell_color,
                                font_size,
                                window,
                            );
                            let _ = line.paint(cell_origin, line_height, window, cx);
                            drew_overlay = true;
                        }
                        if !drew_overlay {
                            if let Some(label) = cells.get(col_idx).and_then(|label| label.as_ref()) {
                                let cell_origin = Point::new(
                                    px(cell_x + CELL_TEXT_PADDING),
                                    px(row_top + text_y_offset),
                                );
                                let line = shaped_line_for_label(
                                    &mut text_cache.cell,
                                    label,
                                    cell_color,
                                    font_size,
                                    window,
                                );
                                let _ = line.paint(cell_origin, line_height, window, cx);
                            }
                        }
                        cell_x += COLUMN_WIDTH;
                    }
                    if column_count < *columns {
                        cell_x += (*columns - column_count) as f32 * COLUMN_WIDTH;
                    }
                } else {
                    cell_x += *columns as f32 * COLUMN_WIDTH;
                }
            }
        }
    }

    let mut line_x = origin_x + TIME_COLUMN_WIDTH;
    let grid_height = body_height;
    let line_bounds = |x: f32| Bounds::new(
        Point::new(px(x), px(origin_y)),
        gpui::size(px(1.0), px(grid_height)),
    );
    window.paint_quad(fill(line_bounds(line_x), harmony_border_color));
    line_x += HARMONY_COLUMN_WIDTH;
    window.paint_quad(fill(line_bounds(line_x), harmony_border_color));

    for columns in track_columns.iter() {
        for _ in 0..*columns {
            line_x += COLUMN_WIDTH;
            window.paint_quad(fill(line_bounds(line_x), border_color));
        }
    }
}

fn time_label_for_nanotick(nanotick: u64) -> SharedString {
    let total_beats = nanotick / crate::app::NANOTICKS_PER_QUARTER;
    let bar = total_beats / crate::app::BEATS_PER_BAR + 1;
    let beat = total_beats % crate::app::BEATS_PER_BAR + 1;
    let tick = (nanotick % crate::app::NANOTICKS_PER_QUARTER) / 10_000;
    SharedString::from(format!("{}:{}:{}", bar, beat, tick))
}

fn set_row_cell_label(
    row: &mut TrackerRowCache,
    track_index: usize,
    column: usize,
    label: SharedString,
) {
    let Some(track_cells) = row.cell_labels.get_mut(track_index) else {
        return;
    };
    if column >= track_cells.len() {
        return;
    }
    track_cells[column] = Some(label);
}

fn harmony_label_for_events(events: &[HarmonyEntry], nanotick: u64) -> Option<SharedString> {
    let event = events.iter().find(|event| event.nanotick == nanotick)?;
    let root = harmony_root_name(event.root);
    let scale = harmony_scale_name(event.scale_id);
    Some(SharedString::from(format!("{root}:{scale}")))
}

fn aggregate_harmony_label(
    events: &[HarmonyEntry],
    start: u64,
    end: u64,
) -> Option<SharedString> {
    let mut count = 0usize;
    let mut label: Option<String> = None;
    for event in events.iter() {
        if event.nanotick < start || event.nanotick >= end {
            continue;
        }
        count += 1;
        if count == 1 {
            label = Some(format!(
                "{}:{}",
                harmony_root_name(event.root),
                harmony_scale_name(event.scale_id)
            ));
        }
    }
    match count {
        0 => None,
        1 => label.map(SharedString::from),
        _ => Some(SharedString::from(format!("[{}]", count))),
    }
}

fn aggregate_cell_label_shared(aggregate: &AggregateCell) -> Option<SharedString> {
    if aggregate.count == 0 {
        return None;
    }
    if aggregate.note_off_only && aggregate.count > 1 {
        return Some(SharedString::from(format!("[OFFx {}]", aggregate.count)));
    }
    if let Some(single) = &aggregate.single {
        match single {
            AggregateSingle::Note { pitch, note_off } => {
                if *note_off {
                    return None;
                }
                return Some(SharedString::from(pitch_to_note(*pitch)));
            }
            AggregateSingle::Chord(chord) => {
                return Some(SharedString::from(chord_token_text(chord)));
            }
        }
    }
    if aggregate.notes_only {
        if let Some(pitch) = aggregate.unique_pitch {
            return Some(SharedString::from(format!(
                "[{}x {}]",
                aggregate.count,
                pitch_to_note(pitch)
            )));
        }
    }
    Some(SharedString::from(format!("[{}]", aggregate.count)))
}

fn build_tracker_row_cache(
    row_index: usize,
    key: &TrackerCacheKey,
    store: &crate::clip_store::ClipStore,
    pending_overlay: &crate::clip_store::PendingOverlay,
    harmony_events: &[HarmonyEntry],
) -> TrackerRowCache {
    let row_nanoticks = key.row_nanoticks.max(1);
    let row_start = key.window_start + row_index as u64 * row_nanoticks;
    let row_end = row_start.saturating_add(row_nanoticks);
    let base_row = NANOTICKS_PER_QUARTER / ZOOM_LEVELS[DEFAULT_ZOOM_INDEX];
    let should_aggregate = row_nanoticks > base_row;
    let time_label = time_label_for_nanotick(row_start);
    let harmony_label = if should_aggregate {
        aggregate_harmony_label(harmony_events, row_start, row_end)
    } else {
        harmony_label_for_events(harmony_events, row_start)
    };
    let mut cell_labels = Vec::with_capacity(TRACK_COUNT);
    for track in 0..TRACK_COUNT {
        let columns = key.track_columns.get(track).copied().unwrap_or(0);
        cell_labels.push(vec![None; columns]);
    }
    let mut row = TrackerRowCache {
        row_start,
        time_label,
        harmony_label,
        cell_labels,
    };

    for (track_index, track) in store.tracks().iter().enumerate() {
        let columns = key.track_columns.get(track_index).copied().unwrap_or(0);
        if should_aggregate {
            let mut aggregates = vec![AggregateCell::new(); columns];
            for (column, map) in track.notes().iter().enumerate() {
                if column >= columns {
                    break;
                }
                for note in map.range(row_start..row_end).map(|(_, note)| note) {
                    let is_note_off = note.velocity == 0 && note.duration == 0;
                    aggregates[column].add_note(note.pitch, is_note_off);
                }
            }
            for (column, map) in track.chords().iter().enumerate() {
                if column >= columns {
                    break;
                }
                for chord in map.range(row_start..row_end).map(|(_, chord)| chord) {
                    aggregates[column].add_chord(chord.clone());
                }
            }
            if let Some(track_notes) = pending_overlay.notes_for_track(track_index) {
                for (column, map) in track_notes.iter().enumerate() {
                    if column >= columns {
                        break;
                    }
                    for note in map.range(row_start..row_end).map(|(_, note)| note) {
                        let is_note_off = note.velocity == 0 && note.duration == 0;
                        aggregates[column].add_note(note.pitch, is_note_off);
                    }
                }
            }
            if let Some(track_chords) = pending_overlay.chords_for_track(track_index) {
                for (column, map) in track_chords.iter().enumerate() {
                    if column >= columns {
                        break;
                    }
                    for chord in map.range(row_start..row_end).map(|(_, chord)| chord) {
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
                }
            }
            for (column, aggregate) in aggregates.iter().enumerate() {
                if let Some(label) = aggregate_cell_label_shared(aggregate) {
                    set_row_cell_label(&mut row, track_index, column, label);
                }
            }
        } else {
            for (column, map) in track.notes().iter().enumerate() {
                if column >= columns {
                    break;
                }
                for note in map.range(row_start..row_end).map(|(_, note)| note) {
                    let label = if note.velocity == 0 && note.duration == 0 {
                        off_label_shared()
                    } else {
                        note_label_shared(note.pitch)
                    };
                    set_row_cell_label(&mut row, track_index, column, label);
                }
            }
            for (column, map) in track.chords().iter().enumerate() {
                if column >= columns {
                    break;
                }
                for chord in map.range(row_start..row_end).map(|(_, chord)| chord) {
                    set_row_cell_label(
                        &mut row,
                        track_index,
                        column,
                        SharedString::from(chord_token_text(chord)),
                    );
                }
            }
            if let Some(track_notes) = pending_overlay.notes_for_track(track_index) {
                for (column, map) in track_notes.iter().enumerate() {
                    if column >= columns {
                        break;
                    }
                    for note in map.range(row_start..row_end).map(|(_, note)| note) {
                        let label = if note.velocity == 0 && note.duration == 0 {
                            off_label_shared()
                        } else {
                            note_label_shared(note.pitch)
                        };
                        set_row_cell_label(&mut row, track_index, column, label);
                    }
                }
            }
            if let Some(track_chords) = pending_overlay.chords_for_track(track_index) {
                for (column, map) in track_chords.iter().enumerate() {
                    if column >= columns {
                        break;
                    }
                    for chord in map.range(row_start..row_end).map(|(_, chord)| chord) {
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
                        set_row_cell_label(
                            &mut row,
                            track_index,
                            column,
                            SharedString::from(chord_token_text(&temp)),
                        );
                    }
                }
            }
        }
    }

    row
}

impl EngineView {
    fn tracker_cache_key(&self) -> TrackerCacheKey {
        TrackerCacheKey {
            window_start: self.scroll_nanotick_offset.max(0) as u64,
            row_nanoticks: self.row_nanoticks(),
            track_columns: self.track_columns.clone(),
        }
    }

    fn build_tracker_cache(
        &self,
        key: TrackerCacheKey,
        store: &crate::clip_store::ClipStore,
    ) -> TrackerCache {
        let pending_overlay = &self.pending_overlay;
        let harmony_events = &self.harmony_events;
        let rows = (0..VISIBLE_ROWS)
            .map(|row_index| {
                build_tracker_row_cache(
                    row_index,
                    &key,
                    store,
                    pending_overlay,
                    harmony_events,
                )
            })
            .collect();
        TrackerCache { key, rows }
    }

    fn tracker_cache(&mut self) -> Option<Arc<TrackerCache>> {
        let key = self.tracker_cache_key();
        let store = self.clip_store.read().ok()?;
        let needs_rebuild = self
            .tracker_cache
            .as_ref()
            .map_or(true, |cache| cache.key != key);
        if needs_rebuild || self.tracker_cache_dirty_all {
            self.tracker_cache = Some(Arc::new(self.build_tracker_cache(key, &store)));
            self.tracker_cache_dirty_rows.clear();
            self.tracker_cache_dirty_all = false;
        } else if !self.tracker_cache_dirty_rows.is_empty() {
            let dirty_rows: Vec<usize> = self.tracker_cache_dirty_rows.drain(..).collect();
            let row_updates = {
                let pending_overlay = &self.pending_overlay;
                let harmony_events = &self.harmony_events;
                dirty_rows
                    .iter()
                    .map(|&row_index| {
                        (
                            row_index,
                            build_tracker_row_cache(
                                row_index,
                                &key,
                                &store,
                                pending_overlay,
                                harmony_events,
                            ),
                        )
                    })
                    .collect::<Vec<_>>()
            };
            if let Some(cache) = self.tracker_cache.as_mut() {
                let cache = Arc::make_mut(cache);
                for (row_index, row_cache) in row_updates {
                    if row_index < cache.rows.len() {
                        cache.rows[row_index] = row_cache;
                    }
                }
            }
        }
        self.tracker_cache.clone()
    }

    /// Renders the tracker viewport as text, from the same row cache and with
    /// the same fast-overlay precedence the painter uses.
    ///
    /// Going through `tracker_cache` on purpose: it applies the incremental
    /// dirty-row logic, so a stale or wrongly-invalidated row shows up here
    /// exactly as it would on screen. A cell taken from the optimistic overlay
    /// is suffixed with `*`, so a snapshot distinguishes an edit the engine has
    /// confirmed from one still in flight.
    pub fn render_tracker_text(&mut self) -> String {
        let cursor_row = self.cursor_view_row();
        let cursor_track = self.focused_track_index;
        let cursor_col = self.cursor_col;
        let playhead = self.snapshot.ui_global_nanotick_playhead;
        let Some(cache) = self.tracker_cache() else {
            return "tracker: clip store unavailable\n".to_string();
        };
        let overlay = self.fast_overlay_grid(&cache.key);
        let key = &cache.key;

        let mut out = String::new();
        out.push_str(&format!(
            "window_start={} row_nanoticks={} track_columns={:?}\n",
            key.window_start, key.row_nanoticks, key.track_columns
        ));
        out.push_str(&format!(
            "cursor=t{}c{}r{} playhead_nanotick={}\n",
            cursor_track, cursor_col, cursor_row, playhead
        ));

        let mut header = format!("{:<3}|{:>9} |", "row", "time");
        for (track, &columns) in key.track_columns.iter().enumerate() {
            if columns == 0 {
                continue;
            }
            for column in 0..columns {
                header.push_str(&format!(" {:<6}", format!("t{track}c{column}")));
            }
            header.push_str(" |");
        }
        header.push_str(" harm");
        out.push_str(&header);
        out.push('\n');
        out.push_str(&"-".repeat(header.len()));
        out.push('\n');

        for row_index in 0..VISIBLE_ROWS {
            let Some(row) = cache.rows.get(row_index) else {
                continue;
            };
            let row_end = row.row_start.saturating_add(key.row_nanoticks.max(1));
            let mut marker = if cursor_row >= 0 && cursor_row as usize == row_index {
                '>'
            } else {
                ' '
            };
            if playhead >= row.row_start && playhead < row_end {
                marker = if marker == '>' { '#' } else { '*' };
            }
            out.push_str(&format!(
                "{:02}{}|{:>9} |",
                row_index,
                marker,
                row.time_label.as_ref()
            ));

            let overlay_row = overlay.get(row_index);
            for (track, &columns) in key.track_columns.iter().enumerate() {
                if columns == 0 {
                    continue;
                }
                for column in 0..columns {
                    // Overlay first, matching the paint order.
                    let overlay_cell = overlay_row
                        .and_then(|tracks| tracks.get(track))
                        .and_then(|cells| cells.get(column))
                        .and_then(|cell| cell.as_ref());
                    let text = if let Some(FastCell::Label(label)) = overlay_cell {
                        format!("{label}*")
                    } else {
                        row.cell_labels
                            .get(track)
                            .and_then(|cells| cells.get(column))
                            .and_then(|cell| cell.as_ref())
                            .map(|label| label.to_string())
                            .unwrap_or_else(|| ".".to_string())
                    };
                    out.push_str(&format!(" {text:<6}"));
                }
                out.push_str(" |");
            }
            match row.harmony_label.as_ref() {
                Some(label) => out.push_str(&format!(" {}", label.as_ref())),
                None => out.push_str(" ."),
            }
            out.push('\n');
        }
        out
    }

    fn row_index_for_nanotick(&self, nanotick: u64) -> Option<usize> {
        let row_nanoticks = self.row_nanoticks().max(1);
        let visible_start = self.scroll_nanotick_offset.max(0) as u64;
        let visible_end = visible_start.saturating_add(
            row_nanoticks.saturating_mul(VISIBLE_ROWS as u64),
        );
        if nanotick < visible_start || nanotick >= visible_end {
            return None;
        }
        // Containment, not alignment: the row cache selects cells with
        // `range(row_start..row_end)`, so invalidation must use the same
        // rule or an edit inside an unaligned row is never repainted.
        let row_index = ((nanotick - visible_start) / row_nanoticks) as usize;
        if row_index < VISIBLE_ROWS {
            Some(row_index)
        } else {
            None
        }
    }

    fn mark_tracker_row_dirty(&mut self, row_index: usize) {
        if self.tracker_cache_dirty_all || row_index >= VISIBLE_ROWS {
            return;
        }
        if !self.tracker_cache_dirty_rows.contains(&row_index) {
            self.tracker_cache_dirty_rows.push(row_index);
        }
    }

    pub(crate) fn mark_tracker_row_dirty_for_nanotick(&mut self, nanotick: u64) {
        if let Some(row_index) = self.row_index_for_nanotick(nanotick) {
            self.mark_tracker_row_dirty(row_index);
        }
    }

    pub(crate) fn mark_tracker_rows_dirty_in_range(&mut self, start: u64, end: u64) {
        if start > end {
            return;
        }
        let row_nanoticks = self.row_nanoticks().max(1);
        let visible_start = self.scroll_nanotick_offset.max(0) as u64;
        for row_index in 0..VISIBLE_ROWS {
            let row_start = visible_start + row_index as u64 * row_nanoticks;
            if row_start >= start && row_start <= end {
                self.mark_tracker_row_dirty(row_index);
            }
        }
    }

    pub(crate) fn mark_tracker_cache_dirty_all(&mut self) {
        self.tracker_cache_dirty_all = true;
        self.tracker_cache_dirty_rows.clear();
    }

    #[cfg(test)]
    pub(crate) fn should_aggregate_rows(&self) -> bool {
        let base_row = NANOTICKS_PER_QUARTER / ZOOM_LEVELS[DEFAULT_ZOOM_INDEX];
        self.row_nanoticks() > base_row
    }

    #[cfg(test)]
    pub(crate) fn aggregate_cells_in_range(
        &self,
        start: u64,
        end: u64,
        track_index: usize,
        columns: usize,
    ) -> Vec<AggregateCell> {
        let mut aggregates = vec![AggregateCell::new(); columns];
        if let Ok(store) = self.clip_store.read() {
            if let Some(track) = store.track(track_index) {
                for (column, map) in track.notes().iter().enumerate() {
                    if column >= columns {
                        break;
                    }
                    for note in map.range(start..end).map(|(_, note)| note) {
                        let is_note_off = note.velocity == 0 && note.duration == 0;
                        aggregates[column].add_note(note.pitch, is_note_off);
                    }
                }
                for (column, map) in track.chords().iter().enumerate() {
                    if column >= columns {
                        break;
                    }
                    for chord in map.range(start..end).map(|(_, chord)| chord) {
                        aggregates[column].add_chord(chord.clone());
                    }
                }
            }
        }
        if let Some(track_notes) = self.pending_overlay.notes_for_track(track_index) {
            for (column, map) in track_notes.iter().enumerate() {
                if column >= columns {
                    break;
                }
                for note in map.range(start..end).map(|(_, note)| note) {
                    let is_note_off = note.velocity == 0 && note.duration == 0;
                    aggregates[column].add_note(note.pitch, is_note_off);
                }
            }
        }
        if let Some(track_chords) = self.pending_overlay.chords_for_track(track_index) {
            for (column, map) in track_chords.iter().enumerate() {
                if column >= columns {
                    break;
                }
                for chord in map.range(start..end).map(|(_, chord)| chord) {
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
            }
        }
        aggregates
    }

    #[cfg(test)]
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

    #[cfg(test)]
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

    pub(crate) fn handle_tracker_click(
        &mut self,
        event: &MouseDownEvent,
        cx: &mut impl crate::app::UiNotify,
    ) {
        let Some(origin) = self.tracker_canvas_origin else {
            return;
        };
        let local_x = f32::from(event.position.x) - origin.0;
        let local_y = f32::from(event.position.y) - origin.1;
        if local_y < 0.0 || local_y >= ROW_HEIGHT * VISIBLE_ROWS as f32 {
            return;
        }
        if local_x < TIME_COLUMN_WIDTH {
            return;
        }
        let row_index = (local_y / ROW_HEIGHT).floor() as usize;
        let mut x = local_x - TIME_COLUMN_WIDTH;
        if x < HARMONY_COLUMN_WIDTH {
            self.focus_harmony_row(row_index, cx);
            return;
        }
        x -= HARMONY_COLUMN_WIDTH;
        for track_index in 0..TRACK_COUNT {
            let columns = self.track_columns[track_index];
            let track_width = columns as f32 * COLUMN_WIDTH;
            if x < track_width {
                let col = (x / COLUMN_WIDTH).floor() as usize;
                self.focus_note_cell(row_index, track_index, col, cx);
                return;
            }
            x -= track_width;
        }
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
        let Some(cache) = self.tracker_cache() else {
            return div();
        };
        let grid_width = tracker_grid_width(&cache.key.track_columns);
        let body_height = ROW_HEIGHT * VISIBLE_ROWS as f32;
        let snapshot = TrackerCanvasSnapshot {
            cache: cache.clone(),
            cursor_row: self.cursor_view_row(),
            cursor_col: self.cursor_col,
            focused_track_index: self.focused_track_index,
            harmony_focus: self.harmony_focus,
            playhead_pos: self.snapshot.ui_global_nanotick_playhead,
            fast_overlay: self.fast_overlay_grid(&cache.key),
        };
        let view_handle = cx.weak_entity();
        let text_cache = self.tracker_text_cache.clone();
        let body = div()
            .w(px(grid_width))
            .h(px(body_height))
            .relative()
            .on_mouse_down(
                MouseButton::Left,
                cx.listener(|view, event: &MouseDownEvent, _, cx| {
                    view.handle_tracker_click(event, cx);
                }),
            )
            .child(
                canvas(
                    move |bounds, _window, cx| {
                        let origin = (f32::from(bounds.origin.x), f32::from(bounds.origin.y));
                        let _ = view_handle.update(cx, |view, cx| {
                            if view.tracker_canvas_origin != Some(origin) {
                                view.tracker_canvas_origin = Some(origin);
                                cx.notify();
                            }
                        });
                        ()
                    },
                    move |bounds, _state, window, cx| {
                        if let Ok(mut cache) = text_cache.lock() {
                            paint_tracker_canvas(bounds, &snapshot, &mut *cache, window, cx);
                        }
                    },
                )
                .w(px(grid_width))
                .h(px(body_height))
                .text_xs(),
            );

        let grid = div()
            .flex()
            .flex_col()
            .gap_0()
            .on_scroll_wheel(cx.listener(|view, event, _, cx| {
                view.handle_scroll_wheel(event, cx);
            }))
            .child(header)
            .child(body);

        div()
            .flex()
            .gap_0()
            .h(px(HEADER_HEIGHT + ROW_HEIGHT * VISIBLE_ROWS as f32))
            .child(minimap)
            .child(grid)
    }
}
