//! Attaching to a running engine's shared memory to query it, and to send it
//! the same `UiCommand` payloads the UI sends.
//!
//! MULTI-PRODUCER since M2.18. The engine is the only consumer, but any number of
//! processes may produce: `write_entry` CAS-reserves a slot on `write_index`, fills
//! it, and only then publishes it by storing `ready` — see the comment at that call
//! site, which is the authority for the protocol. Reading is always safe, and
//! writing needs no external serialisation.
//!
//! THIS PARAGRAPH SAID THE OPPOSITE UNTIL 2026-08-14, and it mattered. It read
//! "the UI command ring is SPSC … exactly one process may be the producer", which
//! was true before M2.18 and describes the world in which `daw-cli do` needed
//! `--force`. The implementation below has carried a comment saying MULTI-PRODUCER
//! since that change landed, so this file contradicted itself across 1,900 lines,
//! with the false half at the top where a reader starts.
//!
//! It was not harmless. The P2-CMD-00 design reasoned from this paragraph about
//! whether a command identity needs to be unique across concurrent producers, and
//! flagged it as a documentation defect it could not fix while read-only. A stale
//! rule stated where a reader begins outranks a correct one stated where the code
//! is.

use std::ffi::CString;
use std::fs::File;
use std::os::fd::FromRawFd;
use std::sync::atomic::Ordering;

use memmap2::{Mmap, MmapMut, MmapOptions};

use std::sync::atomic::fence;
use std::sync::atomic::AtomicU32;

use crate::layout::{
    EventEntry, EventType, RingHeader, ShmHeader, UiChainCommandPayload, UiChordCommandPayload,
    UiClipExtent, UiClipExtentRegion, UiClipWindowCommandPayload, UiClipWindowSnapshot,
    UiCommandPayload, UiHarmonyEvent, UiHarmonySnapshot, UiPatcherEdge, UiPatcherNode,
    UiDeviceParamsRegion, UiPatcherGraphCommandPayload, UiPatcherNodeConfigPayload,
    UiPatcherRegion, UiScaleRegion, K_SHM_MAGIC, K_SHM_VERSION, K_UI_MAX_CLIP_EXTENTS,
    K_UI_MAX_DEVICE_PARAMS, K_UI_MAX_HARMONY_EVENTS,
    K_UI_MAX_PATCHER_EDGES, K_UI_MAX_PATCHER_NODES, K_UI_MAX_SCALES, K_UI_MAX_SCALE_STEPS,
    K_UI_MAX_TRACKS, UiAudioSourceRegion, UiWaveformRegion, UiWaveformRequestPayload,
    K_UI_MAX_AUDIO_CLIPS, K_UI_MAX_AUDIO_SOURCES, K_UI_WAVEFORM_MAX_PAIRS, K_UI_WAVEFORM_SLOTS,
    UiAutomationLaneRegion, UiAutomationLaneRequestPayload, UiAutomationSlotRegion,
    K_UI_AUTOMATION_SLOTS, K_UI_MAX_AUTOMATION_LANES, K_UI_MAX_AUTOMATION_POINTS,
    UI_AUTOMATION_FLAG_DISCRETE,
};
use crate::reader::{SeqlockReader, UiSnapshot};

/// Per-track mixer read-back. Gain in millibels, pan in thousandths (integers —
/// the header carries no float mixer fields), mute/solo in `flags`
/// (`layout::MIXER_FLAG_*`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TrackMixer {
    pub gain_millibels: i32,
    pub pan_thousandths: i32,
    pub flags: u8,
    /// v34: the widest op run on any note in the track — how many glyphs the collapsed ops cell
    /// must draw. 0 = the track uses no ops, so the column need not be drawn at all.
    pub ops_width: u8,
}

/// The published patcher graph: nodes + edges, plus the version to cache-key on
/// and the device it's parked on.
#[derive(Clone, Debug, Default)]
pub struct PatcherView {
    pub version: u32,
    pub device_id: u32,
    pub nodes: Vec<UiPatcherNode>,
    pub edges: Vec<UiPatcherEdge>,
}

/// One scale from the engine's registry, cents resolved from the published
/// milli-cents. `step_cents` has `step_count` entries; the tuning card draws them.
#[derive(Clone, Debug, Default)]
pub struct ScaleView {
    pub id: u32,
    pub name: String,
    pub octave_cents: f64,
    pub step_cents: Vec<f64>,
}

/// One parameter of a device (v17). `uid16` is the durable id to key a mapping on.
#[derive(Clone, Debug, Default)]
pub struct DeviceParamView {
    pub index: u32,
    pub value: f32, // 0..1
    pub uid16: [u8; 16],
    pub name: String,
    pub display: String,
    /// v30: WHAT THE PARAMETER IS, not just where it is. Without these a caller can read
    /// "Cutoff is 0.62, displays 440 Hz" and cannot know what 0.0 and 1.0 mean, whether it is a
    /// switch, or what to reset it to — so setting a value in real units is a binary search
    /// against `display`, which is a guessing loop rather than an interface.
    pub unit: String,
    /// The endpoints AS THE PLUGIN RENDERS THEM. For a VST3 through JUCE the normalisable range
    /// is usually 0..1, so `min`/`max` below say nothing and the real range exists only as text.
    pub min_text: String,
    pub max_text: String,
    pub default_value: f32,
    pub min: f32,
    pub max: f32,
    /// 0 = continuous; else the number of switch positions.
    pub step_count: u32,
    pub discrete: bool,
    /// False means the plugin will IGNORE an automation lane pointed at this, so drawing one
    /// would be a lie.
    pub automatable: bool,
}

/// A device's published parameters, from the last RequestDeviceParams.
#[derive(Clone, Debug, Default)]
pub struct DeviceParamsView {
    pub version: u32,
    pub track_id: u32,
    pub device_id: u32,
    pub device_name: String,
    pub params: Vec<DeviceParamView>,
}

/// One decoded audio source descriptor (UiAudioSource). `content_key` is rejoined
/// from the split lo/hi words. `status`: 0 absent, 1 ready, 2 failed.
#[derive(Debug, Clone, Default)]
pub struct AudioSourceView {
    pub source_id: u32,
    pub content_key: u64,
    pub source_channels: u32,
    pub wave_channels: u32,
    pub status: u32,
    pub source_frames: u64,
    pub source_rate_hz: f64,
    pub abs_peak: f32,
    pub level_mask: u32,
    pub path: String,
    pub flags: u32, // bit0 >2 channels truncated, bit1 |x|>1 seen
}

/// One audio clip descriptor (UiAudioClip) — joins UiClipExtent.clipId to a source.
#[derive(Debug, Clone, Default)]
pub struct AudioClipView {
    pub clip_id: u32,
    pub source_id: u32,
    pub source_start_frame: u64,
    pub clip_length_ticks: u64,
    pub fade_in_ticks: u32,
    pub fade_out_ticks: u32,
    pub gain_db: f32,
    pub flags: u32,
}

/// The audio source + clip descriptor tables (UiAudioSourceRegion), version-gated.
#[derive(Debug, Clone, Default)]
pub struct AudioSourcesView {
    pub version: u32,
    pub audio_map_bpm_milli: u32,
    pub format_version: u32,
    pub sources: Vec<AudioSourceView>,
    pub clips: Vec<AudioClipView>,
}

/// A windowed waveform answer read out of one UiWaveformSlot under its seqlock.
/// v32: one answered sampler kit. `slots` is already truncated to `slot_count`, and
/// `slots_truncated` says how many the region could not carry — never a silent short list.
#[derive(Clone, Debug)]
pub struct SamplerKitView {
    pub request_seq: u32,
    pub track_id: u32,
    pub device_id: u32,
    pub found: bool,
    pub voice_cap: u32,
    /// The bank's own defaults. `default_gate` is what a NEWLY MINTED slot gets — it seeds and
    /// then stops mattering, so it cannot be inferred from the slots (a bank legitimately mixes
    /// one-shot and gated ones).
    pub default_gate: u32,
    pub default_view: u32,
    pub active_voices: u32,
    /// Telemetry. A voice pool running out is a musical fact, not a secret.
    pub steals: u32,
    /// Notes that hit no slot — a kit that is silent everywhere is diagnosable from this.
    pub unmapped: u32,
    /// The version of the state THIS ANSWER was built from, stamped inside the seqlock.
    ///
    /// Not the same fact as `sampler_kit_version()`, which is the model's poll counter written
    /// on a different clock: an answer can arrive carrying older content than that counter
    /// reports. Compare the two to tell "you are looking at the current kit" from "the kit has
    /// moved since this was built".
    pub content_version: u32,
    pub slots_truncated: u32,
    pub slots: Vec<crate::layout::UiSamplerSlotEntry>,
}


/// `pairs` is channel-planar then column then (min,max): for channel c column i,
/// pairs[(c*columns + i)*2] and +1. `status`: 0 ok, 1 truncated, 2 notready, 3 bad.
#[derive(Debug, Clone, Default)]
pub struct WaveformSlotView {
    pub request_seq: u32,
    pub source_id: u32,
    pub content_key: u64,
    pub decimation: u32,
    pub columns: u32,
    pub channels: u32,
    pub first_frame: u64,
    pub frame_count: u64,
    pub status: u32,
    pub flags: u32,
    /// `(trackId << 16) | deviceId`, valid ONLY when `flags` has
    /// `UI_WAVEFORM_FLAG_SAMPLER_SOURCE`. Needed because `source_id` on a sampler answer is a
    /// PER-DEVICE local id, so it does not identify the source on its own — two samplers both
    /// have a local id 1, and a reader keying on source_id alone draws one pad's audio on the
    /// other's canvas.
    pub sampler_addr: u32,
    pub pairs: Vec<i16>,
}

/// One entry of the standing automation lane list (UiAutomationLaneRegion). This answers "which
/// params are automated on this track" without asking for anything — the list is version-gated,
/// so a UI can draw the lane headers from a cheap read and only request POINTS for lanes it opens.
#[derive(Debug, Clone, Default)]
pub struct AutomationLaneView {
    pub track_id: u32,
    /// `PARAM_TARGET_ALL` (0xFFFF_FFFF) = every plugin on the track.
    pub target_plugin_index: u32,
    pub param_id: String,
    pub point_count: u32,
    pub discrete: bool,
}

/// The whole lane list plus its version. `version` moves when ANY automation changes and is NOT
/// the clip version, so typing a note does not invalidate a cached lane list. `truncated` non-zero
/// means lanes did not fit: the list is INCOMPLETE, and a caller that ignores this draws a
/// confident picture with lanes missing from it.
#[derive(Debug, Clone, Default)]
pub struct AutomationLanesView {
    pub version: u32,
    pub truncated: u32,
    pub lanes: Vec<AutomationLaneView>,
}

/// One answered lane, read out of a UiAutomationSlot under its seqlock. The echoed
/// `request_seq`/`track_id`/`param_id` are the point: a slot is reused mod the slot count, so
/// without them an answer to somebody else's question looks like an answer to yours. `found ==
/// false` is an ANSWER — "nothing automates that param" — not silence.
#[derive(Debug, Clone, Default)]
pub struct AutomationLaneAnswer {
    pub request_seq: u32,
    pub track_id: u32,
    pub param_id: String,
    pub found: bool,
    pub discrete: bool,
    pub points_truncated: u32,
    /// (nanotick, normalised 0..1), in tick order. The RESOLVED value between points is
    /// deliberately absent: interpolation is a picture, and a second implementation of it that
    /// can disagree with what plays is the exact class of bug this read-back exists to expose.
    pub points: Vec<(u64, f32)>,
}

/// One modulator's envelope shape, as the engine has it. EVERY FIELD THE WRITE TAKES IS HERE —
/// a read-back that returns a subset is how a pencil editor lies, because an editor that draws
/// the shape and sends it back would CLEAR whatever the answer omitted.
pub struct SamplerEnvelopeAnswer {
    pub request_seq: u32,
    pub track_id: u32,
    pub device_id: u32,
    pub mod_set_id: u32,
    pub modulator_id: u16,
    pub target: u8,
    pub found: bool,
    pub time_base: u8,
    pub rate_milli: u16,
    pub points_truncated: u32,
    /// 255 = no loop, matching kEnvLoopNone and the wire the write side uses.
    pub sustain_loop: (u8, u8),
    pub release_loop: (u8, u8),
    pub release_fade: u32,
    /// (time, valueMilli, tension, flags) in time order.
    pub points: Vec<(u32, i16, i8, u8)>,
}

/// Read a nul-terminated C `char` array (from a bindgen-generated struct, where
/// `char` is `i8`) into an owned String.
fn cchar_str(b: &[std::os::raw::c_char]) -> String {
    let end = b.iter().position(|&c| c == 0).unwrap_or(b.len());
    let bytes = unsafe { std::slice::from_raw_parts(b.as_ptr() as *const u8, end) };
    String::from_utf8_lossy(bytes).into_owned()
}

pub fn default_shm_name() -> String {
    for key in ["DAW_UI_SHM_NAME", "DAW_SHM_NAME"] {
        if let Ok(name) = std::env::var(key) {
            if !name.is_empty() {
                return if name.starts_with('/') {
                    name
                } else {
                    format!("/{name}")
                };
            }
        }
    }
    "/daw_engine_ui".to_string()
}

struct RingView {
    header: *mut RingHeader,
    entries: *mut EventEntry,
    mask: u32,
}

// A read-only attach maps with `map`; only a writable attach may use
// `map_mut`, which needs the descriptor opened O_RDWR.
enum Mapping {
    ReadOnly(Mmap),
    Writable(MmapMut),
}

impl Mapping {
    fn as_ptr(&self) -> *const u8 {
        match self {
            Mapping::ReadOnly(map) => map.as_ptr(),
            Mapping::Writable(map) => map.as_ptr(),
        }
    }
}

pub struct EngineHandle {
    _mmap: Mapping,
    header: *const ShmHeader,
    // AE-P1.3. The mapped length, kept so a region offset out of the header can be checked against
    // something. Without it the only available shape was passing the length as a parameter, which
    // works for the rings because they are built inside attach_inner and reaches none of the
    // fifteen region accessors that are not.
    size: usize,
    ring_ui: Option<RingView>,
    ring_ui_out: Option<RingView>,
}

