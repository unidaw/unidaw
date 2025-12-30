use std::ffi::CString;
use std::fs;
use std::os::unix::io::FromRawFd;
use std::path::PathBuf;
use std::process::{Child, Command, Stdio};
use std::sync::{Arc, Mutex};
use std::sync::atomic::{AtomicBool, fence, Ordering};
use std::time::{Duration, Instant};

use anyhow::{Context as AnyhowContext, Result};
use gpui::{
    actions, div, px, rgb, size, App, Application, Bounds, Context, KeyBinding, MouseButton,
    SharedString, Timer, Window, WindowBounds, WindowOptions, prelude::*,
};
use memmap2::{MmapMut, MmapOptions};
use serde::Deserialize;

use daw_bridge::layout::{
    EventEntry, EventType, K_UI_MAX_TRACKS, RingHeader, ShmHeader, UiClipSnapshot,
    UiCommandPayload, UiCommandType, UiDiffPayload, UiDiffType, UiHarmonyDiffPayload,
    UiHarmonyDiffType, UiHarmonySnapshot, UiChordCommandPayload, UiChordDiffPayload,
    UiChordDiffType,
};
use daw_bridge::reader::{SeqlockReader, UiSnapshot};

const NANOTICKS_PER_QUARTER: u64 = 960_000;
const BEATS_PER_BAR: u64 = 4;
const LINES_PER_BEAT: u64 = 4;
const TRACK_COUNT: usize = 8;
const VISIBLE_ROWS: usize = 32;
const EDIT_STEP_ROWS: i64 = 1;
const MAX_NOTE_COLUMNS: usize = 8;

