use std::sync::atomic::{fence, AtomicU64, Ordering};
use std::time::{Duration, Instant};

use crate::layout::{ShmHeader, K_UI_MAX_TRACKS};

/// How long a seqlock read waits for the writer before giving up. A frame at 60Hz is 16.6ms and the
/// engine holds `ui_version` odd for the length of one publish, so this is several orders of
/// magnitude more than a live writer ever needs — it exists for the writer that is never coming back.
pub const DEFAULT_SEQLOCK_DEADLINE: Duration = Duration::from_millis(50);

/// A BOUNDED SEQLOCK READ.
///
/// Every reader here spun `loop { if v0 % 2 == 1 { continue; } ... }`. If the engine dies with
/// `ui_version` ODD — mid-publish, which is exactly when a crash is most likely, because that is when
/// it is doing work — the version never becomes even again and the reader spins at 100% of a core
/// **forever**. It never returns, so the caller cannot time out, log, or reattach: the sidecar's
/// state reattach and every pre-dispatch base read wedge behind a process that no longer exists.
///
/// ELEVEN sites had this. The finding that opened the ticket named two. The other nine were the same
/// five lines, and a fix scoped to the two named would have left the wedge reachable by nine paths.
///
/// FOUR MORE SITES ALREADY DID IT RIGHT and are left alone: the per-slot waveform, automation,
/// envelope and kit readers spin `for _ in 0..4096` over their own `seq` field. This type follows
/// that precedent rather than inventing a second one, with one change — an iteration count is not a
/// time bound. 4096 spins is microseconds on one machine and milliseconds on another, and it says
/// nothing about how long a caller waited. A deadline is the boundary the caller actually cares
/// about, so that is what this takes.
///
/// The caller distinguishes nothing new: these functions already returned `Option`, and a timeout is
/// `None` — "I could not read it", which is what a dead writer means. See `try_read_snapshot` for the
/// non-waiting form.
pub struct SeqlockAttempt {
    deadline: Option<Instant>,
    spins: u32,
    first: bool,
}

impl SeqlockAttempt {
    /// Wait until `deadline`. `None` means the single-pass form: one look, never a wait.
    pub fn new(deadline: Option<Instant>) -> Self {
        Self { deadline, spins: 0, first: true }
    }

    pub fn until(deadline: Instant) -> Self {
        Self::new(Some(deadline))
    }

    /// One attempt only — used by the `try_*` readers, which must never block a UI frame.
    pub fn once() -> Self {
        Self { deadline: None, spins: 0, first: true }
    }

    /// Start a read. `Some(v0)` is an even version to read under; `None` means give up — the writer
    /// is mid-write and the deadline has passed, or this is a single-pass attempt.
    ///
    /// EVERY CALL AFTER THE FIRST COSTS A WAIT, including one that follows a torn read on an even
    /// version. Without that, a writer publishing continuously would send this straight back round
    /// with no deadline check — a livelock in place of the spin, which is not an improvement.
    pub fn begin(&mut self, version: &AtomicU64) -> Option<u64> {
        if !self.first && !self.wait() {
            return None;
        }
        self.first = false;
        loop {
            let v0 = version.load(Ordering::Acquire);
            if v0 % 2 == 0 {
                return Some(v0);
            }
            if !self.wait() {
                return None;
            }
        }
    }

    /// DID THE READ HOLD — and nothing else.
    ///
    /// My first version returned `self.wait()` when the version had moved, so `true` meant either
    /// "the read held" or "torn, but you may retry". Every call site is
    /// `if attempt.commit(..) { return Some(x) }`, so that second meaning **returns torn data as
    /// valid** — a correctness regression strictly worse than the hang this ticket is about. The
    /// retry belongs to `begin`, which is the only place that can also stop.
    pub fn commit(&self, version: &AtomicU64, v0: u64) -> bool {
        fence(Ordering::Acquire);
        version.load(Ordering::Acquire) == v0 && v0 % 2 == 0
    }