impl EngineHandle {
    /// Maps the engine's UI shared memory. `writable` must be true to send
    /// commands; see the single-producer note above. Writes go to the UI command
    /// ring.
    pub fn attach(name: &str, writable: bool) -> Result<Self, String> {
        Self::attach_inner(name, writable, false)
    }

    /// Maps the engine as the in-app agent: writes go to the agent's OWN command
    /// ring, so the agent never contends with the UI for the single-producer UI
    /// ring. base_version optimistic concurrency arbitrates across the two rings.
    pub fn attach_agent(name: &str) -> Result<Self, String> {
        Self::attach_inner(name, true, true)
    }

    fn attach_inner(name: &str, writable: bool, agent_ring: bool) -> Result<Self, String> {
        let c_name = CString::new(name).map_err(|_| format!("invalid SHM name {name}"))?;
        let flags = if writable { libc::O_RDWR } else { libc::O_RDONLY };
        let fd = unsafe { libc::shm_open(c_name.as_ptr(), flags, 0) };
        if fd < 0 {
            return Err(format!(
                "cannot open {name}: {} (is the engine running?)",
                std::io::Error::last_os_error()
            ));
        }
        let file = unsafe { File::from_raw_fd(fd) };
        let size = file.metadata().map(|meta| meta.len()).unwrap_or(0);
        if (size as usize) < std::mem::size_of::<ShmHeader>() {
            return Err(format!("{name} is too small ({size} bytes)"));
        }
        let mmap = unsafe {
            if writable {
                Mapping::Writable(
                    MmapOptions::new()
                        .len(size as usize)
                        .map_mut(&file)
                        .map_err(|err| format!("cannot map {name} for writing: {err}"))?,
                )
            } else {
                Mapping::ReadOnly(
                    MmapOptions::new()
                        .len(size as usize)
                        .map(&file)
                        .map_err(|err| format!("cannot map {name}: {err}"))?,
                )
            }
        };
        let header = mmap.as_ptr() as *const ShmHeader;
        let magic = unsafe { std::ptr::read_volatile(&(*header).magic) };
        let version = unsafe { std::ptr::read_volatile(&(*header).version) };
        if magic != K_SHM_MAGIC || version != K_SHM_VERSION {
            return Err(format!(
                "shared memory header mismatch (magic 0x{magic:08x} want 0x{:08x}, \
                 version {version} want {}) - engine and CLI builds differ",
                K_SHM_MAGIC, K_SHM_VERSION
            ));
        }
        let ring_offset = unsafe {
            if agent_ring {
                (*header).ring_ui_agent_offset
            } else {
                (*header).ring_ui_offset
            }
        };
        let ring_ui = if writable {
            ring_view(mmap.as_ptr() as *mut u8, ring_offset, size as usize)
        } else {
            None
        };
        // THE OUTBOUND RING, mapped even on a read-only attach. It is the engine's diff channel
        // and this side only ever PEEKS it — see peek_ui_diffs for why it must never drain.
        let ring_ui_out = ring_view(
            mmap.as_ptr() as *mut u8,
            unsafe { (*header).ring_ui_out_offset },
            size as usize,
        );
        Ok(Self {
            _mmap: mmap,
            header,
            size: size as usize,
            ring_ui,
            ring_ui_out,
        })
    }

    /// A pointer to a `T` at `offset` bytes into the mapping, or None if the mapping does not
    /// admit one there.
    ///
    /// AE-P1.3. Fifteen region accessors read a `u64` offset out of the shared header and turned it
    /// into a `*const T` — several into a Rust REFERENCE, which is stricter still — guarded by
    /// `offset == 0` and nothing else. Every one of those offsets is a number another process
    /// wrote. The engine writes them correctly; a truncated segment from an engine that died
    /// mid-setup, or a stale one from a build whose magic and version happen to match, does not.
    ///
    /// ONE HELPER RATHER THAN THE CHECK WRITTEN FIFTEEN TIMES. The rule is identical at every site
    /// and this project has watched an identical rule reach seven hand-written copies under a
    /// comment claiming four. It also means the sites read as what they are — "give me this region
    /// if it is there" — instead of restating pointer arithmetic.
    ///
    /// The subtraction is deliberate: `offset + size_of::<T>() <= self.size` overflows and admits
    /// exactly the offset it exists to refuse. The `self.size < size_of::<T>()` test in front of it
    /// is what keeps the subtraction from underflowing.
    ///
    /// Alignment is checked because these become references to types with alignment requirements,
    /// and a misaligned reference is undefined behaviour before it is a bounds problem. The base is
    /// page-aligned (mmap), so an aligned offset gives an aligned address.
    /// A pointer to `count` consecutive `T` at `offset`, or None if the mapping does not admit
    /// them ALL.
    ///
    /// AE-P1.3. `region::<T>` proves ONE `T` fits, which is the wrong bound wherever the caller
    /// then indexes. The all-tracks clip region is published as
    /// `sizeof(UiClipWindowSnapshot) * kUiMaxTracks` and read with `base.add(track_id)`, so
    /// validating one element left the other sixty-three unchecked — measured at up to 14,970,312
    /// bytes past the end of the real segment. The residual guard there compares the header's
    /// declared `*_bytes` against the expected size, but that is another number the writer chose;
    /// nothing compared it to the MAPPING.
    ///
    /// `ring_view` one screen away had this right from the start: it bounds the header AND the
    /// entries array. This is the same rule for typed regions.
    fn region_slice<T>(&self, offset: u64, count: usize) -> Option<*const T> {
        let bytes = count.checked_mul(std::mem::size_of::<T>())?;
        let offset = usize::try_from(offset).ok()?;
        if !region_fits(offset, bytes, std::mem::align_of::<T>(), self.size) {
            return None;
        }
        Some(unsafe { self._mmap.as_ptr().add(offset) as *const T })
    }

    fn region<T>(&self, offset: u64) -> Option<*const T> {
        // Same precondition ring_view asserts: region_fits validates the OFFSET, so the resulting
        // address is only aligned if the BASE is. mmap gives a page-aligned base. Stated as an
        // assertion here too rather than in prose alone — the asymmetry was a review finding, and
        // the harness that motivated ring_view's assert was itself misaligned.
        debug_assert_eq!(
            self._mmap.as_ptr() as usize % std::mem::align_of::<T>(),
            0,
            "region assumes a mapping-aligned base; the region inherits the base's alignment"
        );
        let offset = usize::try_from(offset).ok()?;
        if !region_fits(
            offset,
            std::mem::size_of::<T>(),
            std::mem::align_of::<T>(),
            self.size,
        ) {
            return None;
        }
        Some(unsafe { self._mmap.as_ptr().add(offset) as *const T })
    }

    /// The diffs currently sitting in the engine's outbound ring, NEWEST LAST.
    ///
    /// PEEK, NEVER DRAIN. The outbound ring is single-consumer: the real UI advances
    /// `read_index`, and a tool that drained here would silently steal diffs from the app it is
    /// supposed to be observing — a debugging aid that breaks the thing being debugged. So this
    /// reads the slots between `read_index` and `write_index` and advances nothing.
    ///
    /// That makes it a SAMPLE rather than a stream: a diff already consumed by a running UI is
    /// gone before this can see it, and a slow caller can miss one to wrap-around. It is enough
    /// to answer "did the engine report this refusal", which is what it exists for, and a caller
    /// that needs every diff has to be the consumer rather than a bystander.
    /// The out ring's (capacity, read_index, write_index), for diagnostics.
    ///
    /// Task #52 has now had three wrong causes, and the reason each survived is that "attached"
    /// and "draining" were the only two things anyone could see. These three numbers separate the
    /// remaining cases outright: write_index STUCK means the engine is not writing to the ring
    /// this handle mapped (wrong segment, or an offset that moved under us); write_index MOVING
    /// with read_index stuck behind it means the drain is at fault. Nothing else fits.
    pub fn out_ring_cursors(&self) -> Option<(u32, u32, u32)> {
        let ring = self.ring_ui_out.as_ref()?;
        unsafe {
            Some((
                (*ring.header).capacity,
                (*ring.header).read_index.load(Ordering::Relaxed),
                (*ring.header).write_index.load(Ordering::Relaxed),
            ))
        }
    }

    /// Is the engine's OUT ring actually mapped?
    ///
    /// `attach` succeeds as soon as the shared segment can be opened, but `ring_view` returns None
    /// while the engine has not yet written its header — offset still 0, or capacity still 0. A
    /// handle in that state is attached and PERMANENTLY USELESS: `drain_ui_out` and
    /// `peek_ui_diffs` both bail on the missing ring and return nothing, for ever, because nothing
    /// re-resolves the view.
    ///
    /// A caller that attaches in a loop must therefore ask this, not just whether attach
    /// succeeded. See task #52 — the sidecar's event-drain thread attached at startup, won the
    /// race against the engine's header write, and delivered zero events for the whole session
    /// while the frame channel (attached later) was perfectly healthy.
    pub fn out_ring_ready(&self) -> bool {
        self.ring_ui_out.is_some()
    }

    pub fn peek_ui_diffs(&self) -> Vec<(u16, [u8; 40])> {
        let Some(ring) = self.ring_ui_out.as_ref() else {
            return Vec::new();
        };
        let read = unsafe { (*ring.header).read_index.load(Ordering::Acquire) };
        let write = unsafe { (*ring.header).write_index.load(Ordering::Acquire) };
        let mut out = Vec::new();
        let mut index = read;
        while index != write {
            let slot = (index & ring.mask) as usize;
            let entry = unsafe { std::ptr::read_volatile(ring.entries.add(slot)) };
            // `ready` is the multi-producer publication flag: a reserved-but-unfilled slot must
            // not be read as data. Same rule the engine follows.
            if entry.ready != 0 && entry.event_type == EventType::UiDiff as u16 {
                let diff_type = u16::from_le_bytes([entry.payload[0], entry.payload[1]]);
                out.push((diff_type, entry.payload));
            }
            // MASKED, LIKE EVERY OTHER WALK OF THIS RING. The indices in this header are SLOT
            // NUMBERS, not free-running counters — the engine advances with
            // `next = (write + 1) & ring.mask` (apps/event_ring.cpp) and drain_ui_out reads with
            // `read = (read + 1) & ring.mask`. Advancing unmasked let `index` climb past the
            // mask, where it can never equal the masked `write` again, so the loop ran until
            // u32 wrapped: about 4.3 billion iterations pushing entries into `out` the whole way.
            index = (index + 1) & ring.mask;
        }
        out
    }

    pub fn snapshot(&self) -> Option<UiSnapshot> {
        SeqlockReader::new(self.header).read_snapshot()
    }

    pub fn clip_version(&self) -> u32 {
        unsafe { std::ptr::read_volatile(&(*self.header).ui_clip_version) }
    }

    /// M2.17: the base version to present for an edit to `track_id`. Acceptance is
    /// per track in the engine, so the global counter is the WRONG base — another
    /// author typing on a different track moves it, and this edit is then refused as
    /// stale even though nothing touched this track. The per-track counter is
    /// published in that track's clip snapshot. Falls back to the global for a track
    /// with no published snapshot, which is the same fallback the engine guard makes.
    pub fn clip_version_for_track(&self, track_id: u32) -> u32 {
        match self.read_track_clip(track_id) {
            Some(snapshot) => snapshot.clip_version,
            None => self.clip_version(),
        }
    }

    /// The published loop span in nanoticks (start, end) — mirrors the engine's
    /// SetLoopRange, so the UI can draw the loop region.
    /// The audio device's block size in frames and its rate in Hz.
    ///
    /// Both zero until the engine has opened a device, which is a state worth forwarding
    /// rather than defaulting: a latency readout of "0.0ms" says the machine is perfect,
    /// and what it means is that nothing has started yet.
    ///
    /// The rate is rounded to an integer here. It is a double in the header and 44100 or
    /// 48000 in practice, and a readout that says 47999.9 would be worse than one that
    /// cannot say a fractional rate at all.
    pub fn device_block(&self) -> (u32, u32) {
        unsafe {
            let block = std::ptr::read_volatile(&(*self.header).block_size);
            let rate = std::ptr::read_volatile(&(*self.header).sample_rate);
            (block, if rate.is_finite() && rate > 0.0 { rate.round() as u32 } else { 0 })
        }
    }

    pub fn loop_range(&self) -> (u64, u64) {
        unsafe {
            (
                std::ptr::read_volatile(&(*self.header).ui_loop_start),
                std::ptr::read_volatile(&(*self.header).ui_loop_end),
            )
        }
    }

    /// LoadProject result signal: (seq, ok). `seq` increments once per load
    /// attempt (watch it move to know a load was processed); `ok` is 1 if the
    /// last attempt loaded, 0 if it was rejected — so a failed load is not a
    /// silent no-op.
    pub fn load_status(&self) -> (u32, u32) {
        unsafe {
            (
                std::ptr::read_volatile(&(*self.header).ui_load_seq),
                std::ptr::read_volatile(&(*self.header).ui_load_ok),
            )
        }
    }

    /// Block until the engine has acknowledged edits that advance the clip
    /// version from `base` to at least `target`, or `timeout` elapses; returns
    /// true if the target was reached. The engine bumps the clip version once
    /// per applied edit and republishes it, so this is how a writer waits for
    /// its writes to actually land instead of guessing a fixed delay. Progress
    /// is measured forward from `base` (`wrapping_sub`) so a u32 wrap is benign.
    /// There is no engine->reader notification channel in the lock-free SHM, so
    /// this polls the published version at a short interval — it returns as soon
    /// as the engine acks (about one audio block), not after a padded sleep.
    /// Wait for the HARMONY version to advance past `base`.
    ///
    /// The harmony timeline has its own counter and its own exact-match gate
    /// (`requireMatchingHarmonyVersion` demands `baseVersion == current`), so a caller that sends
    /// a harmony edit and returns immediately leaves the NEXT caller reading a version the engine
    /// has not reached yet. That one is refused, silently, into a resync diff nothing reads.
    ///
    /// Found from a live report: asking the agent for a key change across four bars produced
    /// "refused an edit composed against version" several times over and landed two of the four.
    /// Each tool call re-read the counter correctly; the counter simply had not moved yet.
    pub fn wait_for_harmony_version(
        &self,
        base: u32,
        timeout: std::time::Duration,
    ) -> bool {
        let deadline = std::time::Instant::now() + timeout;
        loop {
            if self.harmony_version() != base {
                return true;
            }
            if std::time::Instant::now() >= deadline {
                return false;
            }
            std::thread::sleep(std::time::Duration::from_micros(250));
        }
    }

