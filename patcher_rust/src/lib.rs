#![allow(non_camel_case_types)]

use core::ffi::c_void;
use std::sync::{Mutex, OnceLock};

pub const PATCHER_ABI_VERSION: u32 = 1;
const NANOTICKS_PER_QUARTER: u64 = 960_000;
const DEFAULT_BPM: f64 = 120.0;
const EUCLIDEAN_STEPS: u32 = 16;
const EUCLIDEAN_HITS: u32 = 5;
const EUCLIDEAN_OFFSET: u32 = 0;
const EUCLIDEAN_DEGREE: u8 = 1;
const EUCLIDEAN_OCTAVE_OFFSET: i8 = 0;
const EUCLIDEAN_MAX_STEPS: usize = 64;

struct EuclideanCache {
    steps: u32,
    hits: u32,
    pattern: [u8; EUCLIDEAN_MAX_STEPS],
}

static EUCLIDEAN_CACHE: OnceLock<Mutex<EuclideanCache>> = OnceLock::new();

#[repr(C)]
pub struct HarmonyEvent {
    pub nanotick: u64,
    pub root: u32,
    pub scale_id: u32,
    pub flags: u32,
}

#[repr(C)]
pub struct MusicalLogicPayload {
    pub degree: u8,
    pub octave_offset: i8,
    pub _pad0: [u8; 2],
    pub chord_id: u32,
    pub duration_ticks: u64,
    pub priority_hint: u8,
    pub velocity: u8,
    pub base_octave: u8,
    pub metadata: [u8; 21],
}

#[repr(C)]
pub struct PatcherEuclideanConfig {
    pub steps: u32,
    pub hits: u32,
    pub offset: u32,
    pub duration_ticks: u64,
    pub degree: u8,
    pub octave_offset: i8,
    pub velocity: u8,
    pub base_octave: u8,
    pub _pad0: [u8; 2],
}

#[repr(C, align(64))]
pub struct EventEntry {
    pub sample_time: u64,
    pub block_id: u32,
    pub type_: u16,
    pub size: u16,
    pub flags: u32,
    pub payload: [u8; 40],
}

#[repr(C, align(64))]
pub struct PatcherContext {
    pub abi_version: u32,
    pub block_start_tick: u64,
    pub block_end_tick: u64,
    pub sample_rate: f32,
    pub num_frames: u32,

    pub event_buffer: *mut EventEntry,
    pub event_capacity: u32,
    pub event_count: *mut u32,
    pub last_overflow_tick: *mut u64,

    pub audio_channels: *mut *mut f32,
    pub num_channels: u32,

    pub node_config: *const c_void,
    pub node_config_size: u32,

    pub harmony_snapshot: *const HarmonyEvent,
    pub harmony_count: u32,
}

extern "C" {
    pub fn atomic_store_u64(ptr: *mut u64, value: u64);
}

unsafe fn push_event(ctx: &mut PatcherContext, entry: EventEntry, overflow_tick: u64) {
    let count_ptr = ctx.event_count;
    if count_ptr.is_null() {
        return;
    }
    let count = *count_ptr;
    if count < ctx.event_capacity {
        let slot = ctx.event_buffer.add(count as usize);
        *slot = entry;
        *count_ptr = count + 1;
    } else if !ctx.last_overflow_tick.is_null() {
        atomic_store_u64(ctx.last_overflow_tick, overflow_tick);
    }
}

fn euclidean_hit(step_index: u32, hits: u32, steps: u32) -> bool {
    if steps == 0 || hits == 0 {
        return false;
    }
    (step_index * hits) % steps < hits
}

fn bjorklund_pattern(steps: u32, hits: u32, pattern: &mut [u8]) {
    for slot in pattern.iter_mut() {
        *slot = 0;
    }
    if steps == 0 || hits == 0 {
        return;
    }
    let steps_usize = steps as usize;
    let hits_usize = hits.min(steps) as usize;
    if hits_usize == 0 || steps_usize == 0 {
        return;
    }

    let mut counts = vec![0usize; steps_usize];
    let mut remainders = vec![0usize; steps_usize];
    remainders[0] = hits_usize;
    let mut divisor = steps_usize - hits_usize;
    let mut level = 0usize;
    while remainders[level] > 1 {
        counts[level] = divisor / remainders[level];
        remainders[level + 1] = divisor % remainders[level];
        divisor = remainders[level];
        level += 1;
        if level + 1 >= steps_usize {
            break;
        }
    }
    counts[level] = divisor;

    fn build(
        level: isize,
        counts: &[usize],
        remainders: &[usize],
        out: &mut Vec<u8>,
    ) {
        if level == -1 {
            out.push(0);
        } else if level == -2 {
            out.push(1);
        } else {
            let idx = level as usize;
            for _ in 0..counts[idx] {
                build(level - 1, counts, remainders, out);
            }
            if remainders[idx] != 0 {
                build(level - 2, counts, remainders, out);
            }
        }
    }

    let mut output = Vec::with_capacity(steps_usize);
    build(level as isize, &counts, &remainders, &mut output);
    if output.len() != steps_usize {
        output.resize(steps_usize, 0);
    }
    for (i, val) in output.iter().enumerate().take(steps_usize) {
        pattern[i] = *val;
    }
}

#[no_mangle]
pub extern "C" fn patcher_process(ctx: *mut PatcherContext) {
    patcher_process_euclidean(ctx);
}

#[no_mangle]
pub extern "C" fn patcher_process_passthrough(_ctx: *mut PatcherContext) {}

