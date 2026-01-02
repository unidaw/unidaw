use gpui::{div, px, rgb, Context, FontWeight, IntoElement, MouseButton};
use gpui::prelude::*;

use crate::app::{EngineView, PATCHER_NODE_EUCLIDEAN, PATCHER_NODE_EVENT_OUT, PATCHER_NODE_RANDOM_DEGREE};

fn node_type_label(node_type: u32) -> &'static str {
    match node_type {
        0 => "Rust",
        1 => "Euclidean",
        2 => "Passthrough",
        3 => "Audio Pass",
        4 => "LFO",
        5 => "Random Degree",
        6 => "Event Out",
        _ => "Node",
    }
}

impl EngineView {
    pub(crate) fn render_patcher_view(&self, _cx: &mut Context<Self>) -> impl IntoElement {
        let track = self.patcher_track_id as usize;
        let track_label = self
            .track_names
            .get(track)
            .and_then(|name| name.clone())
            .unwrap_or_else(|| format!("Track {}", track + 1));
        let nodes = self
            .patcher_nodes
            .get(track)
            .cloned()
            .unwrap_or_default();

        let header = div()
            .flex()
            .items_center()
            .justify_between()
            .text_sm()
            .font_weight(FontWeight::SEMIBOLD)
            .text_color(rgb(0xb0bac4))
            .child(
                div()
                    .flex()
                    .items_center()
                    .gap_2()
                    .child("PATCHER")
                    .child(
                        div()
                            .text_xs()
                            .text_color(rgb(0x93a1ad))
                            .child(track_label),
                    ),
            )
            .child(
                div()
                    .text_xs()
                    .text_color(rgb(0x93a1ad))
                    .border_1()
                    .border_color(rgb(0x2a3242))
                    .rounded(px(3.0))
                    .px_1()
                    .child("Back")
                    .on_mouse_down(
                        MouseButton::Left,
                        _cx.listener(|view, _, _, cx| {
                            view.close_patcher_view(cx);
                        }),
                    ),
            );

        let controls = div()
            .flex()
            .items_center()
            .gap_2()
            .child(
                div()
                    .text_xs()
                    .text_color(rgb(0x93a1ad))
                    .border_1()
                    .border_color(rgb(0x2a3242))
                    .rounded(px(3.0))
                    .px_1()
                    .child("Add Euclidean")
                    .on_mouse_down(
                        MouseButton::Left,
                        _cx.listener(|view, _, _, cx| {
                            view.add_patcher_node(PATCHER_NODE_EUCLIDEAN, cx);
                        }),
                    ),
            )
            .child(
                div()
                    .text_xs()
                    .text_color(rgb(0x93a1ad))
                    .border_1()
                    .border_color(rgb(0x2a3242))
                    .rounded(px(3.0))
                    .px_1()
                    .child("Add Random Degree")
                    .on_mouse_down(
                        MouseButton::Left,
                        _cx.listener(|view, _, _, cx| {
                            view.add_patcher_node(PATCHER_NODE_RANDOM_DEGREE, cx);
                        }),
                    ),
            )
            .child(
                div()
                    .text_xs()
                    .text_color(rgb(0x93a1ad))
                    .border_1()
                    .border_color(rgb(0x2a3242))
                    .rounded(px(3.0))
                    .px_1()
                    .child("Add Event Out")
                    .on_mouse_down(
                        MouseButton::Left,
                        _cx.listener(|view, _, _, cx| {
                            view.add_patcher_node(PATCHER_NODE_EVENT_OUT, cx);
                        }),
                    ),
            )
            .child(
                div()
                    .text_xs()
                    .text_color(rgb(0x93a1ad))
                    .border_1()
                    .border_color(rgb(0x2a3242))
                    .rounded(px(3.0))
                    .px_1()
                    .child("Connect")
                    .on_mouse_down(
                        MouseButton::Left,
                        _cx.listener(|view, _, _, cx| {
                            view.connect_patcher_nodes(cx);
                        }),
                    ),
            )
            .child(
                div()
                    .text_xs()
                    .text_color(rgb(0x93a1ad))
                    .border_1()
                    .border_color(rgb(0x2a3242))
                    .rounded(px(3.0))
                    .px_1()
                    .child("Save Preset")
                    .on_mouse_down(
                        MouseButton::Left,
                        _cx.listener(|view, _, _, cx| {
                            view.open_preset_save(cx);
                        }),
                    ),
            );

        let mut list = div()
            .flex()
            .flex_col()
            .gap_2()
            .bg(rgb(0x0f1218))
            .border_1()
            .border_color(rgb(0x1f2b35))
            .rounded(px(4.0))
            .px_2()
            .py(px(10.0));

        if nodes.is_empty() {
            list = list.child(
                div()
                    .text_xs()
                    .text_color(rgb(0x6e7883))
                    .child("No nodes. Add Euclidean, Random Degree, Event Out."),
            );
        } else {
            for node in nodes.iter() {
                let source_active = self.patcher_link_source == Some(node.id);
                let target_active = self.patcher_link_target == Some(node.id);
                let border = if source_active || target_active {
                    rgb(0x6ca1ff)
                } else {
                    rgb(0x2a3242)
                };
                let inputs = if node.inputs.is_empty() {
                    "Inputs: -".to_string()
                } else {
                    format!("Inputs: {:?}", node.inputs)
                };
                let node_id = node.id;
                list = list.child(
                    div()
                        .flex()
                        .items_center()
                        .justify_between()
                        .gap_2()
                        .bg(rgb(0x151b23))
                        .border_1()
                        .border_color(border)
                        .rounded(px(4.0))
                        .px_2()
                        .py(px(6.0))
                        .child(
                            div()
                                .flex()
                                .flex_col()
                                .gap_1()
                                .child(
                                    div()
                                        .text_sm()
                                        .text_color(rgb(0xe6eef5))
                                        .child(format!("#{} {}", node.id, node_type_label(node.node_type))),
                                )
                                .child(
                                    div()
                                        .text_xs()
                                        .text_color(rgb(0x6e7883))
                                        .child(inputs),
                                ),
                        )
                        .child(
                            div()
                                .flex()
                                .items_center()
                                .gap_2()
                                .child(
                                    div()
                                        .text_xs()
                                        .text_color(rgb(0x93a1ad))
                                        .border_1()
                                        .border_color(rgb(0x2a3242))
                                        .rounded(px(3.0))
                                        .px_1()
                                        .child("S")
                                        .on_mouse_down(
                                            MouseButton::Left,
                                            _cx.listener(move |view, _, _, cx| {
                                                view.patcher_link_source = Some(node_id);
                                                cx.notify();
                                            }),
                                        ),
                                )
                                .child(
                                    div()
                                        .text_xs()
                                        .text_color(rgb(0x93a1ad))
                                        .border_1()
                                        .border_color(rgb(0x2a3242))
                                        .rounded(px(3.0))
                                        .px_1()
                                        .child("T")
                                        .on_mouse_down(
                                            MouseButton::Left,
                                            _cx.listener(move |view, _, _, cx| {
                                                view.patcher_link_target = Some(node_id);
                                                cx.notify();
                                            }),
                                        ),
                                )
                                .child(
                                    div()
                                        .text_xs()
                                        .text_color(rgb(0x93a1ad))
                                        .border_1()
                                        .border_color(rgb(0x2a3242))
                                        .rounded(px(3.0))
                                        .px_1()
                                        .child("Del")
                                        .on_mouse_down(
                                            MouseButton::Left,
                                            _cx.listener(move |view, _, _, cx| {
                                                view.remove_patcher_node(node_id, cx);
                                            }),
                                        ),
                                ),
                        ),
                );
            }
        }

        div()
            .flex()
            .flex_col()
            .gap_2()
            .bg(rgb(0x0b0f14))
            .border_1()
            .border_color(rgb(0x253240))
            .rounded(px(6.0))
            .px_2()
            .py(px(8.0))
            .child(header)
            .child(controls)
            .child(list)
    }
}
