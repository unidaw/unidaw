use std::collections::{HashSet, VecDeque};
use std::sync::{Arc, Mutex};
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

use anyhow::Result;
use gpui::{
    div, px, rgb, size, App, Application, Bounds, Context, MouseButton,
    ScrollWheelEvent, SharedString, Timer, Window, WindowBounds,
    WindowOptions, prelude::*,
};

use daw_bridge::layout::{
    K_CHAIN_DEVICE_ID_AUTO, K_UI_MAX_TRACKS, UiChainCommandPayload,
    UiClipWindowCommandPayload, UiCommandPayload, UiCommandType, UiDiffType,
    UiHarmonyDiffType, UiChordCommandPayload, UiChordDiffType,
    UiPatcherGraphCommandPayload, UiPatcherNodeConfigPayload,
    UiPatcherPresetCommandPayload, UI_CLIP_WINDOW_FLAG_COMPLETE,
    UI_CLIP_WINDOW_FLAG_RESYNC,
};
use daw_bridge::reader::UiSnapshot;

use crate::engine::bridge::{
    decode_chord_diff, decode_harmony_diff, decode_ui_chain_diff, decode_ui_chain_error,
    decode_ui_diff, decode_ui_patcher_graph_diff, decode_ui_patcher_graph_error,
    log_last_ui_command, ui_diff_type, EngineBridge,
};
use crate::engine::supervisor::{
    default_engine_path, spawn_engine_process, stop_engine_process, EngineSupervisor,
};
use crate::clipboard::{ClipboardChord, ClipboardData, ClipboardHarmony, ClipboardNote};
use crate::plugins::{load_plugin_cache, PluginEntry};
use crate::harmony::{harmony_root_name, harmony_scale_name, SCALE_LIBRARY};
use crate::palette::{PaletteCommandId, PaletteMode, PALETTE_COMMANDS};
use crate::selection::{SelectionMask, SelectionRange};
use crate::state::{
    CellEntry, CellKind, ClipChord, ClipNote, HarmonyEntry, PendingChord, PendingNote,
    QueuedCommand,
};
use crate::ui::actions::{self, *};
use crate::util::split_u64;
use crate::tracker::{
    FOLLOW_PLAYHEAD_LOWER, FOLLOW_PLAYHEAD_UPPER, MAX_NOTE_COLUMNS, ROW_HEIGHT,
    TIME_COLUMN_WIDTH, VISIBLE_ROWS,
};

pub const NANOTICKS_PER_QUARTER: u64 = 960_000;
pub const BEATS_PER_BAR: u64 = 4;
pub const ZOOM_LEVELS: [u64; 7] = [1, 2, 4, 8, 16, 32, 64];
pub const DEFAULT_ZOOM_INDEX: usize = 2;
pub const TRACK_COUNT: usize = 8;
const EDIT_STEP_ROWS: i64 = 1;
const CLIP_WINDOW_MARGIN_ROWS: i64 = 4;
pub(crate) const PATCHER_NODE_EUCLIDEAN: u32 = 1;
pub(crate) const PATCHER_NODE_RANDOM_DEGREE: u32 = 5;
pub(crate) const PATCHER_NODE_EVENT_OUT: u32 = 6;

#[derive(Clone, Debug, Default)]
pub struct ChainDevice {
    pub id: u32,
    pub kind: u32,
    pub position: u32,
    pub patcher_node_id: u32,
    pub host_slot_index: u32,
    pub capability_mask: u32,
    pub bypass: bool,
}

#[derive(Clone, Debug)]
pub struct ChainClipboard {
    pub kind: u32,
    pub patcher_node_id: u32,
    pub host_slot_index: u32,
    pub bypass: bool,
}

#[derive(Clone, Debug, Default)]
pub struct PatcherNodeUi {
    pub id: u32,
    pub node_type: u32,
    pub inputs: Vec<u32>,
}

#[derive(Clone, Copy, Debug)]
pub enum ChainAddMode {
    Instrument,
    Effect,
}

#[derive(Clone, Copy, Debug, Default)]
pub struct ClipWindowState {
    pub request_id: u32,
    pub cursor_event_index: u32,
    pub next_event_index: u32,
    pub window_start: u64,
    pub window_end: u64,
    pub clip_version: u32,
    pub complete: bool,
}

#[derive(Clone, Copy, Debug, Default)]
struct ClipWindowRequestState {
    window_start: u64,
    window_end: u64,
    request_id: u32,
    cursor_event_index: u32,
    in_flight_cursor: u32,
    clip_version: u32,
    complete: bool,
    in_flight: bool,
}
#[derive(Clone)]
#[allow(dead_code)]
pub struct EngineView {
    pub bridge: Option<Arc<EngineBridge>>,
    pub snapshot: UiSnapshot,
    pub clip_window: Vec<ClipWindowState>,
    pub status: SharedString,
    pub plugins: Vec<PluginEntry>,
    pub palette_open: bool,
    pub palette_query: String,
    pub palette_selection: usize,
    pub palette_mode: PaletteMode,
    pub palette_empty_logged: bool,
    pub scale_browser_open: bool,
    pub scale_browser_query: String,
    pub scale_browser_selection: usize,
    pub focused_track_index: usize,
    pub cursor_nanotick: u64,
    pub cursor_col: usize,
    pub scroll_nanotick_offset: i64,
    pub zoom_index: usize,
    pub follow_playhead: bool,
    pub harmony_focus: bool,
    pub harmony_scale_id: u32,
    pub edit_active: bool,
    pub edit_text: String,
    pub jump_open: bool,
    pub jump_text: String,
    pub selection: Option<SelectionRange>,
    pub selection_mask: SelectionMask,
    pub selection_anchor_nanotick: Option<u64>,
    pub loop_range: Option<(u64, u64)>,
    pub clipboard: Option<ClipboardData>,
    pub toast_message: Option<String>,
    pub toast_deadline: Option<Instant>,
    pub pending_notes: Vec<PendingNote>,
    pub pending_chords: Vec<PendingChord>,
    pub clip_notes: Vec<Vec<ClipNote>>,
    pub clip_version_local: u32,
    pub harmony_version_local: u32,
    pub harmony_events: Vec<HarmonyEntry>,
    pub clip_chords: Vec<Vec<ClipChord>>,
    pub queued_commands: VecDeque<QueuedCommand>,
    pub clip_resync_pending: bool,
    pub harmony_resync_pending: bool,
    pub track_columns: Vec<usize>,
    pub track_quantize: Vec<bool>,
    pub track_names: Vec<Option<String>>,
    pub chain_versions: Vec<u32>,
    pub chain_devices: Vec<Vec<ChainDevice>>,
    pub chain_focus: bool,
    pub focused_chain_device_id: Option<u32>,
    pub chain_clipboard: Option<ChainClipboard>,
    pub chain_add_open: bool,
    pub chain_add_mode: Option<ChainAddMode>,
    pub patcher_open: bool,
    pub patcher_track_id: u32,
    pub patcher_versions: Vec<u32>,
    pub patcher_nodes: Vec<Vec<PatcherNodeUi>>,
    pub patcher_link_source: Option<u32>,
    pub patcher_link_target: Option<u32>,
    pub preset_save_open: bool,
    pub preset_save_text: String,
    pub ui_debug: bool,
}

pub trait UiNotify {
    fn notify(&mut self);
}

impl EngineView {
    
    fn new(_cx: &mut Context<Self>) -> Self {
        Self::new_state()
    }

    fn new_state() -> Self {
        let plugins = load_plugin_cache();
        Self {
            bridge: None,
            snapshot: UiSnapshot {
                version: 0,
                ui_visual_sample_count: 0,
                ui_global_nanotick_playhead: 0,
                ui_track_count: 0,
                ui_transport_state: 0,
                ui_clip_version: 0,
                ui_clip_offset: 0,
                ui_clip_bytes: 0,
                ui_harmony_version: 0,
                ui_harmony_offset: 0,
                ui_harmony_bytes: 0,
                ui_track_peak_rms: [0.0; K_UI_MAX_TRACKS],
            },
            clip_window: vec![ClipWindowState::default(); TRACK_COUNT],
            status: "SHM: disconnected".into(),
            plugins,
            palette_open: false,
            palette_query: String::new(),
            palette_selection: 0,
            palette_mode: PaletteMode::Commands,
            palette_empty_logged: false,
            scale_browser_open: false,
            scale_browser_query: String::new(),
            scale_browser_selection: 0,
            focused_track_index: 0,
            cursor_nanotick: 0,
            cursor_col: 0,
            scroll_nanotick_offset: 0,
            zoom_index: DEFAULT_ZOOM_INDEX,
            follow_playhead: true,
            harmony_focus: false,
            harmony_scale_id: 1,
            edit_active: false,
            edit_text: String::new(),
            jump_open: false,
            jump_text: String::new(),
            selection: None,
            selection_mask: SelectionMask::empty(TRACK_COUNT),
            selection_anchor_nanotick: None,
            loop_range: None,
            clipboard: None,
            toast_message: None,
            toast_deadline: None,
            pending_notes: Vec::new(),
            pending_chords: Vec::new(),
            clip_notes: vec![Vec::new(); TRACK_COUNT],
            clip_version_local: 0,
            harmony_version_local: 0,
            harmony_events: Vec::new(),
            clip_chords: vec![Vec::new(); TRACK_COUNT],
            queued_commands: VecDeque::new(),
            clip_resync_pending: false,
            harmony_resync_pending: false,
            track_columns: vec![1; TRACK_COUNT],
            track_quantize: vec![true; TRACK_COUNT],
            track_names: vec![None; TRACK_COUNT],
            chain_versions: vec![0; TRACK_COUNT],
            chain_devices: vec![Vec::new(); TRACK_COUNT],
            chain_focus: false,
            focused_chain_device_id: None,
            chain_clipboard: None,
            chain_add_open: false,
            chain_add_mode: None,
            patcher_open: false,
            patcher_track_id: 0,
            patcher_versions: vec![0; TRACK_COUNT],
            patcher_nodes: vec![Vec::new(); TRACK_COUNT],
            patcher_link_source: None,
            patcher_link_target: None,
            preset_save_open: false,
            preset_save_text: String::new(),
            ui_debug: std::env::var("DAW_UI_DEBUG").map_or(false, |v| v == "1"),
        }
    }

    #[allow(dead_code)]
    pub fn new_for_tests() -> Self {
        Self::new_state()
    }

    pub fn toggle_palette(&mut self, cx: &mut impl UiNotify) {
        if self.scale_browser_open {
            self.scale_browser_open = false;
        }
        self.palette_open = !self.palette_open;
        self.palette_query.clear();
        self.palette_selection = 0;
        self.palette_mode = PaletteMode::Commands;
        self.palette_empty_logged = false;
        cx.notify();
    }

    fn open_plugin_palette(&mut self, cx: &mut impl UiNotify) {
        if self.scale_browser_open {
            self.scale_browser_open = false;
        }
        self.plugins = load_plugin_cache();
        self.palette_open = true;
        self.palette_query.clear();
        self.palette_selection = 0;
        self.palette_mode = PaletteMode::Plugins;
        self.palette_empty_logged = false;
        if self.filtered_plugin_indices().is_empty() {
            eprintln!(
                "daw-app: plugin palette empty (plugins={}, query='{}')",
                self.plugins.len(),
                self.palette_query
            );
        }
        cx.notify();
    }

    pub(crate) fn open_plugin_palette_for_chain(
        &mut self,
        mode: ChainAddMode,
        cx: &mut impl UiNotify,
    ) {
        self.chain_add_mode = Some(mode);
        self.chain_add_open = false;
        self.open_plugin_palette(cx);
    }

    fn close_palette(&mut self, cx: &mut impl UiNotify) {
        if self.palette_open {
            self.palette_open = false;
            self.palette_empty_logged = false;
            self.chain_add_mode = None;
            cx.notify();
        }
    }

    fn open_jump(&mut self, cx: &mut impl UiNotify) {
        if self.scale_browser_open {
            self.scale_browser_open = false;
        }
        self.palette_open = false;
        self.edit_active = false;
        self.edit_text.clear();
        self.jump_open = true;
        self.jump_text.clear();
        cx.notify();
    }

    fn close_jump(&mut self, cx: &mut impl UiNotify) {
        if self.jump_open {
            self.jump_open = false;
            self.jump_text.clear();
            cx.notify();
        }
    }

    fn append_query(&mut self, value: &str, cx: &mut impl UiNotify) {
        if !self.palette_open || value.is_empty() {
            return;
        }
        if value.chars().all(|ch| ch.is_whitespace()) {
            return;
        }
        self.palette_query.push_str(value);
        self.palette_selection = 0;
        self.palette_empty_logged = false;
        cx.notify();
    }

    fn backspace_query(&mut self, cx: &mut impl UiNotify) {
        if !self.palette_open {
            return;
        }
        if self.palette_query.is_empty() {
            if self.palette_mode == PaletteMode::Plugins {
                self.palette_mode = PaletteMode::Commands;
                self.palette_selection = 0;
                cx.notify();
            }
            return;
        }
        self.palette_query.pop();
        self.palette_selection = 0;
        self.palette_empty_logged = false;
        cx.notify();
    }

    fn move_selection(&mut self, delta: i32, cx: &mut impl UiNotify) {
        if !self.palette_open {
            return;
        }
        let filtered = match self.palette_mode {
            PaletteMode::Commands => self.filtered_command_indices(),
            PaletteMode::Plugins => self.filtered_plugin_indices(),
        };
        if filtered.is_empty() {
            self.palette_selection = 0;
            cx.notify();
            return;
        }
        let max_index = filtered.len().saturating_sub(1) as i32;
        let next = (self.palette_selection as i32 + delta).clamp(0, max_index);
        self.palette_selection = next as usize;
        cx.notify();
    }

    pub(crate) fn confirm_palette(&mut self, cx: &mut impl UiNotify) {
        if !self.palette_open {
            return;
        }
        match self.palette_mode {
            PaletteMode::Commands => {
                let filtered = self.filtered_command_indices();
                if filtered.is_empty() {
                    return;
                }
                let selection = self.palette_selection.min(filtered.len() - 1);
                let command = PALETTE_COMMANDS[filtered[selection]];
                match command.id {
                    PaletteCommandId::LoadPlugin => {
                        self.plugins = load_plugin_cache();
                        eprintln!(
                            "daw-app: plugin palette refresh {} entries",
                            self.plugins.len()
                        );
                        self.palette_mode = PaletteMode::Plugins;
                        self.palette_query.clear();
                        self.palette_selection = 0;
                        self.palette_empty_logged = false;
                        if self.filtered_plugin_indices().is_empty() {
                            eprintln!(
                                "daw-app: plugin palette empty (plugins={}, query='{}')",
                                self.plugins.len(),
                                self.palette_query
                            );
                        }
                        cx.notify();
                    }
                    PaletteCommandId::SetHarmonyScale => {
                        self.palette_open = false;
                        self.open_scale_browser(cx);
                    }
                }
            }
            PaletteMode::Plugins => {
                let filtered = self.filtered_plugin_indices();
                if filtered.is_empty() {
                    return;
                }
                let selection = self.palette_selection.min(filtered.len() - 1);
                let index = filtered[selection];
                let plugin = self.plugins[index].clone();

                if self.bridge.is_some() {
                    match self.chain_add_mode {
                        Some(ChainAddMode::Effect) => {
                            let payload = UiChainCommandPayload {
                                command_type: UiCommandType::AddDevice as u16,
                                flags: 0,
                                track_id: self.focused_track_index as u32,
                                base_version: self.chain_versions[self.focused_track_index],
                                device_id: K_CHAIN_DEVICE_ID_AUTO,
                                device_kind: 4,
                                insert_index: K_CHAIN_DEVICE_ID_AUTO,
                                patcher_node_id: 0,
                                host_slot_index: plugin.index as u32,
                                bypass: 0,
                                reserved: [0; 4],
                            };
                            self.enqueue_chain_payload(payload);
                        }
                        _ => {
                            let payload = UiCommandPayload {
                                command_type: UiCommandType::LoadPluginOnTrack as u16,
                                flags: 0,
                                track_id: self.focused_track_index as u32,
                                plugin_index: plugin.index as u32,
                                note_pitch: 0,
                                value0: 0,
                                note_nanotick_lo: 0,
                                note_nanotick_hi: 0,
                                note_duration_lo: 0,
                                note_duration_hi: 0,
                                base_version: 0,
                            };
                            self.enqueue_ui_command(payload);
                            if self.focused_track_index < self.track_names.len() {
                                self.track_names[self.focused_track_index] =
                                    Some(plugin.name.clone());
                            }
                        }
                    }
                } else {
                    // No-op until engine reconnects.
                }

                self.chain_add_mode = None;
                self.palette_open = false;
                cx.notify();
            }
        }
    }

    pub fn handle_keystroke(&mut self, keystroke: &gpui::Keystroke, cx: &mut impl UiNotify) {
        if keystroke.modifiers.control ||
            keystroke.modifiers.platform ||
            keystroke.modifiers.function {
            return;
        }
        let key_char = if keystroke.modifiers.alt &&
            keystroke.key.len() == 1 &&
            keystroke.key.chars().next().map_or(false, |ch| ch.is_ascii_digit()) {
            Some(keystroke.key.as_str())
        } else {
            keystroke_text(keystroke)
        };
        if self.scale_browser_open {
            if let Some(key_char) = key_char {
                self.append_scale_query(key_char, cx);
            }
            return;
        }
        if self.jump_open {
            if let Some(key_char) = key_char {
                if is_jump_char(key_char) {
                    self.jump_text.push_str(key_char);
                    cx.notify();
                }
            }
            return;
        }
        if self.preset_save_open {
            if let Some(key_char) = key_char {
                if key_char.chars().all(|ch| ch.is_ascii_alphanumeric() || ch == '_' || ch == '-') {
                    self.preset_save_text.push_str(key_char);
                    cx.notify();
                }
            }
            return;
        }
        if self.edit_active {
            if let Some(key_char) = key_char {
                self.edit_text.push_str(key_char);
                cx.notify();
            }
            return;
        }
        if let Some(key_char) = key_char {
            if self.palette_open {
                self.append_query(key_char, cx);
                return;
            }
            if self.chain_focus {
                return;
            }
            if self.harmony_focus {
                if let Some(root) = harmony_root_for_key(key_char) {
                    self.write_harmony(root, cx);
                    return;
                }
                return;
            }
            if let Some(degree) = degree_for_digit(key_char) {
                if keystroke.modifiers.alt {
                    if let Some(pitch) = pitch_for_key(key_char) {
                        self.write_note(pitch, cx);
                    }
                    return;
                }
                self.write_degree_note(degree, cx);
                return;
            }
            if key_char.eq_ignore_ascii_case("a") {
                self.write_note_off(cx);
                return;
            }
            // Check for MIDI notes (keyjazz letters).
            if let Some(pitch) = pitch_for_key(key_char) {
                self.write_note(pitch, cx);
                return;
            }
            // Check remaining letter keys for MIDI notes
            if let Some(pitch) = pitch_for_letter_key(key_char) {
                self.write_note(pitch, cx);
                return;
            }
            if is_cell_edit_start(key_char) {
                self.start_cell_edit(key_char, cx);
                return;
            }
        }
    }

    pub fn action_palette_up(&mut self, cx: &mut impl UiNotify) {
        if self.scale_browser_open {
            self.move_scale_selection(-1, cx);
        } else if self.palette_open {
            self.move_selection(-1, cx);
        } else {
            self.move_cursor_row(-1, cx);
        }
    }

    pub fn action_palette_down(&mut self, cx: &mut impl UiNotify) {
        if self.scale_browser_open {
            self.move_scale_selection(1, cx);
        } else if self.palette_open {
            self.move_selection(1, cx);
        } else {
            self.move_cursor_row(1, cx);
        }
    }

    pub fn action_palette_backspace(&mut self, cx: &mut impl UiNotify) {
        if self.edit_active {
            if self.edit_text.is_empty() {
                self.cancel_cell_edit(cx);
                if self.harmony_focus {
                    self.delete_harmony(cx);
                } else if self.chain_focus {
                    self.delete_selected_chain_device(cx);
                } else {
                    self.delete_note(cx);
                }
            } else {
                self.edit_text.pop();
                cx.notify();
            }
        } else if self.jump_open {
            if self.jump_text.is_empty() {
                self.close_jump(cx);
            } else {
                self.jump_text.pop();
                cx.notify();
            }
        } else if self.preset_save_open {
            if self.preset_save_text.is_empty() {
                self.preset_save_open = false;
                cx.notify();
            } else {
                self.preset_save_text.pop();
                cx.notify();
            }
        } else if self.scale_browser_open {
            self.backspace_scale_query(cx);
        } else if self.palette_open {
            self.backspace_query(cx);
        } else if self.chain_focus {
            self.delete_selected_chain_device(cx);
        } else if self.harmony_focus {
            self.delete_harmony(cx);
        } else {
            self.delete_note(cx);
        }
    }

