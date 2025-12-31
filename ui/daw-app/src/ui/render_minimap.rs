use gpui::{div, px, rgb, Context, IntoElement, MouseButton, MouseMoveEvent};
use gpui::prelude::*;

use crate::app::EngineView;
use crate::tracker::{HEADER_HEIGHT, MINIMAP_WIDTH, ROW_HEIGHT, VISIBLE_ROWS};

impl EngineView {
    pub(crate) fn timeline_end_nanotick(&self) -> u64 {
        let mut max_tick = 0_u64;
        for track_notes in &self.clip_notes {
            for note in track_notes {
                max_tick = max_tick.max(note.nanotick);
            }
        }
        for note in &self.pending_notes {
            max_tick = max_tick.max(note.nanotick);
        }
        for track_chords in &self.clip_chords {
            for chord in track_chords {
                max_tick = max_tick.max(chord.nanotick);
            }
        }
        for chord in &self.pending_chords {
            max_tick = max_tick.max(chord.nanotick);
        }
        for event in &self.harmony_events {
            max_tick = max_tick.max(event.nanotick);
        }
        let row = self.row_nanoticks().max(1);
        if max_tick == 0 {
            row.saturating_mul(VISIBLE_ROWS as u64)
        } else {
            max_tick.saturating_add(row)
        }
    }

    pub(crate) fn minimap_bins(&self, start: u64, end: u64, segments: usize) -> Vec<usize> {
        let segments = segments.max(1);
        let mut bins = vec![0usize; segments];
        let span = end.saturating_sub(start).max(1);
        let mut add_tick = |tick: u64| {
            if tick < start || tick >= end {
                return;
            }
            let index = ((tick - start) as u128 * segments as u128 / span as u128) as usize;
            let index = index.min(segments - 1);
            bins[index] += 1;
        };
        for track_notes in &self.clip_notes {
            for note in track_notes {
                add_tick(note.nanotick);
            }
        }
        for note in &self.pending_notes {
            add_tick(note.nanotick);
        }
        for track_chords in &self.clip_chords {
            for chord in track_chords {
                add_tick(chord.nanotick);
            }
        }
        for chord in &self.pending_chords {
            add_tick(chord.nanotick);
        }
        for event in &self.harmony_events {
            add_tick(event.nanotick);
        }
        bins
    }

    pub(crate) fn render_minimap(&self, cx: &mut Context<Self>) -> impl IntoElement {
        let row_nanoticks = self.row_nanoticks() as i64;
        let timeline_end = self.timeline_end_nanotick();
        let timeline_start = 0_u64;
        let body_height = ROW_HEIGHT * VISIBLE_ROWS as f32;
        let segment_count = body_height.ceil().max(1.0) as usize;
        let bins = self.minimap_bins(timeline_start, timeline_end, segment_count);
        let segment_height = body_height / bins.len().max(1) as f32;
        let view_start = self.scroll_nanotick_offset.max(0) as u64;
        let view_len = row_nanoticks.max(1) as u64 * VISIBLE_ROWS as u64;
        let span = timeline_end.saturating_sub(timeline_start).max(1);
        let to_y = |tick: u64| {
            ((tick.saturating_sub(timeline_start) as f32) / span as f32) * body_height
        };
        let view_y = (view_start.saturating_sub(timeline_start) as f32 / span as f32)
            * body_height;
        let mut view_h = (view_len as f32 / span as f32) * body_height;
        if view_h < 6.0 {
            view_h = 6.0;
        }

        let mut segments = Vec::with_capacity(bins.len());
        for (index, count) in bins.iter().enumerate() {
            let color = if *count == 0 {
                rgb(0x1a1f2b)
            } else if *count <= 2 {
                rgb(0x2a3242)
            } else {
                rgb(0x3b4b5d)
            };
            let seg_start = timeline_start +
                ((span as u128 * index as u128) / bins.len() as u128) as u64;
            segments.push(
                div()
                    .w(px(MINIMAP_WIDTH))
                    .h(px(segment_height))
                    .bg(color)
                    .on_mouse_down(
                        MouseButton::Left,
                        cx.listener(move |view, _, _, cx| {
                            view.jump_to_nanotick(seg_start, cx);
                        }),
                    )
                    .on_mouse_move(cx.listener(move |view, event: &MouseMoveEvent, _, cx| {
                        if event.dragging() {
                            view.jump_to_nanotick(seg_start, cx);
                        }
                    })),
            );
        }

        let selection_marker = if let Some((start, end)) = self.selection_bounds() {
            let end_tick = end.saturating_add(row_nanoticks.max(1) as u64);
            let mut y0 = to_y(start);
            let mut y1 = to_y(end_tick.min(timeline_end));
            y0 = y0.clamp(0.0, body_height);
            y1 = y1.clamp(0.0, body_height);
            if y1 < y0 {
                std::mem::swap(&mut y0, &mut y1);
            }
            let mut height = (y1 - y0).max(2.0);
            if y0 + height > body_height {
                height = (body_height - y0).max(2.0);
                if y0 + height > body_height {
                    y0 = (body_height - height).max(0.0);
                }
            }
            div()
                .absolute()
                .left(px(1.0))
                .top(px(y0))
                .w(px(MINIMAP_WIDTH - 2.0))
                .h(px(height))
                .bg(rgb(0x2a3b4d))
        } else {
            div()
        };

        let loop_marker = if let Some((start, end)) = self.loop_range {
            if end > start {
                let mut y0 = to_y(start);
                let mut y1 = to_y(end.min(timeline_end));
                y0 = y0.clamp(0.0, body_height);
                y1 = y1.clamp(0.0, body_height);
                if y1 < y0 {
                    std::mem::swap(&mut y0, &mut y1);
                }
                let mut height = (y1 - y0).max(2.0);
                if y0 + height > body_height {
                    height = (body_height - y0).max(2.0);
                    if y0 + height > body_height {
                        y0 = (body_height - height).max(0.0);
                    }
                }
                div()
                    .absolute()
                    .left(px(1.0))
                    .top(px(y0))
                    .w(px(MINIMAP_WIDTH - 2.0))
                    .h(px(height))
                    .border_1()
                    .border_color(rgb(0x6fb27f))
            } else {
                div()
            }
        } else {
            div()
        };

        div()
            .w(px(MINIMAP_WIDTH))
            .flex()
            .flex_col()
            .child(
                div()
                    .w(px(MINIMAP_WIDTH))
                    .h(px(HEADER_HEIGHT))
                    .bg(rgb(0x1a1f2b)),
            )
            .child(
                div()
                    .w(px(MINIMAP_WIDTH))
                    .h(px(body_height))
                    .relative()
                    .child(div().flex().flex_col().children(segments))
                    .child(selection_marker)
                    .child(loop_marker)
                    .child(
                        div()
                            .absolute()
                            .left(px(1.0))
                            .top(px(view_y))
                            .w(px(MINIMAP_WIDTH - 2.0))
                            .h(px(view_h))
                            .border_1()
                            .border_color(rgb(0x7fa0c0))
                            .bg(rgb(0x233145)),
                    ),
            )
    }
}
