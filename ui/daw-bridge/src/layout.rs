use std::sync::atomic::{AtomicU32, AtomicU64};

/// Must match `kShmMagic` / `kShmVersion` in apps/shared_memory.h. Bump both
/// together whenever `ShmHeader`'s layout changes, so a stale binary on either
/// side of the mapping is rejected instead of silently misreading fields.
pub const K_SHM_MAGIC: u32 = 0x3041_5744;
pub const K_SHM_VERSION: u16 = 42;

/// The project-document schema the engine WRITES. Mirrors `kProjectSchemaVersion` in
/// `apps/project_file.cpp`; the pair is compared by `tools/version_parity_check.sh`.
///
/// It exists so a Rust test can assert "a save round-trips at the current schema" without typing
/// the number — an assertion spelled `Some(4)` failed the moment the schema advanced, and did it
/// with a message about the segmenter rather than about the schema.
pub const PROJECT_SCHEMA_VERSION: u32 = 6;

/// SetLaneQuantize carries swing through an unsigned field; this is the bias.
pub const LANE_QUANTIZE_SWING_BIAS: u32 = 500;
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
/// == K_UI_MAX_CLIP_EXTENTS, and now actually equal. It fell out of step when the extents
/// went 64 -> 256, which left a project with more than 64 audio placements publishing
/// complete rails with waveform data missing from the tail. Raised in v28.
pub const K_UI_MAX_AUDIO_CLIPS: usize = K_UI_MAX_CLIP_EXTENTS;
pub const K_UI_WAVEFORM_SLOTS: usize = 4;
pub const K_UI_WAVEFORM_MAX_PAIRS: usize = 24576;
pub const K_UI_EDIT_BATCH_MAX_OPS: usize = 32;
pub const K_UI_EDIT_BATCH_CAPACITY: usize = 64;
pub const K_UI_COMMAND_OUTCOME_CAPACITY: usize = 256;
pub const K_CHAIN_DEVICE_ID_AUTO: u32 = 0xFFFF_FFFF;
/// `trackId` on a chain command meaning EVERY track. The same bit pattern as
/// `K_CHAIN_DEVICE_ID_AUTO` and deliberately not the same constant: one names a
/// device the engine should number itself, the other names a whole-project
/// request, and folding them together would make a rename of either one silently
/// change the other.
pub const K_CHAIN_TRACK_ALL: u32 = 0xFFFF_FFFF;
/// `hostSlotIndex` meaning "this device is not hosted out of process at all"
/// (`kHostSlotIndexDirect`). Distinct from `K_CHAIN_DEVICE_ID_AUTO`, which in the
/// same field would mean an unassigned slot — a patcher device HAS no slot, and
/// the two must not read the same on a card.
pub const K_HOST_SLOT_DIRECT: u32 = 0xFFFF_FFFE;
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
    /// v26 (M1.13): per-lane non-destructive quantize. Draw each note at its authored
    /// t_on (unchanged, and what is saved) plus a deviation bar to where it sounds,
    /// which is `quantize_tick(t_on, grid, strength, swing)`. grid 0 = lane not
    /// quantized. Swing is signed thousandths of a grid step, applied to ODD slots.
    /// `ui_quantize_version` moves when a lane's quantize changes and is NOT the clip
    /// version — quantize moves no authored note, so it must not invalidate an edit.
    pub ui_track_quantize_grid: [u64; K_UI_MAX_TRACKS],
    pub ui_track_quantize_strength: [u32; K_UI_MAX_TRACKS],
    pub ui_track_quantize_swing: [i32; K_UI_MAX_TRACKS],
    pub ui_quantize_version: u32,
    /// v27 (M3.25): where to find the UiArrangeSummaryRegion. 0 = absent. Its own
    /// version lives INSIDE the region, so a reader takes the spine and the meter under
    /// one read and cannot see a mismatched pair.
    pub ui_arrange_offset: u64,
    pub ui_arrange_bytes: u64,
    /// v28: automation read-back. The lane LIST is standing and version-gated; the SLOTS answer
    /// per-request point queries.
    pub ui_automation_offset: u64,
    pub ui_automation_bytes: u64,
    pub ui_automation_slot_offset: u64,
    pub ui_automation_slot_bytes: u64,
    /// v29: the song's end in ticks, mirrored from the arrange region. NOT a second source of
    /// truth — the same number written from the same atomic in the same pass, put where a reader
    /// that needs it every frame can get it without a second region read.
    ///
    /// This was missing from this mirror until v32. Harmless in practice (regions are addressed
    /// by the offsets the header itself carries, so a SHORT mirror still reads correctly), but a
    /// mirror that silently lags the contract is how the next field lands in the wrong place.
    pub ui_song_end_tick: u64,
    /// v32: one sampler device's kit, on request.
    pub ui_sampler_kit_offset: u64,
    pub ui_sampler_kit_bytes: u64,
    /// v34: the widest op run on any note in the track — how many glyphs the collapsed ops cell
    /// must be able to draw. 0 = no note in the track carries an op, which is R5's "do not draw
    /// the column at all".
    ///
    /// It exists because a client sees only a WINDOW: anything computed from the rows on screen
    /// changes as you scroll, and a column that reflows under the cursor while you type into it
    /// is worse than one that clips. Recomputed where the flat clip is re-derived, so it moves
    /// with clip_version.
    pub ui_track_ops_width: [u8; K_UI_MAX_TRACKS],
    /// v37: one modulator's envelope shape, on request. Appended at the END so every existing
    /// field keeps its offset; only sizeof(ShmHeader) grew, which is what bumps K_SHM_VERSION.
    pub ui_sampler_envelope_offset: u64,
    pub ui_sampler_envelope_bytes: u64,
    /// v41: broadcast exact terminal outcomes for guarded note/chord/harmony commands.
    pub ui_command_outcome_offset: u64,
    pub ui_command_outcome_bytes: u64,
}

pub const UI_COMMAND_OUTCOME_NONE: u8 = 0;
pub const UI_COMMAND_OUTCOME_COMPLETED: u8 = 1;
pub const UI_COMMAND_OUTCOME_REFUSED: u8 = 2;
pub const UI_COMMAND_OUTCOME_REASON_NONE: u8 = 0;
pub const UI_COMMAND_OUTCOME_REASON_STALE_BASE: u8 = 1;
pub const UI_COMMAND_OUTCOME_REASON_UNKNOWN_TRACK: u8 = 2;
pub const UI_COMMAND_OUTCOME_STATUS_NORMAL: u64 = 0;
pub const UI_COMMAND_OUTCOME_STATUS_SEQUENCE_EXHAUSTED: u64 = 1;

#[repr(C, align(64))]
pub struct UiCommandOutcomeEntry {
    pub sequence: AtomicU64,
    pub command_id: AtomicU64,
    pub metadata0: AtomicU64,
    pub metadata1: AtomicU64,
    pub metadata2: AtomicU64,
    pub reserved: [AtomicU64; 3],
}

#[repr(C, align(64))]
pub struct UiCommandOutcomeRegion {
    pub published_sequence: AtomicU64,
    pub next_command_id: AtomicU64,
    pub status: AtomicU64,
    pub capacity: u32,
    pub reserved0: u32,
    pub reserved: [u64; 4],
    pub entries: [UiCommandOutcomeEntry; K_UI_COMMAND_OUTCOME_CAPACITY],
}

/// uiTrackFlags bits (Movement 4).
/// v32: THE SAMPLER KIT READ-BACK. Request/answer with a CLIENT-OWNED request_seq: it names the
/// slot the answer lands in (`request_seq % UI_SAMPLER_KIT_SLOTS`), so a caller knows where to
/// look BEFORE it asks rather than scanning for a reply that resembles its question.
///
/// Published from the SNAPSHOT the producer reads, not from the document — a read-back built from
/// the model answers "what was configured" while the audio thread plays something else, which is
/// precisely the divergence a read-back exists to catch.
pub const UI_MAX_SAMPLER_SLOTS: usize = 64;
/// Bytes in a published slot name INCLUDING the terminator, so 39 usable. Must match
/// `kUiSamplerSlotNameBytes`. The engine REFUSES a longer name rather than truncating it.
pub const UI_SAMPLER_SLOT_NAME_BYTES: usize = 40;
pub const UI_SAMPLER_KIT_SLOTS: usize = 2;

/// The slot's source did not resolve, so it will be SILENT. A flag rather than something to infer
/// from `length_frames == 0`, because "silent because the file is missing" and "silent because the
/// sample is empty" are different problems and a UI should be able to say which.
pub const UI_SAMPLER_SLOT_SOURCE_MISSING: u8 = 1 << 2;
/// The slot names a SLICE that no longer exists — what `sampler-slice --mode clear` leaves
/// behind. Such a slot plays the whole source, which is indistinguishable from a one-slice chop
/// by extent alone, so it has to be published rather than inferred.
pub const UI_SAMPLER_SLOT_SLICE_MISSING: u8 = 1 << 3;

/// SamplerSlice (76) modes. NAMED rather than numbered by position, so adding a mode never
/// changes what an existing saved macro or agent script means.
pub const SAMPLER_SLICE_TRANSIENT: u16 = 0;
pub const SAMPLER_SLICE_EQUAL: u16 = 1;
pub const SAMPLER_SLICE_CLEAR: u16 = 2;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct UiSamplerSlicePayload {
    pub command_type: u16,
    pub mode: u16,
    pub track_id: u32,
    pub device_id: u32,
    pub source_local_id: u32,
    pub sensitivity: u32,
    pub count: u32,
    pub max_slices: u32,
    /// 0 = no snap; else the row grid in nanoticks, which makes the chop tempo-adaptive from the
    /// moment it is made rather than tied to the rate the file was recorded at.
    pub snap_nanoticks: u32,
    /// Non-zero makes a SLOT PER SLICE from `slot_base_key` upward — the gesture that turns a
    /// chop into something playable in one command rather than N.
    pub make_slots: u8,
    pub slot_base_key: u8,
    pub reserved: [u8; 6],
}

pub const SAMPLER_MARKER_ADD: u16 = 0;
pub const SAMPLER_MARKER_MOVE: u16 = 1;
pub const SAMPLER_MARKER_REMOVE: u16 = 2;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
/// SamplerSetFilter (86). The mod set's filter: type, base cutoff, base resonance.
///
/// Nothing in the engine wrote `filterType` before this — the only reference to it anywhere was
/// the read at the kit publish site, so the filter could be turned on by hand-editing project
/// JSON and by nothing else. Every cutoff and resonance modulator reachable from a UI was
/// therefore inert by construction: created, saved, reloaded, publishing its bit, moving nothing.
///
/// `cutoff_milli` is 0..1000 across the audible range logarithmically; `resonance_milli` is
/// 0..1000 onto Q 0.7..10. The two flags exist because zero is a legal cutoff, so "leave it
/// alone" cannot be encoded as a zero value.
pub struct UiSamplerFilterPayload {
    pub command_type: u16,
    pub flags: u16,
    pub track_id: u32,
    pub device_id: u32,
    pub mod_set_id: u32,
    pub filter_type: u8,
    pub reserved0: u8,
    pub cutoff_milli: u16,
    pub resonance_milli: u16,
    pub reserved1: u16,
    pub reserved2: [u32; 4],
}

