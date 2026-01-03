#![allow(non_camel_case_types)]

use core::ffi::c_void;

pub const PATCHER_ABI_VERSION: u32 = 3;
const NANOTICKS_PER_QUARTER: u64 = 960_000;
const DEFAULT_BPM: f64 = 120.0;
const EUCLIDEAN_STEPS: u32 = 16;
const EUCLIDEAN_HITS: u32 = 5;
const EUCLIDEAN_OFFSET: u32 = 0;
const EUCLIDEAN_DEGREE: u8 = 1;
const EUCLIDEAN_OCTAVE_OFFSET: i8 = 0;
const EUCLIDEAN_MAX_STEPS: usize = 64;
const MUSICAL_LOGIC_KIND_GATE: u8 = 1;
const MUSICAL_LOGIC_KIND_DEGREE: u8 = 2;

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

#[repr(C)]
pub struct PatcherLfoConfig {
    pub frequency_hz: f32,
    pub depth: f32,
    pub bias: f32,
    pub phase_offset: f32,
}

#[repr(C)]
pub struct PatcherRandomDegreeConfig {
    pub degree: u8,
    pub velocity: u8,
    pub _pad0: [u8; 2],
    pub duration_ticks: u64,
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
    pub block_start_sample: u64,
    pub sample_rate: f32,
    pub tempo_bpm: f32,
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

    pub mod_outputs: *mut f32,
    pub mod_output_count: u32,
    pub mod_output_samples: *mut f32,
    pub mod_output_stride: u32,

    pub mod_inputs: *mut f32,
    pub mod_input_count: u32,
    pub mod_input_stride: u32,
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

fn bjorklund_pattern(steps: u32, hits: u32, pattern: &mut [u8; EUCLIDEAN_MAX_STEPS]) {
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

    let mut counts = [0usize; EUCLIDEAN_MAX_STEPS];
    let mut remainders = [0usize; EUCLIDEAN_MAX_STEPS];
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
        out: &mut [u8; EUCLIDEAN_MAX_STEPS],
        out_index: &mut usize,
        max_len: usize,
    ) {
        if *out_index >= max_len {
            return;
        }
        if level == -1 {
            out[*out_index] = 0;
            *out_index += 1;
        } else if level == -2 {
            out[*out_index] = 1;
            *out_index += 1;
        } else {
            let idx = level as usize;
            for _ in 0..counts[idx] {
                build(level - 1, counts, remainders, out, out_index, max_len);
                if *out_index >= max_len {
                    return;
                }
            }
            if remainders[idx] != 0 {
                build(level - 2, counts, remainders, out, out_index, max_len);
            }
        }
    }

    let mut out_index = 0usize;
    build(
        level as isize,
        &counts,
        &remainders,
        pattern,
        &mut out_index,
        steps_usize,
    );
}

fn mix64(mut x: u64) -> u64 {
    // SplitMix64 finalizer for stable, deterministic hashing.
    x ^= x >> 30;
    x = x.wrapping_mul(0xbf58_476d_1ce4_e5b9);
    x ^= x >> 27;
    x = x.wrapping_mul(0x94d0_49bb_1331_11eb);
    x ^= x >> 31;
    x
}

#[no_mangle]
pub extern "C" fn patcher_process(ctx: *mut PatcherContext) {
    patcher_process_euclidean(ctx);
}

#[no_mangle]
pub extern "C" fn patcher_process_passthrough(_ctx: *mut PatcherContext) {}

