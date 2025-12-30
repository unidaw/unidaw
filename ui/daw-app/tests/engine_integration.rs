mod app {
    #![allow(dead_code)]
    include!("../src/main.rs");

    #[cfg(test)]
    mod integration_tests {
        use super::*;
        use std::ffi::CString;
        use std::sync::{Mutex, MutexGuard, OnceLock};
        use std::time::{Duration, Instant};
        use std::{env, thread};

        struct NoopNotify;

        impl UiNotify for NoopNotify {
            fn notify(&mut self) {}
        }

        const TRACK_SHM_TIMEOUT: Duration = Duration::from_secs(10);

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

            fn assert_running(&mut self) -> anyhow::Result<()> {
                let Some(child) = self.child.as_mut() else {
                    return Err(anyhow::anyhow!("engine process missing"));
                };
                match child.try_wait()? {
                    Some(status) => Err(anyhow::anyhow!(
                        "engine exited unexpectedly: {}",
                        status
                    )),
                    None => Ok(()),
                }
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
                let shm_name = format!("/daw_ui_test_{}", std::process::id());
                let socket_prefix = {
                    use std::hash::{Hash, Hasher};
                    let mut hasher = std::collections::hash_map::DefaultHasher::new();
                    test_name.hash(&mut hasher);
                    let short = hasher.finish();
                    format!("/tmp/daw_host_{}_{}", std::process::id(), short)
                };
                cleanup_shm(&shm_name);
                for track_id in 0..3 {
                    let socket = format!("{socket_prefix}_{track_id}.sock");
                    cleanup_socket(&socket);
                }
                env::set_var("DAW_UI_SHM_NAME", &shm_name);
                env::set_var("DAW_ENGINE_TEST_MODE", "1");
                env::set_var("DAW_HOST_SOCKET_WAIT_ATTEMPTS", "500");
                env::set_var("DAW_HOST_SOCKET_PREFIX", &socket_prefix);
                reset_ui_counters();

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

            fn send_undo(&mut self) {
                self.view.send_undo(&mut self.notify);
            }

            fn send_redo(&mut self) {
                self.view.send_redo(&mut self.notify);
            }

            fn load_plugin_on_track(&mut self, track_id: u32, plugin_index: u32) {
                let payload = UiCommandPayload {
                    command_type: UiCommandType::LoadPluginOnTrack as u16,
                    flags: 0,
                    track_id,
                    plugin_index,
                    note_pitch: 0,
                    value0: 0,
                    note_nanotick_lo: 0,
                    note_nanotick_hi: 0,
                    note_duration_lo: 0,
                    note_duration_hi: 0,
                    base_version: 0,
                };
                self.bridge.send_ui_command(payload);
            }

            fn set_loop_range(&mut self, start: u64, end: u64) {
                self.view.set_loop_range(start, end, &mut self.notify);
            }

            fn assert_engine_alive(&mut self) -> anyhow::Result<()> {
                self._engine.assert_running()
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
                    self.view.flush_queued_commands();
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
                self.view.flush_queued_commands();
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

            fn entry_count_at_row(&self, track: usize, row: i64) -> usize {
                let nanotick = self.nanotick_for_row(row);
                self.view.entries_for_row(nanotick, track).len()
            }

            fn assert_entries_fit_columns(
                &self,
                track: usize,
                row: i64,
            ) -> anyhow::Result<()> {
                let nanotick = self.nanotick_for_row(row);
                let notes: Vec<_> = self.view.clip_notes[track]
                    .iter()
                    .filter(|note| note.nanotick == nanotick)
                    .collect();
                let chords: Vec<_> = self.view.clip_chords[track]
                    .iter()
                    .filter(|chord| chord.nanotick == nanotick)
                    .collect();
                let pending_notes: Vec<_> = self.view.pending_notes
                    .iter()
                    .filter(|note| note.track_id as usize == track && note.nanotick == nanotick)
                    .collect();
                let pending_chords: Vec<_> = self.view.pending_chords
                    .iter()
                    .filter(|chord| chord.track_id as usize == track && chord.nanotick == nanotick)
                    .collect();
                let note_count = notes.len();
                let pending_note_count = pending_notes.len();
                let total = note_count + chords.len() + pending_note_count + pending_chords.len();
                let columns = self.view.track_columns[track];
                if total > columns {
                    return Err(anyhow::anyhow!(
                        "entries exceed columns at row {} (track {}, columns {}): notes={}, chords={}, pending_notes={}, pending_chords={}",
                        row,
                        track,
                        columns,
                        note_count,
                        chords.len(),
                        pending_note_count,
                        pending_chords.len()
                    ));
                }
                let mut seen = std::collections::HashSet::new();
                for note in notes {
                    if !seen.insert(note.column) {
                        return Err(anyhow::anyhow!(
                            "duplicate note column at row {} (track {}): column {}",
                            row,
                            track,
                            note.column
                        ));
                    }
                }
                for chord in chords {
                    if !seen.insert(chord.column) {
                        return Err(anyhow::anyhow!(
                            "duplicate chord column at row {} (track {}): column {}",
                            row,
                            track,
                            chord.column
                        ));
                    }
                }
                for note in pending_notes {
                    if !seen.insert(note.column) {
                        return Err(anyhow::anyhow!(
                            "duplicate pending note column at row {} (track {}): column {}",
                            row,
                            track,
                            note.column
                        ));
                    }
                }
                for chord in pending_chords {
                    if !seen.insert(chord.column) {
                        return Err(anyhow::anyhow!(
                            "duplicate pending chord column at row {} (track {}): column {}",
                            row,
                            track,
                            chord.column
                        ));
                    }
                }
                Ok(())
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
                let mut snap_notes = snapshot.notes[note_start..note_end]
                    .iter()
                    .filter(|note| note.t_on == nanotick)
                    .map(|note| (note.column, note.pitch))
                    .collect::<Vec<_>>();
                snap_notes.sort();
                let mut view_notes = self.notes_at_row(track, row)
                    .into_iter()
                    .map(|note| (note.column, note.pitch))
                    .collect::<Vec<_>>();
                view_notes.sort();
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

            fn assert_harmony_matches_snapshot_row(&self, row: i64) -> anyhow::Result<()> {
                let snapshot = self.bridge.read_harmony_snapshot()
                    .ok_or_else(|| anyhow::anyhow!("missing harmony snapshot"))?;
                let nanotick = self.nanotick_for_row(row);
                let snap_event = snapshot.events[..snapshot.event_count as usize]
                    .iter()
                    .find(|event| event.nanotick == nanotick)
                    .map(|event| (event.root, event.scale_id));
                let view_event = self.view.harmony_events
                    .iter()
                    .find(|event| event.nanotick == nanotick)
                    .map(|event| (event.root, event.scale_id));
                if snap_event != view_event {
                    return Err(anyhow::anyhow!(
                        "harmony snapshot mismatch at row {}: snapshot={:?} view={:?}",
                        row,
                        snap_event,
                        view_event
                    ));
                }
                Ok(())
            }

            fn wait_for_note_at_row(
                &mut self,
                track: usize,
                row: i64,
                pitch: u8,
                timeout: Duration,
            ) -> anyhow::Result<Duration> {
                let start = Instant::now();
                loop {
                    let notes = self.notes_at_row(track, row);
                    if notes.iter().any(|note| note.pitch == pitch) {
                        return Ok(start.elapsed());
                    }
                    let _ = self.pump(Duration::from_millis(20));
                    if start.elapsed() > timeout {
                        return Err(anyhow::anyhow!(
                            "timed out waiting for note at track {} row {} pitch {}",
                            track,
                            row,
                            pitch
                        ));
                    }
                }
            }
        }

        impl Drop for TestHarness {
            fn drop(&mut self) {
                self._engine.stop();
                cleanup_shm(&self.shm_name);
            }
        }

        fn test_lock() -> MutexGuard<'static, ()> {
            static LOCK: OnceLock<Mutex<()>> = OnceLock::new();
            LOCK.get_or_init(|| Mutex::new(()))
                .lock()
                .unwrap_or_else(|err| err.into_inner())
        }

        fn cleanup_shm(name: &str) {
            if let Ok(c_name) = CString::new(name) {
                unsafe {
                    libc::shm_unlink(c_name.as_ptr());
                }
            }
        }

        fn cleanup_socket(path: &str) {
            let _ = std::fs::remove_file(path);
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

        fn run_with_large_stack<F>(f: F) -> anyhow::Result<()>
        where
            F: FnOnce() -> anyhow::Result<()> + Send + 'static,
        {
            let handle = thread::Builder::new()
                .stack_size(16 * 1024 * 1024)
                .spawn(f)?;
            match handle.join() {
                Ok(result) => result,
                Err(err) => std::panic::resume_unwind(err),
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
            harness.view.cursor_nanotick = 0;
            harness.view.scroll_nanotick_offset = 0;
            harness.view.focused_track_index = 0;
            harness.view.cursor_col = 0;
            harness.view.harmony_focus = false;

            let pitches = [("q", 60), ("w", 62), ("e", 64), ("r", 65)];
            for (index, (key, expected_pitch)) in pitches.iter().enumerate() {
                harness.press_key(key);
                let pump = harness.pump(Duration::from_millis(300));
                assert!(!pump.clip_resync, "unexpected clip resync on {}", key);
                assert_eq!(harness.view.cursor_view_row(), 1, "cursor should move down after {}", key);

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
            assert_eq!(harness.view.cursor_view_row(), 1, "cursor should move down after delete");
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
        fn test_tracker_rapid_sequence_insert_delete() -> anyhow::Result<()> {
            let mut harness = TestHarness::new("rapid_sequence_insert_delete")?;
            harness.view.cursor_nanotick = 0;
            harness.view.scroll_nanotick_offset = 0;
            harness.view.focused_track_index = 0;
            harness.view.cursor_col = 0;
            harness.view.harmony_focus = false;

            let sequence = ["q", "w", "e", "q", "w", "e", "q", "w", "e", "q", "w", "e"];
            for key in sequence.iter() {
                harness.press_key(key);
            }
            let pump = harness.pump(Duration::from_millis(600));
            assert!(!pump.clip_resync, "unexpected clip resync on rapid input");
            let (_, _, send_fail) = ui_cmd_counters();
            assert_eq!(send_fail, 0, "unexpected UI command send failures");

            for (row, key) in sequence.iter().enumerate() {
                let expected_pitch = match *key {
                    "q" => 60,
                    "w" => 62,
                    "e" => 64,
                    _ => 0,
                };
                let notes = harness.notes_at_row(0, row as i64);
                assert_eq!(notes.len(), 1, "expected one note at row {row}");
                assert_eq!(notes[0].pitch, expected_pitch, "pitch mismatch at row {row}");
                harness.assert_view_matches_snapshot_row(0, row as i64)?;
            }

            harness.view.cursor_nanotick = 0;
            for _ in 0..sequence.len() {
                harness.action_palette_backspace();
            }
            let pump = harness.pump(Duration::from_millis(600));
            assert!(!pump.clip_resync, "unexpected clip resync on rapid delete");
            let (_, _, send_fail) = ui_cmd_counters();
            assert_eq!(send_fail, 0, "unexpected UI command send failures");

            for row in 0..sequence.len() {
                let notes = harness.notes_at_row(0, row as i64);
                assert!(notes.is_empty(), "expected empty row {row} after delete");
                harness.assert_view_matches_snapshot_row(0, row as i64)?;
            }

            Ok(())
        }

        #[test]
        fn test_tracker_backspace_clears_full_column_fast() -> anyhow::Result<()> {
            let mut harness = TestHarness::new("backspace_full_column_fast")?;
            harness.view.cursor_nanotick = 0;
            harness.view.scroll_nanotick_offset = 0;
            harness.view.focused_track_index = 0;
            harness.view.cursor_col = 0;
            harness.view.harmony_focus = false;

            let pattern = ["q", "w", "e"];
            let rows = 24;
            for row in 0..rows {
                let key = pattern[row % pattern.len()];
                harness.press_key(key);
            }
            let pump = harness.pump(Duration::from_millis(800));
            assert!(!pump.clip_resync, "unexpected clip resync on fill");
            let (_, _, send_fail) = ui_cmd_counters();
            assert_eq!(send_fail, 0, "unexpected UI command send failures");

            harness.view.cursor_nanotick = 0;
            for _ in 0..rows {
                harness.action_palette_backspace();
            }
            let pump = harness.pump(Duration::from_millis(800));
            assert!(!pump.clip_resync, "unexpected clip resync on rapid backspace");
            let (_, _, send_fail) = ui_cmd_counters();
            assert_eq!(send_fail, 0, "unexpected UI command send failures");

            for row in 0..rows {
                let notes = harness.notes_at_row(0, row as i64);
                assert!(notes.is_empty(), "expected empty row {row}");
                harness.assert_view_matches_snapshot_row(0, row as i64)?;
            }

            Ok(())
        }

        #[test]
        fn test_tracker_backspace_interleaved_with_qwe() -> anyhow::Result<()> {
            run_with_large_stack(|| {
                let mut harness = TestHarness::new("backspace_interleaved_qwe")?;
                harness.view.cursor_nanotick = 0;
                harness.view.scroll_nanotick_offset = 0;
                harness.view.focused_track_index = 0;
                harness.view.cursor_col = 0;
                harness.view.harmony_focus = false;

                let start = Instant::now();
                let mut clip_snapshot = harness.bridge.read_clip_snapshot()
                    .ok_or_else(|| anyhow::anyhow!("missing clip snapshot"))?;
                while clip_snapshot.track_count < 2 && start.elapsed() < Duration::from_secs(1) {
                    if let Some(snapshot) = harness.bridge.read_clip_snapshot() {
                        clip_snapshot = snapshot;
                        if clip_snapshot.track_count >= 2 {
                            break;
                        }
                    }
                    thread::sleep(Duration::from_millis(20));
                }
                assert!(
                    clip_snapshot.track_count >= 2,
                    "expected at least 2 tracks, got {}",
                    clip_snapshot.track_count
                );

                let pattern = ["q", "w", "e"];
                let rows = 24;
                for row in 0..rows {
                    let key = pattern[row % pattern.len()];
                    harness.press_key(key);
                }
                let pump = harness.pump(Duration::from_millis(800));
                assert!(!pump.clip_resync, "unexpected clip resync on fill");
                let (_, _, send_fail) = ui_cmd_counters();
                assert_eq!(send_fail, 0, "unexpected UI command send failures");

                for row in 0..rows {
                    harness.view.focus_note_cell(row, 0, 0, &mut harness.notify);
                    harness.action_palette_backspace();
                    harness.view.focus_note_cell(row, 1, 0, &mut harness.notify);
                    let key = pattern[row % pattern.len()];
                    harness.press_key(key);
                }
                let pump = harness.pump(Duration::from_millis(800));
                assert!(!pump.clip_resync, "unexpected clip resync on interleaved edits");
                let (_, _, send_fail) = ui_cmd_counters();
                assert_eq!(send_fail, 0, "unexpected UI command send failures");

                for row in 0..rows {
                    let notes = harness.notes_at_row(0, row as i64);
                    assert!(notes.is_empty(), "expected empty row {row} on track 0");
                    let notes = harness.notes_at_row(1, row as i64);
                    assert_eq!(notes.len(), 1, "expected one note at row {row} on track 1");
                }

                Ok(())
            })
        }

        #[test]
        fn test_tracker_degree_and_chord_entries() -> anyhow::Result<()> {
            let mut harness = TestHarness::new("degree_and_chords")?;
            harness.view.cursor_nanotick = 0;
            harness.view.scroll_nanotick_offset = 0;
            harness.view.focused_track_index = 0;
            harness.view.cursor_col = 0;

            harness.press_key("1");
            let pump = harness.pump(Duration::from_millis(300));
            assert!(!pump.clip_resync, "unexpected clip resync on degree note");
            assert_eq!(harness.view.cursor_view_row(), 1, "cursor should move down after degree");
            let chords = harness.chords_at_row(0, 0);
            assert_eq!(chords.len(), 1, "expected degree chord at row 0");
            assert_eq!(chords[0].degree, 1);
            harness.assert_entries_fit_columns(0, 0)?;

            harness.action_palette_up();
            harness.action_palette_backspace();
            let pump = harness.pump(Duration::from_millis(300));
            assert!(!pump.clip_resync, "unexpected clip resync on chord delete");
            let chords = harness.chords_at_row(0, 0);
            assert!(chords.is_empty(), "degree chord should be deleted");
            harness.assert_entries_fit_columns(0, 0)?;
            harness.action_palette_up();

            harness.press_key("2");
            let pump = harness.pump(Duration::from_millis(300));
            assert!(!pump.clip_resync, "unexpected clip resync on degree note 2");
            let chords = harness.chords_at_row(0, 0);
            assert_eq!(chords.len(), 1, "expected degree 2 chord at row 0");
            assert_eq!(chords[0].degree, 2);
            harness.assert_entries_fit_columns(0, 0)?;

            harness.action_palette_up();
            harness.action_palette_backspace();
            let pump = harness.pump(Duration::from_millis(300));
            assert!(!pump.clip_resync, "unexpected clip resync on chord delete");
            let chords = harness.chords_at_row(0, 0);
            assert!(chords.is_empty(), "degree 2 chord should be deleted");
            harness.assert_entries_fit_columns(0, 0)?;
            harness.action_palette_up();

            harness.press_key("@");
            harness.press_key("3");
            harness.action_palette_confirm();
            let entry_count = harness.entry_count_at_row(0, 0);
            assert_eq!(entry_count, 1, "expected pending chord immediately");
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
        fn test_tracker_shift_up_down_expands_selection() -> anyhow::Result<()> {
            let mut harness = TestHarness::new("shift_expand_rows")?;
            harness.view.cursor_nanotick = harness.view.view_row_nanotick(2);
            harness.view.focused_track_index = 0;
            harness.view.cursor_col = 0;

            harness.view.expand_selection_rows(1, &mut harness.notify);

            let (start, end) = harness.view.selection_bounds()
                .ok_or_else(|| anyhow::anyhow!("missing selection after shift-down"))?;
            assert_eq!(start, harness.view.view_row_nanotick(2));
            assert_eq!(end, harness.view.view_row_nanotick(3));
            Ok(())
        }

        #[test]
        fn test_tracker_shift_left_right_expands_columns() -> anyhow::Result<()> {
            let mut harness = TestHarness::new("shift_expand_columns")?;
            harness.view.track_columns[0] = 3;
            harness.view.focused_track_index = 0;
            harness.view.cursor_col = 0;

            harness.view.expand_selection_columns(1, &mut harness.notify);

            let mask = harness.view.selection_mask.tracks[0];
            assert_eq!(mask & 0b11, 0b11, "expected columns 0 and 1 in mask");
            assert_eq!(harness.view.cursor_col, 1);
            Ok(())
        }

        #[test]
        fn test_tracker_cmd_shift_bar_expands_selection() -> anyhow::Result<()> {
            let mut harness = TestHarness::new("cmd_shift_bar_expand")?;
            harness.view.cursor_nanotick = harness.view.view_row_nanotick(1);
            harness.view.focused_track_index = 0;
            harness.view.cursor_col = 0;

            harness.view.expand_selection_to_bar(1, &mut harness.notify);

            let (start, end) = harness.view.selection_bounds()
                .ok_or_else(|| anyhow::anyhow!("missing selection after bar expand"))?;
            let bar_len = BEATS_PER_BAR * NANOTICKS_PER_QUARTER;
            assert_eq!(start, harness.view.view_row_nanotick(1));
            assert_eq!(end, bar_len);
            Ok(())
        }

        #[test]
        fn test_tracker_set_loop_range_limits_playback() -> anyhow::Result<()> {
            let mut harness = TestHarness::new("loop_range_limits_playback")?;
            harness.view.cursor_nanotick = 0;
            harness.view.scroll_nanotick_offset = 0;
            harness.view.focused_track_index = 0;
            harness.view.cursor_col = 0;

            harness.view.focus_note_cell(0, 0, 0, &mut harness.notify);
            harness.press_key("q"); // C-4 at row 0
            let pump = harness.pump(Duration::from_millis(200));
            assert!(!pump.clip_resync, "unexpected clip resync on row 0 note");

            harness.view.focus_note_cell(4, 0, 0, &mut harness.notify);
            harness.press_key("w"); // D-4 at row 4
            let pump = harness.pump(Duration::from_millis(200));
            assert!(!pump.clip_resync, "unexpected clip resync on row 4 note");

            let loop_start = harness.view.view_row_nanotick(4);
            let loop_end = harness.view.view_row_nanotick(8);
            harness.set_loop_range(loop_start, loop_end);
            let _ = harness.pump(Duration::from_millis(100));

            let track_shm = open_track_shm(TRACK_SHM_TIMEOUT)?;
            while ring_pop(&track_shm.ring_std).is_some() {}

            harness.view.toggle_play(&mut harness.notify);
            let _ = harness.pump(Duration::from_millis(200));

            let sample_rate = unsafe { (*track_shm.header).sample_rate };
            let loop_ticks = loop_end - loop_start;
            let loop_samples = pattern_samples(sample_rate, loop_ticks);
            let mut pitches: Vec<u8> = Vec::new();
            let mut first_note_sample: Option<u64> = None;
            let mut stop_after: Option<u64> = None;
            let start = Instant::now();
            while start.elapsed() < Duration::from_secs(2) {
                if let Some(entry) = ring_pop(&track_shm.ring_std) {
                    if let Some(payload) = read_midi_payload(&entry) {
                        if payload.status == 0x90 && payload.data2 > 0 {
                            let base = first_note_sample.get_or_insert(entry.sample_time);
                            if stop_after.is_none() {
                                stop_after = Some(*base + loop_samples * 2);
                            }
                            pitches.push(payload.data1);
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

            assert!(!pitches.is_empty(), "no MIDI events captured");
            assert!(
                pitches.iter().all(|pitch| *pitch == 62),
                "expected only loop-range notes, got {:?}",
                pitches
            );

            Ok(())
        }

        #[test]
        fn test_note_off_on_new_note_same_column() -> anyhow::Result<()> {
            let mut harness = TestHarness::new("note_off_on_new_note")?;
            harness.view.cursor_nanotick = 0;
            harness.view.scroll_nanotick_offset = 0;
            harness.view.focused_track_index = 0;
            harness.view.cursor_col = 0;

            harness.press_key("q"); // C-4 at row 0
            for _ in 0..3 {
                harness.action_palette_down();
            }
            harness.press_key("w"); // D-4 at row 4
            let pump = harness.pump(Duration::from_millis(300));
            assert!(!pump.clip_resync, "unexpected clip resync on note entry");

            let track_shm = open_track_shm(TRACK_SHM_TIMEOUT)?;
            while ring_pop(&track_shm.ring_std).is_some() {}

            harness.view.toggle_play(&mut harness.notify);
            let _ = harness.pump(Duration::from_millis(200));

            #[derive(Clone, Copy, Debug)]
            struct MidiEvent {
                sample_time: u64,
                payload: MidiPayload,
            }

            let mut note_ons = Vec::new();
            let mut note_offs = Vec::new();
            let start = Instant::now();
            while start.elapsed() < Duration::from_secs(2) {
                if let Some(entry) = ring_pop(&track_shm.ring_std) {
                    if let Some(payload) = read_midi_payload(&entry) {
                        if payload.status == 0x90 && payload.data2 > 0 {
                            note_ons.push(MidiEvent {
                                sample_time: entry.sample_time,
                                payload,
                            });
                        } else if payload.status == 0x80 {
                            note_offs.push(MidiEvent {
                                sample_time: entry.sample_time,
                                payload,
                            });
                        }
                    }
                } else {
                    thread::sleep(Duration::from_millis(1));
                }
                if note_ons.len() >= 2 &&
                    note_offs.iter().any(|off| off.payload.note_id == note_ons[0].payload.note_id) {
                    break;
                }
            }

            assert!(note_ons.len() >= 2, "expected at least two note-ons");
            let first_on = note_ons[0];
            let second_on = note_ons[1];
            let first_off = note_offs
                .iter()
                .find(|off| off.payload.note_id == first_on.payload.note_id)
                .ok_or_else(|| anyhow::anyhow!("missing note-off for first note"))?;
            assert_eq!(
                first_off.sample_time,
                second_on.sample_time,
                "note-off should align with next note-on"
            );

            Ok(())
        }

        #[test]
        fn test_note_cut_by_chord_in_same_column() -> anyhow::Result<()> {
            let mut harness = TestHarness::new("note_cut_by_chord")?;
            harness.view.cursor_nanotick = 0;
            harness.view.scroll_nanotick_offset = 0;
            harness.view.focused_track_index = 0;
            harness.view.cursor_col = 0;

            harness.view.focus_harmony_row(0, &mut harness.notify);
            harness.press_key("c");
            let pump = harness.pump(Duration::from_millis(300));
            assert!(!pump.harmony_resync, "unexpected harmony resync on C");
            harness.assert_harmony_matches_snapshot_row(0)?;

            harness.view.focus_note_cell(0, 0, 0, &mut harness.notify);
            harness.press_key("q"); // C-4 at row 0
            harness.view.focus_note_cell(1, 0, 0, &mut harness.notify);
            harness.press_key("@");
            harness.press_key("3");
            harness.action_palette_confirm(); // @3 chord at row 1
            let pump = harness.pump(Duration::from_millis(300));
            assert!(!pump.clip_resync, "unexpected clip resync on note/chord entry");
            let notes = harness.notes_at_row(0, 0);
            assert_eq!(notes.len(), 1, "expected note at row 0");
            assert_eq!(notes[0].column, 0, "expected note column 0");
            let chords = harness.chords_at_row(0, 1);
            assert_eq!(chords.len(), 1, "expected chord at row 1");

            let track_shm = open_track_shm(TRACK_SHM_TIMEOUT)?;
            while ring_pop(&track_shm.ring_std).is_some() {}

            harness.view.toggle_play(&mut harness.notify);
            let _ = harness.pump(Duration::from_millis(10));

            let mut first_on: Option<(u64, MidiPayload)> = None;
            let mut chord_on_times: Vec<u64> = Vec::new();
            let mut first_off: Option<u64> = None;
            let start = Instant::now();
            while start.elapsed() < Duration::from_secs(2) {
                if let Some(entry) = ring_pop(&track_shm.ring_std) {
                    if let Some(payload) = read_midi_payload(&entry) {
                        if payload.status == 0x90 && payload.data2 > 0 {
                            if first_on.is_none() && payload.data1 == 60 {
                                first_on = Some((entry.sample_time, payload));
                            } else if let Some((first_on_time, _)) = first_on {
                                if payload.data1 != 60 && entry.sample_time > first_on_time {
                                    chord_on_times.push(entry.sample_time);
                                }
                            }
                        } else if payload.status == 0x80 {
                            if let Some((_, first_payload)) = first_on {
                                if payload.note_id == first_payload.note_id {
                                    first_off = Some(entry.sample_time);
                                }
                            }
                        }
                    }
                } else {
                    thread::sleep(Duration::from_millis(1));
                }
                if !chord_on_times.is_empty() && first_off.is_some() {
                    break;
                }
            }

            if chord_on_times.is_empty() {
                return Err(anyhow::anyhow!("missing chord note-ons after first note"));
            }
            let (first_on_time, _) =
                first_on.ok_or_else(|| anyhow::anyhow!("missing first note-on"))?;
            let first_off_time =
                first_off.ok_or_else(|| anyhow::anyhow!("missing note-off for first note"))?;
            let chord_on_time = *chord_on_times.iter().min().unwrap();
            assert!(
                first_off_time == chord_on_time,
                "note-off should align with chord note-on (on={}, off={}, chord={})",
                first_on_time,
                first_off_time,
                chord_on_time
            );

            Ok(())
        }

        #[test]
        fn test_degree_notes_cut_on_next_degree() -> anyhow::Result<()> {
            let mut harness = TestHarness::new("degree_notes_cut")?;
            harness.view.cursor_nanotick = 0;
            harness.view.scroll_nanotick_offset = 0;
            harness.view.focused_track_index = 0;
            harness.view.cursor_col = 0;

            harness.view.focus_harmony_row(0, &mut harness.notify);
            harness.press_key("c");
            let pump = harness.pump(Duration::from_millis(300));
            assert!(!pump.harmony_resync, "unexpected harmony resync on C");
            harness.assert_harmony_matches_snapshot_row(0)?;

            harness.view.focus_note_cell(0, 0, 0, &mut harness.notify);
            harness.press_key("1");
            harness.press_key("2");
            harness.press_key("3");
            let pump = harness.pump(Duration::from_millis(300));
            assert!(!pump.clip_resync, "unexpected clip resync on degree entry");
            assert_eq!(harness.chords_at_row(0, 0).len(), 1, "expected degree at row 0");
            assert_eq!(harness.chords_at_row(0, 1).len(), 1, "expected degree at row 1");
            assert_eq!(harness.chords_at_row(0, 2).len(), 1, "expected degree at row 2");

            let track_shm = open_track_shm(TRACK_SHM_TIMEOUT)?;
            while ring_pop(&track_shm.ring_std).is_some() {}

            harness.view.toggle_play(&mut harness.notify);
            let _ = harness.pump(Duration::from_millis(10));

            let mut note_ons: Vec<(u64, MidiPayload)> = Vec::new();
            let mut note_offs: Vec<(u64, MidiPayload)> = Vec::new();
            let start = Instant::now();
            while start.elapsed() < Duration::from_secs(2) {
                if let Some(entry) = ring_pop(&track_shm.ring_std) {
                    if let Some(payload) = read_midi_payload(&entry) {
                        if payload.status == 0x90 && payload.data2 > 0 {
                            note_ons.push((entry.sample_time, payload));
                        } else if payload.status == 0x80 {
                            note_offs.push((entry.sample_time, payload));
                        }
                    }
                } else {
                    thread::sleep(Duration::from_millis(1));
                }
                if note_ons.len() >= 3 {
                    let first = note_ons[0].1.note_id;
                    let second = note_ons[1].1.note_id;
                    if note_offs.iter().any(|off| off.1.note_id == first) &&
                        note_offs.iter().any(|off| off.1.note_id == second) {
                        break;
                    }
                }
            }

            assert!(note_ons.len() >= 3, "expected at least three note-ons");
            let first_on = note_ons[0];
            let second_on = note_ons[1];
            let third_on = note_ons[2];
            assert_eq!(first_on.1.data1, 60, "expected degree 1 pitch");
            assert_eq!(second_on.1.data1, 62, "expected degree 2 pitch");
            assert_eq!(third_on.1.data1, 64, "expected degree 3 pitch");

            let first_off = note_offs
                .iter()
                .find(|off| off.1.note_id == first_on.1.note_id)
                .ok_or_else(|| anyhow::anyhow!("missing note-off for degree 1"))?;
            let second_off = note_offs
                .iter()
                .find(|off| off.1.note_id == second_on.1.note_id)
                .ok_or_else(|| anyhow::anyhow!("missing note-off for degree 2"))?;
            assert_eq!(
                first_off.0, second_on.0,
                "degree 1 note-off should align with degree 2 note-on"
            );
            assert_eq!(
                second_off.0, third_on.0,
                "degree 2 note-off should align with degree 3 note-on"
            );

            Ok(())
        }

        #[test]
        fn test_chord_sustains_until_next_note() -> anyhow::Result<()> {
            let mut harness = TestHarness::new("chord_sustain_until_note")?;
            harness.view.cursor_nanotick = 0;
            harness.view.scroll_nanotick_offset = 0;
            harness.view.focused_track_index = 0;
            harness.view.cursor_col = 0;

            harness.view.focus_harmony_row(0, &mut harness.notify);
            harness.press_key("c");
            let pump = harness.pump(Duration::from_millis(300));
            assert!(!pump.harmony_resync, "unexpected harmony resync on C");
            harness.assert_harmony_matches_snapshot_row(0)?;

            harness.view.focus_note_cell(0, 0, 0, &mut harness.notify);
            harness.press_key("@");
            harness.press_key("3");
            harness.action_palette_confirm(); // @3 chord at row 0
            harness.action_palette_down();
            harness.press_key("q"); // C-4 at row 1
            let pump = harness.pump(Duration::from_millis(300));
            assert!(!pump.clip_resync, "unexpected clip resync on chord/note entry");
            let notes = harness.notes_at_row(0, 1);
            assert_eq!(notes.len(), 1, "expected note at row 1");
            assert_eq!(notes[0].column, 0, "expected note column 0");
            let chords = harness.chords_at_row(0, 0);
            assert_eq!(chords.len(), 1, "expected chord at row 0");

            let track_shm = open_track_shm(TRACK_SHM_TIMEOUT)?;
            while ring_pop(&track_shm.ring_std).is_some() {}

            harness.view.toggle_play(&mut harness.notify);
            let _ = harness.pump(Duration::from_millis(10));

            let mut chord_note_ids: Vec<u32> = Vec::new();
            let mut chord_offs: Vec<(u32, u64)> = Vec::new();
            let mut chord_on_time: Option<u64> = None;
            let mut next_note_on: Option<u64> = None;
            let start = Instant::now();
            while start.elapsed() < Duration::from_secs(2) {
                if let Some(entry) = ring_pop(&track_shm.ring_std) {
                    if let Some(payload) = read_midi_payload(&entry) {
                        if payload.status == 0x90 && payload.data2 > 0 {
                            if payload.data1 != 60 && next_note_on.is_none() {
                                chord_note_ids.push(payload.note_id);
                                chord_on_time.get_or_insert(entry.sample_time);
                            } else if payload.data1 == 60 &&
                                chord_on_time.map_or(false, |time| entry.sample_time > time) &&
                                next_note_on.is_none() {
                                next_note_on = Some(entry.sample_time);
                            }
                        } else if payload.status == 0x80 {
                            if chord_note_ids.contains(&payload.note_id) {
                                chord_offs.push((payload.note_id, entry.sample_time));
                            }
                        }
                    }
                } else {
                    thread::sleep(Duration::from_millis(1));
                }
                if let Some(next_time) = next_note_on {
                    if chord_note_ids.len() > 0 &&
                        chord_offs.len() >= chord_note_ids.len() &&
                        chord_offs.iter().all(|(_, off_time)| *off_time == next_time) {
                        break;
                    }
                }
            }

            let next_time =
                next_note_on.ok_or_else(|| anyhow::anyhow!("missing next note-on"))?;
            assert!(
                !chord_note_ids.is_empty(),
                "expected chord note-ons before next note"
            );
            for note_id in chord_note_ids {
                let off_time = chord_offs
                    .iter()
                    .find(|(id, _)| *id == note_id)
                    .map(|(_, time)| *time)
                    .ok_or_else(|| anyhow::anyhow!("missing note-off for chord note {note_id}"))?;
                assert_eq!(
                    off_time,
                    next_time,
                    "chord note-off should align with next note-on"
                );
            }

            Ok(())
        }

        #[test]
        fn test_tracker_cell_edit_midi_note() -> anyhow::Result<()> {
            let mut harness = TestHarness::new("cell_edit_midi")?;
            harness.view.cursor_nanotick = 0;
            harness.view.scroll_nanotick_offset = 0;
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
            harness.view.cursor_nanotick = 0;
            harness.view.scroll_nanotick_offset = 0;
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
            harness.view.cursor_nanotick = 0;
            harness.view.scroll_nanotick_offset = 0;
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
                harness.assert_entries_fit_columns(0, row as i64)?;
            }

            Ok(())
        }

        #[test]
        fn test_tracker_edit_scroll_stress() -> anyhow::Result<()> {
            let mut harness = TestHarness::new("edit_scroll_stress")?;
            harness.view.cursor_nanotick = 0;
            harness.view.scroll_nanotick_offset = 0;
            harness.view.focused_track_index = 0;
            harness.view.cursor_col = 0;

            let keys = ["q", "w", "e", "r"];
            for row in 0..20_i64 {
                let key = keys[(row as usize) % keys.len()];
                harness.press_key(key);
                let pump = harness.pump(Duration::from_millis(200));
                assert!(!pump.clip_resync, "unexpected clip resync on {}", key);
                harness.assert_entries_fit_columns(0, row)?;
            }

            for _ in 0..20 {
                harness.action_palette_up();
            }

            for row in 0..20_i64 {
                harness.action_palette_backspace();
                let pump = harness.pump(Duration::from_millis(200));
                assert!(!pump.clip_resync, "unexpected clip resync on delete");
                harness.assert_entries_fit_columns(0, row)?;
            }

            Ok(())
        }

        #[test]
        fn test_tracker_latency_under_playback() -> anyhow::Result<()> {
            let mut harness = TestHarness::new("latency_under_playback")?;
            harness.view.cursor_nanotick = 0;
            harness.view.scroll_nanotick_offset = 0;
            harness.view.focused_track_index = 0;
            harness.view.cursor_col = 0;

            let budget_ms: u64 = std::env::var("DAW_TEST_UI_LATENCY_MS")
                .ok()
                .and_then(|value| value.parse().ok())
                .unwrap_or(200);

            harness.view.toggle_play(&mut harness.notify);
            let _ = harness.pump(Duration::from_millis(100));

            let sequence = [("q", 60_u8), ("w", 62), ("e", 64), ("r", 65)];
            let mut max_latency = Duration::from_millis(0);
            for (row, (key, pitch)) in sequence.iter().cycle().take(8).enumerate() {
                harness.view.focus_note_cell(row, 0, 0, &mut harness.notify);
                harness.press_key(key);
                let latency = harness.wait_for_note_at_row(0, row as i64, *pitch,
                                                          Duration::from_millis(200))?;
                max_latency = max_latency.max(latency);
                harness.assert_view_matches_snapshot_row(0, row as i64)?;
            }
            eprintln!(
                "tracker latency under playback: max={:?} budget={}ms",
                max_latency,
                budget_ms
            );
            assert!(
                max_latency <= Duration::from_millis(budget_ms),
                "edit latency too high: {:?}",
                max_latency
            );
            Ok(())
        }

        #[test]
        fn test_tracker_burst_under_playback_no_resync() -> anyhow::Result<()> {
            let mut harness = TestHarness::new("burst_under_playback")?;
            harness.view.cursor_nanotick = 0;
            harness.view.scroll_nanotick_offset = 0;
            harness.view.focused_track_index = 0;
            harness.view.cursor_col = 0;

            let pump_ms: u64 = std::env::var("DAW_TEST_UI_BURST_PUMP_MS")
                .ok()
                .and_then(|value| value.parse().ok())
                .unwrap_or(500);

            harness.view.toggle_play(&mut harness.notify);
            let _ = harness.pump(Duration::from_millis(100));

            let sequence = [("q", 60_u8), ("w", 62), ("e", 64), ("r", 65)];
            for row in 0..16 {
                let (key, _pitch) = sequence[row % sequence.len()];
                harness.view.focus_note_cell(row, 0, 0, &mut harness.notify);
                harness.press_key(key);
            }

            let pump = harness.pump(Duration::from_millis(pump_ms));
            assert!(!pump.clip_resync, "unexpected clip resync during burst");
            for row in 0..16_i64 {
                harness.assert_view_matches_snapshot_row(0, row)?;
            }
            Ok(())
        }

        #[test]
        fn test_tracker_worst_case_throttle_edits() -> anyhow::Result<()> {
            std::env::set_var("DAW_ENGINE_TEST_THROTTLE_MS", "50");
            let result = (|| {
                let mut harness = TestHarness::new("worst_case_throttle")?;
                harness.view.cursor_nanotick = 0;
                harness.view.scroll_nanotick_offset = 0;
                harness.view.focused_track_index = 0;
                harness.view.cursor_col = 0;

                harness.view.toggle_play(&mut harness.notify);
                let _ = harness.pump(Duration::from_millis(100));

                let sequence = [("q", 60_u8), ("w", 62), ("e", 64), ("r", 65)];
                let mut max_latency = Duration::from_millis(0);
                for row in 0..24 {
                    let (key, pitch) = sequence[row % sequence.len()];
                    harness.view.focus_note_cell(row, 0, 0, &mut harness.notify);
                    harness.press_key(key);
                    let latency = harness.wait_for_note_at_row(
                        0,
                        row as i64,
                        pitch,
                        Duration::from_millis(250),
                    )?;
                    max_latency = max_latency.max(latency);
                }
                eprintln!(
                    "tracker worst-case throttle latency: max={:?}",
                    max_latency
                );
                Ok(())
            })();
            std::env::remove_var("DAW_ENGINE_TEST_THROTTLE_MS");
            result
        }

        #[test]
        fn test_load_plugin_track1_does_not_crash() -> anyhow::Result<()> {
            let mut harness = TestHarness::new("load_plugin_track1")?;
            let normalize = |value: &str| {
                value.chars()
                    .filter(|ch| ch.is_ascii_alphanumeric())
                    .flat_map(|ch| ch.to_lowercase())
                    .collect::<String>()
            };
            let plugin_index = if let Ok(value) = env::var("DAW_TEST_PLUGIN_INDEX") {
                value.parse::<usize>().ok()
            } else if let Ok(name) = env::var("DAW_TEST_PLUGIN_NAME") {
                let name_norm = normalize(&name);
                harness.view.plugins.iter()
                    .find(|entry| normalize(&entry.name).contains(&name_norm))
                    .map(|entry| entry.index)
            } else {
                harness.view.plugins.first().map(|entry| entry.index)
            }.ok_or_else(|| anyhow::anyhow!("no plugin available for load test"))?;

            harness.load_plugin_on_track(1, plugin_index as u32);
            let _ = harness.pump(Duration::from_millis(500));
            harness.assert_engine_alive()?;

            // Retry a second load to stress the host launch path.
            harness.load_plugin_on_track(1, plugin_index as u32);
            let _ = harness.pump(Duration::from_millis(500));
            harness.assert_engine_alive()?;

            Ok(())
        }

        #[test]
        fn test_load_plugin_track1_via_palette_does_not_crash() -> anyhow::Result<()> {
            let mut harness = TestHarness::new("load_plugin_track1_palette")?;
            harness.view.focused_track_index = 1;

            let query = if let Ok(name) = env::var("DAW_TEST_PLUGIN_NAME") {
                name.chars()
                    .filter(|ch| !ch.is_whitespace())
                    .collect::<String>()
            } else {
                harness.view.plugins.first()
                    .map(|entry| entry.name.clone())
                    .unwrap_or_default()
            };
            if query.is_empty() {
                return Err(anyhow::anyhow!("no plugin available for palette test"));
            }

            harness.view.toggle_palette(&mut harness.notify);
            harness.action_palette_confirm();
            for ch in query.chars() {
                let mut key = String::new();
                key.push(ch.to_ascii_lowercase());
                harness.press_key(&key);
            }
            harness.action_palette_confirm();
            let _ = harness.pump(Duration::from_millis(800));
            harness.assert_engine_alive()?;

            Ok(())
        }

        #[test]
        fn test_tracker_harmony_rapid_edits() -> anyhow::Result<()> {
            let mut harness = TestHarness::new("harmony_edits")?;
            harness.view.cursor_nanotick = 0;
            harness.view.scroll_nanotick_offset = 0;
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
        fn test_tracker_harmony_undo_redo() -> anyhow::Result<()> {
            let mut harness = TestHarness::new("harmony_undo_redo")?;
            harness.view.cursor_nanotick = 0;
            harness.view.scroll_nanotick_offset = 0;
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

            harness.send_undo();
            let pump = harness.pump(Duration::from_millis(300));
            assert!(!pump.harmony_resync, "unexpected harmony resync on undo");
            assert_eq!(harness.view.harmony_events.len(), 1);
            assert_eq!(harness.view.harmony_events[0].root, 0);

            harness.send_redo();
            let pump = harness.pump(Duration::from_millis(300));
            assert!(!pump.harmony_resync, "unexpected harmony resync on redo");
            assert_eq!(harness.view.harmony_events.len(), 1);
            assert_eq!(harness.view.harmony_events[0].root, 2);

            harness.action_palette_backspace();
            let pump = harness.pump(Duration::from_millis(300));
            assert!(!pump.harmony_resync, "unexpected harmony resync on delete");
            assert_eq!(harness.view.harmony_events.len(), 0);

            harness.send_undo();
            let pump = harness.pump(Duration::from_millis(300));
            assert!(!pump.harmony_resync, "unexpected harmony resync on undo delete");
            assert_eq!(harness.view.harmony_events.len(), 1);
            assert_eq!(harness.view.harmony_events[0].root, 2);

            harness.send_redo();
            let pump = harness.pump(Duration::from_millis(300));
            assert!(!pump.harmony_resync, "unexpected harmony resync on redo delete");
            assert_eq!(harness.view.harmony_events.len(), 0);

            Ok(())
        }

        #[test]
        fn test_tracker_mixed_undo_redo() -> anyhow::Result<()> {
            let mut harness = TestHarness::new("mixed_undo_redo")?;
            harness.view.cursor_nanotick = 0;
            harness.view.scroll_nanotick_offset = 0;
            harness.view.focused_track_index = 0;
            harness.view.cursor_col = 0;

            harness.view.focus_harmony_row(0, &mut harness.notify);
            harness.press_key("c");
            let pump = harness.pump(Duration::from_millis(300));
            assert!(!pump.harmony_resync, "unexpected harmony resync on C");
            harness.assert_harmony_matches_snapshot_row(0)?;

            harness.view.focus_note_cell(0, 0, 0, &mut harness.notify);
            harness.press_key("q");
            let pump = harness.pump(Duration::from_millis(300));
            assert!(!pump.clip_resync, "unexpected clip resync on note");
            harness.assert_view_matches_snapshot_row(0, 0)?;

            harness.view.focus_harmony_row(1, &mut harness.notify);
            harness.press_key("d");
            let pump = harness.pump(Duration::from_millis(300));
            assert!(!pump.harmony_resync, "unexpected harmony resync on D");
            harness.assert_harmony_matches_snapshot_row(1)?;

            harness.view.focus_note_cell(1, 0, 0, &mut harness.notify);
            harness.press_key("w");
            let pump = harness.pump(Duration::from_millis(300));
            assert!(!pump.clip_resync, "unexpected clip resync on note 2");
            harness.assert_view_matches_snapshot_row(0, 1)?;

            let row0_notes = harness.notes_at_row(0, 0);
            let row1_notes = harness.notes_at_row(0, 1);
            assert_eq!(row0_notes.len(), 1);
            assert_eq!(row1_notes.len(), 1);
            assert_eq!(row0_notes[0].pitch, 60);
            assert_eq!(row1_notes[0].pitch, 62);

            let row0_harmony = harness.view.harmony_events
                .iter()
                .find(|event| event.nanotick == harness.nanotick_for_row(0));
            let row1_harmony = harness.view.harmony_events
                .iter()
                .find(|event| event.nanotick == harness.nanotick_for_row(1));
            assert!(row0_harmony.is_some());
            assert!(row1_harmony.is_some());

            harness.send_undo();
            let pump = harness.pump(Duration::from_millis(300));
            assert!(!pump.clip_resync, "unexpected clip resync on undo note 2");
            assert!(harness.notes_at_row(0, 1).is_empty());
            harness.assert_view_matches_snapshot_row(0, 1)?;

            harness.send_undo();
            let pump = harness.pump(Duration::from_millis(300));
            assert!(!pump.harmony_resync, "unexpected harmony resync on undo harmony 2");
            harness.assert_harmony_matches_snapshot_row(1)?;

            harness.send_undo();
            let pump = harness.pump(Duration::from_millis(300));
            assert!(!pump.clip_resync, "unexpected clip resync on undo note 1");
            assert!(harness.notes_at_row(0, 0).is_empty());
            harness.assert_view_matches_snapshot_row(0, 0)?;

            harness.send_undo();
            let pump = harness.pump(Duration::from_millis(300));
            assert!(!pump.harmony_resync, "unexpected harmony resync on undo harmony 1");
            harness.assert_harmony_matches_snapshot_row(0)?;

            harness.send_redo();
            let pump = harness.pump(Duration::from_millis(300));
            assert!(!pump.harmony_resync, "unexpected harmony resync on redo harmony 1");
            harness.assert_harmony_matches_snapshot_row(0)?;

            harness.send_redo();
            let pump = harness.pump(Duration::from_millis(300));
            assert!(!pump.clip_resync, "unexpected clip resync on redo note 1");
            harness.assert_view_matches_snapshot_row(0, 0)?;

            harness.send_redo();
            let pump = harness.pump(Duration::from_millis(300));
            assert!(!pump.harmony_resync, "unexpected harmony resync on redo harmony 2");
            harness.assert_harmony_matches_snapshot_row(1)?;

            harness.send_redo();
            let pump = harness.pump(Duration::from_millis(300));
            assert!(!pump.clip_resync, "unexpected clip resync on redo note 2");
            harness.assert_view_matches_snapshot_row(0, 1)?;

            Ok(())
        }

        #[test]
        fn test_tracker_mixed_undo_redo_multi_track_columns() -> anyhow::Result<()> {
            run_with_large_stack(|| {
                let mut harness = TestHarness::new("mixed_undo_redo_multi")?;
                harness.view.cursor_nanotick = 0;
                harness.view.scroll_nanotick_offset = 0;
                harness.view.focused_track_index = 0;
                harness.view.cursor_col = 0;

                let clip_snapshot = harness.bridge.read_clip_snapshot()
                    .ok_or_else(|| anyhow::anyhow!("missing clip snapshot"))?;
                let start = Instant::now();
                let mut clip_snapshot = clip_snapshot;
                while clip_snapshot.track_count < 2 && start.elapsed() < Duration::from_secs(1) {
                    if let Some(snapshot) = harness.bridge.read_clip_snapshot() {
                        clip_snapshot = snapshot;
                        if clip_snapshot.track_count >= 2 {
                            break;
                        }
                    }
                    thread::sleep(Duration::from_millis(20));
                }
                assert!(
                    clip_snapshot.track_count >= 2,
                    "expected at least 2 tracks, got {}",
                    clip_snapshot.track_count
                );

                harness.adjust_columns(0, 1);
                harness.adjust_columns(1, 1);
                assert_eq!(harness.view.track_columns[0], 2);
                assert_eq!(harness.view.track_columns[1], 2);

                // Track 0, row 0, col 0
                harness.view.focus_note_cell(0, 0, 0, &mut harness.notify);
                harness.press_key("q");
                let pump = harness.pump(Duration::from_millis(300));
                assert!(!pump.clip_resync, "unexpected clip resync on track 0 col 0");
                harness.assert_view_matches_snapshot_row(0, 0)?;

                // Track 0, row 0, col 1
                harness.view.focus_note_cell(0, 0, 1, &mut harness.notify);
                harness.press_key("w");
                let pump = harness.pump(Duration::from_millis(300));
                assert!(!pump.clip_resync, "unexpected clip resync on track 0 col 1");
                harness.assert_view_matches_snapshot_row(0, 0)?;

                // Track 1, row 0, col 0
                harness.view.focus_note_cell(0, 1, 0, &mut harness.notify);
                harness.press_key("e");
                let pump = harness.pump(Duration::from_millis(300));
                assert!(!pump.clip_resync, "unexpected clip resync on track 1 col 0");
                harness.assert_view_matches_snapshot_row(1, 0)?;

                // Track 1, row 0, col 1
                harness.view.focus_note_cell(0, 1, 1, &mut harness.notify);
                harness.press_key("r");
                let pump = harness.pump(Duration::from_millis(300));
                assert!(!pump.clip_resync, "unexpected clip resync on track 1 col 1");
                harness.assert_view_matches_snapshot_row(1, 0)?;

                let t0_notes = harness.notes_at_row(0, 0);
                assert_eq!(t0_notes.len(), 2, "expected two notes on track 0 row 0");
                let mut t0_cols: Vec<_> = t0_notes.iter().map(|n| n.column).collect();
                t0_cols.sort();
                assert_eq!(t0_cols, vec![0, 1]);

                let t1_notes = harness.notes_at_row(1, 0);
                assert_eq!(t1_notes.len(), 2, "expected two notes on track 1 row 0");
                let mut t1_cols: Vec<_> = t1_notes.iter().map(|n| n.column).collect();
                t1_cols.sort();
                assert_eq!(t1_cols, vec![0, 1]);

                for _ in 0..4 {
                    harness.send_undo();
                    let pump = harness.pump(Duration::from_millis(300));
                    assert!(!pump.clip_resync, "unexpected clip resync on undo");
                }
                assert!(harness.notes_at_row(0, 0).is_empty());
                assert!(harness.notes_at_row(1, 0).is_empty());
                harness.assert_view_matches_snapshot_row(0, 0)?;
                harness.assert_view_matches_snapshot_row(1, 0)?;

                for _ in 0..4 {
                    harness.send_redo();
                    let pump = harness.pump(Duration::from_millis(300));
                    assert!(!pump.clip_resync, "unexpected clip resync on redo");
                }
                let t0_notes = harness.notes_at_row(0, 0);
                let t1_notes = harness.notes_at_row(1, 0);
                assert_eq!(t0_notes.len(), 2);
                assert_eq!(t1_notes.len(), 2);
                harness.assert_view_matches_snapshot_row(0, 0)?;
                harness.assert_view_matches_snapshot_row(1, 0)?;

                Ok(())
            })
        }

        #[test]
        fn test_tracker_undo_redo_clears_mixed_entries() -> anyhow::Result<()> {
            run_with_large_stack(|| {
                let mut harness = TestHarness::new("undo_redo_clear_mixed")?;
                harness.view.cursor_nanotick = 0;
                harness.view.scroll_nanotick_offset = 0;
                harness.view.focused_track_index = 0;
                harness.view.cursor_col = 0;

                let start = Instant::now();
                let mut clip_snapshot = harness.bridge.read_clip_snapshot()
                    .ok_or_else(|| anyhow::anyhow!("missing clip snapshot"))?;
                while clip_snapshot.track_count < 3 && start.elapsed() < Duration::from_secs(1) {
                    if let Some(snapshot) = harness.bridge.read_clip_snapshot() {
                        clip_snapshot = snapshot;
                        if clip_snapshot.track_count >= 3 {
                            break;
                        }
                    }
                    thread::sleep(Duration::from_millis(20));
                }
                assert!(
                    clip_snapshot.track_count >= 3,
                    "expected at least 3 tracks, got {}",
                    clip_snapshot.track_count
                );

                for track in 0..3 {
                    harness.adjust_columns(track, 1);
                    assert_eq!(harness.view.track_columns[track], 2);
                }

                let harmony_keys = ["c", "d", "e", "f"];
                for (row, key) in harmony_keys.iter().enumerate() {
                    harness.view.focus_harmony_row(row, &mut harness.notify);
                    harness.press_key(key);
                    let pump = harness.pump(Duration::from_millis(300));
                    assert!(!pump.harmony_resync, "unexpected harmony resync on {key}");
                }

                harness.view.focus_note_cell(0, 0, 0, &mut harness.notify);
                harness.press_key("q");
                let pump = harness.pump(Duration::from_millis(300));
                assert!(!pump.clip_resync, "unexpected clip resync on track 0 col 0");

                harness.view.focus_note_cell(0, 0, 1, &mut harness.notify);
                harness.press_key("w");
                let pump = harness.pump(Duration::from_millis(300));
                assert!(!pump.clip_resync, "unexpected clip resync on track 0 col 1");

                harness.view.focus_note_cell(1, 1, 0, &mut harness.notify);
                harness.press_key("1");
                let pump = harness.pump(Duration::from_millis(300));
                assert!(!pump.clip_resync, "unexpected clip resync on track 1 degree");

                harness.view.focus_note_cell(2, 1, 1, &mut harness.notify);
                harness.press_key("e");
                let pump = harness.pump(Duration::from_millis(300));
                assert!(!pump.clip_resync, "unexpected clip resync on track 1 col 1");

                harness.view.focus_note_cell(2, 2, 0, &mut harness.notify);
                harness.press_key("@");
                harness.press_key("3");
                harness.action_palette_confirm();
                let pump = harness.pump(Duration::from_millis(300));
                assert!(!pump.clip_resync, "unexpected clip resync on track 2 chord");

                harness.view.focus_note_cell(3, 2, 1, &mut harness.notify);
                harness.press_key("r");
                let pump = harness.pump(Duration::from_millis(300));
                assert!(!pump.clip_resync, "unexpected clip resync on track 2 col 1");

                for _ in 0..10 {
                    harness.send_undo();
                    let pump = harness.pump(Duration::from_millis(300));
                    assert!(!pump.clip_resync, "unexpected clip resync on undo");
                    assert!(!pump.harmony_resync, "unexpected harmony resync on undo");
                }

                for track in 0..3 {
                    assert!(harness.view.clip_notes[track].is_empty());
                    assert!(harness.view.clip_chords[track].is_empty());
                }
                assert!(harness.view.pending_notes.is_empty());
                assert!(harness.view.pending_chords.is_empty());
                assert!(harness.view.harmony_events.is_empty());

                let clip_snapshot = harness.bridge.read_clip_snapshot()
                    .ok_or_else(|| anyhow::anyhow!("missing clip snapshot"))?;
                assert_eq!(clip_snapshot.note_count, 0);
                assert_eq!(clip_snapshot.chord_count, 0);
                let harmony_snapshot = harness.bridge.read_harmony_snapshot()
                    .ok_or_else(|| anyhow::anyhow!("missing harmony snapshot"))?;
                assert_eq!(harmony_snapshot.event_count, 0);

                Ok(())
            })
        }

        #[test]
        fn test_tracker_multi_column_quick_edit() -> anyhow::Result<()> {
            let mut harness = TestHarness::new("multi_column_quick_edit")?;
            harness.view.cursor_nanotick = 0;
            harness.view.scroll_nanotick_offset = 0;
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
            let entry = harness.view.cell_entry_at(
                harness.nanotick_for_row(0),
                0,
                1,
            );
            assert!(entry.is_some(), "expected entry in column 1");

            harness.adjust_columns(0, -1);
            assert_eq!(harness.view.track_columns[0], 1);
            assert_eq!(harness.view.cursor_col, 0);

            Ok(())
        }

        #[test]
        fn test_tracker_multi_column_column_affinity() -> anyhow::Result<()> {
            let mut harness = TestHarness::new("multi_column_affinity")?;
            harness.view.cursor_nanotick = 0;
            harness.view.scroll_nanotick_offset = 0;
            harness.view.focused_track_index = 0;
            harness.view.cursor_col = 0;

            harness.adjust_columns(0, 1);
            assert_eq!(harness.view.track_columns[0], 2);

            harness.press_key("q");
            let pump = harness.pump(Duration::from_millis(200));
            assert!(!pump.clip_resync, "unexpected clip resync on column 0 note");

            harness.action_palette_up();
            harness.move_column(1);
            assert_eq!(harness.view.cursor_col, 1);
            harness.press_key("w");
            let pump = harness.pump(Duration::from_millis(200));
            assert!(!pump.clip_resync, "unexpected clip resync on column 1 note");

            let entry_col0 = harness.view.cell_entry_at(
                harness.nanotick_for_row(0),
                0,
                0,
            );
            let entry_col1 = harness.view.cell_entry_at(
                harness.nanotick_for_row(0),
                0,
                1,
            );
            assert!(entry_col0.is_some(), "expected entry in column 0");
            assert!(entry_col1.is_some(), "expected entry in column 1");
            assert_eq!(entry_col0.unwrap().note_pitch, Some(60));
            assert_eq!(entry_col1.unwrap().note_pitch, Some(62));

            let notes = harness.notes_at_row(0, 0);
            assert_eq!(notes.len(), 2, "expected two notes at row 0");
            let mut columns: Vec<_> = notes.iter().map(|note| note.column).collect();
            columns.sort();
            assert_eq!(columns, vec![0, 1], "notes should stay in their columns");

            Ok(())
        }

        #[test]
        fn test_tracker_degree_multi_column_same_row() -> anyhow::Result<()> {
            let mut harness = TestHarness::new("degree_multi_column")?;
            harness.view.cursor_nanotick = 0;
            harness.view.scroll_nanotick_offset = 0;
            harness.view.focused_track_index = 0;
            harness.view.cursor_col = 0;

            harness.adjust_columns(0, 1);
            assert_eq!(harness.view.track_columns[0], 2);

            harness.press_key("1");
            harness.move_column(1);
            harness.action_palette_up();
            harness.press_key("2");
            let pump = harness.pump(Duration::from_millis(200));
            assert!(!pump.clip_resync, "unexpected clip resync on degree entry");

            let entry_col0 = harness.view.cell_entry_at(
                harness.nanotick_for_row(0),
                0,
                0,
            );
            let entry_col1 = harness.view.cell_entry_at(
                harness.nanotick_for_row(0),
                0,
                1,
            );
            assert!(entry_col0.is_some(), "expected degree in column 0");
            assert!(entry_col1.is_some(), "expected degree in column 1");

            let chords = harness.chords_at_row(0, 0);
            assert_eq!(chords.len(), 2, "expected two degree chords at row 0");
            let mut columns: Vec<_> = chords.iter().map(|chord| chord.column).collect();
            columns.sort();
            assert_eq!(columns, vec![0, 1], "degrees should stay in their columns");

            Ok(())
        }

        #[test]
        fn test_tracker_chord_packed_fields() -> anyhow::Result<()> {
            let mut harness = TestHarness::new("chord_packed_fields")?;
            harness.view.cursor_nanotick = 0;
            harness.view.scroll_nanotick_offset = 0;
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
            harness.view.cursor_nanotick = 0;
            harness.view.scroll_nanotick_offset = 0;
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
                    assert!(notes.iter().any(|note| {
                        note.pitch == expected_pitch && note.column == column as u8
                    }), "expected note in column {column}");
                    harness.assert_view_matches_snapshot_row(track, 0)?;
                    harness.action_palette_up();
                }
            }

            Ok(())
        }

        #[test]
        fn test_loop_determinism_degree_harmony() -> anyhow::Result<()> {
            let mut harness = TestHarness::new("loop_determinism")?;
            harness.view.cursor_nanotick = 0;
            harness.view.scroll_nanotick_offset = 0;
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

            let track_shm = open_track_shm(TRACK_SHM_TIMEOUT)?;
            while ring_pop(&track_shm.ring_std).is_some() {}

            harness.view.toggle_play(&mut harness.notify);
            let _ = harness.pump(Duration::from_millis(200));

            let sample_rate = unsafe { (*track_shm.header).sample_rate };
            let pattern_ticks =
                (NANOTICKS_PER_QUARTER / ZOOM_LEVELS[DEFAULT_ZOOM_INDEX]) * 16;
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