    pub fn wait_for_clip_version(
        &self,
        base: u32,
        target: u32,
        timeout: std::time::Duration,
    ) -> bool {
        let want = target.wrapping_sub(base);
        let deadline = std::time::Instant::now() + timeout;
        loop {
            if self.clip_version().wrapping_sub(base) >= want {
                return true;
            }
            if std::time::Instant::now() >= deadline {
                return false;
            }
            std::thread::sleep(std::time::Duration::from_micros(250));
        }
    }

    /// Wait for ONE TRACK's clip version to advance, polling the per-track counter.
    ///
    /// wait_for_clip_version above polls the GLOBAL one, which is correct for callers whose base
    /// also came from the global. It is wrong for a caller whose base came from
    /// clip_version_for_track: the two counters move on different events, so comparing a
    /// per-track base against the global returns true as soon as anything anywhere advanced —
    /// including edits to other tracks — and the caller returns believing its own writes have
    /// settled when they may not have been applied at all.
    ///
    /// That crossing was live in add_notes, whose own comment two lines above the call explains
    /// that reading the global for the BASE is "exactly the failure the per-track counters were
    /// introduced to end". The base was right and the wait was not.
    pub fn wait_for_track_clip_version(
        &self,
        track_id: u32,
        base: u32,
        target: u32,
        timeout: std::time::Duration,
    ) -> bool {
        let want = target.wrapping_sub(base);
        let deadline = std::time::Instant::now() + timeout;
        loop {
            if self.clip_version_for_track(track_id).wrapping_sub(base) >= want {
                return true;
            }
            if std::time::Instant::now() >= deadline {
                return false;
            }
            std::thread::sleep(std::time::Duration::from_micros(250));
        }
    }

    pub fn track_count(&self) -> u32 {
        unsafe { std::ptr::read_volatile(&(*self.header).ui_track_count) }
    }

    /// Harmony edits are versioned against their own counter, not the clip one.
    pub fn harmony_version(&self) -> u32 {
        unsafe { std::ptr::read_volatile(&(*self.header).ui_harmony_version) }
    }

    /// Reads the clip window the engine last published, under the same seqlock
    /// the UI uses. Returns None while a write is in progress or no window has
    /// been requested yet.
    pub fn read_clip_window(&self) -> Option<UiClipWindowSnapshot> {
        let mut attempt = crate::reader::SeqlockAttempt::until(
            std::time::Instant::now() + crate::reader::DEFAULT_SEQLOCK_DEADLINE,
        );
        while let Some(v0) = attempt.begin(unsafe { &(*self.header).ui_version }) {
            let offset = unsafe { (*self.header).ui_clip_offset };
            let bytes = unsafe { (*self.header).ui_clip_bytes };
            if offset == 0 || bytes < std::mem::size_of::<UiClipWindowSnapshot>() as u64 {
                return None;
            }
            let Some(snapshot_ptr) = self.region::<UiClipWindowSnapshot>(offset) else {
                return None;
            };
            // A COPY, not a reference: this is inside a seqlock attempt, and the value must be
            // taken before commit() decides whether the read was torn.
            let snapshot = unsafe { *snapshot_ptr };
            if attempt.commit(unsafe { &(*self.header).ui_version }, v0) {
                return Some(snapshot);
            }
        }
        // The writer never finished within the deadline: the engine died mid-publish.
        None
    }

    /// Reads one track's clip from the all-tracks published region (v9), under
    /// the same seqlock. No request needed — any read-only observer sees notes
    /// this way. Returns None while a write is in progress, the region is absent
    /// (older engine), or the track index is out of range.
    pub fn read_track_clip(&self, track_id: u32) -> Option<UiClipWindowSnapshot> {
        if track_id as usize >= K_UI_MAX_TRACKS {
            return None;
        }
        let stride = std::mem::size_of::<UiClipWindowSnapshot>();
        let mut attempt = crate::reader::SeqlockAttempt::until(
            std::time::Instant::now() + crate::reader::DEFAULT_SEQLOCK_DEADLINE,
        );
        while let Some(v0) = attempt.begin(unsafe { &(*self.header).ui_version }) {
            let offset = unsafe { (*self.header).ui_clip_all_offset };
            let bytes = unsafe { (*self.header).ui_clip_all_bytes };
            if offset == 0 || (bytes as usize) < stride * K_UI_MAX_TRACKS {
                return None;
            }
            // THE WHOLE ARRAY, not one element: `base.add(track_id)` reaches the last track.
            let Some(base) = self.region_slice::<UiClipWindowSnapshot>(offset, K_UI_MAX_TRACKS)
            else {
                return None;
            };
            let snapshot = unsafe { *base.add(track_id as usize) };
            if attempt.commit(unsafe { &(*self.header).ui_version }, v0) {
                return Some(snapshot);
            }
        }
        // The writer never finished within the deadline: the engine died mid-publish.
        None
    }

    /// Reads the published clip extents — the placed-clip boxes that drive rails
    /// (M3.4) — under the seqlock. Loose (session) placements are not included.
    /// v27: the arrangement summary — the section spine RESOLVED, the meter points, and
    /// the song end. Read under the region's OWN version rather than the seqlock. That version
    /// is the region's GENERATION and it is 0 while a write is in progress, which is what makes
    /// a torn read detectable: version-body-version alone was NOT sufficient, because the
    /// engine used to leave the old version standing for the whole duration of the rewrite.
    /// Returns None while a write is in flight or the region is absent.
    pub fn read_arrange_summary(&self) -> Option<crate::layout::UiArrangeSummaryRegion> {
        let offset = unsafe { (*self.header).ui_arrange_offset };
        let bytes = unsafe { (*self.header).ui_arrange_bytes };
        if offset == 0
            || (bytes as usize) < std::mem::size_of::<crate::layout::UiArrangeSummaryRegion>()
        {
            return None;
        }
        let Some(base) = self.region::<crate::layout::UiArrangeSummaryRegion>(offset) else { return None; };
        for _ in 0..64 {
            let v0 = unsafe { std::ptr::read_volatile(&(*base).version) };
            // 0 means a write is IN FLIGHT. Without this the read was not torn-safe at all,
            // whatever the comments said: the engine only changed `version` AFTER writing the
            // body, so sampling it, reading a body mid-rewrite, and sampling again before the
            // stamp gave v0 == v1 and a torn spine that looked valid. The engine now zeroes it
            // for the duration of the write, so a reader that lands inside one sees 0 and
            // retries. Published generations start at 1.
            if v0 == 0 {
                continue;
            }
            let snapshot = unsafe { std::ptr::read_volatile(base) };
            fence(Ordering::Acquire);
            let v1 = unsafe { std::ptr::read_volatile(&(*base).version) };
            if v0 == v1 && v0 == snapshot.version {
                return Some(snapshot);
            }
        }
        None
    }

    pub fn read_clip_extents(&self) -> Vec<UiClipExtent> {
        self.read_clip_extents_with_truncation().0
    }

    /// The extents AND how many did not fit, from ONE seqlock pass.
    ///
    /// A non-zero count means the rails are incomplete. It is returned alongside the extents
    /// rather than from a separate accessor on purpose: two reads could land in different
    /// generations, and a count that disagrees with the list it describes is worse than no
    /// count. Callers that do not care use `read_clip_extents`, which is this with the number
    /// dropped — one implementation, so the two can never diverge.
    pub fn read_clip_extents_with_truncation(&self) -> (Vec<UiClipExtent>, u32) {
        let mut attempt = crate::reader::SeqlockAttempt::until(
            std::time::Instant::now() + crate::reader::DEFAULT_SEQLOCK_DEADLINE,
        );
        while let Some(v0) = attempt.begin(unsafe { &(*self.header).ui_version }) {
            let offset = unsafe { (*self.header).ui_clip_extent_offset };
            if offset == 0 {
                return (Vec::new(), 0);
            }
            let Some(region) = self.region::<UiClipExtentRegion>(offset) else { return (Vec::new(), 0); };
            let count = unsafe { (*region).count as usize }.min(K_UI_MAX_CLIP_EXTENTS);
            let truncated = unsafe { (*region).truncated };
            let mut out = Vec::with_capacity(count);
            for i in 0..count {
                out.push(unsafe { (*region).extents[i] });
            }
            if attempt.commit(unsafe { &(*self.header).ui_version }, v0) {
                return (out, truncated);
            }
        }
        // The writer never finished within the deadline — the engine died mid-publish.
        // Empty is what a fresh engine returns too, and it is the only answer that does
        // not hang the caller forever on a process that no longer exists.
        (Vec::new(), 0)
    }

    /// The published harmony timeline (root/scale changes over time), under the
    /// seqlock. Populated from the project's harmony_timeline on load.
    /// HOW MANY EVENTS THE REGION ACTUALLY HOLDS, without copying them.
    ///
    /// Exists because the harmony region — unlike the patcher's — carries NO version of its own,
    /// so a reader has to invalidate its cache against `ui_harmony_version` in the header, which
    /// is written by a different thread at a different time. When the version runs ahead of the
    /// region (the command thread bumps it; the consumer republishes on its next pass) a reader
    /// that stamps "read at version N" before reading caches a SHORT LIST FOREVER: the version
    /// never moves again, so it never re-reads. Four key changes written, two displayed, and a
    /// reload does not help because the cache is in the sidecar, not the page.
    ///
    /// A count is one u32 and catches exactly that. It is a stopgap for a missing field, not a
    /// design — the real fix is for the region to carry the version it corresponds to, which is
    /// what `UiPatcherRegion` already does and what the three reserved words here are for.
    /// THE VERSION THE PUBLISHED EVENTS ARE — written into the region after them, by the thread
    /// that fills it. Gate a cache on this, never on `harmony_version()` from the header: that one
    /// is bumped on the command thread and can run ahead of what is actually published.
    ///
    /// 0 from an engine that predates the field, which never matches a cache and so reads as
    /// "always stale" — slower, never wrong.
    pub fn harmony_region_version(&self) -> u32 {
        let off = unsafe { (*self.header).ui_harmony_offset };
        if off == 0 {
            return 0;
        }
        let Some(snap) = self.region::<UiHarmonySnapshot>(off) else { return 0; };
        unsafe { std::ptr::read_volatile(&(*snap).version) }
    }

    pub fn harmony_event_count(&self) -> u32 {
        let off = unsafe { (*self.header).ui_harmony_offset };
        if off == 0 {
            return 0;
        }
        let Some(snap) = self.region::<UiHarmonySnapshot>(off) else { return 0; };
        (unsafe { (*snap).event_count }).min(K_UI_MAX_HARMONY_EVENTS as u32)
    }

    pub fn read_harmony(&self) -> Vec<UiHarmonyEvent> {
        let mut attempt = crate::reader::SeqlockAttempt::until(
            std::time::Instant::now() + crate::reader::DEFAULT_SEQLOCK_DEADLINE,
        );
        while let Some(v0) = attempt.begin(unsafe { &(*self.header).ui_version }) {
            let off = unsafe { (*self.header).ui_harmony_offset };
            if off == 0 {
                return Vec::new();
            }
            let Some(snap) = self.region::<UiHarmonySnapshot>(off) else { return Vec::new(); };
            let count =
                (unsafe { (*snap).event_count } as usize).min(K_UI_MAX_HARMONY_EVENTS);
            let mut out = Vec::with_capacity(count);
            for i in 0..count {
                out.push(unsafe { (*snap).events[i] });
            }
            if attempt.commit(unsafe { &(*self.header).ui_version }, v0) {
                return out;
            }
        }
        // The writer never finished within the deadline — the engine died mid-publish.
        // Empty is what a fresh engine returns too, and it is the only answer that does
        // not hang the caller forever on a process that no longer exists.
        Vec::new()
    }

    /// The published mixer version — moves only when a track's gain/pan/mute/solo
    /// changes, so the UI can cache-key on it.
    pub fn mixer_version(&self) -> u32 {
        unsafe { std::ptr::read_volatile(&(*self.header).ui_mixer_version) }
    }

    /// Per-track mixer read-back for the current track count: gain in millibels,
    /// pan in thousandths, mute/solo in flags (MIXER_FLAG_*). Read under the
    /// seqlock so the row is internally consistent.
    pub fn read_mixer(&self) -> Vec<TrackMixer> {
        let mut attempt = crate::reader::SeqlockAttempt::until(
            std::time::Instant::now() + crate::reader::DEFAULT_SEQLOCK_DEADLINE,
        );
        while let Some(v0) = attempt.begin(unsafe { &(*self.header).ui_version }) {
            let count =
                unsafe { std::ptr::read_volatile(&(*self.header).ui_track_count) } as usize;
            let count = count.min(K_UI_MAX_TRACKS);
            let mut out = Vec::with_capacity(count);
            for i in 0..count {
                out.push(TrackMixer {
                    gain_millibels: unsafe { (*self.header).ui_track_gain_millibels[i] },
                    pan_thousandths: unsafe { (*self.header).ui_track_pan_thousandths[i] },
                    flags: unsafe { (*self.header).ui_track_mix_flags[i] },
                    ops_width: unsafe { (*self.header).ui_track_ops_width[i] },
                });
            }
            if attempt.commit(unsafe { &(*self.header).ui_version }, v0) {
                return out;
            }
        }
        // The writer never finished within the deadline — the engine died mid-publish.
        // Empty is what a fresh engine returns too, and it is the only answer that does
        // not hang the caller forever on a process that no longer exists.
        Vec::new()
    }

    /// The published patcher-version counter (moves on any patcher edit).
    pub fn patcher_version(&self) -> u32 {
        let off = unsafe { (*self.header).ui_patcher_offset };
        if off == 0 {
            return 0;
        }
        let Some(region) = self.region::<UiPatcherRegion>(off) else { return 0; };
        unsafe { std::ptr::read_volatile(&(*region).version) }
    }