#[no_mangle]
pub extern "C" fn patcher_process_event_out(_ctx: *mut PatcherContext) {}

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
        let mut duration_ticks = 0u64;
        if !ctx_ref.node_config.is_null()
            && ctx_ref.node_config_size as usize >= core::mem::size_of::<PatcherEuclideanConfig>()
        {
            let config = &*(ctx_ref.node_config as *const PatcherEuclideanConfig);
            steps = if config.steps == 0 { steps } else { config.steps };
            hits = if config.hits == 0 { hits } else { config.hits };
            offset = config.offset;
            duration_ticks = config.duration_ticks;
        }

        let loop_ticks = NANOTICKS_PER_QUARTER * 4;
        let step_ticks = loop_ticks / steps as u64;
        if loop_ticks == 0 || step_ticks == 0 {
            return;
        }

        let offset_ticks = (offset as u64) * step_ticks;
        let tempo_bpm = if ctx_ref.tempo_bpm > 0.0 {
            ctx_ref.tempo_bpm as f64
        } else {
            DEFAULT_BPM
        };
        let samples_per_tick =
            (ctx_ref.sample_rate as f64 * 60.0) / (tempo_bpm * NANOTICKS_PER_QUARTER as f64);
        let block_start_sample = ctx_ref.block_start_sample;

        let mut pattern: [u8; EUCLIDEAN_MAX_STEPS] = [0u8; EUCLIDEAN_MAX_STEPS];
        if steps as usize <= EUCLIDEAN_MAX_STEPS {
            bjorklund_pattern(steps, hits, &mut pattern);
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
                    degree: 0,
                    octave_offset: 0,
                    _pad0: [0u8; 2],
                    chord_id: 0,
                    duration_ticks: if duration_ticks == 0 {
                        step_ticks / 2
                    } else {
                        duration_ticks
                    },
                    priority_hint: 0,
                    velocity: 0,
                    base_octave: 0,
                    metadata: {
                        let mut data = [0u8; 21];
                        data[0] = MUSICAL_LOGIC_KIND_GATE;
                        data
                    },
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
pub extern "C" fn patcher_process_random_degree(ctx: *mut PatcherContext) {
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
        let mut config = PatcherRandomDegreeConfig {
            degree: 8,
            velocity: 100,
            _pad0: [0u8; 2],
            duration_ticks: 0,
        };
        if !ctx_ref.node_config.is_null()
            && ctx_ref.node_config_size as usize >= core::mem::size_of::<PatcherRandomDegreeConfig>()
        {
            let cfg = &*(ctx_ref.node_config as *const PatcherRandomDegreeConfig);
            if cfg.degree != 0 {
                config.degree = cfg.degree;
            }
            if cfg.velocity != 0 {
                config.velocity = cfg.velocity;
            }
            if cfg.duration_ticks != 0 {
                config.duration_ticks = cfg.duration_ticks;
            }
        }
        let degree_max = config.degree.max(1);
        let count = *ctx_ref.event_count;
        let events = core::slice::from_raw_parts_mut(ctx_ref.event_buffer, count as usize);
        for (index, entry) in events.iter_mut().enumerate() {
            if entry.type_ != 9 {
                continue;
            }
            let mut payload = MusicalLogicPayload {
                degree: 0,
                octave_offset: 0,
                _pad0: [0u8; 2],
                chord_id: 0,
                duration_ticks: 0,
                priority_hint: 0,
                velocity: 0,
                base_octave: 0,
                metadata: [0u8; 21],
            };
            core::ptr::copy_nonoverlapping(
                entry.payload.as_ptr(),
                &mut payload as *mut MusicalLogicPayload as *mut u8,
                core::mem::size_of::<MusicalLogicPayload>(),
            );
            if payload.metadata[0] != MUSICAL_LOGIC_KIND_GATE {
                continue;
            }
            let seed = (ctx_ref.block_start_tick as u64)
                ^ (entry.sample_time as u64)
                ^ (index as u64).wrapping_mul(0x9e37_79b9);
            let random = (mix64(seed) % degree_max as u64) as u8;
            payload.degree = random.saturating_add(1);
            payload.velocity = if config.velocity != 0 {
                config.velocity
            } else if payload.velocity != 0 {
                payload.velocity
            } else {
                100
            };
            if config.duration_ticks != 0 {
                payload.duration_ticks = config.duration_ticks;
            } else if payload.duration_ticks == 0 {
                payload.duration_ticks = NANOTICKS_PER_QUARTER / 8;
            }
            payload.metadata[0] = MUSICAL_LOGIC_KIND_DEGREE;
            entry.size = core::mem::size_of::<MusicalLogicPayload>() as u16;
            core::ptr::copy_nonoverlapping(
                &payload as *const MusicalLogicPayload as *const u8,
                entry.payload.as_mut_ptr(),
                core::mem::size_of::<MusicalLogicPayload>(),
            );
        }
    }
}

