use std::collections::HashMap;

use gpui::{
    canvas, div, px, rgb, Context, FontWeight, IntoElement, MouseButton, MouseDownEvent,
    MouseMoveEvent, MouseUpEvent, PathBuilder, Point,
};
use gpui::prelude::*;

use crate::app::{
    EngineView, PatcherNodeUi, PatcherPortDirection, PatcherPortKind, PatcherPortRef,
    PATCHER_AUDIO_INPUT_PORT, PATCHER_AUDIO_OUTPUT_PORT, PATCHER_CONTROL_INPUT_PORT,
    PATCHER_CONTROL_OUTPUT_PORT, PATCHER_EVENT_INPUT_PORT, PATCHER_EVENT_OUTPUT_PORT,
    PATCHER_NODE_AUDIO_PASSTHROUGH, PATCHER_NODE_EUCLIDEAN, PATCHER_NODE_EVENT_OUT,
    PATCHER_NODE_LFO, PATCHER_NODE_PASSTHROUGH, PATCHER_NODE_RANDOM_DEGREE,
    PATCHER_NODE_RUST, TRACK_COUNT,
};
use crate::tracker::{HEADER_HEIGHT, ROW_HEIGHT, VISIBLE_ROWS};

const NODE_WIDTH: f32 = 180.0;
const NODE_HEIGHT: f32 = 64.0;
const PORT_RADIUS: f32 = 5.0;
const PORT_LABEL_WIDTH: f32 = 80.0;
const PORT_LABEL_HEIGHT: f32 = 14.0;

#[derive(Clone, Copy)]
struct PortSpec {
    id: u32,
    name: &'static str,
    kind: PatcherPortKind,
    control_rate: Option<ControlRate>,
    channel_count: Option<u16>,
}

#[derive(Clone, Copy)]
enum ControlRate {
    Block,
    Sample,
}

fn node_type_label(node_type: u32) -> &'static str {
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

fn node_input_ports(node_type: u32) -> &'static [PortSpec] {
    const RUST_INPUTS: [PortSpec; 2] = [
        PortSpec {
            id: PATCHER_EVENT_INPUT_PORT,
            name: "Event In",
            kind: PatcherPortKind::Event,
            control_rate: None,
            channel_count: None,
        },
        PortSpec {
            id: PATCHER_CONTROL_INPUT_PORT,
            name: "Ctrl In",
            kind: PatcherPortKind::Control,
            control_rate: Some(ControlRate::Block),
            channel_count: None,
        },
    ];
    const PASSTHROUGH_INPUTS: [PortSpec; 1] = [PortSpec {
        id: PATCHER_EVENT_INPUT_PORT,
        name: "In",
        kind: PatcherPortKind::Event,
        control_rate: None,
        channel_count: None,
    }];
    const RANDOM_INPUTS: [PortSpec; 1] = [PortSpec {
        id: PATCHER_EVENT_INPUT_PORT,
        name: "Gate",
        kind: PatcherPortKind::Event,
        control_rate: None,
        channel_count: None,
    }];
    const EVENT_OUT_INPUTS: [PortSpec; 1] = [PortSpec {
        id: PATCHER_EVENT_INPUT_PORT,
        name: "Event",
        kind: PatcherPortKind::Event,
        control_rate: None,
        channel_count: None,
    }];
    const AUDIO_INPUTS: [PortSpec; 1] = [PortSpec {
        id: PATCHER_AUDIO_INPUT_PORT,
        name: "Audio In",
        kind: PatcherPortKind::Audio,
        control_rate: None,
        channel_count: Some(2),
    }];
    const EMPTY: [PortSpec; 0] = [];
    match node_type {
        PATCHER_NODE_RUST => &RUST_INPUTS,
        PATCHER_NODE_AUDIO_PASSTHROUGH => &AUDIO_INPUTS,
        PATCHER_NODE_PASSTHROUGH => &PASSTHROUGH_INPUTS,
        PATCHER_NODE_RANDOM_DEGREE => &RANDOM_INPUTS,
        PATCHER_NODE_EVENT_OUT => &EVENT_OUT_INPUTS,
        PATCHER_NODE_LFO => &EMPTY,
        _ => &EMPTY,
    }
}