/// Which fields SamplerSetFilter is actually setting.
pub const SAMPLER_FILTER_SET_CUTOFF: u16 = 1 << 0;
pub const SAMPLER_FILTER_SET_RESONANCE: u16 = 1 << 1;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
/// SamplerSetLfo (85). ModKind::Lfo has been in the saved project since the sampler shipped and
/// nothing rendered it — a modulator kind that round-tripped perfectly and made no sound.
///
/// Two depths, meaning different things: `depth` is the LFO's OWN amplitude and `depth_milli` is
/// how much of it reaches the target. The LFO is note-retriggered, so its phase is a pure
/// function of the voice's age and a render does not depend on when playback started.
pub struct UiSamplerLfoPayload {
    pub command_type: u16,
    pub flags: u16,
    pub track_id: u32,
    pub device_id: u32,
    pub mod_set_id: u32,
    pub modulator_id: u16,
    pub target: u8,
    pub reserved: u8,
    pub frequency_hz: f32,
    pub depth: f32,
    pub bias: f32,
    pub phase_offset: f32,
    pub depth_milli: i16,
    pub reserved2: u16,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
/// BulkChunk (83) — one chunk of a longer command. The inward bulk carrier.
///
/// Outbound has SHM regions; inbound had only the ring's 40-byte payload, so any variable-length
/// UI->engine command had no way across. A long message is chunked across ordinary ring entries
/// and the reassembled buffer IS a payload, carrying the real commandType as its first u16 — so
/// once assembled a bulk command looks exactly like a small one.
///
/// `seq`/`total` make a LOST chunk detectable. Concatenating whatever arrived would deliver a
/// truncated message that still parses, and a wrong sound is worse than an error.
pub struct UiBulkChunkPayload {
    pub command_type: u16,
    pub stream_id: u16,
    pub seq: u16,
    pub total: u16,
    pub bytes: [u8; 32],
}

pub const BULK_CHUNK_BYTES: usize = 32;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
/// Header of an assembled SamplerSetEnvelopePoints (84) payload; `point_count` points follow.
/// 255 in a loop index means NO LOOP.
pub struct UiSamplerEnvPointsHeader {
    pub command_type: u16,
    pub flags: u16,
    pub track_id: u32,
    pub device_id: u32,
    pub mod_set_id: u32,
    pub modulator_id: u16,
    pub time_base: u8,
    pub target: u8,
    pub rate_milli: u16,
    pub point_count: u16,
    pub sustain_loop_start: u8,
    pub sustain_loop_end: u8,
    pub release_loop_start: u8,
    pub release_loop_end: u8,
    pub release_fade: u32,
}

/// SetClipGrid (94). Flags per field, because 0 is not spare — it is the packer's "no grid on
/// this extent" sentinel, so there is no value that can mean "leave this one alone".
pub const CLIP_GRID_SET_LINES: u16 = 1 << 0;
pub const CLIP_GRID_SET_NUMERATOR: u16 = 1 << 1;
pub const CLIP_GRID_SET_DENOMINATOR: u16 = 1 << 2;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
/// SetClipGrid (94). Ranges are REFUSED, not clamped: lines and numerator get five bits each
/// (1..=31) and the denominator is stored as a 3-bit exponent, so it must be a power of two.
pub struct UiSetClipGridPayload {
    pub command_type: u16,
    pub flags: u16,
    pub track_id: u32,
    pub clip_id: u32,
    pub lines_per_beat: u32,
    pub time_sig_numerator: u32,
    pub time_sig_denominator: u32,
    pub reserved: [u32; 4],
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
/// RequestSamplerEnvelope (97). `request_seq` is CLIENT-OWNED and picks the answer slot
/// (request_seq % K_UI_SAMPLER_ENVELOPE_SLOTS), so a caller knows where its answer will land
/// before it asks. Addressed by modulator id, or by target with SAMPLER_ENV_BY_TARGET — the same
/// bit the WRITE uses, because asking a different way than writing is how a read-back ends up
/// answering about a different object.
pub struct UiSamplerEnvelopeRequestPayload {
    pub command_type: u16,
    pub flags: u16,
    pub track_id: u32,
    pub device_id: u32,
    pub mod_set_id: u32,
    pub request_seq: u32,
    pub modulator_id: u16,
    pub target: u8,
    pub reserved0: u8,
    pub reserved1: [u32; 4],
}

/// SetAudioClipField (95). Field-ADDRESSED rather than flagged, the shape SamplerSetSlot uses:
/// one property per call, so a fifth audio-clip property later is an enum entry rather than a
/// fifth opcode and nothing is ever silently reset.
pub const AUDIO_CLIP_FIELD_SOURCE_START_FRAME: u16 = 0;
pub const AUDIO_CLIP_FIELD_GAIN_MILLIBELS: u16 = 1;
pub const AUDIO_CLIP_FIELD_FADE_IN_NANOTICKS: u16 = 2;
pub const AUDIO_CLIP_FIELD_FADE_OUT_NANOTICKS: u16 = 3;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
/// SetAudioClipField (95). `value` is SIGNED so the gain can be negative — which is the normal
/// case, a clip being turned down. The engine CLAMPS the gain to -9600..=2400 millibels (matching
/// the sampler slot, the same quantity over the same range) and REFUSES a negative frame or tick
/// count, which is a caller with the wrong idea of the unit rather than a value to round.
pub struct UiAudioClipFieldPayload {
    pub command_type: u16,
    pub field: u16,
    pub track_id: u32,
    pub clip_id: u32,
    pub reserved0: u32,
    pub value: i64,
    pub reserved1: [u32; 4],
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
/// SamplerSetVintage (91). The flags say WHICH of the two this call is about, so setting the bit
/// depth does not silently reset the rate — zero is a legal value for both (it means off), so
/// absence cannot be encoded as a zero.
pub struct UiSamplerVintagePayload {
    pub command_type: u16,
    pub flags: u16,
    pub track_id: u32,
    pub device_id: u32,
    pub mod_set_id: u32,
    pub bit_depth: u8,
    pub reserved0: u8,
    pub rate_hz: u16,
    pub reserved1: [u32; 5],
}

pub const SAMPLER_VINTAGE_SET_BITS: u16 = 1 << 0;
pub const SAMPLER_VINTAGE_SET_RATE: u16 = 1 << 1;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
/// Header of an assembled SamplerSetSlotName (90) payload; `name_bytes` raw bytes follow, NOT
/// nul-terminated — the length is explicit. The engine REFUSES a name that does not fit
/// `UiSamplerSlotEntry::name` rather than shortening it, so a successful send reads back byte for
/// byte and a rejected one leaves the slot alone.
pub struct UiSamplerSlotNameHeader {
    pub command_type: u16,
    pub device_id: u16,
    pub track_id: u32,
    pub slot_id: u16,
    pub name_bytes: u16,
}

/// Which string on the clip. One opcode rather than two, because the carrier, the addressing,
/// the version gate and every rejection are identical — only the destination field differs.
pub const CLIP_TEXT_FIELD_NAME: u16 = 0;
pub const CLIP_TEXT_FIELD_SOURCE_PATH: u16 = 1;

/// How many bytes `UiClipExtent::name` holds. The engine REFUSES a longer name rather than
/// truncating it, so this is a real limit a caller must respect and not a display hint.
pub const UI_CLIP_EXTENT_NAME_BYTES: usize = 32;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
/// Header of an assembled SetClipText (98) payload; `text_bytes` raw UTF-8 bytes follow, NOT
/// nul-terminated.
///
/// Clip `name` and audio `source_path` were the last two GAPs in persisted_field_reach —
/// persisted, published and rendered, with no command able to write either. Both were GAPs for
/// the same reason: a string does not fit the 40-byte ring payload, so both ride the BulkChunk
/// carrier (83) exactly as SamplerSetSlotName (90) does.
pub struct UiClipTextHeader {
    pub command_type: u16,
    /// `CLIP_TEXT_FIELD_*`
    pub field: u16,
    pub track_id: u32,
    pub clip_id: u32,
    pub text_bytes: u32,
    pub base_version: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
/// One drawn point. `tension` is toward the NEXT point: 0 linear, positive ease-in, negative
/// ease-out. `flags` bit 0 = STEP — hold until the next point's time, then jump.
pub struct UiEnvPointWire {
    pub time: u32,
    pub value_milli: i16,
    pub tension: i8,
    pub flags: u8,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
/// SamplerSetEnvelope (82). The ADSR — until this existed the only way to reach a sampler
/// envelope was to hand-edit the project JSON.
///
/// Times are in the modulator's OWN unit, named by `time_base` in the same payload: 0 =
/// microseconds, 1 = nanoticks. Carrying the unit with the numbers is what makes the command
/// complete — bare durations would mean different things depending on state the sender never saw.
pub struct UiSamplerEnvelopePayload {
    pub command_type: u16,
    pub flags: u16,
    pub track_id: u32,
    pub device_id: u32,
    pub mod_set_id: u32,
    pub modulator_id: u16,
    pub time_base: u8,
    pub reserved1: u8,
    pub attack: u32,
    pub decay: u32,
    pub release: u32,
    pub sustain_milli: i16,
    pub rate_milli: u16,
    /// Which modulation domain: 0 Volume, 1 Panning, 2 Pitch, 3 Cutoff, 4 Resonance. The engine
    /// renders envelopes on all of them; for a while nothing could create any but Volume.
    pub target: u8,
    pub reserved2: u8,
    /// Signed. What FULL envelope travel is worth on the target — on Cutoff, 1000 is +-6 octaves.
    pub depth_milli: i16,
}

/// Target the AMP envelope whatever its id, minting one if the mod set has none.
pub const SAMPLER_ENV_BY_TARGET: u16 = 1 << 0;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
/// SetRowOps (81). The write half of the per-note ops the engine has published since v23/v32.
///
/// `mask` says which fields this payload is speaking about: a bit CLEAR leaves that op alone, a
/// bit SET with a zero value CLEARS it. Without the distinction there is no way to remove one op
/// from a note without resending the other four.
///
/// `delay_nanoticks` is ABSOLUTE ticks, not the num/den fraction the notation uses — RowOps
/// resolves the fraction against a beat length at parse time, so the wire carries what the store
/// holds. There is deliberately no pan field: pan is not on the engine's NotePayload, which is
/// pinned at 32 bytes.
pub struct UiSetRowOpsPayload {
    pub command_type: u16,
    pub mask: u16,
    pub track_id: u32,
    pub clip_id: u32,
    /// LOW half of the note's EventId. It is 64 bits because EventId packs the AUTHOR into bits
    /// 48+ and each author has its own independent counter — a 32-bit id drops the author, and
    /// agent note (1, 5) then collides with human note (0, 5) and edits the wrong one. Split
    /// lo/hi rather than moved so every other field keeps the offset it already had.
    pub note_id_lo: u32,
    pub delay_nanoticks: u32,
    pub sound: u16,
    pub sound_offset: u16,
    pub retrigger: u8,
    pub probability: u8,
    /// v33. Took pad0, so the payload is the same 40 bytes and no field moved — the mask says
    /// whether they are being spoken about, exactly as for the ops above.
    pub retrig_ramp: i8,
    pub trig_condition: u8,
    pub note_id_hi: u32,
    pub reserved: [u8; 8],
}

/// SetRowOps mask bits — which ops the payload means.
pub const ROW_OP_MASK_RETRIGGER: u16 = 1 << 0;
pub const ROW_OP_MASK_PROBABILITY: u16 = 1 << 1;
pub const ROW_OP_MASK_SOUND: u16 = 1 << 2;
pub const ROW_OP_MASK_SOUND_OFFSET: u16 = 1 << 3;
pub const ROW_OP_MASK_DELAY: u16 = 1 << 4;
pub const ROW_OP_MASK_RETRIG_RAMP: u16 = 1 << 5;
pub const ROW_OP_MASK_TRIG_CONDITION: u16 = 1 << 6;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct UiSamplerMarkerPayload {
    pub command_type: u16,
    pub op: u16,
    pub track_id: u32,
    pub device_id: u32,
    pub source_local_id: u32,
    pub marker_id: u32,
    pub frame: u64,
    pub reserved: [u8; 8],
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct UiSamplerEmitRowsPayload {
    pub command_type: u16,
    pub flags: u16,
    pub track_id: u32,
    pub device_id: u32,
    pub source_local_id: u32,
    pub at_nanotick: u64,
    /// One row per slice, this far apart. 0 = DERIVE from each slice's own length at the current
    /// tempo, which reproduces the break as recorded; an explicit step re-fits it to a grid.
    pub step_nanoticks: u64,
    pub column: u8,
    pub velocity: u8,
    pub reserved: [u8; 6],
}

/// RequestSamplerKit (75).
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct UiSamplerKitRequestPayload {
    pub command_type: u16,
    pub flags: u16,
    pub track_id: u32,
    pub device_id: u32,
    pub request_seq: u32,
    pub reserved: [u8; 24],
}

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
    /// Which device this node belongs to, or 0 for a pool node with no owning device.
    ///
    /// Was `reserved`, at the same offset and width. The region publishes the ASSEMBLED pool —
    /// a union of every device's graph with re-id'd nodes — so `UiPatcherRegion::deviceId` has
    /// no answer to give, while "which device is this NODE" always does. This is the fact a UI
    /// needs before it can set `UI_PATCHER_FLAG_HAS_DEVICE_ID` on an edit; without it every
    /// patcher command is pool-scoped, and the pool is not what a project renders.
    ///
    /// Carried losslessly BY CONSTRUCTION: a device id is at most `STABLE_DEVICE_ID_MAX`
    /// (0x7FFF), so it always fits. This used to say "capped at 65535 by the half-word", which
    /// was an argument from roominess and stopped being the rule when ids went project-global.
    /// The engine converts through a checked narrowing and reports a value that does not fit
    /// rather than truncating; 0 still means "no owner".
    pub owner_device_id: u16,
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

/// v30: `UiDeviceParam.flags`. A DISCRETE parameter is a switch with `step_count` positions —
/// writing 0.37 to a 5-way selector lands in whichever position that happens to be, which is why
/// knowing it is not optional. NOT-automatable means the plugin will ignore an automation lane
/// pointed at it, so drawing one is a lie.
pub const UI_PARAM_DISCRETE: u32 = 1 << 0;
pub const UI_PARAM_AUTOMATABLE: u32 = 1 << 1;

// v18 waveform regions, generated from shared_memory.h (bindgen's own layout_tests pin
// their sizes/offsets against the C++ structs).
pub use crate::sys::{
    daw_UiAudioClip as UiAudioClip, daw_UiAudioSource as UiAudioSource,
    daw_UiAudioSourceRegion as UiAudioSourceRegion, daw_UiWaveformRegion as UiWaveformRegion,
    daw_UiWaveformSlot as UiWaveformSlot,
};

// v32 sampler kit read-back. ALIASES of the generated structs rather than a hand-written mirror:
// these live in shared_memory.h, bindgen owns them, and a second hand-maintained copy is exactly
// the "two facts about one thing" shape — with the added charm that the two would be checked
// against each other by nobody. (Command payloads, by contrast, come from event_payloads.h which
// is NOT bindgen'd, so those stay hand-written and are pinned by the wire_layout test instead.)
pub use crate::sys::{
    daw_UiSamplerKitRegion as UiSamplerKitRegion, daw_UiSamplerKitSlot as UiSamplerKitSlot,
    daw_UiSamplerSlotEntry as UiSamplerSlotEntry,
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
    /// M2.18 publication flag for the multi-producer ring. A producer CAS-reserves a
    /// slot on `write_index`, fills the entry, then stores 1 here; the engine will not
    /// read a slot until it is set. It occupies what was already tail padding, so the
    /// struct is the same 64 bytes and nothing else moved — but a producer that leaves
    /// it 0 has its command ignored, so every writer must go through `write_entry`.
    pub ready: u32,
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
    /// v26 (M1.13): how far this note moves when it SOUNDS, in nanoticks, SIGNED — a
    /// note pulled earlier reads negative. `t_on` stays the AUTHORED value, so draw the
    /// note where it was played and a mark to where it is heard. 0 means the lane is not
    /// quantized. The SOUNDING tick is `t_on + dev_nanoticks + delay_nanoticks`: the
    /// scheduler quantizes the tick and then adds the row-op delay.
    ///
    /// Published rather than derived client-side on purpose: deriving it means a second
    /// implementation of quantize_tick, whose correctness turns on C++ integer division
    /// truncating TOWARD ZERO on a negative delta — which `Math.floor` and Rust's `/`
    /// on negatives do not both agree with. It took the reserved word, so the struct is
    /// the same 40 bytes and nothing moved.
    pub dev_nanoticks: i32,
    /// v32: THE SOUND ADDRESS. Which slot of the track's sampler this note plays. 0 = the keymap
    /// picks it from pitch, which on an ordinary kit track is EVERY ROW — so draw 0 as empty
    /// rather than as a literal zero. That sparseness is exactly why there is no permanent ops
    /// column (SAMPLER_DESIGN R5).
    pub sound: u16,
    /// v32: the 9xx seek, as a FRACTION of the slot's extent (0..65535) rather than absolute
    /// frames. Absolute breaks when the slot's sample is swapped or its slice re-cut.
    pub sound_offset: u16,
    /// v33: THE RETRIGGER VOLUME RAMP, a SIGNED TOTAL percent change across the burst's strikes.
    /// -60 lands the last strike at 40% of the first; the first is always at the authored
    /// velocity. 0 is flat, which is what every note had before, so an older project is unchanged.
    pub retrig_ramp: i8,
    /// v33: THE CONDITIONAL TRIG. 0 = no condition, always sounds. 1..64 packs an A:B pair three
    /// bits each — 1:2 fires on the first pass of every two. Codes >= 128 are reserved for FILL
    /// and PRE, which need state a per-note code cannot carry.
    ///
    /// NOT probability: `p` is a per-pass roll, this is deterministic in WHICH PASS the transport
    /// is on, which is what lets a phrase resolve every four bars rather than merely thin out.
    pub trig_condition: u8,
    pub reserved32: [u8; 2],
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

/// `UiClipExtent.flags` bits 14-21: how many overrides this appearance carries (adds + mutes),
/// SATURATING at 255. Bit 22 is set whenever the count is non-zero. This is THE BADGE a UI draws
/// to say "this appearance is customised", and until now it was published and unreadable from
/// anywhere outside the engine — so a stale one (a mute whose base note a later clip edit
/// removed) lit the badge over nothing with no way to observe it.
pub const UI_CLIP_OVERRIDE_COUNT_SHIFT: u32 = 14;
pub const UI_CLIP_OVERRIDE_COUNT_MASK: u32 = 0xff;

/// Overrides on this appearance: (count, has_overrides). The count saturates at 255, so
/// `has_overrides` with a count of 255 means "at least 255".
pub fn unpack_clip_overrides(flags: u32) -> (u32, bool) {
    (
        (flags >> UI_CLIP_OVERRIDE_COUNT_SHIFT) & UI_CLIP_OVERRIDE_COUNT_MASK,
        flags & UI_CLIP_EXTENT_HAS_OVERRIDES != 0,
    )
}

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

/// Raised 64 -> 256 in v27. NOTHING asserted this against the C++ side, so when the C++
/// constant moved first the two disagreed and every test still passed — the exact silent
/// divergence the const_asserts exist to prevent. There is one on the region size now.
pub const K_UI_MAX_CLIP_EXTENTS: usize = 256;

/// v28: AUTOMATION READ-BACK. Automation could be written and never read — nothing in the header
/// mentioned it — so the only lane a UI could offer was one you draw into and never see.
///
/// Two shapes for two questions. The LANE LIST is standing and version-gated, so lanes are
/// discoverable without asking. The POINTS are answered per request into a seqlock slot, because a
/// song can hold far more automation than a fixed region could carry and a UI only draws the lanes
/// that are open.
///
/// NOT published: the resolved value at the playhead. Interpolation belongs to whoever draws — it
/// is a picture, not a scheduling decision — and a published resolved value would be a second
/// implementation that can disagree with what plays.
pub const K_UI_MAX_AUTOMATION_LANES: usize = 64;
pub const K_UI_MAX_AUTOMATION_POINTS: usize = 512;
pub const K_UI_AUTOMATION_SLOTS: usize = 4;
/// v37: one modulator's envelope shape, on request. Four slots, chosen the same way.
pub const K_UI_SAMPLER_ENVELOPE_SLOTS: usize = 4;
pub const K_UI_MAX_ENVELOPE_POINTS: usize = 64;
pub const UI_AUTOMATION_FLAG_DISCRETE: u32 = 1 << 0;

// v28 automation read-back regions, generated from shared_memory.h. Hand-mirrored structs are
// how the extents capacity diverged unnoticed; bindgen's layout_tests + the C++ static_asserts
// pin these, and the const_asserts below tie each hand constant to a generated region size, so a
// wrong count fails to COMPILE rather than publishing a short list at runtime.
pub use crate::sys::{
    daw_UiAutomationLane as UiAutomationLane,
    daw_UiAutomationLaneRegion as UiAutomationLaneRegion,
    daw_UiAutomationPointEntry as UiAutomationPointEntry,
    daw_UiAutomationSlot as UiAutomationSlot,
    daw_UiAutomationSlotRegion as UiAutomationSlotRegion,
    daw_UiSamplerEnvelopeRegion as UiSamplerEnvelopeRegion,
};

#[repr(C)]
pub struct UiClipExtentRegion {
    pub count: u32,
    /// How many extents did NOT fit. Non-zero means the rails are incomplete — the cap went
    /// 64 -> 256 and the overflow stayed a bare `break`, so the truncation was silent at both
    /// sizes. Took the reserved word; the region is the same size.
    pub truncated: u32,
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
    /// The version these events ARE — written after them, by the thread that fills the region.
    /// Mirrors `UiHarmonySnapshot::version` in apps/shared_memory.h. Gate a cache on THIS, never
    /// on the header's `ui_harmony_version`, which the command thread moves before the consumer
    /// republishes. 0 from an engine that predates the field, which reads as "always stale".
    pub version: u32,
    pub reserved: [u32; 2],
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
    // Command identity, offset 32 in every refusal payload (P2-CMD-00). Two u32 and not
    // a u64: EventEntry::payload sits at offset 20, so a u64 member would raise alignof
    // to 8 and make the C++ cast sites undefined behaviour.
    pub correlation_lo: u32,
    pub correlation_hi: u32,
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
    /// Rename a track (trackId + name in UiPatcherPresetCommandPayload).
    SetTrackName = 36,
    /// Ask the engine to re-emit a track's device chain on `ringUiOut`.
    ///
    /// The chain is published as diffs, not as a region under the seqlock, so a
    /// consumer that attaches after the last edit has no way to learn the current
    /// chain by reading — it can only wait for the next one, which may never
    /// come. `track_id` = `K_CHAIN_TRACK_ALL` asks for every track, which is what
    /// a freshly connected UI wants.
    RequestChainSnapshot = 37,
    // 38-39 remain reserved for the rest of this family (routing, mod).
    /// Ask the engine to query a device's host for its parameters and publish
    /// them into UiDeviceParamsRegion. trackId in track_id, deviceId in value0.
    RequestDeviceParams = 40,
    /// Set the project tempo. value0 = milli-BPM. flags: 0 = insert-or-replace a point
    /// at note_nanotick_lo/hi; 1 = flatten the map to this single tempo.
    SetTempo = 41,
    /// Shut the engine down cleanly. Sent when the last UI has been gone long
    /// enough that it is not coming back. 42 rather than 41 — see the same note
    /// in apps/event_payloads.h; the two were allocated 41 on separate branches.
    Quit = 42,
    /// Set one plugin parameter from the rack (UiSetParamPayload). 43 because 42
    /// is Quit above — see the note in apps/event_payloads.h. Next free is 44.
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
    /*
     * PLACEMENT OPS (48-51). Where a clip sits on the timeline, rather than what
     * is inside it — the arrangement, as opposed to the notes.
     *
     * All four reuse UiCommandPayload and key on `value0` = the placement's
     * STABLE id. That id used to be the list index, which is why a drag could
     * not be keyed on it: any concurrent edit — the agent, another pane, an
     * undo — renumbered the thing under the mouse mid-gesture. It is a real
     * monotonic id on the placement now, so it survives edits and the undo
     * store-swap. Same lesson as ui_track_id, learned the same way.
     *
     * Overlaps CLAMP rather than refuse. That is right for a mouse and wrong for
     * an agent, so the caller is expected to compare what it asked for against
     * what comes back published and report the difference. See placement_move in
     * the sidecar.
     */
    /// Move a placement in time, and optionally to another track. trackId =
    /// source track, value0 = placement id, nanotick = the new `at`, note_pitch
    /// = the destination track id or PLACEMENT_SAME_TRACK to stay put.
    MovePlacement = 48,
    /// Delete a placement. trackId, value0 = placement id.
    RemovePlacement = 49,
    /// Retime a placement: nanotick = new start, duration = new length, either
    /// or both PLACEMENT_UNCHANGED. Both in ONE op deliberately — a left-edge
    /// trim is a start and a length together, and sending Move then Resize makes
    /// the clip visibly jump through an intermediate position.
    ResizePlacement = 50,
    /// Place a clip on a track. trackId, value0 = clip id, nanotick = at,
    /// duration = length.
    AddPlacement = 51,
    /// PANIC: all sound off — CC120 (all-sound-off) AND CC123 (all-notes-off) on every MIDI
    /// channel to every hosted plugin, plus all pending/active note state dropped. CC120 is
    /// the one that matters: 123 releases notes and lets them ring out, which is not a panic.
    Panic = 52,
    /// M1.13 lane quantize: track_id, note_nanotick = grid in nanoticks (0 = off),
    /// value0 = strength in thousandths, note_pitch = swing in thousandths BIASED by
    /// +500 (so 500 = straight). Changes what SOUNDS; never touches a stored note.
    SetLaneQuantize = 53,
    // 54-58 RETIRED with the Section spine (v29), and deliberately NOT reused: a client still
    // sending 57 for "set this section's length" would get something else entirely, and a command
    // that quietly does a different thing is the failure mode this contract exists to prevent.
    RevertPlacementOverrides = 59,
    WriteAutomationPoint = 60,
    /// Set (or clear) ONE placement's edit scope. trackId, value0 = placementId,
    /// flags bit0 = on. Not version-gated: it changes no note, so it cannot invalidate
    /// anyone's in-flight edit.
    SetPlacementEditScope = 61,
    /// v28: ask for ONE automation lane's points (UiAutomationLaneRequestPayload); answered
    /// into a UiAutomationSlot seqlock slot the CALLER picked. The lane LIST is standing in
    /// UiAutomationLaneRegion and needs no request.
    RequestAutomationLane = 62,
    /// v28: change an existing mod link's depth/bias/enabled IN PLACE, by linkId
    /// (UiModLinkCommandPayload; device/kind fields ignored). Remove+add was the only way, and it
    /// changed the id, dropped the uid16 and was not atomic — so a depth SLIDER was impossible.
    /// AddModLink still refuses an existing id rather than replacing.
    SetModLinkDepth = 63,
    /// v29 ARRANGEMENT ops, replacing the retired section family. Marker ops are TOTAL — they
    /// name a position, move no material, and cannot be refused for anything but a bad id.
    AddMarker = 64,
    RemoveMarker = 65,
    RenameMarker = 66,
    MoveMarker = 67,
    /// Recolour an existing marker. Reuses UiMarkerCommandPayload — it has carried `color_rgb`
    /// since v29 — so this is one more opcode in the marker family, not a wire change. A colour
    /// was write-once until this existed: AddMarker set it and nothing could change it.
    SetMarkerColor = 99,
    /// Insert or replace a meter point (flags bit0 = flatten to this one signature). THIS is
    /// where mid-song meter is authored; a Section's meter was reachable from no command at all.
    SetTimeSignature = 68,
    /// The ripple as its own command: insert or remove arrangement time at a tick, carrying every
    /// placement, tempo point, harmony event, automation point, meter point and marker at or
    /// after it in ONE refusable, undoable transaction.
    InsertRemoveTime = 69,
    /// M2.57 SCRATCH CLIPS — a write target for an agent, instead of your document.
    /// `ForkPlacementClip` copies what a placement plays, points the placement at the copy, and
    /// keeps the original as the ALTERNATE. `SwapPlacementClip` exchanges the two — that IS the
    /// A/B, and what plays is always `clip_id`, so there is no auditioning mode to fall out of
    /// step with what you hear. `ClearPlacementAlternate` drops the other once you have decided.
    ForkPlacementClip = 70,
    SwapPlacementClip = 71,
    ClearPlacementAlternate = 72,

    /// SAMPLER (SAMPLER_DESIGN S1). Loads an audio file as a new SOURCE and mints a SLOT that
    /// plays it — the whole "drop a sample, name it from a row, hear it" line. Carries
    /// UiSamplerLoadPayload, not the generic one.
    SamplerLoad = 73,

    /// Edits ONE FIELD of one sampler slot (SamplerSlotField + a signed value). One field at a
    /// time rather than a whole-slot payload: a whole slot does not fit 40 bytes, and it would
    /// make every edit a read-modify-write, so two callers touching different fields would
    /// clobber each other with stale copies of the rest.
    SamplerSetSlot = 74,

    /// Asks the engine to publish one sampler device's kit into a `UiSamplerKitSlot`.
    RequestSamplerKit = 75,

    /// Slices a source: transient detection, equal division, or clear. Mints STABLE marker ids —
    /// an insert never renumbers an existing one, which is what lets a chop be re-cut while it
    /// plays (SAMPLER_DESIGN §5.1).
    SamplerSlice = 76,

    /// Adds, moves or removes ONE slice marker. The one that matters live: dragging a boundary
    /// changes what a slice PLAYS without touching what any row SAYS.
    SamplerMarker = 77,

    /// Writes the PATTERN that reproduces a chop: one row per slice, each naming its slice by ID.
    /// Octatrack's CREATE LINEAR LOCKS and Bitwig's slice-to-drum-machine clip as one command —
    /// with the difference that matters: re-cutting afterwards moves what the rows PLAY without
    /// moving what they SAY. Bitwig emits its clip once, one-way.
    SamplerEmitRows = 78,

    /// Save/load the project as a `.uni` MODULE — a zip holding project.json plus every sample.
    /// "It is easy to send someone the zip." Broken sample links stop existing, because there
    /// are no links. Same packed-NAME payload as SaveProject/LoadProject: this is the same
    /// operation at a different level of packing, not a different kind of save.
    SaveModule = 79,
    LoadModule = 80,

    /// Writes a note's ROW OPS — retrigger, probability, sound address, sample offset, onset
    /// delay — addressed by NOTE ID. These have been published since v23/v32 and no command
    /// could set one, so every op was readable and none writable. No kShmVersion bump: every
    /// field it writes is already on the wire outbound.
    SetRowOps = 81,

    /// Sets a sampler modulator's ADSR. ADSR fits in 40 bytes; a hand-drawn multi-point
    /// envelope does not and is deliberately not here — that needs an inward bulk carrier.
    SamplerSetEnvelope = 82,

    /// One chunk of a longer command — the inward bulk carrier.
    BulkChunk = 83,
    /// A hand-drawn multi-point envelope, carried over BulkChunk.
    SamplerSetEnvelopePoints = 84,

    /// Sets a sampler modulator's LFO — note-retriggered, on any modulation target.
    SamplerSetLfo = 85,
    SamplerSetFilter = 86,
    /// Per track: pitch never selects a slot, the note's `sound` names it, and a blank `sound`
    /// plays the lowest slot chromatically. Off by default — a blank `sound` still means "the
    /// keymap picks from pitch" (R5). Owner ruling, docs/SAMPLER_DESIGN.md section 8 Q2.
    SetTrackSoundAddressed = 87,
    /// Device-level sampler fields, addressed by field id like SamplerSetSlot (74).
    /// `defaultGate` is the per-bank "ignore note-offs" default; `voiceCap` and `defaultView`
    /// were persisted and rendered and reachable by nothing until this existed.
    SamplerSetDevice = 88,
    /// Fold a track. `collapsed` was persisted, published and restored on load, and settable by
    /// nothing — the fold could be drawn and never set.
    SetTrackCollapsed = 89,
    /// Rename a sampler slot, over the BulkChunk carrier. `name` was persisted by the project
    /// format and published by NOTHING, so no UI could read a pad's name let alone change it.
    SamplerSetSlotName = 90,
    /// Bit depth and sample-rate reduction on a sampler MOD SET — the SP-1200 character.
    /// Applied BEFORE the filter, which is the order the machines it imitates had.
    SamplerSetVintage = 91,
    /// A lane's SUBDIVISION — rows per beat on this track. Rides UiCommandPayload (trackId +
    /// value0), like SetTrackCollapsed. `lines_per_beat` was persisted, published in
    /// uiLinesPerBeat and honoured by the per-lane grid, and settable by nothing. Range is
    /// 1..=31 — the clip-grid packer gives it five bits — and out of range is REFUSED, since a
    /// 32 packs as a 0 and 0 is the packer's "no grid here" sentinel.
    SetTrackLinesPerBeat = 92,
    /// Cut-on-next, or let it ring — per lane. Rides UiCommandPayload (trackId + value0).
    /// addNoteToClip truncates the sounding note in the same column IN THE DOCUMENT, so the
    /// duration the player typed is destroyed at entry. value0 1 skips that truncate. Nothing
    /// in playback changes — the scheduler already honours overlapping durations.
    SetTrackAllowNoteOverlap = 93,
    /// A CLIP's own subdivision and meter (task #43 phase 2). ProjectClip carries linesPerBeat
    /// and a time signature; all three persist, all three publish packed into UiClipExtent's flag
    /// bits, and the tracker draws the CLIP's grid before the track's — so the authority in that
    /// chain was the one thing no command could write. Not a second answer to 92; the other half.
    SetClipGrid = 94,
    /// An audio clip's in-point, gain and fades. All four persist, all four publish, the renderer
    /// bakes all four into the region it schedules — and until this, nothing wrote any of them, so
    /// an audio region was read-only from every surface.
    SetAudioClipField = 95,
    /// Remove an automation point, addressed exactly as WriteAutomationPoint (60) addresses one.
    /// Its own opcode rather than a flag on 60, following DeleteNote beside the note write: a
    /// destructive operation reached by setting a bit on a constructive one is one typo away from
    /// deleting what the caller meant to write.
    DeleteAutomationPoint = 96,
    /// Ask for one modulator's envelope SHAPE — points, both loop ranges, the release fade, the
    /// time base and the rate. Opcode 84 could write all of that and nothing could read it back,
    /// so a pencil editor would have been write-only.
    RequestSamplerEnvelope = 97,
    /// Set a clip's `name` or its audio `source_path`. Rides the BulkChunk carrier (83) as an
    /// inner command — a string does not fit the 40-byte ring payload, which is the only reason
    /// these two fields had no writer at all.
    SetClipText = 98,
}

/// Where a route points. Mirrors daw::TrackRouteKind.
pub const ROUTE_KIND_NONE: u8 = 0;
pub const ROUTE_KIND_MASTER: u8 = 1;
pub const ROUTE_KIND_TRACK: u8 = 2;
pub const ROUTE_KIND_EXTERNAL: u8 = 3;

/// "Leave this field alone" in ResizePlacement.
///
/// All-ones, which a real nanotick can never be: the engine's tick space is
/// bounded well under 2^63, so no legitimate start or length can collide with
/// it. Chosen that way on purpose — a sentinel inside the value range is how
/// `parent_id` bit us, where 0 meant both "track 0" and "no parent".
pub const PLACEMENT_UNCHANGED: u64 = u64::MAX;
/// "Do not change lane" in MovePlacement, in the note_pitch field.
pub const PLACEMENT_SAME_TRACK: u32 = u32::MAX;

pub const MIXER_FLAG_MUTE: u16 = 1 << 0;
pub const MIXER_FLAG_SOLO: u16 = 1 << 1;
/// `UiClipExtent.flags` bit 23: this appearance takes edits LOCALLY — a note typed into it
/// becomes an override on it rather than a change to the clip every appearance shares.
///
/// Chosen over a global "local edit mode" on failure asymmetry: forget the toggle and the note
/// appears in every appearance (loud, one undo away), whereas being in the wrong global mode
/// makes a fix silently fail to propagate (quiet, and you may not notice for an hour).
pub const UI_CLIP_EXTENT_LOCAL_EDITS: u32 = 1 << 23;

/// M2.57 bit 24: this appearance HAS AN ALTERNATE clip to swap to (usually an agent's draft).
/// Published so the A/B can be offered at all — an alternate nobody can see is the same as not
/// having one. What PLAYS is always the extent's `clip_id`; this only says there is another.
pub const UI_CLIP_EXTENT_HAS_ALTERNATE: u32 = 1 << 24;

/// HOW MANY APPEARANCES PLAY EACH CLIP, keyed by clip id.
///
/// Two placements of one clip are the same music seen twice, so an edit to either reaches both.
/// This is the number that answers "if I edit here, what else changes?", and it is here — beside
/// the extents it reads — because two surfaces ask it: the agent's `shared_clips` tool and
/// `daw-cli get shared`. They had a copy each, agreeing today.
///
/// COUNT OVER ALL EXTENTS, NEVER A FILTERED SET. A clip is shared whether or not its other
/// appearances are on the track being asked about, so a caller narrowing its OUTPUT to one track
/// must still pass every extent in here. Counting the filtered set answers 1 for a clip with
/// three appearances — and 1 reads as "safe to edit, nothing else changes", which is the precise
/// opposite of the truth. That failure is silent and destroys work, which is why this takes the
/// whole slice and does no filtering of its own: there is no way to pass it a filtered set by
/// accident that does not look wrong at the call site.
pub fn clip_appearances(extents: &[UiClipExtent]) -> std::collections::HashMap<u32, u32> {
    let mut uses = std::collections::HashMap::new();
    for e in extents {
        *uses.entry(e.clip_id).or_insert(0u32) += 1;
    }
    uses
}

/// Per-device addressing for the patcher graph commands, carried in the payload's `flags`
/// because the payload is exactly 40 bytes and full. Bit 15 marks the id PRESENT; bits 0-14 are
/// the id, and 0x7FFF is exactly `STABLE_DEVICE_ID_MAX` — this carrier is WHY the ceiling is
/// 0x7FFF. The presence bit matters, though its reason has changed: it used to be "device ids
/// start at 0, so a bare 0 cannot mean unspecified", and zero is never a device identity now
/// (AE-P1.2 G2-B item 18, R-DEVICE-ID-LIFETIME). The bit stays because without it every caller
/// sending flags=0 would start addressing a device instead of the legacy shared pool — a
/// compatibility fact rather than a numbering one.
pub const UI_PATCHER_FLAG_HAS_DEVICE_ID: u16 = 1 << 15;
pub const UI_PATCHER_DEVICE_ID_MASK: u16 = 0x7FFF;

/// The largest project-global stable device id. Mirrors `kStableDeviceIdMax` in
/// `apps/stable_device_id.h`; the pair is compared by `tools/version_parity_check.sh`.
///
/// 0x7FFF is not a round number chosen for comfort — it is the NARROWEST lossless bound across
/// every carrier that names a device, and `UI_PATCHER_DEVICE_ID_MASK` directly above is the
/// carrier that sets it. AE-P1.2 G2-B item 18, R-DEVICE-ID-LIFETIME.
pub const STABLE_DEVICE_ID_MAX: u32 = 0x7FFF;

/// Is this a device IDENTITY? Zero is the absence of a device everywhere in this protocol, so it
/// is not one — which is why `UI_PATCHER_FLAG_HAS_DEVICE_ID` exists rather than a bare zero
/// meaning "unspecified".
///
/// Takes u64 so a value parsed from a command line or a JSON body is checked BEFORE it is cast:
/// `id as u16 & MASK` silently turns 32768 into 0 and 65537 into 1, which addresses a different
/// device that probably exists. The whole point is to refuse before the cast, not after.
pub fn is_stable_device_id(id: u64) -> bool {
    id >= 1 && id <= STABLE_DEVICE_ID_MAX as u64
}

/// `ui_track_mix_flags` bit 2: does this track quantize its notes to the harmony timeline?
///
/// SetTrackHarmonyQuantize (10) had no read-back at all, so the only control that could be built
/// was a WRITE-ONLY TOGGLE — press it and the interface can never say which way it is set. After
/// a load it would have to guess or show nothing, and a control drawing a state it invented is
/// worse than no control.
///
/// Bits 0-1 of that byte are the mute/solo COMMAND flags above; this is the first read-back-only
/// bit in it, so the byte is a union of two enumerations. Do not add a command flag at 1 << 2.
/// uiTrackMixFlags bits 0 and 1: this track's mute and solo.
///
/// The SAME BITS as MIXER_FLAG_MUTE/SOLO, and deliberately a separate pair of constants: those
/// are `u16` because they live in a COMMAND payload's flags word, these are `u8` because they
/// live in the published per-track byte. Reaching for the command family to read the SHM byte is
/// a type error today and was a silent one before these existed — which is how bits 0 and 1 came
/// to be the only two in this byte with no name of their own.
pub const MIX_FLAG_MUTE: u8 = 1 << 0;
pub const MIX_FLAG_SOLO: u8 = 1 << 1;
pub const MIX_FLAG_HARMONY_QUANTIZE: u8 = 1 << 2;
/// Bit 3: this track addresses its sampler by SOUND, not by pitch (opcode 87, section 8 Q2).
/// Read-back only, like bit 2, and added without a kShmVersion bump for the same reason.
pub const MIX_FLAG_SOUND_ADDRESSED: u8 = 1 << 3;
/// uiTrackMixFlags bit 4: does entering a note over a sounding one in this column LET IT RING?
/// Off truncates the sounding note IN THE DOCUMENT, which is the one edit here that loses data.
pub const MIX_FLAG_ALLOW_NOTE_OVERLAP: u8 = 1 << 4;
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
    /// A clip edit was REFUSED, and why. Before this, a stale-base edit was dropped in
    /// total silence — no error, no code, nothing on the outbound ring — so every
    /// symptom was "the app does nothing". Payload: `UiClipRejectPayload`.
    /// ResyncNeeded (4) is still emitted alongside, unchanged.
    ClipRejected = 15,
    /// SavePatcherPreset's outcome. Without it a "save this graph as a preset" button could
    /// only lie about half the time — daw-cli read the path off the engine's stderr, which a
    /// browser cannot.
    PresetSaved = 16,
    /// A SAMPLER COMMAND WAS REFUSED, and why. Payload: `UiSamplerRejectPayload`.
    ///
    /// Every sampler verb refused into the engine's log and nowhere else — 20 sites across seven
    /// commands. daw-cli can read stderr; a browser cannot, so from a UI each one was a silent
    /// no-op that reported success. Additive like 15 and 16: a reader that switches on diff_type
    /// and ignores unknown values is unaffected, so no kShmVersion bump.
    SamplerRejected = 17,
}

/// Why a sampler command was refused. Distinct codes rather than one "rejected", because the
/// fix differs: `NoSuchSlot` means stop and re-read the kit, `BadValue` means the caller clamped
/// wrong and retrying identically will never help, `NotASampler` means the device id names
/// something else entirely.
#[repr(u16)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum UiSamplerRejectReason {
    None = 0,
    NoSuchTrack = 1,
    NoSuchDevice = 2,
    NoSuchSlot = 3,
    NoSuchModSet = 4,
    NoSuchModulator = 5,
    NoSuchSource = 6,
    NoSuchSliceSet = 7,
    BadValue = 8,
    NotASampler = 9,
    LoadFailed = 10,
}

/// Rides the same 40-byte diff slot. `diff_type` is FIRST — dispatch on it, never on size.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct UiSamplerRejectPayload {
    pub diff_type: u16,
    /// `UiSamplerRejectReason`.
    pub reason: u16,
    /// The `UiCommandType` refused, so a caller can match it to what it sent.
    pub command_type: u16,
    /// The id that could not be found — slot, mod set, modulator, source or slice set, according
    /// to `reason`. One field rather than five: exactly one of them is ever the answer, and five
    /// parallel ids would be four opportunities to disagree.
    pub target_id: u16,
    pub track_id: u32,
    pub device_id: u32,
    pub reserved: [u32; 4],
    // Command identity, offset 32 in every refusal payload (P2-CMD-00). Two u32 and not
    // a u64: EventEntry::payload sits at offset 20, so a u64 member would raise alignof
    // to 8 and make the C++ cast sites undefined behaviour.
    pub correlation_lo: u32,
    pub correlation_hi: u32,
}

/// Why a clip edit was refused. The distinction matters because the fix differs: a
/// stale base means re-read `clip_version_for_track` and retry; an unknown track means
/// the caller is addressing something that is not there and retrying will never help.
#[repr(u16)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum UiClipRejectReason {
    None = 0,
    StaleBase = 1,
    UnknownTrack = 2,
}

/// Rides the same 40-byte diff slot as every other payload. `diff_type` is FIRST —
/// dispatch on it, never on the payload's size (UiChainDiffPayload and
/// UiChainErrorPayload are both 40 bytes).
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct UiPresetSavedPayload {
    pub diff_type: u16,
    /// 1 = written, 0 = failed. The reason is on the engine's event stream, not here.
    pub ok: u16,
    /// The name the caller SENT, echoed so it can be matched exactly. Not the path: the engine
    /// owns the preset directory, and a 28-byte path could truncate — a caller matching a
    /// truncated path against its request could conclude the wrong save succeeded.
    pub name: [u8; 28],
    pub reserved: [u32; 2],
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct UiClipRejectPayload {
    pub diff_type: u16,
    pub reason: u16,
    pub track_id: u32,
    /// What the caller presented.
    pub sent_base: u32,
    /// What the engine holds — the value to retry with.
    pub current_base: u32,
    pub command_type: u16,
    pub reserved: u16,
    pub reserved2: [u32; 3],
    // Command identity, offset 32 in every refusal payload (P2-CMD-00). Two u32 and not
    // a u64: EventEntry::payload sits at offset 20, so a u64 member would raise alignof
    // to 8 and make the C++ cast sites undefined behaviour.
    pub correlation_lo: u32,
    pub correlation_hi: u32,
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

/// `flags` bit 1: `source_id` is a SAMPLER source's per-device LOCAL id, and reserved0/reserved1
/// name its track and device. The waveform store interns by resolved PATH while a sampler's local
/// id is a per-device counter — two id spaces — so without this a sample view's requests address
/// nothing and answer nothing, forever. The engine translates the triple to the same path-keyed
/// store, so one file loaded into a sampler AND placed as an audio clip is one pyramid. The reply
/// echoes the id that was SENT, so existing keying keeps working.
pub const WAVEFORM_REQUEST_SAMPLER_SOURCE: u16 = 1 << 1;

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


/// SetTrackRouting (19). REPLACE semantics: the engine writes all four routes plus the
/// pre-fader flag from this one payload, so a caller must send the state it wants, not a
/// delta. There is deliberately no partial form — and note there is currently NO routing
/// read-back in the SHM header (routing is only published as an outbound diff), so a
/// read-modify-write is not available either. Both are on the list for the item-25
/// contract batch.
///
/// `sidechain` is absent from this payload: TrackRouting carries one, the handler leaves
/// it alone, and it can only be set from a project file today.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct UiTrackRoutingPayload {
    pub command_type: u16,
    /// bit 0: pre-fader send
    pub flags: u16,
    pub track_id: u32,
    pub base_version: u32,
    pub midi_in_kind: u8,
    pub midi_out_kind: u8,
    pub audio_in_kind: u8,
    pub audio_out_kind: u8,
    pub midi_in_track_id: u32,
    pub midi_out_track_id: u32,
    pub audio_in_track_id: u32,
    pub audio_out_track_id: u32,
    pub midi_in_input_id: u32,
    pub audio_in_input_id: u32,
}

/// AddModLink / RemoveModLink (20/21). `flags` packs the kinds and rate — bits 0-3
/// source kind, 4-7 target kind, 8-9 rate, bit 10 enabled — because the four enums are
/// small and the payload is full. `link_id` = MOD_LINK_ID_AUTO lets the engine assign.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct UiModLinkCommandPayload {
    pub command_type: u16,
    pub flags: u16,
    pub track_id: u32,
    pub base_version: u32,
    pub link_id: u32,
    pub source_device_id: u32,
    pub source_id: u32,
    pub target_device_id: u32,
    pub target_id: u32,
    pub depth: f32,
    pub bias: f32,
}

/// SetModLinkUid16 (22): names the VST parameter a link targets, by its 16-byte plugin
/// uid. Separate from the link itself because it does not fit alongside it.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct UiModLinkUid16Payload {
    pub command_type: u16,
    pub flags: u16,
    pub track_id: u32,
    pub base_version: u32,
    pub link_id: u32,
    pub uid16: [u8; 16],
    pub reserved: [u8; 8],
}

impl Default for UiModLinkUid16Payload {
    fn default() -> Self {
        Self {
            command_type: 0,
            flags: 0,
            track_id: 0,
            base_version: 0,
            link_id: 0,
            uid16: [0u8; 16],
            reserved: [0u8; 8],
        }
    }
}

/// SetModSourceValue (23): drives a macro/source value directly, which is how a macro
/// knob is turned.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct UiModSourceValuePayload {
    pub command_type: u16,
    pub flags: u16,
    pub track_id: u32,
    pub base_version: u32,
    pub source_device_id: u32,
    pub source_id: u32,
    pub value: f32,
    pub reserved: [u8; 16],
}

impl Default for UiModSourceValuePayload {
    fn default() -> Self {
        Self {
            command_type: 0,
            flags: 0,
            track_id: 0,
            base_version: 0,
            source_device_id: 0,
            source_id: 0,
            value: 0.0,
            reserved: [0u8; 16],
        }
    }
}

/// v29: a MARKER — a named tick. Replaces `UiArrangeSection`, same 56 bytes, so the region and
/// every offset after it are unchanged; only the meaning moved, which is exactly why the wire
/// version had to.
///
/// THE BAR IS PUBLISHED RESOLVED, and that is why this region exists rather than the client just
/// reading the marker list: a bar number is a PREFIX SUM across every meter change before it, NOT
/// `tick / bar_length`. Deriving it here would be reimplementing the engine's
/// `TimeSignatureMap::barBeatAt`, and the first disagreement draws a marker at the wrong bar with
/// nothing reporting an error.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct UiMarker {
    pub id: u32,
    /// ONE-based, like every ruler. Prefix-summed through the meter map.
    pub bar: u32,
    /// ONE-based within the bar.
    pub beat: u32,
    pub color_rgb: u32,
    pub nanotick: u64,
    pub reserved: u64,
    pub name: [u8; 24],
}