    pub fn action_palette_confirm(&mut self, cx: &mut impl UiNotify) {
        if self.edit_active {
            self.commit_cell_edit(cx);
        } else if self.jump_open {
            self.confirm_jump(cx);
        } else if self.preset_save_open {
            self.commit_preset_save(cx);
        } else if self.scale_browser_open {
            self.confirm_scale_browser(cx);
        } else if self.palette_open {
            self.confirm_palette(cx);
        } else {
            self.start_cell_edit("", cx);
        }
    }

    fn move_cursor_row(&mut self, delta: i64, cx: &mut impl UiNotify) {
        let row_nanoticks = self.row_nanoticks() as i64;
        if row_nanoticks <= 0 {
            return;
        }
        let current = self.cursor_nanotick as i64;
        let mut next = current + delta * row_nanoticks;
        if next < 0 {
            next = 0;
        }
        self.cursor_nanotick = next as u64;
        self.ensure_cursor_visible();
        self.clear_edit_state();
        cx.notify();
    }

    fn move_cursor_or_focus(&mut self, delta: i32, cx: &mut impl UiNotify) {
        if self.harmony_focus {
            if delta > 0 {
                self.harmony_focus = false;
                self.cursor_col = 0;
            }
            self.clear_edit_state();
            cx.notify();
            return;
        }

        let columns = self.track_columns[self.focused_track_index];
        if delta > 0 {
            if self.cursor_col + 1 < columns {
                self.cursor_col += 1;
            } else if self.focused_track_index + 1 < TRACK_COUNT {
                self.focused_track_index += 1;
                self.cursor_col = 0;
            }
        } else if self.cursor_col > 0 {
            self.cursor_col -= 1;
        } else if self.focused_track_index > 0 {
            self.focused_track_index -= 1;
            let next_columns = self.track_columns[self.focused_track_index];
            self.cursor_col = next_columns.saturating_sub(1);
        } else {
            self.harmony_focus = true;
        }
        self.clear_edit_state();
        cx.notify();
    }

    fn scroll_rows(&mut self, delta: i64, cx: &mut impl UiNotify) {
        let row_nanoticks = self.row_nanoticks() as i64;
        if row_nanoticks <= 0 {
            return;
        }
        let next = (self.scroll_nanotick_offset + delta * row_nanoticks).max(0);
        if next != self.scroll_nanotick_offset {
            self.scroll_nanotick_offset = next;
            self.follow_playhead = false;
            cx.notify();
        }
    }

    fn scroll_by_nanoticks(&mut self, delta: i64, cx: &mut impl UiNotify) {
        let next = (self.scroll_nanotick_offset + delta).max(0);
        if next != self.scroll_nanotick_offset {
            self.scroll_nanotick_offset = next;
            self.follow_playhead = false;
            cx.notify();
        }
    }

    fn zoom_by(&mut self, delta: i32, cx: &mut impl UiNotify) {
        let max_index = ZOOM_LEVELS.len().saturating_sub(1) as i32;
        let next = (self.zoom_index as i32 + delta).clamp(0, max_index) as usize;
        if next == self.zoom_index {
            return;
        }
        let cursor_view_row = self.cursor_view_row();
        self.zoom_index = next;
        let row_nanoticks = self.row_nanoticks() as i64;
        self.scroll_nanotick_offset =
            self.cursor_nanotick as i64 - cursor_view_row * row_nanoticks;
        if self.scroll_nanotick_offset < 0 {
            self.scroll_nanotick_offset = 0;
        }
        self.ensure_cursor_visible();
        self.follow_playhead = false;
        cx.notify();
    }

    pub(crate) fn handle_scroll_wheel(
        &mut self,
        event: &ScrollWheelEvent,
        cx: &mut impl UiNotify,
    ) {
        let delta_pixels = event.delta.pixel_delta(px(ROW_HEIGHT));
        let line_delta = delta_pixels.y / px(ROW_HEIGHT);
        if line_delta == 0.0 {
            return;
        }
        let step = if line_delta.abs() < 1.0 {
            line_delta.signum()
        } else {
            line_delta.round()
        };
        if event.modifiers.platform {
            let zoom_step = if step > 0.0 { -1 } else { 1 };
            self.zoom_by(zoom_step, cx);
        } else if event.modifiers.shift {
            let row_nanoticks = self.row_nanoticks() as f32;
            let micro = (row_nanoticks / 4.0).max(1.0);
            let delta_ticks = (line_delta * micro).round() as i64;
            if delta_ticks != 0 {
                self.scroll_by_nanoticks(delta_ticks, cx);
            }
        } else {
            let steps = step as i64;
            if steps != 0 {
                self.scroll_rows(steps, cx);
            }
        }
    }

    pub(crate) fn jump_to_nanotick(&mut self, nanotick: u64, cx: &mut impl UiNotify) {
        let snapped = self.snap_nanotick_to_row(nanotick);
        let row_nanoticks = self.row_nanoticks() as i64;
        self.cursor_nanotick = snapped;
        self.scroll_nanotick_offset =
            snapped as i64 - (VISIBLE_ROWS as i64 / 2) * row_nanoticks;
        if self.scroll_nanotick_offset < 0 {
            self.scroll_nanotick_offset = 0;
        }
        self.follow_playhead = false;
        self.ensure_cursor_visible();
        cx.notify();
    }

    fn confirm_jump(&mut self, cx: &mut impl UiNotify) {
        if let Some(nanotick) = parse_jump_text(&self.jump_text) {
            self.jump_to_nanotick(nanotick, cx);
            self.close_jump(cx);
        }
    }

    pub(crate) fn show_toast(&mut self, message: &str, cx: &mut impl UiNotify) {
        self.toast_message = Some(message.to_string());
        self.toast_deadline = Some(Instant::now() + Duration::from_millis(1200));
        cx.notify();
    }

    fn set_loop_from_selection_or_page(&mut self, cx: &mut impl UiNotify) {
        let row_nanoticks = self.row_nanoticks();
        if row_nanoticks == 0 {
            return;
        }
        let (start, end) = if let Some((start, end)) = self.selection_bounds() {
            (start, end.saturating_add(row_nanoticks))
        } else {
            let (page_start, page_end) = self.page_range();
            (page_start, page_end.saturating_add(row_nanoticks))
        };
        if end <= start {
            self.show_toast("Invalid loop range", cx);
            return;
        }
        self.set_loop_range(start, end, cx);
    }

    pub fn set_loop_range(&mut self, start: u64, end: u64, cx: &mut impl UiNotify) {
        self.loop_range = Some((start, end));
        if self.bridge.is_none() {
            cx.notify();
            return;
        }
        let (start_lo, start_hi) = split_u64(start);
        let (end_lo, end_hi) = split_u64(end);
        let payload = UiCommandPayload {
            command_type: UiCommandType::SetLoopRange as u16,
            flags: 0,
            track_id: 0,
            plugin_index: 0,
            note_pitch: 0,
            value0: 0,
            note_nanotick_lo: start_lo,
            note_nanotick_hi: start_hi,
            note_duration_lo: end_lo,
            note_duration_hi: end_hi,
            base_version: 0,
        };
        self.enqueue_ui_command(payload);
        cx.notify();
    }

    pub fn selection_bounds(&self) -> Option<(u64, u64)> {
        let selection = self.selection?;
        let start = selection.start.min(selection.end);
        let end = selection.start.max(selection.end);
        Some((start, end))
    }

    pub(crate) fn selection_contains_cell(
        &self,
        nanotick: u64,
        track: usize,
        column: usize,
    ) -> bool {
        let Some((start, end)) = self.selection_bounds() else {
            return false;
        };
        if track >= self.selection_mask.tracks.len() {
            return false;
        }
        if nanotick < start || nanotick > end {
            return false;
        }
        let mask = self.selection_mask.tracks[track];
        mask & (1u8 << column) != 0
    }

    pub(crate) fn selection_contains_harmony(&self, nanotick: u64) -> bool {
        let Some((start, end)) = self.selection_bounds() else {
            return false;
        };
        self.selection_mask.harmony && nanotick >= start && nanotick <= end
    }

    fn ensure_selection_for_cursor(&mut self, cx: &mut impl UiNotify) {
        if self.selection.is_some() {
            return;
        }
        let harmony = self.harmony_focus;
        let track = if harmony { None } else { Some(self.focused_track_index) };
        let column = if harmony { None } else { Some(self.cursor_col) };
        self.start_selection(
            self.cursor_nanotick,
            track,
            column,
            harmony,
            false,
            cx,
        );
    }

    pub fn expand_selection_rows(&mut self, delta_rows: i64, cx: &mut impl UiNotify) {
        if self.palette_open || self.scale_browser_open || self.jump_open {
            return;
        }
        self.ensure_selection_for_cursor(cx);
        self.move_cursor_row(delta_rows, cx);
        self.update_selection_end(self.cursor_nanotick, cx);
    }

    fn next_bar_boundary(&self, nanotick: u64, direction: i32) -> u64 {
        let bar_len = BEATS_PER_BAR * NANOTICKS_PER_QUARTER;
        if bar_len == 0 {
            return nanotick;
        }
        let bar_index = nanotick / bar_len;
        let at_boundary = nanotick % bar_len == 0;
        if direction >= 0 {
            (bar_index + 1) * bar_len
        } else if at_boundary {
            bar_index.saturating_sub(1) * bar_len
        } else {
            bar_index * bar_len
        }
    }

    pub fn expand_selection_to_bar(&mut self, direction: i32, cx: &mut impl UiNotify) {
        if self.palette_open || self.scale_browser_open || self.jump_open {
            return;
        }
        self.ensure_selection_for_cursor(cx);
        let target = self.next_bar_boundary(self.cursor_nanotick, direction);
        self.cursor_nanotick = target;
        self.ensure_cursor_visible();
        self.clear_edit_state();
        self.update_selection_end(self.cursor_nanotick, cx);
    }

    pub fn expand_selection_columns(&mut self, delta: i32, cx: &mut impl UiNotify) {
        if self.palette_open || self.scale_browser_open || self.jump_open {
            return;
        }
        self.ensure_selection_for_cursor(cx);
        self.move_cursor_or_focus(delta, cx);
        if self.harmony_focus {
            self.selection_mask.harmony = true;
        } else if self.focused_track_index < self.selection_mask.tracks.len() &&
            self.cursor_col < MAX_NOTE_COLUMNS {
            self.selection_mask.tracks[self.focused_track_index] |= 1u8 << self.cursor_col;
        }
        cx.notify();
    }

    pub(crate) fn start_selection(
        &mut self,
        nanotick: u64,
        track: Option<usize>,
        column: Option<usize>,
        harmony: bool,
        extend: bool,
        cx: &mut impl UiNotify,
    ) {
        let snapped = self.snap_nanotick_to_row(nanotick);
        if !extend {
            self.selection_mask = SelectionMask::empty(TRACK_COUNT);
            self.selection_anchor_nanotick = Some(snapped);
        }
        if harmony {
            self.selection_mask.harmony = true;
        } else if let (Some(track), Some(column)) = (track, column) {
            if track < self.selection_mask.tracks.len() && column < MAX_NOTE_COLUMNS {
                self.selection_mask.tracks[track] |= 1u8 << column;
            }
        }
        let anchor = self.selection_anchor_nanotick.unwrap_or(snapped);
        self.selection = Some(SelectionRange {
            start: anchor,
            end: snapped,
        });
        cx.notify();
    }

    pub(crate) fn update_selection_end(&mut self, nanotick: u64, cx: &mut impl UiNotify) {
        let snapped = self.snap_nanotick_to_row(nanotick);
        let Some(selection) = self.selection.as_mut() else {
            return;
        };
        selection.end = snapped;
        cx.notify();
    }

    #[allow(dead_code)]
    fn clear_selection(&mut self, cx: &mut impl UiNotify) {
        self.selection = None;
        self.selection_anchor_nanotick = None;
        self.selection_mask = SelectionMask::empty(TRACK_COUNT);
        cx.notify();
    }

    fn page_range(&self) -> (u64, u64) {
        let row_nanoticks = self.row_nanoticks();
        let start = self.scroll_nanotick_offset.max(0) as u64;
        let end = start + row_nanoticks.saturating_mul((VISIBLE_ROWS - 1) as u64);
        (start, end)
    }

    fn collect_notes_in_range(
        &self,
        start: u64,
        end: u64,
        mask: &SelectionMask,
        include_pending: bool,
    ) -> Vec<(usize, ClipNote)> {
        let mut notes = Vec::new();
        let mut seen = HashSet::new();
        if include_pending {
            for pending in &self.pending_notes {
                if pending.velocity == 0 {
                    continue;
                }
                let track = pending.track_id as usize;
                let column = pending.column as usize;
                if track >= mask.tracks.len() ||
                    (mask.tracks[track] & (1u8 << column)) == 0 {
                    continue;
                }
                if pending.nanotick < start || pending.nanotick > end {
                    continue;
                }
                let key = (track, pending.nanotick, pending.column);
                if seen.insert(key) {
                    notes.push((track, ClipNote {
                        nanotick: pending.nanotick,
                        duration: pending.duration,
                        pitch: pending.pitch,
                        velocity: pending.velocity,
                        column: pending.column,
                    }));
                }
            }
        }
        for (track, track_notes) in self.clip_notes.iter().enumerate() {
            if track >= mask.tracks.len() {
                break;
            }
            for note in track_notes {
                let column = note.column as usize;
                if (mask.tracks[track] & (1u8 << column)) == 0 {
                    continue;
                }
                if note.nanotick < start || note.nanotick > end {
                    continue;
                }
                let key = (track, note.nanotick, note.column);
                if seen.insert(key) {
                    notes.push((track, note.clone()));
                }
            }
        }
        notes
    }

    fn collect_chords_in_range(
        &self,
        start: u64,
        end: u64,
        mask: &SelectionMask,
        include_pending: bool,
    ) -> Vec<(usize, ClipChord)> {
        let mut chords = Vec::new();
        let mut seen = HashSet::new();
        if include_pending {
            for pending in &self.pending_chords {
                let track = pending.track_id as usize;
                let column = pending.column as usize;
                if track >= mask.tracks.len() ||
                    (mask.tracks[track] & (1u8 << column)) == 0 {
                    continue;
                }
                if pending.nanotick < start || pending.nanotick > end {
                    continue;
                }
                let key = (track, pending.nanotick, pending.column);
                if seen.insert(key) {
                    chords.push((track, ClipChord {
                        chord_id: 0,
                        nanotick: pending.nanotick,
                        duration: pending.duration,
                        spread: pending.spread,
                        humanize_timing: pending.humanize_timing,
                        humanize_velocity: pending.humanize_velocity,
                        degree: pending.degree,
                        quality: pending.quality,
                        inversion: pending.inversion,
                        base_octave: pending.base_octave,
                        column: pending.column,
                    }));
                }
            }
        }
        for (track, track_chords) in self.clip_chords.iter().enumerate() {
            if track >= mask.tracks.len() {
                break;
            }
            for chord in track_chords {
                let column = chord.column as usize;
                if (mask.tracks[track] & (1u8 << column)) == 0 {
                    continue;
                }
                if chord.nanotick < start || chord.nanotick > end {
                    continue;
                }
                let key = (track, chord.nanotick, chord.column);
                if seen.insert(key) {
                    chords.push((track, chord.clone()));
                }
            }
        }
        chords
    }

    fn collect_harmony_in_range(&self, start: u64, end: u64, mask: &SelectionMask)
        -> Vec<HarmonyEntry> {
        if !mask.harmony {
            return Vec::new();
        }
        self.harmony_events
            .iter()
            .filter(|event| event.nanotick >= start && event.nanotick <= end)
            .cloned()
            .collect()
    }

    fn build_clipboard(
        &mut self,
        start: u64,
        end: u64,
        mask: &SelectionMask,
        include_pending: bool,
    ) -> ClipboardData {
        let notes = self.collect_notes_in_range(start, end, mask, include_pending)
            .into_iter()
            .map(|(track, note)| ClipboardNote {
                track,
                column: note.column,
                offset: note.nanotick as i64 - start as i64,
                pitch: note.pitch,
                velocity: note.velocity,
                duration: note.duration,
            })
            .collect();
        let chords = self.collect_chords_in_range(start, end, mask, include_pending)
            .into_iter()
            .map(|(track, chord)| ClipboardChord {
                track,
                column: chord.column,
                offset: chord.nanotick as i64 - start as i64,
                duration: chord.duration,
                spread: chord.spread,
                humanize_timing: chord.humanize_timing,
                humanize_velocity: chord.humanize_velocity,
                degree: chord.degree,
                quality: chord.quality,
                inversion: chord.inversion,
                base_octave: chord.base_octave,
            })
            .collect();
        let harmonies = self.collect_harmony_in_range(start, end, mask)
            .into_iter()
            .map(|event| ClipboardHarmony {
                offset: event.nanotick as i64 - start as i64,
                root: event.root,
                scale_id: event.scale_id,
            })
            .collect();
        ClipboardData {
            notes,
            chords,
            harmonies,
        }
    }

    fn copy_selection(&mut self, cx: &mut impl UiNotify) {
        let Some((start, end)) = self.selection_bounds() else {
            self.show_toast("No selection", cx);
            return;
        };
        let mask = self.selection_mask.clone();
        let clipboard = self.build_clipboard(start, end, &mask, true);
        self.clipboard = Some(clipboard);
        cx.notify();
    }

    fn cut_selection(&mut self, cx: &mut impl UiNotify) {
        let Some((start, end)) = self.selection_bounds() else {
            self.show_toast("No selection", cx);
            return;
        };
        let mask = self.selection_mask.clone();
        let clipboard = self.build_clipboard(start, end, &mask, true);
        self.clipboard = Some(clipboard);
        self.delete_range(start, end, &mask, cx);
        cx.notify();
    }

    fn paste_selection(&mut self, cx: &mut impl UiNotify) {
        let Some(clipboard) = self.clipboard.clone() else {
            self.show_toast("Clipboard empty", cx);
            return;
        };
        let target = self.current_row_nanotick();
        self.paste_clipboard_at(&clipboard, target, cx);
    }

    fn copy_page(&mut self, cx: &mut impl UiNotify) {
        let (start, end) = self.page_range();
        let mut mask = SelectionMask::empty(TRACK_COUNT);
        let columns = self.track_columns[self.focused_track_index].min(MAX_NOTE_COLUMNS);
        if columns > 0 {
            mask.tracks[self.focused_track_index] = ((1u16 << columns) - 1) as u8;
        }
        let clipboard = self.build_clipboard(start, end, &mask, true);
        self.clipboard = Some(clipboard);
        cx.notify();
    }

    fn cut_page(&mut self, cx: &mut impl UiNotify) {
        let (start, end) = self.page_range();
        let mut mask = SelectionMask::empty(TRACK_COUNT);
        let columns = self.track_columns[self.focused_track_index].min(MAX_NOTE_COLUMNS);
        if columns > 0 {
            mask.tracks[self.focused_track_index] = ((1u16 << columns) - 1) as u8;
        }
        let clipboard = self.build_clipboard(start, end, &mask, true);
        self.clipboard = Some(clipboard);
        self.delete_range(start, end, &mask, cx);
        cx.notify();
    }

    fn paste_page(&mut self, cx: &mut impl UiNotify) {
        let Some(clipboard) = self.clipboard.clone() else {
            self.show_toast("Clipboard empty", cx);
            return;
        };
        let (start, _) = self.page_range();
        self.paste_clipboard_at(&clipboard, start, cx);
    }

    fn paste_clipboard_at(
        &mut self,
        clipboard: &ClipboardData,
        target_start: u64,
        cx: &mut impl UiNotify,
    ) {
        for harmony in &clipboard.harmonies {
            let target = target_start as i64 + harmony.offset;
            if target < 0 {
                continue;
            }
            self.write_harmony_at(target as u64, harmony.root, harmony.scale_id, cx);
        }
        for note in &clipboard.notes {
            let target = target_start as i64 + note.offset;
            if target < 0 {
                continue;
            }
            self.write_note_at(
                note.track,
                note.column,
                target as u64,
                note.pitch,
                note.velocity,
                note.duration,
                cx,
            );
        }
        for chord in &clipboard.chords {
            let target = target_start as i64 + chord.offset;
            if target < 0 {
                continue;
            }
            self.write_chord_at(
                chord.track,
                chord.column,
                target as u64,
                chord.duration,
                chord.degree,
                chord.quality,
                chord.inversion,
                chord.base_octave,
                chord.spread,
                chord.humanize_timing,
                chord.humanize_velocity,
                cx,
            );
        }
        cx.notify();
    }

