use std::sync::atomic::{AtomicU32, AtomicU64};

/// Must match `kShmMagic` / `kShmVersion` in apps/shared_memory.h. Bump both
/// together whenever `ShmHeader`'s layout changes, so a stale binary on either
/// side of the mapping is rejected instead of silently misreading fields.
pub const K_SHM_MAGIC: u32 = 0x3041_5744;
pub const K_SHM_VERSION: u16 = 24;
pub const K_UI_TRACK_NAME_BYTES: usize = 24;
pub const K_UI_MAX_PATCHER_NODES: usize = 64;
pub const K_UI_MAX_PATCHER_EDGES: usize = 128;

pub const K_UI_MAX_TRACKS: usize = 64;
/// Per-insert metering (roadmap 15b). 16, not 8: a normal channel is EQ, comp, saturator,
/// chorus, delay, reverb, limiter, utility — exactly eight — and running out does not
/// degrade gracefully (a ninth insert shows a dead meter, or worse, another insert's).
pub const K_UI_MAX_METERED_DEVICES: usize = 16;
/// Levels are dBFS MILLIBELS (0 = full scale, ordinary values negative), the same scale as
/// the track meters so gain staging is comparable insert to insert. This sentinel means
/// "silent or below floor" — 0.0 amplitude is -inf dB and must not render as -327 dB.
pub const UI_METER_SILENT: i16 = i16::MIN;
/// deviceId value meaning "this slot holds no insert" — distinct from silence.
pub const UI_METER_NO_DEVICE: u32 = 0xFFFF_FFFF;
/// The master track's stable id (>= K_UI_MAX_TRACKS so per-track note/clip handlers
/// reject it). Published in ui_track_id with UI_TRACK_FLAG_MASTER; address the master
/// by this id. (patcher-is-a-device item 4.)
pub const MASTER_TRACK_ID: u32 = 0xFFFF_0000;
pub const K_UI_MAX_CLIP_NOTES: usize = 4096;
pub const K_UI_MAX_CLIP_CHORDS: usize = 1024;
pub const K_UI_MAX_HARMONY_EVENTS: usize = 512;
pub const K_UI_MAX_SCALES: usize = 32;
pub const K_UI_MAX_SCALE_STEPS: usize = 48;
pub const K_UI_MAX_DEVICE_PARAMS: usize = 256;
// v18 waveform region sizes — mirror shared_memory.h kUi* constants.
pub const K_UI_MAX_AUDIO_SOURCES: usize = 32;
pub const K_UI_MAX_AUDIO_CLIPS: usize = 64;
pub const K_UI_WAVEFORM_SLOTS: usize = 4;
pub const K_UI_WAVEFORM_MAX_PAIRS: usize = 24576;
pub const K_UI_EDIT_BATCH_MAX_OPS: usize = 32;
pub const K_UI_EDIT_BATCH_CAPACITY: usize = 64;
pub const K_CHAIN_DEVICE_ID_AUTO: u32 = 0xFFFF_FFFF;
pub const UI_CLIP_WINDOW_FLAG_COMPLETE: u32 = 1 << 0;
pub const UI_CLIP_WINDOW_FLAG_RESYNC: u32 = 1 << 1;

