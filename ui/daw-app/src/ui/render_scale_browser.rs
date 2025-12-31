use gpui::{div, rgb, Context, IntoElement, MouseButton};
use gpui::prelude::*;

use crate::app::{format_playhead, EngineView};
use crate::harmony::{harmony_root_name, harmony_scale_name, SCALE_LIBRARY};

impl EngineView {
    pub(crate) fn filtered_scale_indices(&self) -> Vec<usize> {
        if self.scale_browser_query.is_empty() {
            return (0..SCALE_LIBRARY.len()).collect();
        }
        let query = self.scale_browser_query.to_lowercase();
        SCALE_LIBRARY
            .iter()
            .enumerate()
            .filter_map(|(index, scale)| {
                if scale.name.to_lowercase().contains(&query) ||
                    scale.key.to_lowercase().contains(&query) {
                    Some(index)
                } else {
                    None
                }
            })
            .collect()
    }

    pub(crate) fn render_scale_browser(&self, cx: &mut Context<Self>) -> impl IntoElement {
        if !self.scale_browser_open {
            return div();
        }
        let nanotick = self.current_row_nanotick();
        let row_time = format_playhead(nanotick);
        let root = harmony_root_name(self.harmony_root_at(nanotick));
        let current_scale_id = self.harmony_scale_at(nanotick);
        let current_scale_name = harmony_scale_name(current_scale_id);
        let filtered = self.filtered_scale_indices();
        let selection = self
            .scale_browser_selection
            .min(filtered.len().saturating_sub(1));

        div()
            .absolute()
            .top_20()
            .left_10()
            .right_10()
            .bg(rgb(0x10161c))
            .border_1()
            .border_color(rgb(0x2b3945))
            .p_3()
            .child(
                div()
                    .text_sm()
                    .text_color(rgb(0x93a1ad))
                    .child(format!(
                        "Scale Browser @ {}  [{}:{}]  {}  ({} results)",
                        row_time,
                        root,
                        current_scale_name,
                        if self.scale_browser_query.is_empty() {
                            "".to_string()
                        } else {
                            format!("{}", self.scale_browser_query)
                        },
                        filtered.len()
                    )),
            )
            .child(
                div()
                    .mt_1()
                    .text_xs()
                    .text_color(rgb(0x6f818f))
                    .child("Enter: set scale  Esc: close  Backspace: edit"),
            )
            .children(filtered.iter().enumerate().map(|(row, index)| {
                let scale = SCALE_LIBRARY[*index];
                let is_selected = row == selection;
                let is_active = scale.id == current_scale_id;
                let bg = if is_selected { rgb(0x1f2b35) } else { rgb(0x10161c) };
                div()
                    .mt_2()
                    .p_2()
                    .bg(bg)
                    .border_1()
                    .border_color(rgb(0x253240))
                    .on_mouse_down(
                        MouseButton::Left,
                        cx.listener(move |view, _, _, cx| {
                            view.scale_browser_selection = row;
                            view.confirm_scale_browser(cx);
                        }),
                    )
                    .child(format!(
                        "{}{}  (key {})",
                        if is_active { "*" } else { " " },
                        scale.name,
                        scale.key
                    ))
            }))
    }
}
