use gpui::{
    div, px, rgb, Context, FontWeight, IntoElement, MouseButton, MouseDownEvent, MouseMoveEvent,
};
use gpui::prelude::*;

use crate::app::{
    chord_token_text, format_playhead, pitch_to_note, EngineView, DEFAULT_ZOOM_INDEX,
    NANOTICKS_PER_QUARTER, TRACK_COUNT, ZOOM_LEVELS,
};
use crate::harmony::{harmony_root_name, harmony_scale_name};
use crate::state::{AggregateCell, AggregateSingle, ClipChord, HarmonyAggregate};
use crate::tracker::{
    COLUMN_WIDTH, HEADER_HEIGHT, HARMONY_COLUMN_WIDTH, ROW_HEIGHT, TIME_COLUMN_WIDTH,
    VISIBLE_ROWS,
};

impl EngineView {
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

    pub(crate) fn render_tracker_grid(&self, cx: &mut Context<Self>) -> impl IntoElement {
        let row_nanoticks = self.row_nanoticks() as i64;
        let aggregate_rows = self.should_aggregate_rows();
        let playhead_view_row = if row_nanoticks > 0 {
            let row = (self.snapshot.ui_global_nanotick_playhead as i64 -
                self.scroll_nanotick_offset) / row_nanoticks;
            if row >= 0 && row < VISIBLE_ROWS as i64 {
                Some(row)
            } else {
                None
            }
        } else {
            None
        };
        let mut rows = Vec::with_capacity(VISIBLE_ROWS);

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
                    .child("TIME")
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

        let cursor_row = self.cursor_view_row();
        let scroll_row_base = if row_nanoticks > 0 {
            self.scroll_nanotick_offset.max(0) / row_nanoticks
        } else {
            0
        };

        let selection_bg = rgb(0x233145);
        for row_index in 0..VISIBLE_ROWS {
            let row_index = row_index as i64;
            let absolute_row = row_index + scroll_row_base;
            let nanotick = self.view_row_nanotick(row_index);
            let row_end = if row_nanoticks > 0 {
                nanotick.saturating_add(row_nanoticks as u64)
            } else {
                nanotick
            };
            let row_label = format_playhead(nanotick);

            let row_bg = if absolute_row % 4 == 0 {
                rgb(0x0a0d12)
            } else if absolute_row % 2 == 0 {
                rgb(0x0f1218)
            } else {
                rgb(0x12151c)
            };

            let mut row = div()
                .flex()
                .gap_0()
                .items_center()
                .bg(row_bg)
                .h(px(ROW_HEIGHT))
                .child(
                    div()
                        .w(px(TIME_COLUMN_WIDTH))
                        .h_full()
                        .flex()
                        .items_center()
                        .text_xs()
                        .px_2()
                        .text_color(if absolute_row % 4 == 0 {
                            rgb(0xb0bac4)
                        } else {
                            rgb(0x707a84)
                        })
                        .font_weight(if absolute_row % 4 == 0 {
                            FontWeight::MEDIUM
                        } else {
                            FontWeight::NORMAL
                        })
                        .child(row_label)
                );

            let harmony_aggregate = if aggregate_rows {
                Some(self.aggregate_harmony_in_range(nanotick, row_end))
            } else {
                None
            };
            let harmony_label = if let Some(aggregate) = &harmony_aggregate {
                if aggregate.count == 0 {
                    None
                } else if aggregate.count == 1 {
                    Some(aggregate.labels[0].clone())
                } else if aggregate.count <= 3 {
                    Some(format!("[{}: {}]", aggregate.count, aggregate.labels.join("->")))
                } else {
                    Some(format!("[{} scales]", aggregate.count))
                }
            } else {
                self.harmony_label_at_nanotick(nanotick)
            };
            let has_harmony = harmony_label.is_some();
            let harmony_bg = if self.selection_contains_harmony(nanotick) {
                selection_bg
            } else if self.harmony_focus && row_index == cursor_row {
                rgb(0x1a3045)
            } else if self.harmony_focus {
                rgb(0x162535)
            } else if has_harmony {
                rgb(0x141820)
            } else {
                rgb(0x0f1218)
            };
            let harmony_text = harmony_label.unwrap_or_else(|| "·····".to_string());
            let density_ratio = if let Some(aggregate) = &harmony_aggregate {
                if aggregate.count > 1 {
                    (aggregate.count.min(8) as f32) / 8.0
                } else {
                    0.0
                }
            } else {
                0.0
            };
            let density_width =
                (HARMONY_COLUMN_WIDTH - 10.0).max(0.0) * density_ratio;
            row = row.child(
                div()
                    .w(px(HARMONY_COLUMN_WIDTH))
                    .h(px(ROW_HEIGHT))
                    .flex()
                    .items_center()
                    .text_xs()
                    .text_color(if has_harmony {
                        rgb(0x7fa0c0)
                    } else {
                        rgb(0x404550)
                    })
                    .bg(harmony_bg)
                    .relative()
                    .border_l_1()
                    .border_r_2()
                    .border_color(rgb(0x2a3242))
                    .px_1()
                    .on_mouse_down(
                        MouseButton::Left,
                        cx.listener(move |view, event: &MouseDownEvent, _, cx| {
                            view.focus_harmony_row(row_index as usize, cx);
                            let nanotick = view.view_row_nanotick(row_index as i64);
                            view.start_selection(
                                nanotick,
                                None,
                                None,
                                true,
                                event.modifiers.shift,
                                cx,
                            );
                        }),
                    )
                    .on_mouse_move(cx.listener(move |view, event: &MouseMoveEvent, _, cx| {
                        if event.dragging() {
                            let nanotick = view.view_row_nanotick(row_index as i64);
                            view.update_selection_end(nanotick, cx);
                            }
                        }))
                    .child(if density_width > 0.0 {
                        div()
                            .absolute()
                            .bottom(px(3.0))
                            .left(px(4.0))
                            .h(px(2.0))
                            .w(px(density_width))
                            .bg(rgb(0x2f3b4b))
                    } else {
                        div()
                    })
                    .child(harmony_text),
            );

            for track in 0..TRACK_COUNT {
                let is_focus = track == self.focused_track_index;
                let is_cursor_row = row_index == cursor_row && is_focus && !self.harmony_focus;
                let is_playhead = playhead_view_row == Some(row_index);
                let nanotick = self.view_row_nanotick(row_index);
                let columns = self.track_columns[track];
                let aggregates = if aggregate_rows {
                    Some(self.aggregate_cells_in_range(nanotick, row_end, track, columns))
                } else {
                    None
                };
                let entries = if aggregate_rows {
                    Vec::new()
                } else {
                    self.entries_for_row(nanotick, track)
                };
                for col in 0..columns {
                    let is_cursor = is_cursor_row && col == self.cursor_col;
                    let aggregate = aggregates
                        .as_ref()
                        .and_then(|list| list.get(col));
                    let has_content = aggregate.map_or_else(
                        || entries.iter().any(|entry| entry.column == col),
                        |agg| agg.count > 0,
                    );
                    let bg = if self.selection_contains_cell(nanotick, track, col) {
                        selection_bg
                    } else if is_cursor && is_playhead {
                        rgb(0x2b3b4b)
                    } else if is_cursor {
                        rgb(0x1f2b35)
                    } else if is_playhead {
                        rgb(0x1a242e)
                    } else if has_content {
                        rgb(0x141820)
                    } else {
                        rgb(0x0f161c)
                    };
                    let entry = entries.iter().find(|entry| entry.column == col);
                    let is_note_off = aggregate
                        .map(|agg| agg.note_off_only && agg.count == 1)
                        .unwrap_or_else(|| entry.map(|entry| entry.note_off).unwrap_or(false));
                    let cell_text = if is_cursor && self.edit_active {
                        self.edit_text.clone()
                    } else if is_note_off {
                        String::new()
                    } else {
                        if let Some(aggregate) = aggregate {
                            self.aggregate_cell_label(aggregate)
                                .unwrap_or_else(|| if is_cursor {
                                    "···".to_string()
                                } else {
                                    "·  ".to_string()
                                })
                        } else {
                            entry
                                .map(|entry| entry.text.clone())
                                .unwrap_or_else(|| {
                                    if is_cursor { "···".to_string() } else { "·  ".to_string() }
                                })
                        }
                    };

                    let mut cell = div()
                        .w(px(COLUMN_WIDTH))
                        .h(px(ROW_HEIGHT))
                        .flex()
                        .items_center()
                        .text_xs()
                        .bg(bg)
                        .px_1();

                    if col == 0 && track > 0 {
                        cell = cell.border_l_1().border_color(rgb(0x2a3242));
                    }

                    let content = if is_note_off && !(is_cursor && self.edit_active) {
                        div()
                            .w_full()
                            .h_full()
                            .flex()
                            .items_center()
                            .justify_center()
                            .child(
                                div()
                                    .w(px(7.0))
                                    .h(px(7.0))
                                    .bg(rgb(0xa0aab4)),
                            )
                    } else if aggregate_rows &&
                        aggregate.map_or(false, |agg| agg.count > 1) &&
                        !(is_cursor && self.edit_active) {
                        div()
                            .w_full()
                            .h_full()
                            .flex()
                            .items_center()
                            .justify_center()
                            .child(
                                div()
                                    .px_1()
                                    .py(px(1.0))
                                    .rounded(px(3.0))
                                    .bg(rgb(0x2a3242))
                                    .text_color(rgb(0xa0aab4))
                                    .text_xs()
                                    .child(cell_text),
                            )
                    } else {
                        div()
                            .text_color(if has_content {
                                rgb(0xa0aab4)
                            } else {
                                rgb(0x404550)
                            })
                            .child(cell_text)
                    };

                    row = row.child(
                        cell.on_mouse_down(
                            MouseButton::Left,
                            cx.listener(move |view, event: &MouseDownEvent, _, cx| {
                                view.focus_note_cell(
                                    row_index as usize,
                                    track,
                                    col,
                                    cx,
                                );
                                let nanotick = view.view_row_nanotick(row_index as i64);
                                view.start_selection(
                                    nanotick,
                                    Some(track),
                                    Some(col),
                                    false,
                                    event.modifiers.shift,
                                    cx,
                                );
                            }),
                        )
                        .on_mouse_move(cx.listener(move |view, event: &MouseMoveEvent, _, cx| {
                            if event.dragging() {
                                let nanotick = view.view_row_nanotick(row_index as i64);
                                view.update_selection_end(nanotick, cx);
                            }
                        }))
                        .child(content),
                    );
                }
            }
            rows.push(row);
        }

        let grid = div()
            .flex()
            .flex_col()
            .gap_0()
            .on_scroll_wheel(cx.listener(|view, event, _, cx| {
                view.handle_scroll_wheel(event, cx);
            }))
            .child(header)
            .children(rows);

        div()
            .flex()
            .gap_0()
            .h(px(HEADER_HEIGHT + ROW_HEIGHT * VISIBLE_ROWS as f32))
            .child(self.render_minimap(cx))
            .child(grid)
    }
}
