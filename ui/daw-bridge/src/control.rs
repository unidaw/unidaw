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

use crate::layout::{
    EventEntry, EventType, RingHeader, ShmHeader, UiChordCommandPayload,
    UiClipExtent, UiClipExtentRegion, UiClipWindowCommandPayload, UiClipWindowSnapshot,
    UiCommandPayload, UiHarmonyEvent, UiHarmonySnapshot, UiPatcherEdge, UiPatcherNode,
    UiDeviceParamsRegion, UiPatcherRegion, UiScaleRegion, K_SHM_MAGIC, K_SHM_VERSION,
    K_UI_MAX_CLIP_EXTENTS, K_UI_MAX_DEVICE_PARAMS, K_UI_MAX_HARMONY_EVENTS,
    K_UI_MAX_PATCHER_EDGES, K_UI_MAX_PATCHER_NODES, K_UI_MAX_SCALES, K_UI_MAX_SCALE_STEPS,
    K_UI_MAX_TRACKS,
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
        let n = (unsafe { (*region).scale_count } as usize).min(K_UI_MAX_SCALES);
        let mut out = Vec::with_capacity(n);
        for i in 0..n {
            let s = unsafe { &(*region).scales[i] };
            let steps = (s.step_count as usize).min(K_UI_MAX_SCALE_STEPS);
            let end = s.name.iter().position(|&b| b == 0).unwrap_or(s.name.len());
            out.push(ScaleView {
                id: s.id,
                name: String::from_utf8_lossy(&s.name[..end]).into_owned(),
                octave_cents: s.octave_milli_cents as f64 / 1000.0,
                step_cents: (0..steps)
                    .map(|k| s.step_milli_cents[k] as f64 / 1000.0)
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
        let cstr = |b: &[u8]| {
            let end = b.iter().position(|&c| c == 0).unwrap_or(b.len());
            String::from_utf8_lossy(&b[..end]).into_owned()
        };
        let n = (unsafe { (*region).param_count } as usize).min(K_UI_MAX_DEVICE_PARAMS);
        let mut view = DeviceParamsView {
            version: unsafe { (*region).version },
            track_id: unsafe { (*region).track_id },
            device_id: unsafe { (*region).device_id },
            device_name: cstr(unsafe { &(*region).device_name }),
            params: Vec::with_capacity(n),
        };
        for i in 0..n {
            let p = unsafe { &(*region).params[i] };
            view.params.push(DeviceParamView {
                index: p.index,
                value: p.value_milli as f32 / 1000.0,
                uid16: p.uid16,
                name: cstr(&p.name),
                display: cstr(&p.display),
            });
        }
        view
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
        };
        let bytes = unsafe { std::slice::from_raw_parts(payload, size) };
        entry.payload[..bytes.len()].copy_from_slice(bytes);

        let write = unsafe { (*ring.header).write_index.load(Ordering::Relaxed) };
        let read = unsafe { (*ring.header).read_index.load(Ordering::Acquire) };
        let next = (write + 1) & ring.mask;
        if next == read {
            return Err("UI command ring is full (engine not draining)".to_string());
        }
        unsafe {
            *ring.entries.add(write as usize) = entry;
            (*ring.header).write_index.store(next, Ordering::Release);
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
