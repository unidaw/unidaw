use gpui::{div, px, rgb, Context, FontWeight, IntoElement, MouseButton};
use gpui::prelude::*;

use crate::app::{
    ChainAddMode, ChainDevice, EngineView, TRACK_COUNT, PATCHER_NODE_AUDIO_PASSTHROUGH,
    PATCHER_NODE_EUCLIDEAN, PATCHER_NODE_EVENT_OUT, PATCHER_NODE_LFO,
    PATCHER_NODE_PASSTHROUGH, PATCHER_NODE_RANDOM_DEGREE, PATCHER_NODE_RUST,
};

fn device_kind_label(kind: u32) -> &'static str {
    match kind {
        0 => "Patcher Event",
        1 => "Patcher Instrument",
        2 => "Patcher Audio",
        3 => "VST Instrument",
        4 => "VST Effect",
        _ => "Device",
    }
}

fn plugin_name_for_slot<'a>(view: &'a EngineView, slot: u32) -> Option<&'a str> {
    view.plugins
        .iter()
        .find(|plugin| plugin.index as u32 == slot)
        .map(|plugin| plugin.name.as_str())
}

fn patcher_node_type_label(node_type: u32) -> &'static str {
    match node_type {
        PATCHER_NODE_RUST => "Rust",
        PATCHER_NODE_EUCLIDEAN => "Euclidean",
        PATCHER_NODE_PASSTHROUGH => "Passthrough",
        PATCHER_NODE_AUDIO_PASSTHROUGH => "Audio Pass",
        PATCHER_NODE_LFO => "LFO",
        PATCHER_NODE_RANDOM_DEGREE => "Random Degree",
        PATCHER_NODE_EVENT_OUT => "Event Out",
        _ => "Node",
    }
}

fn device_subtitle(view: &EngineView, track: usize, device: &ChainDevice) -> String {
    if device.kind <= 2 {
        let node_label = view
            .patcher_nodes
            .get(track)
            .and_then(|nodes| nodes.iter().find(|node| node.id == device.patcher_node_id))
            .map(|node| patcher_node_type_label(node.node_type));
        if let Some(label) = node_label {
            format!("Node {} · {}", device.patcher_node_id, label)
        } else {
            format!("Node {} (missing)", device.patcher_node_id)
        }
    } else {
        device_kind_label(device.kind).to_string()
    }
}

impl EngineView {
    pub(crate) fn render_device_chain_strip(&self, _cx: &mut Context<Self>) -> impl IntoElement {
        let track = self.focused_track_index.min(TRACK_COUNT.saturating_sub(1));
        let track_name = self
            .track_names
            .get(track)
            .and_then(|name| name.clone())
            .unwrap_or_else(|| format!("Track {}", track + 1));
        let track_label = format!("Track {}: {}", track + 1, track_name);

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
                    .child("DEVICE CHAIN")
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
                    .child("+ Add")
                    .on_mouse_down(
                        MouseButton::Left,
                        _cx.listener(|view, _, _, cx| {
                            view.toggle_chain_add_menu(cx);
                        }),
                    ),
            );

        let mut row = div()
            .flex()
            .items_center()
            .gap_2()
            .bg(rgb(0x0f1218))
            .border_1()
            .border_color(rgb(0x1f2b35))
            .rounded(px(4.0))
            .px_2()
            .py(px(10.0));