    /// The published patcher graph the engine runs (one global graph today),
    /// under the seqlock. `device_id` is the device it's parked on (0 = default).
    pub fn read_patcher(&self) -> PatcherView {
        let mut attempt = crate::reader::SeqlockAttempt::until(
            std::time::Instant::now() + crate::reader::DEFAULT_SEQLOCK_DEADLINE,
        );
        while let Some(v0) = attempt.begin(unsafe { &(*self.header).ui_version }) {
            let off = unsafe { (*self.header).ui_patcher_offset };
            if off == 0 {
                return PatcherView::default();
            }
            let Some(region) = self.region::<UiPatcherRegion>(off) else { return PatcherView::default(); };
            let nodes_n =
                (unsafe { (*region).node_count } as usize).min(K_UI_MAX_PATCHER_NODES);
            let edges_n =
                (unsafe { (*region).edge_count } as usize).min(K_UI_MAX_PATCHER_EDGES);
            let mut view = PatcherView {
                version: unsafe { (*region).version },
                device_id: unsafe { (*region).device_id },
                nodes: Vec::with_capacity(nodes_n),
                edges: Vec::with_capacity(edges_n),
            };
            for i in 0..nodes_n {
                view.nodes.push(unsafe { (*region).nodes[i] });
            }
            for i in 0..edges_n {
                view.edges.push(unsafe { (*region).edges[i] });
            }
            if attempt.commit(unsafe { &(*self.header).ui_version }, v0) {
                return view;
            }
        }
        // The writer never finished within the deadline — the engine died mid-publish.
        // Empty is what a fresh engine returns too, and it is the only answer that does
        // not hang the caller forever on a process that no longer exists.
        PatcherView::default()
    }

    /// The engine's scale registry (v16). Written once at startup and immutable,
    /// so this is a plain read — no seqlock needed. Empty if not published.
    pub fn read_scales(&self) -> Vec<ScaleView> {
        let off = unsafe { (*self.header).ui_scales_offset };
        if off == 0 {
            return Vec::new();
        }
        let Some(region) = self.region::<UiScaleRegion>(off) else { return Vec::new(); };
        let n = (unsafe { (*region).scaleCount } as usize).min(K_UI_MAX_SCALES);
        let mut out = Vec::with_capacity(n);
        for i in 0..n {
            let s = unsafe { &(*region).scales[i] };
            let steps = (s.stepCount as usize).min(K_UI_MAX_SCALE_STEPS);
            out.push(ScaleView {
                id: s.id,
                name: cchar_str(&s.name),
                octave_cents: s.octaveMilliCents as f64 / 1000.0,
                step_cents: (0..steps)
                    .map(|k| s.stepMilliCents[k] as f64 / 1000.0)
                    .collect(),
            });
        }
        out
    }

    /// The device-params region (v17), refreshed by RequestDeviceParams. `version`
    /// bumps per publish; poll it after sending the request. Empty if not published.
    pub fn read_device_params(&self) -> DeviceParamsView {
        let off = unsafe { (*self.header).ui_device_params_offset };
        if off == 0 {
            return DeviceParamsView::default();
        }
        let Some(region) = self.region::<UiDeviceParamsRegion>(off) else { return DeviceParamsView::default(); };
        let n = (unsafe { (*region).paramCount } as usize).min(K_UI_MAX_DEVICE_PARAMS);
        let mut view = DeviceParamsView {
            version: unsafe { (*region).version },
            track_id: unsafe { (*region).trackId },
            device_id: unsafe { (*region).deviceId },
            device_name: cchar_str(unsafe { &(*region).deviceName }),
            params: Vec::with_capacity(n),
        };
        for i in 0..n {
            let p = unsafe { &(*region).params[i] };
            view.params.push(DeviceParamView {
                index: p.index,
                value: p.valueMilli as f32 / 1000.0,
                uid16: p.uid16,
                name: cchar_str(&p.name),
                display: cchar_str(&p.display),
                unit: cchar_str(&p.label),
                min_text: cchar_str(&p.minText),
                max_text: cchar_str(&p.maxText),
                default_value: p.defaultMilli as f32 / 1000.0,
                min: p.minMilli as f32 / 1000.0,
                max: p.maxMilli as f32 / 1000.0,
                step_count: p.stepCount,
                discrete: p.flags & crate::layout::UI_PARAM_DISCRETE != 0,
                automatable: p.flags & crate::layout::UI_PARAM_AUTOMATABLE != 0,
            });
        }
        view
    }

    /// Reads the audio source + clip descriptor tables (UiAudioSourceRegion). The
    /// region is version-gated and rewritten only at project load; a version double-
    /// check rejects a torn read against a concurrent load.
    pub fn read_audio_sources(&self) -> AudioSourcesView {
        let off = unsafe { (*self.header).ui_audio_source_offset };
        if off == 0 {
            return AudioSourcesView::default();
        }
        let Some(region) = self.region::<UiAudioSourceRegion>(off) else { return AudioSourcesView::default(); };
        let ver_ptr = unsafe { std::ptr::addr_of!((*region).version) } as *const AtomicU32;
        for _ in 0..256 {
            let v0 = unsafe { (*ver_ptr).load(Ordering::Acquire) };
            let source_count =
                (unsafe { (*region).sourceCount } as usize).min(K_UI_MAX_AUDIO_SOURCES);
            let clip_count =
                (unsafe { (*region).clipCount } as usize).min(K_UI_MAX_AUDIO_CLIPS);
            let mut view = AudioSourcesView {
                version: v0,
                audio_map_bpm_milli: unsafe { (*region).audioMapBpmMilli },
                format_version: unsafe { (*region).formatVersion },
                sources: Vec::with_capacity(source_count),
                clips: Vec::with_capacity(clip_count),
            };
            for i in 0..source_count {
                let s = unsafe { &(*region).sources[i] };
                view.sources.push(AudioSourceView {
                    source_id: s.sourceId,
                    content_key: s.contentKeyLo as u64 | ((s.contentKeyHi as u64) << 32),
                    source_channels: s.sourceChannels,
                    wave_channels: s.waveChannels,
                    status: s.status,
                    source_frames: s.sourceFrames,
                    source_rate_hz: s.sourceRateHz,
                    abs_peak: s.absPeak,
                    level_mask: s.levelMask,
                    path: cchar_str(&s.path),
                    flags: s.flags,
                });
            }
            for i in 0..clip_count {
                let c = unsafe { &(*region).clips[i] };
                view.clips.push(AudioClipView {
                    clip_id: c.clipId,
                    source_id: c.sourceId,
                    source_start_frame: c.sourceStartFrame,
                    clip_length_ticks: c.clipLengthTicks,
                    fade_in_ticks: c.fadeInTicks,
                    fade_out_ticks: c.fadeOutTicks,
                    gain_db: c.gainDb,
                    flags: c.flags,
                });
            }
            fence(Ordering::Acquire);
            let v1 = unsafe { (*ver_ptr).load(Ordering::Acquire) };
            if v0 == v1 {
                return view;
            }
        }
        AudioSourcesView::default()
    }

    /// Sends a windowed waveform query. The engine answers it into
    /// `slots[requestSeq % K_UI_WAVEFORM_SLOTS]`; read it back with
    /// `read_waveform_slot`. Same ring as every other command.
    pub fn send_waveform_request(
        &self,
        payload: UiWaveformRequestPayload,
    ) -> Result<(), String> {
        self.write_entry(
            &payload as *const UiWaveformRequestPayload as *const u8,
            std::mem::size_of::<UiWaveformRequestPayload>(),
        )
    }

    /// Reads one waveform answer slot under its per-slot seqlock (seq odd while the
    /// engine is writing). Returns None if the region is absent, the index is out of
    /// range, or the writer never settled. The caller must still check `request_seq`
    /// + `content_key` against what it asked — slots are reused mod the slot count.
    pub fn read_waveform_slot(&self, index: usize) -> Option<WaveformSlotView> {
        if index >= K_UI_WAVEFORM_SLOTS {
            return None;
        }
        let off = unsafe { (*self.header).ui_waveform_offset };
        if off == 0 {
            return None;
        }
        let Some(region) = self.region::<UiWaveformRegion>(off) else { return None; };
        let slot = unsafe { std::ptr::addr_of!((*region).slots[index]) };
        // The slot's `seq` is a plain u32 in the bindgen struct (SHM_BINDGEN maps the
        // C++ atomic to u32); read it through an AtomicU32 cast so the seqlock's
        // acquire ordering against the payload is real.
        let seq_ptr = unsafe { std::ptr::addr_of!((*slot).seq) } as *const AtomicU32;
        for _ in 0..4096 {
            let v0 = unsafe { (*seq_ptr).load(Ordering::Acquire) };
            if v0 % 2 == 1 {
                continue; // engine mid-write
            }
            let snap = unsafe { std::ptr::read_volatile(slot) };
            fence(Ordering::Acquire);
            let v1 = unsafe { (*seq_ptr).load(Ordering::Acquire) };
            if v0 == v1 && v0 % 2 == 0 {
                let n = (snap.columns as usize)
                    .saturating_mul(snap.channels as usize)
                    .saturating_mul(2)
                    .min(K_UI_WAVEFORM_MAX_PAIRS * 2);
                return Some(WaveformSlotView {
                    request_seq: snap.requestSeq,
                    source_id: snap.sourceId,
                    content_key: snap.contentKeyLo as u64
                        | ((snap.contentKeyHi as u64) << 32),
                    decimation: snap.decimation,
                    columns: snap.columns,
                    channels: snap.channels,
                    first_frame: snap.firstFrame,
                    frame_count: snap.frameCount,
                    status: snap.status,
                    flags: snap.flags,
                    sampler_addr: snap.samplerAddr,
                    pairs: snap.pairs[..n].to_vec(),
                });
            }
        }
        None
    }

    /// v28: the standing automation lane list. Version-gated like the other published regions —
    /// read it, compare `version` to what you cached, and skip the redraw when it has not moved.
    /// Returns an empty view (version 0) when the engine predates the region.
    pub fn read_automation_lanes(&self) -> AutomationLanesView {
        let off = unsafe { (*self.header).ui_automation_offset };
        if off == 0 {
            return AutomationLanesView::default();
        }
        let Some(region) = self.region::<UiAutomationLaneRegion>(off) else { return AutomationLanesView::default(); };
        // Same discipline as the arrange summary, for the same reason: version 0 means a write is
        // IN FLIGHT. Sampling the version, reading the body, and sampling again is NOT torn-safe
        // by itself — the engine only moves the number after writing, so a reader that lands
        // inside a rewrite sees v0 == v1 over a half-cleared array. Published generations are >= 1.
        for _ in 0..4096 {
            let v0 = unsafe { std::ptr::read_volatile(std::ptr::addr_of!((*region).version)) };
            if v0 == 0 {
                continue;
            }
            let count = unsafe {
                std::ptr::read_volatile(std::ptr::addr_of!((*region).laneCount))
            } as usize;
            let truncated = unsafe {
                std::ptr::read_volatile(std::ptr::addr_of!((*region).lanesTruncated))
            };
            let count = count.min(K_UI_MAX_AUTOMATION_LANES);
            let mut lanes = Vec::with_capacity(count);
            for i in 0..count {
                let lane = unsafe { std::ptr::read_volatile(std::ptr::addr_of!((*region).lanes[i])) };
                lanes.push(AutomationLaneView {
                    track_id: lane.trackId,
                    target_plugin_index: lane.targetPluginIndex,
                    param_id: cchar_str(&lane.paramId),
                    point_count: lane.pointCount,
                    discrete: lane.flags & UI_AUTOMATION_FLAG_DISCRETE != 0,
                });
            }
            fence(Ordering::Acquire);
            let v1 = unsafe { std::ptr::read_volatile(std::ptr::addr_of!((*region).version)) };
            if v0 == v1 {
                return AutomationLanesView { version: v0, truncated, lanes };
            }
        }
        AutomationLanesView::default()
    }

    /// Asks for one lane's points. The CALLER owns `request_seq` and therefore knows the slot the
    /// answer lands in: `read_automation_slot(request_seq as usize % K_UI_AUTOMATION_SLOTS)`.
    pub fn send_automation_lane_request(
        &self,
        payload: UiAutomationLaneRequestPayload,
    ) -> Result<(), String> {
        self.write_entry(
            &payload as *const UiAutomationLaneRequestPayload as *const u8,
            std::mem::size_of::<UiAutomationLaneRequestPayload>(),
        )
    }

    /// Reads one automation answer slot under its per-slot seqlock (seq ODD while the engine is
    /// writing). Returns None if the region is absent, the index is out of range, or the writer
    /// never settled. The caller must still check `request_seq` against what it asked: slots are
    /// reused mod the slot count, so a stale answer is indistinguishable from a fresh one on
    /// anything but the echo.
    pub fn read_automation_slot(&self, index: usize) -> Option<AutomationLaneAnswer> {
        if index >= K_UI_AUTOMATION_SLOTS {
            return None;
        }
        let off = unsafe { (*self.header).ui_automation_slot_offset };
        if off == 0 {
            return None;
        }
        let Some(region) = self.region::<UiAutomationSlotRegion>(off) else { return None; };
        let slot = unsafe { std::ptr::addr_of!((*region).slots[index]) };
        let seq_ptr = unsafe { std::ptr::addr_of!((*slot).seq) } as *const AtomicU32;
        for _ in 0..4096 {
            let v0 = unsafe { (*seq_ptr).load(Ordering::Acquire) };
            if v0 % 2 == 1 {
                continue; // engine mid-write
            }
            let snap = unsafe { std::ptr::read_volatile(slot) };
            fence(Ordering::Acquire);
            let v1 = unsafe { (*seq_ptr).load(Ordering::Acquire) };
            if v0 == v1 && v0 % 2 == 0 {
                let n = (snap.pointCount as usize).min(K_UI_MAX_AUTOMATION_POINTS);
                return Some(AutomationLaneAnswer {
                    request_seq: snap.requestSeq,
                    track_id: snap.trackId,
                    param_id: cchar_str(&snap.paramId),
                    found: snap.found != 0,
                    discrete: snap.flags & UI_AUTOMATION_FLAG_DISCRETE != 0,
                    points_truncated: snap.pointsTruncated,
                    points: snap.points[..n]
                        .iter()
                        .map(|p| (p.nanotick, p.value))
                        .collect(),
                });
            }
        }
        None
    }