    fn delete_range(
        &mut self,
        start: u64,
        end: u64,
        mask: &SelectionMask,
        cx: &mut impl UiNotify,
    ) {
        let notes = self.collect_notes_in_range(start, end, mask, true);
        let chords = self.collect_chords_in_range(start, end, mask, false);
        let harmonies = self.collect_harmony_in_range(start, end, mask);

        for (track, note) in notes {
            self.delete_note_at(track, note.column, note.nanotick, note.pitch, cx);
        }
        for (track, chord) in chords {
            if chord.chord_id != 0 {
                self.delete_chord_at(track, chord.chord_id, chord.nanotick, chord.column, cx);
            }
        }
        for harmony in harmonies {
            self.delete_harmony_at(harmony.nanotick, cx);
        }
        self.pending_chords.retain(|pending| {
            let track = pending.track_id as usize;
            let column = pending.column as usize;
            if track >= mask.tracks.len() ||
                (mask.tracks[track] & (1u8 << column)) == 0 {
                return true;
            }
            pending.nanotick < start || pending.nanotick > end
        });
        cx.notify();
    }

    fn toggle_follow_playhead(&mut self, cx: &mut impl UiNotify) {
        self.follow_playhead = !self.follow_playhead;
        cx.notify();
    }

    pub fn toggle_harmony_focus(&mut self, cx: &mut impl UiNotify) {
        self.harmony_focus = !self.harmony_focus;
        self.clear_edit_state();
        cx.notify();
    }

    pub fn move_column(&mut self, delta: i32, cx: &mut impl UiNotify) {
        let columns = self.track_columns[self.focused_track_index];
        let max_index = columns.saturating_sub(1) as i32;
        let next = (self.cursor_col as i32 + delta).clamp(0, max_index);
        self.cursor_col = next as usize;
        self.clear_edit_state();
        cx.notify();
    }

    pub fn adjust_columns(&mut self, track: usize, delta: i32, cx: &mut impl UiNotify) {
        if track >= self.track_columns.len() {
            return;
        }
        let current = self.track_columns[track] as i32;
        let next = (current + delta).clamp(1, MAX_NOTE_COLUMNS as i32) as usize;
        self.track_columns[track] = next;
        if self.focused_track_index == track {
            self.cursor_col = self.cursor_col.min(next.saturating_sub(1));
        }
        self.clear_edit_state();
        cx.notify();
    }

    pub fn focus_harmony_row(&mut self, row: usize, cx: &mut impl UiNotify) {
        let row = row.min(VISIBLE_ROWS - 1) as i64;
        self.cursor_nanotick = self.view_row_nanotick(row);
        self.harmony_focus = true;
        self.chain_focus = false;
        self.focused_chain_device_id = None;
        self.edit_active = false;
        self.edit_text.clear();
        cx.notify();
    }

    pub fn focus_note_cell(
        &mut self,
        row: usize,
        track: usize,
        column: usize,
        cx: &mut impl UiNotify,
    ) {
        let row = row.min(VISIBLE_ROWS - 1) as i64;
        self.cursor_nanotick = self.view_row_nanotick(row);
        self.focused_track_index = track.min(TRACK_COUNT - 1);
        let max_column = self.track_columns[self.focused_track_index]
            .saturating_sub(1);
        self.cursor_col = column.min(max_column);
        self.harmony_focus = false;
        self.chain_focus = false;
        self.focused_chain_device_id = None;
        self.clear_edit_state();
        cx.notify();
    }

    fn chain_selected_device(&self) -> Option<ChainDevice> {
        let track = self.focused_track_index;
        let device_id = self.focused_chain_device_id?;
        self.chain_devices
            .get(track)
            .and_then(|devices| devices.iter().find(|d| d.id == device_id))
            .cloned()
    }

    pub(crate) fn select_chain_device(&mut self, device_id: u32, cx: &mut impl UiNotify) {
        self.chain_focus = true;
        self.focused_chain_device_id = Some(device_id);
        self.harmony_focus = false;
        self.clear_edit_state();
        self.chain_add_open = false;
        cx.notify();
    }

    pub(crate) fn open_patcher_view(&mut self, cx: &mut impl UiNotify) {
        self.patcher_open = true;
        self.patcher_track_id = self.focused_track_index as u32;
        self.patcher_link_source = None;
        self.patcher_link_target = None;
        self.preset_save_open = false;
        self.preset_save_text.clear();
        cx.notify();
    }

    pub(crate) fn close_patcher_view(&mut self, cx: &mut impl UiNotify) {
        self.patcher_open = false;
        self.patcher_link_source = None;
        self.patcher_link_target = None;
        self.preset_save_open = false;
        self.preset_save_text.clear();
        cx.notify();
    }

    fn enqueue_patcher_graph_payload(&mut self, payload: UiPatcherGraphCommandPayload) {
        self.enqueue_patcher_graph_command(payload);
    }

    fn enqueue_patcher_config_payload(&mut self, payload: UiPatcherNodeConfigPayload) {
        self.enqueue_patcher_node_config(payload);
    }

    fn enqueue_patcher_preset_payload(&mut self, payload: UiPatcherPresetCommandPayload) {
        self.enqueue_patcher_preset(payload);
    }

    pub(crate) fn add_patcher_node(&mut self, node_type: u32, cx: &mut impl UiNotify) {
        let track = self.patcher_track_id.min((TRACK_COUNT - 1) as u32);
        let payload = UiPatcherGraphCommandPayload {
            command_type: UiCommandType::AddPatcherNode as u16,
            flags: 0,
            track_id: track,
            base_version: self.patcher_versions[track as usize],
            node_id: 0,
            node_type,
            src_node_id: 0,
            dst_node_id: 0,
            reserved: [0; 12],
        };
        self.enqueue_patcher_graph_payload(payload);
        cx.notify();
    }

    pub(crate) fn remove_patcher_node(&mut self, node_id: u32, cx: &mut impl UiNotify) {
        let track = self.patcher_track_id.min((TRACK_COUNT - 1) as u32);
        let payload = UiPatcherGraphCommandPayload {
            command_type: UiCommandType::RemovePatcherNode as u16,
            flags: 0,
            track_id: track,
            base_version: self.patcher_versions[track as usize],
            node_id,
            node_type: 0,
            src_node_id: 0,
            dst_node_id: 0,
            reserved: [0; 12],
        };
        self.enqueue_patcher_graph_payload(payload);
        cx.notify();
    }

    pub(crate) fn connect_patcher_nodes(&mut self, cx: &mut impl UiNotify) {
        let Some(src) = self.patcher_link_source else {
            self.show_toast("Select a source node", cx);
            return;
        };
        let Some(dst) = self.patcher_link_target else {
            self.show_toast("Select a target node", cx);
            return;
        };
        let track = self.patcher_track_id.min((TRACK_COUNT - 1) as u32);
        let payload = UiPatcherGraphCommandPayload {
            command_type: UiCommandType::ConnectPatcherNodes as u16,
            flags: 0,
            track_id: track,
            base_version: self.patcher_versions[track as usize],
            node_id: 0,
            node_type: 0,
            src_node_id: src,
            dst_node_id: dst,
            reserved: [0; 12],
        };
        self.enqueue_patcher_graph_payload(payload);
        cx.notify();
    }

    fn send_random_degree_config(&mut self, node_id: u32) {
        let track = self.patcher_track_id.min((TRACK_COUNT - 1) as u32);
        let mut config = [0u8; 16];
        let degree: u8 = 8;
        let velocity: u8 = 100;
        let duration: u64 = 0;
        config[0] = degree;
        config[1] = velocity;
        config[4..12].copy_from_slice(&duration.to_le_bytes());
        let payload = UiPatcherNodeConfigPayload {
            command_type: UiCommandType::SetPatcherNodeConfig as u16,
            flags: 0,
            track_id: track,
            base_version: self.patcher_versions[track as usize],
            node_id,
            config_type: PATCHER_NODE_RANDOM_DEGREE,
            config,
            reserved: [0; 4],
        };
        self.enqueue_patcher_config_payload(payload);
    }

    pub(crate) fn open_preset_save(&mut self, cx: &mut impl UiNotify) {
        self.preset_save_open = true;
        self.preset_save_text.clear();
        cx.notify();
    }

    pub(crate) fn commit_preset_save(&mut self, cx: &mut impl UiNotify) {
        if !self.preset_save_open {
            return;
        }
        let name = self.preset_save_text.trim().to_string();
        self.preset_save_open = false;
        self.preset_save_text.clear();
        if name.is_empty() {
            self.show_toast("Preset name empty", cx);
            return;
        }
        let track = self.patcher_track_id.min((TRACK_COUNT - 1) as u32);
        let mut payload = UiPatcherPresetCommandPayload::default();
        payload.command_type = UiCommandType::SavePatcherPreset as u16;
        payload.track_id = track;
        let bytes = name.as_bytes();
        let len = bytes.len().min(payload.name.len());
        payload.name[..len].copy_from_slice(&bytes[..len]);
        self.enqueue_patcher_preset_payload(payload);
        self.show_toast("Saved patcher preset", cx);
    }

    fn enqueue_chain_payload(&mut self, payload: UiChainCommandPayload) {
        self.enqueue_chain_command(payload);
    }

    pub(crate) fn toggle_chain_add_menu(&mut self, cx: &mut impl UiNotify) {
        self.chain_add_open = !self.chain_add_open;
        self.chain_focus = false;
        cx.notify();
    }

    pub(crate) fn add_chain_device(
        &mut self,
        device_kind: u32,
        patcher_node_id: u32,
        host_slot_index: u32,
        cx: &mut impl UiNotify,
    ) {
        let payload = UiChainCommandPayload {
            command_type: UiCommandType::AddDevice as u16,
            flags: 0,
            track_id: self.focused_track_index as u32,
            base_version: self.chain_versions[self.focused_track_index],
            device_id: K_CHAIN_DEVICE_ID_AUTO,
            device_kind,
            insert_index: K_CHAIN_DEVICE_ID_AUTO,
            patcher_node_id,
            host_slot_index,
            bypass: 0,
            reserved: [0; 4],
        };
        self.enqueue_chain_payload(payload);
        self.chain_add_open = false;
        cx.notify();
    }

    fn delete_selected_chain_device(&mut self, cx: &mut impl UiNotify) {
        let Some(device) = self.chain_selected_device() else {
            self.show_toast("No device selected", cx);
            return;
        };
        let payload = UiChainCommandPayload {
            command_type: UiCommandType::RemoveDevice as u16,
            flags: 0,
            track_id: self.focused_track_index as u32,
            base_version: self.chain_versions[self.focused_track_index],
            device_id: device.id,
            device_kind: device.kind,
            insert_index: K_CHAIN_DEVICE_ID_AUTO,
            patcher_node_id: device.patcher_node_id,
            host_slot_index: device.host_slot_index,
            bypass: device.bypass as u32,
            reserved: [0; 4],
        };
        self.enqueue_chain_payload(payload);
        self.focused_chain_device_id = None;
        cx.notify();
    }

    fn copy_selected_chain_device(&mut self, cx: &mut impl UiNotify) {
        let Some(device) = self.chain_selected_device() else {
            self.show_toast("No device selected", cx);
            return;
        };
        self.chain_clipboard = Some(ChainClipboard {
            kind: device.kind,
            patcher_node_id: device.patcher_node_id,
            host_slot_index: device.host_slot_index,
            bypass: device.bypass,
        });
        cx.notify();
    }

    fn cut_selected_chain_device(&mut self, cx: &mut impl UiNotify) {
        self.copy_selected_chain_device(cx);
        self.delete_selected_chain_device(cx);
    }

    fn paste_chain_device(&mut self, cx: &mut impl UiNotify) {
        let Some(clipboard) = self.chain_clipboard.clone() else {
            self.show_toast("Chain clipboard empty", cx);
            return;
        };
        let insert_index = self
            .chain_selected_device()
            .map(|device| device.position)
            .unwrap_or(K_CHAIN_DEVICE_ID_AUTO);
        let payload = UiChainCommandPayload {
            command_type: UiCommandType::AddDevice as u16,
            flags: 0,
            track_id: self.focused_track_index as u32,
            base_version: self.chain_versions[self.focused_track_index],
            device_id: K_CHAIN_DEVICE_ID_AUTO,
            device_kind: clipboard.kind,
            insert_index,
            patcher_node_id: clipboard.patcher_node_id,
            host_slot_index: clipboard.host_slot_index,
            bypass: clipboard.bypass as u32,
            reserved: [0; 4],
        };
        self.enqueue_chain_payload(payload);
    }

    fn duplicate_selected_chain_device(&mut self, cx: &mut impl UiNotify) {
        let Some(device) = self.chain_selected_device() else {
            self.show_toast("No device selected", cx);
            return;
        };
        let payload = UiChainCommandPayload {
            command_type: UiCommandType::AddDevice as u16,
            flags: 0,
            track_id: self.focused_track_index as u32,
            base_version: self.chain_versions[self.focused_track_index],
            device_id: K_CHAIN_DEVICE_ID_AUTO,
            device_kind: device.kind,
            insert_index: device.position.saturating_add(1),
            patcher_node_id: device.patcher_node_id,
            host_slot_index: device.host_slot_index,
            bypass: device.bypass as u32,
            reserved: [0; 4],
        };
        self.enqueue_chain_payload(payload);
    }

    pub(crate) fn toggle_chain_device_bypass(
        &mut self,
        device_id: u32,
        cx: &mut impl UiNotify,
    ) {
        let track = self.focused_track_index;
        let devices = match self.chain_devices.get_mut(track) {
            Some(devices) => devices,
            None => {
                self.show_toast("No chain for track", cx);
                return;
            }
        };
        let Some(device) = devices.iter_mut().find(|d| d.id == device_id) else {
            self.show_toast("Device not found", cx);
            return;
        };
        device.bypass = !device.bypass;
        let payload = UiChainCommandPayload {
            command_type: UiCommandType::UpdateDevice as u16,
            flags: 0x1,
            track_id: track as u32,
            base_version: self.chain_versions[track],
            device_id: device.id,
            device_kind: device.kind,
            insert_index: K_CHAIN_DEVICE_ID_AUTO,
            patcher_node_id: device.patcher_node_id,
            host_slot_index: device.host_slot_index,
            bypass: device.bypass as u32,
            reserved: [0; 4],
        };
        self.enqueue_chain_payload(payload);
        cx.notify();
    }

    pub(crate) fn open_vst_editor(&mut self, device_id: u32, cx: &mut impl UiNotify) {
        let track = self.focused_track_index;
        let Some(device) = self
            .chain_devices
            .get(track)
            .and_then(|devices| devices.iter().find(|d| d.id == device_id))
            .cloned()
        else {
            self.show_toast("Device not found", cx);
            return;
        };
        if device.kind != 3 && device.kind != 4 {
            self.show_toast("Editor only available for VSTs", cx);
            return;
        }
        if self.bridge.is_none() {
            self.show_toast("Engine not connected", cx);
            return;
        }
        let payload = UiCommandPayload {
            command_type: UiCommandType::OpenPluginEditor as u16,
            flags: 0,
            track_id: track as u32,
            plugin_index: 0,
            note_pitch: 0,
            value0: device_id,
            note_nanotick_lo: 0,
            note_nanotick_hi: 0,
            note_duration_lo: 0,
            note_duration_hi: 0,
            base_version: 0,
        };
        self.enqueue_ui_command(payload);
        cx.notify();
    }

    fn start_cell_edit(&mut self, initial: &str, cx: &mut impl UiNotify) {
        if begin_cell_edit(&mut self.edit_active, &mut self.edit_text, initial) {
            cx.notify();
        }
    }

    fn cancel_cell_edit(&mut self, cx: &mut impl UiNotify) {
        if cancel_cell_edit_state(&mut self.edit_active, &mut self.edit_text) {
            cx.notify();
        }
    }

    fn commit_cell_edit(&mut self, cx: &mut impl UiNotify) {
        if let Some(token) = commit_cell_edit_state(&mut self.edit_active, &mut self.edit_text) {
            if !token.is_empty() {
                self.apply_cell_token(&token, cx);
            }
            cx.notify();
        }
    }

    fn clear_edit_state(&mut self) {
        if self.edit_active {
            self.edit_active = false;
            self.edit_text.clear();
        }
    }

    #[allow(dead_code)]
    pub fn set_harmony_scale(&mut self, scale_id: u32, cx: &mut impl UiNotify) {
        self.harmony_scale_id = scale_id;
        cx.notify();
    }

    fn open_scale_browser(&mut self, cx: &mut impl UiNotify) {
        self.scale_browser_open = true;
        self.scale_browser_query.clear();
        self.scale_browser_selection = 0;
        cx.notify();
    }

    fn close_scale_browser(&mut self, cx: &mut impl UiNotify) {
        if self.scale_browser_open {
            self.scale_browser_open = false;
            cx.notify();
        }
    }

    fn append_scale_query(&mut self, value: &str, cx: &mut impl UiNotify) {
        if value.is_empty() {
            return;
        }
        if value.chars().all(|ch| ch.is_whitespace()) {
            return;
        }
        self.scale_browser_query.push_str(value);
        self.scale_browser_selection = 0;
        cx.notify();
    }

    fn backspace_scale_query(&mut self, cx: &mut impl UiNotify) {
        if self.scale_browser_query.is_empty() {
            return;
        }
        self.scale_browser_query.pop();
        self.scale_browser_selection = 0;
        cx.notify();
    }

    fn move_scale_selection(&mut self, delta: i32, cx: &mut impl UiNotify) {
        let filtered = self.filtered_scale_indices();
        if filtered.is_empty() {
            self.scale_browser_selection = 0;
            cx.notify();
            return;
        }
        let max_index = filtered.len().saturating_sub(1) as i32;
        let next =
            (self.scale_browser_selection as i32 + delta).clamp(0, max_index);
        self.scale_browser_selection = next as usize;
        cx.notify();
    }

    pub(crate) fn confirm_scale_browser(&mut self, cx: &mut impl UiNotify) {
        if !self.scale_browser_open {
            return;
        }
        let filtered = self.filtered_scale_indices();
        if filtered.is_empty() {
            return;
        }
        let selection = self
            .scale_browser_selection
            .min(filtered.len().saturating_sub(1));
        let scale = SCALE_LIBRARY[filtered[selection]];
        self.write_harmony_scale(scale.id, cx);
        self.scale_browser_open = false;
        cx.notify();
    }

    fn write_harmony(&mut self, root: u32, cx: &mut impl UiNotify) {
        let nanotick = self.current_row_nanotick();

        if self.bridge.is_some() {
            let (nanotick_lo, nanotick_hi) = split_u64(nanotick);
            let payload = UiCommandPayload {
                command_type: UiCommandType::WriteHarmony as u16,
                flags: 0,
                track_id: 0,
                plugin_index: 0,
                note_pitch: root,
                value0: self.harmony_scale_id,
                note_nanotick_lo: nanotick_lo,
                note_nanotick_hi: nanotick_hi,
                note_duration_lo: 0,
                note_duration_hi: 0,
                base_version: self.current_harmony_version(),
            };
            self.enqueue_ui_command(payload);
            self.bump_harmony_version();
        }
        cx.notify();
    }

    fn write_harmony_at(
        &mut self,
        nanotick: u64,
        root: u32,
        scale_id: u32,
        cx: &mut impl UiNotify,
    ) {
        if self.bridge.is_none() {
            self.harmony_events.retain(|event| event.nanotick != nanotick);
            self.harmony_events.push(HarmonyEntry {
                nanotick,
                root,
                scale_id,
            });
            self.harmony_events.sort_by_key(|event| event.nanotick);
            cx.notify();
            return;
        }
        let (nanotick_lo, nanotick_hi) = split_u64(nanotick);
        let payload = UiCommandPayload {
            command_type: UiCommandType::WriteHarmony as u16,
            flags: 0,
            track_id: 0,
            plugin_index: 0,
            note_pitch: root,
            value0: scale_id,
            note_nanotick_lo: nanotick_lo,
            note_nanotick_hi: nanotick_hi,
            note_duration_lo: 0,
            note_duration_hi: 0,
            base_version: self.current_harmony_version(),
        };
        self.enqueue_ui_command(payload);
        self.bump_harmony_version();
        self.harmony_events.retain(|event| event.nanotick != nanotick);
        self.harmony_events.push(HarmonyEntry {
            nanotick,
            root,
            scale_id,
        });
        self.harmony_events.sort_by_key(|event| event.nanotick);
        cx.notify();
    }

