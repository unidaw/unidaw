use std::sync::atomic::{AtomicU32, AtomicU64};

pub const K_UI_MAX_TRACKS: usize = 8;
pub const K_UI_MAX_CLIP_NOTES: usize = 4096;
pub const K_UI_MAX_CLIP_CHORDS: usize = 1024;
pub const K_UI_MAX_HARMONY_EVENTS: usize = 512;

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
    pub note_id: u32,
    pub pitch: u8,
    pub velocity: u8,
    pub column: u8,
    pub reserved: u8,
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
pub struct UiClipSnapshot {
    pub track_count: u32,
    pub note_count: u32,
    pub chord_count: u32,
    pub reserved: u32,
    pub tracks: [UiClipTrack; K_UI_MAX_TRACKS],
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

impl Default for UiClipSnapshot {
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
}

#[repr(u16)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum UiDiffType {
    None = 0,
    AddNote = 1,
    RemoveNote = 2,
    UpdateNote = 3,
    ResyncNeeded = 4,
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
    fn shm_header_layout_matches_cpp() {
        const_assert_eq!(size_of::<ShmHeader>(), 256);
        const_assert_eq!(align_of::<ShmHeader>(), 64);
        assert_eq!(offset_of!(ShmHeader, ring_std_offset), 56);
        assert_eq!(offset_of!(ShmHeader, ring_ctrl_offset), 64);
        assert_eq!(offset_of!(ShmHeader, ring_ui_offset), 72);
        assert_eq!(offset_of!(ShmHeader, ring_ui_out_offset), 80);
        assert_eq!(offset_of!(ShmHeader, mailbox_offset), 88);
        assert_eq!(offset_of!(ShmHeader, ui_version), 96);
        assert_eq!(offset_of!(ShmHeader, ui_visual_sample_count), 104);
        assert_eq!(offset_of!(ShmHeader, ui_global_nanotick_playhead), 112);
        assert_eq!(offset_of!(ShmHeader, ui_track_count), 120);
        assert_eq!(offset_of!(ShmHeader, ui_transport_state), 124);
        assert_eq!(offset_of!(ShmHeader, ui_clip_version), 128);
        assert_eq!(offset_of!(ShmHeader, ui_clip_offset), 136);
        assert_eq!(offset_of!(ShmHeader, ui_clip_bytes), 144);
        assert_eq!(offset_of!(ShmHeader, ui_harmony_version), 152);
        assert_eq!(offset_of!(ShmHeader, ui_harmony_offset), 160);
        assert_eq!(offset_of!(ShmHeader, ui_harmony_bytes), 168);
        assert_eq!(offset_of!(ShmHeader, ui_track_peak_rms), 176);
    }
}