    /// Read one answered envelope out of its seqlock slot. `None` while the engine is mid-write
    /// or the region is absent — the caller retries, exactly as with the automation lane.
    pub fn read_sampler_envelope_slot(&self, index: usize) -> Option<SamplerEnvelopeAnswer> {
        if index >= crate::layout::K_UI_SAMPLER_ENVELOPE_SLOTS {
            return None;
        }
        let off = unsafe { (*self.header).ui_sampler_envelope_offset };
        if off == 0 {
            return None;
        }
        let Some(region) = self.region::<crate::layout::UiSamplerEnvelopeRegion>(off) else { return None; };
        let slot = unsafe { std::ptr::addr_of!((*region).slots[index]) };
        let seq_ptr = unsafe { std::ptr::addr_of!((*slot).seq) } as *const AtomicU32;
        for _ in 0..4096 {
            let v0 = unsafe { (*seq_ptr).load(Ordering::Acquire) };
            if v0 % 2 == 1 {
                continue; // engine mid-write
            }
            let snap = unsafe { std::ptr::read_volatile(slot) };
            fence(Ordering::Acquire);
            let v1 = unsafe { (*seq_ptr).load(Ordering::Acquire) };
            if v0 == v1 && v0 % 2 == 0 {
                let n = (snap.pointCount as usize)
                    .min(crate::layout::K_UI_MAX_ENVELOPE_POINTS);
                return Some(SamplerEnvelopeAnswer {
                    request_seq: snap.requestSeq,
                    track_id: snap.trackId,
                    device_id: snap.deviceId,
                    mod_set_id: snap.modSetId,
                    modulator_id: snap.modulatorId,
                    target: snap.target,
                    found: snap.found != 0,
                    time_base: snap.timeBase,
                    rate_milli: snap.rateMilli,
                    points_truncated: snap.pointsTruncated,
                    sustain_loop: (snap.sustainLoopStart, snap.sustainLoopEnd),
                    release_loop: (snap.releaseLoopStart, snap.releaseLoopEnd),
                    release_fade: snap.releaseFade,
                    points: snap.points[..n]
                        .iter()
                        .map(|p| (p.time, p.valueMilli, p.tension, p.flags))
                        .collect(),
                });
            }
        }
        None
    }

    /// Ask for one modulator's envelope shape (RequestSamplerEnvelope, 97).
    pub fn send_sampler_envelope_request(
        &self,
        payload: crate::layout::UiSamplerEnvelopeRequestPayload,
    ) -> Result<(), String> {
        self.write_entry(
            &payload as *const crate::layout::UiSamplerEnvelopeRequestPayload as *const u8,
            std::mem::size_of::<crate::layout::UiSamplerEnvelopeRequestPayload>(),
        )
    }

    /// Per-track display names for the current track count (nul-trimmed).
    pub fn read_track_names(&self) -> Vec<String> {
        let mut attempt = crate::reader::SeqlockAttempt::until(
            std::time::Instant::now() + crate::reader::DEFAULT_SEQLOCK_DEADLINE,
        );
        while let Some(v0) = attempt.begin(unsafe { &(*self.header).ui_version }) {
            let count =
                unsafe { std::ptr::read_volatile(&(*self.header).ui_track_count) } as usize;
            let count = count.min(K_UI_MAX_TRACKS);
            let mut out = Vec::with_capacity(count);
            for i in 0..count {
                let raw = unsafe { &(*self.header).ui_track_name[i] };
                let end = raw.iter().position(|&b| b == 0).unwrap_or(raw.len());
                out.push(String::from_utf8_lossy(&raw[..end]).into_owned());
            }
            if attempt.commit(unsafe { &(*self.header).ui_version }, v0) {
                return out;
            }
        }
        // The writer never finished within the deadline — the engine died mid-publish.
        // Empty is what a fresh engine returns too, and it is the only answer that does
        // not hang the caller forever on a process that no longer exists.
        Vec::new()
    }

    /// A patcher node's configuration.
    ///
    /// Its own payload rather than a UiCommandPayload: every command payload is
    /// 40 bytes, so the engine checks the size and then dispatches on
    /// commandType — the shape has to match the one that command reads, field
    /// for field, or the engine reads a config out of the wrong offsets.
    pub fn send_patcher_config(
        &self,
        payload: UiPatcherNodeConfigPayload,
    ) -> Result<(), String> {
        self.write_entry(
            &payload as *const UiPatcherNodeConfigPayload as *const u8,
            std::mem::size_of::<UiPatcherNodeConfigPayload>(),
        )
    }

    /// Add, remove or connect patcher nodes. Same story as the config above:
    /// a distinct 40-byte shape the engine reads for these three command types.
    pub fn send_patcher_graph(
        &self,
        payload: UiPatcherGraphCommandPayload,
    ) -> Result<(), String> {
        self.write_entry(
            &payload as *const UiPatcherGraphCommandPayload as *const u8,
            std::mem::size_of::<UiPatcherGraphCommandPayload>(),
        )
    }

    /// v23: the first instrument's name per track (empty when the track has none).
    pub fn read_track_device_names(&self) -> Vec<String> {
        let mut attempt = crate::reader::SeqlockAttempt::until(
            std::time::Instant::now() + crate::reader::DEFAULT_SEQLOCK_DEADLINE,
        );
        while let Some(v0) = attempt.begin(unsafe { &(*self.header).ui_version }) {
            let count =
                unsafe { std::ptr::read_volatile(&(*self.header).ui_track_count) } as usize;
            let count = count.min(K_UI_MAX_TRACKS);
            let mut out = Vec::with_capacity(count);
            for i in 0..count {
                let raw = unsafe { &(*self.header).ui_track_device_name[i] };
                let end = raw.iter().position(|&b| b == 0).unwrap_or(raw.len());
                out.push(String::from_utf8_lossy(&raw[..end]).into_owned());
            }
            if attempt.commit(unsafe { &(*self.header).ui_version }, v0) {
                return out;
            }
        }
        // The writer never finished within the deadline — the engine died mid-publish.
        // Empty is what a fresh engine returns too, and it is the only answer that does
        // not hang the caller forever on a process that no longer exists.
        Vec::new()
    }

    /// Per-track stable ids (uiTrackId) + flags (UI_TRACK_FLAG_*), read together under
    /// the seqlock so a caller can key on the id (never the moving slot) and tell the
    /// master / absent / child entries apart. Returns (ids, flags), both `track_count` long.
    /// v26 per-lane quantize, as (grid nanoticks, strength thousandths, swing
    /// thousandths) per published slot, plus `ui_quantize_version`.
    ///
    /// SWING IS PLAIN SIGNED HERE. The SetLaneQuantize *command* carries it biased
    /// by +500 because the payload field is unsigned; the read-back is written from
    /// the runtime's already-debiased value and is not. Applying the bias on both
    /// legs is an off-by-500 that would read as a groove nobody asked for.
    ///
    /// Its own reader rather than a UiSnapshot field: the snapshot is copied inside
    /// the seqlock on every frame, and these are three more 64-entry arrays — 1 KB
    /// of copying at 120 Hz for values that change when a person turns a knob.
    pub fn read_track_quantize(&self) -> (Vec<(u64, u32, i32)>, u32) {
        let mut attempt = crate::reader::SeqlockAttempt::until(
            std::time::Instant::now() + crate::reader::DEFAULT_SEQLOCK_DEADLINE,
        );
        while let Some(v0) = attempt.begin(unsafe { &(*self.header).ui_version }) {
            let count =
                unsafe { std::ptr::read_volatile(&(*self.header).ui_track_count) } as usize;
            let count = count.min(K_UI_MAX_TRACKS);
            let mut out = Vec::with_capacity(count);
            for i in 0..count {
                out.push((
                    unsafe { std::ptr::read_volatile(&(*self.header).ui_track_quantize_grid[i]) },
                    unsafe {
                        std::ptr::read_volatile(&(*self.header).ui_track_quantize_strength[i])
                    },
                    unsafe { std::ptr::read_volatile(&(*self.header).ui_track_quantize_swing[i]) },
                ));
            }
            let version =
                unsafe { std::ptr::read_volatile(&(*self.header).ui_quantize_version) };
            if attempt.commit(unsafe { &(*self.header).ui_version }, v0) {
                return (out, version);
            }
        }
        // The writer never finished within the deadline — the engine died mid-publish.
        // Empty is what a fresh engine returns too, and it is the only answer that does
        // not hang the caller forever on a process that no longer exists.
        (Vec::new(), 0)
    }

    pub fn read_track_ids_and_flags(&self) -> (Vec<u32>, Vec<u8>) {
        let mut attempt = crate::reader::SeqlockAttempt::until(
            std::time::Instant::now() + crate::reader::DEFAULT_SEQLOCK_DEADLINE,
        );
        while let Some(v0) = attempt.begin(unsafe { &(*self.header).ui_version }) {
            let count =
                unsafe { std::ptr::read_volatile(&(*self.header).ui_track_count) } as usize;
            let count = count.min(K_UI_MAX_TRACKS);
            let mut ids = Vec::with_capacity(count);
            let mut flags = Vec::with_capacity(count);
            for i in 0..count {
                ids.push(unsafe { std::ptr::read_volatile(&(*self.header).ui_track_id[i]) });
                flags.push(unsafe { std::ptr::read_volatile(&(*self.header).ui_track_flags[i]) });
            }
            if attempt.commit(unsafe { &(*self.header).ui_version }, v0) {
                return (ids, flags);
            }
        }
        // The writer never finished within the deadline — the engine died mid-publish.
        // Empty is what a fresh engine returns too, and it is the only answer that does
        // not hang the caller forever on a process that no longer exists.
        (Vec::new(), Vec::new())
    }

    /// v24 per-insert meters for one track SLOT: (device_id, in_peak_mb, out_peak_mb,
    /// in_rms_mb, out_rms_mb) per insert, dBFS millibels. Entries with device_id ==
    /// UI_METER_NO_DEVICE are empty slots, and UI_METER_SILENT means silent/below floor —
    /// both are distinct from a real level, so render them as "no meter" not as -327 dB.
    pub fn read_device_meters(&self, slot: usize) -> Vec<(u32, i16, i16, i16, i16)> {
        use crate::layout::{UiDeviceMeterRegion, K_UI_MAX_METERED_DEVICES, UI_METER_NO_DEVICE};
        let mut out = Vec::new();
        if slot >= crate::layout::K_UI_MAX_TRACKS {
            return out;
        }
        let offset = unsafe { std::ptr::read_volatile(&(*self.header).ui_device_meter_offset) };
        if offset == 0 {
            return out;
        }
        let Some(region_ptr) = self.region::<UiDeviceMeterRegion>(offset) else { return out; };
        let region = unsafe { &*region_ptr };
        for d in 0..K_UI_MAX_METERED_DEVICES {
            let m = region.meters[slot][d];
            if m.device_id == UI_METER_NO_DEVICE {
                continue;
            }
            out.push((m.device_id, m.in_peak_mb, m.out_peak_mb, m.in_rms_mb, m.out_rms_mb));
        }
        out
    }

    pub fn send_chord_command(&self, payload: UiChordCommandPayload) -> Result<(), String> {
        self.write_entry(
            &payload as *const UiChordCommandPayload as *const u8,
            std::mem::size_of::<UiChordCommandPayload>(),
        )
    }

    pub fn send_clip_window_request(
        &self,
        payload: UiClipWindowCommandPayload,
    ) -> Result<(), String> {
        self.write_entry(
            &payload as *const UiClipWindowCommandPayload as *const u8,
            std::mem::size_of::<UiClipWindowCommandPayload>(),
        )
    }

    /// Drain the engine's outbound diff ring into `out`, up to `max` entries.
    ///
    /// SINGLE CONSUMER. This advances the ring's read_index, so exactly one
    /// thread in one process may call it for a given segment — whatever drains
    /// it takes the messages away from everyone else. Nothing else consumes this
    /// ring on the UI segment today (the engine only writes; the C++ device-chain
    /// tests read their own segments), which is why it is safe to start.
    ///
    /// Returns the number drained. An empty ring is the normal case and costs
    /// two atomic loads.
    pub fn drain_ui_out(&self, out: &mut Vec<EventEntry>, max: usize) -> usize {
        let Some(ring) = self.ring_ui_out.as_ref() else { return 0 };
        let mut read = unsafe { (*ring.header).read_index.load(Ordering::Relaxed) };
        let write = unsafe { (*ring.header).write_index.load(Ordering::Acquire) };
        let mut n = 0;
        while read != write && n < max {
            // Masked here for the same reason as write_entry: the FIRST `read` is the raw shared
            // value, and only the increment below was masked — so every iteration after the first
            // was in range and the first was not.
            out.push(unsafe { *ring.entries.add((read & ring.mask) as usize) });
            read = (read + 1) & ring.mask;
            n += 1;
        }
        if n > 0 {
            unsafe { (*ring.header).read_index.store(read, Ordering::Release) };
        }
        n
    }

    /// Writes one command into the UI ring. Returns false when the ring is
    /// full, which means the engine is not draining and the caller should
    /// retry rather than treat the command as sent.
    pub fn send_command(&self, payload: UiCommandPayload) -> Result<(), String> {
        self.write_entry(
            &payload as *const UiCommandPayload as *const u8,
            std::mem::size_of::<UiCommandPayload>(),
        )
    }

    /// Send a device-chain edit (AddDevice/RemoveDevice/MoveDevice/UpdateDevice). Same
    /// ring as send_command; a distinct payload (UiChainCommandPayload). track_id may be
    /// MASTER_TRACK_ID to edit the master chain.
    ///
    /// The distinct payload is load-bearing, not tidiness: the engine matches on
    /// the entry's SIZE first and only then looks at commandType, so a chain edit
    /// sent in a UiCommandPayload is not refused — it is read as some other
    /// command's fields, or ignored entirely.
    pub fn send_chain_command(
        &self,
        payload: crate::layout::UiChainCommandPayload,
    ) -> Result<(), String> {
        self.write_entry(
            &payload as *const crate::layout::UiChainCommandPayload as *const u8,
            std::mem::size_of::<crate::layout::UiChainCommandPayload>(),
        )
    }

