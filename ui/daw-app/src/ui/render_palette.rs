use gpui::{div, rgb, Context, IntoElement, MouseButton};
use gpui::prelude::*;

use crate::app::EngineView;
use crate::palette::{PaletteMode, PALETTE_COMMANDS};

impl EngineView {
    pub(crate) fn filtered_command_indices(&self) -> Vec<usize> {
        if self.palette_query.is_empty() {
            return (0..PALETTE_COMMANDS.len()).collect();
        }
        let query = self.palette_query.to_lowercase();
        PALETTE_COMMANDS
            .iter()
            .enumerate()
            .filter_map(|(index, command)| {
                if command.label.to_lowercase().contains(&query) {
                    Some(index)
                } else {
                    None
                }
            })
            .collect()
    }

    pub(crate) fn filtered_plugin_indices(&self) -> Vec<usize> {
        if self.palette_query.is_empty() {
            return (0..self.plugins.len()).collect();
        }
        let query = self.palette_query.to_lowercase();
        self.plugins
            .iter()
            .enumerate()
            .filter_map(|(index, plugin)| {
                let haystack = format!("{} {}", plugin.name, plugin.vendor).to_lowercase();
                if haystack.contains(&query) {
                    Some(index)
                } else {
                    None
                }
            })
            .collect()
    }

    pub(crate) fn render_palette(&mut self, cx: &mut Context<Self>) -> impl IntoElement {
        if !self.palette_open {
            return div();
        }
        let filtered = match self.palette_mode {
            PaletteMode::Commands => self.filtered_command_indices(),
            PaletteMode::Plugins => self.filtered_plugin_indices(),
        };
        if self.palette_mode == PaletteMode::Plugins &&
            filtered.is_empty() &&
            !self.palette_empty_logged {
            eprintln!(
                "daw-app: plugin palette empty (plugins={}, query={:?})",
                self.plugins.len(),
                self.palette_query
            );
            self.palette_empty_logged = true;
        }
        let selection = self.palette_selection.min(filtered.len().saturating_sub(1));
        let header_label = match self.palette_mode {
            PaletteMode::Commands => "Cmd+P  Command Palette",
            PaletteMode::Plugins => "Cmd+P  Load Plugin on Track…",
        };

        div()
            .absolute()
            .top_6()
            .left_6()
            .right_6()
            .bg(rgb(0x131a21))
            .border_1()
            .border_color(rgb(0x2b3945))
            .p_3()
            .child(
                div()
                    .text_sm()
                    .text_color(rgb(0x93a1ad))
                    .child(format!(
                        "{}  {}  ({} results)",
                        header_label,
                        if self.palette_query.is_empty() {
                            "".to_string()
                        } else {
                            format!("{}", self.palette_query)
                        },
                        filtered.len()
                    )),
            )
            .children(filtered.iter().enumerate().map(|(row, index)| {
                let is_selected = row == selection;
                let bg = if is_selected { rgb(0x1f2b35) } else { rgb(0x131a21) };
                match self.palette_mode {
                    PaletteMode::Commands => {
                        let command = PALETTE_COMMANDS[*index];
                        div()
                            .mt_2()
                            .p_2()
                            .bg(bg)
                            .border_1()
                            .border_color(rgb(0x253240))
                            .on_mouse_down(
                                MouseButton::Left,
                                cx.listener(move |view, _, _, cx| {
                                    view.palette_selection = row;
                                    view.confirm_palette(cx);
                                }),
                            )
                            .child(format!("{}  [{}]", command.label, command.hint))
                    }
                    PaletteMode::Plugins => {
                        let plugin = &self.plugins[*index];
                        let kind = if plugin.is_instrument { "INST" } else { "FX" };
                        div()
                            .mt_2()
                            .p_2()
                            .bg(bg)
                            .border_1()
                            .border_color(rgb(0x253240))
                            .on_mouse_down(
                                MouseButton::Left,
                                cx.listener(move |view, _, _, cx| {
                                    view.palette_selection = row;
                                    view.confirm_palette(cx);
                                }),
                            )
                            .child(format!(
                                "{} — {} ({})",
                                plugin.name,
                                if plugin.vendor.is_empty() {
                                    "Unknown"
                                } else {
                                    &plugin.vendor
                                },
                                kind
                            ))
                    }
                }
            }))
    }
}
