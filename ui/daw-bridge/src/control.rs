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
    EventEntry, EventType, RingHeader, ShmHeader, UiChainCommandPayload, UiChordCommandPayload,
    UiClipExtent, UiClipExtentRegion, UiClipWindowCommandPayload, UiClipWindowSnapshot,
    UiCommandPayload, UiHarmonyEvent, UiHarmonySnapshot, UiPatcherEdge, UiPatcherNode,
    UiDeviceParamsRegion, UiPatcherGraphCommandPayload, UiPatcherNodeConfigPayload,
    UiPatcherRegion, UiScaleRegion, K_SHM_MAGIC, K_SHM_VERSION, K_UI_MAX_CLIP_EXTENTS,
    K_UI_MAX_DEVICE_PARAMS, K_UI_MAX_HARMONY_EVENTS,
    K_UI_MAX_PATCHER_EDGES, K_UI_MAX_PATCHER_NODES, K_UI_MAX_SCALES, K_UI_MAX_SCALE_STEPS,
    K_UI_MAX_TRACKS, UiAudioSourceRegion, UiWaveformRegion, UiWaveformRequestPayload,
    K_UI_MAX_AUDIO_CLIPS, K_UI_MAX_AUDIO_SOURCES, K_UI_WAVEFORM_MAX_PAIRS, K_UI_WAVEFORM_SLOTS,
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
    /// The engine's OUT ring, which this side consumes. Only present on a
    /// writable handle: a consumer advances read_index, so draining is a write.
    ring_out: Option<RingView>,
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
        let ring_out = if writable {
            ring_view(mmap.as_ptr() as *mut u8, unsafe { (*header).ring_ui_out_offset })
        } else {
            None
        };
        Ok(Self {
            _mmap: mmap,
            header,
            ring_ui,
            ring_out,
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
    /// the song end. Read under the region's OWN version rather than the seqlock: the
    /// engine writes the whole region and then stores `version` behind a release fence, so
    /// reading version-body-version and requiring the two to match is what makes a torn
    /// read impossible. Returns None while a write is in flight or the region is absent.
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
        loop {
            let v0 = unsafe { (*self.header).ui_version.load(Ordering::Acquire) };
            if v0 % 2 == 1 {
                continue;
            }
            let offset = unsafe { (*self.header).ui_clip_extent_offset };
            if offset == 0 {
                return Vec::new();
            }
            let region = self._mmap.as_ptr().wrapping_add(offset as usize)
                as *const UiClipExtentRegion;
            let count = unsafe { (*region).count as usize }.min(K_UI_MAX_CLIP_EXTENTS);
            let mut out = Vec::with_capacity(count);
            for i in 0..count {
                out.push(unsafe { (*region).extents[i] });
            }
            fence(Ordering::Acquire);
            let v1 = unsafe { (*self.header).ui_version.load(Ordering::Acquire) };
            if v0 == v1 && v0 % 2 == 0 {
                return out;
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
            fence(Ordering::Acquire);
            let v1 = unsafe { (*self.header).ui_version.load(Ordering::Acquire) };
            if v0 == v1 && v0 % 2 == 0 {
                return (out, version);
            }
        }
    }

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
        let Some(ring) = self.ring_out.as_ref() else { return 0 };
        let mut read = unsafe { (*ring.header).read_index.load(Ordering::Relaxed) };
        let write = unsafe { (*ring.header).write_index.load(Ordering::Acquire) };
        let mut n = 0;
        while read != write && n < max {
            out.push(unsafe { *ring.entries.add(read as usize) });
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

    /// Send a section command (add/remove/rename/set-length/move).
    pub fn send_section_command(
        &self,
        payload: crate::layout::UiSectionCommandPayload,
    ) -> Result<(), String> {
        self.write_entry(
            &payload as *const crate::layout::UiSectionCommandPayload as *const u8,
            std::mem::size_of::<crate::layout::UiSectionCommandPayload>(),
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