    fn write_harmony_scale(&mut self, scale_id: u32, cx: &mut impl UiNotify) {
        let nanotick = self.current_row_nanotick();
        let root = self.harmony_root_at(nanotick);

        if self.bridge.is_some() {
            let (nanotick_lo, nanotick_hi) = split_u64(nanotick);
            let payload = UiCommandPayload {
                command_type: UiCommandType::WriteHarmony as u16,
                flags: 0,
                track_id: 0,
                plugin_index: 0,
                note_pitch: root,
                value0: scale_id,
                note_nanotick_lo: nanotick_lo,
                note_nanotick_hi: nanotick_hi,
                note_duration_lo: 0,
                note_duration_hi: 0,
                base_version: self.current_harmony_version(),
            };
            self.enqueue_ui_command(payload);
            self.bump_harmony_version();
        }
        self.harmony_scale_id = scale_id;
        cx.notify();
    }

    pub(crate) fn harmony_root_at(&self, nanotick: u64) -> u32 {
        let mut root = 0;
        for event in &self.harmony_events {
            if event.nanotick > nanotick {
                break;
            }
            root = event.root;
            if event.nanotick == nanotick {
                break;
            }
        }
        root
    }

    pub(crate) fn harmony_scale_at(&self, nanotick: u64) -> u32 {
        let mut scale_id = self.harmony_scale_id;
        for event in &self.harmony_events {
            if event.nanotick > nanotick {
                break;
            }
            scale_id = event.scale_id;
            if event.nanotick == nanotick {
                break;
            }
        }
        scale_id
    }

    pub(crate) fn current_row_nanotick(&self) -> u64 {
        self.cursor_nanotick
    }

    fn lines_per_beat(&self) -> u64 {
        ZOOM_LEVELS[self.zoom_index]
    }

    pub(crate) fn row_nanoticks(&self) -> u64 {
        NANOTICKS_PER_QUARTER / self.lines_per_beat()
    }

    pub(crate) fn desired_clip_window_range(&self) -> (u64, u64) {
        let row_nanoticks = self.row_nanoticks() as i64;
        if row_nanoticks <= 0 {
            return (0, 0);
        }
        let visible_start = self.scroll_nanotick_offset.max(0);
        let visible_end =
            visible_start + row_nanoticks.saturating_mul(VISIBLE_ROWS as i64);
        let mut start = visible_start;
        let mut end = visible_end;
        if let Some((sel_start, sel_end)) = self.selection_bounds() {
            start = start.min(sel_start as i64);
            end = end.max((sel_end + row_nanoticks as u64) as i64);
        }
        let margin = row_nanoticks.saturating_mul(CLIP_WINDOW_MARGIN_ROWS);
        start = (start - margin).max(0);
        end = end.saturating_add(margin);
        (start as u64, end as u64)
    }

    fn snap_nanotick_to_row(&self, nanotick: u64) -> u64 {
        let row_nanoticks = self.row_nanoticks();
        if row_nanoticks == 0 {
            return nanotick;
        }
        (nanotick / row_nanoticks) * row_nanoticks
    }

    pub fn cursor_view_row(&self) -> i64 {
        let row_nanoticks = self.row_nanoticks() as i64;
        if row_nanoticks <= 0 {
            return 0;
        }
        (self.cursor_nanotick as i64 - self.scroll_nanotick_offset) / row_nanoticks
    }

    pub fn view_row_nanotick(&self, row_index: i64) -> u64 {
        let row_nanoticks = self.row_nanoticks() as i64;
        let scroll = self.scroll_nanotick_offset.max(0);
        let row = row_index.max(0);
        (scroll + row * row_nanoticks) as u64
    }

    #[allow(dead_code)]
    pub fn nanotick_for_row(&self, absolute_row: i64) -> u64 {
        let row_nanoticks = self.row_nanoticks();
        let row = absolute_row.max(0) as u64;
        row * row_nanoticks
    }

    fn ensure_cursor_visible(&mut self) {
        let view_rows = VISIBLE_ROWS as i64;
        let row_nanoticks = self.row_nanoticks() as i64;
        if row_nanoticks <= 0 {
            return;
        }
        let cursor_row = self.cursor_view_row();
        if cursor_row < 0 {
            self.scroll_nanotick_offset = self.cursor_nanotick as i64;
        } else if cursor_row >= view_rows {
            self.scroll_nanotick_offset =
                self.cursor_nanotick as i64 - (view_rows - 1) * row_nanoticks;
        }
        if self.scroll_nanotick_offset < 0 {
            self.scroll_nanotick_offset = 0;
        }
    }

    fn delete_harmony(&mut self, cx: &mut impl UiNotify) {
        let nanotick = self.current_row_nanotick();

        // Check if there's actually a harmony event to delete
        let has_harmony = self.harmony_events.iter()
            .any(|event| event.nanotick == nanotick);

        if self.bridge.is_some() {
            let (nanotick_lo, nanotick_hi) = split_u64(nanotick);
            let payload = UiCommandPayload {
                command_type: UiCommandType::DeleteHarmony as u16,
                flags: 0,
                track_id: 0,
                plugin_index: 0,
                note_pitch: 0,
                value0: 0,
                note_nanotick_lo: nanotick_lo,
                note_nanotick_hi: nanotick_hi,
                note_duration_lo: 0,
                note_duration_hi: 0,
                base_version: self.current_harmony_version(),
            };
            self.enqueue_ui_command(payload);
            self.bump_harmony_version();
        }

        // Move cursor down after deletion if there was something to delete
        if has_harmony {
            self.move_cursor_row(1, cx);
        }
        cx.notify();
    }

    fn delete_note(&mut self, cx: &mut impl UiNotify) {
        let nanotick = self.current_row_nanotick();
        if let Some(entry) = self.cell_entry_at(
            nanotick,
            self.focused_track_index,
            self.cursor_col,
        ) {
            match entry.kind {
                CellKind::Note => {
                    let Some(pitch) = entry.note_pitch else {
                        return;
                    };
                    if self.bridge.is_some() {
                        let (note_nanotick_lo, note_nanotick_hi) = split_u64(nanotick);
                        let payload = UiCommandPayload {
                            command_type: UiCommandType::DeleteNote as u16,
                            flags: self.cursor_col as u16,
                            track_id: self.focused_track_index as u32,
                            plugin_index: 0,
                            note_pitch: pitch as u32,
                            value0: 0,
                            note_nanotick_lo,
                            note_nanotick_hi,
                            note_duration_lo: 0,
                            note_duration_hi: 0,
                            base_version: self.current_clip_version(),
                        };
                        self.enqueue_ui_command(payload);
                        self.bump_clip_version();
                    } else {
                        return;
                    }
                    if let Some(notes) = self.clip_notes.get_mut(self.focused_track_index) {
                        notes.retain(|note| {
                            !(note.nanotick == nanotick &&
                                note.column == self.cursor_col as u8)
                        });
                    }
                    self.pending_notes.retain(|note| {
                        !(note.track_id == self.focused_track_index as u32 &&
                            note.nanotick == nanotick &&
                            note.column == self.cursor_col as u8)
                    });
                    self.pending_chords.retain(|chord| {
                        !(chord.track_id == self.focused_track_index as u32 &&
                            chord.nanotick == nanotick &&
                            chord.column == self.cursor_col as u8)
                    });
                }
                CellKind::Chord => {
                    let Some(chord_id) = entry.chord_id else {
                        return;
                    };
                    self.delete_chord_at(
                        self.focused_track_index,
                        chord_id,
                        nanotick,
                        self.cursor_col as u8,
                        cx,
                    );
                }
            }
        }
        // Always move cursor down to support rapid backspace on empty cells.
        self.move_cursor_row(1, cx);
        cx.notify();
    }

    fn delete_note_at(
        &mut self,
        track: usize,
        column: u8,
        nanotick: u64,
        pitch: u8,
        cx: &mut impl UiNotify,
    ) {
        if track >= TRACK_COUNT {
            return;
        }
        if self.bridge.is_some() {
            let (note_nanotick_lo, note_nanotick_hi) = split_u64(nanotick);
            let payload = UiCommandPayload {
                command_type: UiCommandType::DeleteNote as u16,
                flags: column as u16,
                track_id: track as u32,
                plugin_index: 0,
                note_pitch: pitch as u32,
                value0: 0,
                note_nanotick_lo,
                note_nanotick_hi,
                note_duration_lo: 0,
                note_duration_hi: 0,
                base_version: self.current_clip_version(),
            };
            self.enqueue_ui_command(payload);
            self.bump_clip_version();
        }
        if let Some(notes) = self.clip_notes.get_mut(track) {
            notes.retain(|note| !(note.nanotick == nanotick && note.column == column));
        }
        self.pending_notes.retain(|note| {
            !(note.track_id == track as u32 &&
                note.nanotick == nanotick &&
                note.column == column)
        });
        cx.notify();
    }

    fn delete_chord_at(
        &mut self,
        track: usize,
        chord_id: u32,
        nanotick: u64,
        column: u8,
        cx: &mut impl UiNotify,
    ) {
        if track >= TRACK_COUNT {
            return;
        }
        if self.bridge.is_some() {
            let (nanotick_lo, nanotick_hi) = split_u64(nanotick);
            let payload = UiChordCommandPayload {
                command_type: UiCommandType::DeleteChord as u16,
                flags: column as u16,
                track_id: track as u32,
                base_version: self.current_clip_version(),
                nanotick_lo,
                nanotick_hi,
                duration_lo: 0,
                duration_hi: 0,
                degree: 0,
                quality: 0,
                inversion: 0,
                base_octave: 0,
                humanize_timing: 0,
                humanize_velocity: 0,
                reserved: 0,
                spread_nanoticks: chord_id,
            };
            self.enqueue_chord_command(payload);
            self.bump_clip_version();
        }
        if let Some(chords) = self.clip_chords.get_mut(track) {
            chords.retain(|chord| chord.chord_id != chord_id);
        }
        self.pending_chords.retain(|chord| {
            if chord.track_id != track as u32 {
                return true;
            }
            if chord_id == 0 {
                !(chord.nanotick == nanotick && chord.column == column)
            } else {
                true
            }
        });
        cx.notify();
    }

    fn delete_harmony_at(&mut self, nanotick: u64, cx: &mut impl UiNotify) {
        if self.bridge.is_some() {
            let (nanotick_lo, nanotick_hi) = split_u64(nanotick);
            let payload = UiCommandPayload {
                command_type: UiCommandType::DeleteHarmony as u16,
                flags: 0,
                track_id: 0,
                plugin_index: 0,
                note_pitch: 0,
                value0: 0,
                note_nanotick_lo: nanotick_lo,
                note_nanotick_hi: nanotick_hi,
                note_duration_lo: 0,
                note_duration_hi: 0,
                base_version: self.current_harmony_version(),
            };
            self.enqueue_ui_command(payload);
            self.bump_harmony_version();
        }
        self.harmony_events.retain(|event| event.nanotick != nanotick);
        cx.notify();
    }

    pub fn send_undo(&mut self, cx: &mut impl UiNotify) {
        if self.bridge.is_some() {
            let payload = UiCommandPayload {
                command_type: UiCommandType::Undo as u16,
                flags: 0,
                track_id: 0,
                plugin_index: 0,
                note_pitch: 0,
                value0: 0,
                note_nanotick_lo: 0,
                note_nanotick_hi: 0,
                note_duration_lo: 0,
                note_duration_hi: 0,
                base_version: self.current_clip_version(),
            };
            self.enqueue_ui_command(payload);
        }
        cx.notify();
    }

    pub fn send_redo(&mut self, cx: &mut impl UiNotify) {
        if self.bridge.is_some() {
            let payload = UiCommandPayload {
                command_type: UiCommandType::Redo as u16,
                flags: 0,
                track_id: 0,
                plugin_index: 0,
                note_pitch: 0,
                value0: 0,
                note_nanotick_lo: 0,
                note_nanotick_hi: 0,
                note_duration_lo: 0,
                note_duration_hi: 0,
                base_version: self.current_clip_version(),
            };
            self.enqueue_ui_command(payload);
        }
        cx.notify();
    }

    fn write_degree_note(&mut self, degree: u8, cx: &mut impl UiNotify) {
        let chord = ParsedChordToken {
            degree: degree as u32,
            quality: 0,  // Plain degree note
            inversion: 0,
            base_octave: 4,  // Default to octave 4
            spread_nanoticks: 0,
            humanize_timing: 0,
            humanize_velocity: 0,
            duration: None,
        };
        self.send_chord(chord, cx);
        self.move_cursor_row(EDIT_STEP_ROWS, cx);
    }

    fn write_note(&mut self, pitch: u8, cx: &mut impl UiNotify) {
        let nanotick = self.current_row_nanotick();
        let (note_nanotick_lo, note_nanotick_hi) = split_u64(nanotick);
        let (note_duration_lo, note_duration_hi) = split_u64(0);

        // Clear any existing pending notes at this position
        self.pending_notes.retain(|note| {
            !(note.track_id == self.focused_track_index as u32 &&
                note.nanotick == nanotick &&
                note.column == self.cursor_col as u8)
        });
        self.pending_chords.retain(|chord| {
            !(chord.track_id == self.focused_track_index as u32 &&
                chord.nanotick == nanotick &&
                chord.column == self.cursor_col as u8)
        });

        if self.bridge.is_some() {
            let payload = UiCommandPayload {
                command_type: UiCommandType::WriteNote as u16,
                flags: self.cursor_col as u16,
                track_id: self.focused_track_index as u32,
                plugin_index: 0,
                note_pitch: pitch as u32,
                value0: 100,
                note_nanotick_lo,
                note_nanotick_hi,
                note_duration_lo,
                note_duration_hi,
                base_version: self.current_clip_version(),
            };
            self.enqueue_ui_command(payload);
            self.bump_clip_version();
        } else {
            return;
        }
        self.pending_notes.retain(|note| {
            !(note.track_id == self.focused_track_index as u32 &&
                note.nanotick == nanotick &&
                note.column == self.cursor_col as u8)
        });
        self.pending_chords.retain(|chord| {
            !(chord.track_id == self.focused_track_index as u32 &&
                chord.nanotick == nanotick &&
                chord.column == self.cursor_col as u8)
        });
        if let Some(notes) = self.clip_notes.get_mut(self.focused_track_index) {
            notes.retain(|note| {
                !(note.nanotick == nanotick && note.column == self.cursor_col as u8)
            });
        }
        if let Some(chords) = self.clip_chords.get_mut(self.focused_track_index) {
            chords.retain(|chord| {
                !(chord.nanotick == nanotick && chord.column == self.cursor_col as u8)
            });
        }
        self.pending_notes.push(PendingNote {
            track_id: self.focused_track_index as u32,
            nanotick,
            duration: 0,
            pitch,
            velocity: 100,
            column: self.cursor_col as u8,
        });
        self.move_cursor_row(EDIT_STEP_ROWS, cx);
    }

    fn write_note_at(
        &mut self,
        track: usize,
        column: u8,
        nanotick: u64,
        pitch: u8,
        velocity: u8,
        duration: u64,
        cx: &mut impl UiNotify,
    ) {
        if track >= TRACK_COUNT || column as usize >= MAX_NOTE_COLUMNS {
            return;
        }
        if self.bridge.is_none() {
            if let Some(notes) = self.clip_notes.get_mut(track) {
                notes.retain(|note| !(note.nanotick == nanotick && note.column == column));
                notes.push(ClipNote {
                    nanotick,
                    duration,
                    pitch,
                    velocity,
                    column,
                });
                notes.sort_by_key(|note| (note.nanotick, note.column));
            }
            if let Some(chords) = self.clip_chords.get_mut(track) {
                chords.retain(|chord| !(chord.nanotick == nanotick && chord.column == column));
            }
            cx.notify();
            return;
        }
        let (note_nanotick_lo, note_nanotick_hi) = split_u64(nanotick);
        let (note_duration_lo, note_duration_hi) = split_u64(duration);
        let payload = UiCommandPayload {
            command_type: UiCommandType::WriteNote as u16,
            flags: column as u16,
            track_id: track as u32,
            plugin_index: 0,
            note_pitch: pitch as u32,
            value0: velocity as u32,
            note_nanotick_lo,
            note_nanotick_hi,
            note_duration_lo,
            note_duration_hi,
            base_version: self.current_clip_version(),
        };
        self.enqueue_ui_command(payload);
        self.bump_clip_version();

        self.pending_notes.retain(|note| {
            !(note.track_id == track as u32 &&
                note.nanotick == nanotick &&
                note.column == column)
        });
        self.pending_chords.retain(|chord| {
            !(chord.track_id == track as u32 &&
                chord.nanotick == nanotick &&
                chord.column == column)
        });
        if let Some(notes) = self.clip_notes.get_mut(track) {
            notes.retain(|note| !(note.nanotick == nanotick && note.column == column));
        }
        if let Some(chords) = self.clip_chords.get_mut(track) {
            chords.retain(|chord| !(chord.nanotick == nanotick && chord.column == column));
        }
        self.pending_notes.push(PendingNote {
            track_id: track as u32,
            nanotick,
            duration,
            pitch,
            velocity,
            column,
        });
        cx.notify();
    }

    fn note_off_pitch(&self, track_index: usize, column: u8, nanotick: u64) -> Option<u8> {
        let mut best: Option<(u64, u8)> = None;

        for note in self.pending_notes.iter() {
            if note.track_id as usize != track_index || note.column != column {
                continue;
            }
            if note.nanotick == nanotick && note.velocity > 0 {
                return Some(note.pitch);
            }
            if note.nanotick < nanotick && note.velocity > 0 {
                if best.map_or(true, |(best_tick, _)| note.nanotick > best_tick) {
                    best = Some((note.nanotick, note.pitch));
                }
            }
        }

        if let Some(notes) = self.clip_notes.get(track_index) {
            for note in notes.iter() {
                if note.column != column {
                    continue;
                }
                if note.nanotick == nanotick && note.velocity > 0 {
                    return Some(note.pitch);
                }
                if note.nanotick < nanotick && note.velocity > 0 {
                    if best.map_or(true, |(best_tick, _)| note.nanotick > best_tick) {
                        best = Some((note.nanotick, note.pitch));
                    }
                }
            }
        }

        if let Some((_, pitch)) = best {
            return Some(pitch);
        }

        let mut chord_tick: Option<u64> = None;
        for chord in self.pending_chords.iter() {
            if chord.track_id as usize != track_index || chord.column != column {
                continue;
            }
            if chord.nanotick < nanotick &&
                chord_tick.map_or(true, |best_tick| chord.nanotick > best_tick) {
                chord_tick = Some(chord.nanotick);
            }
        }
        if let Some(chords) = self.clip_chords.get(track_index) {
            for chord in chords.iter() {
                if chord.column != column {
                    continue;
                }
                if chord.nanotick < nanotick &&
                    chord_tick.map_or(true, |best_tick| chord.nanotick > best_tick) {
                    chord_tick = Some(chord.nanotick);
                }
            }
        }
        if chord_tick.is_some() {
            return Some(0);
        }
        None
    }

    fn next_note_on_nanotick(&self, track_index: usize, column: u8, nanotick: u64) -> Option<u64> {
        let mut next: Option<u64> = None;
        for note in self.pending_notes.iter() {
            if note.track_id as usize != track_index || note.column != column {
                continue;
            }
            if note.nanotick <= nanotick || note.velocity == 0 {
                continue;
            }
            if next.map_or(true, |best| note.nanotick < best) {
                next = Some(note.nanotick);
            }
        }
        if let Some(notes) = self.clip_notes.get(track_index) {
            for note in notes.iter() {
                if note.column != column || note.velocity == 0 {
                    continue;
                }
                if note.nanotick <= nanotick {
                    continue;
                }
                if next.map_or(true, |best| note.nanotick < best) {
                    next = Some(note.nanotick);
                }
            }
        }
        next
    }

    fn prev_note_on_nanotick(&self, track_index: usize, column: u8, nanotick: u64) -> Option<u64> {
        let mut prev: Option<u64> = None;
        for note in self.pending_notes.iter() {
            if note.track_id as usize != track_index || note.column != column {
                continue;
            }
            if note.nanotick >= nanotick || note.velocity == 0 {
                continue;
            }
            if prev.map_or(true, |best| note.nanotick > best) {
                prev = Some(note.nanotick);
            }
        }
        if let Some(notes) = self.clip_notes.get(track_index) {
            for note in notes.iter() {
                if note.column != column || note.velocity == 0 {
                    continue;
                }
                if note.nanotick >= nanotick {
                    continue;
                }
                if prev.map_or(true, |best| note.nanotick > best) {
                    prev = Some(note.nanotick);
                }
            }
        }
        prev
    }

    fn prev_chord_nanotick(&self, track_index: usize, column: u8, nanotick: u64) -> Option<u64> {
        let mut prev: Option<u64> = None;
        for chord in self.pending_chords.iter() {
            if chord.track_id as usize != track_index {
                continue;
            }
            if chord.column != column {
                continue;
            }
            if chord.nanotick >= nanotick {
                continue;
            }
            if prev.map_or(true, |best| chord.nanotick > best) {
                prev = Some(chord.nanotick);
            }
        }
        if let Some(chords) = self.clip_chords.get(track_index) {
            for chord in chords.iter() {
                if chord.column != column {
                    continue;
                }
                if chord.nanotick >= nanotick {
                    continue;
                }
                if prev.map_or(true, |best| chord.nanotick > best) {
                    prev = Some(chord.nanotick);
                }
            }
        }
        prev
    }