impl Default for UiMarker {
    fn default() -> Self {
        Self {
            id: 0,
            bar: 1,
            beat: 1,
            color_rgb: 0,
            nanotick: 0,
            reserved: 0,
            name: [0u8; 24],
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct UiTimeSigPoint {
    pub nanotick: u64,
    pub numerator: u32,
    pub denominator: u32,
}

/// The markers and the song's meter in ONE region with one version, because they are read
/// together — a marker's BAR comes from the meter, and two regions could be seen mismatched.
/// `version` moves when the markers or the meter change and never when a note does, so renaming
/// a marker does not invalidate an in-flight clip edit. **0 means a write is IN FLIGHT** — retry
/// rather than treating it as empty.
///
/// v29: the time-signature points are AUTHORITATIVE now, not a derived read-back of a spine. This
/// is where mid-song meter lives.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct UiArrangeSummaryRegion {
    pub version: u32,
    pub marker_count: u32,
    pub time_sig_count: u32,
    /// How many did NOT fit. Non-zero means the list you are reading is incomplete —
    /// published rather than dropped silently, because a truncated list that says nothing
    /// reads as a complete one.
    pub markers_truncated: u32,
    pub time_sig_truncated: u32,
    pub reserved: u32,
    /// The furthest placement end. NOT marker-derived: material can sit past the last marker, and
    /// it plays and is unnamed. Also mirrored in the header as `ui_song_end_tick`, written from
    /// the same atomic — the same number where a per-frame reader can get it without a second
    /// region read.
    pub song_end_tick: u64,
    pub markers: [UiMarker; K_UI_MAX_MARKERS],
    pub time_sig_points: [UiTimeSigPoint; K_UI_MAX_TIME_SIG_POINTS],
}

/// M3.24: the override badge in UiClipExtent.flags. `override_count` SATURATES at 255 —
/// a count that wrapped would draw a placement with 256 overrides as unmodified, which is
/// the one thing the badge exists to prevent — and `HAS_OVERRIDES` is set whenever the
/// real count is non-zero, so a saturated count can never read as none.
pub const UI_CLIP_EXTENT_OVERRIDE_SHIFT: u32 = 14;
pub const UI_CLIP_EXTENT_OVERRIDE_MASK: u32 = 0xFF << UI_CLIP_EXTENT_OVERRIDE_SHIFT;
pub const UI_CLIP_EXTENT_HAS_OVERRIDES: u32 = 1 << 22;

/// M3.24: edit scope on WriteNote / DeleteNote / WriteChord. CLEAR = the CLIP (every
/// appearance) — today's behaviour and the default. SET = THIS APPEARANCE, recorded as an
/// add or a mute on the placement. Never inferred: which one the caller meant is the
/// difference between "fix the bass in chorus 1 and all three change" and "the hat you
/// added to chorus 3 stays there", and no rule about whether the cell is occupied gets
/// both right.
pub const UI_EDIT_SCOPE_LOCAL: u16 = 1 << 15;
pub const UI_EDIT_COLUMN_MASK: u16 = 0x00FF;

/// An edit column, refused rather than truncated — the single owner of that rule.
///
/// SIX CALL SITES HAD SIX CASTS. Three in daw-cli (`do note`, `do phrase`, `do chord`) and three
/// in daw-agent (`add_notes`, `add_chords`, `delete_chord`), all writing the column into the same
/// `flags` field. Five were `as u16` and one clamped to `u16::MAX`. Not one of them was the
/// READER's bound: the engine takes `flags & kUiEditColumnMask`, which is 0x00FF.
///
/// AND THE OVERFLOW IS NOT A WRONG COLUMN, IT IS A DIFFERENT KIND OF EDIT. The two constants
/// above share the field, so 32768 does not land in column 0 — it sets `UI_EDIT_SCOPE_LOCAL` and
/// turns a document edit into a placement-local override. daw-cli's `do note` even ORs that flag
/// in explicitly on the next expression, so the collision is visible on one line if anyone looks.
///
/// It lives in the layout crate rather than in either binary because both binaries write this
/// wire and a rule with six sites and no owner is a rule that comes back. The sidecar is the third
/// producer; it clamps to 0..255 and masks, which is safe but is a fourth spelling of one idea.
pub fn edit_column(value: u64) -> Result<u16, String> {
    if value > u64::from(UI_EDIT_COLUMN_MASK) {
        return Err(format!(
            "column {value} does not fit the byte the engine reads (0..255). The rest of that \
             field is FLAG BITS — bit 15 is the local-edit scope — so this would not simply land \
             in the wrong column, it would change what kind of edit this is"));
    }
    Ok(value as u16)
}

pub const K_UI_MAX_MARKERS: usize = 64;
pub const K_UI_MAX_TIME_SIG_POINTS: usize = 32;

/// M3.27 (60): one automation point. `param_id` is the STRING the clip is keyed on (the
/// engine hashes it to the uid16 the wire and the param mirror use). `value` is the
/// plugin's normalised 0..1. flags bit 0 = DISCRETE (step instead of interpolate), applied
/// when the clip is CREATED and ignored afterwards — a switch that changed meaning halfway
/// through a curve would make the curve unreadable.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct UiAutomationPointPayload {
    pub command_type: u16,
    pub flags: u16,
    pub track_id: u32,
    pub target_plugin_index: u32,
    pub nanotick_lo: u32,
    pub nanotick_hi: u32,
    pub value: f32,
    pub param_id: [u8; 16],
}

impl Default for UiAutomationPointPayload {
    fn default() -> Self {
        Self {
            command_type: 0,
            flags: 0,
            track_id: 0,
            target_plugin_index: 0,
            nanotick_lo: 0,
            nanotick_hi: 0,
            value: 0.0,
            param_id: [0u8; 16],
        }
    }
}

pub const UI_AUTOMATION_DISCRETE: u16 = 1 << 0;

/// v28: ASK for one automation lane's points (`UiCommandType::RequestAutomationLane`). Its own
/// struct rather than a reuse of `UiAutomationPointPayload`, for one reason: the CLIENT owns
/// `request_seq`, exactly as `RequestWaveform` does. That is what lets a caller know which slot
/// its answer will land in before it asks, and match the echo without racing on a counter it
/// never wrote.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct UiAutomationLaneRequestPayload {
    pub command_type: u16,
    pub flags: u16,
    /// Answered into `slots[request_seq % K_UI_AUTOMATION_SLOTS]`.
    pub request_seq: u32,
    pub track_id: u32,
    pub target_plugin_index: u32,
    pub param_id: [u8; 16],
    pub reserved0: u32,
    pub reserved1: u32,
}