#[no_mangle]
pub extern "C" fn patcher_process_lfo(ctx: *mut PatcherContext) {
    if ctx.is_null() {
        return;
    }
    unsafe {
        let ctx_ref = &mut *ctx;
        if ctx_ref.mod_output_count == 0 || ctx_ref.mod_outputs.is_null() {
            return;
        }
        let mut config = PatcherLfoConfig {
            frequency_hz: 1.0,
            depth: 1.0,
            bias: 0.0,
            phase_offset: 0.0,
        };
        if !ctx_ref.node_config.is_null()
            && ctx_ref.node_config_size as usize >= core::mem::size_of::<PatcherLfoConfig>()
        {
            let cfg = &*(ctx_ref.node_config as *const PatcherLfoConfig);
            config.frequency_hz = cfg.frequency_hz;
            config.depth = cfg.depth;
            config.bias = cfg.bias;
            config.phase_offset = cfg.phase_offset;
        }

        let outputs =
            core::slice::from_raw_parts_mut(ctx_ref.mod_outputs, ctx_ref.mod_output_count as usize);
        for value in outputs.iter_mut() {
            *value = config.bias;
        }
        if ctx_ref.mod_output_samples.is_null() || ctx_ref.mod_output_stride == 0 {
            outputs[0] = config.bias;
            return;
        }
        let stride = ctx_ref.mod_output_stride as usize;
        let total = stride * ctx_ref.mod_output_count as usize;
        let samples =
            core::slice::from_raw_parts_mut(ctx_ref.mod_output_samples, total);

        let tempo_bpm = if ctx_ref.tempo_bpm > 0.0 {
            ctx_ref.tempo_bpm as f64
        } else {
            DEFAULT_BPM
        };
        let seconds_per_tick =
            60.0 / (tempo_bpm * NANOTICKS_PER_QUARTER as f64);
        let block_time =
            ctx_ref.block_start_tick as f64 * seconds_per_tick;
        let phase_base =
            (block_time as f32) * (config.frequency_hz * std::f32::consts::TAU)
                + config.phase_offset * std::f32::consts::TAU;
        let inv_sample_rate = 1.0 / ctx_ref.sample_rate.max(1.0);
        let phase_step = config.frequency_hz * std::f32::consts::TAU * inv_sample_rate;
        for i in 0..stride {
            let phase = phase_base + phase_step * (i as f32);
            let value = phase.sin() * config.depth + config.bias;
            samples[i] = value;
        }
        outputs[0] = samples[stride - 1];
    }
}

#[no_mangle]
pub extern "C" fn patcher_process_audio_passthrough(ctx: *mut PatcherContext) {
    if ctx.is_null() {
        return;
    }
    unsafe {
        let ctx_ref = &mut *ctx;
        if !ctx_ref.mod_inputs.is_null()
            && ctx_ref.mod_input_count > 0
            && ctx_ref.mod_input_stride > 0
        {
            let stride = ctx_ref.mod_input_stride as usize;
            let inputs = core::slice::from_raw_parts(
                ctx_ref.mod_inputs,
                stride * ctx_ref.mod_input_count as usize,
            );
            if ctx_ref.audio_channels.is_null() || ctx_ref.num_channels == 0 {
                return;
            }
            let frames = ctx_ref.num_frames as usize;
            let gain_base = 0usize;
            let frame_count = frames.min(stride);
            for ch in 0..ctx_ref.num_channels as usize {
                let channel_ptr = *ctx_ref.audio_channels.add(ch);
                if channel_ptr.is_null() {
                    continue;
                }
                let channel = core::slice::from_raw_parts_mut(channel_ptr, frames);
                for i in 0..frame_count {
                    let sample = &mut channel[i];
                    let gain = inputs[gain_base + i];
                    *sample *= gain;
                }
            }
            return;
        }
        let mod_count = ctx_ref.mod_output_count as usize;
        if !ctx_ref.mod_outputs.is_null() && mod_count > 0 {
            let outputs = core::slice::from_raw_parts_mut(ctx_ref.mod_outputs, mod_count);
            for value in outputs.iter_mut() {
                *value = 0.0;
            }
        }
        if !ctx_ref.mod_output_samples.is_null()
            && ctx_ref.mod_output_stride > 0
            && mod_count > 0
        {
            let stride = ctx_ref.mod_output_stride as usize;
            let total = stride * mod_count;
            let samples =
                core::slice::from_raw_parts_mut(ctx_ref.mod_output_samples, total);
            let phase0 = (ctx_ref.block_start_tick as f32 / NANOTICKS_PER_QUARTER as f32)
                * std::f32::consts::TAU;
            for output in 0..mod_count {
                let base = output * stride;
                for i in 0..stride {
                    let phase = phase0 + (i as f32 / stride as f32) * std::f32::consts::TAU;
                    samples[base + i] = phase.sin();
                }
                if !ctx_ref.mod_outputs.is_null() {
                    let outputs =
                        core::slice::from_raw_parts_mut(ctx_ref.mod_outputs, mod_count);
                    outputs[output] = samples[base + stride - 1];
                }
            }
        }
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