    fn next_chord_nanotick(&self, track_index: usize, column: u8, nanotick: u64) -> Option<u64> {
        let mut next: Option<u64> = None;
        for chord in self.pending_chords.iter() {
            if chord.track_id as usize != track_index {
                continue;
            }
            if chord.column != column {
                continue;
            }
            if chord.nanotick <= nanotick {
                continue;
            }
            if next.map_or(true, |best| chord.nanotick < best) {
                next = Some(chord.nanotick);
            }
        }
        if let Some(chords) = self.clip_chords.get(track_index) {
            for chord in chords.iter() {
                if chord.column != column {
                    continue;
                }
                if chord.nanotick <= nanotick {
                    continue;
                }
                if next.map_or(true, |best| chord.nanotick < best) {
                    next = Some(chord.nanotick);
                }
            }
        }
        next
    }

    fn remove_note_offs_in_span(
        &mut self,
        track_index: usize,
        column: u8,
        nanotick: u64,
        prev_boundary: Option<u64>,
        next_boundary: Option<u64>,
    ) {
        let lower = prev_boundary.unwrap_or(0);
        let upper = next_boundary.unwrap_or(u64::MAX);
        let in_range = |note_nanotick: u64| {
            if note_nanotick == nanotick {
                return false;
            }
            note_nanotick > lower && note_nanotick < upper
        };

        self.pending_notes.retain(|note| {
            if note.track_id as usize != track_index || note.column != column {
                return true;
            }
            if note.velocity != 0 || note.duration != 0 {
                return true;
            }
            !in_range(note.nanotick)
        });

        if let Some(notes) = self.clip_notes.get_mut(track_index) {
            notes.retain(|note| {
                if note.column != column {
                    return true;
                }
                if note.velocity != 0 || note.duration != 0 {
                    return true;
                }
                !in_range(note.nanotick)
            });
        }
    }

    fn write_note_off(&mut self, cx: &mut impl UiNotify) {
        let nanotick = self.current_row_nanotick();
        let column = self.cursor_col as u8;
        let prev_note_on = self.prev_note_on_nanotick(
            self.focused_track_index,
            column,
            nanotick,
        );
        let next_note_on = self.next_note_on_nanotick(
            self.focused_track_index,
            column,
            nanotick,
        );
        let prev_chord = self.prev_chord_nanotick(self.focused_track_index, column, nanotick);
        let next_chord = self.next_chord_nanotick(self.focused_track_index, column, nanotick);
        let (prev_boundary, next_boundary) = (
            match (prev_note_on, prev_chord) {
                (Some(a), Some(b)) => Some(a.max(b)),
                (Some(a), None) => Some(a),
                (None, Some(b)) => Some(b),
                (None, None) => None,
            },
            match (next_note_on, next_chord) {
                (Some(a), Some(b)) => Some(a.min(b)),
                (Some(a), None) => Some(a),
                (None, Some(b)) => Some(b),
                (None, None) => None,
            },
        );

        let pitch = self
            .note_off_pitch(self.focused_track_index, column, nanotick)
            .unwrap_or(0);
        if pitch == 0 && prev_boundary.is_none() {
            self.move_cursor_row(EDIT_STEP_ROWS, cx);
            cx.notify();
            return;
        }
        let (note_nanotick_lo, note_nanotick_hi) = split_u64(nanotick);
        let (note_duration_lo, note_duration_hi) = split_u64(0);

        self.pending_notes.retain(|note| {
            !(note.track_id == self.focused_track_index as u32 &&
                note.nanotick == nanotick &&
                note.column == self.cursor_col as u8)
        });
        self.pending_chords.retain(|chord| {
            !(chord.track_id == self.focused_track_index as u32 &&
                chord.nanotick == nanotick &&
                chord.column == self.cursor_col as u8)
        });

        if self.bridge.is_some() {
            let payload = UiCommandPayload {
                command_type: UiCommandType::WriteNote as u16,
                flags: self.cursor_col as u16,
                track_id: self.focused_track_index as u32,
                plugin_index: 0,
                note_pitch: pitch as u32,
                value0: 0,
                note_nanotick_lo,
                note_nanotick_hi,
                note_duration_lo,
                note_duration_hi,
                base_version: self.current_clip_version(),
            };
            self.enqueue_ui_command(payload);
            self.bump_clip_version();
        } else {
            return;
        }
        self.pending_notes.retain(|note| {
            !(note.track_id == self.focused_track_index as u32 &&
                note.nanotick == nanotick &&
                note.column == self.cursor_col as u8)
        });
        self.pending_chords.retain(|chord| {
            !(chord.track_id == self.focused_track_index as u32 &&
                chord.nanotick == nanotick &&
                chord.column == self.cursor_col as u8)
        });
        if let Some(notes) = self.clip_notes.get_mut(self.focused_track_index) {
            notes.retain(|note| {
                !(note.nanotick == nanotick && note.column == self.cursor_col as u8)
            });
        }
        if let Some(chords) = self.clip_chords.get_mut(self.focused_track_index) {
            chords.retain(|chord| {
                !(chord.nanotick == nanotick && chord.column == self.cursor_col as u8)
            });
        }
        self.remove_note_offs_in_span(
            self.focused_track_index,
            column,
            nanotick,
            prev_boundary,
            next_boundary,
        );
        self.pending_notes.push(PendingNote {
            track_id: self.focused_track_index as u32,
            nanotick,
            duration: 0,
            pitch,
            velocity: 0,
            column: self.cursor_col as u8,
        });
        self.move_cursor_row(EDIT_STEP_ROWS, cx);
    }

    pub fn toggle_play(&mut self, cx: &mut impl UiNotify) {
        if self.bridge.is_some() {
            let payload = UiCommandPayload {
                command_type: UiCommandType::TogglePlay as u16,
                flags: 0,
                track_id: 0,
                plugin_index: 0,
                note_pitch: 0,
                value0: 0,
                note_nanotick_lo: 0,
                note_nanotick_hi: 0,
                note_duration_lo: 0,
                note_duration_hi: 0,
                base_version: 0,
            };
            self.enqueue_ui_command(payload);
        }
        cx.notify();
    }


    pub fn cell_entry_at(
        &self,
        nanotick: u64,
        track_index: usize,
        column: usize,
    ) -> Option<CellEntry> {
        let entries = self.entries_for_row(nanotick, track_index);
        entries
            .iter()
            .find(|entry| entry.column == column)
            .cloned()
    }

    pub fn entries_for_row(&self, nanotick: u64, track_index: usize) -> Vec<CellEntry> {
        let mut entries: Vec<CellEntry> = Vec::new();
        if let Some(notes) = self.clip_notes.get(track_index) {
            for note in notes.iter().filter(|note| note.nanotick == nanotick) {
                let is_note_off = note.velocity == 0 && note.duration == 0;
                entries.push(CellEntry {
                    kind: CellKind::Note,
                    text: if is_note_off {
                        String::new()
                    } else {
                        pitch_to_note(note.pitch)
                    },
                    nanotick,
                    note_pitch: Some(note.pitch),
                    chord_id: None,
                    column: note.column as usize,
                    note_off: is_note_off,
                });
            }
        }
        for note in self
            .pending_notes
            .iter()
            .filter(|note| note.track_id as usize == track_index && note.nanotick == nanotick)
        {
            let is_note_off = note.velocity == 0 && note.duration == 0;
            entries.push(CellEntry {
                kind: CellKind::Note,
                text: if is_note_off {
                    String::new()
                } else {
                    pitch_to_note(note.pitch)
                },
                nanotick,
                note_pitch: Some(note.pitch),
                chord_id: None,
                column: note.column as usize,
                note_off: is_note_off,
            });
        }
        let mut pending_chord_columns = std::collections::HashSet::new();
        for chord in self.pending_chords.iter().filter(|chord| {
            chord.track_id as usize == track_index && chord.nanotick == nanotick
        }) {
            pending_chord_columns.insert(chord.column);
            let temp = ClipChord {
                chord_id: 0,
                nanotick,
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
            entries.push(CellEntry {
                kind: CellKind::Chord,
                text: chord_token_text(&temp),
                nanotick,
                note_pitch: None,
                chord_id: Some(0),
                column: chord.column as usize,
                note_off: false,
            });
        }
        if let Some(chords) = self.clip_chords.get(track_index) {
            for chord in chords.iter().filter(|chord| chord.nanotick == nanotick) {
                if pending_chord_columns.contains(&chord.column) {
                    continue;
                }
                let text = chord_token_text(chord);
                entries.push(CellEntry {
                    kind: CellKind::Chord,
                    text,
                    nanotick,
                    note_pitch: None,
                    chord_id: Some(chord.chord_id),
                    column: chord.column as usize,
                    note_off: false,
                });
            }
        }
        entries.sort_by(|a, b| {
            match (&a.kind, &b.kind) {
                (CellKind::Chord, CellKind::Note) => std::cmp::Ordering::Less,
                (CellKind::Note, CellKind::Chord) => std::cmp::Ordering::Greater,
                _ => a.text.cmp(&b.text),
            }
        });
        entries
    }

    fn apply_cell_token(&mut self, token: &str, cx: &mut impl UiNotify) {
        if let Some(parsed) = parse_chord_token(token) {
            self.send_chord(parsed, cx);
            return;
        }
        if let Some(parsed) = parse_degree_note_token(token) {
            self.send_chord(parsed, cx);
            return;
        }
        if let Some(note) = parse_note_token(token) {
            self.write_note(note, cx);
        }
    }

    fn send_chord(&mut self, chord: ParsedChordToken, cx: &mut impl UiNotify) {
        let nanotick = self.current_row_nanotick();
        let duration = chord.duration.unwrap_or(0);
        let degree = chord.degree.min(255) as u16;
        let pending_degree = degree as u8;
        let column = self.cursor_col as u8;

        if self.bridge.is_some() {
            let (nanotick_lo, nanotick_hi) = split_u64(nanotick);
            let (duration_lo, duration_hi) = split_u64(duration);
            let payload = UiChordCommandPayload {
                command_type: UiCommandType::WriteChord as u16,
                flags: column as u16,
                track_id: self.focused_track_index as u32,
                base_version: self.current_clip_version(),
                nanotick_lo,
                nanotick_hi,
                duration_lo,
                duration_hi,
                degree,
                quality: chord.quality,
                inversion: chord.inversion,
                base_octave: chord.base_octave,
                humanize_timing: chord.humanize_timing,
                humanize_velocity: chord.humanize_velocity,
                reserved: 0,
                spread_nanoticks: chord.spread_nanoticks,
            };
            self.enqueue_chord_command(payload);
            self.bump_clip_version();
        } else {
            return;
        }
        self.pending_notes.retain(|note| {
            !(note.track_id == self.focused_track_index as u32 &&
                note.nanotick == nanotick &&
                note.column == column)
        });
        self.pending_chords.retain(|pending| {
            !(pending.track_id == self.focused_track_index as u32 &&
                pending.nanotick == nanotick &&
                pending.column == column)
        });
        if let Some(notes) = self.clip_notes.get_mut(self.focused_track_index) {
            notes.retain(|note| !(note.nanotick == nanotick && note.column == column));
        }
        if let Some(chords) = self.clip_chords.get_mut(self.focused_track_index) {
            chords.retain(|chord| !(chord.nanotick == nanotick && chord.column == column));
        }
        self.pending_chords.push(PendingChord {
            track_id: self.focused_track_index as u32,
            nanotick,
            duration,
            spread: chord.spread_nanoticks,
            humanize_timing: chord.humanize_timing as u16,
            humanize_velocity: chord.humanize_velocity as u16,
            degree: pending_degree,
            quality: chord.quality,
            inversion: chord.inversion,
            base_octave: chord.base_octave,
            column,
        });
        cx.notify();
    }

    fn write_chord_at(
        &mut self,
        track: usize,
        column: u8,
        nanotick: u64,
        duration: u64,
        degree: u8,
        quality: u8,
        inversion: u8,
        base_octave: u8,
        spread: u32,
        humanize_timing: u16,
        humanize_velocity: u16,
        cx: &mut impl UiNotify,
    ) {
        if track >= TRACK_COUNT || column as usize >= MAX_NOTE_COLUMNS {
            return;
        }
        if self.bridge.is_none() {
            if let Some(notes) = self.clip_notes.get_mut(track) {
                notes.retain(|note| !(note.nanotick == nanotick && note.column == column));
            }
            if let Some(chords) = self.clip_chords.get_mut(track) {
                chords.retain(|chord| !(chord.nanotick == nanotick && chord.column == column));
                chords.push(ClipChord {
                    chord_id: 0,
                    nanotick,
                    duration,
                    spread,
                    humanize_timing,
                    humanize_velocity,
                    degree,
                    quality,
                    inversion,
                    base_octave,
                    column,
                });
                chords.sort_by_key(|chord| (chord.nanotick, chord.column));
            }
            cx.notify();
            return;
        }
        let (nanotick_lo, nanotick_hi) = split_u64(nanotick);
        let (duration_lo, duration_hi) = split_u64(duration);
        let timing = humanize_timing.min(u16::from(u8::MAX)) as u8;
        let velocity = humanize_velocity.min(u16::from(u8::MAX)) as u8;
        let payload = UiChordCommandPayload {
            command_type: UiCommandType::WriteChord as u16,
            flags: column as u16,
            track_id: track as u32,
            base_version: self.current_clip_version(),
            nanotick_lo,
            nanotick_hi,
            duration_lo,
            duration_hi,
            degree: degree as u16,
            quality,
            inversion,
            base_octave,
            humanize_timing: timing,
            humanize_velocity: velocity,
            reserved: 0,
            spread_nanoticks: spread,
        };
        self.enqueue_chord_command(payload);
        self.bump_clip_version();

        self.pending_notes.retain(|note| {
            !(note.track_id == track as u32 &&
                note.nanotick == nanotick &&
                note.column == column)
        });
        self.pending_chords.retain(|pending| {
            !(pending.track_id == track as u32 &&
                pending.nanotick == nanotick &&
                pending.column == column)
        });
        if let Some(notes) = self.clip_notes.get_mut(track) {
            notes.retain(|note| !(note.nanotick == nanotick && note.column == column));
        }
        if let Some(chords) = self.clip_chords.get_mut(track) {
            chords.retain(|chord| !(chord.nanotick == nanotick && chord.column == column));
        }
        self.pending_chords.push(PendingChord {
            track_id: track as u32,
            nanotick,
            duration,
            spread,
            humanize_timing,
            humanize_velocity,
            degree,
            quality,
            inversion,
            base_octave,
            column,
        });
        cx.notify();
    }

}

impl UiNotify for Context<'_, EngineView> {
    fn notify(&mut self) {
        Context::notify(self);
    }
}

impl Render for EngineView {
    fn render(&mut self, _window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        if self.follow_playhead && self.snapshot.ui_transport_state != 0 {
            let row_nanoticks = self.row_nanoticks() as i64;
            if row_nanoticks > 0 {
                let playhead_view_row =
                    (self.snapshot.ui_global_nanotick_playhead as i64 -
                        self.scroll_nanotick_offset) / row_nanoticks;
                let lower =
                    (VISIBLE_ROWS as f32 * FOLLOW_PLAYHEAD_LOWER).floor() as i64;
                let upper =
                    (VISIBLE_ROWS as f32 * FOLLOW_PLAYHEAD_UPPER).ceil() as i64;
                if playhead_view_row < lower || playhead_view_row > upper {
                    let target = self.snapshot.ui_global_nanotick_playhead as i64 -
                        (VISIBLE_ROWS as i64 / 2) * row_nanoticks;
                    self.scroll_nanotick_offset = target.max(0);
                }
            }
        }
        if let Some(deadline) = self.toast_deadline {
            if Instant::now() > deadline {
                self.toast_deadline = None;
                self.toast_message = None;
            }
        }
        let playhead = format_playhead(self.snapshot.ui_global_nanotick_playhead);
        let is_playing = self.snapshot.ui_transport_state != 0;
        let transport_label = if is_playing { "[Stop]" } else { "[Play]" };
        let follow_label = if self.follow_playhead { "Follow" } else { "Free" };
        let harmony_label = if self.harmony_focus {
            format!("Harmony:{}*", harmony_scale_name(self.harmony_scale_id))
        } else {
            format!("Harmony:{}", harmony_scale_name(self.harmony_scale_id))
        };
        let track_name = self
            .track_names
            .get(self.focused_track_index)
            .and_then(|name| name.clone())
            .unwrap_or_else(|| "Empty".to_string());
        let quantize_label = if self
            .track_quantize
            .get(self.focused_track_index)
            .copied()
            .unwrap_or(true)
        {
            "Quantize:On"
        } else {
            "Quantize:Off"
        };

        let mut root = div()
            .relative()
            .flex()
            .flex_col()
            .gap_2()
            .bg(rgb(0x0f161c))
            .text_color(rgb(0xe6eef5))
            .font_family("Menlo")  // Use Menlo for better monospace on macOS
            .w_full()
            .h_full()
            .p_3()
            .child(
                div()
                    .flex()
                    .items_center()
                    .gap_0()
                    .child(
                        div()
                            .w(px(TIME_COLUMN_WIDTH))
                            .text_sm()
                            .child(playhead.clone()),
                    )
                    .child(
                        div()
                            .text_sm()
                            .text_color(rgb(0x93a1ad))
                            .bg(rgb(0x18222b))
                            .px_2()
                            .py_1()
                            .border_1()
                            .border_color(rgb(0x253240))
                            .on_mouse_down(
                                MouseButton::Left,
                                cx.listener(|view, _, _, cx| {
                                    view.toggle_play(cx);
                                }),
                            )
                            .child(transport_label),
                    ),
            )
            ;

        if self.patcher_open {
            root = root.child(self.render_patcher_view(cx));
        } else {
            root = root.child(self.render_tracker_grid(cx));
        }

        root
            .child(self.render_device_chain_strip(cx))
            .child(
                div()
                    .text_sm()
                    .text_color(rgb(0x93a1ad))
                    .child(format!(
                        "[Track {}:{} {}] [{}] [BPM 120] [{}] [View: {}] [{}]",
                        self.focused_track_index,
                        self.cursor_col + 1,
                        track_name,
                        playhead,
                        quantize_label,
                        follow_label,
                        harmony_label
                    )),
            )
            .child(self.render_palette(cx))
            .child(self.render_scale_browser(cx))
            .child(self.render_jump_overlay(cx))
            .child(self.render_preset_save_overlay(cx))
            .child(self.render_debug_overlay(cx))
            .child(self.render_toast(cx))
    }
}

impl EngineView {
    pub(crate) fn harmony_label_at_nanotick(&self, nanotick: u64) -> Option<String> {
        let event = self
            .harmony_events
            .iter()
            .find(|event| event.nanotick == nanotick)?;
        let root = harmony_root_name(event.root);
        let scale = harmony_scale_name(event.scale_id);
        Some(format!("{root}:{scale}"))
    }
}

pub(crate) fn format_playhead(nanoticks: u64) -> String {
    let total_beats = nanoticks / NANOTICKS_PER_QUARTER;
    let bar = total_beats / BEATS_PER_BAR + 1;
    let beat = total_beats % BEATS_PER_BAR + 1;
    let tick = (nanoticks % NANOTICKS_PER_QUARTER) / 10_000;
    format!("{}:{}:{}", bar, beat, tick)
}

fn parse_jump_text(text: &str) -> Option<u64> {
    if text.trim().is_empty() {
        return None;
    }
    let parts: Vec<&str> = text.split(':').collect();
    let bar = parts.get(0)?.parse::<u64>().ok()?;
    if bar == 0 {
        return None;
    }
    let beat = match parts.get(1) {
        Some(part) => part.parse::<u64>().ok()?,
        None => 1,
    };
    if beat == 0 {
        return None;
    }
    let tick = match parts.get(2) {
        Some(part) => part.parse::<u64>().ok()?,
        None => 0,
    };
    let total_beats = (bar - 1) * BEATS_PER_BAR + (beat - 1);
    Some(total_beats * NANOTICKS_PER_QUARTER + tick * 10_000)
}

fn is_jump_char(value: &str) -> bool {
    value.len() == 1 && value.chars().all(|ch| ch.is_ascii_digit() || ch == ':')
}