impl Default for UiAutomationLaneRequestPayload {
    fn default() -> Self {
        Self {
            command_type: UiCommandType::RequestAutomationLane as u16,
            flags: 0,
            request_seq: 0,
            track_id: 0,
            target_plugin_index: 0,
            param_id: [0u8; 16],
            reserved0: 0,
            reserved1: 0,
        }
    }
}

/// v29 MARKER commands (64-67). A marker is a named tick; `marker_id` addresses an existing one
/// (0 with AddMarker = let the engine assign and report the id it chose). Marker ops are TOTAL:
/// they move no material and cannot be refused for anything but a bad id.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct UiMarkerCommandPayload {
    pub command_type: u16,
    pub flags: u16,
    pub marker_id: u32,
    pub nanotick_lo: u32,
    pub nanotick_hi: u32,
    pub color_rgb: u32,
    /// 20 bytes — what is left of the 40-byte slot. A longer name is truncated.
    pub name: [u8; 20],
}

impl Default for UiMarkerCommandPayload {
    fn default() -> Self {
        Self {
            command_type: 0,
            flags: 0,
            marker_id: 0,
            nanotick_lo: 0,
            nanotick_hi: 0,
            color_rgb: 0,
            name: [0u8; 20],
        }
    }
}

/// v29 TIMELINE commands (68-69) — the two that change time rather than a label.
///
///   `SetTimeSignature`   insert-or-replace a meter point at `nanotick`; `UI_TIME_SIG_FLATTEN`
///                        replaces the whole map with this one signature.
///   `InsertRemoveTime`   `delta` BARS of arrangement time inserted (positive) or removed
///                        (negative) at `nanotick`. Bars, not ticks, because a bar's length
///                        depends on the meter in force there — which the engine knows
///                        authoritatively and a client would have to re-derive.
///                        `UI_TIME_EDIT_DELTA_IS_TICKS` switches to raw ticks.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct UiArrangeTimeCommandPayload {
    pub command_type: u16,
    pub flags: u16,
    pub nanotick_lo: u32,
    pub nanotick_hi: u32,
    pub delta: i32,
    pub numerator: u32,
    pub denominator: u32,
    pub reserved0: u32,
    pub reserved1: u32,
    pub reserved2: u32,
    pub reserved3: u32,
}