#[no_mangle]
pub extern "C" fn patcher_process_euclidean(ctx: *mut PatcherContext) {
    if ctx.is_null() {
        return;
    }
    unsafe {
        let ctx_ref = &mut *ctx;
        if ctx_ref.abi_version != PATCHER_ABI_VERSION {
            return;
        }
        if ctx_ref.event_buffer.is_null() || ctx_ref.event_count.is_null() {
            return;
        }
        if ctx_ref.event_capacity == 0 {
            return;
        }

        let mut steps = EUCLIDEAN_STEPS;
        let mut hits = EUCLIDEAN_HITS;
        let mut offset = EUCLIDEAN_OFFSET;
        let mut degree = EUCLIDEAN_DEGREE;
        let mut octave_offset = EUCLIDEAN_OCTAVE_OFFSET;
        let mut velocity = 100u8;
        let mut base_octave = 4u8;
        let mut duration_ticks = 0u64;
        if !ctx_ref.node_config.is_null()
            && ctx_ref.node_config_size as usize >= core::mem::size_of::<PatcherEuclideanConfig>()
        {
            let config = &*(ctx_ref.node_config as *const PatcherEuclideanConfig);
            steps = if config.steps == 0 { steps } else { config.steps };
            hits = if config.hits == 0 { hits } else { config.hits };
            offset = config.offset;
            degree = if config.degree == 0 { degree } else { config.degree };
            octave_offset = config.octave_offset;
            velocity = if config.velocity == 0 { velocity } else { config.velocity };
            base_octave = if config.base_octave == 0 { base_octave } else { config.base_octave };
            duration_ticks = config.duration_ticks;
        }

        let loop_ticks = NANOTICKS_PER_QUARTER * 4;
        let step_ticks = loop_ticks / steps as u64;
        if loop_ticks == 0 || step_ticks == 0 {
            return;
        }

        let offset_ticks = (offset as u64) * step_ticks;
        let samples_per_tick =
            (ctx_ref.sample_rate as f64 * 60.0) / (DEFAULT_BPM * NANOTICKS_PER_QUARTER as f64);
        let block_start_sample =
            (ctx_ref.block_start_tick as f64 * samples_per_tick).round() as u64;

        let mut pattern: [u8; EUCLIDEAN_MAX_STEPS] = [0u8; EUCLIDEAN_MAX_STEPS];
        if steps as usize <= EUCLIDEAN_MAX_STEPS {
            let cache = EUCLIDEAN_CACHE.get_or_init(|| {
                Mutex::new(EuclideanCache {
                    steps: 0,
                    hits: 0,
                    pattern: [0u8; EUCLIDEAN_MAX_STEPS],
                })
            });
            if let Ok(mut cache_guard) = cache.lock() {
                if cache_guard.steps != steps || cache_guard.hits != hits {
                    bjorklund_pattern(steps, hits, &mut cache_guard.pattern);
                    cache_guard.steps = steps;
                    cache_guard.hits = hits;
                }
                pattern.copy_from_slice(&cache_guard.pattern);
            }
        }

        let mut tick = ctx_ref.block_start_tick;
        let remainder = (tick + offset_ticks) % step_ticks;
        if remainder != 0 {
            tick = tick.saturating_add(step_ticks - remainder);
        }

        while tick < ctx_ref.block_end_tick {
            let step_index = ((tick + offset_ticks) % loop_ticks) / step_ticks;
            let hit = if steps as usize <= EUCLIDEAN_MAX_STEPS {
                pattern[step_index as usize] != 0
            } else {
                euclidean_hit(step_index as u32, hits, steps)
            };
            if hit {
                let tick_delta = tick - ctx_ref.block_start_tick;
                let sample_delta = (tick_delta as f64 * samples_per_tick).round() as u64;
                let mut entry = EventEntry {
                    sample_time: block_start_sample + sample_delta,
                    block_id: 0,
                    type_: 9,
                    size: core::mem::size_of::<MusicalLogicPayload>() as u16,
                    flags: 0,
                    payload: [0u8; 40],
                };
                let payload = MusicalLogicPayload {
                    degree,
                    octave_offset,
                    _pad0: [0u8; 2],
                    chord_id: 0,
                    duration_ticks: if duration_ticks == 0 {
                        step_ticks / 2
                    } else {
                        duration_ticks
                    },
                    priority_hint: 0,
                    velocity,
                    base_octave,
                    metadata: [0u8; 21],
                };
                let payload_bytes = core::mem::size_of::<MusicalLogicPayload>();
                core::ptr::copy_nonoverlapping(
                    &payload as *const MusicalLogicPayload as *const u8,
                    entry.payload.as_mut_ptr(),
                    payload_bytes,
                );
                push_event(ctx_ref, entry, tick);
            }
            tick = tick.saturating_add(step_ticks);
        }
    }
}

#[no_mangle]
pub extern "C" fn patcher_process_audio_passthrough(ctx: *mut PatcherContext) {
    if ctx.is_null() {
        return;
    }
    unsafe {
        let ctx_ref = &mut *ctx;
        if ctx_ref.audio_channels.is_null() || ctx_ref.num_channels == 0 {
            return;
        }
        let frames = ctx_ref.num_frames as usize;
        for ch in 0..ctx_ref.num_channels as usize {
            let channel_ptr = *ctx_ref.audio_channels.add(ch);
            if channel_ptr.is_null() {
                continue;
            }
            let channel = core::slice::from_raw_parts_mut(channel_ptr, frames);
            for sample in channel.iter_mut() {
                *sample = 0.0;
            }
            if !channel.is_empty() {
                channel[0] = 1.0;
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn euclidean_hit_distribution() {
        let steps = 8;
        let hits = 3;
        let mut count = 0;
        for i in 0..steps {
            if euclidean_hit(i, hits, steps) {
                count += 1;
            }
        }
        assert_eq!(count, hits);
    }
}