    /// Load a sample into a sampler device, minting a source and a slot.
    pub fn send_sampler_load(
        &self,
        payload: crate::layout::UiSamplerLoadPayload,
    ) -> Result<(), String> {
        self.write_entry(
            &payload as *const crate::layout::UiSamplerLoadPayload as *const u8,
            std::mem::size_of::<crate::layout::UiSamplerLoadPayload>(),
        )
    }

    /// Edit one field of one sampler slot.
    pub fn send_sampler_set_slot(
        &self,
        payload: crate::layout::UiSamplerSetSlotPayload,
    ) -> Result<(), String> {
        self.write_entry(
            &payload as *const crate::layout::UiSamplerSetSlotPayload as *const u8,
            std::mem::size_of::<crate::layout::UiSamplerSetSlotPayload>(),
        )
    }

    /// Write a note's row ops. The MASK in the payload says which of them this command is
    /// speaking about; a bit clear leaves that op untouched.
    pub fn send_row_ops(
        &self,
        payload: crate::layout::UiSetRowOpsPayload,
    ) -> Result<(), String> {
        self.write_entry(
            &payload as *const crate::layout::UiSetRowOpsPayload as *const u8,
            std::mem::size_of::<crate::layout::UiSetRowOpsPayload>(),
        )
    }

    pub fn send_sampler_set_device(
        &self,
        payload: crate::layout::UiSamplerSetDevicePayload,
    ) -> Result<(), String> {
        self.write_entry(
            &payload as *const crate::layout::UiSamplerSetDevicePayload as *const u8,
            std::mem::size_of::<crate::layout::UiSamplerSetDevicePayload>(),
        )
    }

    /// Ask the engine to publish one sampler device's kit, then read the answer.
    pub fn send_sampler_kit_request(
        &self,
        payload: crate::layout::UiSamplerKitRequestPayload,
    ) -> Result<(), String> {
        self.write_entry(
            &payload as *const crate::layout::UiSamplerKitRequestPayload as *const u8,
            std::mem::size_of::<crate::layout::UiSamplerKitRequestPayload>(),
        )
    }

    /// Reads one answered kit slot under its seqlock. `None` while the engine is mid-write or
    /// the region does not exist.
    /// The kit's POLL COUNTER: bumped whenever any track's sampler state changes, and written
    /// every publish cycle so it can be read WITHOUT first asking for a kit. 0 means the engine
    /// does not publish one (the counter starts at 1), which is distinguishable from an
    /// unchanged kit precisely because it never appears after a change.
    pub fn sampler_kit_version(&self) -> u32 {
        let off = unsafe { (*self.header).ui_sampler_kit_offset };
        if off == 0 {
            return 0;
        }
        let Some(region) = self.region::<crate::layout::UiSamplerKitRegion>(off) else { return 0; };
        let ptr = unsafe { std::ptr::addr_of!((*region).version) } as *const AtomicU32;
        unsafe { (*ptr).load(Ordering::Acquire) }
    }

    pub fn read_sampler_kit_slot(&self, index: usize) -> Option<SamplerKitView> {
        if index >= crate::layout::UI_SAMPLER_KIT_SLOTS {
            return None;
        }
        let off = unsafe { (*self.header).ui_sampler_kit_offset };
        if off == 0 {
            return None;
        }
        let Some(region) = self.region::<crate::layout::UiSamplerKitRegion>(off) else { return None; };
        let slot = unsafe { std::ptr::addr_of!((*region).slots[index]) };
        let seq_ptr = unsafe { std::ptr::addr_of!((*slot).seq) } as *const AtomicU32;
        for _ in 0..4096 {
            let v0 = unsafe { (*seq_ptr).load(Ordering::Acquire) };
            if v0 % 2 == 1 {
                continue; // engine mid-write
            }
            let snap = unsafe { std::ptr::read_volatile(slot) };
            fence(Ordering::Acquire);
            let v1 = unsafe { (*seq_ptr).load(Ordering::Acquire) };
            if v0 == v1 && v0 % 2 == 0 {
                let n = (snap.slotCount as usize).min(crate::layout::UI_MAX_SAMPLER_SLOTS);
                return Some(SamplerKitView {
                    request_seq: snap.requestSeq,
                    track_id: snap.trackId,
                    device_id: snap.deviceId,
                    found: snap.found != 0,
                    voice_cap: snap.voiceCap,
                    default_gate: snap.defaultGate,
                    default_view: snap.defaultView,
                    active_voices: snap.activeVoices,
                    steals: snap.steals,
                    unmapped: snap.unmapped,
                    content_version: snap.contentVersion,
                    slots_truncated: snap.slotsTruncated,
                    slots: snap.slots[..n].to_vec(),
                });
            }
        }
        None
    }

    /// Slice a source (transient / equal / clear).
    pub fn send_sampler_slice(
        &self,
        payload: crate::layout::UiSamplerSlicePayload,
    ) -> Result<(), String> {
        self.write_entry(
            &payload as *const crate::layout::UiSamplerSlicePayload as *const u8,
            std::mem::size_of::<crate::layout::UiSamplerSlicePayload>(),
        )
    }

    /// Add, move or remove one slice marker.
    pub fn send_sampler_marker(
        &self,
        payload: crate::layout::UiSamplerMarkerPayload,
    ) -> Result<(), String> {
        self.write_entry(
            &payload as *const crate::layout::UiSamplerMarkerPayload as *const u8,
            std::mem::size_of::<crate::layout::UiSamplerMarkerPayload>(),
        )
    }

    /// Send an arbitrarily long payload as BulkChunk (83) entries for the engine to reassemble.
    ///
    /// `payload` must already begin with the real commandType as its first u16 — the assembled
    /// buffer IS a payload, which is what keeps one dispatch rule instead of two.
    ///
    /// The last chunk is ZERO-PADDED to 32 bytes rather than short, so every entry on the ring is
    /// the same size and the reader never needs a length field it could disagree with. The
    /// message's own header carries the real length; trailing zeroes are the payload's business.
    pub fn send_bulk(&self, payload: &[u8]) -> Result<(), String> {
        // THE STREAM ID IS OURS TO PICK, not the caller's. The engine merges chunks that share
        // an id, so reusing one while an earlier message is still incomplete interleaves two
        // payloads into one buffer and delivers a corrupt message that still parses. Asking
        // callers to vary it is asking them to hold a correctness property the library can
        // simply guarantee — and the first caller I wrote derived it from the pid, which is
        // CONSTANT for a process sending twice.
        //
        // Seeded from the pid so two processes are unlikely to collide, incremented per message
        // so one process never collides with itself. Never zero: a zero id is the obvious value
        // for an uninitialised sender, and keeping it out of circulation makes that mistake
        // visible instead of merging into a real stream.
        static NEXT_STREAM: std::sync::atomic::AtomicU16 = std::sync::atomic::AtomicU16::new(0);
        let seed = std::process::id() as u16;
        let n = NEXT_STREAM.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        let stream_id = seed.wrapping_add(n) | 1;
        if payload.len() < 2 {
            return Err("bulk payload must carry a commandType".to_string());
        }
        let chunks = payload.len().div_ceil(crate::layout::BULK_CHUNK_BYTES);
        if chunks == 0 || chunks > 512 {
            return Err(format!("bulk payload needs {chunks} chunks, max 512"));
        }
        for seq in 0..chunks {
            let start = seq * crate::layout::BULK_CHUNK_BYTES;
            let end = usize::min(start + crate::layout::BULK_CHUNK_BYTES, payload.len());
            let mut bytes = [0u8; 32];
            bytes[..end - start].copy_from_slice(&payload[start..end]);
            let chunk = crate::layout::UiBulkChunkPayload {
                command_type: crate::layout::UiCommandType::BulkChunk as u16,
                stream_id,
                seq: seq as u16,
                total: chunks as u16,
                bytes,
            };
            self.write_entry(
                &chunk as *const crate::layout::UiBulkChunkPayload as *const u8,
                std::mem::size_of::<crate::layout::UiBulkChunkPayload>(),
            )?;
        }
        Ok(())
    }

    /// Set a sampler modulator's LFO (SamplerSetLfo).
    pub fn send_sampler_lfo(
        &self,
        payload: crate::layout::UiSamplerLfoPayload,
    ) -> Result<(), String> {
        self.write_entry(
            &payload as *const crate::layout::UiSamplerLfoPayload as *const u8,
            std::mem::size_of::<crate::layout::UiSamplerLfoPayload>(),
        )
    }

    /// Set a sampler mod set's filter (SamplerSetFilter).
    ///
    /// The field that nothing could write: `filterType` was read at the kit publish site and
    /// written nowhere, so every cutoff and resonance modulator reachable from a UI modulated a
    /// filter that was off.
    pub fn send_sampler_filter(
        &self,
        payload: crate::layout::UiSamplerFilterPayload,
    ) -> Result<(), String> {
        self.write_entry(
            &payload as *const crate::layout::UiSamplerFilterPayload as *const u8,
            std::mem::size_of::<crate::layout::UiSamplerFilterPayload>(),
        )
    }

    /// Set a sampler mod set's VINTAGE: bit depth and rate reduction (SamplerSetVintage).
    pub fn send_sampler_vintage(
        &self,
        payload: crate::layout::UiSamplerVintagePayload,
    ) -> Result<(), String> {
        self.write_entry(
            &payload as *const crate::layout::UiSamplerVintagePayload as *const u8,
            std::mem::size_of::<crate::layout::UiSamplerVintagePayload>(),
        )
    }

    /// Set a CLIP's own subdivision and meter (SetClipGrid, 94).
    pub fn send_clip_grid(
        &self,
        payload: crate::layout::UiSetClipGridPayload,
    ) -> Result<(), String> {
        self.write_entry(
            &payload as *const crate::layout::UiSetClipGridPayload as *const u8,
            std::mem::size_of::<crate::layout::UiSetClipGridPayload>(),
        )
    }

    /// Set one field of an audio clip: in-point, gain, or either fade (SetAudioClipField).
    pub fn send_audio_clip_field(
        &self,
        payload: crate::layout::UiAudioClipFieldPayload,
    ) -> Result<(), String> {
        self.write_entry(
            &payload as *const crate::layout::UiAudioClipFieldPayload as *const u8,
            std::mem::size_of::<crate::layout::UiAudioClipFieldPayload>(),
        )
    }

    /// Set a sampler modulator's ADSR (SamplerSetEnvelope).
    pub fn send_sampler_envelope(
        &self,
        payload: crate::layout::UiSamplerEnvelopePayload,
    ) -> Result<(), String> {
        self.write_entry(
            &payload as *const crate::layout::UiSamplerEnvelopePayload as *const u8,
            std::mem::size_of::<crate::layout::UiSamplerEnvelopePayload>(),
        )
    }

    /// Write a note's row ops (SetRowOps). The write half of what the engine already publishes.
    pub fn send_set_row_ops(
        &self,
        payload: crate::layout::UiSetRowOpsPayload,
    ) -> Result<(), String> {
        self.write_entry(
            &payload as *const crate::layout::UiSetRowOpsPayload as *const u8,
            std::mem::size_of::<crate::layout::UiSetRowOpsPayload>(),
        )
    }

    /// Write the pattern that reproduces a chop.
    pub fn send_sampler_emit_rows(
        &self,
        payload: crate::layout::UiSamplerEmitRowsPayload,
    ) -> Result<(), String> {
        self.write_entry(
            &payload as *const crate::layout::UiSamplerEmitRowsPayload as *const u8,
            std::mem::size_of::<crate::layout::UiSamplerEmitRowsPayload>(),
        )
    }

    /// Send a track-routing replace (SetTrackRouting). Its own 40-byte payload,
    /// matched on SIZE by the engine before commandType is looked at — see
    /// send_chain_command for why that matters.
    ///
    /// Named `send_routing_command` after a merge in which both sides had added the
    /// same wrapper under different names. One function, one name.
    pub fn send_routing_command(
        &self,
        payload: crate::layout::UiTrackRoutingPayload,
    ) -> Result<(), String> {
        self.write_entry(
            &payload as *const crate::layout::UiTrackRoutingPayload as *const u8,
            std::mem::size_of::<crate::layout::UiTrackRoutingPayload>(),
        )
    }

    /// Add or remove a modulation link.
    pub fn send_mod_link_command(
        &self,
        payload: crate::layout::UiModLinkCommandPayload,
    ) -> Result<(), String> {
        self.write_entry(
            &payload as *const crate::layout::UiModLinkCommandPayload as *const u8,
            std::mem::size_of::<crate::layout::UiModLinkCommandPayload>(),
        )
    }

    /// Name the VST parameter a link targets.
    pub fn send_mod_link_uid16(
        &self,
        payload: crate::layout::UiModLinkUid16Payload,
    ) -> Result<(), String> {
        self.write_entry(
            &payload as *const crate::layout::UiModLinkUid16Payload as *const u8,
            std::mem::size_of::<crate::layout::UiModLinkUid16Payload>(),
        )
    }

    /// Drive a modulation source value (turn a macro knob).
    pub fn send_mod_source_value(
        &self,
        payload: crate::layout::UiModSourceValuePayload,
    ) -> Result<(), String> {
        self.write_entry(
            &payload as *const crate::layout::UiModSourceValuePayload as *const u8,
            std::mem::size_of::<crate::layout::UiModSourceValuePayload>(),
        )
    }

    /// Add/remove a patcher node, or connect two.
    pub fn send_patcher_graph_command(
        &self,
        payload: crate::layout::UiPatcherGraphCommandPayload,
    ) -> Result<(), String> {
        self.write_entry(
            &payload as *const crate::layout::UiPatcherGraphCommandPayload as *const u8,
            std::mem::size_of::<crate::layout::UiPatcherGraphCommandPayload>(),
        )
    }

