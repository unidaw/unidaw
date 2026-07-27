use std::sync::atomic::{fence, Ordering};

use crate::layout::{ShmHeader, K_UI_MAX_TRACKS};

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
}

pub struct SeqlockReader {
    header: *const ShmHeader,
}

impl SeqlockReader {
    pub fn new(header: *const ShmHeader) -> Self {
        Self { header }
    }

    pub fn read_snapshot(&self) -> Option<UiSnapshot> {
        if self.header.is_null() {
            return None;
        }

        loop {
            let v0 = unsafe { (*self.header).ui_version.load(Ordering::Acquire) };
            if v0 % 2 == 1 {
                continue;
            }

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

            fence(Ordering::Acquire);
            let v1 = unsafe { (*self.header).ui_version.load(Ordering::Acquire) };
            if v0 == v1 && v0 % 2 == 0 {
                return Some(UiSnapshot {
                    version: v1,
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
                });
            }
        }
    }
}