    /// `false` once the caller should stop. SPIN THEN YIELD: a live writer finishes in nanoseconds
    /// and a spin hint is the cheapest way to wait for it, but a spin that never yields starves a
    /// writer sharing this core — which turns a brief overlap into the hang it was trying to avoid.
    fn wait(&mut self) -> bool {
        let Some(deadline) = self.deadline else {
            return false;
        };
        self.spins += 1;
        if self.spins < 64 {
            std::hint::spin_loop();
        } else {
            std::thread::yield_now();
        }
        Instant::now() < deadline
    }
}

#[derive(Clone, Copy, Debug)]
pub struct UiSnapshot {
    pub version: u64,
    pub ui_visual_sample_count: u64,
    pub ui_global_nanotick_playhead: u64,
    pub ui_track_count: u32,
    pub ui_transport_state: u32,
    /// Tempo at the current playhead, milli-BPM (120000 = 120.000). Read in the same
    /// seqlock frame as the playhead, so it is the tempo at THIS position.
    pub ui_tempo_milli_bpm: u32,
    /// Points in the project tempo map (1 = constant tempo).
    pub ui_tempo_point_count: u32,
    pub ui_clip_version: u32,
    pub ui_clip_offset: u64,
    pub ui_clip_bytes: u64,
    pub ui_harmony_version: u32,
    pub ui_harmony_offset: u64,
    pub ui_harmony_bytes: u64,
    pub ui_track_peak_rms: [f32; K_UI_MAX_TRACKS],
    /// v10: per-track tracker subdivision (0 = absent track). Build one LaneGrid
    /// per track from this rather than one grid for the whole viewport.
    pub ui_lines_per_beat: [u8; K_UI_MAX_TRACKS],
    /// v20 child-track structure and v13 lane quantize. Copied into the snapshot for the same
    /// reason lines_per_beat is: they are published per track every frame and a reader that has
    /// to reach past this struct for them ends up reading them OUTSIDE the seqlock, which is how
    /// a row gets a parent from one frame and a grid from the next.
    pub ui_track_quantize_grid: [u64; K_UI_MAX_TRACKS],
    pub ui_track_quantize_strength: [u32; K_UI_MAX_TRACKS],
    pub ui_track_quantize_swing: [i32; K_UI_MAX_TRACKS],
    /// v19: the song's time signature (ruler + time gutter). A clip's own meter is
    /// separate, on UiClipExtent.
    pub ui_song_time_sig_num: u32,
    pub ui_song_time_sig_den: u32,
    /// v20: child-track structure (Movement 4). `parent_id` 0 = top-level, else the
    /// parent's track_id; bit0 of `flags` = collapsed. Children are ORDINARY tracks
    /// in these same flat arrays — collapse is a drawing decision, never a change to
    /// what exists — so a reader that ignores both still renders every track.
    pub ui_track_parent_id: [u32; K_UI_MAX_TRACKS],
    pub ui_track_flags: [u8; K_UI_MAX_TRACKS],
    /// v34: how wide a track's per-note op run gets, in GLYPHS — 0 means no note in
    /// the track carries an op at all, so the column can be hidden rather than drawn
    /// empty on every track that never uses one.
    ///
    /// It is read HERE, under the seqlock, and not off `TrackMixer` (which also carries
    /// it) because the mixer read is gated on the engine's mixer version and this
    /// changes when NOTES change. A mixer-gated read would hold yesterday's width for
    /// as long as nobody touched a fader.
    pub ui_track_ops_width: [u8; K_UI_MAX_TRACKS],
}

pub struct SeqlockReader {
    header: *const ShmHeader,
}

impl SeqlockReader {
    pub fn new(header: *const ShmHeader) -> Self {
        Self { header }
    }

    /// Waits up to `DEFAULT_SEQLOCK_DEADLINE`. `None` now means either "no header" or "the writer
    /// did not finish in time" — previously the second case did not return at all.
    pub fn read_snapshot(&self) -> Option<UiSnapshot> {
        self.read_snapshot_until(Instant::now() + DEFAULT_SEQLOCK_DEADLINE)
    }

    /// ONE LOOK, NEVER A WAIT. For a caller on a UI frame that would rather draw the previous
    /// snapshot than miss its deadline.
    pub fn try_read_snapshot(&self) -> Option<UiSnapshot> {
        self.read_snapshot_attempt(SeqlockAttempt::once())
    }

