//! Attaching to a running engine's shared memory to query it, and to send it
//! the same `UiCommand` payloads the UI sends.
//!
//! Single-producer constraint: the UI command ring is SPSC. The engine is the
//! only consumer, and exactly one process may be the producer. While the UI app
//! is running it *is* that producer, so a second writer would corrupt the ring.
//! Reading is always safe; writing is the caller's responsibility to serialise.

use std::ffi::CString;
use std::fs::File;
use std::os::fd::FromRawFd;
use std::sync::atomic::Ordering;

use memmap2::{Mmap, MmapMut, MmapOptions};

use std::sync::atomic::fence;
use std::sync::atomic::AtomicU32;

use crate::layout::{
    EventEntry, EventType, RingHeader, ShmHeader, UiChordCommandPayload,
    UiClipExtent, UiClipExtentRegion, UiClipWindowCommandPayload, UiClipWindowSnapshot,
    UiCommandPayload, UiHarmonyEvent, UiHarmonySnapshot, UiPatcherEdge, UiPatcherNode,
    UiDeviceParamsRegion, UiPatcherRegion, UiScaleRegion, K_SHM_MAGIC, K_SHM_VERSION,
    K_UI_MAX_CLIP_EXTENTS, K_UI_MAX_DEVICE_PARAMS, K_UI_MAX_HARMONY_EVENTS,
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
    pub active_voices: u32,
    /// Telemetry. A voice pool running out is a musical fact, not a secret.
    pub steals: u32,
    /// Notes that hit no slot — a kit that is silent everywhere is diagnosable from this.
    pub unmapped: u32,
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
    ring_ui: Option<RingView>,
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
            ring_view(mmap.as_ptr() as *mut u8, ring_offset)
        } else {
            None
        };
        Ok(Self {
            _mmap: mmap,
            header,
            ring_ui,
        })
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
        loop {
            let v0 = unsafe { (*self.header).ui_version.load(Ordering::Acquire) };
            if v0 % 2 == 1 {
                continue;
            }
            let offset = unsafe { (*self.header).ui_clip_offset };
            let bytes = unsafe { (*self.header).ui_clip_bytes };
            if offset == 0 || bytes < std::mem::size_of::<UiClipWindowSnapshot>() as u64 {
                return None;
            }
            let snapshot = unsafe {
                *(self._mmap.as_ptr().add(offset as usize) as *const UiClipWindowSnapshot)
            };
            fence(Ordering::Acquire);
            let v1 = unsafe { (*self.header).ui_version.load(Ordering::Acquire) };
            if v0 == v1 && v0 % 2 == 0 {
                return Some(snapshot);
            }
        }
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
        loop {
            let v0 = unsafe { (*self.header).ui_version.load(Ordering::Acquire) };
            if v0 % 2 == 1 {
                continue;
            }
            let offset = unsafe { (*self.header).ui_clip_all_offset };
            let bytes = unsafe { (*self.header).ui_clip_all_bytes };
            if offset == 0 || (bytes as usize) < stride * K_UI_MAX_TRACKS {
                return None;
            }
            let base = self._mmap.as_ptr().wrapping_add(offset as usize)
                as *const UiClipWindowSnapshot;
            let snapshot = unsafe { *base.add(track_id as usize) };
            fence(Ordering::Acquire);
            let v1 = unsafe { (*self.header).ui_version.load(Ordering::Acquire) };
            if v0 == v1 && v0 % 2 == 0 {
                return Some(snapshot);
            }
        }
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
        let base = self._mmap.as_ptr().wrapping_add(offset as usize)
            as *const crate::layout::UiArrangeSummaryRegion;
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
        loop {
            let v0 = unsafe { (*self.header).ui_version.load(Ordering::Acquire) };
            if v0 % 2 == 1 {
                continue;
            }
            let offset = unsafe { (*self.header).ui_clip_extent_offset };
            if offset == 0 {
                return (Vec::new(), 0);
            }
            let region = self._mmap.as_ptr().wrapping_add(offset as usize)
                as *const UiClipExtentRegion;
            let count = unsafe { (*region).count as usize }.min(K_UI_MAX_CLIP_EXTENTS);
            let truncated = unsafe { (*region).truncated };
            let mut out = Vec::with_capacity(count);
            for i in 0..count {
                out.push(unsafe { (*region).extents[i] });
            }
            fence(Ordering::Acquire);
            let v1 = unsafe { (*self.header).ui_version.load(Ordering::Acquire) };
            if v0 == v1 && v0 % 2 == 0 {
                return (out, truncated);
            }
        }
    }

    /// The published harmony timeline (root/scale changes over time), under the
    /// seqlock. Populated from the project's harmony_timeline on load.
    pub fn read_harmony(&self) -> Vec<UiHarmonyEvent> {
        loop {
            let v0 = unsafe { (*self.header).ui_version.load(Ordering::Acquire) };
            if v0 % 2 == 1 {
                continue;
            }
            let off = unsafe { (*self.header).ui_harmony_offset };
            if off == 0 {
                return Vec::new();
            }
            let snap =
                self._mmap.as_ptr().wrapping_add(off as usize) as *const UiHarmonySnapshot;
            let count =
                (unsafe { (*snap).event_count } as usize).min(K_UI_MAX_HARMONY_EVENTS);
            let mut out = Vec::with_capacity(count);
            for i in 0..count {
                out.push(unsafe { (*snap).events[i] });
            }
            fence(Ordering::Acquire);
            let v1 = unsafe { (*self.header).ui_version.load(Ordering::Acquire) };
            if v0 == v1 && v0 % 2 == 0 {
                return out;
            }
        }
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
        loop {
            let v0 = unsafe { (*self.header).ui_version.load(Ordering::Acquire) };
            if v0 % 2 == 1 {
                continue;
            }
            let count =
                unsafe { std::ptr::read_volatile(&(*self.header).ui_track_count) } as usize;
            let count = count.min(K_UI_MAX_TRACKS);
            let mut out = Vec::with_capacity(count);
            for i in 0..count {
                out.push(TrackMixer {
                    gain_millibels: unsafe { (*self.header).ui_track_gain_millibels[i] },
                    pan_thousandths: unsafe { (*self.header).ui_track_pan_thousandths[i] },
                    flags: unsafe { (*self.header).ui_track_mix_flags[i] },
                });
            }
            fence(Ordering::Acquire);
            let v1 = unsafe { (*self.header).ui_version.load(Ordering::Acquire) };
            if v0 == v1 && v0 % 2 == 0 {
                return out;
            }
        }
    }

    /// The published patcher-version counter (moves on any patcher edit).
    pub fn patcher_version(&self) -> u32 {
        let off = unsafe { (*self.header).ui_patcher_offset };
        if off == 0 {
            return 0;
        }
        let region = self._mmap.as_ptr().wrapping_add(off as usize) as *const UiPatcherRegion;
        unsafe { std::ptr::read_volatile(&(*region).version) }
    }

    /// The published patcher graph the engine runs (one global graph today),
    /// under the seqlock. `device_id` is the device it's parked on (0 = default).
    pub fn read_patcher(&self) -> PatcherView {
        loop {
            let v0 = unsafe { (*self.header).ui_version.load(Ordering::Acquire) };
            if v0 % 2 == 1 {
                continue;
            }
            let off = unsafe { (*self.header).ui_patcher_offset };
            if off == 0 {
                return PatcherView::default();
            }
            let region =
                self._mmap.as_ptr().wrapping_add(off as usize) as *const UiPatcherRegion;
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
            fence(Ordering::Acquire);
            let v1 = unsafe { (*self.header).ui_version.load(Ordering::Acquire) };
            if v0 == v1 && v0 % 2 == 0 {
                return view;
            }
        }
    }

    /// The engine's scale registry (v16). Written once at startup and immutable,
    /// so this is a plain read — no seqlock needed. Empty if not published.
    pub fn read_scales(&self) -> Vec<ScaleView> {
        let off = unsafe { (*self.header).ui_scales_offset };
        if off == 0 {
            return Vec::new();
        }
        let region =
            self._mmap.as_ptr().wrapping_add(off as usize) as *const UiScaleRegion;
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
        let region = self._mmap.as_ptr().wrapping_add(off as usize)
            as *const UiDeviceParamsRegion;
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
        let region = self._mmap.as_ptr().wrapping_add(off as usize)
            as *const UiAudioSourceRegion;
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
        let region =
            self._mmap.as_ptr().wrapping_add(off as usize) as *const UiWaveformRegion;
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
        let region = self._mmap.as_ptr().wrapping_add(off as usize)
            as *const UiAutomationLaneRegion;
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
        let region = self._mmap.as_ptr().wrapping_add(off as usize)
            as *const UiAutomationSlotRegion;
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

    /// Per-track display names for the current track count (nul-trimmed).
    pub fn read_track_names(&self) -> Vec<String> {
        loop {
            let v0 = unsafe { (*self.header).ui_version.load(Ordering::Acquire) };
            if v0 % 2 == 1 {
                continue;
            }
            let count =
                unsafe { std::ptr::read_volatile(&(*self.header).ui_track_count) } as usize;
            let count = count.min(K_UI_MAX_TRACKS);
            let mut out = Vec::with_capacity(count);
            for i in 0..count {
                let raw = unsafe { &(*self.header).ui_track_name[i] };
                let end = raw.iter().position(|&b| b == 0).unwrap_or(raw.len());
                out.push(String::from_utf8_lossy(&raw[..end]).into_owned());
            }
            fence(Ordering::Acquire);
            let v1 = unsafe { (*self.header).ui_version.load(Ordering::Acquire) };
            if v0 == v1 && v0 % 2 == 0 {
                return out;
            }
        }
    }

    /// v23: the first instrument's name per track (empty when the track has none).
    pub fn read_track_device_names(&self) -> Vec<String> {
        loop {
            let v0 = unsafe { (*self.header).ui_version.load(Ordering::Acquire) };
            if v0 % 2 == 1 {
                continue;
            }
            let count =
                unsafe { std::ptr::read_volatile(&(*self.header).ui_track_count) } as usize;
            let count = count.min(K_UI_MAX_TRACKS);
            let mut out = Vec::with_capacity(count);
            for i in 0..count {
                let raw = unsafe { &(*self.header).ui_track_device_name[i] };
                let end = raw.iter().position(|&b| b == 0).unwrap_or(raw.len());
                out.push(String::from_utf8_lossy(&raw[..end]).into_owned());
            }
            fence(Ordering::Acquire);
            let v1 = unsafe { (*self.header).ui_version.load(Ordering::Acquire) };
            if v0 == v1 && v0 % 2 == 0 {
                return out;
            }
        }
    }

    /// Per-track stable ids (uiTrackId) + flags (UI_TRACK_FLAG_*), read together under
    /// the seqlock so a caller can key on the id (never the moving slot) and tell the
    /// master / absent / child entries apart. Returns (ids, flags), both `track_count` long.
    pub fn read_track_ids_and_flags(&self) -> (Vec<u32>, Vec<u8>) {
        loop {
            let v0 = unsafe { (*self.header).ui_version.load(Ordering::Acquire) };
            if v0 % 2 == 1 {
                continue;
            }
            let count =
                unsafe { std::ptr::read_volatile(&(*self.header).ui_track_count) } as usize;
            let count = count.min(K_UI_MAX_TRACKS);
            let mut ids = Vec::with_capacity(count);
            let mut flags = Vec::with_capacity(count);
            for i in 0..count {
                ids.push(unsafe { std::ptr::read_volatile(&(*self.header).ui_track_id[i]) });
                flags.push(unsafe { std::ptr::read_volatile(&(*self.header).ui_track_flags[i]) });
            }
            fence(Ordering::Acquire);
            let v1 = unsafe { (*self.header).ui_version.load(Ordering::Acquire) };
            if v0 == v1 && v0 % 2 == 0 {
                return (ids, flags);
            }
        }
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
        let region = unsafe {
            &*((self._mmap.as_ptr()).add(offset as usize) as *const UiDeviceMeterRegion)
        };
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
    pub fn read_sampler_kit_slot(&self, index: usize) -> Option<SamplerKitView> {
        if index >= crate::layout::UI_SAMPLER_KIT_SLOTS {
            return None;
        }
        let off = unsafe { (*self.header).ui_sampler_kit_offset };
        if off == 0 {
            return None;
        }
        let region = self._mmap.as_ptr().wrapping_add(off as usize)
            as *const crate::layout::UiSamplerKitRegion;
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
                    active_voices: snap.activeVoices,
                    steals: snap.steals,
                    unmapped: snap.unmapped,
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

    /// Send a track-routing replace (SetTrackRouting). Same ring as send_command; a
    /// distinct payload shape.
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
            sample_time: 0,
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
            let slot = ring.entries.add(write as usize);
            std::ptr::write_volatile(slot, entry);
            // Release: everything written above is visible to the engine before it can
            // observe ready == 1.
            let ready = &(*slot).ready as *const u32 as *const AtomicU32;
            (*ready).store(1, Ordering::Release);
        }
        Ok(())
    }
}

fn ring_view(base: *mut u8, offset: u64) -> Option<RingView> {
    if offset == 0 {
        return None;
    }
    let header = unsafe { base.add(offset as usize) as *mut RingHeader };
    let capacity = unsafe { (*header).capacity };
    if capacity == 0 || (capacity & (capacity - 1)) != 0 {
        return None;
    }
    let entry_size = unsafe { (*header).entry_size } as usize;
    if entry_size != std::mem::size_of::<EventEntry>() {
        return None;
    }
    let entries_offset = (std::mem::size_of::<RingHeader>() + 63) & !63;
    let entries = header as *mut u8;
    Some(RingView {
        header,
        entries: unsafe { entries.add(entries_offset) as *mut EventEntry },
        mask: capacity - 1,
    })
}
