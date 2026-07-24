use std::time::{Duration, Instant};

use daw_bridge::layout::{
    EventEntry, EventType, K_CHAIN_DEVICE_ID_AUTO, K_UI_EDIT_BATCH_MAX_OPS,
    UiChainCommandPayload, UiChainDiffPayload, UiChainErrorPayload, UiChordCommandPayload,
    UiChordDiffPayload, UiChordDiffType, UiClipWindowSnapshot, UiCommandPayload,
    UiCommandType, UiDiffPayload, UiDiffType, UiEditBatchEntry, UiHarmonyDiffPayload,
    UiHarmonyDiffType, UiHarmonySnapshot, UiPatcherGraphCommandPayload,
    UiPatcherGraphDiffPayload, UiPatcherNodeConfigPayload, UiPatcherPresetCommandPayload,
    UiPatcherGraphErrorPayload, UI_CLIP_WINDOW_FLAG_COMPLETE,
};

use crate::app::{ChainDevice, EngineView, PatcherEdgeUi, PatcherNodeUi, PatcherPortKind, TRACK_COUNT};
use crate::engine::bridge::{
    bump_ui_enqueued, bump_ui_send_fail, bump_ui_sent, log_ui_send_fail, EngineBridge,
};
use crate::state::{HarmonyEntry, QueuedCommand};
use crate::util::unpack_chord_spread;

impl EngineView {
    fn note_key(payload: &UiCommandPayload) -> Option<(u32, u64, u8)> {
        let command = payload.command_type;
        if command == UiCommandType::WriteNote as u16 ||
            command == UiCommandType::DeleteNote as u16 {
            let nanotick = (payload.note_nanotick_lo as u64) |
                ((payload.note_nanotick_hi as u64) << 32);
            return Some((payload.track_id, nanotick, payload.flags as u8));
        }
        None
    }

    fn chord_key(payload: &UiChordCommandPayload) -> Option<(u32, u64, u8)> {
        let command = payload.command_type;
        if command == UiCommandType::WriteChord as u16 ||
            command == UiCommandType::DeleteChord as u16 {
            let nanotick = (payload.nanotick_lo as u64) |
                ((payload.nanotick_hi as u64) << 32);
            return Some((payload.track_id, nanotick, payload.flags as u8));
        }
        None
    }

    fn is_clip_edit_payload(command_type: u16) -> bool {
        matches!(
            command_type,
            x if x == UiCommandType::WriteNote as u16 ||
                x == UiCommandType::DeleteNote as u16 ||
                x == UiCommandType::Undo as u16 ||
                x == UiCommandType::Redo as u16
        )
    }

    fn is_harmony_edit_payload(command_type: u16) -> bool {
        matches!(
            command_type,
            x if x == UiCommandType::WriteHarmony as u16 ||
                x == UiCommandType::DeleteHarmony as u16
        )
    }

    fn is_edit_payload(command_type: u16) -> bool {
        Self::is_clip_edit_payload(command_type) || Self::is_harmony_edit_payload(command_type)
    }

    fn is_clip_edit_command(entry: &QueuedCommand) -> bool {
        match entry {
            QueuedCommand::Ui(payload) => Self::is_clip_edit_payload(payload.command_type),
            QueuedCommand::Chord(payload) => payload.command_type == UiCommandType::WriteChord as u16 ||
                payload.command_type == UiCommandType::DeleteChord as u16,
            _ => false,
        }
    }

    fn is_harmony_edit_command(entry: &QueuedCommand) -> bool {
        match entry {
            QueuedCommand::Ui(payload) => Self::is_harmony_edit_payload(payload.command_type),
            _ => false,
        }
    }