    pub fn read_snapshot_until(&self, deadline: Instant) -> Option<UiSnapshot> {
        self.read_snapshot_attempt(SeqlockAttempt::until(deadline))
    }

    fn read_snapshot_attempt(&self, mut attempt: SeqlockAttempt) -> Option<UiSnapshot> {
        if self.header.is_null() {
            return None;
        }

        while let Some(v0) = attempt.begin(unsafe { &(*self.header).ui_version }) {

            let ui_visual_sample_count = unsafe { (*self.header).ui_visual_sample_count };
            let ui_global_nanotick_playhead = unsafe { (*self.header).ui_global_nanotick_playhead };
            let ui_track_count = unsafe { (*self.header).ui_track_count };
            let ui_transport_state = unsafe { (*self.header).ui_transport_state };
            let ui_tempo_milli_bpm = unsafe { (*self.header).ui_tempo_milli_bpm };
            let ui_tempo_point_count = unsafe { (*self.header).ui_tempo_point_count };
            let ui_clip_version = unsafe { (*self.header).ui_clip_version };
            let ui_clip_offset = unsafe { (*self.header).ui_clip_offset };
            let ui_clip_bytes = unsafe { (*self.header).ui_clip_bytes };
            let ui_harmony_version = unsafe { (*self.header).ui_harmony_version };
            let ui_harmony_offset = unsafe { (*self.header).ui_harmony_offset };
            let ui_harmony_bytes = unsafe { (*self.header).ui_harmony_bytes };
            let ui_track_peak_rms = unsafe { (*self.header).ui_track_peak_rms };
            let ui_lines_per_beat = unsafe { (*self.header).ui_lines_per_beat };
            let ui_track_quantize_grid = unsafe { (*self.header).ui_track_quantize_grid };
            let ui_track_quantize_strength =
                unsafe { (*self.header).ui_track_quantize_strength };
            let ui_track_quantize_swing = unsafe { (*self.header).ui_track_quantize_swing };
            let ui_song_time_sig_num = unsafe { (*self.header).ui_song_time_sig_num };
            let ui_song_time_sig_den = unsafe { (*self.header).ui_song_time_sig_den };
            let ui_track_parent_id = unsafe { (*self.header).ui_track_parent_id };
            let ui_track_flags = unsafe { (*self.header).ui_track_flags };
            let ui_track_ops_width = unsafe { (*self.header).ui_track_ops_width };

            if attempt.commit(unsafe { &(*self.header).ui_version }, v0) {
                return Some(UiSnapshot {
                    version: v0,
                    ui_visual_sample_count,
                    ui_global_nanotick_playhead,
                    ui_track_count,
                    ui_transport_state,
                    ui_tempo_milli_bpm,
                    ui_tempo_point_count,
                    ui_clip_version,
                    ui_clip_offset,
                    ui_clip_bytes,
                    ui_harmony_version,
                    ui_harmony_offset,
                    ui_harmony_bytes,
                    ui_track_peak_rms,
                    ui_lines_per_beat,
                    ui_track_quantize_grid,
                    ui_track_quantize_strength,
                    ui_track_quantize_swing,
                    ui_song_time_sig_num,
                    ui_song_time_sig_den,
                    ui_track_parent_id,
                    ui_track_flags,
                    ui_track_ops_width,
                });
            }
        }
        None
    }
}

#[cfg(test)]
mod seqlock_liveness_tests {
    use super::*;
    use std::sync::atomic::AtomicU64;

    // THE WEDGE. An engine that dies mid-publish leaves ui_version ODD, and the old readers spun
    // `loop { if v0 % 2 == 1 { continue; } }` at 100% of a core with no exit. These tests are written
    // against SeqlockAttempt directly rather than through a mapped header, because the property is
    // "it returns", and a test that needs a live engine to prove termination cannot run in CI.
    //
    // EACH ASSERTS A BOUND IT WOULD BLOW WITHOUT THE FIX. `#[test]` has no timeout, so a test that
    // merely called the old code would hang the suite rather than fail it — which is why the
    // assertion is on ELAPSED TIME against a deadline, not on the mere fact of returning.