    /// Configure a patcher node.
    pub fn send_patcher_node_config(
        &self,
        payload: crate::layout::UiPatcherNodeConfigPayload,
    ) -> Result<(), String> {
        self.write_entry(
            &payload as *const crate::layout::UiPatcherNodeConfigPayload as *const u8,
            std::mem::size_of::<crate::layout::UiPatcherNodeConfigPayload>(),
        )
    }

    /// Write one automation point.
    pub fn send_automation_point(
        &self,
        payload: crate::layout::UiAutomationPointPayload,
    ) -> Result<(), String> {
        self.write_entry(
            &payload as *const crate::layout::UiAutomationPointPayload as *const u8,
            std::mem::size_of::<crate::layout::UiAutomationPointPayload>(),
        )
    }

    /// v29: send a MARKER command (add / remove / rename / move). Total — a marker names a
    /// position and moves no material.
    pub fn send_marker_command(
        &self,
        payload: crate::layout::UiMarkerCommandPayload,
    ) -> Result<(), String> {
        self.write_entry(
            &payload as *const crate::layout::UiMarkerCommandPayload as *const u8,
            std::mem::size_of::<crate::layout::UiMarkerCommandPayload>(),
        )
    }

    /// v29: send a TIMELINE command — SetTimeSignature, or InsertRemoveTime (the ripple).
    pub fn send_arrange_time_command(
        &self,
        payload: crate::layout::UiArrangeTimeCommandPayload,
    ) -> Result<(), String> {
        self.write_entry(
            &payload as *const crate::layout::UiArrangeTimeCommandPayload as *const u8,
            std::mem::size_of::<crate::layout::UiArrangeTimeCommandPayload>(),
        )
    }

    /// Send a rack knob write. Same ring as send_command; a distinct payload shape
    /// (UiSetParamPayload carries a uid16 that does not fit UiCommandPayload's fields).
    pub fn send_set_param(&self, payload: crate::layout::UiSetParamPayload) -> Result<(), String> {
        self.write_entry(
            &payload as *const crate::layout::UiSetParamPayload as *const u8,
            std::mem::size_of::<crate::layout::UiSetParamPayload>(),
        )
    }

    /// The engine dispatches on the entry's payload size, so every command
    /// shape shares one ring-write path.
    fn write_entry(&self, payload: *const u8, size: usize) -> Result<(), String> {
        let Some(ring) = self.ring_ui.as_ref() else {
            return Err("handle was not opened for writing".to_string());
        };
        if size > 40 {
            return Err(format!("payload of {size} bytes does not fit an EventEntry"));
        }
        let mut entry = EventEntry {
            // P2-CMD-00: the command's identity rides here. sampleTime is unused on a UI
            // command entry — it names an audio position and a command has none — so these eight
            // bytes were free and are exactly the id's width. Giving them a meaning is why
            // kShmVersion moves to 39; see the rule at that constant.
            sample_time: command_id_next(),
            block_id: 0,
            event_type: EventType::UiCommand as u16,
            size: size as u16,
            flags: 0,
            payload: [0u8; 40],
            ready: 0,
        };
        let bytes = unsafe { std::slice::from_raw_parts(payload, size) };
        entry.payload[..bytes.len()].copy_from_slice(bytes);

        // M2.18: MULTI-PRODUCER. Reserve the slot with a CAS on write_index, fill it,
        // and only then publish it by setting `ready`. Reading write_index and storing
        // it back — what this did before — meant two writers claimed the same slot and
        // one command was silently lost, which is why `daw-cli do` needed --force.
        let mut write = unsafe { (*ring.header).write_index.load(Ordering::Relaxed) };
        let next = loop {
            let next = (write + 1) & ring.mask;
            if next == unsafe { (*ring.header).read_index.load(Ordering::Acquire) } {
                return Err("UI command ring is full (engine not draining)".to_string());
            }
            match unsafe {
                (*ring.header).write_index.compare_exchange_weak(
                    write,
                    next,
                    Ordering::AcqRel,
                    Ordering::Relaxed,
                )
            } {
                Ok(_) => break next,
                Err(actual) => write = actual,
            }
        };
        let _ = next;
        unsafe {
            // MASK AT THE POINT OF USE. `write` is the value this producer reserved, and it came
            // out of SHARED MEMORY — either the initial load or the `actual` returned by a failed
            // CAS. The loop masks `next`, the value stored BACK, and then this line indexed with
            // the unmasked `write`, so the mask never reached the arithmetic that dereferences.
            //
            // In a correct system that is harmless: both implementations only ever store a masked
            // value, so the index is in range by INVARIANT. But the invariant is a property of a
            // word another process writes, which is exactly the trust this file is being taught not
            // to extend. Out of range it is a 64-byte volatile write at an arbitrary distance from
            // the mapping — the harm AE-P1.3's descriptor validation is named for and could not
            // prevent, because validating the descriptor says nothing about the indices.
            //
            // `peek_ui_diffs` in this file already masks at the point of use; this makes the three
            // ring accessors agree.
            let slot = ring.entries.add((write & ring.mask) as usize);
            std::ptr::write_volatile(slot, entry);
            // Release: everything written above is visible to the engine before it can
            // observe ready == 1.
            let ready = &(*slot).ready as *const u32 as *const AtomicU32;
            (*ready).store(1, Ordering::Release);
        }
        Ok(())
    }
}

/// The identity this process stamps on the commands it sends, per P2-CMD-00 §3.
///
/// `hi` is a 32-bit nonce drawn ONCE for this process; `lo` is a counter within it starting at 1.
/// Together they are the 64-bit id the engine echoes back on a refusal, so a caller can tell which
/// of its commands was refused rather than adopting the outcome of somebody else's.
///
/// WHY A NONCE AND NOT A CLAIMED ID. The rings are MULTI-PRODUCER since M2.18 — a producer
/// CAS-reserves a slot, fills it, then sets `ready` — so an id must be unique across concurrent
/// producers AND across producer lifetimes, because the outbound ring is peeked rather than drained
/// and a restarted process's refusals are still visible. A counter alone fails the second test. The
/// exact alternative is a claim array in shared memory with CAS acquisition, which buys exactness
/// at the price of a wire change, an allocation authority and a stale-claim reclamation rule; the
/// nonce needs none of those. Ruled by the owner 2026-08-13.
///
/// THE BOUND, STATED RATHER THAN CALLED UNIQUE. Collision is birthday-on-2^32 across producer
/// LIFETIMES, not across commands: about 1e-6 at 100 lifetimes in a session and 1e-4 at 1000. When
/// it does collide, one client adopts another's refusal once and self-corrects on the next read.
///
/// The nonce comes from `RandomState`, which the standard library seeds randomly per process — no
/// dependency, and the value is stable for this process because it is drawn once.
fn command_id_next() -> u64 {
    use std::collections::hash_map::RandomState;
    use std::hash::{BuildHasher, Hasher};
    use std::sync::atomic::{AtomicU32, Ordering};

    static NONCE: std::sync::OnceLock<u32> = std::sync::OnceLock::new();
    static COUNTER: AtomicU32 = AtomicU32::new(0);

    let hi = *NONCE.get_or_init(|| {
        let mut h = RandomState::new().build_hasher();
        h.write_u64(std::process::id() as u64);
        let n = (h.finish() >> 32) as u32;
        // 0 is the "no id" sentinel for the WHOLE id, and hi == 0 with lo == 0 would collide with
        // it. Any non-zero lo keeps the id distinguishable, but borrowing 1 costs nothing and keeps
        // the invariant local to the nonce rather than spread across two fields.
        if n == 0 { 1 } else { n }
    });
    // Starts at 1: fetch_add returns the PREVIOUS value, so the first id has lo == 1 and an
    // all-zero id can only mean "this sender does not mint one".
    let lo = COUNTER.fetch_add(1, Ordering::Relaxed).wrapping_add(1);
    ((hi as u64) << 32) | (lo as u64)
}

/// Does a `size`-byte region of alignment `align` fit at `offset` in a mapping of `mapped` bytes?
///
/// AE-P1.3, and free rather than a method for one reason: as a method on EngineHandle it could only
/// be exercised by attaching to a live engine, so the malformed cases it exists to refuse could not
/// be posed at all. A guard that cannot be shown to reject anything is indistinguishable from no
/// guard, and this project has shipped that shape more than once.
///
/// Offset 0 is "the region is absent", which every caller already treated as absent.
///
/// THE SUBTRACTION IS THE POINT. `offset + size <= mapped` overflows and admits exactly the offset
/// it exists to refuse; the `mapped < size` test in front of it stops the subtraction underflowing.
fn region_fits(offset: usize, size: usize, align: usize, mapped: usize) -> bool {
    if offset == 0 || align == 0 {
        return false;
    }
    if offset % align != 0 {
        return false;
    }
    if mapped < size {
        return false;
    }
    offset <= mapped - size
}

/// A typed view of one ring, or None if the header does not describe one that FITS.
///
/// AE-P1.3. Every value this reads comes out of shared memory, and until this validated nothing but
/// `offset != 0` the sequence was: take an unbounded `offset` from the header, `base.add(offset)`,
/// and DEREFERENCE it to read `capacity` — an out-of-bounds read performed before the first check.
/// The capacity and entry-size tests that followed could not help; they ran on whatever those bytes
/// happened to be. The resulting `mask` is `capacity - 1`, so a plausible-looking capacity indexes
/// far past the mapping, and on a writable attach that is a write.
///
/// A corrupt header is not only an attack. A truncated file from an engine that died mid-setup, or
/// a stale segment from a different build whose magic and version happen to match, arrives here the
/// same way.
///
/// ORDER IS THE WHOLE POINT. Each check must pass before the read it protects:
///   1. the offset is non-zero and correctly aligned for RingHeader;
///   2. the HEADER fits inside the mapping — checked before any field of it is read;
///   3. only then are capacity and entry_size read, and validated;
///   4. the ENTRIES array fits too, computed with checked arithmetic so a huge capacity cannot
///      wrap the sum back into range.
///
/// `len` is the mapped length, which the caller already knows from the file's metadata.
fn ring_view(base: *mut u8, offset: u64, len: usize) -> Option<RingView> {
    if offset == 0 {
        return None;
    }
    let offset = usize::try_from(offset).ok()?;
    // THE ALIGNMENT CHECK IS ON THE OFFSET, SO IT ONLY WORKS IF THE BASE IS ALIGNED — entries land
    // at `base + offset + 64`, so `entries % 64 == base % 64`. In production `base` comes from mmap
    // and is page-aligned, which is why this holds; it is a precondition of the function and not
    // something the offset test establishes. Stated as an assertion rather than a comment because
    // the first test harness written for this function allocated `vec![0u8; n]`, which measured 32
    // mod 64, and therefore validated nothing while appearing to.
    debug_assert_eq!(
        base as usize % std::mem::align_of::<RingHeader>(),
        0,
        "ring_view assumes a mapping-aligned base; entries inherit the base's alignment"
    );
    if offset % std::mem::align_of::<RingHeader>() != 0 {
        return None;
    }
    // (2) The header must fit BEFORE it is read. `checked_add` because offset is attacker-shaped.
    if offset.checked_add(std::mem::size_of::<RingHeader>())? > len {
        return None;
    }
    let header = unsafe { base.add(offset) as *mut RingHeader };
    let capacity = unsafe { (*header).capacity };
    if capacity == 0 || (capacity & (capacity - 1)) != 0 {
        return None;
    }
    let entry_size = unsafe { (*header).entry_size } as usize;
    if entry_size != std::mem::size_of::<EventEntry>() {
        return None;
    }
    let entries_offset = (std::mem::size_of::<RingHeader>() + 63) & !63;
    // (4) ...and so must every entry the mask can reach. capacity is a u32 power of two, so the
    // product is computed in usize with checked arithmetic rather than trusted to be small.
    let entries_bytes = (capacity as usize).checked_mul(entry_size)?;
    let end = offset
        .checked_add(entries_offset)?
        .checked_add(entries_bytes)?;
    if end > len {
        return None;
    }
    let entries = header as *mut u8;
    Some(RingView {
        header,
        entries: unsafe { entries.add(entries_offset) as *mut EventEntry },
        mask: capacity - 1,
    })
}

#[cfg(test)]
mod ring_view_bounds_tests {
    use super::*;

    // AE-P1.3. Every field ring_view reads comes out of shared memory, so these build the header
    // BY HAND in a plain buffer and hand it offsets a corrupt or truncated segment would produce.
    //
    // WHAT EACH MUTATION ACTUALLY DOES TO THIS SUITE. All three were run; an earlier version of
    // this comment reported one of them and implied the set had been characterised, which an
    // independent review called out. The three are not equivalent and the difference is the
    // interesting part:
    //
    //   remove the ENTRIES-FIT arithmetic (step 4)  -> two clean assertion failures,
    //                                                  `entries_that_do_not_fit_are_refused` and
    //                                                  `a_capacity_larger_than_the_mapping_is_refused`.
    //                                                  These are one mechanism (`end > len`) tested
    //                                                  twice, not two independent ratchets.
    //   remove the HEADER-FITS check (step 2)       -> SIGSEGV. The binary dies.
    //   remove the alignment check                  -> no test notices (see below).
    //
    // STEP (2) CANNOT BE RATCHETED BY AN OUTCOME TEST, and that is a property of the design rather
    // than a gap in the suite. Any offset whose HEADER does not fit also has ENTRIES that do not
    // fit, so step (4) reaches the same verdict — after reading out of bounds to get there. Step
    // (2)'s job is not to change the answer; it is to make arriving at the answer safe. The only
    // observable difference is the segfault above, which is UB manifesting, and a test that asserts
    // on UB asserts on nothing. What establishes step (2) is reading the order.
    //
    // The remaining tests are OUTCOME PINS, not ratchets: they pass against weaker code too,
    // because these buffers are zeroed and `capacity == 0` was already refused. Worth having,
    // worth not overselling.

    const HDR: usize = std::mem::size_of::<RingHeader>();
    const ENTRIES_AT: usize = (HDR + 63) & !63;
    const ENTRY: usize = std::mem::size_of::<EventEntry>();