    fn entry_from_ui_payload(payload: &UiCommandPayload) -> EventEntry {
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
                payload as *const UiCommandPayload as *const u8,
                std::mem::size_of::<UiCommandPayload>(),
            )
        };
        entry.payload[..payload_bytes.len()].copy_from_slice(payload_bytes);
        entry
    }

    fn entry_from_chord_payload(payload: &UiChordCommandPayload) -> EventEntry {
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
                payload as *const UiChordCommandPayload as *const u8,
                std::mem::size_of::<UiChordCommandPayload>(),
            )
        };
        entry.payload[..payload_bytes.len()].copy_from_slice(payload_bytes);
        entry
    }

    pub(crate) fn enqueue_ui_command(&mut self, mut payload: UiCommandPayload) {
        if Self::is_clip_edit_payload(payload.command_type) {
            self.drain_pending_edits_for_ordering();
        }
        let is_edit = Self::is_edit_payload(payload.command_type);
        let mut superseded_base = None;
        {
            let queue = if is_edit {
                &mut self.queued_edit_commands
            } else {
                &mut self.queued_control_commands
            };
            if let Some(QueuedCommand::Ui(prev)) = queue.back() {
                if let (Some(prev_key), Some(next_key)) =
                    (Self::note_key(prev), Self::note_key(&payload)) {
                    if prev_key == next_key {
                        superseded_base = Some(prev.base_version);
                        queue.pop_back();
                    }
                }
            }
        }
        if let Some(base) = superseded_base {
            // Dropping the superseded command leaves its reserved clip version
            // unclaimed. The engine advances one version per command it
            // applies, so the replacement must take the dropped command's slot
            // instead of the next one, or it arrives a version ahead.
            payload.base_version = base;
            self.release_reserved_clip_version();
        }
        let queue = if is_edit {
            &mut self.queued_edit_commands
        } else {
            &mut self.queued_control_commands
        };
        queue.push_back(QueuedCommand::Ui(payload));
        bump_ui_enqueued();
    }

    pub(crate) fn enqueue_chord_command(&mut self, mut payload: UiChordCommandPayload) {
        self.drain_pending_edits_for_ordering();
        let mut superseded_base = None;
        if let Some(QueuedCommand::Chord(prev)) = self.queued_edit_commands.back() {
            if let (Some(prev_key), Some(next_key)) =
                (Self::chord_key(prev), Self::chord_key(&payload)) {
                if prev_key == next_key {
                    superseded_base = Some(prev.base_version);
                    self.queued_edit_commands.pop_back();
                }
            }
        }
        if let Some(base) = superseded_base {
            // See enqueue_ui_command: the replacement inherits the dropped
            // command's reserved clip version.
            payload.base_version = base;
            self.release_reserved_clip_version();
        }
        self.queued_edit_commands.push_back(QueuedCommand::Chord(payload));
        bump_ui_enqueued();
    }

    pub(crate) fn enqueue_chain_command(&mut self, payload: UiChainCommandPayload) {
        self.queued_control_commands
            .push_back(QueuedCommand::Chain(payload));
        bump_ui_enqueued();
    }

    pub(crate) fn enqueue_patcher_graph_command(
        &mut self,
        payload: UiPatcherGraphCommandPayload,
    ) {
        self.queued_control_commands
            .push_back(QueuedCommand::PatcherGraph(payload));
        bump_ui_enqueued();
    }

    pub(crate) fn enqueue_patcher_node_config(
        &mut self,
        payload: UiPatcherNodeConfigPayload,
    ) {
        self.queued_control_commands
            .push_back(QueuedCommand::PatcherConfig(payload));
        bump_ui_enqueued();
    }

    pub(crate) fn enqueue_patcher_preset(
        &mut self,
        payload: UiPatcherPresetCommandPayload,
    ) {
        self.queued_control_commands
            .push_back(QueuedCommand::PatcherPreset(payload));
        bump_ui_enqueued();
    }

    fn should_pause_edit_entry(&self, entry: &QueuedCommand) -> bool {
        if self.clip_resync_pending {
            return match entry {
                QueuedCommand::Ui(payload) =>
                    Self::is_clip_edit_payload(payload.command_type),
                QueuedCommand::Chord(_) => true,
                _ => false,
            };
        }
        if self.harmony_resync_pending {
            return Self::is_harmony_edit_command(entry);
        }
        false
    }

    fn flush_control_queue(
        &mut self,
        bridge: &EngineBridge,
        start: Instant,
        budget: Duration,
        max_ops: usize,
    ) -> usize {
        let mut sent_ops = 0usize;
        while let Some(entry) = self.queued_control_commands.front() {
            if sent_ops >= max_ops || start.elapsed() >= budget {
                break;
            }
            let sent = match entry {
                QueuedCommand::Ui(payload) => bridge.try_send_ui_command(*payload),
                QueuedCommand::Chord(payload) => bridge.try_send_ui_chord_command(*payload),
                QueuedCommand::Chain(payload) => bridge.try_send_ui_chain_command(*payload),
                QueuedCommand::PatcherGraph(payload) =>
                    bridge.try_send_ui_patcher_graph_command(*payload),
                QueuedCommand::PatcherConfig(payload) =>
                    bridge.try_send_ui_patcher_node_config(*payload),
                QueuedCommand::PatcherPreset(payload) =>
                    bridge.try_send_ui_patcher_preset(*payload),
            };
            if sent {
                bump_ui_sent();
                self.queued_control_commands.pop_front();
                sent_ops = sent_ops.saturating_add(1);
            } else {
                bump_ui_send_fail();
                log_ui_send_fail();
                break;
            }
        }
        sent_ops
    }

    fn flush_edit_queue(
        &mut self,
        bridge: &EngineBridge,
        start: Instant,
        budget: Duration,
        max_ops: usize,
    ) -> usize {
        let mut sent_ops = 0usize;
        while let Some(entry) = self.queued_edit_commands.front() {
            if sent_ops >= max_ops || start.elapsed() >= budget {
                break;
            }
            if self.should_pause_edit_entry(entry) {
                break;
            }
            if Self::is_clip_edit_command(entry) && bridge.supports_edit_batches() {
                let remaining = max_ops.saturating_sub(sent_ops).max(1);
                let mut batch = UiEditBatchEntry {
                    batch_id: self.next_edit_batch_id,
                    op_count: 0,
                    ops: [EventEntry {
                        sample_time: 0,
                        block_id: 0,
                        event_type: EventType::UiCommand as u16,
                        size: 0,
                        flags: 0,
                        payload: [0u8; 40],
                    }; K_UI_EDIT_BATCH_MAX_OPS],
                };
                let mut count = 0usize;
                for queued in self.queued_edit_commands.iter() {
                    if !Self::is_clip_edit_command(queued) ||
                        count >= K_UI_EDIT_BATCH_MAX_OPS {
                        break;
                    }
                    let entry = match queued {
                        QueuedCommand::Ui(payload) => Self::entry_from_ui_payload(payload),
                        QueuedCommand::Chord(payload) => Self::entry_from_chord_payload(payload),
                        _ => break,
                    };
                    batch.ops[count] = entry;
                    count = count.saturating_add(1);
                    if count >= remaining {
                        break;
                    }
                }
                if count > 0 {
                    batch.op_count = count as u32;
                    if bridge.try_send_ui_edit_batch(batch) {
                        for _ in 0..count {
                            bump_ui_sent();
                            self.queued_edit_commands.pop_front();
                        }
                        self.next_edit_batch_id =
                            self.next_edit_batch_id.wrapping_add(1).max(1);
                        sent_ops = sent_ops.saturating_add(count);
                        continue;
                    }
                    // The edit ring is full. Do NOT fall through to the legacy
                    // ring: the engine drains the edit ring to exhaustion first,
                    // so a clip edit sent the old way would arrive after edits
                    // queued behind it and be rejected on base_version. Leave it
                    // queued and retry on the next flush.
                    bump_ui_send_fail();
                    log_ui_send_fail();
                    break;
                }
            }
            let sent = match entry {
                QueuedCommand::Ui(payload) => bridge.try_send_ui_command(*payload),
                QueuedCommand::Chord(payload) => bridge.try_send_ui_chord_command(*payload),
                QueuedCommand::Chain(payload) => bridge.try_send_ui_chain_command(*payload),
                QueuedCommand::PatcherGraph(payload) =>
                    bridge.try_send_ui_patcher_graph_command(*payload),
                QueuedCommand::PatcherConfig(payload) =>
                    bridge.try_send_ui_patcher_node_config(*payload),
                QueuedCommand::PatcherPreset(payload) =>
                    bridge.try_send_ui_patcher_preset(*payload),
            };
            if sent {
                bump_ui_sent();
                self.queued_edit_commands.pop_front();
                sent_ops = sent_ops.saturating_add(1);
            } else {
                bump_ui_send_fail();
                log_ui_send_fail();
                break;
            }
        }
        sent_ops
    }

    pub fn flush_queued_commands(&mut self) {
        let Some(bridge) = self.bridge.clone() else {
            return;
        };
        let start = Instant::now();
        let budget = Duration::from_millis(2);
        let max_ops = 64usize;
        let edit_pending = !self.queued_edit_commands.is_empty();
        let mut sent_ops = 0usize;
        let mut control_budget = if edit_pending { max_ops / 4 } else { max_ops };
        if control_budget == 0 && !self.queued_control_commands.is_empty() {
            control_budget = 1;
        }
        sent_ops = sent_ops.saturating_add(self.flush_control_queue(
            &bridge,
            start,
            budget,
            control_budget,
        ));
        if sent_ops < max_ops && start.elapsed() < budget {
            let remaining = max_ops.saturating_sub(sent_ops);
            sent_ops = sent_ops.saturating_add(self.flush_edit_queue(
                &bridge,
                start,
                budget,
                remaining,
            ));
        }
        if sent_ops < max_ops &&
            start.elapsed() < budget &&
            self.queued_edit_commands.is_empty() {
            let remaining = max_ops.saturating_sub(sent_ops);
            let _ = self.flush_control_queue(&bridge, start, budget, remaining);
        }
    }

    pub(crate) fn rebase_clip_queue(&mut self, base_version: u32) {
        let mut next = base_version;
        for entry in self.queued_edit_commands.iter_mut() {
            match entry {
                QueuedCommand::Ui(payload) => {
                    let cmd = payload.command_type;
                    if Self::is_clip_edit_payload(cmd) {
                        payload.base_version = next;
                        next = next.saturating_add(1);
                    }
                }
                QueuedCommand::Chord(payload) => {
                    let cmd = payload.command_type;
                    if cmd == UiCommandType::WriteChord as u16 ||
                        cmd == UiCommandType::DeleteChord as u16 {
                        payload.base_version = next;
                        next = next.saturating_add(1);
                    }
                }
                QueuedCommand::Chain(_) => {}
                QueuedCommand::PatcherGraph(_) => {}
                QueuedCommand::PatcherConfig(_) => {}
                QueuedCommand::PatcherPreset(_) => {}
            }
        }
        self.clip_version_local = next;
    }

    pub(crate) fn rebase_harmony_queue(&mut self, base_version: u32) {
        let mut next = base_version;
        for entry in self.queued_edit_commands.iter_mut() {
            if let QueuedCommand::Ui(payload) = entry {
                if Self::is_harmony_edit_payload(payload.command_type) {
                    payload.base_version = next;
                    next = next.saturating_add(1);
                }
            }
        }
        self.harmony_version_local = next;
    }

    pub fn apply_clip_window_page(&mut self, snapshot: UiClipWindowSnapshot, reset: bool) {
        let track_index = snapshot.track_id as usize;
        if track_index >= TRACK_COUNT {
            return;
        }
        if let Ok(mut store) = self.clip_store.write() {
            store.apply_clip_window_page(snapshot, reset);
        }
        if reset && self.pending_overlay.clear_track(track_index) {
            self.bump_clip_render_version();
        }

        let note_count = (snapshot.note_count as usize).min(snapshot.notes.len());
        let chord_count = (snapshot.chord_count as usize).min(snapshot.chords.len());
        for note_index in 0..note_count {
            let note = snapshot.notes[note_index];
            self.pending_overlay.remove_note_at(
                track_index,
                note.column,
                note.t_on,
            );
        }
        for chord_index in 0..chord_count {
            let chord = snapshot.chords[chord_index];
            let column = (chord.flags & 0xff) as u8;
            self.pending_overlay.remove_chord_at(track_index, column, chord.nanotick);
        }

        if let Some(state) = self.clip_window.get_mut(track_index) {
            state.request_id = snapshot.request_id;
            state.cursor_event_index = snapshot.cursor_event_index;
            state.next_event_index = snapshot.next_event_index;
            state.window_start = snapshot.window_start_nanotick;
            state.window_end = snapshot.window_end_nanotick;
            state.clip_version = snapshot.clip_version;
            state.complete = (snapshot.flags & UI_CLIP_WINDOW_FLAG_COMPLETE) != 0;
        }

        if self.clip_version_local < snapshot.clip_version {
            self.clip_version_local = snapshot.clip_version;
        }
        self.bump_clip_render_version();
        self.mark_tracker_cache_dirty_all();
    }

    pub fn apply_harmony_snapshot(&mut self, snapshot: UiHarmonySnapshot) {
        self.harmony_events.clear();
        let count = snapshot.event_count as usize;
        let max = snapshot.events.len();
        let count = count.min(max);
        for idx in 0..count {
            let event = snapshot.events[idx];
            self.harmony_events.push(HarmonyEntry {
                nanotick: event.nanotick,
                root: event.root,
                scale_id: event.scale_id,
            });
        }
        self.harmony_version_local = self.snapshot.ui_harmony_version;
        self.bump_harmony_render_version();
        self.mark_tracker_cache_dirty_all();
    }

    pub fn apply_chain_diff(&mut self, diff: UiChainDiffPayload) {
        if diff.diff_type != UiDiffType::ChainSnapshot as u16 {
            return;
        }
        let track_index = diff.track_id as usize;
        if track_index >= self.chain_devices.len() {
            return;
        }
        let version = diff.chain_version;
        if self.chain_versions[track_index] != version {
            self.chain_versions[track_index] = version;
            self.chain_devices[track_index].clear();
        }
        if diff.device_id == K_CHAIN_DEVICE_ID_AUTO {
            return;
        }
        let device = ChainDevice {
            id: diff.device_id,
            kind: diff.device_kind,
            position: diff.position,
            patcher_node_id: diff.patcher_node_id,
            host_slot_index: diff.host_slot_index,
            capability_mask: diff.capability_mask,
            bypass: diff.bypass != 0,
        };
        let devices = &mut self.chain_devices[track_index];
        if let Some(existing) = devices.iter_mut().find(|d| d.id == device.id) {
            *existing = device;
        } else {
            devices.push(device);
        }
    }

    pub fn apply_patcher_graph_diff(&mut self, diff: UiPatcherGraphDiffPayload) {
        if diff.diff_type != UiDiffType::PatcherGraphDelta as u16 {
            return;
        }
        let track_index = diff.track_id as usize;
        if track_index >= self.patcher_nodes.len() ||
            track_index >= self.patcher_edges.len() {
            return;
        }
        if self.patcher_versions[track_index] != diff.graph_version {
            self.patcher_versions[track_index] = diff.graph_version;
        }
        let nodes = &mut self.patcher_nodes[track_index];
        let edges = &mut self.patcher_edges[track_index];
        match diff.flags {
            0 => {
                let index = nodes.len();
                let col = index % 4;
                let row = index / 4;
                nodes.push(PatcherNodeUi {
                    id: diff.node_id,
                    node_type: diff.node_type,
                    pos_x: 40.0 + (col as f32) * 220.0,
                    pos_y: 40.0 + (row as f32) * 120.0,
                });
            }
            1 => {
                if let Some(index) = nodes.iter().position(|node| node.id == diff.node_id) {
                    nodes.remove(index);
                }
                edges.retain(|edge| {
                    edge.src_node_id != diff.node_id &&
                        edge.dst_node_id != diff.node_id
                });
            }
            2 => {
                let kind = match diff.edge_kind {
                    0 => PatcherPortKind::Event,
                    1 => PatcherPortKind::Audio,
                    2 => PatcherPortKind::Control,
                    _ => PatcherPortKind::Event,
                };
                let exists = edges.iter().any(|edge| {
                    edge.src_node_id == diff.src_node_id &&
                        edge.src_port_id == diff.src_port_id &&
                        edge.dst_node_id == diff.dst_node_id &&
                        edge.dst_port_id == diff.dst_port_id &&
                        edge.kind == kind
                });
                if !exists {
                    edges.push(PatcherEdgeUi {
                        src_node_id: diff.src_node_id,
                        src_port_id: diff.src_port_id,
                        dst_node_id: diff.dst_node_id,
                        dst_port_id: diff.dst_port_id,
                        kind,
                    });
                }
            }
            _ => {}
        }
    }

    pub fn chain_error_message(&self, diff: UiChainErrorPayload) -> String {
        format!(
            "Chain error {} on track {}",
            diff.error_code,
            diff.track_id + 1
        )
    }

    pub fn patcher_error_message(&self, diff: UiPatcherGraphErrorPayload) -> String {
        let message = match diff.error_code {
            1 => "Invalid node type",
            2 => "Invalid node",
            3 => "Connection would create a cycle",
            4 => "Failed to add node",
            5 => "Invalid connection",
            6 => "Invalid port",
            _ => "Unknown patcher error",
        };
        format!("Patcher error: {} on track {}", message, diff.track_id + 1)
    }

    pub fn apply_diff(&mut self, diff: UiDiffPayload) {
        let nanotick =
            (diff.note_nanotick_lo as u64) | ((diff.note_nanotick_hi as u64) << 32);
        let column = diff.note_column.min(255) as u8;
        if let Ok(mut store) = self.clip_store.write() {
            store.apply_note_diff(&diff);
        }
        if self.clip_version_local < diff.clip_version {
            self.clip_version_local = diff.clip_version;
        }
        let track_index = diff.track_id as usize;
        self.pending_overlay.remove_note_at(track_index, column, nanotick);
        self.pending_overlay.remove_chord_at(track_index, column, nanotick);
        self.mark_tracker_row_dirty_for_nanotick(nanotick);
        self.bump_clip_render_version();
    }

    pub fn apply_harmony_diff(&mut self, diff: UiHarmonyDiffPayload) {
        let nanotick =
            (diff.nanotick_lo as u64) | ((diff.nanotick_hi as u64) << 32);
        match diff.diff_type {
            x if x == UiHarmonyDiffType::AddEvent as u16 ||
                x == UiHarmonyDiffType::UpdateEvent as u16 => {
                if let Some(event) = self
                    .harmony_events
                    .iter_mut()
                    .find(|event| event.nanotick == nanotick)
                {
                    event.root = diff.root;
                    event.scale_id = diff.scale_id;
                } else {
                    self.harmony_events.push(HarmonyEntry {
                        nanotick,
                        root: diff.root,
                        scale_id: diff.scale_id,
                    });
                    self.harmony_events
                        .sort_by_key(|event| event.nanotick);
                }
            }
            x if x == UiHarmonyDiffType::RemoveEvent as u16 => {
                if let Some(index) = self
                    .harmony_events
                    .iter()
                    .position(|event| event.nanotick == nanotick)
                {
                    self.harmony_events.remove(index);
                }
            }
            _ => {}
        }
        if self.harmony_version_local < diff.harmony_version {
            self.harmony_version_local = diff.harmony_version;
        }
        self.bump_harmony_render_version();
        self.mark_tracker_row_dirty_for_nanotick(nanotick);
    }

    pub fn apply_chord_diff(&mut self, diff: UiChordDiffPayload) {
        let track_index = diff.track_id as usize;
        let nanotick = (diff.nanotick_lo as u64) | ((diff.nanotick_hi as u64) << 32);
        let (_, column) = unpack_chord_spread(diff.spread_nanoticks);
        if let Ok(mut store) = self.clip_store.write() {
            store.apply_chord_diff(&diff);
        }
        if self.clip_version_local < diff.clip_version {
            self.clip_version_local = diff.clip_version;
        }
        if diff.diff_type == UiChordDiffType::AddChord as u16 ||
            diff.diff_type == UiChordDiffType::UpdateChord as u16 {
            self.pending_overlay.remove_note_at(track_index, column, nanotick);
        }
        self.pending_overlay.remove_chord_at(track_index, column, nanotick);
        self.mark_tracker_row_dirty_for_nanotick(nanotick);
        self.bump_clip_render_version();
    }

    pub(crate) fn current_clip_version(&self) -> u32 {
        if self.clip_version_local != 0 {
            self.clip_version_local
        } else {
            self.snapshot.ui_clip_version
        }
    }

    pub(crate) fn bump_clip_version(&mut self) {
        let next = self.current_clip_version().saturating_add(1);
        self.clip_version_local = next;
    }

    pub(crate) fn bump_clip_render_version(&mut self) {
        self.clip_render_version = self.clip_render_version.saturating_add(1);
    }

    pub(crate) fn current_harmony_version(&self) -> u32 {
        if self.harmony_version_local != 0 {
            self.harmony_version_local
        } else {
            self.snapshot.ui_harmony_version
        }
    }

    pub(crate) fn bump_harmony_version(&mut self) {
        let next = self.current_harmony_version().saturating_add(1);
        self.harmony_version_local = next;
    }

    pub(crate) fn bump_harmony_render_version(&mut self) {
        self.harmony_render_version = self.harmony_render_version.saturating_add(1);
    }
}