pub const UI_TIME_SIG_FLATTEN: u16 = 1 << 0;
pub const UI_TIME_EDIT_DELTA_IS_TICKS: u16 = 1 << 1;

/// PatcherNodeType, mirroring apps/patcher_graph.h.
pub const PATCHER_NODE_RUST_KERNEL: u32 = 0;
pub const PATCHER_NODE_EUCLIDEAN: u32 = 1;
pub const PATCHER_NODE_PASSTHROUGH: u32 = 2;
pub const PATCHER_NODE_AUDIO_PASSTHROUGH: u32 = 3;
pub const PATCHER_NODE_LFO: u32 = 4;
pub const PATCHER_NODE_RANDOM_DEGREE: u32 = 5;
/// SliceSelect: chooses WHICH SOUND a note plays, leaving the pitch alone.
pub const PATCHER_NODE_SLICE_SELECT: u32 = 7;
pub const PATCHER_NODE_EVENT_OUT: u32 = 6;

/// PatcherPortKind.
pub const PATCHER_PORT_EVENT: u32 = 0;
pub const PATCHER_PORT_AUDIO: u32 = 1;
pub const PATCHER_PORT_CV: u32 = 2;

/// ModSourceKind / ModTargetKind / ModRate, mirroring apps/modulation.h. Modulation
/// flows FORWARD: the source device must not be LATER in the chain than the target.
/// Same device is legal and is the common case with per-device patchers.
pub const MOD_SOURCE_MACRO: u16 = 0;
pub const MOD_SOURCE_LFO: u16 = 1;
pub const MOD_SOURCE_ENVELOPE: u16 = 2;
pub const MOD_SOURCE_PATCHER_NODE_OUTPUT: u16 = 3;
pub const MOD_TARGET_VST_PARAM: u16 = 0;
pub const MOD_TARGET_PATCHER_PARAM: u16 = 1;
pub const MOD_TARGET_PATCHER_MACRO: u16 = 2;
pub const MOD_RATE_BLOCK: u16 = 0;
pub const MOD_RATE_SAMPLE: u16 = 1;
/// Let the engine assign the link id (apps/modulation.h kModLinkIdAuto).
pub const MOD_LINK_ID_AUTO: u32 = 0xFFFF_FFFF;

/// TrackRouteKind, mirroring apps/track_routing.h.
pub const TRACK_ROUTE_NONE: u8 = 0;
pub const TRACK_ROUTE_MASTER: u8 = 1;
pub const TRACK_ROUTE_TRACK: u8 = 2;
pub const TRACK_ROUTE_EXTERNAL_INPUT: u8 = 3;