    /// A 64-ALIGNED zeroed buffer. `vec![0u8; n]` is not aligned for an `align(64)` type — measured
    /// at 32 mod 64 for the sizes used here — so the previous harness wrote through a misaligned
    /// `*mut RingHeader`, which is undefined behaviour inside a test module whose subject is memory
    /// safety. It also meant the tests did not model production, where `base` comes from mmap and
    /// is page-aligned. `ring_view` validates the OFFSET, so entries are aligned only if the BASE
    /// is; that precondition is now asserted in the function and honoured here.
    struct Aligned {
        ptr: *mut u8,
        layout: std::alloc::Layout,
    }
    impl Aligned {
        fn new(len: usize) -> Self {
            let layout = std::alloc::Layout::from_size_align(len.max(1), 64).unwrap();
            let ptr = unsafe { std::alloc::alloc_zeroed(layout) };
            assert!(!ptr.is_null());
            assert_eq!(ptr as usize % 64, 0);
            Self { ptr, layout }
        }
    }
    impl Drop for Aligned {
        fn drop(&mut self) {
            unsafe { std::alloc::dealloc(self.ptr, self.layout) };
        }
    }

    /// A 64-aligned buffer with a well-formed ring header at `offset`.
    fn buffer_with_ring(offset: usize, capacity: u32, len: usize) -> Aligned {
        let buf = Aligned::new(len);
        let hdr = unsafe { buf.ptr.add(offset) as *mut RingHeader };
        unsafe {
            (*hdr).capacity = capacity;
            (*hdr).entry_size = ENTRY as u32;
        }
        buf
    }

    fn view(buf: &Aligned, offset: u64, len: usize) -> bool {
        ring_view(buf.ptr, offset, len).is_some()
    }

    #[test]
    fn a_well_formed_ring_is_accepted() {
        let cap = 4u32;
        let len = 64 + ENTRIES_AT + cap as usize * ENTRY;
        let buf = buffer_with_ring(64, cap, len);
        assert!(view(&buf, 64, len), "a ring that fits must be usable");
    }

    #[test]
    fn an_offset_past_the_mapping_is_refused() {
        let buf = buffer_with_ring(64, 4, 4096);
        // The shape a corrupt header produces: a plausible number that is simply outside.
        assert!(!view(&buf, 1 << 30, 4096));
        assert!(!view(&buf, u64::MAX, 4096));
    }

    #[test]
    fn a_header_straddling_the_end_is_refused() {
        // THIS TEST WAS VACUOUS AND IS THE REASON TO WRITE THE NUMBERS DOWN. It used
        // `((4096 - HDR/2) & !63)`, which rounds DOWN to 4032 — and 4032 + 64 == 4096 == len, so
        // the header fitted exactly and was refused by the pre-existing `capacity == 0` rule while
        // its comment claimed it straddled the end. An independent review measured it.
        //
        // A straddle needs a mapping whose length is NOT a multiple of 64, because the offset must
        // be 64-aligned to get past the alignment gate at all. 3968 + 64 = 4032 > 4000.
        //
        // This is the ONLY test that covers step (2), the header-fits check — the centrepiece of
        // this function's ordering argument. Before this rewrite, deleting that check left seven of
        // eight tests green.
        let len = 4000;
        let straddle = 3968u64;
        assert_eq!(straddle % 64, 0, "must pass the alignment gate to reach the fits check");
        assert!(straddle as usize + HDR > len, "the fixture must actually straddle");
        let buf = buffer_with_ring(64, 4, len);
        assert!(!view(&buf, straddle, len));
    }

    #[test]
    fn a_misaligned_offset_is_refused() {
        let buf = buffer_with_ring(64, 4, 4096);
        assert!(!view(&buf, 65, 4096), "RingHeader is align(64)");
    }

    #[test]
    fn entries_that_do_not_fit_are_refused() {
        // The header fits and every field is individually plausible; only the ENTRIES run past the
        // end. This is the case the pre-AE-P1.3 code could not see at all: it validated the
        // capacity's shape and never asked whether the array it describes exists.
        let cap = 1024u32;
        let len = 64 + ENTRIES_AT + 8 * ENTRY;
        let buf = buffer_with_ring(64, cap, 64 + ENTRIES_AT + 8 * ENTRY);
        assert!(!view(&buf, 64, len));
    }

    #[test]
    fn a_capacity_larger_than_the_mapping_is_refused() {
        // RENAMED. This was `a_capacity_that_would_overflow_the_sum_is_refused`, and it overflows
        // nothing: 2^31 * 64 = 2^37, measured. It is refused by `end > len` — the same branch the
        // previous test exercises — so the two are one mechanism tested twice, not two ratchets.
        //
        // The `checked_mul` in the function is UNREACHABLE by construction and stays as defence
        // rather than coverage: `entry_size` is pinned to exactly 64 two lines above it and
        // `capacity` is a u32, so the product is bounded by 2^38. The `checked_add` is reachable
        // only at an offset within 64 bytes of usize::MAX, which is also untested. Saying so beats
        // a name that promises a branch no test enters.
        let cap = 1u32 << 31;
        let len = 4096;
        let buf = buffer_with_ring(64, cap, len);
        assert!(!view(&buf, 64, len));
    }

    #[test]
    fn a_non_power_of_two_capacity_is_still_refused() {
        // Pre-existing rule, pinned here so the reordering above cannot have dropped it: mask
        // arithmetic requires a power of two.
        let len = 4096;
        let buf = buffer_with_ring(64, 3, len);
        assert!(!view(&buf, 64, len));
    }

    #[test]
    fn a_wrong_entry_size_is_still_refused() {
        let len = 4096;
        let buf = buffer_with_ring(64, 4, len);
        let hdr = unsafe { buf.ptr.add(64) as *mut RingHeader };
        unsafe { (*hdr).entry_size = (ENTRY as u32) + 1 };
        assert!(!view(&buf, 64, len));
    }
}

#[cfg(test)]
mod region_fits_tests {
    use super::region_fits;

    // AE-P1.3. These pose the malformed descriptors a corrupt or truncated segment produces, which
    // the accessors themselves cannot be asked about without attaching to a live engine.
    //
    // WHICH OF THESE RATCHET: all FIVE conditions are covered, one test each, and this is the
    // measured result of deleting each in turn rather than a claim about the suite. It said FOUR
    // until a reviewer counted them: the first clause is a disjunction, `offset == 0 || align == 0`,
    // and deleting the second half left the suite green. Counting a disjunction as one condition is
    // how a covered-looking predicate keeps an untested branch. The
    // ring-view suite next door is weaker precisely because a pre-existing rule refused its zeroed
    // fixtures by accident; region_fits is the only thing deciding here.
    //
    //   remove `offset == 0`      -> absent_is_not_a_region
    //   remove `align == 0`       -> a_zero_alignment_is_refused
    //   remove the alignment test -> a_misaligned_offset_is_refused
    //   remove `mapped < size`    -> a_mapping_smaller_than_the_region_is_refused, and note it
    //                                fails by PANICKING inside region_fits — the subtraction
    //                                underflows, which is what that clause exists to prevent
    //   remove the bounds test    -> one_byte_past_the_end_is_refused AND
    //                                an_offset_near_the_top_of_the_range_cannot_wrap_into_bounds

    #[test]
    fn a_region_that_fits_is_accepted() {
        assert!(region_fits(64, 128, 64, 4096));
        // Exactly flush with the end is legal: the last byte is mapped.
        assert!(region_fits(4096 - 128, 128, 64, 4096));
    }

    #[test]
    fn absent_is_not_a_region() {
        // Offset 0 means "the engine has not published this region", which every caller already
        // treated as absent. It is not a bounds failure and must not be reported as one.
        assert!(!region_fits(0, 128, 64, 4096));
    }

    #[test]
    fn one_byte_past_the_end_is_refused() {
        // The off-by-one in the dangerous direction. If this passed, the last byte of the region
        // would be the first byte after the mapping.
        assert!(!region_fits(4096 - 128 + 64, 128, 64, 4096));
    }

    #[test]
    fn a_zero_alignment_is_refused() {
        // THE FIFTH CONDITION THE COMMIT MESSAGE CALLED FOUR. `offset == 0 || align == 0` is a
        // disjunction, and deleting only the second half left this suite fully green — so the
        // claim "all four clauses are covered" described five conditions as four and one of them
        // was untested.
        //
        // `region` only ever passes `align_of::<T>()`, which is never 0, so this is unreachable
        // from the two callers. It is covered anyway rather than deleted because region_fits is a
        // FREE function whose entire justification is that malformed inputs can be posed to it —
        // an argument that does not survive leaving one of its own inputs unexercised. It also
        // stops `%` dividing by zero if a future caller computes an alignment.
        assert!(!region_fits(64, 128, 0, 4096));
    }

    #[test]
    fn a_misaligned_offset_is_refused() {
        assert!(!region_fits(65, 128, 64, 4096));
        // ...and alignment is judged against the type's requirement, not a constant.
        assert!(region_fits(4, 8, 4, 4096));
        assert!(!region_fits(4, 8, 8, 4096));
    }

    #[test]
    fn a_mapping_smaller_than_the_region_is_refused() {
        // The case that makes the subtraction safe: without the `mapped < size` test the next line
        // would underflow to a huge bound and admit everything.
        assert!(!region_fits(64, 4096, 64, 128));
    }

    #[test]
    fn an_offset_near_the_top_of_the_range_cannot_wrap_into_bounds() {
        // Written as `offset + size <= mapped` this is the input that wraps and passes. The
        // subtraction cannot, which is why the check is phrased that way.
        assert!(!region_fits(usize::MAX - 63, 128, 64, 4096));
    }
}

#[cfg(test)]
mod region_fits_property_tests {
    use super::region_fits;

    // AE-P1.3's gate is worded "fuzz and property tests cannot construct an out-of-bounds or
    // misaligned typed view". The suite beside this one is example-based: it pins outcomes for
    // inputs I thought of. This asserts the CONTRACT instead — for any input at all, saying yes
    // implies the region genuinely lies inside the mapping and is correctly aligned.
    //
    // That is the difference between "the cases I chose are refused" and "nothing that passes can
    // be out of bounds", and only the second is what the gate asks for. It is also the direction
    // that survives someone rewriting the predicate: the examples would need updating, this would
    // not.
    //
    // Deterministic rather than randomised — a fixed LCG plus the edge values that actually break
    // this kind of arithmetic. A test that draws different inputs each run reports a different
    // fact each run, and this project has spent a long time on claims that were true by luck.

    fn interesting() -> Vec<usize> {
        let mut v = vec![
            0, 1, 2, 7, 8, 63, 64, 65, 127, 128, 4095, 4096, 4097,
            usize::MAX, usize::MAX - 1, usize::MAX - 63, usize::MAX - 64,
            usize::MAX / 2, usize::MAX / 2 + 1,
        ];
        // ...and a spread of ordinary values, so the edges are not the only shapes exercised.
        let mut lcg: u64 = 0x2545_F491_4F6C_DD1D;
        // Kept small on purpose: the first draft used 400 rounds and the test took 29 SECONDS,
        // which is a tax on every run of the suite for coverage the edge values already give. The
        // accepted-count assertion below is what guards against trimming it into vacuity.
        for _ in 0..24 {
            lcg = lcg.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
            v.push((lcg >> 11) as usize);
            v.push(((lcg >> 11) % 8192) as usize);
        }
        v
    }

    #[test]
    fn nothing_that_passes_can_be_out_of_bounds_or_misaligned() {
        let vals = interesting();
        let aligns = [0usize, 1, 2, 4, 8, 16, 32, 64];
        let mut accepted = 0u64;
        for &offset in &vals {
            for &size in &vals {
                for &align in &aligns {
                    for &mapped in &vals {
                        if !region_fits(offset, size, align, mapped) {
                            continue;
                        }
                        accepted += 1;
                        // The four things a caller is entitled to assume when this says yes.
                        assert_ne!(align, 0, "a zero alignment must never be accepted");
                        assert_ne!(offset, 0, "offset 0 means absent, never a valid region");
                        assert_eq!(offset % align, 0, "accepted a misaligned offset");
                        // The one that matters: the region lies inside the mapping, computed in a
                        // form that CANNOT itself overflow — `offset + size` is exactly the
                        // expression the predicate avoids, so asserting with it here would be
                        // asserting the bug.
                        assert!(
                            offset <= mapped - size,
                            "accepted offset {offset} size {size} in mapping {mapped}"
                        );
                        assert!(mapped >= size);
                    }
                }
            }
        }
        // A PREDICATE THAT ALWAYS SAYS NO SATISFIES EVERY ASSERTION ABOVE. Without this the whole
        // test is vacuous, which is the exact shape of two controls this project has already had
        // to correct.
        assert!(
            accepted > 1000,
            "only {accepted} inputs were accepted; the test proved nothing about a predicate that \
             refuses everything"
        );
    }
}

#[cfg(test)]
mod command_id_tests {
    use super::command_id_next;

    // P2-CMD-00 §3. The id is a per-process nonce in the high word and a counter in the low word.
    // These pin the three properties a correlator depends on; the collision BOUND is stated in the
    // function's own comment and is not testable here, because it is a property of many processes.
    #[test]
    fn ids_are_distinct_non_zero_and_share_one_nonce() {
        let a = command_id_next();
        let b = command_id_next();
        let c = command_id_next();

        // NON-ZERO IS LOAD-BEARING: all-zero is the documented "no id" sentinel, so a minted id
        // colliding with it would report as un-correlated and never match.
        assert_ne!(a, 0);
        assert_ne!(b, 0);

        // Distinct, and monotonic within the process — the low word is what separates two commands
        // from the same sender.
        assert_ne!(a, b);
        assert_ne!(b, c);
        assert_eq!((b & 0xFFFF_FFFF) , (a & 0xFFFF_FFFF) + 1);

        // ONE NONCE FOR THE PROCESS. If the high word moved per call it would still be unique, and
        // it would stop identifying the SENDER — which is the half that survives a restart, and the
        // reason a bare counter was refused.
        assert_eq!(a >> 32, b >> 32);
        assert_eq!(b >> 32, c >> 32);
        assert_ne!(a >> 32, 0, "a zero nonce would make the first id collide with the sentinel");
    }
}
