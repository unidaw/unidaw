use std::sync::atomic::{AtomicU32, AtomicU64};

/// Must match `kShmMagic` / `kShmVersion` in apps/shared_memory.h. Bump both
/// together whenever `ShmHeader`'s layout changes, so a stale binary on either
/// side of the mapping is rejected instead of silently misreading fields.
pub const K_SHM_MAGIC: u32 = 0x3041_5744;
pub const K_SHM_VERSION: u16 = 11;

pub const K_UI_MAX_TRACKS: usize = 8;
pub const K_UI_MAX_CLIP_NOTES: usize = 4096;
pub const K_UI_MAX_CLIP_CHORDS: usize = 1024;
pub const K_UI_MAX_HARMONY_EVENTS: usize = 512;
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
    pub reserved_ui: u32,
    pub ui_clip_offset: u64,
    pub ui_clip_bytes: u64,
    pub ui_harmony_version: u32,
    pub reserved_ui2: u32,
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
}

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

impl UiPatcherPresetCommandPayload {
    /// Build a named command (load/save a project or patcher preset).
    ///
    /// The name is a fixed 28-byte field, not a pointer: a command ring entry is
    /// a fixed-size POD slot shared with another process, so a longer name is
    /// truncated here rather than silently reinterpreted there.
    pub fn named(command: UiCommandType, name: &str) -> Self {
        let mut bytes = [0u8; 28];
        let source = name.as_bytes();
        let len = source.len().min(bytes.len());
        bytes[..len].copy_from_slice(&source[..len]);
        Self { command_type: command as u16, flags: 0, track_id: 0, base_version: 0, name: bytes }
    }

    /// Reinterpret as the generic command payload for the ring.
    ///
    /// Sound only because both are 40-byte `#[repr(C)]` PODs — the engine
    /// dispatches on `entry.size == sizeof(UiPatcherPresetCommandPayload)`, so if
    /// the two ever diverge in size the engine stops recognising named commands
    /// and simply ignores them. `SIZES_MATCH` below turns that into a build
    /// error instead of a project that silently refuses to open.
    pub fn as_command(self) -> UiCommandPayload {
        const _: () = assert!(
            core::mem::size_of::<UiPatcherPresetCommandPayload>()
                == core::mem::size_of::<UiCommandPayload>(),
            "named commands ride in a UiCommandPayload slot; the sizes must match",
        );
        // SAFETY: same size, both #[repr(C)], both plain data with no padding
        // invariants or niches. The assert above is the guard.
        unsafe { core::mem::transmute(self) }
    }
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
}

pub const MIXER_FLAG_MUTE: u16 = 1 << 0;
pub const MIXER_FLAG_SOLO: u16 = 1 << 1;

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
        // Named commands are transmuted into a UiCommandPayload slot; see
        // UiPatcherPresetCommandPayload::as_command.
        const_assert_eq!(size_of::<UiCommandPayload>(), 40);
        const_assert_eq!(size_of::<UiPatcherPresetCommandPayload>(), 40);
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
        const_assert_eq!(size_of::<ShmHeader>(), 256);
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
        // v9 tail fields; header size stays 256 (they fit inside the align(64)
        // padding after the peak-rms array).
        assert_eq!(offset_of!(ShmHeader, ui_clip_all_offset), 216);
        assert_eq!(offset_of!(ShmHeader, ui_clip_all_bytes), 224);
        assert_eq!(offset_of!(ShmHeader, ring_ui_agent_offset), 232);
        assert_eq!(offset_of!(ShmHeader, ui_lines_per_beat), 240);
        assert_eq!(offset_of!(ShmHeader, ui_clip_extent_offset), 248);
    }
}