    #[test]
    fn an_odd_version_gives_up_at_the_deadline_instead_of_spinning_forever() {
        let version = AtomicU64::new(7); // odd: the writer died mid-publish
        let start = Instant::now();
        let mut attempt = SeqlockAttempt::until(start + Duration::from_millis(30));
        assert!(attempt.begin(&version).is_none(), "must refuse to read under an odd version");
        let waited = start.elapsed();
        assert!(waited >= Duration::from_millis(25), "gave up too early: {waited:?}");
        assert!(waited < Duration::from_millis(500), "did not honour the deadline: {waited:?}");
    }

    #[test]
    fn the_single_pass_form_never_waits() {
        let version = AtomicU64::new(7);
        let start = Instant::now();
        assert!(SeqlockAttempt::once().begin(&version).is_none());
        assert!(start.elapsed() < Duration::from_millis(5), "try_* must not block a UI frame");
    }

    #[test]
    fn an_even_version_reads_immediately() {
        let version = AtomicU64::new(8);
        let mut attempt = SeqlockAttempt::until(Instant::now() + Duration::from_secs(5));
        assert_eq!(attempt.begin(&version), Some(8));
        assert!(attempt.commit(&version, 8), "an unchanged even version must commit");
    }

    #[test]
    fn a_torn_read_is_rejected_and_never_returned_as_valid() {
        // THE REGRESSION THIS GUARDS. My first `commit` returned the backoff's verdict when the
        // version had moved, so `true` meant "held" OR "torn, retry" — and every call site is
        // `if commit(..) { return Some(x) }`. That returns TORN DATA AS VALID, which is worse than
        // the hang the ticket is about. commit answers one question now.
        let version = AtomicU64::new(8);
        let mut attempt = SeqlockAttempt::until(Instant::now() + Duration::from_millis(30));
        assert_eq!(attempt.begin(&version), Some(8));
        version.store(10, Ordering::Release); // published again while we were reading
        assert!(!attempt.commit(&version, 8), "a torn read must NEVER commit");
        assert_eq!(attempt.begin(&version), Some(10), "and the retry sees the new value");
    }

    #[test]
    fn the_retry_loop_stops_at_the_deadline_even_on_an_even_version() {
        // The caller's shape: `while let Some(v0) = begin() { if commit() { return } }`. A writer
        // that keeps publishing makes every commit fail, and if `begin` did not charge a wait for a
        // retry this would livelock instead of spinning — a different infinite loop, not a fix.
        let version = AtomicU64::new(8);
        let start = Instant::now();
        let mut attempt = SeqlockAttempt::until(start + Duration::from_millis(20));
        let mut rounds = 0u32;
        while let Some(v0) = attempt.begin(&version) {
            version.store(v0 + 2, Ordering::Release); // the writer moves under every read
            assert!(!attempt.commit(&version, v0));
            rounds += 1;
            assert!(rounds < 50_000_000, "the retry loop never stopped — this is the wedge");
        }
        let waited = start.elapsed();
        assert!(waited >= Duration::from_millis(15), "gave up too early: {waited:?}");
        assert!(waited < Duration::from_millis(500), "did not honour the deadline: {waited:?}");
    }

    #[test]
    fn a_writer_that_finishes_late_is_still_read() {
        // The deadline must not make a LIVE writer fail. Odd for 20ms, then even.
        let version = std::sync::Arc::new(AtomicU64::new(9));
        let w = version.clone();
        let h = std::thread::spawn(move || {
            std::thread::sleep(Duration::from_millis(20));
            w.store(10, Ordering::Release);
        });
        let mut attempt = SeqlockAttempt::until(Instant::now() + Duration::from_secs(2));
        assert_eq!(attempt.begin(&version), Some(10), "a slow but live writer must still be read");
        h.join().unwrap();
    }

    #[test]
    fn a_null_header_is_still_none_and_costs_nothing() {
        let reader = SeqlockReader::new(std::ptr::null());
        let start = Instant::now();
        assert!(reader.read_snapshot().is_none());
        assert!(reader.try_read_snapshot().is_none());
        assert!(start.elapsed() < Duration::from_millis(5), "the null check must precede the wait");
    }
}