fn node_output_ports(node_type: u32) -> &'static [PortSpec] {
    const RUST_OUTPUTS: [PortSpec; 2] = [
        PortSpec {
            id: PATCHER_EVENT_OUTPUT_PORT,
            name: "Event Out",
            kind: PatcherPortKind::Event,
            control_rate: None,
            channel_count: None,
        },
        PortSpec {
            id: PATCHER_CONTROL_OUTPUT_PORT,
            name: "Ctrl Out",
            kind: PatcherPortKind::Control,
            control_rate: Some(ControlRate::Block),
            channel_count: None,
        },
    ];
    const EUCLIDEAN_OUTPUTS: [PortSpec; 1] = [PortSpec {
        id: PATCHER_EVENT_OUTPUT_PORT,
        name: "Gate",
        kind: PatcherPortKind::Event,
        control_rate: None,
        channel_count: None,
    }];
    const PASSTHROUGH_OUTPUTS: [PortSpec; 1] = [PortSpec {
        id: PATCHER_EVENT_OUTPUT_PORT,
        name: "Out",
        kind: PatcherPortKind::Event,
        control_rate: None,
        channel_count: None,
    }];
    const RANDOM_OUTPUTS: [PortSpec; 1] = [PortSpec {
        id: PATCHER_EVENT_OUTPUT_PORT,
        name: "Degree",
        kind: PatcherPortKind::Event,
        control_rate: None,
        channel_count: None,
    }];
    const LFO_CONTROL_OUTPUTS: [PortSpec; 1] = [PortSpec {
        id: PATCHER_CONTROL_OUTPUT_PORT,
        name: "LFO",
        kind: PatcherPortKind::Control,
        control_rate: Some(ControlRate::Sample),
        channel_count: None,
    }];
    const AUDIO_OUTPUTS: [PortSpec; 1] = [PortSpec {
        id: PATCHER_AUDIO_OUTPUT_PORT,
        name: "Audio Out",
        kind: PatcherPortKind::Audio,
        control_rate: None,
        channel_count: Some(2),
    }];
    const EMPTY: [PortSpec; 0] = [];
    match node_type {
        PATCHER_NODE_RUST => &RUST_OUTPUTS,
        PATCHER_NODE_EUCLIDEAN => &EUCLIDEAN_OUTPUTS,
        PATCHER_NODE_PASSTHROUGH => &PASSTHROUGH_OUTPUTS,
        PATCHER_NODE_RANDOM_DEGREE => &RANDOM_OUTPUTS,
        PATCHER_NODE_AUDIO_PASSTHROUGH => &AUDIO_OUTPUTS,
        PATCHER_NODE_LFO => &LFO_CONTROL_OUTPUTS,
        _ => &EMPTY,
    }
}

fn port_offset_y(index: usize, total: usize) -> f32 {
    if total == 0 {
        NODE_HEIGHT * 0.5
    } else {
        NODE_HEIGHT * ((index + 1) as f32 / (total + 1) as f32)
    }
}

fn port_position(
    node: &PatcherNodeUi,
    direction: PatcherPortDirection,
    port_id: u32,
) -> Option<(f32, f32)> {
    let ports = match direction {
        PatcherPortDirection::Input => node_input_ports(node.node_type),
        PatcherPortDirection::Output => node_output_ports(node.node_type),
    };
    let index = ports.iter().position(|port| port.id == port_id)?;
    let y = node.pos_y + port_offset_y(index, ports.len());
    let x = match direction {
        PatcherPortDirection::Input => node.pos_x,
        PatcherPortDirection::Output => node.pos_x + NODE_WIDTH,
    };
    Some((x, y))
}

fn port_color(kind: PatcherPortKind) -> gpui::Rgba {
    match kind {
        PatcherPortKind::Event => rgb(0x5fa4d3),
        PatcherPortKind::Audio => rgb(0xd28b5f),
        PatcherPortKind::Control => rgb(0x6cc28b),
    }
}

fn port_label(port: &PortSpec) -> String {
    let mut label = port.name.to_string();
    if let Some(channels) = port.channel_count {
        label = format!("{label} {channels}ch");
    }
    if let Some(rate) = port.control_rate {
        let suffix = match rate {
            ControlRate::Block => "BR",
            ControlRate::Sample => "SR",
        };
        label = format!("{label} {suffix}");
    }
    label
}