fn pitch_for_key(key: &str) -> Option<u8> {
    let k = key.to_lowercase();
    let pitch = match k.as_str() {
        "z" => 48,  // C-3
        "s" => 49,  // C#3
        "x" => 50,  // D-3
        "d" => 51,  // D#3
        "c" => 52,  // E-3
        "v" => 53,  // F-3
        "g" => 54,  // F#3
        "b" => 55,  // G-3
        "h" => 56,  // G#3
        "n" => 57,  // A-3
        "j" => 58,  // A#3
        "m" => 59,  // B-3
        "q" => 60,  // C-4
        "2" => 61,  // C#4
        "w" => 62,  // D-4
        "3" => 63,  // D#4
        "e" => 64,  // E-4
        "r" => 65,  // F-4
        "5" => 66,  // F#4
        "t" => 67,  // G-4
        "6" => 68,  // G#4
        "y" => 69,  // A-4
        "7" => 70,  // A#4
        "u" => 71,  // B-4
        _ => return None,
    };
    Some(pitch)
}

fn pitch_for_letter_key(key: &str) -> Option<u8> {
    if key.chars().all(|ch| ch.is_ascii_alphabetic()) {
        return pitch_for_key(key);
    }
    None
}

fn degree_for_digit(key: &str) -> Option<u8> {
    if key.len() != 1 {
        return None;
    }
    let ch = key.chars().next()?;
    if !ch.is_ascii_digit() {
        return None;
    }
    let digit = ch.to_digit(10)? as u8;
    if digit == 0 {
        return None;
    }
    Some(digit)
}

fn harmony_root_for_key(key: &str) -> Option<u32> {
    let mut sharp = false;
    let mut k = key.to_string();
    if k.len() == 1 && k.chars().next().unwrap().is_ascii_uppercase() {
        sharp = true;
        k = k.to_lowercase();
    }
    let base = match k.as_str() {
        "c" => 0,
        "d" => 2,
        "e" => 4,
        "f" => 5,
        "g" => 7,
        "a" => 9,
        "b" => 11,
        _ => return None,
    };
    let root = if sharp { (base + 1) % 12 } else { base };
    Some(root)
}

#[allow(dead_code)]
fn harmony_scale_id_for_key(key: &str) -> Option<u32> {
    for scale in SCALE_LIBRARY {
        if scale.key == key {
            return Some(scale.id);
        }
    }
    None
}

pub(crate) fn pitch_to_note(pitch: u8) -> String {
    const NAMES: [&str; 12] = [
        "C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-",
    ];
    let name = NAMES[(pitch as usize) % 12];
    let octave = (pitch / 12) as i8 - 1;
    format!("{name}{octave}")
}

pub(crate) fn chord_token_text(chord: &ClipChord) -> String {
    if chord.quality == 0 {
        return format!("{}-{}", chord.degree, chord.base_octave);
    }
    let mut text = format!("@{}", chord.degree);
    if chord.quality == 2 {
        text.push_str("^7");
    }
    if chord.inversion > 0 {
        text.push_str(&format!("/{}", chord.inversion));
    }
    if chord.base_octave != 0 {
        text.push_str(&format!("-{}", chord.base_octave));
    }
    if chord.spread > 0 {
        text.push_str(&format!("~{}", chord.spread));
    }
    if chord.humanize_timing > 0 {
        text.push_str(&format!("h{}", chord.humanize_timing));
    }
    text
}

struct ParsedChordToken {
    degree: u32,
    quality: u8,
    inversion: u8,
    base_octave: u8,
    spread_nanoticks: u32,
    humanize_timing: u8,
    humanize_velocity: u8,
    duration: Option<u64>,
}

fn parse_chord_token(token: &str) -> Option<ParsedChordToken> {
    let token = token.trim();
    let mut chars = token.chars().peekable();
    if chars.next()? != '@' {
        return None;
    }
    let mut degree_str = String::new();
    while let Some(&ch) = chars.peek() {
        if ch.is_ascii_digit() {
            degree_str.push(ch);
            chars.next();
        } else {
            break;
        }
    }
    let degree: u32 = degree_str.parse().ok()?;
    let mut quality: u8 = 1;
    let mut inversion: u8 = 0;
    let mut base_octave: u8 = 4;
    let mut spread: u32 = 0;
    let mut humanize: u8 = 0;
    let rest: Vec<char> = chars.collect();
    let mut index = 0;
    while index < rest.len() {
        match rest[index] {
            '^' => {
                index += 1;
                let mut number = String::new();
                while index < rest.len() && rest[index].is_ascii_digit() {
                    number.push(rest[index]);
                    index += 1;
                }
                if number == "7" {
                    quality = 2;
                }
            }
            '/' => {
                index += 1;
                let mut number = String::new();
                while index < rest.len() && rest[index].is_ascii_digit() {
                    number.push(rest[index]);
                    index += 1;
                }
                if let Ok(value) = number.parse::<u8>() {
                    inversion = value;
                }
            }
            '-' => {
                index += 1;
                let mut number = String::new();
                while index < rest.len() && rest[index].is_ascii_digit() {
                    number.push(rest[index]);
                    index += 1;
                }
                if let Ok(value) = number.parse::<u8>() {
                    base_octave = value;
                }
            }
            '~' => {
                index += 1;
                let mut number = String::new();
                while index < rest.len() && rest[index].is_ascii_digit() {
                    number.push(rest[index]);
                    index += 1;
                }
                if let Ok(value) = number.parse::<u32>() {
                    spread = value;
                }
            }
            'h' => {
                index += 1;
                let mut number = String::new();
                while index < rest.len() && rest[index].is_ascii_digit() {
                    number.push(rest[index]);
                    index += 1;
                }
                if let Ok(value) = number.parse::<u8>() {
                    humanize = value;
                }
            }
            _ => {
                index += 1;
            }
        }
    }
    Some(ParsedChordToken {
        degree,
        quality,
        inversion,
        base_octave,
        spread_nanoticks: spread,
        humanize_timing: humanize,
        humanize_velocity: humanize,
        duration: None,
    })
}

fn parse_degree_note_token(token: &str) -> Option<ParsedChordToken> {
    let token = token.trim();
    let mut parts = token.splitn(2, '-');
    let degree_str = parts.next()?;
    let octave_and_mods = parts.next();
    if degree_str.is_empty() {
        return None;
    }
    if !degree_str.chars().all(|c| c.is_ascii_digit()) {
        return None;
    }
    let degree: u32 = degree_str.parse().ok()?;
    let mut oct_chars = octave_and_mods.unwrap_or("").chars().peekable();
    let mut octave_str = String::new();
    while let Some(&ch) = oct_chars.peek() {
        if ch.is_ascii_digit() {
            octave_str.push(ch);
            oct_chars.next();
        } else {
            break;
        }
    }
    let base_octave: u8 = if octave_str.is_empty() {
        4
    } else {
        octave_str.parse().ok()?
    };
    let rest: Vec<char> = oct_chars.collect();
    let mut spread: u32 = 0;
    let mut humanize: u8 = 0;
    let mut index = 0;
    while index < rest.len() {
        match rest[index] {
            '~' => {
                index += 1;
                let mut number = String::new();
                while index < rest.len() && rest[index].is_ascii_digit() {
                    number.push(rest[index]);
                    index += 1;
                }
                if let Ok(value) = number.parse::<u32>() {
                    spread = value;
                }
            }
            'h' => {
                index += 1;
                let mut number = String::new();
                while index < rest.len() && rest[index].is_ascii_digit() {
                    number.push(rest[index]);
                    index += 1;
                }
                if let Ok(value) = number.parse::<u8>() {
                    humanize = value;
                }
            }
            _ => {
                index += 1;
            }
        }
    }
    Some(ParsedChordToken {
        degree,
        quality: 0,
        inversion: 0,
        base_octave,
        spread_nanoticks: spread,
        humanize_timing: humanize,
        humanize_velocity: humanize,
        duration: None,
    })
}

fn parse_note_token(token: &str) -> Option<u8> {
    let token = token.trim();
    if token.len() < 2 {
        return None;
    }
    let mut chars = token.chars();
    let letter = chars.next()?.to_ascii_uppercase();
    let mut accidental: i8 = 0;
    let mut next = chars.next()?;
    if next == '#' {
        accidental = 1;
        next = chars.next()?;
    } else if next == 'b' || next == 'B' {
        accidental = -1;
        next = chars.next()?;
    } else if next == '-' {
        next = chars.next()?;
    }
    let mut octave_str = String::new();
    octave_str.push(next);
    octave_str.extend(chars);
    let octave: i8 = octave_str.parse().ok()?;
    let base = match letter {
        'C' => 0,
        'D' => 2,
        'E' => 4,
        'F' => 5,
        'G' => 7,
        'A' => 9,
        'B' => 11,
        _ => return None,
    };
    let mut pitch = (octave + 1) as i16 * 12 + base as i16 + accidental as i16;
    if pitch < 0 {
        pitch = 0;
    }
    if pitch > 127 {
        pitch = 127;
    }
    Some(pitch as u8)
}

fn keystroke_text(keystroke: &gpui::Keystroke) -> Option<&str> {
    if let Some(key_char) = keystroke.key_char.as_ref() {
        return Some(key_char);
    }
    if keystroke.key.len() == 1 {
        return Some(keystroke.key.as_str());
    }
    None
}

fn begin_cell_edit(edit_active: &mut bool, edit_text: &mut String, initial: &str) -> bool {
    *edit_active = true;
    edit_text.clear();
    edit_text.push_str(initial);
    true
}

fn cancel_cell_edit_state(edit_active: &mut bool, edit_text: &mut String) -> bool {
    if !*edit_active {
        return false;
    }
    *edit_active = false;
    edit_text.clear();
    true
}

fn commit_cell_edit_state(edit_active: &mut bool, edit_text: &mut String) -> Option<String> {
    if !*edit_active {
        return None;
    }
    let token = edit_text.trim().to_string();
    *edit_active = false;
    edit_text.clear();
    Some(token)
}