#[repr(C, align(64))]
pub struct ShmHeader {
    pub magic: u32,
    pub version: u16,
    pub flags: u16,
    pub block_size: u32,
    pub sample_rate: f64,
    pub num_channels_in: u32,
    pub num_channels_out: u32,
    pub num_blocks: u32,
    pub channel_stride_bytes: u32,
    pub audio_in_offset: u64,
    pub audio_out_offset: u64,
    pub ring_std_offset: u64,
    pub ring_ctrl_offset: u64,
    pub ring_ui_offset: u64,
    pub ring_ui_out_offset: u64,
    pub ring_ui_edit_offset: u64,
    pub mailbox_offset: u64,
    pub ui_version: AtomicU64,
    pub ui_visual_sample_count: u64,
    pub ui_global_nanotick_playhead: u64,
    pub ui_track_count: u32,
    pub ui_transport_state: u32,
    pub ui_clip_version: u32,
    /// Tempo at the current playhead, milli-BPM (120000 = 120.000). Repurposed
    /// reserved slot — same offset, no version bump.
    pub ui_tempo_milli_bpm: u32,
    pub ui_clip_offset: u64,
    pub ui_clip_bytes: u64,
    pub ui_harmony_version: u32,
    /// Number of points in the project tempo map (1 = constant tempo).
    pub ui_tempo_point_count: u32,
    pub ui_harmony_offset: u64,
    pub ui_harmony_bytes: u64,
    pub ui_track_peak_rms: [f32; K_UI_MAX_TRACKS],
    // v9 (appended; earlier offsets unchanged, size stays 256). All-tracks
    // published clip snapshot region and the agent's own command ring.
    pub ui_clip_all_offset: u64,
    pub ui_clip_all_bytes: u64,
    pub ring_ui_agent_offset: u64,
    // v10: per-track tracker subdivision (Mock B per-lane grids). Header stays
    // 256 (fits the align(64) tail).
    pub ui_lines_per_beat: [u8; K_UI_MAX_TRACKS],
    // v11 (M3.4): offset of the UiClipExtentRegion (clip boxes for rails).
    pub ui_clip_extent_offset: u64,
    // v12: per-track mixer read-back. Gain in millibels, pan in thousandths,
    // mute/solo in flags (MIXER_FLAG_*). ui_mixer_version moves only on change.
    pub ui_track_gain_millibels: [i32; K_UI_MAX_TRACKS],
    pub ui_track_pan_thousandths: [i32; K_UI_MAX_TRACKS],
    pub ui_track_mix_flags: [u8; K_UI_MAX_TRACKS],
    pub ui_mixer_version: u32,
    // v13: per-track names, nul-padded.
    pub ui_track_name: [[u8; K_UI_TRACK_NAME_BYTES]; K_UI_MAX_TRACKS],
    // v14: byte offset of the published UiPatcherRegion (0 = none). Fits the
    // header's tail padding, so the header size is unchanged.
    pub ui_patcher_offset: u64,
    // v15: loop region (nanoticks), and a load-result signal (ui_load_seq bumps
    // per LoadProject attempt, ui_load_ok is the last result). Ride the tail
    // padding; header size unchanged.
    pub ui_loop_start: u64,
    pub ui_loop_end: u64,
    pub ui_load_seq: u32,
    pub ui_load_ok: u32,
    // v16: byte offset of the published UiScaleRegion (0 = none).
    pub ui_scales_offset: u64,
    // v17: byte offset of the UiDeviceParamsRegion (0 = none).
    pub ui_device_params_offset: u64,
    // v18: byte offsets of the two waveform regions (0 = none). These two u64s push
    // sizeof(ShmHeader) 576 -> 640, so the size/offset asserts below moved with v18.
    pub ui_audio_source_offset: u64,
    pub ui_waveform_offset: u64,
    // v19: song time signature for the ruler + time gutter. Two u32s ride the header's
    // alignment tail padding, so sizeof(ShmHeader) stays 640.
    pub ui_song_time_sig_num: u32,
    pub ui_song_time_sig_den: u32,
    // v20: child-track structure (Movement 4). flags bit0 = collapsed, bit1 = has
    // parent (parent_id is a valid track id, so 0 alone can't distinguish "top-level"
    // from "child of track 0" — read parent_id ONLY when HAS_PARENT is set). Fill the
    // header's tail padding, so sizeof(ShmHeader) stays 640.
    pub ui_track_parent_id: [u32; K_UI_MAX_TRACKS],
    pub ui_track_flags: [u8; K_UI_MAX_TRACKS],
    // v22: a STABLE per-slot track id. The engine never renumbers a slot (RemoveTrack
    // tombstones it via UI_TRACK_FLAG_ABSENT, AddTrack refills the lowest tombstone or
    // appends), so ui_track_count is the EXTENT: iterate 0..count, SKIP absent slots, and
    // key selection/cursor/caches on ui_track_id — never the flat visual position.
    pub ui_track_id: [u32; K_UI_MAX_TRACKS],
    /// v23: the first instrument's name per track (nul-padded), so a surface / the agent
    /// can see what is on a track. Empty when the track has no instrument.
    pub ui_track_device_name: [[u8; K_UI_TRACK_NAME_BYTES]; K_UI_MAX_TRACKS],
    /// v24: byte offset of the published UiDeviceMeterRegion (0 = none). UI SHM only.
    pub ui_device_meter_offset: u64,
    /// v24: the HOST writes its own inserts' meters here, in ITS per-track SHM header.
    /// Host->engine leg only; the UI reads the published region, not this.
    pub host_device_meters: [[i16; 4]; K_UI_MAX_METERED_DEVICES],
}

/// uiTrackFlags bits (Movement 4).
pub const UI_TRACK_FLAG_COLLAPSED: u8 = 1 << 0;
/// Set when ui_track_parent_id is meaningful; without it, parent_id 0 is ambiguous
/// (top-level vs child of track 0).
pub const UI_TRACK_FLAG_HAS_PARENT: u8 = 1 << 1;
/// v22: this slot is a tombstone — a removed track whose id is retired but whose slot is
/// kept so neighbours don't renumber. Skip absent slots when drawing/iterating.
pub const UI_TRACK_FLAG_ABSENT: u8 = 1 << 2;
/// This published entry is the MASTER track (its ui_track_id == MASTER_TRACK_ID): a
/// real device chain + mixer whose output is the master bus, but no arrangement rail
/// and no clips. Render it as the master strip, never as a tracker lane.
pub const UI_TRACK_FLAG_MASTER: u8 = 1 << 3;

/// v14: a published patcher-graph node. `config` is type-interpreted (see the C++
/// UiPatcherNode doc): Euclidean/RandomDegree ints; Lfo floats as milli-units.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct UiPatcherNode {
    pub id: u32,
    pub node_type: u8, // PatcherNodeType
    pub has_config: u8,
    pub reserved: u16,
    pub config: [i32; 8],
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct UiPatcherEdge {
    pub src_node: u32,
    pub src_port: u32,
    pub dst_node: u32,
    pub dst_port: u32,
    pub kind: u8, // PatcherPortKind
    pub reserved: [u8; 3],
}

#[repr(C, align(64))]
pub struct UiPatcherRegion {
    pub version: u32,
    pub device_id: u32,
    pub node_count: u32,
    pub edge_count: u32,
    pub nodes: [UiPatcherNode; K_UI_MAX_PATCHER_NODES],
    pub edges: [UiPatcherEdge; K_UI_MAX_PATCHER_EDGES],
}