        let mut devices = self
            .chain_devices
            .get(track)
            .cloned()
            .unwrap_or_default();
        devices.sort_by_key(|device| device.position);
        if devices.is_empty() {
            row = row.child(
                div()
                    .w(px(140.0))
                    .h(px(44.0))
                    .flex()
                    .flex_col()
                    .justify_center()
                    .gap_1()
                    .bg(rgb(0x151b23))
                    .border_1()
                    .border_color(rgb(0x2a3242))
                    .rounded(px(4.0))
                    .px_2()
                    .child(
                        div()
                            .text_xs()
                            .text_color(rgb(0x6e7883))
                            .child("Empty"),
                    ),
            );
        } else {
            for device in devices.iter() {
                let title = if device.kind == 3 || device.kind == 4 {
                    plugin_name_for_slot(self, device.host_slot_index)
                        .unwrap_or_else(|| device_kind_label(device.kind))
                        .to_string()
                } else {
                    device_kind_label(device.kind).to_string()
                };
                let subtitle = device_subtitle(self, track, device);
                let muted = if device.bypass { rgb(0x6e7883) } else { rgb(0xe6eef5) };
                let power_color = if device.bypass {
                    rgb(0xf36b6b)
                } else {
                    rgb(0x4ad66d)
                };
                let power_label = "●";
                let device_id = device.id;
                let is_vst = device.kind == 3 || device.kind == 4;
                let is_selected = self
                    .focused_chain_device_id
                    .map_or(false, |selected| selected == device_id) && self.chain_focus;
                let border = if is_selected { rgb(0x6ca1ff) } else { rgb(0x2a3242) };
                row = row.child(
                    div()
                        .w(px(320.0))
                        .h(px(108.0))
                        .flex()
                        .flex_col()
                        .justify_center()
                        .gap_3()
                        .bg(rgb(0x151b23))
                        .border_1()
                        .border_color(border)
                        .rounded(px(4.0))
                        .px_2()
                        .py(px(8.0))
                        .on_mouse_down(
                            MouseButton::Left,
                            _cx.listener(move |view, _, _, cx| {
                                view.select_chain_device(device_id, cx);
                            }),
                        )
                        .child(
                            div()
                                .text_sm()
                                .text_color(muted)
                                .child(title),
                        )
                        .child(
                            div()
                                .text_xs()
                                .text_color(rgb(0x6e7883))
                                .child(subtitle),
                        )
                        .child(
                            div()
                                .flex()
                                .items_center()
                                .gap_2()
                                .child(
                                    div()
                                        .text_xs()
                                        .text_color(power_color)
                                        .border_1()
                                        .border_color(rgb(0x2a3242))
                                        .rounded(px(3.0))
                                        .px_2()
                                        .py(px(3.0))
                                        .child(power_label)
                                        .on_mouse_down(
                                            MouseButton::Left,
                                            _cx.listener(move |view, _, _, cx| {
                                                view.select_chain_device(device_id, cx);
                                                view.toggle_chain_device_bypass(device_id, cx);
                                            }),
                                        ),
                                )
                                .child(
                                    if is_vst {
                                        div()
                                            .text_xs()
                                            .text_color(rgb(0x93a1ad))
                                            .border_1()
                                            .border_color(rgb(0x2a3242))
                                            .rounded(px(3.0))
                                            .px_1()
                                            .py(px(1.0))
                                            .child("Edit")
                                            .on_mouse_down(
                                                MouseButton::Left,
                                                _cx.listener(move |view, _, _, cx| {
                                                    view.select_chain_device(device_id, cx);
                                                    view.open_vst_editor(device_id, cx);
                                                }),
                                            )
                                    } else {
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
                                                    .py(px(1.0))
                                                    .child("Open")
                                                    .on_mouse_down(
                                                        MouseButton::Left,
                                                        _cx.listener(move |view, _, _, cx| {
                                                            view.select_chain_device(
                                                                device_id, cx,
                                                            );
                                                            view.open_patcher_view(cx);
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
                                                    .py(px(1.0))
                                                    .child(format!("Node {}", device.patcher_node_id))
                                                    .on_mouse_down(
                                                        MouseButton::Left,
                                                        _cx.listener(move |view, _, _, cx| {
                                                            view.select_chain_device(
                                                                device_id, cx,
                                                            );
                                                            view.cycle_chain_device_patcher_node(
                                                                device_id,
                                                                true,
                                                                cx,
                                                            );
                                                        }),
                                                    )
                                                    .on_mouse_down(
                                                        MouseButton::Right,
                                                        _cx.listener(move |view, _, _, cx| {
                                                            view.select_chain_device(
                                                                device_id, cx,
                                                            );
                                                            view.cycle_chain_device_patcher_node(
                                                                device_id,
                                                                false,
                                                                cx,
                                                            );
                                                        }),
                                                    ),
                                            )
                                    },
                                ),
                        ),
                );
            }
        }

        let add_menu = if self.chain_add_open {
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
                        .child("Patcher Event")
                        .on_mouse_down(
                            MouseButton::Left,
                            _cx.listener(|view, _, _, cx| {
                                view.add_chain_patcher_device(0, cx);
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
                        .child("VST Instrument")
                        .on_mouse_down(
                            MouseButton::Left,
                            _cx.listener(|view, _, _, cx| {
                                view.open_plugin_palette_for_chain(ChainAddMode::Instrument, cx);
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
                        .child("VST Effect")
                        .on_mouse_down(
                            MouseButton::Left,
                            _cx.listener(|view, _, _, cx| {
                                view.open_plugin_palette_for_chain(ChainAddMode::Effect, cx);
                            }),
                        ),
                )
        } else {
            div()
        };

        div()
            .flex()
            .flex_col()
            .gap_2()
            .bg(rgb(0x0b0f14))
            .border_t_1()
            .border_color(rgb(0x253240))
            .px_2()
            .py(px(8.0))
            .child(header)
            .child(add_menu)
            .child(row)
    }
}
