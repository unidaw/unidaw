use std::ffi::CString;
use std::os::unix::io::FromRawFd;
use std::sync::atomic::{AtomicU64, Ordering, fence};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use anyhow::{Context as AnyhowContext, Result};
use memmap2::{MmapMut, MmapOptions};

use daw_bridge::layout::{
    EventEntry, EventType, RingHeader, ShmHeader, UiClipSnapshot, UiCommandPayload,
    UiChordCommandPayload, UiChordDiffPayload, UiDiffPayload, UiHarmonyDiffPayload,
    UiHarmonySnapshot,
};
use daw_bridge::reader::{SeqlockReader, UiSnapshot};

static LAST_UI_CMD: AtomicU64 = AtomicU64::new(0);
static LAST_UI_CMD_TIME_MS: AtomicU64 = AtomicU64::new(0);
static UI_CMD_ENQUEUED: AtomicU64 = AtomicU64::new(0);
static UI_CMD_SENT: AtomicU64 = AtomicU64::new(0);
static UI_CMD_SEND_FAIL: AtomicU64 = AtomicU64::new(0);
static UI_CMD_SEND_FAIL_LOG_MS: AtomicU64 = AtomicU64::new(0);

pub(crate) fn bump_ui_enqueued() {
    UI_CMD_ENQUEUED.fetch_add(1, Ordering::Relaxed);
}

pub(crate) fn bump_ui_sent() {
    UI_CMD_SENT.fetch_add(1, Ordering::Relaxed);
}

pub(crate) fn bump_ui_send_fail() {
    UI_CMD_SEND_FAIL.fetch_add(1, Ordering::Relaxed);
}

pub(crate) fn log_ui_send_fail() {
    let now_ms = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0);
    let last = UI_CMD_SEND_FAIL_LOG_MS.load(Ordering::Relaxed);
    if now_ms.saturating_sub(last) >= 1000 {
        UI_CMD_SEND_FAIL_LOG_MS.store(now_ms, Ordering::Relaxed);
        let enqueued = UI_CMD_ENQUEUED.load(Ordering::Relaxed);
        let sent = UI_CMD_SENT.load(Ordering::Relaxed);
        let failed = UI_CMD_SEND_FAIL.load(Ordering::Relaxed);
        eprintln!(
            "daw-app: UI command ring saturated (enqueued {}, sent {}, send_fail {})",
            enqueued, sent, failed
        );
    }
}

#[allow(dead_code)]
pub fn reset_ui_counters() {
    UI_CMD_ENQUEUED.store(0, Ordering::Relaxed);
    UI_CMD_SENT.store(0, Ordering::Relaxed);
    UI_CMD_SEND_FAIL.store(0, Ordering::Relaxed);
    UI_CMD_SEND_FAIL_LOG_MS.store(0, Ordering::Relaxed);
}

#[allow(dead_code)]
pub fn ui_cmd_counters() -> (u64, u64, u64) {
    (
        UI_CMD_ENQUEUED.load(Ordering::Relaxed),
        UI_CMD_SENT.load(Ordering::Relaxed),
        UI_CMD_SEND_FAIL.load(Ordering::Relaxed),
    )
}

fn record_ui_command(command_type: u16, track_id: u32) {
    let packed = (command_type as u64) | ((track_id as u64) << 32);
    LAST_UI_CMD.store(packed, Ordering::Relaxed);
    let now_ms = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0);
    LAST_UI_CMD_TIME_MS.store(now_ms, Ordering::Relaxed);
}

pub fn log_last_ui_command() {
    let packed = LAST_UI_CMD.load(Ordering::Relaxed);
    if packed == 0 {
        return;
    }
    let command_type = (packed & 0xffff) as u16;
    let track_id = (packed >> 32) as u32;
    let time_ms = LAST_UI_CMD_TIME_MS.load(Ordering::Relaxed);
    eprintln!(
        "daw-app: last ui cmd type {} track {} at {}ms",
        command_type,
        track_id,
        time_ms
    );
}

pub struct RingView {
    header: *mut RingHeader,
    entries: *mut EventEntry,
    mask: u32,
}

