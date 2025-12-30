mod app {
    #![allow(dead_code)]
    include!("../src/main.rs");

    #[cfg(test)]
    mod integration_tests {
        use super::*;
        use std::ffi::CString;
        use std::sync::{Mutex, MutexGuard, OnceLock};
        use std::sync::atomic::{AtomicUsize, Ordering};
        use std::time::{Duration, Instant};
        use std::{env, thread};

        struct NoopNotify;

        impl UiNotify for NoopNotify {
            fn notify(&mut self) {}
        }

        struct EngineProcess {
            child: Option<Child>,
        }

        impl EngineProcess {
            fn start() -> anyhow::Result<Self> {
                let engine_path = default_engine_path()
                    .ok_or_else(|| anyhow::anyhow!("Could not find daw_engine binary"))?;
                let child = spawn_engine_process(&engine_path)?;
                Ok(Self { child: Some(child) })
            }

            fn stop(&mut self) {
                if let Some(mut child) = self.child.take() {
                    let _ = child.kill();
                    let _ = child.wait();
                }
            }
        }

        impl Drop for EngineProcess {
            fn drop(&mut self) {
                self.stop();
            }
        }

        struct PumpResult {
            clip_resync: bool,
            harmony_resync: bool,
        }

        struct TestHarness {
            _guard: MutexGuard<'static, ()>,
            _engine: EngineProcess,
            bridge: Arc<EngineBridge>,
            view: EngineView,
            notify: NoopNotify,
            shm_name: String,
        }

        #[repr(C)]
        #[derive(Clone, Copy, Debug)]
        struct MidiPayload {
            status: u8,
            data1: u8,
            data2: u8,
            channel: u8,
            tuning_cents: f32,
            note_id: u32,
            reserved: [u8; 28],
        }

        struct TrackShm {
            _mmap: MmapMut,
            base: *mut u8,
            header: *const ShmHeader,
            ring_std: RingView,
        }

        impl TestHarness {
            fn new(test_name: &str) -> anyhow::Result<Self> {
                let guard = test_lock();
                let shm_name = format!("/daw_ui_t{}_{}", std::process::id(), next_shm_id());
                cleanup_shm(&shm_name);
                env::set_var("DAW_UI_SHM_NAME", &shm_name);
                env::set_var("DAW_ENGINE_TEST_MODE", "1");

                let engine = EngineProcess::start()?;
                let bridge = connect_bridge_with_retry(Duration::from_secs(5))?;
                let bridge = Arc::new(bridge);

                let mut view = EngineView::new_for_tests();
                view.bridge = Some(bridge.clone());
                view.status = "SHM: connected".into();
                let mut harness = Self {
                    _guard: guard,
                    _engine: engine,
                    bridge,
                    view,
                    notify: NoopNotify,
                    shm_name,
                };
                harness.wait_for_initial_snapshots(Duration::from_secs(2))?;
                Ok(harness)
            }

            fn wait_for_initial_snapshots(&mut self, timeout: Duration) -> anyhow::Result<()> {
                let start = Instant::now();
                loop {
                    if let Some(snapshot) = self.bridge.read_snapshot() {
                        self.view.snapshot = snapshot;
                    }
                    let clip_snapshot = self.bridge.read_clip_snapshot();
                    let harmony_snapshot = self.bridge.read_harmony_snapshot();
                    if let (Some(clip_snapshot), Some(harmony_snapshot)) =
                        (clip_snapshot, harmony_snapshot) {
                        self.view.apply_clip_snapshot(clip_snapshot);
                        self.view.apply_harmony_snapshot(harmony_snapshot);
                        self.view.pending_notes.clear();
                        return Ok(());
                    }
                    if start.elapsed() > timeout {
                        return Err(anyhow::anyhow!(
                            "Timed out waiting for initial snapshots"
                        ));
                    }
                    thread::sleep(Duration::from_millis(20));
                }
            }

            fn press_key(&mut self, key: &str) {
                let keystroke = gpui::Keystroke::parse(key)
                    .unwrap_or_else(|err| panic!("invalid keystroke {}: {}", key, err));
                self.view.handle_keystroke(&keystroke, &mut self.notify);
            }

            fn action_palette_up(&mut self) {
                self.view.action_palette_up(&mut self.notify);
            }

            fn action_palette_down(&mut self) {
                self.view.action_palette_down(&mut self.notify);
            }

            fn action_palette_backspace(&mut self) {
                self.view.action_palette_backspace(&mut self.notify);
            }

            fn action_palette_confirm(&mut self) {
                self.view.action_palette_confirm(&mut self.notify);
            }

            fn toggle_harmony_focus(&mut self) {
                self.view.toggle_harmony_focus(&mut self.notify);
            }

            fn move_column(&mut self, delta: i32) {
                self.view.move_column(delta, &mut self.notify);
            }

            fn adjust_columns(&mut self, track: usize, delta: i32) {
                self.view.adjust_columns(track, delta, &mut self.notify);
            }

            fn pump(&mut self, timeout: Duration) -> PumpResult {
                let start = Instant::now();
                let mut clip_resync = false;
                let mut harmony_resync = false;
                let mut processed_any = false;
                loop {
                    let mut processed_loop = false;
                    while let Some(entry) = self.bridge.pop_ui_event() {
                        if let Some(diff) = decode_ui_diff(&entry) {
                            if diff.diff_type == UiDiffType::ResyncNeeded as u16 {
                                clip_resync = true;
                            } else {
                                self.view.apply_diff(diff);
                            }
                            processed_loop = true;
                            continue;
                        }
                        if let Some(diff) = decode_harmony_diff(&entry) {
                            if diff.diff_type == UiHarmonyDiffType::ResyncNeeded as u16 {
                                harmony_resync = true;
                            } else {
                                self.view.apply_harmony_diff(diff);
                            }
                            processed_loop = true;
                            continue;
                        }
                        if let Some(diff) = decode_chord_diff(&entry) {
                            if diff.diff_type == UiChordDiffType::ResyncNeeded as u16 {
                                clip_resync = true;
                            } else {
                                self.view.apply_chord_diff(diff);
                            }
                            processed_loop = true;
                        }
                    }
                    if processed_loop {
                        processed_any = true;
                    }
                    if start.elapsed() > timeout {
                        break;
                    }
                    if !processed_loop {
                        thread::sleep(Duration::from_millis(5));
                    }
                }
                if processed_any {
                    if let Some(snapshot) = self.bridge.read_snapshot() {
                        self.view.snapshot = snapshot;
                    }
                }
                if clip_resync {
                    if let Some(snapshot) = self.bridge.read_snapshot() {
                        self.view.snapshot = snapshot;
                    }
                    if let Some(clip_snapshot) = self.bridge.read_clip_snapshot() {
                        self.view.apply_clip_snapshot(clip_snapshot);
                    }
                }
                if harmony_resync {
                    if let Some(snapshot) = self.bridge.read_snapshot() {
                        self.view.snapshot = snapshot;
                    }
                    if let Some(harmony_snapshot) = self.bridge.read_harmony_snapshot() {
                        self.view.apply_harmony_snapshot(harmony_snapshot);
                    }
                }
                PumpResult {
                    clip_resync,
                    harmony_resync,
                }
            }

            fn nanotick_for_row(&self, row: i64) -> u64 {
                self.view.nanotick_for_row(row)
            }

            fn notes_at_row(&self, track: usize, row: i64) -> Vec<ClipNote> {
                let nanotick = self.nanotick_for_row(row);
                self.view.clip_notes[track]
                    .iter()
                    .filter(|note| note.nanotick == nanotick)
                    .cloned()
                    .collect()
            }

            fn chords_at_row(&self, track: usize, row: i64) -> Vec<ClipChord> {
                let nanotick = self.nanotick_for_row(row);
                self.view.clip_chords[track]
                    .iter()
                    .filter(|chord| chord.nanotick == nanotick)
                    .cloned()
                    .collect()
            }

            fn assert_view_matches_snapshot_row(
                &self,
                track: usize,
                row: i64,
            ) -> anyhow::Result<()> {
                let snapshot = self.bridge.read_clip_snapshot()
                    .ok_or_else(|| anyhow::anyhow!("missing clip snapshot"))?;
                let nanotick = self.nanotick_for_row(row);
                let track_entry = snapshot.tracks[track];
                let note_start = track_entry.note_offset as usize;
                let note_end = note_start + track_entry.note_count as usize;
                let snap_notes = snapshot.notes[note_start..note_end]
                    .iter()
                    .filter(|note| note.t_on == nanotick)
                    .map(|note| note.pitch)
                    .collect::<Vec<_>>();
                let view_notes = self.notes_at_row(track, row)
                    .into_iter()
                    .map(|note| note.pitch)
                    .collect::<Vec<_>>();
                if snap_notes != view_notes {
                    return Err(anyhow::anyhow!(
                        "snapshot mismatch at row {}: snapshot={:?} view={:?}",
                        row,
                        snap_notes,
                        view_notes
                    ));
                }
                Ok(())
            }
        }

        impl Drop for TestHarness {
            fn drop(&mut self) {
                cleanup_shm(&self.shm_name);
            }
        }

        fn test_lock() -> MutexGuard<'static, ()> {
            static LOCK: OnceLock<Mutex<()>> = OnceLock::new();
            LOCK.get_or_init(|| Mutex::new(()))
                .lock()
                .unwrap_or_else(|err| err.into_inner())
        }

        fn next_shm_id() -> usize {
            static COUNTER: AtomicUsize = AtomicUsize::new(1);
            COUNTER.fetch_add(1, Ordering::Relaxed)
        }

        fn cleanup_shm(name: &str) {
            if let Ok(c_name) = CString::new(name) {
                unsafe {
                    libc::shm_unlink(c_name.as_ptr());
                }
            }
        }

        fn connect_bridge_with_retry(timeout: Duration) -> anyhow::Result<EngineBridge> {
            let start = Instant::now();
            loop {
                match EngineBridge::open() {
                    Ok(bridge) => return Ok(bridge),
                    Err(err) => {
                        if start.elapsed() > timeout {
                            return Err(anyhow::anyhow!(
                                "failed to connect to engine: {err}"
                            ));
                        }
                        thread::sleep(Duration::from_millis(50));
                    }
                }
            }
        }

        fn open_track_shm(timeout: Duration) -> anyhow::Result<TrackShm> {
            let start = Instant::now();
            let name = CString::new("/daw_engine_shared").unwrap();
            loop {
                let fd = unsafe { libc::shm_open(name.as_ptr(), libc::O_RDWR, 0) };
                if fd >= 0 {
                    let file = unsafe { std::fs::File::from_raw_fd(fd) };
                    let mmap = unsafe { MmapOptions::new().map_mut(&file)? };
                    let base = mmap.as_ptr() as *mut u8;
                    let header = base as *const ShmHeader;
                    let ring_std = ring_view(base, unsafe { (*header).ring_std_offset })
                        .ok_or_else(|| anyhow::anyhow!("ring_std missing"))?;
                    return Ok(TrackShm {
                        _mmap: mmap,
                        base,
                        header,
                        ring_std,
                    });
                }
                if start.elapsed() > timeout {
                    return Err(anyhow::anyhow!("timed out opening track shm"));
                }
                thread::sleep(Duration::from_millis(20));
            }
        }

        fn read_midi_payload(entry: &EventEntry) -> Option<MidiPayload> {
            if entry.event_type != EventType::Midi as u16 {
                return None;
            }
            if entry.size as usize != std::mem::size_of::<MidiPayload>() {
                return None;
            }
            let mut payload = MidiPayload {
                status: 0,
                data1: 0,
                data2: 0,
                channel: 0,
                tuning_cents: 0.0,
                note_id: 0,
                reserved: [0u8; 28],
            };
            let payload_bytes = unsafe {
                std::slice::from_raw_parts_mut(
                    &mut payload as *mut MidiPayload as *mut u8,
                    std::mem::size_of::<MidiPayload>(),
                )
            };
            payload_bytes.copy_from_slice(&entry.payload[..payload_bytes.len()]);
            Some(payload)
        }

        fn pattern_samples(sample_rate: f64, pattern_ticks: u64) -> u64 {
            let bpm = 120.0_f64;
            let ticks_per_quarter = NANOTICKS_PER_QUARTER as f64;
            let ticks_per_second = bpm * ticks_per_quarter / 60.0;
            let seconds = pattern_ticks as f64 / ticks_per_second;
            (seconds * sample_rate).round() as u64
        }

        #[test]
        fn test_tracker_rapid_note_replacement() -> anyhow::Result<()> {
            let mut harness = TestHarness::new("rapid_note_replacement")?;
            harness.view.cursor_row = 0;
            harness.view.scroll_row_offset = 0;
            harness.view.focused_track_index = 0;
            harness.view.cursor_col = 0;
            harness.view.harmony_focus = false;

            let pitches = [("q", 60), ("w", 62), ("e", 64), ("r", 65)];
            for (index, (key, expected_pitch)) in pitches.iter().enumerate() {
                harness.press_key(key);
                let pump = harness.pump(Duration::from_millis(300));
                assert!(!pump.clip_resync, "unexpected clip resync on {}", key);
                assert_eq!(harness.view.cursor_row, 1, "cursor should move down after {}", key);

                harness.action_palette_up();
                let notes = harness.notes_at_row(0, 0);
                assert_eq!(notes.len(), 1, "expected one note after {}", key);
                assert_eq!(notes[0].pitch, *expected_pitch, "pitch mismatch after {}", key);
                harness.assert_view_matches_snapshot_row(0, 0)?;

                if index + 1 < pitches.len() {
                    harness.action_palette_up();
                }
            }

            harness.action_palette_backspace();
            let pump = harness.pump(Duration::from_millis(300));
            assert!(!pump.clip_resync, "unexpected clip resync on backspace");
            assert_eq!(harness.view.cursor_row, 1, "cursor should move down after delete");
            let notes = harness.notes_at_row(0, 0);
            assert!(notes.is_empty(), "note should be deleted");
            harness.assert_view_matches_snapshot_row(0, 0)?;

            harness.action_palette_up();
            harness.action_palette_backspace();
            let pump = harness.pump(Duration::from_millis(300));
            assert!(!pump.clip_resync, "unexpected clip resync on empty backspace");
            let notes = harness.notes_at_row(0, 0);
            assert!(notes.is_empty(), "cell should remain empty");
            harness.assert_view_matches_snapshot_row(0, 0)?;

            Ok(())
        }

        #[test]
        fn test_tracker_degree_and_chord_entries() -> anyhow::Result<()> {
            let mut harness = TestHarness::new("degree_and_chords")?;
            harness.view.cursor_row = 0;
            harness.view.scroll_row_offset = 0;
            harness.view.focused_track_index = 0;
            harness.view.cursor_col = 0;

            harness.press_key("1");
            let pump = harness.pump(Duration::from_millis(300));
            assert!(!pump.clip_resync, "unexpected clip resync on degree note");
            let chords = harness.chords_at_row(0, 0);
            assert_eq!(chords.len(), 1, "expected degree chord at row 0");
            assert_eq!(chords[0].degree, 1);

            harness.action_palette_backspace();
            let pump = harness.pump(Duration::from_millis(300));
            assert!(!pump.clip_resync, "unexpected clip resync on chord delete");
            let chords = harness.chords_at_row(0, 0);
            assert!(chords.is_empty(), "degree chord should be deleted");
            harness.action_palette_up();

            harness.press_key("2");
            let pump = harness.pump(Duration::from_millis(300));
            assert!(!pump.clip_resync, "unexpected clip resync on degree note 2");
            let chords = harness.chords_at_row(0, 0);
            assert_eq!(chords.len(), 1, "expected degree 2 chord at row 0");
            assert_eq!(chords[0].degree, 2);

            harness.action_palette_backspace();
            let pump = harness.pump(Duration::from_millis(300));
            assert!(!pump.clip_resync, "unexpected clip resync on chord delete");
            let chords = harness.chords_at_row(0, 0);
            assert!(chords.is_empty(), "degree 2 chord should be deleted");
            harness.action_palette_up();

            harness.press_key("@");
            harness.press_key("3");
            harness.action_palette_confirm();
            let pump = harness.pump(Duration::from_millis(300));
            assert!(!pump.clip_resync, "unexpected clip resync on chord entry");
            let chords = harness.chords_at_row(0, 0);
            assert_eq!(chords.len(), 1, "expected @3 chord at row 0");
            assert_eq!(chords[0].degree, 3);

            harness.action_palette_backspace();
            let pump = harness.pump(Duration::from_millis(300));
            assert!(!pump.clip_resync, "unexpected clip resync on chord delete");
            let chords = harness.chords_at_row(0, 0);
            assert!(chords.is_empty(), "@3 chord should be deleted");

            Ok(())
        }

        #[test]
        fn test_tracker_cell_edit_midi_note() -> anyhow::Result<()> {
            let mut harness = TestHarness::new("cell_edit_midi")?;
            harness.view.cursor_row = 0;
            harness.view.scroll_row_offset = 0;
            harness.view.focused_track_index = 0;
            harness.view.cursor_col = 0;

            harness.action_palette_confirm();
            harness.press_key("C");
            harness.press_key("-");
            harness.press_key("4");
            harness.action_palette_confirm();
            let pump = harness.pump(Duration::from_millis(300));
            assert!(!pump.clip_resync, "unexpected clip resync on MIDI token");
            let notes = harness.notes_at_row(0, 0);
            assert_eq!(notes.len(), 1, "expected C-4 note");
            assert_eq!(notes[0].pitch, 60);
            harness.assert_view_matches_snapshot_row(0, 0)?;

            Ok(())
        }

        #[test]
        fn test_tracker_alt_sharp_entry() -> anyhow::Result<()> {
            let mut harness = TestHarness::new("alt_sharp_entry")?;
            harness.view.cursor_row = 0;
            harness.view.scroll_row_offset = 0;
            harness.view.focused_track_index = 0;
            harness.view.cursor_col = 0;

            harness.press_key("alt-2");
            let pump = harness.pump(Duration::from_millis(300));
            assert!(!pump.clip_resync, "unexpected clip resync on alt-2");
            let notes = harness.notes_at_row(0, 0);
            assert_eq!(notes.len(), 1, "expected one note on alt-2");
            assert_eq!(notes[0].pitch, 61, "expected C#4 for alt-2");
            harness.assert_view_matches_snapshot_row(0, 0)?;

            Ok(())
        }

        #[test]
        fn test_tracker_alt_sharp_sequence() -> anyhow::Result<()> {
            let mut harness = TestHarness::new("alt_sharp_sequence")?;
            harness.view.cursor_row = 0;
            harness.view.scroll_row_offset = 0;
            harness.view.focused_track_index = 0;
            harness.view.cursor_col = 0;

            let sequence = [
                ("alt-2", 61_u8),
                ("alt-3", 63_u8),
                ("alt-5", 66_u8),
                ("alt-6", 68_u8),
                ("alt-7", 70_u8),
            ];

            for (key, _) in sequence {
                harness.press_key(key);
                let pump = harness.pump(Duration::from_millis(300));
                assert!(!pump.clip_resync, "unexpected clip resync on {key}");
            }

            for (row, (_, pitch)) in sequence.iter().enumerate() {
                let notes = harness.notes_at_row(0, row as i64);
                assert_eq!(notes.len(), 1, "expected one note at row {row}");
                assert_eq!(notes[0].pitch, *pitch, "pitch mismatch at row {row}");
                harness.assert_view_matches_snapshot_row(0, row as i64)?;
            }

            Ok(())
        }

        #[test]
        fn test_tracker_harmony_rapid_edits() -> anyhow::Result<()> {
            let mut harness = TestHarness::new("harmony_edits")?;
            harness.view.cursor_row = 0;
            harness.view.scroll_row_offset = 0;
            harness.toggle_harmony_focus();

            harness.press_key("c");
            let pump = harness.pump(Duration::from_millis(300));
            assert!(!pump.harmony_resync, "unexpected harmony resync on C");
            assert_eq!(harness.view.harmony_events.len(), 1);
            assert_eq!(harness.view.harmony_events[0].root, 0);

            harness.press_key("d");
            let pump = harness.pump(Duration::from_millis(300));
            assert!(!pump.harmony_resync, "unexpected harmony resync on D");
            assert_eq!(harness.view.harmony_events.len(), 1);
            assert_eq!(harness.view.harmony_events[0].root, 2);

            harness.action_palette_down();
            harness.press_key("e");
            let pump = harness.pump(Duration::from_millis(300));
            assert!(!pump.harmony_resync, "unexpected harmony resync on E");
            assert_eq!(harness.view.harmony_events.len(), 2);

            harness.action_palette_backspace();
            let pump = harness.pump(Duration::from_millis(300));
            assert!(!pump.harmony_resync, "unexpected harmony resync on delete");
            assert_eq!(harness.view.harmony_events.len(), 1);

            Ok(())
        }

        #[test]
        fn test_tracker_multi_column_quick_edit() -> anyhow::Result<()> {
            let mut harness = TestHarness::new("multi_column_quick_edit")?;
            harness.view.cursor_row = 0;
            harness.view.scroll_row_offset = 0;
            harness.view.focused_track_index = 0;
            harness.view.cursor_col = 0;

            harness.adjust_columns(0, 1);
            assert_eq!(harness.view.track_columns[0], 2);
            harness.move_column(1);
            assert_eq!(harness.view.cursor_col, 1);

            harness.press_key("q");
            let pump = harness.pump(Duration::from_millis(200));
            assert!(!pump.clip_resync, "unexpected clip resync on multi-column note");
            let notes = harness.notes_at_row(0, 0);
            assert_eq!(notes.len(), 1);
            assert_eq!(notes[0].pitch, 60);

            harness.adjust_columns(0, -1);
            assert_eq!(harness.view.track_columns[0], 1);
            assert_eq!(harness.view.cursor_col, 0);

            Ok(())
        }

        #[test]
        fn test_tracker_chord_packed_fields() -> anyhow::Result<()> {
            let mut harness = TestHarness::new("chord_packed_fields")?;
            harness.view.cursor_row = 0;
            harness.view.scroll_row_offset = 0;
            harness.view.focused_track_index = 0;
            harness.view.cursor_col = 0;

            let token = "@3^7/1-5~240h6";
            for ch in token.chars() {
                let mut key = String::new();
                key.push(ch);
                harness.press_key(&key);
            }
            harness.action_palette_confirm();
            let pump = harness.pump(Duration::from_millis(300));
            assert!(!pump.clip_resync, "unexpected clip resync on chord packed");

            let chords = harness.chords_at_row(0, 0);
            assert_eq!(chords.len(), 1, "expected packed chord at row 0");
            let chord = &chords[0];
            assert_eq!(chord.degree, 3);
            assert_eq!(chord.quality, 2);
            assert_eq!(chord.inversion, 1);
            assert_eq!(chord.base_octave, 5);
            assert_eq!(chord.spread, 240);
            assert_eq!(chord.humanize_timing, 6);
            assert_eq!(chord.humanize_velocity, 6);

            harness.action_palette_backspace();
            let pump = harness.pump(Duration::from_millis(300));
            assert!(!pump.clip_resync, "unexpected clip resync on chord delete");
            let chords = harness.chords_at_row(0, 0);
            assert!(chords.is_empty(), "packed chord should be deleted");

            Ok(())
        }

        #[test]
        fn test_tracker_multi_track_stress_edits() -> anyhow::Result<()> {
            let mut harness = TestHarness::new("multi_track_stress")?;
            harness.view.cursor_row = 0;
            harness.view.scroll_row_offset = 0;
            harness.view.harmony_focus = false;

            harness.adjust_columns(0, 1);

            let edits = [("q", 60), ("w", 62), ("e", 64), ("r", 65)];
            for (track, column) in [(0, 0), (0, 1)] {
                harness.view.focus_note_cell(0, track, column, &mut harness.notify);
                for (key, expected_pitch) in edits {
                    harness.press_key(key);
                    let pump = harness.pump(Duration::from_millis(200));
                    assert!(!pump.clip_resync, "unexpected clip resync on {key}");
                    harness.action_palette_up();
                    let notes = harness.notes_at_row(track, 0);
                    assert_eq!(notes.len(), 1, "expected one note at row 0");
                    assert_eq!(notes[0].pitch, expected_pitch);
                    harness.assert_view_matches_snapshot_row(track, 0)?;
                    harness.action_palette_up();
                }
            }

            Ok(())
        }

        #[test]
        fn test_loop_determinism_degree_harmony() -> anyhow::Result<()> {
            let mut harness = TestHarness::new("loop_determinism")?;
            harness.view.cursor_row = 0;
            harness.view.scroll_row_offset = 0;
            harness.view.focused_track_index = 0;
            harness.view.cursor_col = 0;
            harness.view.harmony_focus = false;

            // Harmony changes at rows 0, 4, 8: C:min, C:maj, C:dor.
            harness.toggle_harmony_focus();
            harness.view.set_harmony_scale(2, &mut harness.notify); // min
            harness.press_key("c");
            let pump = harness.pump(Duration::from_millis(200));
            assert!(!pump.harmony_resync, "unexpected harmony resync");

            for _ in 0..4 {
                harness.action_palette_down();
            }
            harness.view.set_harmony_scale(1, &mut harness.notify); // maj
            harness.press_key("c");
            let pump = harness.pump(Duration::from_millis(200));
            assert!(!pump.harmony_resync, "unexpected harmony resync");

            for _ in 0..4 {
                harness.action_palette_down();
            }
            harness.view.set_harmony_scale(3, &mut harness.notify); // dor
            harness.press_key("c");
            let pump = harness.pump(Duration::from_millis(200));
            assert!(!pump.harmony_resync, "unexpected harmony resync");

            harness.toggle_harmony_focus();
            for _ in 0..8 {
                harness.action_palette_up();
            }

            // Degree notes 1..8 at rows 0..7 via cell edit tokens.
            for degree in 1_u8..=8_u8 {
                harness.press_key("i");
                let token = format!("{}-4", degree);
                for ch in token.chars() {
                    let mut key = String::new();
                    key.push(ch);
                    harness.press_key(&key);
                }
                harness.action_palette_confirm();
                let pump = harness.pump(Duration::from_millis(200));
                assert!(!pump.clip_resync, "unexpected clip resync");
                if degree < 8 {
                    harness.action_palette_down();
                }
            }

            let track_shm = open_track_shm(Duration::from_secs(2))?;
            while ring_pop(&track_shm.ring_std).is_some() {}

            harness.view.toggle_play(&mut harness.notify);
            let _ = harness.pump(Duration::from_millis(200));

            let sample_rate = unsafe { (*track_shm.header).sample_rate };
            let pattern_ticks = (NANOTICKS_PER_QUARTER / LINES_PER_BEAT) * 16;
            let loop_samples = pattern_samples(sample_rate, pattern_ticks);
            let mut loops: Vec<Vec<u8>> = vec![Vec::new(), Vec::new(), Vec::new()];
            let mut first_note_sample: Option<u64> = None;
            let mut stop_after: Option<u64> = None;
            let start = Instant::now();
            while start.elapsed() < Duration::from_secs(3) {
                if let Some(entry) = ring_pop(&track_shm.ring_std) {
                    if let Some(payload) = read_midi_payload(&entry) {
                        if payload.status == 0x90 && payload.data2 > 0 {
                            let base = first_note_sample.get_or_insert(entry.sample_time);
                            if stop_after.is_none() {
                                stop_after = Some(*base + loop_samples * loops.len() as u64);
                            }
                            let delta = entry.sample_time.saturating_sub(*base);
                            let loop_index = (delta / loop_samples) as usize;
                            if loop_index < loops.len() {
                                loops[loop_index].push(payload.data1);
                            }
                        }
                    }
                    if let Some(stop) = stop_after {
                        if entry.sample_time >= stop {
                            break;
                        }
                    }
                } else {
                    thread::sleep(Duration::from_millis(1));
                }
            }

            let expected = &loops[0];
            assert!(!expected.is_empty(), "no MIDI events captured");
            for (index, loop_notes) in loops.iter().enumerate().skip(1) {
                assert_eq!(
                    expected, loop_notes,
                    "loop {} note sequence differs: {:?} vs {:?}",
                    index,
                    expected,
                    loop_notes
                );
            }

            Ok(())
        }
    }
}