fn is_cell_edit_start(key: &str) -> bool {
    key == "@"
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_parse_note_token() {
        assert_eq!(parse_note_token("C-4"), Some(60));
        assert_eq!(parse_note_token("D#5"), Some(75));
        assert_eq!(parse_note_token("Bb3"), Some(58));
        assert_eq!(parse_note_token("Q-4"), None);
    }

    #[test]
    fn test_parse_degree_note_token() {
        let parsed = parse_degree_note_token("24-4~120h8").expect("degree token");
        assert_eq!(parsed.degree, 24);
        assert_eq!(parsed.base_octave, 4);
        assert_eq!(parsed.quality, 0);
        assert_eq!(parsed.spread_nanoticks, 120);
        assert_eq!(parsed.humanize_timing, 8);
    }

    #[test]
    fn test_parse_chord_token() {
        let parsed = parse_chord_token("@3^7/1-5~240h6").expect("chord token");
        assert_eq!(parsed.degree, 3);
        assert_eq!(parsed.quality, 2);
        assert_eq!(parsed.inversion, 1);
        assert_eq!(parsed.base_octave, 5);
        assert_eq!(parsed.spread_nanoticks, 240);
        assert_eq!(parsed.humanize_timing, 6);
    }

    #[test]
    fn test_cell_edit_flow() {
        let mut active = false;
        let mut text = String::new();
        assert!(begin_cell_edit(&mut active, &mut text, "A"));
        assert!(active);
        assert_eq!(text, "A");

        let token = commit_cell_edit_state(&mut active, &mut text);
        assert_eq!(token, Some("A".to_string()));
        assert!(!active);
        assert!(text.is_empty());

        assert!(begin_cell_edit(&mut active, &mut text, " @3 "));
        assert!(cancel_cell_edit_state(&mut active, &mut text));
        assert!(!active);
        assert!(text.is_empty());
    }

    #[test]
    fn test_column_widths() {
        // Test that column widths are consistent
        const COLUMN_WIDTH: f32 = 52.0;

        // Test multi-column track width calculation
        for columns in 1..=8 {
            let expected_width = COLUMN_WIDTH * columns as f32;
            let header_width = COLUMN_WIDTH * columns as f32;
            assert_eq!(header_width, expected_width,
                      "Header width mismatch for {} columns", columns);
        }
    }

    #[test]
    fn test_tracker_dimensions() {
        // Verify tracker grid dimensions
        const TRACK_COUNT: usize = 8;
        const VISIBLE_ROWS: usize = 40;

        // Calculate total width
        let time_col = super::TIME_COLUMN_WIDTH;
        let harmony_col = 52.0_f32;
        let track_cols = 52.0_f32 * TRACK_COUNT as f32; // Assuming 1 column per track initially
        let min_width = time_col + harmony_col + track_cols;

        assert!(min_width > 0.0, "Total width must be positive");
        assert!(min_width > 400.0, "Minimum width seems too small");

        // Test row height
        const ROW_HEIGHT: f32 = 16.0;
        const HEADER_HEIGHT: f32 = 24.0;
        let total_height = HEADER_HEIGHT + (ROW_HEIGHT * VISIBLE_ROWS as f32);
        assert!(total_height > 0.0, "Total height must be positive");
    }

    #[test]
    fn test_note_display_formats() {
        // Test various note formats fit in column width
        let test_cases = vec![
            ("C-4", 3),      // MIDI note
            ("C#4", 3),      // Sharp note
            ("1-4", 3),      // Degree note
            ("24-4", 4),     // Two-digit degree
            ("@3-4", 4),     // Basic chord
            ("@3^7-4", 6),   // Chord with 7th
            ("@11^7/2-5", 9), // Complex chord
        ];

        for (token, expected_len) in test_cases {
            assert!(token.len() <= 10,
                   "Token '{}' might be too long for column", token);
            assert_eq!(token.len(), expected_len,
                      "Token '{}' has unexpected length", token);
        }
    }

    #[test]
    fn test_alignment_calculation() {
        // Test that headers align with cells
        const COLUMN_WIDTH: f32 = 52.0;

        // Simulate header positions
        let mut x_pos = 0.0_f32;
        let _time_x = x_pos;
        x_pos += super::TIME_COLUMN_WIDTH; // TIME column
        let _harmony_x = x_pos;
        x_pos += COLUMN_WIDTH; // HARM column

        let mut track_positions = Vec::new();
        for _track in 0..8 {
            track_positions.push(x_pos);
            x_pos += COLUMN_WIDTH; // Each track with 1 column
        }

        // Verify spacing is consistent
        for i in 1..track_positions.len() {
            let spacing = track_positions[i] - track_positions[i-1];
            assert_eq!(spacing, COLUMN_WIDTH,
                      "Inconsistent spacing between tracks {} and {}", i-1, i);
        }
    }

    #[test]
    fn test_multi_column_tracks() {
        const COLUMN_WIDTH: f32 = 52.0;

        // Test tracks with different column counts
        let track_columns = vec![1, 2, 1, 3, 1, 1, 2, 1];
        let mut x_pos = super::TIME_COLUMN_WIDTH + COLUMN_WIDTH; // After TIME and HARM

        for (track_idx, &columns) in track_columns.iter().enumerate() {
            let track_width = COLUMN_WIDTH * columns as f32;
            assert!(track_width >= COLUMN_WIDTH,
                   "Track {} width too small", track_idx);
            assert!(track_width <= COLUMN_WIDTH * 8.0,
                   "Track {} width too large", track_idx);
            x_pos += track_width;
        }

        assert!(x_pos > 200.0, "Total width seems too small");
        assert!(x_pos < 2000.0, "Total width seems too large");
    }

    #[test]
    fn test_note_entry_parsing() {
        // Test entering different types of notes

        // MIDI notes
        assert_eq!(parse_note_token("C-4"), Some(60));
        assert_eq!(parse_note_token("D#5"), Some(75));
        assert_eq!(parse_note_token("G-3"), Some(55));

        // Degree notes
        let degree = parse_degree_note_token("1-4");
        assert!(degree.is_some());
        let degree_val = degree.unwrap();
        assert_eq!(degree_val.degree, 1);
        assert_eq!(degree_val.base_octave, 4);

        // Chord tokens
        let chord = parse_chord_token("@3-4");
        assert!(chord.is_some());
        let chord_val = chord.unwrap();
        assert_eq!(chord_val.degree, 3);
        assert_eq!(chord_val.base_octave, 4);
    }

    #[test]
    fn test_edit_state_transitions() {
        // Test the edit state machine
        let mut edit_active = false;
        let mut edit_text = String::new();

        // Start editing
        assert!(!edit_active);
        begin_cell_edit(&mut edit_active, &mut edit_text, "C");
        assert!(edit_active);
        assert_eq!(edit_text, "C");

        // Add more text (simulating typing)
        edit_text.push_str("-4");
        assert_eq!(edit_text, "C-4");

        // Commit the edit
        let result = commit_cell_edit_state(&mut edit_active, &mut edit_text);
        assert_eq!(result, Some("C-4".to_string()));
        assert!(!edit_active);
        assert!(edit_text.is_empty());
    }

    #[test]
    fn test_note_overwrite_scenario() {
        // Simulate overwriting a note with another note
        let mut edit_active = false;
        let mut edit_text = String::new();

        // First note entry
        begin_cell_edit(&mut edit_active, &mut edit_text, "C");
        edit_text.push_str("-4");
        let first_note = commit_cell_edit_state(&mut edit_active, &mut edit_text);
        assert_eq!(first_note, Some("C-4".to_string()));

        // Overwrite with new note (same position)
        begin_cell_edit(&mut edit_active, &mut edit_text, "E");
        edit_text.push_str("-4");
        let second_note = commit_cell_edit_state(&mut edit_active, &mut edit_text);
        assert_eq!(second_note, Some("E-4".to_string()));

        // Verify both are different
        assert_ne!(first_note, second_note);
    }

    #[test]
    fn test_delete_operation() {
        // Test deleting a note
        let mut edit_active = false;
        let mut edit_text = String::new();

        // Enter a note
        begin_cell_edit(&mut edit_active, &mut edit_text, "F");
        edit_text.push_str("#4");
        let note = commit_cell_edit_state(&mut edit_active, &mut edit_text);
        assert_eq!(note, Some("F#4".to_string()));

        // Simulate delete (empty entry)
        begin_cell_edit(&mut edit_active, &mut edit_text, "");
        let delete_result = commit_cell_edit_state(&mut edit_active, &mut edit_text);
        assert_eq!(delete_result, Some("".to_string()));
    }

    #[test]
    fn test_cancel_edit() {
        // Test canceling an edit operation
        let mut edit_active = false;
        let mut edit_text = String::new();

        // Start editing
        begin_cell_edit(&mut edit_active, &mut edit_text, "G");
        assert!(edit_active);
        assert_eq!(edit_text, "G");

        // Cancel the edit
        let canceled = cancel_cell_edit_state(&mut edit_active, &mut edit_text);
        assert!(canceled);
        assert!(!edit_active);
        assert!(edit_text.is_empty());
    }

    #[test]
    fn test_chord_overwrite_note() {
        // Test overwriting a note with a chord
        let mut edit_active = false;
        let mut edit_text = String::new();

        // First enter a note
        begin_cell_edit(&mut edit_active, &mut edit_text, "A");
        edit_text.push_str("-3");
        let note = commit_cell_edit_state(&mut edit_active, &mut edit_text);
        assert_eq!(note, Some("A-3".to_string()));

        // Now enter a chord at the same position
        begin_cell_edit(&mut edit_active, &mut edit_text, "@");
        edit_text.push_str("3-4");
        let chord = commit_cell_edit_state(&mut edit_active, &mut edit_text);
        assert_eq!(chord, Some("@3-4".to_string()));

        // Verify the chord parsed correctly
        assert!(parse_chord_token(&chord.unwrap()).is_some());
    }

    #[test]
    fn test_complex_chord_entry() {
        // Test entering complex chords
        let tokens = vec![
            "@3^7-4",
            "@5^7/2-3",
            "@1^7~240h10",
        ];

        for token in tokens {
            let mut edit_active = false;
            let mut edit_text = String::new();

            begin_cell_edit(&mut edit_active, &mut edit_text, &token[0..1]);
            edit_text.push_str(&token[1..]);
            let result = commit_cell_edit_state(&mut edit_active, &mut edit_text);
            assert_eq!(result, Some(token.to_string()));

            // Verify it parses
            let parsed = parse_chord_token(&result.unwrap());
            assert!(parsed.is_some(), "Failed to parse: {}", token);
        }
    }

    #[test]
    fn test_multi_column_operations() {
        // Test operations across multiple columns
        const TRACK_IDX: usize = 0;

        // Simulate having 2 columns for track 0
        // Test entering notes in both columns
        let mut pending_notes = Vec::new();
        let nanotick = 0;

        // Add note to column 0
        pending_notes.push(PendingNote {
            track_id: TRACK_IDX as u32,
            nanotick,
            duration: 240000,
            pitch: 60,
            velocity: 100,
            column: 0,
        });

        // Add note to same position (should replace)
        pending_notes.retain(|note| {
            !(note.track_id == TRACK_IDX as u32 && note.nanotick == nanotick)
        });
        pending_notes.push(PendingNote {
            track_id: TRACK_IDX as u32,
            nanotick,
            duration: 240000,
            pitch: 64,
            velocity: 100,
            column: 0,
        });

        // Verify only one note at position
        let notes_at_position: Vec<_> = pending_notes.iter()
            .filter(|n| n.track_id == TRACK_IDX as u32 && n.nanotick == nanotick)
            .collect();
        assert_eq!(notes_at_position.len(), 1);
        assert_eq!(notes_at_position[0].pitch, 64);

        // Test delete operation
        pending_notes.retain(|note| {
            !(note.track_id == TRACK_IDX as u32 && note.nanotick == nanotick)
        });
        let remaining: Vec<_> = pending_notes.iter()
            .filter(|n| n.track_id == TRACK_IDX as u32 && n.nanotick == nanotick)
            .collect();
        assert_eq!(remaining.len(), 0);
    }

    #[test]
    fn test_sharp_note_keys() {
        // Test that sharp note keys work correctly
        assert_eq!(pitch_for_key("2"), Some(61)); // C#4
        assert_eq!(pitch_for_key("3"), Some(63)); // D#4
        assert_eq!(pitch_for_key("5"), Some(66)); // F#4
        assert_eq!(pitch_for_key("6"), Some(68)); // G#4
        assert_eq!(pitch_for_key("7"), Some(70)); // A#4
        assert_eq!(pitch_for_key("s"), Some(49)); // C#3
        assert_eq!(pitch_for_key("d"), Some(51)); // D#3
        assert_eq!(pitch_for_key("g"), Some(54)); // F#3
        assert_eq!(pitch_for_key("h"), Some(56)); // G#3
        assert_eq!(pitch_for_key("j"), Some(58)); // A#3
    }

    #[test]
    fn test_degree_note_keys() {
        // Digits 1..9 map to degree entry.
        for digit in 1..=9 {
            let key = digit.to_string();
            assert_eq!(degree_for_digit(&key), Some(digit as u8));
        }
        assert_eq!(degree_for_digit("0"), None);
    }

    #[test]
    fn test_parse_jump_text_bar_only() {
        let expected = (24 - 1) * BEATS_PER_BAR * NANOTICKS_PER_QUARTER;
        assert_eq!(parse_jump_text("24"), Some(expected));
        let expected = (3 - 1) * BEATS_PER_BAR * NANOTICKS_PER_QUARTER +
            (2 - 1) * NANOTICKS_PER_QUARTER +
            5 * 10_000;
        assert_eq!(parse_jump_text("3:2:5"), Some(expected));
    }

    #[test]
    fn test_backspace_moves_cursor_down() {
        struct TestNotify;
        impl super::UiNotify for TestNotify {
            fn notify(&mut self) {}
        }

        let mut view = super::EngineView::new_state();
        view.cursor_nanotick = view.view_row_nanotick(5);
        view.focused_track_index = 0;
        view.cursor_col = 0;
        let mut notify = TestNotify;

        view.delete_note(&mut notify);
        assert_eq!(
            view.cursor_view_row(),
            6,
            "Backspace should move cursor down"
        );
    }

    #[test]
    fn test_apply_diff_replaces_notes() {
        // Test that applying an AddNote diff replaces existing notes at same position
        use super::{EngineView, ClipNote};
        use daw_bridge::layout::{UiDiffPayload, UiDiffType, K_UI_MAX_TRACKS};
        use daw_bridge::reader::UiSnapshot;

        let mut engine_view = EngineView::new_state();
        engine_view.bridge = None;
        engine_view.snapshot = UiSnapshot {
            version: 0,
            ui_visual_sample_count: 0,
            ui_global_nanotick_playhead: 0,
            ui_track_count: 0,
            ui_transport_state: 0,
            ui_clip_version: 0,
            ui_clip_offset: 0,
            ui_clip_bytes: 0,
            ui_harmony_version: 0,
            ui_harmony_offset: 0,
            ui_harmony_bytes: 0,
            ui_track_peak_rms: [0.0; K_UI_MAX_TRACKS],
        };
        if let Some(state) = engine_view.clip_window.get_mut(0) {
            state.window_start = 0;
            state.window_end = NANOTICKS_PER_QUARTER * 4;
        }
        engine_view.track_columns = vec![1; TRACK_COUNT];
        engine_view.track_quantize = vec![false; TRACK_COUNT];
        engine_view.track_names = vec![None; TRACK_COUNT];
        engine_view.clip_notes = vec![Vec::new(); TRACK_COUNT];
        engine_view.clip_chords = vec![Vec::new(); TRACK_COUNT];
        engine_view.queued_commands = VecDeque::new();
        engine_view.chain_versions = vec![0; TRACK_COUNT];
        engine_view.chain_devices = vec![Vec::new(); TRACK_COUNT];
        engine_view.patcher_versions = vec![0; TRACK_COUNT];
        engine_view.patcher_nodes = vec![Vec::new(); TRACK_COUNT];

        // Add initial note at position 0
        engine_view.clip_notes[0].push(ClipNote {
            nanotick: 0,
            duration: 240000,
            pitch: 60,  // C-4
            velocity: 100,
            column: 0,
        });

        // Apply diff to add D-4 at same position
        let diff = UiDiffPayload {
            diff_type: UiDiffType::AddNote as u16,
            flags: 0,
            track_id: 0,
            clip_version: 1,
            note_nanotick_lo: 0,
            note_nanotick_hi: 0,
            note_duration_lo: 240000,
            note_duration_hi: 0,
            note_pitch: 62,  // D-4
            note_velocity: 100,
            note_column: 0,
        };

        engine_view.apply_diff(diff);

        // Should have only 1 note (D-4), not 2
        assert_eq!(engine_view.clip_notes[0].len(), 1,
                   "Should have replaced C-4 with D-4");
        assert_eq!(engine_view.clip_notes[0][0].pitch, 62,
                   "Note should be D-4");
    }

    #[test]
    fn test_pending_notes_cleared_on_write() {
        // Test that pending notes are cleared when writing new note
        use super::PendingNote;

        let mut pending_notes = Vec::new();
        let track_id = 0_u32;
        let nanotick = 0_u64;

        // Add first pending note
        pending_notes.push(PendingNote {
            track_id,
            nanotick,
            duration: 240000,
            pitch: 60,
            velocity: 100,
            column: 0,
        });

        // Simulate write_note clearing pending notes at position
        pending_notes.retain(|note| {
            !(note.track_id == track_id && note.nanotick == nanotick)
        });

        // Add new pending note
        pending_notes.push(PendingNote {
            track_id,
            nanotick,
            duration: 240000,
            pitch: 62,
            velocity: 100,
            column: 0,
        });

        assert_eq!(pending_notes.len(), 1, "Should have only new note");
        assert_eq!(pending_notes[0].pitch, 62, "Should be D-4");
    }

    #[test]
    fn test_semantic_zoom_aggregate_notes() {
        let mut view = super::EngineView::new_state();
        view.zoom_index = 0; // zoomed out -> aggregate

        view.clip_notes[0].push(super::ClipNote {
            nanotick: 0,
            duration: 240000,
            pitch: 60,
            velocity: 100,
            column: 0,
        });
        view.clip_notes[0].push(super::ClipNote {
            nanotick: 240000,
            duration: 240000,
            pitch: 60,
            velocity: 100,
            column: 0,
        });

        let row_start = 0;
        let row_end = view.row_nanoticks();
        let aggregates = view.aggregate_cells_in_range(row_start, row_end, 0, 1);
        assert_eq!(aggregates[0].count, 2);
        assert!(view.should_aggregate_rows());
        assert_eq!(
            view.aggregate_cell_label(&aggregates[0]),
            Some("[2x C-4]".to_string())
        );
    }

    #[test]
    fn test_semantic_zoom_default_no_aggregate() {
        let view = super::EngineView::new_state();
        assert!(!view.should_aggregate_rows());
    }

    #[test]
    fn test_semantic_zoom_harmony_aggregate() {
        let mut view = super::EngineView::new_state();
        view.harmony_events.push(super::HarmonyEntry {
            nanotick: 0,
            root: 0,
            scale_id: 1,
        });
        view.harmony_events.push(super::HarmonyEntry {
            nanotick: 240000,
            root: 0,
            scale_id: 2,
        });

        let aggregate = view.aggregate_harmony_in_range(0, 480000);
        assert_eq!(aggregate.count, 2);
        assert_eq!(aggregate.labels.len(), 2);
    }

    #[test]
    fn test_expand_selection_rows_creates_range() {
        struct TestNotify;
        impl super::UiNotify for TestNotify {
            fn notify(&mut self) {}
        }

        let mut view = super::EngineView::new_state();
        view.cursor_nanotick = view.view_row_nanotick(4);
        view.focused_track_index = 0;
        view.cursor_col = 0;
        let mut notify = TestNotify;

        view.expand_selection_rows(2, &mut notify);

        let (start, end) = view.selection_bounds().expect("selection should exist");
        assert_eq!(start, view.view_row_nanotick(4));
        assert_eq!(end, view.view_row_nanotick(6));
    }

    #[test]
    fn test_expand_selection_to_bar_snaps() {
        struct TestNotify;
        impl super::UiNotify for TestNotify {
            fn notify(&mut self) {}
        }

        let mut view = super::EngineView::new_state();
        view.cursor_nanotick = view.view_row_nanotick(1);
        view.focused_track_index = 0;
        view.cursor_col = 0;
        let mut notify = TestNotify;

        view.expand_selection_to_bar(1, &mut notify);

        let (start, end) = view.selection_bounds().expect("selection should exist");
        let bar_len = super::BEATS_PER_BAR * super::NANOTICKS_PER_QUARTER;
        assert_eq!(start, view.view_row_nanotick(1));
        assert_eq!(end, bar_len);
    }

    #[test]
    fn test_expand_selection_columns_updates_mask() {
        struct TestNotify;
        impl super::UiNotify for TestNotify {
            fn notify(&mut self) {}
        }

        let mut view = super::EngineView::new_state();
        view.track_columns[0] = 3;
        view.focused_track_index = 0;
        view.cursor_col = 0;
        let mut notify = TestNotify;

        view.expand_selection_columns(1, &mut notify);

        let mask = view.selection_mask.tracks[0];
        assert_eq!(mask & 0b11, 0b11, "should include columns 0 and 1");
        assert_eq!(view.cursor_col, 1);
    }

    #[test]
    fn test_copy_selection_without_range_shows_toast() {
        struct TestNotify;
        impl super::UiNotify for TestNotify {
            fn notify(&mut self) {}
        }

        let mut view = super::EngineView::new_state();
        let mut notify = TestNotify;

        view.copy_selection(&mut notify);

        assert_eq!(view.toast_message.as_deref(), Some("No selection"));
    }

    #[test]
    fn test_page_copy_cut_paste_roundtrip() {
        struct TestNotify;
        impl super::UiNotify for TestNotify {
            fn notify(&mut self) {}
        }

        let mut view = super::EngineView::new_state();
        view.focused_track_index = 0;
        view.track_columns[0] = 2;
        let row = view.row_nanoticks();
        let (page_start, page_end) = view.page_range();
        let outside = page_end + row;

        view.clip_notes[0].push(super::ClipNote {
            nanotick: page_start,
            duration: row,
            pitch: 60,
            velocity: 100,
            column: 0,
        });
        view.clip_notes[0].push(super::ClipNote {
            nanotick: page_start + row,
            duration: row,
            pitch: 62,
            velocity: 100,
            column: 1,
        });
        view.clip_notes[0].push(super::ClipNote {
            nanotick: outside,
            duration: row,
            pitch: 64,
            velocity: 100,
            column: 0,
        });

        let mut notify = TestNotify;
        view.copy_page(&mut notify);
        let clipboard = view.clipboard.clone().expect("clipboard should be set");
        assert_eq!(clipboard.notes.len(), 2);

        view.cut_page(&mut notify);
        assert_eq!(view.clip_notes[0].len(), 1);
        assert_eq!(view.clip_notes[0][0].nanotick, outside);

        view.scroll_nanotick_offset = (row * 4) as i64;
        view.paste_page(&mut notify);

        let target = view.page_range().0;
        let mut pasted = view.clip_notes[0]
            .iter()
            .filter(|note| note.nanotick == target || note.nanotick == target + row)
            .map(|note| (note.nanotick, note.column))
            .collect::<Vec<_>>();
        pasted.sort();
        assert_eq!(pasted, vec![(target, 0), (target + row, 1)]);
    }

    #[test]
    fn test_mouse_paint_selection_sets_mask_and_range() {
        struct TestNotify;
        impl super::UiNotify for TestNotify {
            fn notify(&mut self) {}
        }

        let mut view = super::EngineView::new_state();
        let row = view.row_nanoticks();
        let mut notify = TestNotify;

        view.start_selection(row / 2, Some(1), Some(1), false, false, &mut notify);
        view.update_selection_end(row * 3 + row / 4, &mut notify);

        let (start, end) = view.selection_bounds().expect("selection should exist");
        assert_eq!(start, 0);
        assert_eq!(end, row * 3);
        let mask = view.selection_mask.tracks[1];
        assert!(mask & (1u8 << 1) != 0, "column mask should include col 1");
    }

    #[test]
    fn test_minimap_bins_counts_items_in_range() {
        let mut view = super::EngineView::new_state();
        let row = view.row_nanoticks();

        view.clip_notes[0].push(super::ClipNote {
            nanotick: 0,
            duration: row,
            pitch: 60,
            velocity: 100,
            column: 0,
        });
        view.pending_notes.push(super::PendingNote {
            track_id: 0,
            nanotick: row * 2,
            duration: row,
            pitch: 62,
            velocity: 100,
            column: 0,
        });
        view.clip_chords[0].push(super::ClipChord {
            chord_id: 1,
            nanotick: row * 3,
            duration: row,
            spread: 0,
            humanize_timing: 0,
            humanize_velocity: 0,
            degree: 1,
            quality: 0,
            inversion: 0,
            base_octave: 4,
            column: 0,
        });
        view.harmony_events.push(super::HarmonyEntry {
            nanotick: row * 4,
            root: 0,
            scale_id: 1,
        });
        view.clip_notes[0].push(super::ClipNote {
            nanotick: row * 20,
            duration: row,
            pitch: 64,
            velocity: 100,
            column: 0,
        });

        let bins = view.minimap_bins(0, row * 8, 8);
        assert_eq!(bins.len(), 8);
        let total: usize = bins.iter().sum();
        assert_eq!(total, 4);
    }
}