#[repr(C)]
/// SAMPLER LOAD (opcode 73). Mirrors apps/event_payloads.h UiSamplerLoadPayload exactly —
/// 40 bytes, the whole command payload. `name` is a PROJECT-RELATIVE file name rather than a
/// path: that is the module model (SAMPLER_DESIGN R3), not merely a size constraint. A project
/// that refers to a sample by absolute path stops playing the moment you send it to someone.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct UiSamplerLoadPayload {
    pub command_type: u16,
    pub flags: u16,
    pub track_id: u32,
    pub device_id: u32,
    pub root_key: u8,
    pub reserved: [u8; 3],
    pub name: [u8; 24],
}

/// keyLow == keyHigh == rootKey: how a drum stays a drum across the keyboard. Clear it for a
/// playable zone. There is no mapping-MODE stored anywhere — this chooses which KEYS to write.
pub const SAMPLER_LOAD_FIXED_PITCH: u16 = 1 << 0;

/// Which device-level sampler field `SamplerSetDevice` (88) is speaking about.
pub const SAMPLER_DEVICE_FIELD_DEFAULT_GATE: u16 = 1;
pub const SAMPLER_DEVICE_FIELD_VOICE_CAP: u16 = 2;
pub const SAMPLER_DEVICE_FIELD_DEFAULT_VIEW: u16 = 3;

/// SamplerSetDevice (88). The same shape as `UiSamplerSetSlotPayload` minus the slot id — these
/// are properties of the DEVICE, and a slot field saying "not a slot" would be a sentinel nobody
/// needs.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct UiSamplerSetDevicePayload {
    pub command_type: u16,
    pub field: u16,
    pub track_id: u32,
    pub device_id: u32,
    pub value: i32,
    pub reserved: [u8; 24],
}

/// Mirrors apps/event_payloads.h UiSamplerSetSlotPayload. `value` is SIGNED: gain, pan, tune and
/// pitch-track all take negative values as normal settings.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct UiSamplerSetSlotPayload {
    pub command_type: u16,
    pub field: u16,
    pub track_id: u32,
    pub device_id: u32,
    pub slot_id: u32,
    pub value: i32,
    pub reserved: [u8; 20],
}

/// Which slot field SamplerSetSlot writes. NAMED rather than an index into the struct, so adding
/// a field never renumbers an existing one — a renumbered selector would silently write the
/// wrong field on a saved macro or an agent's script.
/// The DEVICE-level sampler fields (opcode 88). Names rather than numbers, for the reason the
/// slot table gives below: a caller who mistypes gets the list back instead of a silent no-op.
pub const SAMPLER_DEVICE_FIELDS: &[(&str, u16)] = &[
    ("default-gate", SAMPLER_DEVICE_FIELD_DEFAULT_GATE),
    ("voice-cap", SAMPLER_DEVICE_FIELD_VOICE_CAP),
    ("default-view", SAMPLER_DEVICE_FIELD_DEFAULT_VIEW),
];

pub const SAMPLER_SLOT_FIELDS: &[(&str, u16)] = &[
    ("voice-group", 0),
    ("nna", 1),
    ("gate", 2),
    ("reverse", 3),
    ("gain-mb", 4),
    ("pan", 5),
    ("tune-cents", 6),
    ("pitch-track", 7),
    ("root", 8),
    ("key-low", 9),
    ("key-high", 10),
    ("vel-low", 11),
    ("vel-high", 12),
    ("select-mode", 13),
    ("polyphony", 14),
    ("choke-fade-us", 15),
    ("mod-set", 16),
    ("stem", 17),
    ("quality", 18),
    ("layer-group", 19),
    // The loop and the trim, rendered by the voice since S3 and unsettable until 2026-07-31.
    // A frame position is capped by the payload's int32 value at 2147483647 — about 12.4 hours
    // at 48 kHz.
    ("loop-mode", 20),
    ("sustain-loop", 21),
    ("loop-start", 22),
    ("loop-end", 23),
    ("loop-xfade", 24),
    ("start-frame", 25),
    ("end-frame", 26),
    // Repointing a pad. Set at mint by sampler-load / sampler-slice and, until these existed,
    // never again — so "this pad should play that other file" meant rebuilding the slot.
    ("source", 27),
    ("slice", 28),
];

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
    pub reserved: [u32; 3],
    // Command identity, offset 32 in every refusal payload (P2-CMD-00). Two u32 and not
    // a u64: EventEntry::payload sits at offset 20, so a u64 member would raise alignof
    // to 8 and make the C++ cast sites undefined behaviour.
    pub correlation_lo: u32,
    pub correlation_hi: u32,
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
    // Command identity, offset 32 in every refusal payload (P2-CMD-00). Two u32 and not
    // a u64: EventEntry::payload sits at offset 20, so a u64 member would raise alignof
    // to 8 and make the C++ cast sites undefined behaviour.
    pub correlation_lo: u32,
    pub correlation_hi: u32,
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
        same!(UiCommandOutcomeEntry, sys::daw_UiCommandOutcomeEntry);
        same!(UiCommandOutcomeRegion, sys::daw_UiCommandOutcomeRegion);
        same!(RingHeader, sys::daw_RingHeader);
        same!(EventEntry, sys::daw_EventEntry);
        same!(UiEditBatchEntry, sys::daw_UiEditBatchEntry);
        same!(UiPatcherNode, sys::daw_UiPatcherNode);
        same!(UiPatcherEdge, sys::daw_UiPatcherEdge);
        same!(UiPatcherRegion, sys::daw_UiPatcherRegion);
        // UiScale/UiScaleRegion/UiDeviceParam/UiDeviceParamsRegion are now aliases
        // OF the generated structs, so a parity check on them is vacuous.
        same!(UiClipNote, sys::daw_UiClipNote);
        same!(BlockMailbox, sys::daw_BlockMailbox);
        same!(UiArrangeSummaryRegion, sys::daw_UiArrangeSummaryRegion);
        same!(UiArrangeTimeCommandPayload, sys::daw_UiArrangeTimeCommandPayload);
        same!(UiAutomationLaneRequestPayload, sys::daw_UiAutomationLaneRequestPayload);
        same!(UiAutomationPointPayload, sys::daw_UiAutomationPointPayload);
        same!(UiBusDiffPayload, sys::daw_UiBusDiffPayload);
        same!(UiChainCommandPayload, sys::daw_UiChainCommandPayload);
        same!(UiChainDiffPayload, sys::daw_UiChainDiffPayload);
        same!(UiChainErrorPayload, sys::daw_UiChainErrorPayload);
        same!(UiChordCommandPayload, sys::daw_UiChordCommandPayload);
        same!(UiChordDiffPayload, sys::daw_UiChordDiffPayload);
        same!(UiClipChord, sys::daw_UiClipChord);
        same!(UiClipExtent, sys::daw_UiClipExtent);
        same!(UiClipExtentRegion, sys::daw_UiClipExtentRegion);
        same!(UiClipRejectPayload, sys::daw_UiClipRejectPayload);
        same!(UiClipTrack, sys::daw_UiClipTrack);
        same!(UiClipWindowCommandPayload, sys::daw_UiClipWindowCommandPayload);
        same!(UiClipWindowSnapshot, sys::daw_UiClipWindowSnapshot);
        same!(UiCommandPayload, sys::daw_UiCommandPayload);
        same!(UiDeviceMeter, sys::daw_UiDeviceMeter);
        same!(UiDeviceMeterRegion, sys::daw_UiDeviceMeterRegion);
        same!(UiDiffPayload, sys::daw_UiDiffPayload);
        same!(UiHarmonyDiffPayload, sys::daw_UiHarmonyDiffPayload);
        same!(UiHarmonyEvent, sys::daw_UiHarmonyEvent);
        same!(UiHarmonySnapshot, sys::daw_UiHarmonySnapshot);
        same!(UiMarker, sys::daw_UiMarker);
        same!(UiMarkerCommandPayload, sys::daw_UiMarkerCommandPayload);
        same!(UiModLinkCommandPayload, sys::daw_UiModLinkCommandPayload);
        same!(UiModLinkUid16Payload, sys::daw_UiModLinkUid16Payload);
        same!(UiModSourceValuePayload, sys::daw_UiModSourceValuePayload);
        same!(UiPatcherGraphCommandPayload, sys::daw_UiPatcherGraphCommandPayload);
        same!(UiPatcherGraphDiffPayload, sys::daw_UiPatcherGraphDiffPayload);
        same!(UiPatcherGraphErrorPayload, sys::daw_UiPatcherGraphErrorPayload);
        same!(UiPatcherNodeConfigPayload, sys::daw_UiPatcherNodeConfigPayload);
        same!(UiPatcherPresetCommandPayload, sys::daw_UiPatcherPresetCommandPayload);
        same!(UiPresetSavedPayload, sys::daw_UiPresetSavedPayload);
        same!(UiSamplerEmitRowsPayload, sys::daw_UiSamplerEmitRowsPayload);
        same!(UiSamplerEnvPointsHeader, sys::daw_UiSamplerEnvPointsHeader);
        same!(UiSamplerKitRequestPayload, sys::daw_UiSamplerKitRequestPayload);
        same!(UiSamplerLoadPayload, sys::daw_UiSamplerLoadPayload);
        same!(UiSamplerMarkerPayload, sys::daw_UiSamplerMarkerPayload);
        same!(UiSamplerSetSlotPayload, sys::daw_UiSamplerSetSlotPayload);
        same!(UiSamplerSetDevicePayload, sys::daw_UiSamplerSetDevicePayload);
        same!(UiSamplerSlicePayload, sys::daw_UiSamplerSlicePayload);
        same!(UiSamplerSlotNameHeader, sys::daw_UiSamplerSlotNameHeader);
        same!(UiSamplerVintagePayload, sys::daw_UiSamplerVintagePayload);
        same!(UiSetClipGridPayload, sys::daw_UiSetClipGridPayload);
        same!(UiAudioClipFieldPayload, sys::daw_UiAudioClipFieldPayload);
        same!(UiSamplerEnvelopeRequestPayload, sys::daw_UiSamplerEnvelopeRequestPayload);
        same!(UiSamplerFilterPayload, sys::daw_UiSamplerFilterPayload);
        same!(UiSamplerRejectPayload, sys::daw_UiSamplerRejectPayload);
        same!(UiSetParamPayload, sys::daw_UiSetParamPayload);
        same!(UiTimeSigPoint, sys::daw_UiTimeSigPoint);
        same!(UiTrackRoutingPayload, sys::daw_UiTrackRoutingPayload);
        same!(UiWaveformRequestPayload, sys::daw_UiWaveformRequestPayload);
        // These six were pinned only by a size number typed into
        // `command_payload_sizes_match_the_engine` below. That assertion catches an edit to the
        // RUST struct and cannot see an edit to the C++ at all — backwards, since the header is
        // the authority and the side that drifts unseen. They were invisible to
        // tools/contract_layout_check.sh because its population regex allowed 200 characters
        // between `#[repr(C)]` and `pub struct`, and each of these carries a longer doc comment.
        same!(UiBulkChunkPayload, sys::daw_UiBulkChunkPayload);
        same!(UiClipTextHeader, sys::daw_UiClipTextHeader);
        same!(UiEnvPointWire, sys::daw_UiEnvPointWire);
        same!(UiSamplerEnvelopePayload, sys::daw_UiSamplerEnvelopePayload);
        same!(UiSamplerLfoPayload, sys::daw_UiSamplerLfoPayload);
        same!(UiSetRowOpsPayload, sys::daw_UiSetRowOpsPayload);
    }

    #[test]
    fn clip_window_command_payload_size() {
        assert_eq!(size_of::<UiClipWindowCommandPayload>(), 40);
        // The engine matches this payload on SIZE before it looks at
        // commandType, so a mirror that drifts is not rejected — it is read as
        // some other command's fields.
        assert_eq!(size_of::<UiTrackRoutingPayload>(), 40);
    }

    #[test]
    fn ui_clip_note_layout_matches_cpp() {
        // Widened for the authored EventId; the C++ side static_asserts the
        // same size, so a mismatch fails at compile time on one end and here
        // on the other.
        // v32: 40 -> 48 for the sampler's sound address (`sound` + `sound_offset`).
        // Append-only, so no existing offset moved — the assertions below are the
        // proof of that and must keep passing rather than being renumbered.
        const_assert_eq!(size_of::<UiClipNote>(), 48);
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

    /// The engine dispatches a chain command on the entry SIZE before it looks
    /// at commandType, so a payload that is not exactly 40 bytes is not refused
    /// — it is dropped while the ack still says ok. GUIDELINES 2.3.
    #[test]
    fn ui_chain_command_payload_layout_matches_cpp() {
        const_assert_eq!(size_of::<UiChainCommandPayload>(), 40);
        assert_eq!(offset_of!(UiChainCommandPayload, device_id), 12);
        assert_eq!(offset_of!(UiChainCommandPayload, device_kind), 16);
        assert_eq!(offset_of!(UiChainCommandPayload, insert_index), 20);
        assert_eq!(offset_of!(UiChainCommandPayload, host_slot_index), 28);
    }

    #[test]
    fn ui_edit_batch_entry_layout_matches_cpp() {
        const_assert_eq!(size_of::<EventEntry>(), 64);
        // Every payload that rides the 40-byte diff slot needs its size PINNED on both sides.
        // Neither of these had an assert, and the last time a shared constant went unasserted
        // (kUiMaxClipExtents) it diverged 256 vs 64 across the two languages with every test
        // green, because nothing was comparing them.
        const_assert_eq!(size_of::<UiClipRejectPayload>(), 40);
        const_assert_eq!(size_of::<UiPresetSavedPayload>(), 40);
        // M2.18: `ready` must sit in the old tail padding, or every ring offset moves.
        assert_eq!(offset_of!(EventEntry, ready), 60);
        const_assert_eq!(size_of::<UiEditBatchEntry>(), 2112);
        const_assert_eq!(align_of::<UiEditBatchEntry>(), 64);
        assert_eq!(offset_of!(UiEditBatchEntry, batch_id), 0);
        assert_eq!(offset_of!(UiEditBatchEntry, op_count), 4);
        assert_eq!(offset_of!(UiEditBatchEntry, ops), 64);
    }

    #[test]
    fn shm_header_layout_matches_cpp() {
        // v34: + uiTrackOpsWidth[64]. The header had ZERO tail padding, so one cache line of
        // per-track bytes grew it 6080 -> 6144 and bumped kShmVersion — region offsets are
        // computed from sizeof(ShmHeader), so every region shifted.
        // v37: + uiSamplerEnvelopeOffset/Bytes, appended at the END so no existing
        // field moved — only the total grew, which is what bumps kShmVersion.
        const_assert_eq!(size_of::<ShmHeader>(), 6208);
        const_assert_eq!(size_of::<UiCommandOutcomeEntry>(), 64);
        const_assert_eq!(align_of::<UiCommandOutcomeEntry>(), 64);
        const_assert_eq!(offset_of!(UiCommandOutcomeEntry, sequence), 0);
        const_assert_eq!(offset_of!(UiCommandOutcomeEntry, command_id), 8);
        const_assert_eq!(offset_of!(UiCommandOutcomeEntry, metadata0), 16);
        const_assert_eq!(offset_of!(UiCommandOutcomeEntry, metadata1), 24);
        const_assert_eq!(offset_of!(UiCommandOutcomeEntry, metadata2), 32);
        const_assert_eq!(offset_of!(UiCommandOutcomeEntry, reserved), 40);
        const_assert_eq!(
            size_of::<UiCommandOutcomeRegion>(),
            64 + K_UI_COMMAND_OUTCOME_CAPACITY * 64
        );
        const_assert_eq!(align_of::<UiCommandOutcomeRegion>(), 64);
        const_assert_eq!(offset_of!(UiCommandOutcomeRegion, published_sequence), 0);
        const_assert_eq!(offset_of!(UiCommandOutcomeRegion, next_command_id), 8);
        const_assert_eq!(offset_of!(UiCommandOutcomeRegion, status), 16);
        const_assert_eq!(offset_of!(UiCommandOutcomeRegion, capacity), 24);
        const_assert_eq!(offset_of!(UiCommandOutcomeRegion, reserved0), 28);
        const_assert_eq!(offset_of!(UiCommandOutcomeRegion, reserved), 32);
        const_assert_eq!(offset_of!(UiCommandOutcomeRegion, entries), 64);
        const_assert_eq!(size_of::<UiDeviceMeter>(), 12);
        const_assert_eq!(size_of::<UiDeviceMeterRegion>(), 12352);
        const_assert_eq!(align_of::<ShmHeader>(), 64);
        assert_eq!(offset_of!(ShmHeader, audio_in_offset), 40);
        assert_eq!(offset_of!(ShmHeader, audio_out_offset), 48);
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
        assert_eq!(offset_of!(ShmHeader, ui_track_quantize_grid), 4976); // v26
        assert_eq!(offset_of!(ShmHeader, ui_track_quantize_strength), 5488);
        assert_eq!(offset_of!(ShmHeader, ui_track_quantize_swing), 5744);
        assert_eq!(offset_of!(ShmHeader, ui_quantize_version), 6000);
        assert_eq!(offset_of!(ShmHeader, ui_arrange_offset), 6008); // v27
        assert_eq!(offset_of!(ShmHeader, ui_arrange_bytes), 6016);
        assert_eq!(offset_of!(ShmHeader, ui_automation_offset), 6024); // v28
        assert_eq!(offset_of!(ShmHeader, ui_automation_slot_offset), 6040);
        assert_eq!(offset_of!(ShmHeader, ui_sampler_kit_offset), 6064); // v32
        assert_eq!(offset_of!(ShmHeader, ui_sampler_envelope_offset), 6144); // v37
        assert_eq!(offset_of!(ShmHeader, ui_command_outcome_offset), 6160);
        assert_eq!(offset_of!(ShmHeader, ui_command_outcome_bytes), 6168);
        // The scale + device-param region structs (v16/v17) are now generated from
        // the C++ header; bindgen's own layout_tests pin them, so no hand offsets.
        const_assert_eq!(size_of::<UiPatcherNode>(), 40);
        const_assert_eq!(size_of::<UiPatcherEdge>(), 20);
        const_assert_eq!(size_of::<UiPatcherRegion>(), 5184);
        const_assert_eq!(size_of::<UiBusDiffPayload>(), 40); // v20, fits EventEntry
        // The engine dispatches SetTrackRouting BY PAYLOAD SIZE (daw_engine_main.cpp
        // checks `entry.size == sizeof(UiTrackRoutingPayload)`), so a mismatch here does
        // not fail to compile — it makes the command silently unrecognised.
        const_assert_eq!(size_of::<UiTrackRoutingPayload>(), 40);
        // Dispatched by payload SIZE too — a mismatch makes the command unrecognised
        // rather than failing to compile.
        const_assert_eq!(size_of::<UiModLinkCommandPayload>(), 40);
        const_assert_eq!(size_of::<UiModLinkUid16Payload>(), 40);
        const_assert_eq!(size_of::<UiModSourceValuePayload>(), 40);
        const_assert_eq!(size_of::<UiPatcherGraphCommandPayload>(), 40);
        const_assert_eq!(size_of::<UiPatcherNodeConfigPayload>(), 40);
        const_assert_eq!(size_of::<UiMarkerCommandPayload>(), 40);
        const_assert_eq!(size_of::<UiArrangeTimeCommandPayload>(), 40);
        const_assert_eq!(size_of::<UiAutomationPointPayload>(), 40);
        const_assert_eq!(size_of::<UiAutomationLaneRequestPayload>(), 40);
        const_assert_eq!(size_of::<UiMarker>(), 56);
        const_assert_eq!(size_of::<UiTimeSigPoint>(), 16);
        const_assert_eq!(size_of::<UiArrangeSummaryRegion>(), 4128);
        // The extents region had NO size assert, which is how its capacity constant
        // diverged from the C++ side unnoticed.
        const_assert_eq!(size_of::<UiClipExtentRegion>(), 16392);
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
        // v28 automation. Same discipline: the counts are tied to the generated region sizes.
        const_assert_eq!(size_of::<UiAutomationLane>(), 32);
        const_assert_eq!(size_of::<UiAutomationPointEntry>(), 16);
        const_assert_eq!(
            size_of::<UiAutomationLaneRegion>(),
            64 + K_UI_MAX_AUTOMATION_LANES * 32
        );
        const_assert_eq!(
            size_of::<UiAutomationSlot>(),
            64 + K_UI_MAX_AUTOMATION_POINTS * 16
        );
        const_assert_eq!(
            size_of::<UiAutomationSlotRegion>(),
            64 + K_UI_AUTOMATION_SLOTS * size_of::<UiAutomationSlot>()
        );
    }
}