pub struct EngineBridge {
    _mmap: MmapMut,
    base: *const u8,
    header: *const ShmHeader,
    reader: SeqlockReader,
    ring_ui: Option<RingView>,
    ring_ui_out: Option<RingView>,
}

impl EngineBridge {
    pub fn open() -> Result<Self> {
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

    pub fn read_snapshot(&self) -> Option<UiSnapshot> {
        self.reader.read_snapshot()
    }

    #[allow(dead_code)]
    pub fn send_ui_command(&self, payload: UiCommandPayload) -> bool {
        let Some(ring) = self.ring_ui.as_ref() else {
            return false;
        };
        record_ui_command(payload.command_type, payload.track_id);
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
        let ok = ring_write_with_retry(ring, entry, Duration::from_millis(20));
        if ok {
            bump_ui_sent();
        } else {
            bump_ui_send_fail();
            log_ui_send_fail();
        }
        ok
    }

    #[allow(dead_code)]
    pub fn send_ui_chord_command(&self, payload: UiChordCommandPayload) -> bool {
        let Some(ring) = self.ring_ui.as_ref() else {
            return false;
        };
        record_ui_command(payload.command_type, payload.track_id);
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
        let ok = ring_write_with_retry(ring, entry, Duration::from_millis(20));
        if ok {
            bump_ui_sent();
        } else {
            bump_ui_send_fail();
            log_ui_send_fail();
        }
        ok
    }

    pub fn try_send_ui_command(&self, payload: UiCommandPayload) -> bool {
        let Some(ring) = self.ring_ui.as_ref() else {
            return false;
        };
        record_ui_command(payload.command_type, payload.track_id);
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

    pub fn try_send_ui_chord_command(&self, payload: UiChordCommandPayload) -> bool {
        let Some(ring) = self.ring_ui.as_ref() else {
            return false;
        };
        record_ui_command(payload.command_type, payload.track_id);
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

    pub fn pop_ui_event(&self) -> Option<EventEntry> {
        let Some(ring) = self.ring_ui_out.as_ref() else {
            return None;
        };
        ring_pop(ring)
    }

    pub fn read_clip_snapshot(&self) -> Option<UiClipSnapshot> {
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

    pub fn read_harmony_snapshot(&self) -> Option<UiHarmonySnapshot> {
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
            if harmony_offset == 0
                || harmony_bytes < std::mem::size_of::<UiHarmonySnapshot>() as u64
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

fn align_up(value: usize, alignment: usize) -> usize {
    (value + alignment - 1) & !(alignment - 1)
}

fn is_power_of_two(value: u32) -> bool {
    value != 0 && (value & (value - 1)) == 0
}

pub fn ring_view(base: *mut u8, offset: u64) -> Option<RingView> {
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
    let write = unsafe { (*ring.header).write_index.load(Ordering::Relaxed) };
    let read = unsafe { (*ring.header).read_index.load(Ordering::Acquire) };
    let next = (write + 1) & ring.mask;
    if next == read {
        return false;
    }
    unsafe {
        *ring.entries.add(write as usize) = entry;
        (*ring.header)
            .write_index
            .store(next, Ordering::Release);
    }
    true
}

#[allow(dead_code)]
fn ring_write_with_retry(ring: &RingView, entry: EventEntry, timeout: Duration) -> bool {
    let start = Instant::now();
    let mut spins = 0_u32;
    loop {
        if ring_write(ring, entry) {
            return true;
        }
        if start.elapsed() >= timeout {
            return false;
        }
        if spins < 64 {
            std::thread::yield_now();
        } else {
            std::thread::sleep(Duration::from_micros(200));
        }
        spins = spins.saturating_add(1);
    }
}

pub fn ring_pop(ring: &RingView) -> Option<EventEntry> {
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

pub fn decode_ui_diff(entry: &EventEntry) -> Option<UiDiffPayload> {
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

pub fn decode_harmony_diff(entry: &EventEntry) -> Option<UiHarmonyDiffPayload> {
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

pub fn decode_chord_diff(entry: &EventEntry) -> Option<UiChordDiffPayload> {
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