// v16/v17 scale + device-param regions are now generated from the C++ header
// (bindgen) rather than hand-mirrored — see the `sys` module. Re-exported under
// their idiomatic names; bindgen's layout_tests + the C++ static_asserts pin them
// to the wire format, so the hand-written offset asserts are gone.
pub use crate::sys::{
    daw_UiDeviceParam as UiDeviceParam, daw_UiDeviceParamsRegion as UiDeviceParamsRegion,
    daw_UiScale as UiScale, daw_UiScaleRegion as UiScaleRegion,
};

// v18 waveform regions, generated from shared_memory.h (bindgen's own layout_tests pin
// their sizes/offsets against the C++ structs).
pub use crate::sys::{
    daw_UiAudioClip as UiAudioClip, daw_UiAudioSource as UiAudioSource,
    daw_UiAudioSourceRegion as UiAudioSourceRegion, daw_UiWaveformRegion as UiWaveformRegion,
    daw_UiWaveformSlot as UiWaveformSlot,
};

#[repr(C, align(64))]
#[derive(Clone, Copy, Debug)]
pub struct EventEntry {
    pub sample_time: u64,
    pub block_id: u32,
    pub event_type: u16,
    pub size: u16,
    pub flags: u32,
    pub payload: [u8; 40],
}

#[repr(C, align(64))]
#[derive(Clone, Copy, Debug)]
pub struct UiEditBatchEntry {
    pub batch_id: u32,
    pub op_count: u32,
    pub ops: [EventEntry; K_UI_EDIT_BATCH_MAX_OPS],
}

#[repr(u16)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum EventType {
    Midi = 1,
    Param = 2,
    Transport = 3,
    ReplayComplete = 4,
    UiCommand = 5,
    UiDiff = 6,
    UiHarmonyDiff = 7,
    UiChordDiff = 8,
}

#[repr(C, align(64))]
pub struct RingHeader {
    pub capacity: u32,
    pub entry_size: u32,
    pub read_index: AtomicU32,
    pub write_index: AtomicU32,
    pub reserved: [u32; 12],
}