impl EngineView {
    pub(crate) fn render_patcher_view(&self, cx: &mut Context<Self>) -> impl IntoElement {
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
        let edges = self
            .patcher_edges
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
                        cx.listener(|view, _, _, cx| {
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
                        cx.listener(|view, _, _, cx| {
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
                        cx.listener(|view, _, _, cx| {
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
                        cx.listener(|view, _, _, cx| {
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
                        cx.listener(|view, _, _, cx| {
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
                        cx.listener(|view, _, _, cx| {
                            view.open_preset_save(cx);
                        }),
                    ),
            );

        let link_source = self.patcher_link_source;
        let link_target = self.patcher_link_target;
        let mouse_pos = self.patcher_mouse_pos;
        let canvas_nodes = nodes.clone();
        let canvas_edges = edges.clone();
        let view_handle = cx.weak_entity();
        let wires = canvas(
            move |bounds, _window, cx| {
                let origin = (f32::from(bounds.origin.x), f32::from(bounds.origin.y));
                let _ = view_handle.update(cx, |view, cx| {
                    if view.patcher_canvas_origin != Some(origin) {
                        view.patcher_canvas_origin = Some(origin);
                        cx.notify();
                    }
                });
                ()
            },
            move |bounds, _state, window, _cx| {
                let origin_x = f32::from(bounds.origin.x);
                let origin_y = f32::from(bounds.origin.y);
                let mut node_map = HashMap::new();
                for node in canvas_nodes.iter() {
                    node_map.insert(node.id, node);
                }
                for edge in canvas_edges.iter() {
                    let Some(src) = node_map.get(&edge.src_node_id) else { continue };
                    let Some(dst) = node_map.get(&edge.dst_node_id) else { continue };
                    let Some((sx, sy)) = port_position(
                        src,
                        PatcherPortDirection::Output,
                        edge.src_port_id,
                    ) else { continue };
                    let Some((dx, dy)) = port_position(
                        dst,
                        PatcherPortDirection::Input,
                        edge.dst_port_id,
                    ) else { continue };
                    let ctrl = Point::new(
                        px(origin_x + (sx + dx) * 0.5),
                        px(origin_y + sy),
                    );
                    let mut builder = PathBuilder::stroke(px(1.5));
                    builder.move_to(Point::new(px(origin_x + sx), px(origin_y + sy)));
                    builder.curve_to(Point::new(px(origin_x + dx), px(origin_y + dy)), ctrl);
                    if let Ok(path) = builder.build() {
                        window.paint_path(path, port_color(edge.kind));
                    }
                }
                if let Some(src_ref) = link_source {
                    if link_target.is_none() {
                        if let (Some(src), Some(mouse)) =
                            (node_map.get(&src_ref.node_id), mouse_pos) {
                            if let Some((sx, sy)) = port_position(
                                src,
                                PatcherPortDirection::Output,
                                src_ref.port_id,
                            ) {
                                let ctrl = Point::new(
                                    px(origin_x + (sx + mouse.0) * 0.5),
                                    px(origin_y + sy),
                                );
                                let mut builder = PathBuilder::stroke(px(1.5));
                                builder.move_to(Point::new(px(origin_x + sx), px(origin_y + sy)));
                                builder.curve_to(
                                    Point::new(px(origin_x + mouse.0), px(origin_y + mouse.1)),
                                    ctrl,
                                );
                                if let Ok(path) = builder.build() {
                                    window.paint_path(path, rgb(0x6ca1ff));
                                }
                            }
                        }
                    }
                }
            },
        )
        .size_full()
        .bg(rgb(0x0f1218));

        let mut node_views = Vec::new();
        for node in nodes.iter() {
            let node_id = node.id;
            let node_type = node.node_type;
            let pos_x = node.pos_x;
            let pos_y = node.pos_y;
            let is_source = link_source.map(|port| port.node_id) == Some(node_id);
            let is_target = link_target.map(|port| port.node_id) == Some(node_id);
            let border = if is_source || is_target {
                rgb(0x6ca1ff)
            } else {
                rgb(0x2a3242)
            };
            let input_ports = node_input_ports(node_type);
            let output_ports = node_output_ports(node_type);

            let header_row = div()
                .flex()
                .items_center()
                .justify_between()
                .child(
                    div()
                        .text_sm()
                        .text_color(rgb(0xe6eef5))
                        .child(format!("#{} {}", node_id, node_type_label(node_type))),
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
                            cx.listener(move |view, _, _, cx| {
                                view.remove_patcher_node(node_id, cx);
                            }),
                        ),
                );

            let mut node_body = div()
                .relative()
                .w(px(NODE_WIDTH))
                .h(px(NODE_HEIGHT))
                .bg(rgb(0x151b23))
                .border_1()
                .border_color(border)
                .rounded(px(6.0))
                .px_2()
                .py(px(6.0))
                .child(header_row);

            for (index, port) in input_ports.iter().enumerate() {
                let port_id = port.id;
                let port_kind = port.kind;
                let port_y = port_offset_y(index, input_ports.len());
                let port_label = port_label(port);
                node_body = node_body.child(
                    div()
                        .absolute()
                        .left(px(0.0))
                        .top(px(port_y - PORT_RADIUS))
                        .w(px(PORT_RADIUS * 2.0))
                        .h(px(PORT_RADIUS * 2.0))
                        .rounded(px(PORT_RADIUS))
                        .bg(port_color(port_kind))
                        .on_mouse_down(
                            MouseButton::Left,
                            cx.listener(move |view, _, _, cx| {
                                let port_ref = PatcherPortRef {
                                    node_id,
                                    port_id,
                                    kind: port_kind,
                                    direction: PatcherPortDirection::Input,
                                };
                                if view.patcher_link_source.is_some() {
                                    view.patcher_link_target = Some(port_ref);
                                    view.connect_patcher_nodes(cx);
                                } else {
                                    view.patcher_link_target = Some(port_ref);
                                    cx.notify();
                                }
                            }),
                        ),
                );
                node_body = node_body.child(
                    div()
                        .absolute()
                        .left(px(14.0))
                        .top(px(port_y - PORT_LABEL_HEIGHT * 0.5))
                        .w(px(PORT_LABEL_WIDTH))
                        .text_xs()
                        .text_color(rgb(0x93a1ad))
                        .child(port_label),
                );
            }

            for (index, port) in output_ports.iter().enumerate() {
                let port_id = port.id;
                let port_kind = port.kind;
                let port_y = port_offset_y(index, output_ports.len());
                let port_label = port_label(port);
                node_body = node_body.child(
                    div()
                        .absolute()
                        .right(px(0.0))
                        .top(px(port_y - PORT_RADIUS))
                        .w(px(PORT_RADIUS * 2.0))
                        .h(px(PORT_RADIUS * 2.0))
                        .rounded(px(PORT_RADIUS))
                        .bg(port_color(port_kind))
                        .on_mouse_down(
                            MouseButton::Left,
                            cx.listener(move |view, _, _, cx| {
                                view.patcher_link_source = Some(PatcherPortRef {
                                    node_id,
                                    port_id,
                                    kind: port_kind,
                                    direction: PatcherPortDirection::Output,
                                });
                                view.patcher_link_target = None;
                                cx.notify();
                            }),
                        ),
                );
                node_body = node_body.child(
                    div()
                        .absolute()
                        .right(px(14.0))
                        .top(px(port_y - PORT_LABEL_HEIGHT * 0.5))
                        .w(px(PORT_LABEL_WIDTH))
                        .text_xs()
                        .text_color(rgb(0x93a1ad))
                        .child(port_label),
                );
            }

            let node_view = div()
                .absolute()
                .left(px(pos_x))
                .top(px(pos_y))
                .child(node_body)
                .on_mouse_down(
                    MouseButton::Left,
                    cx.listener(move |view, event: &MouseDownEvent, _, cx| {
                        let Some(origin) = view.patcher_canvas_origin else {
                            return;
                        };
                        let local_x = f32::from(event.position.x) - origin.0;
                        let local_y = f32::from(event.position.y) - origin.1;
                        view.patcher_drag_node = Some(node_id);
                        view.patcher_drag_offset = (local_x - pos_x, local_y - pos_y);
                        cx.notify();
                    }),
                );
            node_views.push(node_view);
        }

        let empty_hint = if nodes.is_empty() {
            div()
                .absolute()
                .top(px(10.0))
                .left(px(12.0))
                .text_xs()
                .text_color(rgb(0x6e7883))
                .child("No nodes. Add Euclidean, Random Degree, Event Out.")
        } else {
            div()
        };

        let canvas = div()
            .relative()
            .flex_1()
            .bg(rgb(0x0f1218))
            .border_1()
            .border_color(rgb(0x1f2b35))
            .rounded(px(4.0))
            .child(wires)
            .children(node_views)
            .child(empty_hint)
            .on_mouse_move(cx.listener(move |view, event: &MouseMoveEvent, _, cx| {
                let Some(origin) = view.patcher_canvas_origin else {
                    return;
                };
                let local_x = f32::from(event.position.x) - origin.0;
                let local_y = f32::from(event.position.y) - origin.1;
                view.patcher_mouse_pos = Some((local_x, local_y));
                if event.dragging() {
                    if let Some(node_id) = view.patcher_drag_node {
                        let track = view.patcher_track_id.min((TRACK_COUNT - 1) as u32) as usize;
                        if let Some(node) = view
                            .patcher_nodes
                            .get_mut(track)
                            .and_then(|nodes| nodes.iter_mut().find(|node| node.id == node_id))
                        {
                            node.pos_x = (local_x - view.patcher_drag_offset.0).max(0.0);
                            node.pos_y = (local_y - view.patcher_drag_offset.1).max(0.0);
                        }
                    }
                }
                cx.notify();
            }))
            .on_mouse_up(
                MouseButton::Left,
                cx.listener(|view, _event: &MouseUpEvent, _, cx| {
                    view.patcher_drag_node = None;
                    cx.notify();
                }),
            );

        let total_height = HEADER_HEIGHT + ROW_HEIGHT * VISIBLE_ROWS as f32;

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
            .h(px(total_height))
            .child(header)
            .child(controls)
            .child(canvas)
    }
}