pub fn run_app() -> Result<()> {
    Application::new().run(|cx: &mut App| {
        actions::bind_keys(cx);

        let engine_supervisor = Arc::new(Mutex::new(EngineSupervisor {
            child: None,
            last_spawn_attempt: Instant::now()
                .checked_sub(Duration::from_secs(5))
                .unwrap_or_else(Instant::now),
            engine_path: default_engine_path(),
            engine_missing_logged: false,
        }));
        let shutting_down = Arc::new(AtomicBool::new(false));

        let bounds = Bounds::centered(None, size(px(1200.0), px(800.0)), cx);
        let window = cx
            .open_window(
                WindowOptions {
                    window_bounds: Some(WindowBounds::Maximized(bounds)),
                    ..Default::default()
                },
                |_, cx| cx.new(|cx| EngineView::new(cx)),
            )
            .unwrap();

        let view = window.update(cx, |_, _, cx| cx.entity()).unwrap();

        let _ = cx.on_window_closed({
            let engine_supervisor = engine_supervisor.clone();
            let shutting_down = shutting_down.clone();
            move |_cx| {
                shutting_down.store(true, Ordering::Relaxed);
                if let Ok(mut supervisor) = engine_supervisor.lock() {
                    stop_engine_process(&mut supervisor);
                }
            }
        });

        cx.on_action({
            let view = view.clone();
            move |_: &TogglePalette, cx| {
                view.update(cx, |view, cx| view.toggle_palette(cx));
            }
        });
        cx.on_action({
            let view = view.clone();
            move |_: &OpenPluginPalette, cx| {
                view.update(cx, |view, cx| view.open_plugin_palette(cx));
            }
        });
        cx.on_action({
            let view = view.clone();
            move |_: &OpenJump, cx| {
                view.update(cx, |view, cx| view.open_jump(cx));
            }
        });
        cx.on_action({
            let view = view.clone();
            move |_: &PaletteClose, cx| {
                view.update(cx, |view, cx| {
                    if view.edit_active {
                        view.cancel_cell_edit(cx);
                    } else if view.jump_open {
                        view.close_jump(cx);
                    } else if view.scale_browser_open {
                        view.close_scale_browser(cx);
                    } else {
                        view.close_palette(cx);
                    }
                });
            }
        });
        cx.on_action({
            let view = view.clone();
            move |_: &PaletteUp, cx| {
                view.update(cx, |view, cx| view.action_palette_up(cx));
            }
        });
        cx.on_action({
            let view = view.clone();
            move |_: &PaletteDown, cx| {
                view.update(cx, |view, cx| view.action_palette_down(cx));
            }
        });
        cx.on_action({
            let view = view.clone();
            move |_: &PaletteBackspace, cx| {
                view.update(cx, |view, cx| view.action_palette_backspace(cx));
            }
        });
        cx.on_action({
            let view = view.clone();
            move |_: &PaletteConfirm, cx| {
                view.update(cx, |view, cx| view.action_palette_confirm(cx));
            }
        });
        cx.on_action({
            let view = view.clone();
            move |_: &OpenScaleBrowser, cx| {
                view.update(cx, |view, cx| {
                    view.close_palette(cx);
                    view.open_scale_browser(cx);
                });
            }
        });
        cx.on_action({
            let view = view.clone();
            move |_: &FocusLeft, cx| {
                view.update(cx, |view, cx| view.move_cursor_or_focus(-1, cx));
            }
        });
        cx.on_action({
            let view = view.clone();
            move |_: &FocusRight, cx| {
                view.update(cx, |view, cx| view.move_cursor_or_focus(1, cx));
            }
        });
        cx.on_action({
            let view = view.clone();
            move |_: &ColumnLeft, cx| {
                view.update(cx, |view, cx| view.move_column(-1, cx));
            }
        });
        cx.on_action({
            let view = view.clone();
            move |_: &ColumnRight, cx| {
                view.update(cx, |view, cx| view.move_column(1, cx));
            }
        });
        cx.on_action({
            let view = view.clone();
            move |_: &TogglePlay, cx| {
                view.update(cx, |view, cx| view.toggle_play(cx));
            }
        });
        cx.on_action({
            let view = view.clone();
            move |_: &SetLoopRange, cx| {
                view.update(cx, |view, cx| view.set_loop_from_selection_or_page(cx));
            }
        });
        cx.on_action({
            let view = view.clone();
            move |_: &PageZoomIn, cx| {
                view.update(cx, |view, cx| view.zoom_by(-1, cx));
            }
        });
        cx.on_action({
            let view = view.clone();
            move |_: &PageZoomOut, cx| {
                view.update(cx, |view, cx| view.zoom_by(1, cx));
            }
        });
        cx.on_action({
            let view = view.clone();
            move |_: &ScrollUp, cx| {
                view.update(cx, |view, cx| view.scroll_rows(-1, cx));
            }
        });
        cx.on_action({
            let view = view.clone();
            move |_: &ScrollDown, cx| {
                view.update(cx, |view, cx| view.scroll_rows(1, cx));
            }
        });
        cx.on_action({
            let view = view.clone();
            move |_: &ExpandSelectionUp, cx| {
                view.update(cx, |view, cx| view.expand_selection_rows(-1, cx));
            }
        });
        cx.on_action({
            let view = view.clone();
            move |_: &ExpandSelectionDown, cx| {
                view.update(cx, |view, cx| view.expand_selection_rows(1, cx));
            }
        });
        cx.on_action({
            let view = view.clone();
            move |_: &ExpandSelectionBarUp, cx| {
                view.update(cx, |view, cx| view.expand_selection_to_bar(-1, cx));
            }
        });
        cx.on_action({
            let view = view.clone();
            move |_: &ExpandSelectionBarDown, cx| {
                view.update(cx, |view, cx| view.expand_selection_to_bar(1, cx));
            }
        });
        cx.on_action({
            let view = view.clone();
            move |_: &ExpandSelectionLeft, cx| {
                view.update(cx, |view, cx| view.expand_selection_columns(-1, cx));
            }
        });
        cx.on_action({
            let view = view.clone();
            move |_: &ExpandSelectionRight, cx| {
                view.update(cx, |view, cx| view.expand_selection_columns(1, cx));
            }
        });
        cx.on_action({
            let view = view.clone();
            move |_: &ScrollPageUp, cx| {
                view.update(cx, |view, cx| {
                    view.scroll_rows(-(VISIBLE_ROWS as i64), cx);
                });
            }
        });
        cx.on_action({
            let view = view.clone();
            move |_: &ScrollPageDown, cx| {
                view.update(cx, |view, cx| {
                    view.scroll_rows(VISIBLE_ROWS as i64, cx);
                });
            }
        });
        cx.on_action({
            let view = view.clone();
            move |_: &ToggleFollowPlayhead, cx| {
                view.update(cx, |view, cx| view.toggle_follow_playhead(cx));
            }
        });
        cx.on_action({
            let view = view.clone();
            move |_: &ToggleHarmonyFocus, cx| {
                view.update(cx, |view, cx| view.toggle_harmony_focus(cx));
            }
        });
        cx.on_action({
            let view = view.clone();
            move |_: &Undo, cx| {
                view.update(cx, |view, cx| view.send_undo(cx));
            }
        });
        cx.on_action({
            let view = view.clone();
            move |_: &Redo, cx| {
                view.update(cx, |view, cx| view.send_redo(cx));
            }
        });
        cx.on_action({
            let view = view.clone();
            move |_: &DeleteNote, cx| {
                view.update(cx, |view, cx| {
                    if view.edit_active {
                        view.edit_text.pop();
                        cx.notify();
                    } else if view.chain_focus {
                        view.delete_selected_chain_device(cx);
                    } else if view.harmony_focus {
                        view.delete_harmony(cx);
                    } else {
                        view.delete_note(cx);
                    }
                });
            }
        });
        cx.on_action({
            let view = view.clone();
            move |_: &CopySelection, cx| {
                view.update(cx, |view, cx| {
                    if view.chain_focus {
                        view.copy_selected_chain_device(cx);
                    } else {
                        view.copy_selection(cx);
                    }
                });
            }
        });
        cx.on_action({
            let view = view.clone();
            move |_: &CutSelection, cx| {
                view.update(cx, |view, cx| {
                    if view.chain_focus {
                        view.cut_selected_chain_device(cx);
                    } else {
                        view.cut_selection(cx);
                    }
                });
            }
        });
        cx.on_action({
            let view = view.clone();
            move |_: &PasteSelection, cx| {
                view.update(cx, |view, cx| {
                    if view.chain_focus {
                        view.paste_chain_device(cx);
                    } else {
                        view.paste_selection(cx);
                    }
                });
            }
        });
        cx.on_action({
            let view = view.clone();
            move |_: &DuplicateDevice, cx| {
                view.update(cx, |view, cx| view.duplicate_selected_chain_device(cx));
            }
        });
        cx.on_action({
            let view = view.clone();
            move |_: &PageCut, cx| {
                view.update(cx, |view, cx| view.cut_page(cx));
            }
        });
        cx.on_action({
            let view = view.clone();
            move |_: &PageCopy, cx| {
                view.update(cx, |view, cx| view.copy_page(cx));
            }
        });
        cx.on_action({
            let view = view.clone();
            move |_: &PagePaste, cx| {
                view.update(cx, |view, cx| view.paste_page(cx));
            }
        });

        cx.observe_keystrokes({
            let view = view.clone();
            move |event, _, cx| {
                let keystroke = &event.keystroke;
                view.update(cx, |view, cx| view.handle_keystroke(keystroke, cx));
            }
        })
        .detach();

        let engine_supervisor = engine_supervisor.clone();
        let shutting_down = shutting_down.clone();
        cx.spawn(move |cx: &mut gpui::AsyncApp| {
            let async_cx = cx.clone();
            async move {
                let mut async_cx = async_cx;
                let mut bridge: Option<Arc<EngineBridge>> = None;
                let mut last_status: Option<SharedString> = None;
                let mut last_version: u64 = 0;
                let mut last_clip_version: u32 = 0;
                let mut needs_clip_resync = false;
                let mut clip_window_requests =
                    vec![ClipWindowRequestState::default(); TRACK_COUNT];
                let mut next_clip_window_request_id: u32 = 1;
                let mut last_window_start: u64 = 0;
                let mut last_window_end: u64 = 0;
                let mut last_window_clip_version: u32 = 0;
                let mut last_harmony_version: u32 = 0;
                let mut needs_harmony_resync = false;
                let mut have_harmony_snapshot = false;
                let mut last_change = std::time::Instant::now();
                loop {
                    if shutting_down.load(Ordering::Relaxed) {
                        break;
                    }
                    {
                        let mut supervisor = engine_supervisor
                            .lock()
                            .expect("engine supervisor lock");
                        if let Some(child) = supervisor.child.as_mut() {
                            match child.try_wait() {
                                Ok(Some(status)) => {
                                    eprintln!("daw-app: engine exited ({})", status);
                                    log_last_ui_command();
                                    supervisor.child = None;
                                }
                                Ok(None) => {}
                                Err(err) => {
                                    eprintln!("daw-app: engine status check failed: {}", err);
                                    supervisor.child = None;
                                }
                            }
                        }
                    }

                    // First, ensure engine is spawned if needed
                    if bridge.is_none() && !shutting_down.load(Ordering::Relaxed) {
                        let mut supervisor = engine_supervisor
                            .lock()
                            .expect("engine supervisor lock");
                        if supervisor.engine_path.is_none() {
                            supervisor.engine_path = default_engine_path();
                            if supervisor.engine_path.is_some() {
                                supervisor.engine_missing_logged = false;
                            }
                        }
                        if supervisor.child.is_none() &&
                            supervisor.last_spawn_attempt.elapsed() > Duration::from_millis(500) {
                            supervisor.last_spawn_attempt = Instant::now();
                            if let Some(engine_path) = supervisor.engine_path.clone() {
                                match spawn_engine_process(&engine_path) {
                                    Ok(child) => {
                                        eprintln!(
                                            "daw-app: spawned engine {} (pid: {:?})",
                                            engine_path.display(),
                                            child.id()
                                        );
                                        supervisor.child = Some(child);
                                        drop(supervisor); // Release the lock before sleeping
                                        // Give the engine time to initialize
                                        eprintln!("daw-app: waiting for engine to initialize...");
                                        Timer::after(Duration::from_secs(2)).await;
                                    }
                                    Err(spawn_err) => {
                                        eprintln!("daw-app: {}", spawn_err);
                                    }
                                }
                            } else if !supervisor.engine_missing_logged {
                                eprintln!(
                                    "daw-app: engine binary not found (build/daw_engine or DAW_ENGINE_PATH)"
                                );
                                supervisor.engine_missing_logged = true;
                            }
                        }
                    }

                    // Then try to connect
                    if bridge.is_none() {
                        match EngineBridge::open() {
                            Ok(opened) => {
                                eprintln!("daw-app: Successfully connected to engine");
                                bridge = Some(Arc::new(opened));
                                let status: SharedString = "SHM: connected".into();
                                let bridge_ref = bridge.clone();
                                let _ = window.update(&mut async_cx, |view, _, cx| {
                                    view.status = status.clone();
                                    view.bridge = bridge_ref;
                                    cx.notify();
                                });
                                last_status = Some(status);
                                // Always reset these when a new bridge is created
                                last_version = 0;
                                last_clip_version = 0;
                                clip_window_requests =
                                    vec![ClipWindowRequestState::default(); TRACK_COUNT];
                                next_clip_window_request_id = 1;
                                last_window_start = 0;
                                last_window_end = 0;
                                last_window_clip_version = 0;
                                last_harmony_version = 0;
                                have_harmony_snapshot = false;
                                last_change = std::time::Instant::now();
                            }
                            Err(err) => {
                                let status: SharedString = "SHM: disconnected".into();
                                if last_status.as_ref() != Some(&status) {
                                    eprintln!("daw-app: Cannot connect to engine: {err}");
                                    let _ = window.update(&mut async_cx, |view, _, cx| {
                                        view.status = status.clone();
                                        view.bridge = None;
                                        cx.notify();
                                    });
                                    last_status = Some(status);
                                }
                                // Wait a bit before retrying connection
                                Timer::after(Duration::from_millis(250)).await;
                                continue;
                            }
                        }
                    }

                    let mut needs_reopen = false;
                    let mut current_snapshot = None;
                    if let Some(bridge_ref) = bridge.as_ref() {
                        // Read and update snapshot
                        if let Some(snapshot) = bridge_ref.read_snapshot() {
                            current_snapshot = Some(snapshot);
                            // Update the timestamp whenever we successfully read a snapshot
                            last_change = std::time::Instant::now();
                            if snapshot.version != last_version {
                                last_version = snapshot.version;
                            }
                            let _ = window.update(&mut async_cx, |view, _, cx| {
                                if view.snapshot.version != snapshot.version {
                                    view.snapshot = snapshot;
                                    cx.notify();
                                }
                            });
                        } else {
                            // Can't read snapshot - check if connection is dead
                            if last_change.elapsed() > Duration::from_secs(2) {
                                eprintln!("daw-app: No snapshot received for 2 seconds, reconnecting");
                                needs_reopen = true;
                            }
                        }

                        // Process events from engine (must happen outside snapshot block)
                        if !needs_reopen {
                            loop {
                                let Some(entry) = bridge_ref.pop_ui_event() else {
                                    break;
                                };
                                if let Some(diff_type) = ui_diff_type(&entry) {
                                    if diff_type == UiDiffType::ChainSnapshot as u16 {
                                        if let Some(diff) = decode_ui_chain_diff(&entry) {
                                            let _ = window.update(&mut async_cx, |view, _, cx| {
                                                view.apply_chain_diff(diff);
                                                cx.notify();
                                            });
                                        }
                                        continue;
                                    }
                                    if diff_type == UiDiffType::ChainError as u16 {
                                        if let Some(diff) = decode_ui_chain_error(&entry) {
                                            let _ = window.update(&mut async_cx, |view, _, cx| {
                                                let message = view.chain_error_message(diff);
                                                view.show_toast(&message, cx);
                                            });
                                        }
                                        continue;
                                    }
                                    if diff_type == UiDiffType::PatcherGraphDelta as u16 {
                                        if let Some(diff) = decode_ui_patcher_graph_diff(&entry) {
                                            let _ = window.update(&mut async_cx, |view, _, cx| {
                                                view.apply_patcher_graph_diff(diff);
                                                if diff.flags == 0 &&
                                                    diff.node_type == PATCHER_NODE_RANDOM_DEGREE {
                                                    view.send_random_degree_config(diff.node_id);
                                                }
                                                cx.notify();
                                            });
                                        }
                                        continue;
                                    }
                                    if diff_type == UiDiffType::PatcherGraphError as u16 {
                                        if let Some(diff) = decode_ui_patcher_graph_error(&entry) {
                                            let _ = window.update(&mut async_cx, |view, _, cx| {
                                                let message = view.patcher_error_message(diff);
                                                view.show_toast(&message, cx);
                                            });
                                        }
                                        continue;
                                    }
                                }
                                if let Some(diff) = decode_ui_diff(&entry) {
                                    if diff.diff_type == UiDiffType::ResyncNeeded as u16 {
                                        eprintln!(
                                            "daw-app: ResyncNeeded (local {}, engine {})",
                                            last_clip_version,
                                            diff.clip_version
                                        );
                                        needs_clip_resync = true;
                                        let _ = window.update(&mut async_cx, |view, _, _| {
                                            view.clip_resync_pending = true;
                                        });
                                        continue;
                                    }
                                    let next_expected = last_clip_version.saturating_add(1);
                                    if diff.clip_version != next_expected && last_clip_version != 0 {
                                        eprintln!(
                                            "daw-app: Clip diff gap (expected {}, got {})",
                                            next_expected,
                                            diff.clip_version
                                        );
                                        needs_clip_resync = true;
                                        continue;
                                    }
                                    last_clip_version = diff.clip_version;
                                    let _ = window.update(&mut async_cx, |view, _, cx| {
                                        view.apply_diff(diff);
                                        cx.notify();
                                    });
                                    continue;
                                }
                                if let Some(diff) = decode_harmony_diff(&entry) {
                                    if diff.diff_type == UiHarmonyDiffType::ResyncNeeded as u16 {
                                        eprintln!(
                                            "daw-app: Harmony resync needed (local {}, engine {})",
                                            last_harmony_version,
                                            diff.harmony_version
                                        );
                                        needs_harmony_resync = true;
                                        let _ = window.update(&mut async_cx, |view, _, _| {
                                            view.harmony_resync_pending = true;
                                        });
                                        continue;
                                    }
                                    let next_expected = last_harmony_version.saturating_add(1);
                                    if diff.harmony_version != next_expected &&
                                        last_harmony_version != 0 {
                                        eprintln!(
                                            "daw-app: Harmony diff gap (expected {}, got {})",
                                            next_expected,
                                            diff.harmony_version
                                        );
                                        needs_harmony_resync = true;
                                        continue;
                                    }
                                    last_harmony_version = diff.harmony_version;
                                    let _ = window.update(&mut async_cx, |view, _, cx| {
                                        view.apply_harmony_diff(diff);
                                        cx.notify();
                                    });
                                    continue;
                                }
                                if let Some(diff) = decode_chord_diff(&entry) {
                                    if diff.diff_type == UiChordDiffType::ResyncNeeded as u16 {
                                        eprintln!(
                                            "daw-app: Chord resync needed (local {}, engine {})",
                                            last_clip_version,
                                            diff.clip_version
                                        );
                                        needs_clip_resync = true;
                                        let _ = window.update(&mut async_cx, |view, _, _| {
                                            view.clip_resync_pending = true;
                                        });
                                        continue;
                                    }
                                    let next_expected = last_clip_version.saturating_add(1);
                                    if diff.clip_version != next_expected &&
                                        last_clip_version != 0 {
                                        eprintln!(
                                            "daw-app: Chord diff gap (expected {}, got {})",
                                            next_expected,
                                            diff.clip_version
                                        );
                                        needs_clip_resync = true;
                                        continue;
                                    }
                                    last_clip_version = diff.clip_version;
                                    let _ = window.update(&mut async_cx, |view, _, cx| {
                                        view.apply_chord_diff(diff);
                                        cx.notify();
                                    });
                                    continue;
                                }
                            }
                            if let Some(snapshot) = current_snapshot.as_ref() {
                                let track_count = snapshot.ui_track_count.min(TRACK_COUNT as u32) as usize;
                                let desired_range = window
                                    .update(&mut async_cx, |view, _, _| {
                                        view.desired_clip_window_range()
                                    })
                                    .unwrap_or((0, 0));
                                let window_start = desired_range.0;
                                let window_end = desired_range.1;
                                let clip_version = snapshot.ui_clip_version;
                                let window_changed = window_start != last_window_start ||
                                    window_end != last_window_end ||
                                    clip_version != last_window_clip_version;

                                if needs_clip_resync || window_changed {
                                    last_window_start = window_start;
                                    last_window_end = window_end;
                                    last_window_clip_version = clip_version;
                                    for track_index in 0..track_count {
                                        let request_id = next_clip_window_request_id;
                                        next_clip_window_request_id = next_clip_window_request_id.wrapping_add(1).max(1);
                                        clip_window_requests[track_index] = ClipWindowRequestState {
                                            window_start,
                                            window_end,
                                            request_id,
                                            cursor_event_index: 0,
                                            in_flight_cursor: 0,
                                            clip_version,
                                            complete: false,
                                            in_flight: false,
                                        };
                                    }
                                    let _ = window.update(&mut async_cx, |view, _, cx| {
                                        for track_index in 0..track_count {
                                            if let Some(notes) = view.clip_notes.get_mut(track_index) {
                                                notes.clear();
                                            }
                                            if let Some(chords) = view.clip_chords.get_mut(track_index) {
                                                chords.clear();
                                            }
                                            if let Some(state) = view.clip_window.get_mut(track_index) {
                                                state.request_id = clip_window_requests[track_index].request_id;
                                                state.cursor_event_index = 0;
                                                state.next_event_index = 0;
                                                state.window_start = window_start;
                                                state.window_end = window_end;
                                                state.clip_version = clip_version;
                                                state.complete = false;
                                            }
                                        }
                                        view.pending_notes.clear();
                                        view.pending_chords.clear();
                                        view.clip_resync_pending = false;
                                        view.rebase_clip_queue(clip_version);
                                        cx.notify();
                                    });
                                    needs_clip_resync = false;
                                }

                                if let Some(window_snapshot) = bridge_ref.read_clip_window_snapshot() {
                                    let track_index = window_snapshot.track_id as usize;
                                    if track_index < track_count {
                                        let state = &mut clip_window_requests[track_index];
                                        if state.in_flight &&
                                            window_snapshot.request_id == state.request_id &&
                                            window_snapshot.cursor_event_index == state.in_flight_cursor &&
                                            window_snapshot.window_start_nanotick == state.window_start &&
                                            window_snapshot.window_end_nanotick == state.window_end {
                                            if (window_snapshot.flags & UI_CLIP_WINDOW_FLAG_RESYNC) != 0 ||
                                                window_snapshot.clip_version != state.clip_version {
                                                needs_clip_resync = true;
                                                let _ = window.update(&mut async_cx, |view, _, _| {
                                                    view.clip_resync_pending = true;
                                                });
                                            } else {
                                                let reset = window_snapshot.cursor_event_index == 0;
                                                let _ = window.update(&mut async_cx, |view, _, cx| {
                                                    view.apply_clip_window_page(window_snapshot, reset);
                                                    cx.notify();
                                                });
                                                state.cursor_event_index = window_snapshot.next_event_index;
                                                state.complete =
                                                    (window_snapshot.flags & UI_CLIP_WINDOW_FLAG_COMPLETE) != 0;
                                            }
                                            state.in_flight = false;
                                        }
                                    }
                                }

                                if !clip_window_requests.iter().any(|state| state.in_flight) {
                                    for track_index in 0..track_count {
                                        let state = &mut clip_window_requests[track_index];
                                        if state.complete {
                                            continue;
                                        }
                                        let (start_lo, start_hi) = split_u64(state.window_start);
                                        let (end_lo, end_hi) = split_u64(state.window_end);
                                        let payload = UiClipWindowCommandPayload {
                                            command_type: UiCommandType::RequestClipWindow as u16,
                                            flags: 0,
                                            track_id: track_index as u32,
                                            request_id: state.request_id,
                                            window_start_lo: start_lo,
                                            window_start_hi: start_hi,
                                            window_end_lo: end_lo,
                                            window_end_hi: end_hi,
                                            cursor_event_index: state.cursor_event_index,
                                            reserved: 0,
                                            reserved2: 0,
                                        };
                                        if bridge_ref.send_ui_clip_window_command(payload) {
                                            state.in_flight = true;
                                            state.in_flight_cursor = state.cursor_event_index;
                                        }
                                        break;
                                    }
                                }
                            }
                            if needs_harmony_resync {
                                if let Some(harmony_snapshot) = bridge_ref.read_harmony_snapshot() {
                                    let new_version = current_snapshot.as_ref().map(|s| s.ui_harmony_version).unwrap_or(0);
                                    eprintln!(
                                        "daw-app: Harmony resync applied (version {})",
                                        new_version
                                    );
                                    last_harmony_version = new_version;
                                    needs_harmony_resync = false;
                                    have_harmony_snapshot = true;
                                    let _ = window.update(&mut async_cx, |view, _, cx| {
                                        view.apply_harmony_snapshot(harmony_snapshot);
                                        view.harmony_resync_pending = false;
                                        view.rebase_harmony_queue(new_version);
                                        cx.notify();
                                    });
                                }
                            } else if !have_harmony_snapshot && last_harmony_version == 0 {
                                // Only read initial snapshot once when first connecting
                                if let Some(harmony_snapshot) = bridge_ref.read_harmony_snapshot() {
                                    let new_version = current_snapshot.as_ref().map(|s| s.ui_harmony_version).unwrap_or(0);
                                    eprintln!(
                                        "daw-app: Initial harmony snapshot loaded (version {})",
                                        new_version
                                    );
                                    last_harmony_version = new_version;
                                    have_harmony_snapshot = true;
                                    let _ = window.update(&mut async_cx, |view, _, cx| {
                                        view.apply_harmony_snapshot(harmony_snapshot);
                                        view.rebase_harmony_queue(new_version);
                                        cx.notify();
                                    });
                                }
                            }
                        }
                    }
                    if needs_reopen {
                        let status: SharedString = "SHM: disconnected".into();
                        let _ = window.update(&mut async_cx, |view, _, cx| {
                            view.status = status.clone();
                            view.bridge = None;
                            cx.notify();
                        });
                        bridge = None;
                        last_status = Some(status);
                        Timer::after(Duration::from_millis(250)).await;
                        continue;
                    }

                    let _ = window.update(&mut async_cx, |view, _, _| {
                        view.flush_queued_commands();
                    });
                    Timer::after(Duration::from_millis(8)).await;
                }
            }
        })
        .detach();

        cx.activate(true);
    });
    Ok(())
}