// ---------------------------------------------------------------------------------------------
// THE WIRE-LAYOUT GUARD.
//
// Every struct in this file mirrors a C++ struct that the engine memcpy's out of a shared-memory
// ring. That correspondence rests entirely on `#[repr(C)]`, and LOSING IT IS SILENT: Rust is free
// to reorder the fields of a default-repr struct, so the code still compiles, the CLI still
// prints "sent", and the engine reads whatever landed at offset 0.
//
// That is not hypothetical. `UiChainCommandPayload` lost its `#[repr(C)]` to a careless insertion
// above it, and every add-device command arrived at the engine with commandType 0 — silently
// ignored, no error anywhere, and the first hypothesis was that add-device had been broken for
// some time. It had been broken for four minutes.
//
// So: assert the property the engine actually depends on. Not the size — a reordered struct can
// keep its size — but that `command_type` is the FIRST field, since that is the byte the engine
// dispatches on (`handleUiEntry` reads a UiCommandPayload header out of every entry regardless of
// which payload it really is).
#[cfg(test)]
mod wire_layout {
    use super::*;

    macro_rules! command_type_first {
        ($($t:ty),+ $(,)?) => {
            $(
                assert_eq!(
                    std::mem::offset_of!($t, command_type), 0,
                    concat!(stringify!($t),
                            "::command_type is not at offset 0 — the struct has lost #[repr(C)] \
                             and Rust has reordered it. The engine dispatches on the first two \
                             bytes of every command payload, so this one will arrive as \
                             commandType 0 and be silently ignored.")
                );
            )+
        };
    }

    #[test]
    fn command_type_is_always_the_first_field() {
        command_type_first!(
            UiCommandPayload,
            UiChainCommandPayload,
            UiChordCommandPayload,
            UiClipWindowCommandPayload,
            UiPatcherGraphCommandPayload,
            UiPatcherNodeConfigPayload,
            UiPatcherPresetCommandPayload,
            UiSetParamPayload,
            UiWaveformRequestPayload,
            UiTrackRoutingPayload,
            UiModLinkCommandPayload,
            UiModLinkUid16Payload,
            UiModSourceValuePayload,
            UiAutomationPointPayload,
            UiAutomationLaneRequestPayload,
            UiMarkerCommandPayload,
            UiArrangeTimeCommandPayload,
            UiSamplerLoadPayload,
            UiSamplerSetSlotPayload,
            UiSamplerKitRequestPayload,
            UiSamplerSlicePayload,
            UiSamplerMarkerPayload,
            UiSamplerEmitRowsPayload,
            UiSetRowOpsPayload,
            UiSamplerEnvelopePayload,
            UiBulkChunkPayload,
            UiSamplerLfoPayload,
        );
    }

    // The engine dispatches by SIZE as well as by commandType, so a payload whose size drifts
    // from its C++ twin is never dispatched at all. These numbers are the C++ static_asserts.
    #[test]
    fn command_payload_sizes_match_the_engine() {
        assert_eq!(std::mem::size_of::<UiCommandPayload>(), 40);
        assert_eq!(std::mem::size_of::<UiChainCommandPayload>(), 40);
        assert_eq!(std::mem::size_of::<UiSamplerLoadPayload>(), 40);
        assert_eq!(std::mem::size_of::<UiSamplerSetSlotPayload>(), 40);
        assert_eq!(std::mem::size_of::<UiSamplerKitRequestPayload>(), 40);
        assert_eq!(std::mem::size_of::<UiSamplerSlicePayload>(), 40);
        assert_eq!(std::mem::size_of::<UiSamplerMarkerPayload>(), 40);
        assert_eq!(std::mem::size_of::<UiSamplerEmitRowsPayload>(), 40);
        assert_eq!(std::mem::size_of::<UiSetRowOpsPayload>(), 40);
        assert_eq!(std::mem::size_of::<UiSamplerEnvelopePayload>(), 40);
        assert_eq!(std::mem::size_of::<UiBulkChunkPayload>(), 40);
        assert_eq!(std::mem::size_of::<UiSamplerLfoPayload>(), 40);
        assert_eq!(std::mem::size_of::<UiSamplerFilterPayload>(), 40);
        assert_eq!(std::mem::size_of::<UiSamplerRejectPayload>(), 40);
        // Not a ring payload — the ASSEMBLED shapes, which the engine memcpys.
        assert_eq!(std::mem::size_of::<UiSamplerEnvPointsHeader>(), 32);
        assert_eq!(std::mem::size_of::<UiSamplerSlotNameHeader>(), 12);
        assert_eq!(std::mem::size_of::<UiClipTextHeader>(), 20);
        assert_eq!(std::mem::size_of::<UiSamplerVintagePayload>(), 40);
        assert_eq!(std::mem::size_of::<UiSetClipGridPayload>(), 40);
        assert_eq!(std::mem::size_of::<UiAudioClipFieldPayload>(), 40);
        assert_eq!(std::mem::size_of::<UiSamplerEnvelopeRequestPayload>(), 40);
        assert_eq!(std::mem::size_of::<UiEnvPointWire>(), 8);
        // v35: + sliceBeginFrame / sliceEndFrame. Only three bytes were spare and two frame
        // counts need eight, so the entry's STRIDE changed — which is why this needed a version
        // bump and a parallel array would not have.
        assert_eq!(std::mem::size_of::<UiSamplerSlotEntry>(), 80);
    }
}

/// A PATCHER NODE'S CONFIG, PACKED INTO THE 16 BYTES THE ENGINE EXPECTS.
///
/// The read side hands back eight i32 per node; the write side is an explicit little-endian
/// layout that DIFFERS per node type and is deliberately not a C++ struct memcpy — that coupled
/// the wire to padding and truncated Euclidean. So a caller sends the same eight values it read,
/// and the packing happens once.
///
///   Euclidean(1)    steps u16@0, hits u16@2, offset u16@4, degree u8@6,
///                   octaveOffset i8@7, velocity u8@8, baseOctave u8@9,
///                   pad u16@10, durationTicks u32@12
///   Lfo(4)          freqMilliHz i32@0, depthMilli i32@4, biasMilli i32@8, phaseMilli i32@12
///   RandomDegree(5) degree u8@0, velocity u8@1, pad u16@2, durationTicks u32@4
///   SliceSelect(7)  base u16@0, count u16@2
///
/// ── WHY IT MOVED HERE ───────────────────────────────────────────────────────────────────────
///
/// There were two, and they did not agree. The sidecar CLAMPED every field; daw-cli cast straight
/// into the width, so `--steps 70000` wrapped to 4464 while the same request through the browser
/// arrived as 65535. Same op, same engine, two answers — and neither surface could tell you which
/// one it had made, because a wrapped value is a perfectly ordinary number on arrival.
///
/// CLAMPING, not refusing, because the read-back is what a caller sends back: a value the engine
/// itself published must survive a round trip, and clamping is what the sidecar has always done.
///
/// The eight values are POSITIONAL, in the order the published config reports them — the same
/// order `CONFIG_FIELDS` names in ui-web/src/patchermodel.js. Positional because that is what the
/// read side gives; a struct here would need a name for every field of every type and would then
/// be a second place to keep those names right.
pub fn pack_patcher_node_config(node_type: u32, c: &[i64; 8]) -> Result<[u8; 16], &'static str> {
    let mut cfg = [0u8; 16];
    match node_type {
        PATCHER_NODE_EUCLIDEAN => {
            cfg[0..2].copy_from_slice(&(c[0].clamp(0, 65535) as u16).to_le_bytes());
            cfg[2..4].copy_from_slice(&(c[1].clamp(0, 65535) as u16).to_le_bytes());
            cfg[4..6].copy_from_slice(&(c[2].clamp(0, 65535) as u16).to_le_bytes());
            cfg[6] = c[3].clamp(0, 255) as u8;
            cfg[7] = (c[4].clamp(-128, 127) as i8) as u8;
            cfg[8] = c[5].clamp(0, 255) as u8;
            cfg[9] = c[6].clamp(0, 255) as u8;
            cfg[12..16].copy_from_slice(&(c[7].clamp(0, u32::MAX as i64) as u32).to_le_bytes());
        }
        PATCHER_NODE_LFO => {
            for i in 0..4 {
                let v = c[i].clamp(i32::MIN as i64, i32::MAX as i64) as i32;
                cfg[i * 4..i * 4 + 4].copy_from_slice(&v.to_le_bytes());
            }
        }
        PATCHER_NODE_RANDOM_DEGREE => {
            cfg[0] = c[0].clamp(0, 255) as u8;
            cfg[1] = c[1].clamp(0, 255) as u8;
            cfg[4..8].copy_from_slice(&(c[2].clamp(0, u32::MAX as i64) as u32).to_le_bytes());
        }
        // Two SLOT ADDRESSES, packed as they are and not scaled. A count of 0 would be an empty
        // range for a node whose whole job is to pick from one, so the low bound is 1; a base of
        // 0 is legal and is the sampler's own "let the keymap pick from the pitch" sentinel,
        // which is a setting rather than an unset value.
        PATCHER_NODE_SLICE_SELECT => {
            cfg[0..2].copy_from_slice(&(c[0].clamp(0, 65535) as u16).to_le_bytes());
            cfg[2..4].copy_from_slice(&(c[1].clamp(1, 65535) as u16).to_le_bytes());
        }
        // A type with no layout is REFUSED rather than sent as zeros, which the engine would
        // apply — silently reconfiguring a node to nothing.
        _ => return Err("no config layout for that node type"),
    }
    Ok(cfg)
}