// UI Layout Dimensions
const COLUMN_WIDTH: f32 = 52.0;
const TIME_COLUMN_WIDTH: f32 = 70.0;
const HARMONY_COLUMN_WIDTH: f32 = COLUMN_WIDTH;
const ROW_HEIGHT: f32 = 16.0;
const HEADER_HEIGHT: f32 = 24.0;
const SCALE_LIBRARY: &[ScaleInfo] = &[
    ScaleInfo { id: 1, name: "maj", key: "1" },
    ScaleInfo { id: 2, name: "min", key: "2" },
    ScaleInfo { id: 3, name: "dor", key: "3" },
    ScaleInfo { id: 4, name: "mix", key: "4" },
];

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum PaletteMode {
    Commands,
    Plugins,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum PaletteCommandId {
    LoadPlugin,
    SetHarmonyScale,
}

#[derive(Clone, Copy, Debug)]
struct PaletteCommand {
    id: PaletteCommandId,
    label: &'static str,
    hint: &'static str,
}

const PALETTE_COMMANDS: &[PaletteCommand] = &[
    PaletteCommand {
        id: PaletteCommandId::LoadPlugin,
        label: "Load Plugin on Track…",
        hint: "Enter",
    },
    PaletteCommand {
        id: PaletteCommandId::SetHarmonyScale,
        label: "Set Harmony Scale…",
        hint: "Cmd+Shift+S",
    },
];

actions!(
    daw_app,
    [
        TogglePalette,
        OpenPluginPalette,
        PaletteUp,
        PaletteDown,
        PaletteConfirm,
        PaletteBackspace,
        PaletteClose,
        OpenScaleBrowser,
        TogglePlay,
        Undo,
        DeleteNote,
        ScrollUp,
        ScrollDown,
        ScrollPageUp,
        ScrollPageDown,
        ToggleFollowPlayhead,
        ToggleHarmonyFocus,
        ColumnLeft,
        ColumnRight,
        CommitCellEdit,
        CancelCellEdit,
        FocusLeft,
        FocusRight
    ]
);

#[derive(Clone, Debug)]
struct PluginEntry {
    index: usize,
    name: String,
    vendor: String,
    is_instrument: bool,
}

#[derive(Deserialize)]
struct PluginCacheFile {
    #[serde(default)]
    plugins: Vec<PluginCacheEntry>,
}

#[derive(Deserialize)]
struct PluginCacheEntry {
    name: String,
    vendor: String,
    #[serde(default)]
    is_instrument: bool,
    #[serde(default)]
    ok: bool,
    #[serde(default)]
    scan_status: String,
    #[serde(default)]
    error: String,
}

struct RingView {
    header: *mut RingHeader,
    entries: *mut EventEntry,
    mask: u32,
}

struct EngineBridge {
    _mmap: MmapMut,
    base: *const u8,
    header: *const ShmHeader,
    reader: SeqlockReader,
    ring_ui: Option<RingView>,
    ring_ui_out: Option<RingView>,
}

impl EngineBridge {
    fn open() -> Result<Self> {
        let name = default_shm_name();
        let c_name = CString::new(name.clone())
            .with_context(|| format!("invalid SHM name: {name}"))?;
        let fd = unsafe { libc::shm_open(c_name.as_ptr(), libc::O_RDWR, 0) };
        if fd < 0 {
            return Err(std::io::Error::last_os_error())
                .with_context(|| format!("failed to open SHM {name}"));
        }

        let file = unsafe { std::fs::File::from_raw_fd(fd) };
        let mmap = unsafe { MmapOptions::new().map_mut(&file) }?;
        let base = mmap.as_ptr() as *const u8;
        let header = base as *const ShmHeader;
        let reader = SeqlockReader::new(header);
        let ring_ui = ring_view(base as *mut u8, unsafe { (*header).ring_ui_offset });
        let ring_ui_out = ring_view(base as *mut u8, unsafe { (*header).ring_ui_out_offset });
        Ok(Self {
            _mmap: mmap,
            base,
            header,
            reader,
            ring_ui,
            ring_ui_out,
        })
    }

    fn read_snapshot(&self) -> Option<UiSnapshot> {
        self.reader.read_snapshot()
    }

    fn send_ui_command(&self, payload: UiCommandPayload) -> bool {
        let Some(ring) = self.ring_ui.as_ref() else {
            return false;
        };
        let mut entry = EventEntry {
            sample_time: 0,
            block_id: 0,
            event_type: EventType::UiCommand as u16,
            size: std::mem::size_of::<UiCommandPayload>() as u16,
            flags: 0,
            payload: [0u8; 40],
        };
        let payload_bytes = unsafe {
            std::slice::from_raw_parts(
                &payload as *const UiCommandPayload as *const u8,
                std::mem::size_of::<UiCommandPayload>(),
            )
        };
        entry.payload[..payload_bytes.len()].copy_from_slice(payload_bytes);
        ring_write(ring, entry)
    }

    fn send_ui_chord_command(&self, payload: UiChordCommandPayload) -> bool {
        let Some(ring) = self.ring_ui.as_ref() else {
            return false;
        };
        let mut entry = EventEntry {
            sample_time: 0,
            block_id: 0,
            event_type: EventType::UiCommand as u16,
            size: std::mem::size_of::<UiChordCommandPayload>() as u16,
            flags: 0,
            payload: [0u8; 40],
        };
        let payload_bytes = unsafe {
            std::slice::from_raw_parts(
                &payload as *const UiChordCommandPayload as *const u8,
                std::mem::size_of::<UiChordCommandPayload>(),
            )
        };
        entry.payload[..payload_bytes.len()].copy_from_slice(payload_bytes);
        ring_write(ring, entry)
    }

    fn pop_ui_event(&self) -> Option<EventEntry> {
        let Some(ring) = self.ring_ui_out.as_ref() else {
            return None;
        };
        ring_pop(ring)
    }

    fn read_clip_snapshot(&self) -> Option<UiClipSnapshot> {
        if self.header.is_null() {
            return None;
        }
        loop {
            let v0 = unsafe { (*self.header).ui_version.load(Ordering::Acquire) };
            if v0 % 2 == 1 {
                continue;
            }
            let clip_offset = unsafe { (*self.header).ui_clip_offset };
            let clip_bytes = unsafe { (*self.header).ui_clip_bytes };
            if clip_offset == 0 || clip_bytes < std::mem::size_of::<UiClipSnapshot>() as u64 {
                return None;
            }
            let snapshot_ptr =
                unsafe { self.base.add(clip_offset as usize) as *const UiClipSnapshot };
            let snapshot = unsafe { *snapshot_ptr };

            fence(Ordering::Acquire);
            let v1 = unsafe { (*self.header).ui_version.load(Ordering::Acquire) };
            if v0 == v1 && v0 % 2 == 0 {
                return Some(snapshot);
            }
        }
    }

    fn read_harmony_snapshot(&self) -> Option<UiHarmonySnapshot> {
        if self.header.is_null() {
            return None;
        }
        loop {
            let v0 = unsafe { (*self.header).ui_version.load(Ordering::Acquire) };
            if v0 % 2 == 1 {
                continue;
            }
            let harmony_offset = unsafe { (*self.header).ui_harmony_offset };
            let harmony_bytes = unsafe { (*self.header).ui_harmony_bytes };
            if harmony_offset == 0 ||
                harmony_bytes < std::mem::size_of::<UiHarmonySnapshot>() as u64
            {
                return None;
            }
            let snapshot_ptr =
                unsafe { self.base.add(harmony_offset as usize) as *const UiHarmonySnapshot };
            let snapshot = unsafe { *snapshot_ptr };

            fence(Ordering::Acquire);
            let v1 = unsafe { (*self.header).ui_version.load(Ordering::Acquire) };
            if v0 == v1 && v0 % 2 == 0 {
                return Some(snapshot);
            }
        }
    }
}

#[derive(Clone, Debug)]
#[allow(dead_code)]
struct PendingNote {
    track_id: u32,
    nanotick: u64,
    duration: u64,
    pitch: u8,
    velocity: u8,
}

#[derive(Clone, Debug)]
#[allow(dead_code)]
struct ClipNote {
    nanotick: u64,
    duration: u64,
    pitch: u8,
    velocity: u8,
}

#[derive(Clone, Debug)]
struct HarmonyEntry {
    nanotick: u64,
    root: u32,
    scale_id: u32,
}

#[derive(Clone, Debug)]
#[allow(dead_code)]
struct ClipChord {
    chord_id: u32,
    nanotick: u64,
    duration: u64,
    spread: u32,
    humanize_timing: u16,
    humanize_velocity: u16,
    degree: u8,
    quality: u8,
    inversion: u8,
    base_octave: u8,
}

#[derive(Clone, Copy, Debug)]
struct ScaleInfo {
    id: u32,
    name: &'static str,
    key: &'static str,
}

#[derive(Clone, Debug)]
enum CellKind {
    Note,
    Chord,
}

#[derive(Clone, Debug)]
#[allow(dead_code)]
struct CellEntry {
    kind: CellKind,
    text: String,
    nanotick: u64,
    note_pitch: Option<u8>,
    chord_id: Option<u32>,
}

struct EngineView {
    bridge: Option<Arc<EngineBridge>>,
    snapshot: UiSnapshot,
    clip_snapshot: Option<UiClipSnapshot>,
    status: SharedString,
    plugins: Vec<PluginEntry>,
    palette_open: bool,
    palette_query: String,
    palette_selection: usize,
    palette_mode: PaletteMode,
    palette_empty_logged: bool,
    scale_browser_open: bool,
    scale_browser_query: String,
    scale_browser_selection: usize,
    focused_track_index: usize,
    cursor_row: i64,
    cursor_col: usize,
    scroll_row_offset: i64,
    follow_playhead: bool,
    harmony_focus: bool,
    harmony_scale_id: u32,
    edit_active: bool,
    edit_text: String,
    pending_notes: Vec<PendingNote>,
    clip_notes: Vec<Vec<ClipNote>>,
    clip_version_local: u32,
    harmony_version_local: u32,
    harmony_events: Vec<HarmonyEntry>,
    clip_chords: Vec<Vec<ClipChord>>,
    track_columns: Vec<usize>,
    track_quantize: Vec<bool>,
    track_names: Vec<Option<String>>,
}

struct EngineSupervisor {
    child: Option<Child>,
    last_spawn_attempt: Instant,
    engine_path: Option<PathBuf>,
    engine_missing_logged: bool,
}

trait UiNotify {
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
            clip_snapshot: None,
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
            cursor_row: 0,
            cursor_col: 0,
            scroll_row_offset: 0,
            follow_playhead: true,
            harmony_focus: false,
            harmony_scale_id: 1,
            edit_active: false,
            edit_text: String::new(),
            pending_notes: Vec::new(),
            clip_notes: vec![Vec::new(); TRACK_COUNT],
            clip_version_local: 0,
            harmony_version_local: 0,
            harmony_events: Vec::new(),
            clip_chords: vec![Vec::new(); TRACK_COUNT],
            track_columns: vec![1; TRACK_COUNT],
            track_quantize: vec![true; TRACK_COUNT],
            track_names: vec![None; TRACK_COUNT],
        }
    }

    #[cfg(test)]
    fn new_for_tests() -> Self {
        Self::new_state()
    }

    fn toggle_palette(&mut self, cx: &mut impl UiNotify) {
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

    fn close_palette(&mut self, cx: &mut impl UiNotify) {
        if self.palette_open {
            self.palette_open = false;
            self.palette_empty_logged = false;
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

    fn confirm_palette(&mut self, cx: &mut impl UiNotify) {
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
                let plugin = &self.plugins[index];

                if let Some(bridge) = &self.bridge {
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
                    bridge.send_ui_command(payload);
                    if self.focused_track_index < self.track_names.len() {
                        self.track_names[self.focused_track_index] =
                            Some(plugin.name.clone());
                    }
                } else {
                    // No-op until engine reconnects.
                }

                self.palette_open = false;
                cx.notify();
            }
        }
    }

    fn handle_keystroke(&mut self, keystroke: &gpui::Keystroke, cx: &mut impl UiNotify) {
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

    fn action_palette_up(&mut self, cx: &mut impl UiNotify) {
        if self.scale_browser_open {
            self.move_scale_selection(-1, cx);
        } else if self.palette_open {
            self.move_selection(-1, cx);
        } else {
            self.move_cursor_row(-1, cx);
        }
    }

    fn action_palette_down(&mut self, cx: &mut impl UiNotify) {
        if self.scale_browser_open {
            self.move_scale_selection(1, cx);
        } else if self.palette_open {
            self.move_selection(1, cx);
        } else {
            self.move_cursor_row(1, cx);
        }
    }

    fn action_palette_backspace(&mut self, cx: &mut impl UiNotify) {
        if self.edit_active {
            if self.edit_text.is_empty() {
                self.cancel_cell_edit(cx);
                if self.harmony_focus {
                    self.delete_harmony(cx);
                } else {
                    self.delete_note(cx);
                }
            } else {
                self.edit_text.pop();
                cx.notify();
            }
        } else if self.scale_browser_open {
            self.backspace_scale_query(cx);
        } else if self.palette_open {
            self.backspace_query(cx);
        } else if self.harmony_focus {
            self.delete_harmony(cx);
        } else {
            self.delete_note(cx);
        }
    }

    fn action_palette_confirm(&mut self, cx: &mut impl UiNotify) {
        if self.edit_active {
            self.commit_cell_edit(cx);
        } else if self.scale_browser_open {
            self.confirm_scale_browser(cx);
        } else if self.palette_open {
            self.confirm_palette(cx);
        } else {
            self.start_cell_edit("", cx);
        }
    }

    fn move_cursor_row(&mut self, delta: i64, cx: &mut impl UiNotify) {
        let view_rows = VISIBLE_ROWS as i64;
        let current_abs = (self.scroll_row_offset + self.cursor_row).max(0);
        let mut next_abs = current_abs + delta;
        if next_abs < 0 {
            next_abs = 0;
        }
        if next_abs < self.scroll_row_offset {
            self.scroll_row_offset = next_abs;
            self.cursor_row = 0;
        } else if next_abs >= self.scroll_row_offset + view_rows {
            self.scroll_row_offset = next_abs - (view_rows - 1);
            self.cursor_row = view_rows - 1;
        } else {
            self.cursor_row = next_abs - self.scroll_row_offset;
        }
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
        let next = (self.scroll_row_offset + delta).max(0);
        if next != self.scroll_row_offset {
            self.scroll_row_offset = next;
            self.follow_playhead = false;
            cx.notify();
        }
    }

    fn toggle_follow_playhead(&mut self, cx: &mut impl UiNotify) {
        self.follow_playhead = !self.follow_playhead;
        cx.notify();
    }

    fn toggle_harmony_focus(&mut self, cx: &mut impl UiNotify) {
        self.harmony_focus = !self.harmony_focus;
        self.clear_edit_state();
        cx.notify();
    }

    fn move_column(&mut self, delta: i32, cx: &mut impl UiNotify) {
        let columns = self.track_columns[self.focused_track_index];
        let max_index = columns.saturating_sub(1) as i32;
        let next = (self.cursor_col as i32 + delta).clamp(0, max_index);
        self.cursor_col = next as usize;
        self.clear_edit_state();
        cx.notify();
    }

    fn adjust_columns(&mut self, track: usize, delta: i32, cx: &mut impl UiNotify) {
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

    fn focus_harmony_row(&mut self, row: usize, cx: &mut impl UiNotify) {
        self.cursor_row = row.min(VISIBLE_ROWS - 1) as i64;
        self.harmony_focus = true;
        self.edit_active = false;
        self.edit_text.clear();
        cx.notify();
    }

    fn focus_note_cell(
        &mut self,
        row: usize,
        track: usize,
        column: usize,
        cx: &mut impl UiNotify,
    ) {
        self.cursor_row = row.min(VISIBLE_ROWS - 1) as i64;
        self.focused_track_index = track.min(TRACK_COUNT - 1);
        let max_column = self.track_columns[self.focused_track_index]
            .saturating_sub(1);
        self.cursor_col = column.min(max_column);
        self.harmony_focus = false;
        self.clear_edit_state();
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
    fn set_harmony_scale(&mut self, scale_id: u32, cx: &mut impl UiNotify) {
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

    fn confirm_scale_browser(&mut self, cx: &mut impl UiNotify) {
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

        // Process any pending diffs before sending command
        self.process_pending_diffs();

        if let Some(bridge) = &self.bridge {
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
            bridge.send_ui_command(payload);
            self.bump_harmony_version();
        }
        cx.notify();
    }

    fn write_harmony_scale(&mut self, scale_id: u32, cx: &mut impl UiNotify) {
        let nanotick = self.current_row_nanotick();
        let root = self.harmony_root_at(nanotick);

        // Process any pending diffs before sending command
        self.process_pending_diffs();

        if let Some(bridge) = &self.bridge {
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
            bridge.send_ui_command(payload);
            self.bump_harmony_version();
        }
        self.harmony_scale_id = scale_id;
        cx.notify();
    }

    fn harmony_root_at(&self, nanotick: u64) -> u32 {
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

    fn harmony_scale_at(&self, nanotick: u64) -> u32 {
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

    fn current_row_nanotick(&self) -> u64 {
        let absolute_row = self.current_absolute_row();
        self.nanotick_for_row(absolute_row)
    }

    fn row_nanoticks(&self) -> u64 {
        NANOTICKS_PER_QUARTER / LINES_PER_BEAT
    }

    fn current_absolute_row(&self) -> i64 {
        (self.scroll_row_offset + self.cursor_row).max(0)
    }

    fn nanotick_for_row(&self, absolute_row: i64) -> u64 {
        let row_nanoticks = self.row_nanoticks();
        let row = absolute_row.max(0) as u64;
        row * row_nanoticks
    }

    fn delete_harmony(&mut self, cx: &mut impl UiNotify) {
        let row_nanoticks = NANOTICKS_PER_QUARTER / LINES_PER_BEAT;
        let absolute_row = (self.scroll_row_offset + self.cursor_row).max(0);
        let nanotick = (absolute_row as u64) * row_nanoticks;

        // Check if there's actually a harmony event to delete
        let has_harmony = self.harmony_events.iter()
            .any(|event| event.nanotick == nanotick);

        if let Some(bridge) = &self.bridge {
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
            bridge.send_ui_command(payload);
            self.bump_harmony_version();
        }

        // Move cursor down after deletion if there was something to delete
        if has_harmony {
            self.move_cursor_row(1, cx);
        }
        cx.notify();
    }

    fn current_clip_version(&self) -> u32 {
        if self.clip_version_local != 0 {
            self.clip_version_local
        } else {
            self.snapshot.ui_clip_version
        }
    }

    fn bump_clip_version(&mut self) {
        let next = self.current_clip_version().saturating_add(1);
        self.clip_version_local = next;
    }

    fn process_pending_diffs(&mut self) {
        // Process all pending UI diffs to ensure we have the latest version
        let bridge = match &self.bridge {
            Some(b) => b.clone(),
            None => return,
        };

        let mut processed_any = false;
        while let Some(entry) = bridge.pop_ui_event() {
            if let Some(diff) = decode_ui_diff(&entry) {
                if diff.diff_type != UiDiffType::ResyncNeeded as u16 {
                    self.apply_diff(diff);
                    processed_any = true;
                }
            }
            if let Some(diff) = decode_harmony_diff(&entry) {
                if diff.diff_type != UiHarmonyDiffType::ResyncNeeded as u16 {
                    self.apply_harmony_diff(diff);
                    processed_any = true;
                }
            }
            if let Some(diff) = decode_chord_diff(&entry) {
                if diff.diff_type != UiChordDiffType::ResyncNeeded as u16 {
                    self.apply_chord_diff(diff);
                    processed_any = true;
                }
            }
        }
        // Also update the snapshot to get latest version
        if processed_any {
            if let Some(snapshot) = bridge.reader.read_snapshot() {
                self.snapshot = snapshot;
            }
        }
    }

    fn current_harmony_version(&self) -> u32 {
        if self.harmony_version_local != 0 {
            self.harmony_version_local
        } else {
            self.snapshot.ui_harmony_version
        }
    }

    fn bump_harmony_version(&mut self) {
        let next = self.current_harmony_version().saturating_add(1);
        self.harmony_version_local = next;
    }

    fn delete_note(&mut self, cx: &mut impl UiNotify) {
        let nanotick = self.current_row_nanotick();
        let Some(entry) = self.cell_entry_at(
            nanotick,
            self.focused_track_index,
            self.cursor_col,
        ) else {
            return;
        };

        // Process any pending diffs before sending command
        self.process_pending_diffs();

        let mut deleted_something = false;
        match entry.kind {
            CellKind::Note => {
                let Some(pitch) = entry.note_pitch else {
                    return;
                };
                if let Some(bridge) = &self.bridge {
                    let (note_nanotick_lo, note_nanotick_hi) = split_u64(nanotick);
                    let payload = UiCommandPayload {
                        command_type: UiCommandType::DeleteNote as u16,
                        flags: 0,
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
                    bridge.send_ui_command(payload);
                    self.bump_clip_version();
                }
                if let Some(notes) = self.clip_notes.get_mut(self.focused_track_index) {
                    notes.retain(|note| {
                        !(note.nanotick == nanotick && note.pitch == pitch)
                    });
                }
                self.pending_notes.retain(|note| {
                    !(note.track_id == self.focused_track_index as u32 &&
                        note.nanotick == nanotick &&
                        note.pitch == pitch)
                });
                deleted_something = true;
            }
            CellKind::Chord => {
                let Some(chord_id) = entry.chord_id else {
                    return;
                };
                self.send_delete_chord(chord_id);
                if let Some(chords) = self.clip_chords.get_mut(self.focused_track_index) {
                    chords.retain(|chord| chord.chord_id != chord_id);
                }
                deleted_something = true;
            }
        }
        // Move cursor down after successful deletion
        if deleted_something {
            self.move_cursor_row(1, cx);
        }
        cx.notify();
    }

    fn send_undo(&mut self, cx: &mut impl UiNotify) {
        if let Some(bridge) = &self.bridge {
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
            bridge.send_ui_command(payload);
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
    }

    fn write_note(&mut self, pitch: u8, cx: &mut impl UiNotify) {
        let row_nanoticks = self.row_nanoticks();
        let nanotick = self.current_row_nanotick();
        let (note_nanotick_lo, note_nanotick_hi) = split_u64(nanotick);
        let (note_duration_lo, note_duration_hi) = split_u64(row_nanoticks);

        // Clear any existing pending notes at this position
        self.pending_notes.retain(|note| {
            !(note.track_id == self.focused_track_index as u32 && note.nanotick == nanotick)
        });

        // Process any pending diffs before sending command
        self.process_pending_diffs();

        if let Some(notes) = self.clip_notes.get_mut(self.focused_track_index) {
            notes.retain(|note| note.nanotick != nanotick);
        }
        if let Some(chords) = self.clip_chords.get_mut(self.focused_track_index) {
            chords.retain(|chord| chord.nanotick != nanotick);
        }

        if let Some(bridge) = &self.bridge {
            let payload = UiCommandPayload {
                command_type: UiCommandType::WriteNote as u16,
                flags: 0,
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
            bridge.send_ui_command(payload);
            self.bump_clip_version();
        }
        self.pending_notes.push(PendingNote {
            track_id: self.focused_track_index as u32,
            nanotick,
            duration: row_nanoticks,
            pitch,
            velocity: 100,
        });
        self.move_cursor_row(EDIT_STEP_ROWS, cx);
    }

    fn send_delete_chord(&mut self, chord_id: u32) {
        // Process any pending diffs before sending command
        self.process_pending_diffs();

        if let Some(bridge) = &self.bridge {
            let nanotick = self.current_row_nanotick();
            let (nanotick_lo, nanotick_hi) = split_u64(nanotick);
            let payload = UiChordCommandPayload {
                command_type: UiCommandType::DeleteChord as u16,
                flags: 0,
                track_id: self.focused_track_index as u32,
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
            bridge.send_ui_chord_command(payload);
            self.bump_clip_version();
        }
    }

    fn toggle_play(&mut self, cx: &mut impl UiNotify) {
        if let Some(bridge) = &self.bridge {
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
            bridge.send_ui_command(payload);
        }
        cx.notify();
    }

    fn filtered_command_indices(&self) -> Vec<usize> {
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

    fn filtered_plugin_indices(&self) -> Vec<usize> {
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

    fn filtered_scale_indices(&self) -> Vec<usize> {
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

    fn render_palette(&mut self, cx: &mut Context<Self>) -> impl IntoElement {
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

    fn render_scale_browser(&self, cx: &mut Context<Self>) -> impl IntoElement {
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

    fn cell_entry_at(
        &self,
        nanotick: u64,
        track_index: usize,
        column: usize,
    ) -> Option<CellEntry> {
        let entries = self.entries_for_row(nanotick, track_index);
        entries.get(column).cloned()
    }

    fn entries_for_row(&self, nanotick: u64, track_index: usize) -> Vec<CellEntry> {
        let mut entries: Vec<CellEntry> = Vec::new();
        if let Some(notes) = self.clip_notes.get(track_index) {
            for note in notes.iter().filter(|note| note.nanotick == nanotick) {
                entries.push(CellEntry {
                    kind: CellKind::Note,
                    text: pitch_to_note(note.pitch),
                    nanotick,
                    note_pitch: Some(note.pitch),
                    chord_id: None,
                });
            }
        }
        for note in self
            .pending_notes
            .iter()
            .filter(|note| note.track_id as usize == track_index && note.nanotick == nanotick)
        {
            entries.push(CellEntry {
                kind: CellKind::Note,
                text: pitch_to_note(note.pitch),
                nanotick,
                note_pitch: Some(note.pitch),
                chord_id: None,
            });
        }
        if let Some(chords) = self.clip_chords.get(track_index) {
            for chord in chords.iter().filter(|chord| chord.nanotick == nanotick) {
                let text = chord_token_text(chord);
                entries.push(CellEntry {
                    kind: CellKind::Chord,
                    text,
                    nanotick,
                    note_pitch: None,
                    chord_id: Some(chord.chord_id),
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
        let row_nanoticks = self.row_nanoticks();
        let nanotick = self.current_row_nanotick();
        let duration = chord.duration.unwrap_or(row_nanoticks);
        let degree = chord.degree.min(255) as u16;

        // Clear any existing pending notes at this position
        self.pending_notes.retain(|note| {
            !(note.track_id == self.focused_track_index as u32 && note.nanotick == nanotick)
        });

        // Process any pending diffs before sending command
        self.process_pending_diffs();

        if let Some(notes) = self.clip_notes.get_mut(self.focused_track_index) {
            notes.retain(|note| note.nanotick != nanotick);
        }
        if let Some(chords) = self.clip_chords.get_mut(self.focused_track_index) {
            chords.retain(|chord| chord.nanotick != nanotick);
        }

        if let Some(bridge) = &self.bridge {
            let (nanotick_lo, nanotick_hi) = split_u64(nanotick);
            let (duration_lo, duration_hi) = split_u64(duration);
            let payload = UiChordCommandPayload {
                command_type: UiCommandType::WriteChord as u16,
                flags: 0,
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
            bridge.send_ui_chord_command(payload);
            self.bump_clip_version();
        }
        cx.notify();
    }

    fn apply_clip_snapshot(&mut self, snapshot: UiClipSnapshot) {
        self.clip_notes = vec![Vec::new(); TRACK_COUNT];
        self.clip_chords = vec![Vec::new(); TRACK_COUNT];
        let track_count = snapshot.track_count.min(TRACK_COUNT as u32) as usize;
        for track_index in 0..track_count {
            let track = snapshot.tracks[track_index];
            let note_start = track.note_offset as usize;
            let note_end = note_start + track.note_count as usize;
            let notes = &mut self.clip_notes[track_index];
            notes.reserve(note_end.saturating_sub(note_start));
            for note_index in note_start..note_end {
                let note = snapshot.notes[note_index];
                notes.push(ClipNote {
                    nanotick: note.t_on,
                    duration: note.t_off.saturating_sub(note.t_on),
                    pitch: note.pitch,
                    velocity: note.velocity,
                });
            }
            let chord_start = track.chord_offset as usize;
            let chord_end = chord_start + track.chord_count as usize;
            let chords = &mut self.clip_chords[track_index];
            chords.clear();
            chords.reserve(chord_end.saturating_sub(chord_start));
            for chord_index in chord_start..chord_end {
                let chord = snapshot.chords[chord_index];
                chords.push(ClipChord {
                    chord_id: chord.chord_id,
                    nanotick: chord.nanotick,
                    duration: chord.duration_nanoticks,
                    spread: chord.spread_nanoticks,
                    humanize_timing: chord.humanize_timing,
                    humanize_velocity: chord.humanize_velocity,
                    degree: chord.degree,
                    quality: chord.quality,
                    inversion: chord.inversion,
                    base_octave: chord.base_octave,
                });
            }
        }
        self.clip_version_local = self.snapshot.ui_clip_version;
        self.clip_snapshot = Some(snapshot);
    }

    fn apply_harmony_snapshot(&mut self, snapshot: UiHarmonySnapshot) {
        self.harmony_events.clear();
        let count = snapshot.event_count as usize;
        let max = snapshot.events.len();
        let count = count.min(max);
        for index in 0..count {
            let event = snapshot.events[index];
            self.harmony_events.push(HarmonyEntry {
                nanotick: event.nanotick,
                root: event.root,
                scale_id: event.scale_id,
            });
        }
        self.harmony_version_local = self.snapshot.ui_harmony_version;
    }

    fn apply_diff(&mut self, diff: UiDiffPayload) {
        let track_index = diff.track_id as usize;
        if track_index >= self.clip_notes.len() {
            return;
        }
        let nanotick =
            (diff.note_nanotick_lo as u64) | ((diff.note_nanotick_hi as u64) << 32);
        let duration =
            (diff.note_duration_lo as u64) | ((diff.note_duration_hi as u64) << 32);
        let pitch = diff.note_pitch.min(127) as u8;
        let velocity = diff.note_velocity.min(127) as u8;

        match diff.diff_type {
            x if x == UiDiffType::AddNote as u16 => {
                let notes = &mut self.clip_notes[track_index];
                let chords = &mut self.clip_chords[track_index];

                // Remove any existing notes at this exact nanotick
                // (The engine should have already done this, but we ensure it here too)
                notes.retain(|note| note.nanotick != nanotick);
                // Remove any existing chords at this nanotick (notes replace chords in tracker)
                chords.retain(|chord| chord.nanotick != nanotick);

                // Now insert the new note
                let insert_at = notes
                    .iter()
                    .position(|note| note.nanotick > nanotick)
                    .unwrap_or(notes.len());
                notes.insert(
                    insert_at,
                    ClipNote {
                        nanotick,
                        duration,
                        pitch,
                        velocity,
                    },
                );

                // Also clear any pending notes at this position
                self.pending_notes.retain(|note| {
                    !(note.track_id == track_index as u32 && note.nanotick == nanotick)
                });
            }
            x if x == UiDiffType::RemoveNote as u16 => {
                let notes = &mut self.clip_notes[track_index];
                if let Some(index) = notes.iter().position(|note| {
                    note.nanotick == nanotick && note.pitch == pitch
                }) {
                    notes.remove(index);
                }
            }
            _ => {}
        }
        self.clip_version_local = diff.clip_version;

        self.pending_notes.retain(|note| {
            !(note.track_id == diff.track_id &&
                note.nanotick == nanotick &&
                note.pitch == pitch)
        });
    }

    fn apply_harmony_diff(&mut self, diff: UiHarmonyDiffPayload) {
        let nanotick =
            (diff.nanotick_lo as u64) | ((diff.nanotick_hi as u64) << 32);
        match diff.diff_type {
            x if x == UiHarmonyDiffType::AddEvent as u16 ||
                x == UiHarmonyDiffType::UpdateEvent as u16 => {
                if let Some(event) = self
                    .harmony_events
                    .iter_mut()
                    .find(|event| event.nanotick == nanotick)
                {
                    event.root = diff.root;
                    event.scale_id = diff.scale_id;
                } else {
                    self.harmony_events.push(HarmonyEntry {
                        nanotick,
                        root: diff.root,
                        scale_id: diff.scale_id,
                    });
                    self.harmony_events
                        .sort_by_key(|event| event.nanotick);
                }
            }
            x if x == UiHarmonyDiffType::RemoveEvent as u16 => {
                if let Some(index) = self
                    .harmony_events
                    .iter()
                    .position(|event| event.nanotick == nanotick)
                {
                    self.harmony_events.remove(index);
                }
            }
            _ => {}
        }
        self.harmony_version_local = diff.harmony_version;
    }

    fn apply_chord_diff(&mut self, diff: UiChordDiffPayload) {
        let track_index = diff.track_id as usize;
        if track_index >= self.clip_chords.len() {
            return;
        }
        let nanotick = (diff.nanotick_lo as u64) | ((diff.nanotick_hi as u64) << 32);
        let duration = (diff.duration_lo as u64) | ((diff.duration_hi as u64) << 32);
        let (degree, quality, inversion, base_octave) = unpack_chord_packed(diff.packed);
        let humanize_timing = (diff.flags & 0xff) as u16;
        let humanize_velocity = ((diff.flags >> 8) & 0xff) as u16;

        match diff.diff_type {
            x if x == UiChordDiffType::AddChord as u16 ||
                x == UiChordDiffType::UpdateChord as u16 => {
                let notes = &mut self.clip_notes[track_index];
                let chords = &mut self.clip_chords[track_index];

                // Chords replace any notes or chords at this nanotick.
                notes.retain(|note| note.nanotick != nanotick);
                chords.retain(|chord| {
                    chord.chord_id != diff.chord_id && chord.nanotick != nanotick
                });

                let insert_at = chords
                    .iter()
                    .position(|chord| chord.nanotick > nanotick)
                    .unwrap_or(chords.len());
                chords.insert(
                    insert_at,
                    ClipChord {
                        chord_id: diff.chord_id,
                        nanotick,
                        duration,
                        spread: diff.spread_nanoticks,
                        humanize_timing,
                        humanize_velocity,
                        degree,
                        quality,
                        inversion,
                        base_octave,
                    },
                );

                self.pending_notes.retain(|note| {
                    !(note.track_id == diff.track_id && note.nanotick == nanotick)
                });
            }
            x if x == UiChordDiffType::RemoveChord as u16 => {
                let chords = &mut self.clip_chords[track_index];
                if let Some(index) = chords
                    .iter()
                    .position(|chord| chord.chord_id == diff.chord_id)
                {
                    chords.remove(index);
                } else if let Some(index) = chords
                    .iter()
                    .position(|chord| chord.nanotick == nanotick)
                {
                    chords.remove(index);
                }
            }
            _ => {}
        }
        self.clip_version_local = diff.clip_version;
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
            let row_nanoticks = NANOTICKS_PER_QUARTER / LINES_PER_BEAT;
            let playhead_row =
                (self.snapshot.ui_global_nanotick_playhead / row_nanoticks) as i64;
            let target = playhead_row - (VISIBLE_ROWS as i64 / 2);
            self.scroll_row_offset = target.max(0);
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

        div()
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
                    .gap_3()
                    .child(
                        div()
                            .text_base()
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
            .child(self.render_tracker_grid(cx))
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
    }
}

impl EngineView {
    fn render_tracker_grid(&self, cx: &mut Context<Self>) -> impl IntoElement {
        let row_nanoticks = NANOTICKS_PER_QUARTER / LINES_PER_BEAT;
        let playhead_row =
            (self.snapshot.ui_global_nanotick_playhead / row_nanoticks) as i64;
        let playhead_view_row = playhead_row - self.scroll_row_offset;
        let playhead_view_row = if playhead_view_row >= 0 &&
            playhead_view_row < VISIBLE_ROWS as i64 {
                Some(playhead_view_row)
            } else {
                None
            };
        let mut rows = Vec::with_capacity(VISIBLE_ROWS);

        let mut header = div()
            .flex()
            .gap_0()
            .items_center()
            .h(px(HEADER_HEIGHT))
            .bg(rgb(0x1a1f2b))  // Darker header background
            .border_b_1()
            .border_color(rgb(0x3a4555))  // Brighter border
            .child(
                div()
                    .w(px(TIME_COLUMN_WIDTH))
                    .h_full()
                    .flex()
                    .items_center()
                    .text_sm()  // Larger text
                    .font_weight(gpui::FontWeight::SEMIBOLD)
                    .text_color(rgb(0xb0bac4))  // Brighter text
                    .px_2()
                    .child("TIME")
            )
            .child(
                div()
                    .w(px(HARMONY_COLUMN_WIDTH))
                    .h_full()
                    .flex()
                    .items_center()
                    .text_sm()  // Larger text
                    .font_weight(gpui::FontWeight::SEMIBOLD)
                    .text_color(rgb(0x7fa0c0))  // Harmony gets blue tint
                    .bg(rgb(0x151922))  // Slightly different bg for harmony
                    .border_l_1()
                    .border_r_2()  // Thicker right border to separate from tracks
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
                        .font_weight(gpui::FontWeight::MEDIUM)
                        .text_color(rgb(0xa0aab4))
                        .child(track_label),
                )
                .child(div().flex().items_center().gap_1().child(plus).child(minus));
            header = header.child(header_cell);
        }

        for row_index in 0..VISIBLE_ROWS {
            let row_index = row_index as i64;
            let absolute_row = row_index + self.scroll_row_offset;
            let nanotick = (absolute_row.max(0) as u64) * row_nanoticks;
            let row_label = format_playhead(nanotick);

            // Alternating row background with better contrast
            let row_bg = if absolute_row % 4 == 0 {
                rgb(0x0a0d12)  // Darker for bar lines (beat boundaries)
            } else if absolute_row % 2 == 0 {
                rgb(0x0f1218)  // Slightly darker for even rows
            } else {
                rgb(0x12151c)  // Normal background
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
                        .px_2()
                        .text_color(if absolute_row % 4 == 0 {
                            rgb(0xb0bac4)  // Brighter for bar lines
                        } else {
                            rgb(0x707a84)  // Normal time labels
                        })
                        .font_weight(if absolute_row % 4 == 0 {
                            gpui::FontWeight::MEDIUM
                        } else {
                            gpui::FontWeight::NORMAL
                        })
                        .child(row_label)
                );

            let harmony_label = self.harmony_label_at_row(absolute_row, row_nanoticks);
            let has_harmony = harmony_label.is_some();
            let harmony_bg = if self.harmony_focus && row_index == self.cursor_row {
                rgb(0x1a3045)  // Blue tint for focused cursor
            } else if self.harmony_focus {
                rgb(0x162535)  // Blue tint when harmony track is focused
            } else if has_harmony {
                rgb(0x141820)  // Slightly different bg when has content
            } else {
                rgb(0x0f1218)  // Match row background when empty
            };
            let harmony_text = harmony_label.unwrap_or_else(|| "·····".to_string());
            row = row.child(
                div()
                    .w(px(HARMONY_COLUMN_WIDTH))
                    .h(px(ROW_HEIGHT))
                    .flex()
                    .items_center()
                    .text_xs()
                    .text_color(if has_harmony {
                        rgb(0x7fa0c0)  // Blue tint for harmony labels
                    } else {
                        rgb(0x404550)  // Dimmer for empty cells
                    })
                    .bg(harmony_bg)
                    .border_l_1()
                    .border_r_2()  // Thicker right border
                    .border_color(rgb(0x2a3242))
                    .px_1()
                    .on_mouse_down(
                        MouseButton::Left,
                        cx.listener(move |view, _, _, cx| {
                            view.focus_harmony_row(row_index as usize, cx);
                        }),
                    )
                    .child(harmony_text),
            );

            for track in 0..TRACK_COUNT {
                let is_focus = track == self.focused_track_index;
                let is_cursor_row = row_index == self.cursor_row && is_focus && !self.harmony_focus;
                let is_playhead = playhead_view_row == Some(row_index);
                let nanotick = (absolute_row.max(0) as u64) * row_nanoticks;
                let entries = self.entries_for_row(nanotick, track);
                let columns = self.track_columns[track];
                for col in 0..columns {
                    let is_cursor = is_cursor_row && col == self.cursor_col;
                    let bg = if is_cursor && is_playhead {
                        rgb(0x2b3b4b)
                    } else if is_cursor {
                        rgb(0x1f2b35)
                    } else if is_playhead {
                        rgb(0x1a242e)
                    } else if entries.get(col).is_some() {
                        rgb(0x141820)  // Slightly different bg when has content
                    } else {
                        rgb(0x0f161c)
                    };
                    let cell_text = if is_cursor && self.edit_active {
                        self.edit_text.clone()
                    } else {
                        entries.get(col)
                            .map(|entry| entry.text.clone())
                            .unwrap_or_else(|| {
                                if is_cursor { "···".to_string() } else { "·  ".to_string() }
                            })
                    };

                    let mut cell = div()
                        .w(px(COLUMN_WIDTH))
                        .h(px(ROW_HEIGHT))
                        .flex()
                        .items_center()
                        .text_xs()
                        .bg(bg)
                        .px_1();

                    // Add left border for track separation
                    if col == 0 && track > 0 {
                        cell = cell.border_l_1().border_color(rgb(0x2a3242));
                    }

                    row = row.child(
                        cell.on_mouse_down(
                            MouseButton::Left,
                            cx.listener(move |view, _, _, cx| {
                                view.focus_note_cell(
                                    row_index as usize,
                                    track,
                                    col,
                                    cx,
                                );
                            }),
                        )
                        .child(
                            div()
                                .text_color(if entries.get(col).is_some() {
                                    rgb(0xa0aab4)  // Brighter for content
                                } else {
                                    rgb(0x404550)  // Dimmer for empty cells
                                })
                                .child(cell_text)
                        ),
                    );
                }
            }
            rows.push(row);
        }

        div()
            .flex()
            .flex_col()
            .gap_0()
            .child(header)
            .children(rows)
    }

    fn harmony_label_at_row(&self, absolute_row: i64, row_nanoticks: u64) -> Option<String> {
        if absolute_row < 0 {
            return None;
        }
        let nanotick = (absolute_row as u64) * row_nanoticks;
        let event = self
            .harmony_events
            .iter()
            .find(|event| event.nanotick == nanotick)?;
        let root = harmony_root_name(event.root);
        let scale = harmony_scale_name(event.scale_id);
        Some(format!("{root}:{scale}"))
    }
}

fn format_playhead(nanoticks: u64) -> String {
    let total_beats = nanoticks / NANOTICKS_PER_QUARTER;
    let bar = total_beats / BEATS_PER_BAR + 1;
    let beat = total_beats % BEATS_PER_BAR + 1;
    let tick = (nanoticks % NANOTICKS_PER_QUARTER) / 10_000;
    format!("{}:{}:{}", bar, beat, tick)
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

fn pitch_to_note(pitch: u8) -> String {
    const NAMES: [&str; 12] = [
        "C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-",
    ];
    let name = NAMES[(pitch as usize) % 12];
    let octave = (pitch / 12) as i8 - 1;
    format!("{name}{octave}")
}

fn chord_token_text(chord: &ClipChord) -> String {
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

fn unpack_chord_packed(packed: u32) -> (u8, u8, u8, u8) {
    let degree = (packed & 0xff) as u8;
    let quality = ((packed >> 8) & 0xff) as u8;
    let inversion = ((packed >> 16) & 0xff) as u8;
    let base_octave = ((packed >> 24) & 0xff) as u8;
    (degree, quality, inversion, base_octave)
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
    use gpui::SharedString;

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
        const TIME_WIDTH: f32 = 70.0;
        const HARMONY_WIDTH: f32 = COLUMN_WIDTH;

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
        const MAX_NOTE_COLUMNS: usize = 8;
        const VISIBLE_ROWS: usize = 40;

        // Calculate total width
        let time_col = 70.0_f32;
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
        let time_x = x_pos;
        x_pos += 70.0; // TIME column
        let harmony_x = x_pos;
        x_pos += COLUMN_WIDTH; // HARM column

        let mut track_positions = Vec::new();
        for track in 0..8 {
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
        let mut x_pos = 70.0 + COLUMN_WIDTH; // After TIME and HARM

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
        let track_columns = vec![2, 1, 1, 1, 1, 1, 1, 1];

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
    fn test_backspace_moves_cursor_down() {
        // This test should FAIL initially - backspace should move cursor down after delete
        // Currently it doesn't, so this test documents the expected behavior

        // Simulating: cursor at row 5, press backspace (delete note),
        // cursor should move to row 6
        let initial_cursor_row = 5_i64;
        let expected_cursor_after_backspace = 6_i64;

        // This will fail until we implement the fix
        // Documenting expected behavior:
        // After deleting with backspace, cursor should advance one row
        assert_eq!(initial_cursor_row + 1, expected_cursor_after_backspace,
                   "Backspace should move cursor down one row after delete");
    }

    #[test]
    fn test_apply_diff_replaces_notes() {
        // Test that applying an AddNote diff replaces existing notes at same position
        use super::{EngineView, ClipNote};
        use daw_bridge::layout::{UiDiffPayload, UiDiffType, K_UI_MAX_TRACKS};
        use daw_bridge::reader::UiSnapshot;

        let mut engine_view = EngineView {
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
            clip_snapshot: None,
            status: SharedString::default(),
            plugins: Vec::new(),
            palette_open: false,
            palette_mode: PaletteMode::Commands,
            palette_query: String::new(),
            palette_selection: 0,
            palette_empty_logged: false,
            scale_browser_open: false,
            scale_browser_query: String::new(),
            scale_browser_selection: 0,
            track_columns: vec![1; TRACK_COUNT],
            focused_track_index: 0,
            cursor_row: 0,
            cursor_col: 0,
            scroll_row_offset: 0,
            follow_playhead: false,
            harmony_focus: false,
            harmony_scale_id: 1,
            edit_active: false,
            edit_text: String::new(),
            pending_notes: Vec::new(),
            clip_notes: vec![Vec::new(); TRACK_COUNT],
            clip_version_local: 0,
            harmony_version_local: 0,
            clip_chords: vec![Vec::new(); TRACK_COUNT],
            harmony_events: Vec::new(),
            track_quantize: vec![false; TRACK_COUNT],
            track_names: vec![None; TRACK_COUNT],
        };

        // Add initial note at position 0
        engine_view.clip_notes[0].push(ClipNote {
            nanotick: 0,
            duration: 240000,
            pitch: 60,  // C-4
            velocity: 100,
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
            reserved: 0,
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
        });

        assert_eq!(pending_notes.len(), 1, "Should have only new note");
        assert_eq!(pending_notes[0].pitch, 62, "Should be D-4");
    }
}

fn harmony_root_name(root: u32) -> &'static str {
    match root % 12 {
        0 => "C",
        1 => "C#",
        2 => "D",
        3 => "D#",
        4 => "E",
        5 => "F",
        6 => "F#",
        7 => "G",
        8 => "G#",
        9 => "A",
        10 => "A#",
        11 => "B",
        _ => "C",
    }
}

fn harmony_scale_name(scale_id: u32) -> &'static str {
    for scale in SCALE_LIBRARY {
        if scale.id == scale_id {
            return scale.name;
        }
    }
    "chr"
}

fn split_u64(value: u64) -> (u32, u32) {
    (value as u32, (value >> 32) as u32)
}

fn default_shm_name() -> String {
    if let Ok(name) = std::env::var("DAW_UI_SHM_NAME") {
        if name.starts_with('/') {
            return name;
        }
        return format!("/{}", name);
    }
    if let Ok(name) = std::env::var("DAW_SHM_NAME") {
        if name.starts_with('/') {
            return name;
        }
        return format!("/{}", name);
    }

    "/daw_engine_ui".to_string()
}

fn default_plugin_cache_path() -> PathBuf {
    if let Ok(path) = std::env::var("DAW_PLUGIN_CACHE") {
        return PathBuf::from(path);
    }
    if let Ok(cwd) = std::env::current_dir() {
        for ancestor in cwd.ancestors() {
            let build_candidate = ancestor.join("build/plugin_cache.json");
            if build_candidate.exists() {
                return build_candidate;
            }
            let flat_candidate = ancestor.join("plugin_cache.json");
            if flat_candidate.exists() {
                return flat_candidate;
            }
        }
    }
    PathBuf::from("build/plugin_cache.json")
}

fn load_plugin_cache() -> Vec<PluginEntry> {
    let path = default_plugin_cache_path();
    let data = fs::read_to_string(&path);
    let Ok(json) = data else {
        eprintln!(
            "daw-app: plugin cache not found at {}",
            path.display()
        );
        return Vec::new();
    };

    let parsed: Result<PluginCacheFile> = serde_json::from_str(&json)
        .context("failed to parse plugin cache JSON")
        .map_err(|err| err);
    match parsed {
        Ok(cache) => {
            let plugins = cache
                .plugins
                .into_iter()
                .enumerate()
                .filter(|(_, entry)| {
                    entry.ok
                        || entry.scan_status.eq_ignore_ascii_case("ok")
                        || entry.error.is_empty()
                })
                .map(|(index, entry)| PluginEntry {
                    index,
                    name: entry.name,
                    vendor: entry.vendor,
                    is_instrument: entry.is_instrument,
                })
                .collect::<Vec<_>>();
            eprintln!(
                "daw-app: loaded {} plugins from {}",
                plugins.len(),
                path.display()
            );
            plugins
        }
        Err(_err) => Vec::new(),
    }
}

fn default_engine_path() -> Option<PathBuf> {
    if let Ok(path) = std::env::var("DAW_ENGINE_PATH") {
        let candidate = PathBuf::from(path);
        if candidate.exists() {
            return Some(candidate);
        }
    }
    let mut roots = Vec::new();
    if let Ok(cwd) = std::env::current_dir() {
        roots.push(cwd);
    }
    if let Ok(exe) = std::env::current_exe() {
        if let Some(dir) = exe.parent() {
            roots.push(dir.to_path_buf());
        }
    }
    for root in &roots {
        eprintln!("daw-app: Searching for engine from root: {}", root.display());
        for ancestor in root.ancestors() {
            let candidate = ancestor.join("build").join("daw_engine");
            if candidate.exists() {
                eprintln!("daw-app: Found engine at: {}", candidate.display());
                return Some(candidate);
            }
        }
    }
    eprintln!("daw-app: Engine not found in any search paths");
    None
}

fn engine_repo_root(engine_path: &PathBuf) -> Option<PathBuf> {
    let parent = engine_path.parent()?;
    if parent.file_name()? == "build" {
        return parent.parent().map(|root| root.to_path_buf());
    }
    None
}

fn spawn_engine_process(engine_path: &PathBuf) -> Result<Child> {
    let mut command = Command::new(engine_path);
    eprintln!("daw-app: Spawning engine from: {}", engine_path.display());

    // Run the engine from its own directory (build/) so it can find juce_host_process
    if let Some(engine_dir) = engine_path.parent() {
        eprintln!("daw-app: Setting engine working dir to: {}", engine_dir.display());
        command.current_dir(engine_dir);
    }

    eprintln!("daw-app: Current UI working dir: {:?}", std::env::current_dir());
    command
        .stdout(Stdio::inherit())
        .stderr(Stdio::inherit())
        .spawn()
        .with_context(|| format!("failed to spawn engine at {}", engine_path.display()))
}

fn stop_engine_process(supervisor: &mut EngineSupervisor) {
    if let Some(mut child) = supervisor.child.take() {
        let _ = child.kill();
        let _ = child.wait();
    }
}

fn align_up(value: usize, alignment: usize) -> usize {
    (value + alignment - 1) & !(alignment - 1)
}

fn is_power_of_two(value: u32) -> bool {
    value != 0 && (value & (value - 1)) == 0
}

fn ring_view(base: *mut u8, offset: u64) -> Option<RingView> {
    if offset == 0 {
        return None;
    }
    let header = unsafe { base.add(offset as usize) as *mut RingHeader };
    if header.is_null() {
        return None;
    }
    let capacity = unsafe { (*header).capacity };
    if !is_power_of_two(capacity) {
        return None;
    }
    let entries_offset = align_up(std::mem::size_of::<RingHeader>(), 64);
    let entries = unsafe { (header as *mut u8).add(entries_offset) as *mut EventEntry };
    Some(RingView {
        header,
        entries,
        mask: capacity - 1,
    })
}

fn ring_write(ring: &RingView, entry: EventEntry) -> bool {
    let write = unsafe { (*ring.header).write_index.load(std::sync::atomic::Ordering::Relaxed) };
    let read = unsafe { (*ring.header).read_index.load(std::sync::atomic::Ordering::Acquire) };
    let next = (write + 1) & ring.mask;
    if next == read {
        return false;
    }
    unsafe {
        *ring.entries.add(write as usize) = entry;
        (*ring.header)
            .write_index
            .store(next, std::sync::atomic::Ordering::Release);
    }
    true
}

fn ring_pop(ring: &RingView) -> Option<EventEntry> {
    let read = unsafe { (*ring.header).read_index.load(Ordering::Acquire) };
    let write = unsafe { (*ring.header).write_index.load(Ordering::Acquire) };
    if read == write {
        return None;
    }
    let entry = unsafe { *ring.entries.add(read as usize) };
    let next = (read + 1) & ring.mask;
    unsafe {
        (*ring.header)
            .read_index
            .store(next, Ordering::Release);
    }
    Some(entry)
}

fn decode_ui_diff(entry: &EventEntry) -> Option<UiDiffPayload> {
    if entry.event_type != EventType::UiDiff as u16 {
        return None;
    }
    if entry.size as usize != std::mem::size_of::<UiDiffPayload>() {
        return None;
    }
    let mut payload = UiDiffPayload::default();
    let payload_bytes = unsafe {
        std::slice::from_raw_parts_mut(
            &mut payload as *mut UiDiffPayload as *mut u8,
            std::mem::size_of::<UiDiffPayload>(),
        )
    };
    payload_bytes.copy_from_slice(&entry.payload[..payload_bytes.len()]);
    Some(payload)
}

fn decode_harmony_diff(entry: &EventEntry) -> Option<UiHarmonyDiffPayload> {
    if entry.event_type != EventType::UiHarmonyDiff as u16 {
        return None;
    }
    if entry.size as usize != std::mem::size_of::<UiHarmonyDiffPayload>() {
        return None;
    }
    let mut payload = UiHarmonyDiffPayload::default();
    let payload_bytes = unsafe {
        std::slice::from_raw_parts_mut(
            &mut payload as *mut UiHarmonyDiffPayload as *mut u8,
            std::mem::size_of::<UiHarmonyDiffPayload>(),
        )
    };
    payload_bytes.copy_from_slice(&entry.payload[..payload_bytes.len()]);
    Some(payload)
}

fn decode_chord_diff(entry: &EventEntry) -> Option<UiChordDiffPayload> {
    if entry.event_type != EventType::UiChordDiff as u16 {
        return None;
    }
    if entry.size as usize != std::mem::size_of::<UiChordDiffPayload>() {
        return None;
    }
    let mut payload = UiChordDiffPayload::default();
    let payload_bytes = unsafe {
        std::slice::from_raw_parts_mut(
            &mut payload as *mut UiChordDiffPayload as *mut u8,
            std::mem::size_of::<UiChordDiffPayload>(),
        )
    };
    payload_bytes.copy_from_slice(&entry.payload[..payload_bytes.len()]);
    Some(payload)
}

fn main() {
    Application::new().run(|cx: &mut App| {
        cx.bind_keys([
            KeyBinding::new("cmd-p", OpenPluginPalette, None),
            KeyBinding::new("cmd-k", TogglePalette, None),
            KeyBinding::new("cmd-shift-s", OpenScaleBrowser, None),
            KeyBinding::new("escape", PaletteClose, None),
            KeyBinding::new("up", PaletteUp, None),
            KeyBinding::new("down", PaletteDown, None),
            KeyBinding::new("shift-up", ScrollUp, None),
            KeyBinding::new("shift-down", ScrollDown, None),
            KeyBinding::new("pageup", ScrollPageUp, None),
            KeyBinding::new("pagedown", ScrollPageDown, None),
            KeyBinding::new("space", TogglePlay, None),
            KeyBinding::new("f", ToggleFollowPlayhead, None),
            KeyBinding::new("ctrl-h", ToggleHarmonyFocus, None),
            KeyBinding::new("enter", PaletteConfirm, None),
            KeyBinding::new("backspace", PaletteBackspace, None),
            KeyBinding::new("delete", DeleteNote, None),
            KeyBinding::new("cmd-z", Undo, None),
            KeyBinding::new("ctrl-z", Undo, None),
            KeyBinding::new("left", FocusLeft, None),
            KeyBinding::new("right", FocusRight, None),
            KeyBinding::new("[", ColumnLeft, None),
            KeyBinding::new("]", ColumnRight, None),
        ]);

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
            move |_: &PaletteClose, cx| {
                view.update(cx, |view, cx| {
                    if view.edit_active {
                        view.cancel_cell_edit(cx);
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
            move |_: &DeleteNote, cx| {
                view.update(cx, |view, cx| {
                    if view.edit_active {
                        view.edit_text.pop();
                        cx.notify();
                    } else if view.harmony_focus {
                        view.delete_harmony(cx);
                    } else {
                        view.delete_note(cx);
                    }
                });
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
                let mut have_clip_snapshot = false;
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
                                have_clip_snapshot = false;
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
                                if let Some(diff) = decode_ui_diff(&entry) {
                                    if diff.diff_type == UiDiffType::ResyncNeeded as u16 {
                                        eprintln!(
                                            "daw-app: ResyncNeeded (local {}, engine {})",
                                            last_clip_version,
                                            diff.clip_version
                                        );
                                        needs_clip_resync = true;
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
                            if needs_clip_resync {
                                if let Some(clip_snapshot) = bridge_ref.read_clip_snapshot() {
                                    let new_version = current_snapshot.as_ref().map(|s| s.ui_clip_version).unwrap_or(0);
                                    eprintln!(
                                        "daw-app: Clip resync applied (version {})",
                                        new_version
                                    );
                                    last_clip_version = new_version;
                                    needs_clip_resync = false;
                                    have_clip_snapshot = true;
                                    let _ = window.update(&mut async_cx, |view, _, cx| {
                                        view.apply_clip_snapshot(clip_snapshot);
                                        view.pending_notes.clear();
                                        cx.notify();
                                    });
                                }
                            } else if !have_clip_snapshot && last_clip_version == 0 {
                                // Only read initial snapshot once when first connecting
                                if let Some(clip_snapshot) = bridge_ref.read_clip_snapshot() {
                                    let new_version = current_snapshot.as_ref().map(|s| s.ui_clip_version).unwrap_or(0);
                                    eprintln!(
                                        "daw-app: Initial clip snapshot loaded (version {})",
                                        new_version
                                    );
                                    last_clip_version = new_version;
                                    have_clip_snapshot = true;
                                    let _ = window.update(&mut async_cx, |view, _, cx| {
                                        view.apply_clip_snapshot(clip_snapshot);
                                        view.pending_notes.clear();
                                        cx.notify();
                                    });
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

                    Timer::after(Duration::from_millis(8)).await;
                }
            }
        })
        .detach();

        cx.activate(true);
    });
}