#[repr(C, align(64))]
pub struct BlockMailbox {
    pub completed_block_id: AtomicU32,
    pub completed_sample_time: AtomicU64,
    pub replay_ack_sample_time: AtomicU64,
    pub reserved: [u32; 11],
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct UiClipTrack {
    pub track_id: u32,
    pub note_offset: u32,
    pub note_count: u32,
    pub chord_offset: u32,
    pub chord_count: u32,
    pub reserved: u32,
    pub clip_start_nanotick: u64,
    pub clip_end_nanotick: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct UiClipNote {
    pub t_on: u64,
    pub t_off: u64,
    /// Authored EventId: author in the top 16 bits, counter in the low 48.
    pub note_id: u64,
    pub pitch: u8,
    pub velocity: u8,
    pub column: u8,
    /// Row op: 0/1 = one strike, N = N strikes.
    pub retrigger: u8,
    /// Row op: 0 = always sound, 1..=100 = percent chance.
    pub probability: u8,
    /// v11 provenance: bit0 = muted (draw struck-out), bit1 = is_add.
    pub placement_flags: u8,
    /// v11 provenance: which placement this note belongs to.
    pub placement_id: u16,
    /// Row op: onset delay in absolute nanoticks.
    pub delay_nanoticks: u32,
    pub reserved3: u32,
}

pub const UI_CLIP_NOTE_MUTED: u8 = 1 << 0;
pub const UI_CLIP_NOTE_ADD: u8 = 1 << 1;

/// `UiClipExtent.flags`: the rail is an audio region (render a waveform, no
/// notes). Audio clips persist and show as rails but are not scheduled until the
/// Movement 4 audio engine.
pub const UI_CLIP_EXTENT_AUDIO: u32 = 1 << 0;

/// v19: the clip's own musical grid packed into the spare bits of
/// `UiClipExtent.flags`. `linesPerBeat == 0` is the sentinel for "no grid" — fall
/// back to the song meter. Denominator is a power-of-two exponent. See shared_memory.h
/// for the three rules (0 = no grid, clamp-not-truncate, power-of-two denominator).
pub const UI_CLIP_GRID_LPB_SHIFT: u32 = 1;
pub const UI_CLIP_GRID_NUM_SHIFT: u32 = 6;
pub const UI_CLIP_GRID_DEN_EXP_SHIFT: u32 = 11;
pub const UI_CLIP_GRID_LPB_MASK: u32 = 0x1f;
pub const UI_CLIP_GRID_NUM_MASK: u32 = 0x1f;
pub const UI_CLIP_GRID_DEN_EXP_MASK: u32 = 0x7;

/// Decode the per-clip grid from `UiClipExtent.flags`. Returns
/// `Some((lines_per_beat, numerator, denominator))`, or `None` when no grid is
/// published (the caller uses the song meter).
pub fn unpack_clip_grid(flags: u32) -> Option<(u32, u32, u32)> {
    let lpb = (flags >> UI_CLIP_GRID_LPB_SHIFT) & UI_CLIP_GRID_LPB_MASK;
    if lpb == 0 {
        return None;
    }
    let num = (flags >> UI_CLIP_GRID_NUM_SHIFT) & UI_CLIP_GRID_NUM_MASK;
    let exp = (flags >> UI_CLIP_GRID_DEN_EXP_SHIFT) & UI_CLIP_GRID_DEN_EXP_MASK;
    Some((lpb, num, 1u32 << exp))
}

/// v11 (M3.4): a placed clip's timeline box — a rail. `start_tick`/`end_tick` are
/// absolute; `name` is nul-padded. Loose (session) placements are not published.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct UiClipExtent {
    pub placement_id: u32,
    pub clip_id: u32,
    pub track_id: u32,
    pub flags: u32,
    pub start_tick: u64,
    pub end_tick: u64,
    pub name: [u8; 32],
}

pub const K_UI_MAX_CLIP_EXTENTS: usize = 64;

#[repr(C)]
pub struct UiClipExtentRegion {
    pub count: u32,
    pub reserved: u32,
    pub extents: [UiClipExtent; K_UI_MAX_CLIP_EXTENTS],
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct UiClipChord {
    pub nanotick: u64,
    pub duration_nanoticks: u64,
    pub spread_nanoticks: u32,
    pub humanize_timing: u16,
    pub humanize_velocity: u16,
    pub chord_id: u32,
    pub degree: u8,
    pub quality: u8,
    pub inversion: u8,
    pub base_octave: u8,
    pub flags: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct UiClipWindowSnapshot {
    pub track_id: u32,
    pub clip_version: u32,
    pub window_start_nanotick: u64,
    pub window_end_nanotick: u64,
    pub request_id: u32,
    pub cursor_event_index: u32,
    pub next_event_index: u32,
    pub note_count: u32,
    pub chord_count: u32,
    pub flags: u32,
    pub reserved: u32,
    pub notes: [UiClipNote; K_UI_MAX_CLIP_NOTES],
    pub chords: [UiClipChord; K_UI_MAX_CLIP_CHORDS],
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct UiHarmonyEvent {
    pub nanotick: u64,
    pub root: u32,
    pub scale_id: u32,
    pub flags: u32,
    pub reserved: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct UiHarmonySnapshot {
    pub event_count: u32,
    pub reserved: [u32; 3],
    pub events: [UiHarmonyEvent; K_UI_MAX_HARMONY_EVENTS],
}

impl Default for UiClipWindowSnapshot {
    fn default() -> Self {
        unsafe { std::mem::zeroed() }
    }
}

impl Default for UiHarmonySnapshot {
    fn default() -> Self {
        unsafe { std::mem::zeroed() }
    }
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct UiPatcherGraphCommandPayload {
    pub command_type: u16,
    pub flags: u16,
    pub track_id: u32,
    pub base_version: u32,
    pub node_id: u32,
    pub node_type: u32,
    pub src_node_id: u32,
    pub dst_node_id: u32,
    pub src_port_id: u32,
    pub dst_port_id: u32,
    pub edge_kind: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct UiPatcherNodeConfigPayload {
    pub command_type: u16,
    pub flags: u16,
    pub track_id: u32,
    pub base_version: u32,
    pub node_id: u32,
    pub config_type: u32,
    pub config: [u8; 16],
    pub reserved: [u8; 4],
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct UiPatcherGraphDiffPayload {
    pub diff_type: u16,
    pub flags: u16,
    pub track_id: u32,
    pub graph_version: u32,
    pub node_id: u32,
    pub node_type: u32,
    pub src_node_id: u32,
    pub dst_node_id: u32,
    pub src_port_id: u32,
    pub dst_port_id: u32,
    pub edge_kind: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct UiPatcherGraphErrorPayload {
    pub diff_type: u16,
    pub error_code: u16,
    pub track_id: u32,
    pub node_id: u32,
    pub src_node_id: u32,
    pub dst_node_id: u32,
    pub src_port_id: u32,
    pub dst_port_id: u32,
    pub edge_kind: u32,
    pub reserved: [u8; 8],
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct UiPatcherPresetCommandPayload {
    pub command_type: u16,
    pub flags: u16,
    pub track_id: u32,
    pub base_version: u32,
    pub name: [u8; 28],
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub enum UiCommandType {
    None = 0,
    LoadPluginOnTrack = 1,
    WriteNote = 2,
    TogglePlay = 3,
    DeleteNote = 4,
    Undo = 5,
    WriteHarmony = 6,
    DeleteHarmony = 7,
    WriteChord = 8,
    DeleteChord = 9,
    SetTrackHarmonyQuantize = 10,
    Redo = 11,
    SetLoopRange = 12,
    SetAutomationTarget = 13,
    AddDevice = 14,
    RemoveDevice = 15,
    MoveDevice = 16,
    UpdateDevice = 17,
    SetDeviceEuclideanConfig = 18,
    SetTrackRouting = 19,
    AddModLink = 20,
    RemoveModLink = 21,
    SetModLinkUid16 = 22,
    SetModSourceValue = 23,
    OpenPluginEditor = 24,
    AddPatcherNode = 25,
    RemovePatcherNode = 26,
    ConnectPatcherNodes = 27,
    SetPatcherNodeConfig = 28,
    SavePatcherPreset = 29,
    RequestClipWindow = 30,
    /// Both carry a project name in `UiPatcherPresetCommandPayload::name`,
    /// resolved against the engine's project directory.
    SaveProject = 31,
    LoadProject = 32,
    /// Gain in millibels (`value0`, signed), pan in thousandths
    /// (`plugin_index`, signed), mute/solo in `flags`.
    SetTrackMixer = 33,
    /// Halt playback and rewind the transport to the loop start.
    Stop = 34,
    /// Seek the transport to the nanotick in note_nanotick_lo/hi (clamped to the loop).
    SetPosition = 35,
    /// Rename a track (trackId + name in UiPatcherPresetCommandPayload).
    SetTrackName = 36,
    // 37-39 reserved for the frontend's read-back request commands (web-ui branch).
    RequestDeviceParams = 40,
    /// Set the project tempo. value0 = milli-BPM. flags: 0 = insert-or-replace a point
    /// at note_nanotick_lo/hi; 1 = flatten the map to this single tempo.
    SetTempo = 41,
    // 42 = Quit, taken by the frontend on its web-ui branch. Reserved; do not reuse.
    /// Set one plugin parameter from the rack (UiSetParamPayload).
    SetDeviceParam = 43,
    /// Windowed waveform query (UiWaveformRequestPayload); answered into a
    /// UiWaveformRegion seqlock slot from the per-source min/max pyramid.
    RequestWaveform = 44,
    /// Audition a pitch on a track's instrument without writing it (keyjazz).
    /// Reuses UiCommandPayload: trackId, note_pitch = pitch, value0 = velocity,
    /// flags bit0 = on. Held, out of band — never recorded, undoable, or dirtying.
    PreviewNote = 45,
    /// Append an empty top-level track at the current extent (v1: append only). The new
    /// track's id == its stable slot; RemoveTrack never renumbers neighbours.
    AddTrack = 46,
    /// Remove the track whose stable id is in trackId, tombstoning its slot
    /// (UI_TRACK_FLAG_ABSENT). Takes its aux children with it; rejects a child id.
    RemoveTrack = 47,
    /// Arrangement placement ops, keyed on the stable placement id (value0), published in
    /// the clip extent's placementId. See the C++ UiCommandType doc for the field mapping.
    MovePlacement = 48,
    RemovePlacement = 49,
    ResizePlacement = 50,
    AddPlacement = 51,
    /// PANIC: all sound off — CC120 (all-sound-off) AND CC123 (all-notes-off) on every MIDI
    /// channel to every hosted plugin, plus all pending/active note state dropped. CC120 is
    /// the one that matters: 123 releases notes and lets them ring out, which is not a panic.
    Panic = 52,
}

pub const MIXER_FLAG_MUTE: u16 = 1 << 0;
pub const MIXER_FLAG_SOLO: u16 = 1 << 1;
/// PreviewNote flags: bit0 set = note-on, clear = note-off.
pub const PREVIEW_NOTE_FLAG_ON: u16 = 1 << 0;

#[repr(u16)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum UiDiffType {
    None = 0,
    AddNote = 1,
    RemoveNote = 2,
    UpdateNote = 3,
    ResyncNeeded = 4,
    ChainSnapshot = 5,
    ChainError = 6,
    RoutingSnapshot = 7,
    RoutingError = 8,
    ModSnapshot = 9,
    ModError = 10,
    ModLinkUid16 = 11,
    PatcherGraphDelta = 12,
    PatcherGraphError = 13,
    /// v20 (Movement 4): one per audio bus of a device, streamed after that device's
    /// ChainSnapshot diff. See UiBusDiffPayload.
    DeviceBus = 14,
}

#[repr(u16)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum UiHarmonyDiffType {
    None = 0,
    AddEvent = 1,
    RemoveEvent = 2,
    UpdateEvent = 3,
    ResyncNeeded = 4,
}

#[repr(u16)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum UiChordDiffType {
    None = 0,
    AddChord = 1,
    RemoveChord = 2,
    UpdateChord = 3,
    ResyncNeeded = 4,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct UiCommandPayload {
    pub command_type: u16,
    pub flags: u16,
    pub track_id: u32,
    pub plugin_index: u32,
    pub note_pitch: u32,
    pub value0: u32,
    pub note_nanotick_lo: u32,
    pub note_nanotick_hi: u32,
    pub note_duration_lo: u32,
    pub note_duration_hi: u32,
    pub base_version: u32,
}

/// A rack knob write (UiCommandType::SetDeviceParam). Mirrors the C++
/// UiSetParamPayload (40 bytes). value_milli is the normalized value in milli
/// (0..1000); uid16 is the durable param key from the device-params read-back.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct UiSetParamPayload {
    pub command_type: u16,
    pub flags: u16,
    pub track_id: u32,
    pub device_id: u32,
    pub value_milli: u32,
    pub uid16: [u8; 16],
    pub reserved: [u8; 8],
}

/// A windowed waveform query (UiCommandType::RequestWaveform). Mirrors the C++
/// UiWaveformRequestPayload (40 bytes). requestSeq is sidecar-allocated; slot =
/// requestSeq % kUiWaveformSlots. decimation is a power of two >= 1.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct UiWaveformRequestPayload {
    pub command_type: u16,
    pub flags: u16,
    pub request_seq: u32,
    pub source_id: u32,
    pub decimation: u32,
    pub first_frame_lo: u32,
    pub first_frame_hi: u32,
    pub columns: u32,
    pub channel_mask: u32,
    pub reserved0: u32,
    pub reserved1: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct UiClipWindowCommandPayload {
    pub command_type: u16,
    pub flags: u16,
    pub track_id: u32,
    pub request_id: u32,
    pub window_start_lo: u32,
    pub window_start_hi: u32,
    pub window_end_lo: u32,
    pub window_end_hi: u32,
    pub cursor_event_index: u32,
    pub reserved: u32,
    pub reserved2: u32,
}


#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct UiChainCommandPayload {
    pub command_type: u16,
    pub flags: u16,
    pub track_id: u32,
    pub base_version: u32,
    pub device_id: u32,
    pub device_kind: u32,
    pub insert_index: u32,
    pub patcher_node_id: u32,
    pub host_slot_index: u32,
    pub bypass: u32,
    pub reserved: [u8; 4],
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct UiChainDiffPayload {
    pub diff_type: u16,
    pub flags: u16,
    pub track_id: u32,
    pub chain_version: u32,
    pub device_id: u32,
    pub device_kind: u32,
    pub position: u32,
    pub patcher_node_id: u32,
    pub host_slot_index: u32,
    pub capability_mask: u32,
    pub bypass: u32,
}

/// One insert's meters. `device_id` is the STABLE device id from the chain snapshot, NOT a
/// positional index: the host's compacted plugin order skips non-VST devices, so matching
/// by position paints one device's meter on another's card. Match on device_id.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct UiDeviceMeter {
    pub in_peak_mb: i16,
    pub out_peak_mb: i16,
    pub in_rms_mb: i16,
    pub out_rms_mb: i16,
    pub device_id: u32,
}

/// Published per track SLOT, so the MASTER (a real slot with UI_TRACK_FLAG_MASTER) is
/// metered by the same path with no special case. Rewritten every UI frame: an absent
/// track or insert reads UI_METER_NO_DEVICE with silent levels rather than holding a
/// stale value that would look like a stuck meter.
#[repr(C, align(64))]
#[derive(Clone, Copy, Debug)]
pub struct UiDeviceMeterRegion {
    pub version: u32,
    pub reserved: u32,
    pub meters: [[UiDeviceMeter; K_UI_MAX_METERED_DEVICES]; K_UI_MAX_TRACKS],
}


/// v20 (Movement 4): on a ChainSnapshot diff, `flags` low byte is the count of
/// DeviceBus diffs that follow for this device (so a reader draws once); bit8 =
/// bus list truncated at the cap. NOTE: this is `UiChainDiffPayload.flags` (u16, at
/// payload offset 2) — NOT `EventEntry.flags` (u32). Both have a `flags`; decode from
/// the payload's, or a plugin reporting N buses under a busCount of 0 is your only tell.
pub const UI_CHAIN_DIFF_BUS_COUNT_MASK: u16 = 0x00ff;
pub const UI_CHAIN_DIFF_BUS_TRUNCATED: u16 = 1 << 8;
/// bit9 = this device's patcher graph contains an event GENERATOR node (euclidean,
/// random_degree, ...): it emits events the user never wrote. Mark the device — and
/// its track — as a source of "notes I didn't type", so a phantom note is a glance at
/// the chain, not an investigation.
pub const UI_CHAIN_DIFF_GENERATES: u16 = 1 << 9;

/// v20: one audio bus of a hosted plugin, streamed after the device's ChainSnapshot
/// diff. `channel_offset` is the bus's first channel in the flat post-negotiation
/// buffer; `layout_id` is the stable UiBusLayoutId. `name` is nul-PADDED — an exactly-
/// 22-char name has no terminator, so bound decoding by the field width.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct UiBusDiffPayload {
    pub diff_type: u16,
    pub flags: u16, // bit0 isInput, bit1 isMain, bit2 enabled
    pub track_id: u32,
    pub device_id: u32,
    pub index: u8,
    pub channel_count: u8,
    pub layout_id: u16,
    pub channel_offset: u16,
    pub name: [u8; 22],
}

pub const UI_BUS_DIFF_INPUT: u16 = 1 << 0;
pub const UI_BUS_DIFF_MAIN: u16 = 1 << 1;
pub const UI_BUS_DIFF_ENABLED: u16 = 1 << 2;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct UiChainErrorPayload {
    pub diff_type: u16,
    pub error_code: u16,
    pub track_id: u32,
    pub device_id: u32,
    pub device_kind: u32,
    pub insert_index: u32,
    pub reserved: [u32; 5],
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct UiChordCommandPayload {
    pub command_type: u16,
    pub flags: u16,
    pub track_id: u32,
    pub base_version: u32,
    pub nanotick_lo: u32,
    pub nanotick_hi: u32,
    pub duration_lo: u32,
    pub duration_hi: u32,
    pub degree: u16,
    pub quality: u8,
    pub inversion: u8,
    pub base_octave: u8,
    pub humanize_timing: u8,
    pub humanize_velocity: u8,
    pub reserved: u8,
    pub spread_nanoticks: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct UiDiffPayload {
    pub diff_type: u16,
    pub flags: u16,
    pub track_id: u32,
    pub clip_version: u32,
    pub note_nanotick_lo: u32,
    pub note_nanotick_hi: u32,
    pub note_duration_lo: u32,
    pub note_duration_hi: u32,
    pub note_pitch: u32,
    pub note_velocity: u32,
    pub note_column: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct UiHarmonyDiffPayload {
    pub diff_type: u16,
    pub flags: u16,
    pub harmony_version: u32,
    pub nanotick_lo: u32,
    pub nanotick_hi: u32,
    pub root: u32,
    pub scale_id: u32,
    pub reserved0: u32,
    pub reserved1: u32,
    pub reserved2: u32,
    pub reserved3: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct UiChordDiffPayload {
    pub diff_type: u16,
    pub flags: u16,
    pub track_id: u32,
    pub clip_version: u32,
    pub nanotick_lo: u32,
    pub nanotick_hi: u32,
    pub duration_lo: u32,
    pub duration_hi: u32,
    pub chord_id: u32,
    pub spread_nanoticks: u32,
    pub packed: u32,
}

#[cfg(test)]
mod tests {
    use super::*;
    use memoffset::offset_of;
    use static_assertions::const_assert_eq;
    use std::mem::{align_of, size_of};

    /// The bindgen-generated structs (crate::sys, from the C++ header) byte-match
    /// the hand-written mirror here. Once this holds, the cutover can replace these
    /// hand-written structs with the generated ones with no wire change. bindgen's
    /// own layout_tests already pin the generated structs to the C++; this pins the
    /// hand-written ones to the generated, closing the loop.
    #[test]
    fn bindgen_matches_hand_written() {
        use crate::sys;
        macro_rules! same {
            ($hand:ty, $gen:ty) => {
                assert_eq!(
                    size_of::<$hand>(), size_of::<$gen>(),
                    concat!("size mismatch: ", stringify!($hand))
                );
                assert_eq!(
                    align_of::<$hand>(), align_of::<$gen>(),
                    concat!("align mismatch: ", stringify!($hand))
                );
            };
        }
        same!(ShmHeader, sys::daw_ShmHeader);
        same!(RingHeader, sys::daw_RingHeader);
        same!(EventEntry, sys::daw_EventEntry);
        same!(UiEditBatchEntry, sys::daw_UiEditBatchEntry);
        same!(UiPatcherNode, sys::daw_UiPatcherNode);
        same!(UiPatcherEdge, sys::daw_UiPatcherEdge);
        same!(UiPatcherRegion, sys::daw_UiPatcherRegion);
        // UiScale/UiScaleRegion/UiDeviceParam/UiDeviceParamsRegion are now aliases
        // OF the generated structs, so a parity check on them is vacuous.
        same!(UiClipNote, sys::daw_UiClipNote);
    }

    #[test]
    fn clip_window_command_payload_size() {
        assert_eq!(size_of::<UiClipWindowCommandPayload>(), 40);
    }

    #[test]
    fn ui_clip_note_layout_matches_cpp() {
        // Widened for the authored EventId; the C++ side static_asserts the
        // same size, so a mismatch fails at compile time on one end and here
        // on the other.
        const_assert_eq!(size_of::<UiClipNote>(), 40);
        assert_eq!(offset_of!(UiClipNote, t_on), 0);
        assert_eq!(offset_of!(UiClipNote, t_off), 8);
        assert_eq!(offset_of!(UiClipNote, note_id), 16);
        assert_eq!(offset_of!(UiClipNote, pitch), 24);
        assert_eq!(offset_of!(UiClipNote, velocity), 25);
        assert_eq!(offset_of!(UiClipNote, column), 26);
        assert_eq!(offset_of!(UiClipNote, retrigger), 27);
        assert_eq!(offset_of!(UiClipNote, probability), 28);
        assert_eq!(offset_of!(UiClipNote, placement_flags), 29);
        assert_eq!(offset_of!(UiClipNote, placement_id), 30);
        assert_eq!(offset_of!(UiClipNote, delay_nanoticks), 32);
        const_assert_eq!(size_of::<UiClipExtent>(), 64);
        assert_eq!(offset_of!(UiClipExtent, start_tick), 16);
        assert_eq!(offset_of!(UiClipExtent, name), 32);
    }

    #[test]
    fn ui_edit_batch_entry_layout_matches_cpp() {
        const_assert_eq!(size_of::<EventEntry>(), 64);
        const_assert_eq!(size_of::<UiEditBatchEntry>(), 2112);
        const_assert_eq!(align_of::<UiEditBatchEntry>(), 64);
        assert_eq!(offset_of!(UiEditBatchEntry, batch_id), 0);
        assert_eq!(offset_of!(UiEditBatchEntry, op_count), 4);
        assert_eq!(offset_of!(UiEditBatchEntry, ops), 64);
    }

    #[test]
    fn shm_header_layout_matches_cpp() {
        const_assert_eq!(size_of::<ShmHeader>(), 4992); // v24: + meter offset + host meters
        const_assert_eq!(size_of::<UiDeviceMeter>(), 12);
        const_assert_eq!(size_of::<UiDeviceMeterRegion>(), 12352);
        const_assert_eq!(align_of::<ShmHeader>(), 64);
        assert_eq!(offset_of!(ShmHeader, ring_std_offset), 56);
        assert_eq!(offset_of!(ShmHeader, ring_ctrl_offset), 64);
        assert_eq!(offset_of!(ShmHeader, ring_ui_offset), 72);
        assert_eq!(offset_of!(ShmHeader, ring_ui_out_offset), 80);
        assert_eq!(offset_of!(ShmHeader, ring_ui_edit_offset), 88);
        assert_eq!(offset_of!(ShmHeader, mailbox_offset), 96);
        assert_eq!(offset_of!(ShmHeader, ui_version), 104);
        assert_eq!(offset_of!(ShmHeader, ui_visual_sample_count), 112);
        assert_eq!(offset_of!(ShmHeader, ui_global_nanotick_playhead), 120);
        assert_eq!(offset_of!(ShmHeader, ui_track_count), 128);
        assert_eq!(offset_of!(ShmHeader, ui_transport_state), 132);
        assert_eq!(offset_of!(ShmHeader, ui_clip_version), 136);
        assert_eq!(offset_of!(ShmHeader, ui_clip_offset), 144);
        assert_eq!(offset_of!(ShmHeader, ui_clip_bytes), 152);
        assert_eq!(offset_of!(ShmHeader, ui_harmony_version), 160);
        assert_eq!(offset_of!(ShmHeader, ui_harmony_offset), 168);
        assert_eq!(offset_of!(ShmHeader, ui_harmony_bytes), 176);
        assert_eq!(offset_of!(ShmHeader, ui_track_peak_rms), 184);
        // v21: kUiMaxTracks 8 -> 64 widened every per-track array, so all offsets after
        // the first one (ui_track_peak_rms) shifted. Recomputed from the C++ header.
        assert_eq!(offset_of!(ShmHeader, ui_clip_all_offset), 440);
        assert_eq!(offset_of!(ShmHeader, ui_clip_all_bytes), 448);
        assert_eq!(offset_of!(ShmHeader, ring_ui_agent_offset), 456);
        assert_eq!(offset_of!(ShmHeader, ui_lines_per_beat), 464);
        assert_eq!(offset_of!(ShmHeader, ui_clip_extent_offset), 528);
        assert_eq!(offset_of!(ShmHeader, ui_track_gain_millibels), 536);
        assert_eq!(offset_of!(ShmHeader, ui_track_pan_thousandths), 792);
        assert_eq!(offset_of!(ShmHeader, ui_track_mix_flags), 1048);
        assert_eq!(offset_of!(ShmHeader, ui_mixer_version), 1112);
        assert_eq!(offset_of!(ShmHeader, ui_track_name), 1116); // v13
        assert_eq!(offset_of!(ShmHeader, ui_patcher_offset), 2656); // v14
        assert_eq!(offset_of!(ShmHeader, ui_loop_start), 2664); // v15
        assert_eq!(offset_of!(ShmHeader, ui_loop_end), 2672);
        assert_eq!(offset_of!(ShmHeader, ui_load_seq), 2680);
        assert_eq!(offset_of!(ShmHeader, ui_load_ok), 2684);
        assert_eq!(offset_of!(ShmHeader, ui_scales_offset), 2688); // v16
        assert_eq!(offset_of!(ShmHeader, ui_device_params_offset), 2696); // v17
        assert_eq!(offset_of!(ShmHeader, ui_audio_source_offset), 2704); // v18
        assert_eq!(offset_of!(ShmHeader, ui_waveform_offset), 2712);
        assert_eq!(offset_of!(ShmHeader, ui_song_time_sig_num), 2720); // v19
        assert_eq!(offset_of!(ShmHeader, ui_song_time_sig_den), 2724);
        assert_eq!(offset_of!(ShmHeader, ui_track_parent_id), 2728); // v20
        assert_eq!(offset_of!(ShmHeader, ui_track_flags), 2984);
        assert_eq!(offset_of!(ShmHeader, ui_track_id), 3048); // v22 (appended at the end)
        assert_eq!(offset_of!(ShmHeader, ui_track_device_name), 3304); // v23
        assert_eq!(offset_of!(ShmHeader, ui_device_meter_offset), 4840); // v24
        assert_eq!(offset_of!(ShmHeader, host_device_meters), 4848); // v24
        // The scale + device-param region structs (v16/v17) are now generated from
        // the C++ header; bindgen's own layout_tests pin them, so no hand offsets.
        const_assert_eq!(size_of::<UiPatcherNode>(), 40);
        const_assert_eq!(size_of::<UiPatcherEdge>(), 20);
        const_assert_eq!(size_of::<UiPatcherRegion>(), 5184);
        const_assert_eq!(size_of::<UiBusDiffPayload>(), 40); // v20, fits EventEntry
        // v18 waveform structs are bindgen-generated; pin the element sizes, then tie
        // each hand constant to the generated region size so neither can drift: if a
        // K_* count is wrong the region no longer sums, and this fails to compile.
        const_assert_eq!(size_of::<UiAudioSource>(), 320);
        const_assert_eq!(size_of::<UiAudioClip>(), 64);
        const_assert_eq!(
            size_of::<UiAudioSourceRegion>(),
            64 + K_UI_MAX_AUDIO_SOURCES * 320 + K_UI_MAX_AUDIO_CLIPS * 64
        );
        const_assert_eq!(
            size_of::<UiWaveformSlot>(),
            64 + K_UI_WAVEFORM_MAX_PAIRS * 4 // 64 B header + [i16; MAX_PAIRS*2]
        );
        const_assert_eq!(
            size_of::<UiWaveformRegion>(),
            64 + K_UI_WAVEFORM_SLOTS * size_of::<UiWaveformSlot>()
        );
    }
}