/// The engine's own defaults for each configurable node type, in the same positional order
/// `pack_patcher_node_config` takes.
///
/// These are not invented: they are the member initialisers of `PatcherEuclideanConfig` and
/// friends in apps/patcher_abi.h, and the values the Rust kernel falls back to for a node whose
/// config block is null. `AddPatcherNode` mints a node with NO config struct, so a caller that
/// wants to change ONE field has to send all eight — and without these it would have to invent
/// the other seven, which is how a node acquires settings nobody chose.
pub fn default_patcher_node_config(node_type: u32) -> Option<[i64; 8]> {
    match node_type {
        // steps, hits, offset, degree, octaveOffset, velocity, baseOctave, durationTicks.
        // DEGREE 1, not 0: degrees are 1-based throughout this program, and 1 is what the kernel
        // uses for a euclidean with no config.
        PATCHER_NODE_EUCLIDEAN => Some([16, 5, 0, 1, 0, 100, 4, 0]),
        // Milli-units: 1.0 Hz, depth 1.0, bias 0, phase 0.
        PATCHER_NODE_LFO => Some([1000, 1000, 0, 0, 0, 0, 0, 0]),
        PATCHER_NODE_RANDOM_DEGREE => Some([8, 100, 0, 0, 0, 0, 0, 0]),
        PATCHER_NODE_SLICE_SELECT => Some([1, 8, 0, 0, 0, 0, 0, 0]),
        _ => None,
    }
}

#[cfg(test)]
mod patcher_config_tests {
    use super::*;

    #[test]
    fn euclidean_lands_on_the_documented_offsets() {
        let cfg = pack_patcher_node_config(PATCHER_NODE_EUCLIDEAN,
                                           &[16, 5, 3, 2, -1, 100, 4, 480000]).unwrap();
        assert_eq!(u16::from_le_bytes([cfg[0], cfg[1]]), 16, "steps");
        assert_eq!(u16::from_le_bytes([cfg[2], cfg[3]]), 5, "hits");
        assert_eq!(u16::from_le_bytes([cfg[4], cfg[5]]), 3, "offset");
        assert_eq!(cfg[6], 2, "degree");
        assert_eq!(cfg[7] as i8, -1, "octaveOffset is SIGNED");
        assert_eq!(cfg[8], 100, "velocity");
        assert_eq!(cfg[9], 4, "baseOctave");
        assert_eq!(u32::from_le_bytes([cfg[12], cfg[13], cfg[14], cfg[15]]), 480000, "duration");
    }

    #[test]
    fn out_of_range_values_clamp_rather_than_wrap() {
        // THE DIVERGENCE THIS FUNCTION EXISTS TO END. daw-cli cast straight into the width, so
        // 70000 steps arrived as 4464 — a plausible number for a node to be set to, and no way
        // to tell it from one somebody chose.
        let cfg = pack_patcher_node_config(PATCHER_NODE_EUCLIDEAN,
                                           &[70000, 5, 0, 1, 0, 100, 4, 0]).unwrap();
        assert_eq!(u16::from_le_bytes([cfg[0], cfg[1]]), 65535, "steps must clamp, not wrap");
        assert_ne!(u16::from_le_bytes([cfg[0], cfg[1]]), 70000u32 as u16);
    }

    #[test]
    fn slice_select_will_not_be_given_an_empty_range() {
        let cfg = pack_patcher_node_config(PATCHER_NODE_SLICE_SELECT, &[0, 0, 0, 0, 0, 0, 0, 0])
            .unwrap();
        assert_eq!(u16::from_le_bytes([cfg[0], cfg[1]]), 0, "base 0 is the keymap sentinel");
        assert_eq!(u16::from_le_bytes([cfg[2], cfg[3]]), 1, "count clamps up to 1");
    }

    #[test]
    fn a_type_with_no_layout_is_refused_rather_than_zeroed() {
        assert!(pack_patcher_node_config(PATCHER_NODE_PASSTHROUGH, &[0; 8]).is_err());
        assert!(default_patcher_node_config(PATCHER_NODE_PASSTHROUGH).is_none());
    }

    #[test]
    fn the_defaults_pack() {
        for t in [PATCHER_NODE_EUCLIDEAN, PATCHER_NODE_LFO, PATCHER_NODE_RANDOM_DEGREE,
                  PATCHER_NODE_SLICE_SELECT] {
            let d = default_patcher_node_config(t).expect("a default");
            pack_patcher_node_config(t, &d).expect("packs");
        }
    }
}

/// One note a transpose intends to rewrite.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TransposedNote {
    pub tick: u64,
    pub duration: u64,
    pub pitch: u8,
    pub velocity: u8,
    pub column: u8,
}

/// What a transpose would do: the notes it moves, and how many it declined to.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct TransposePlan {
    pub moved: Vec<TransposedNote>,
    /// In the range, but the new pitch would leave 0..=127.
    pub skipped: usize,
}

/// WHICH NOTES A TRANSPOSE TOUCHES, AND WHAT HAPPENS AT THE EDGES.
///
/// The rule the web UI already implements in `transposeSelection`, moved somewhere the CLI and the
/// agent can share it — because the interesting part is not the addition, it is the two decisions
/// either side of it, and those are what two independent implementations get differently.
///
/// SKIP, NEVER CLAMP. A note transposed past 127 is not a note at 127; clamping would silently
/// change the music and leave nothing to notice. The UI's own comment on that line says
/// "silently clamping would lose the note", and this keeps that promise on every surface.
///
/// HALF-OPEN ON THE RIGHT — `[from, to)`. A note starting exactly at `to` belongs to the next bar,
/// the next selection, the next anything; including it makes two adjacent ranges overlap by one
/// note and transpose it twice. The caller that wants "everything from here on" passes u64::MAX.
///
/// The note's COLUMN and DURATION ride along untouched. A transpose is a rewrite of the same cell
/// with a different pitch, so dropping either would move the note as well as retune it — which is
/// the defect that made ten operations in the web UI edit the wrong column.
pub fn plan_transpose(
    notes: &[UiClipNote],
    from: u64,
    to: u64,
    semitones: i32,
) -> TransposePlan {
    let mut plan = TransposePlan::default();
    for n in notes {
        if n.t_on < from || n.t_on >= to {
            continue;
        }
        let want = i32::from(n.pitch) + semitones;
        if !(0..=127).contains(&want) {
            plan.skipped += 1;
            continue;
        }
        plan.moved.push(TransposedNote {
            tick: n.t_on,
            // A zero duration on the wire means "until the next event", which is NOT what this
            // note is doing — it has a measured length and must keep it.
            duration: n.t_off.saturating_sub(n.t_on).max(1),
            pitch: want as u8,
            velocity: n.velocity,
            column: n.column,
        });
    }
    plan
}

/// ONE EDIT PER CLIP NOTE, not one per appearance.
///
/// `read_track_clip` publishes a FLATTENED track: a clip placed three times contributes its notes
/// three times, at three different track ticks. Transposing that list verbatim writes to the same
/// clip once per appearance, and the result is not what the range describes — measured on a
/// fixture with one clip placed three times, two notes became three.
///
/// The fix is not to refuse. Editing a shared clip is SUPPOSED to change every appearance — that
/// is what sharing means, and `shared_clips` exists to tell you so before you do it. What must not
/// happen is the same clip note being written several times over.
///
/// So each note is resolved to the clip cell it actually occupies — (clip id, tick WITHIN the
/// clip, column) — and only the FIRST appearance of each cell is kept. The rest are the same music
/// seen again.
///
/// A note that falls in no published extent is kept as-is: better to transpose something the
/// extents could not explain than to drop it silently, and the extent table truncates at 256.
pub fn dedupe_by_clip_cell(
    notes: &[UiClipNote],
    extents: &[UiClipExtent],
    track: u32,
) -> Vec<UiClipNote> {
    use std::collections::HashSet;
    let mut seen: HashSet<(u32, u64, u8)> = HashSet::new();
    let mut out = Vec::with_capacity(notes.len());
    for n in notes {
        // The placement covering this tick, on this track. Extents are half-open: a note at
        // exactly `end_tick` belongs to whatever comes next, the same rule plan_transpose uses.
        let cell = extents.iter()
            .find(|e| e.track_id == track && n.t_on >= e.start_tick && n.t_on < e.end_tick)
            .map(|e| {
                let span = e.end_tick.saturating_sub(e.start_tick).max(1);
                (e.clip_id, (n.t_on - e.start_tick) % span, n.column)
            });
        match cell {
            Some(key) => { if seen.insert(key) { out.push(*n); } }
            None => out.push(*n),
        }
    }
    out
}

#[cfg(test)]
mod transpose_tests {
    use super::*;

    fn note(t_on: u64, t_off: u64, pitch: u8, column: u8) -> UiClipNote {
        UiClipNote { t_on, t_off, pitch, velocity: 100, column, ..Default::default() }
    }

    #[test]
    fn it_moves_what_is_in_range_and_keeps_column_and_duration() {
        let notes = [note(0, 480, 60, 0), note(960, 1440, 67, 1)];
        let p = plan_transpose(&notes, 0, u64::MAX, 2);
        assert_eq!(p.skipped, 0);
        assert_eq!(p.moved.len(), 2);
        assert_eq!(p.moved[0], TransposedNote { tick: 0, duration: 480, pitch: 62,
                                                velocity: 100, column: 0 });
        assert_eq!(p.moved[1].column, 1, "the column must ride along");
        assert_eq!(p.moved[1].duration, 480, "and so must the length");
    }

    #[test]
    fn the_range_is_half_open_so_adjacent_ranges_do_not_share_a_note() {
        let notes = [note(0, 480, 60, 0), note(960, 1440, 62, 0)];
        assert_eq!(plan_transpose(&notes, 0, 960, 1).moved.len(), 1, "960 belongs to the next range");
        assert_eq!(plan_transpose(&notes, 960, 1920, 1).moved.len(), 1);
    }

    #[test]
    fn out_of_midi_range_is_skipped_not_clamped() {
        let notes = [note(0, 480, 126, 0), note(960, 1440, 60, 0)];
        let p = plan_transpose(&notes, 0, u64::MAX, 4);
        assert_eq!(p.skipped, 1, "126 + 4 leaves MIDI range");
        assert_eq!(p.moved.len(), 1);
        assert_eq!(p.moved[0].pitch, 64);
        // The one that would clamp must NOT appear at 127 — that is a different note.
        assert!(p.moved.iter().all(|m| m.pitch != 127));
    }

    #[test]
    fn a_zero_length_note_still_gets_a_length() {
        // 0 on the wire means "until the next event". A note that already has an end must not be
        // re-described as open-ended by a transpose.
        let p = plan_transpose(&[note(0, 0, 60, 0)], 0, u64::MAX, 1);
        assert_eq!(p.moved[0].duration, 1);
    }

    #[test]
    fn nothing_in_range_is_an_empty_plan_rather_than_a_panic() {
        let p = plan_transpose(&[note(0, 480, 60, 0)], 5000, 6000, 1);
        assert!(p.moved.is_empty() && p.skipped == 0);
    }
}

#[cfg(test)]
mod clip_cell_tests {
    use super::*;

    fn note(t_on: u64, pitch: u8, column: u8) -> UiClipNote {
        UiClipNote { t_on, t_off: t_on + 100, pitch, velocity: 100, column, ..Default::default() }
    }
    fn ext(placement_id: u32, clip_id: u32, start: u64, end: u64) -> UiClipExtent {
        UiClipExtent { placement_id, clip_id, track_id: 0, flags: 0,
                       start_tick: start, end_tick: end, name: [0; 32] }
    }

    #[test]
    fn one_clip_placed_twice_yields_one_edit_per_note() {
        // Clip 1 at 0..1000 and again at 2000..3000. Its note sits at clip-tick 0 both times.
        let extents = [ext(1, 1, 0, 1000), ext(2, 1, 2000, 3000)];
        let notes = [note(0, 60, 0), note(2000, 60, 0)];
        let kept = dedupe_by_clip_cell(&notes, &extents, 0);
        assert_eq!(kept.len(), 1, "the same clip cell seen twice is ONE edit: {kept:?}");
        assert_eq!(kept[0].t_on, 0, "and it is the first appearance");
    }

    #[test]
    fn different_clips_are_not_merged() {
        let extents = [ext(1, 1, 0, 1000), ext(2, 2, 2000, 3000)];
        let notes = [note(0, 60, 0), note(2000, 60, 0)];
        assert_eq!(dedupe_by_clip_cell(&notes, &extents, 0).len(), 2,
                   "two DIFFERENT clips are two edits, even at the same clip-relative tick");
    }

    #[test]
    fn the_same_tick_in_different_columns_is_two_notes() {
        let extents = [ext(1, 1, 0, 1000)];
        let notes = [note(0, 60, 0), note(0, 67, 1)];
        assert_eq!(dedupe_by_clip_cell(&notes, &extents, 0).len(), 2,
                   "column is part of the cell — collapsing these would silently drop a voice");
    }

    #[test]
    fn a_note_outside_every_extent_is_kept() {
        // The extent table truncates at 256. Dropping what it cannot explain would silently skip
        // part of the edit; transposing it is the recoverable direction.
        let extents = [ext(1, 1, 0, 1000)];
        let notes = [note(5000, 60, 0)];
        assert_eq!(dedupe_by_clip_cell(&notes, &extents, 0).len(), 1);
    }

    #[test]
    fn another_track_s_extents_do_not_claim_these_notes() {
        let mut e = ext(1, 1, 0, 1000);
        e.track_id = 3;
        let notes = [note(0, 60, 0), note(0, 60, 0)];
        // No extent for track 0, so both are kept rather than merged against track 3's clip.
        assert_eq!(dedupe_by_clip_cell(&notes, &[e], 0).len(), 2);
    }
}
