use gpui::{div, px, rgb, Context, IntoElement};
use gpui::prelude::*;

use crate::app::EngineView;
use crate::tracker::TIME_COLUMN_WIDTH;

impl EngineView {
    pub(crate) fn render_jump_overlay(&self, _cx: &mut Context<Self>) -> impl IntoElement {
        if !self.jump_open {
            return div();
        }
        let content = if self.jump_text.is_empty() {
            "Jump: bar[:beat[:tick]]".to_string()
        } else {
            format!("Jump: {}", self.jump_text)
        };
        div()
            .absolute()
            .top(px(6.0))
            .left(px(TIME_COLUMN_WIDTH + 8.0))
            .bg(rgb(0x1b242e))
            .text_color(rgb(0xd6dee6))
            .border_1()
            .border_color(rgb(0x2a3242))
            .px_2()
            .py_1()
            .text_sm()
            .child(content)
    }

    pub(crate) fn render_toast(&self, _cx: &mut Context<Self>) -> impl IntoElement {
        let Some(message) = self.toast_message.as_ref() else {
            return div();
        };
        div()
            .absolute()
            .bottom(px(10.0))
            .left(px(12.0))
            .bg(rgb(0x1b242e))
            .text_color(rgb(0xd6dee6))
            .border_1()
            .border_color(rgb(0x2a3242))
            .px_2()
            .py_1()
            .text_sm()
            .child(message.clone())
    }

    pub(crate) fn render_preset_save_overlay(&self, _cx: &mut Context<Self>) -> impl IntoElement {
        if !self.preset_save_open {
            return div();
        }
        let content = if self.preset_save_text.is_empty() {
            "Preset name... (Enter to save)".to_string()
        } else {
            format!("Preset: {}", self.preset_save_text)
        };
        div()
            .absolute()
            .top(px(36.0))
            .left(px(TIME_COLUMN_WIDTH + 8.0))
            .bg(rgb(0x1b242e))
            .text_color(rgb(0xd6dee6))
            .border_1()
            .border_color(rgb(0x2a3242))
            .px_2()
            .py_1()
            .text_sm()
            .child(content)
    }

    pub(crate) fn render_debug_overlay(&self, _cx: &mut Context<Self>) -> impl IntoElement {
        if !self.ui_debug {
            return div();
        }
        let content = format!(
            "DBG playhead={} transport={} ui_v={}",
            self.snapshot.ui_global_nanotick_playhead,
            self.snapshot.ui_transport_state,
            self.snapshot.version
        );
        div()
            .absolute()
            .bottom(px(10.0))
            .right(px(12.0))
            .bg(rgb(0x1b242e))
            .text_color(rgb(0xd6dee6))
            .border_1()
            .border_color(rgb(0x2a3242))
            .px_2()
            .py_1()
            .text_sm()
            .child(content)
    }
}
