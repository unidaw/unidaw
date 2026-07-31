#![allow(non_camel_case_types)]

use core::ffi::c_void;

pub const PATCHER_ABI_VERSION: u32 = 4;
const NANOTICKS_PER_QUARTER: u64 = 960_000;
const DEFAULT_BPM: f64 = 120.0;
const EUCLIDEAN_STEPS: u32 = 16;
const EUCLIDEAN_HITS: u32 = 5;
const EUCLIDEAN_OFFSET: u32 = 0;
// These are the NO-CONFIG fallbacks — used only when a node arrives with a null or
// undersized config block, i.e. never in a loaded project. They mirror the defaults in
// PatcherEuclideanConfig (apps/patcher_abi.h); a config that IS present is taken
// verbatim, zeros included.
const EUCLIDEAN_DEGREE: u8 = 1;
const EUCLIDEAN_OCTAVE_OFFSET: i8 = 0;
const EUCLIDEAN_VELOCITY: u8 = 100;
const EUCLIDEAN_BASE_OCTAVE: u8 = 4;
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

// This struct is handed across the C ABI as `const HarmonyEvent*` (apps/patcher_abi.h),
// and until now NOTHING on either side pinned its layout — a widened field would have
// compiled clean here and in C++ while every node below read garbage roots and scales.
// The C++ side asserts the same numbers.
const _: () = {
    assert!(core::mem::size_of::<HarmonyEvent>() == 24);
    assert!(core::mem::align_of::<HarmonyEvent>() == 8);
};

#[repr(C)]
pub struct MusicalLogicPayload {
    pub degree: u8,
    pub octave_offset: i8,
    /// The sound address this note plays, or 0 for "let the keymap pick from the pitch".
    /// Was `_pad0[2]`: same offset, same size, zeroed and never read. See apps/patcher_abi.h.
    pub sound: u16,
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

/// SliceSelect: which SOUND a generated note plays, chosen reproducibly.
///
/// `count` is the SIZE of the range, so 0 and 1 both mean "always `base`" — a range being
/// empty, not a sentinel being decoded, exactly as PatcherRandomDegreeConfig::degree works.
/// `base` is the first sound address in the range, so a chop laid down from slot 1 is
/// base=1, count=8.
#[repr(C)]
pub struct PatcherSliceSelectConfig {
    pub base: u16,
    pub count: u16,
    pub _pad0: [u8; 4],
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

    /// ABI 4: the node's id and the project seed, so generation is reproducible AND
    /// decorrelated per node. Mirrors apps/patcher_abi.h; appended at the end.
    pub node_id: u32,
    pub seed: u64,
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

/// The musical tick an upstream generator stamped into the payload, or `None` if it did not.
///
/// Stamped as tick+1 so zero means "absent" and tick 0 is still expressible. Events that arrive
/// from somewhere with no stamp — a clip note routed through a graph, say — fall back to
/// recovering the tick from the sample time, which is what every node did before.
fn stamped_tick(metadata: &[u8; 21]) -> Option<u64> {
    let mut bytes = [0u8; 8];
    bytes.copy_from_slice(&metadata[1..9]);
    let raw = u64::from_le_bytes(bytes);
    if raw == 0 {
        None
    } else {
        Some(raw - 1)
    }
}

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
        // These four were in the config, stored, round-tripped through the read-back and
        // drawn as turnable controls — and this function hard-zeroed them into the
        // payload, so four of the node's seven knobs did nothing. Measured and reported
        // by the frontend: a nine-octave change to base_octave moved the zero-crossing
        // rate 18% (i.e. not at all), while the same harness turning a wired knob moved
        // RMS by 5.6x. A control that looks operable and is not costs more than one that
        // is visibly inert, because the user spends their time doubting their ears.
        let mut degree = EUCLIDEAN_DEGREE;
        let mut octave_offset = EUCLIDEAN_OCTAVE_OFFSET;
        let mut velocity = EUCLIDEAN_VELOCITY;
        let mut base_octave = EUCLIDEAN_BASE_OCTAVE;
        if !ctx_ref.node_config.is_null()
            && ctx_ref.node_config_size as usize >= core::mem::size_of::<PatcherEuclideanConfig>()
        {
            let config = &*(ctx_ref.node_config as *const PatcherEuclideanConfig);
            // 0 MEANS 0. This used to read "0 means the caller left it unset, so use
            // the default", which made `hits 0` play five hits while the read-back
            // reported 0 — a confidently wrong number, and it stole the one natural
            // spelling of "this generator is in the graph and emitting nothing".
            // Defaults now come from the config struct itself, applied where the file
            // is parsed, so a zero that arrives here was actually asked for.
            steps = config.steps;
            hits = config.hits;
            offset = config.offset;
            duration_ticks = config.duration_ticks;
            degree = config.degree;
            octave_offset = config.octave_offset;
            velocity = config.velocity;
            base_octave = config.base_octave;
        }

        // Zero steps is not a pattern, and dividing by it panics. `hits == 0` needs no
        // guard: bjorklund of zero hits is a pattern of rests, which is the point.
        if steps == 0 {
            return;
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
                // ROUNDED FROM THE ABSOLUTE TICK, NOT FROM A DELTA OFF THE BLOCK BASE.
                //
                // This was `round((tick - block_start_tick) * samples_per_tick)`, which makes an
                // onset's rounding depend on where the block boundary happened to fall: the same
                // absolute tick lands one sample apart at 64 frames and at 256, and a bounce
                // stops equalling the previous bounce. It is the same defect the CLIP path had
                // (task #84) on its own code path — a position derived from a per-block base
                // rather than from the music.
                //
                // Both terms below are functions of ABSOLUTE ticks, so the difference is too,
                // and the block grid drops out. The base is still block_start_sample rather than
                // a global conversion because this node only knows ONE tempo: under a tempo map
                // the engine's base is right locally and a from-zero conversion would not be.
                // ROUNDED FROM THE ABSOLUTE TICK, NOT FROM A DELTA OFF THE BLOCK BASE.
                //
                // `round((tick - block_start_tick) * samples_per_tick)` makes an onset's
                // rounding depend on where the block boundary happened to fall, so the same
                // absolute tick lands one sample apart at 256 frames and at 1024. It is the same
                // defect the CLIP path had (task #84) on its own code path: a position derived
                // from a per-block base rather than from the music.
                //
                // Both terms below are functions of ABSOLUTE ticks, so their difference is too
                // and the block grid drops out. The base stays block_start_sample rather than a
                // conversion from zero because this node knows only ONE tempo — under a tempo
                // map the engine's base is right locally and a from-zero conversion would not be.
                //
                // Worth recording how this was established, because it was reverted once first:
                // changing it alone moved nothing, because a dropped note and a mis-seeded draw
                // were both louder. It was restored only after those two were fixed and a
                // one-sample difference was what remained.
                // FLOOR, NOT ROUND — and measured from the block's own base, not reconstructed.
                //
                // A tick inside [block_start_tick, block_end_tick) must convert to a sample
                // inside that block. `round` breaks that on a half: the sixteenth at 1.875 s is
                // sample 82687.5, which rounds to 82688 — the first sample of the NEXT block,
                // outside the block whose tick window owns it. The engine then dropped it, and
                // the next block never emitted it either because its tick window starts later,
                // so the note vanished at some buffer sizes and not others.
                //
                // AND THE BASE IS block_start_sample, NOT A CONVERSION OF block_start_tick. An
                // attempt at "round the absolute tick, both terms from zero" is one line away and
                // is WRONG here, which took a measurement to establish: floor(bst * spt) is
                // 82431 where the block's real base sample is 82432, so reconstructing the base
                // introduces exactly the one-sample error it was meant to remove. The base the
                // engine hands us is ground truth; the delta from it is the only thing worth
                // computing, and floor keeps that delta inside the block.
                let tick_delta = tick - ctx_ref.block_start_tick;
                let sample_delta = (tick_delta as f64 * samples_per_tick).floor() as i64;
                let mut entry = EventEntry {
                    sample_time: (block_start_sample as i64 + sample_delta) as u64,
                    block_id: 0,
                    type_: 9,
                    size: core::mem::size_of::<MusicalLogicPayload>() as u16,
                    flags: 0,
                    payload: [0u8; 40],
                };
                let payload = MusicalLogicPayload {
                    // Downstream nodes may override some of these — random_degree
                    // replaces degree and velocity from its own config, which is what
                    // putting a randomiser in the path means. euclidean -> event_out is
                    // a legal graph too, and there these are the only source of pitch and
                    // velocity; octave_offset and base_octave survive random_degree in
                    // every graph.
                    degree,
                    octave_offset,
                    sound: 0,
                    chord_id: 0,
                    duration_ticks: if duration_ticks == 0 {
                        step_ticks / 2
                    } else {
                        duration_ticks
                    },
                    priority_hint: 0,
                    velocity,
                    base_octave,
                    metadata: {
                        let mut data = [0u8; 21];
                        data[0] = MUSICAL_LOGIC_KIND_GATE;
                        // THE EXACT TICK THIS EVENT WAS EMITTED AT, stamped as tick+1 so that 0
                        // still means "not stamped" and tick 0 is expressible.
                        //
                        // Downstream nodes seed themselves from the event's musical position and
                        // had to RECOVER it by dividing (sample_time - block_start_sample) by
                        // samples_per_tick — accurate to about a sample, which at 120bpm is 43
                        // nanoticks. That is far below the 1/64-quarter seed grid and would be
                        // harmless if onsets fell anywhere. They do not: a 16-step bar is 240000
                        // ticks per step and the grid is 15000, so EVERY euclidean onset lands
                        // exactly on a grid boundary, where a one-sample recovery error flips the
                        // cell and reseeds. That is why the same bar drew different notes at 64
                        // and 256 frames while random_degree's own unit test passed — the test
                        // feeds it a tick, and only the end-to-end render goes through the
                        // recovery.
                        let stamped = tick.wrapping_add(1).to_le_bytes();
                        data[1..9].copy_from_slice(&stamped);
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
            // 0 MEANS 0 here too — see patcher_process_euclidean. `degree` is the SIZE
            // of the random range, so 0 and 1 both mean "always degree 0"; that is what
            // degree_max below clamps, and it is a range being empty rather than a
            // sentinel being decoded.
            let cfg = &*(ctx_ref.node_config as *const PatcherRandomDegreeConfig);
            config.degree = cfg.degree;
            config.velocity = cfg.velocity;
            config.duration_ticks = cfg.duration_ticks;
        }
        let degree_max = config.degree.max(1);
        // Seeding needs a musical position, not an audio-block one. The old
        // seed mixed block_start_tick with the event's index inside the block,
        // so rendering the same music at a different buffer size produced
        // different notes.
        let tempo_bpm = if ctx_ref.tempo_bpm > 0.0 {
            ctx_ref.tempo_bpm as f64
        } else {
            DEFAULT_BPM
        };
        let samples_per_tick =
            (ctx_ref.sample_rate as f64 * 60.0) / (tempo_bpm * NANOTICKS_PER_QUARTER as f64);
        let count = *ctx_ref.event_count;
        let events = core::slice::from_raw_parts_mut(ctx_ref.event_buffer, count as usize);
        for entry in events.iter_mut() {
            if entry.type_ != 9 {
                continue;
            }
            let mut payload = MusicalLogicPayload {
                degree: 0,
                octave_offset: 0,
                sound: 0,
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
            // Recover the event's absolute musical tick. sample_time only
            // recovers to within a sample, and one sample spans many nanoticks,
            // so snap to a grid far finer than any musical subdivision but far
            // coarser than that jitter — otherwise a one-tick wobble would
            // reseed and defeat the whole point.
            // THE STAMP FIRST, the recovery only as a fallback. Recovering the tick from the
            // sample time is accurate to about a sample — 43 nanoticks at 120bpm — and the seed
            // grid is 15000, so it would be harmless if onsets fell anywhere. A 16-step bar puts
            // every onset exactly ON a grid boundary, where that jitter flips the cell.
            let tick = match stamped_tick(&payload.metadata) {
                Some(t) => t as i64,
                None if samples_per_tick > 0.0 => {
                    let sample_delta =
                        entry.sample_time as f64 - ctx_ref.block_start_sample as f64;
                    ctx_ref.block_start_tick as i64
                        + (sample_delta / samples_per_tick).round() as i64
                }
                None => ctx_ref.block_start_tick as i64,
            };
            const SEED_GRID: i64 = (NANOTICKS_PER_QUARTER / 64) as i64;
            // Reproducible AND decorrelated: fold the project seed and this node's id into
            // the musical position. Position alone made every random_degree node in the
            // song emit the SAME pitch at the same tick — two generators on two tracks
            // moved in lockstep, which reads as a bug and hides the second generator. The
            // odd multipliers are just decorrelating constants (golden-ratio derived); the
            // hash is mix64, so any one input changing scatters the whole result.
            let grid = tick.div_euclid(SEED_GRID) as u64;
            let seed = mix64(ctx_ref.seed ^ 0x9e37_79b9_7f4a_7c15u64
                                 .wrapping_mul(ctx_ref.node_id as u64 + 1))
                ^ grid;
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

/// SLICE SELECT: the same node as random_degree, writing `sound` instead of `pitch`.
///
/// The sampler's sound address has been a per-NOTE field since v32, so a CLIP could always say
/// which slice to play and a GENERATED note could not — it fell back to the keymap whatever the
/// graph did. This fills that field, which makes "euclidean rhythm x weighted-random slice over
/// an amen break, identical on every render" three nodes and a save.
///
/// SEEDED FROM THE MUSICAL POSITION, not the audio one, and this is the whole reason the node is
/// worth having rather than an easy `rand()`. The seed is the event's absolute tick snapped to a
/// 1/64-quarter grid — far finer than any subdivision anyone writes, far coarser than the
/// sub-sample jitter of recovering a tick from a sample time — folded with the project seed and
/// this node's id through mix64. So the same bar picks the same slices at 64, 256 or 1024 frames,
/// and two SliceSelect nodes in one song do not move in lockstep.
///
/// GATES ONLY, like random_degree: a note that was written with a pitch is the user's, and
/// rewriting what it plays would make the tracker lie about its own rows.
#[no_mangle]
pub extern "C" fn patcher_process_slice_select(ctx: *mut PatcherContext) {
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
        let mut config = PatcherSliceSelectConfig {
            base: 1,
            count: 8,
            _pad0: [0u8; 4],
        };
        if !ctx_ref.node_config.is_null()
            && ctx_ref.node_config_size as usize >= core::mem::size_of::<PatcherSliceSelectConfig>()
        {
            let cfg = &*(ctx_ref.node_config as *const PatcherSliceSelectConfig);
            config.base = cfg.base;
            config.count = cfg.count;
        }
        // A base of 0 would emit sound 0, which MEANS "no address, use the keymap" — so the node
        // would silently do nothing at all while looking configured. Clamped to 1, which is the
        // first real slot id, rather than refused: there is nowhere here to report a refusal to,
        // and every slot id in this engine starts at 1.
        let base = config.base.max(1);
        let count_max = config.count.max(1) as u64;
        let tempo_bpm = if ctx_ref.tempo_bpm > 0.0 {
            ctx_ref.tempo_bpm as f64
        } else {
            DEFAULT_BPM
        };
        let samples_per_tick =
            (ctx_ref.sample_rate as f64 * 60.0) / (tempo_bpm * NANOTICKS_PER_QUARTER as f64);
        let count = *ctx_ref.event_count;
        let events = core::slice::from_raw_parts_mut(ctx_ref.event_buffer, count as usize);
        for entry in events.iter_mut() {
            if entry.type_ != 9 {
                continue;
            }
            let mut payload = MusicalLogicPayload {
                degree: 0,
                octave_offset: 0,
                sound: 0,
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
            // EITHER KIND. `sound` is ORTHOGONAL to pitch — it names which slot plays, not
            // which note — so this node has no business refusing an event that already has a
            // pitch. random_degree takes gates only because it SUPPLIES the pitch and would
            // otherwise overwrite one the user typed; that argument does not transfer.
            let was_gate = payload.metadata[0] == MUSICAL_LOGIC_KIND_GATE;
            if !was_gate && payload.metadata[0] != MUSICAL_LOGIC_KIND_DEGREE {
                continue;
            }
            // THE STAMP FIRST, the recovery only as a fallback. Recovering the tick from the
            // sample time is accurate to about a sample — 43 nanoticks at 120bpm — and the seed
            // grid is 15000, so it would be harmless if onsets fell anywhere. A 16-step bar puts
            // every onset exactly ON a grid boundary, where that jitter flips the cell.
            let tick = match stamped_tick(&payload.metadata) {
                Some(t) => t as i64,
                None if samples_per_tick > 0.0 => {
                    let sample_delta =
                        entry.sample_time as f64 - ctx_ref.block_start_sample as f64;
                    ctx_ref.block_start_tick as i64
                        + (sample_delta / samples_per_tick).round() as i64
                }
                None => ctx_ref.block_start_tick as i64,
            };
            const SEED_GRID: i64 = (NANOTICKS_PER_QUARTER / 64) as i64;
            // A DIFFERENT DECORRELATING CONSTANT FROM random_degree'S, so that a graph running
            // both nodes does not have them agree: with the same constant, the same tick and the
            // same node id would give the same draw, and "slice N always plays with degree N"
            // is a correlation nobody asked for and nobody would find.
            let grid = tick.div_euclid(SEED_GRID) as u64;
            let seed = mix64(ctx_ref.seed ^ 0xbf58_476d_1ce4_e5b9u64
                                 .wrapping_mul(ctx_ref.node_id as u64 + 1))
                ^ grid;
            let pick = (mix64(seed) % count_max) as u16;
            payload.sound = base.saturating_add(pick);
            // A BARE GATE IS PROMOTED TO A NOTE, because otherwise this node is unusable alone.
            // The resolution path turns DEGREES into pitches and skips GATES by design, so
            // euclidean -> slice_select -> event_out would emit a rhythm that resolves to
            // nothing and renders silence — which looks exactly like the node not working.
            //
            // Degree 1 is the tonic, and for a chop that is the right answer rather than a
            // placeholder: the slot is chosen by `sound`, so the pitch is only there to make the
            // event a note at all, and a slice plays at its recorded speed when its slot has
            // pitch tracking off (which is what the chop mints). Anyone wanting the pitch to
            // move as well puts a random_degree BEFORE this node, where it still sees a gate.
            if was_gate {
                payload.degree = 1;
                payload.metadata[0] = MUSICAL_LOGIC_KIND_DEGREE;
                if payload.velocity == 0 {
                    payload.velocity = 100;
                }
                if payload.duration_ticks == 0 {
                    payload.duration_ticks = NANOTICKS_PER_QUARTER / 8;
                }
            }
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

    /// Renders gate events at fixed musical positions through
    /// random_degree, cutting the timeline into `block_frames`-sized blocks,
    /// and returns the degrees produced.
    fn render_degrees(block_frames: u64, gate_ticks: &[u64], node_id: u32) -> Vec<u8> {
        const SAMPLE_RATE: f64 = 48_000.0;
        const BPM: f64 = 120.0;
        let samples_per_tick = (SAMPLE_RATE * 60.0) / (BPM * NANOTICKS_PER_QUARTER as f64);
        let total_frames = 96_000u64;
        let mut degrees = Vec::new();

        let mut block_start_sample = 0u64;
        while block_start_sample < total_frames {
            let block_end_sample = block_start_sample + block_frames;
            let block_start_tick = (block_start_sample as f64 / samples_per_tick).round() as u64;
            let block_end_tick = (block_end_sample as f64 / samples_per_tick).round() as u64;

            let mut buffer: Vec<EventEntry> = Vec::new();
            for &tick in gate_ticks {
                let sample = (tick as f64 * samples_per_tick).round() as u64;
                if sample < block_start_sample || sample >= block_end_sample {
                    continue;
                }
                let payload = MusicalLogicPayload {
                    degree: 0,
                    octave_offset: 0,
                    sound: 0,
                    chord_id: 0,
                    duration_ticks: 0,
                    priority_hint: 0,
                    velocity: 100,
                    base_octave: 4,
                    metadata: {
                        let mut m = [0u8; 21];
                        m[0] = MUSICAL_LOGIC_KIND_GATE;
                        m
                    },
                };
                let mut entry = EventEntry {
                    sample_time: sample,
                    block_id: 0,
                    type_: 9,
                    size: core::mem::size_of::<MusicalLogicPayload>() as u16,
                    flags: 0,
                    payload: [0u8; 40],
                };
                unsafe {
                    core::ptr::copy_nonoverlapping(
                        &payload as *const MusicalLogicPayload as *const u8,
                        entry.payload.as_mut_ptr(),
                        core::mem::size_of::<MusicalLogicPayload>(),
                    );
                }
                buffer.push(entry);
            }

            if !buffer.is_empty() {
                let mut count = buffer.len() as u32;
                let capacity = buffer.len() as u32;
                let mut overflow_tick = 0u64;
                let config = PatcherRandomDegreeConfig {
                    degree: 7,
                    velocity: 100,
                    _pad0: [0u8; 2],
                    duration_ticks: 0,
                };
                let mut ctx = PatcherContext {
                    abi_version: PATCHER_ABI_VERSION,
                    block_start_tick,
                    block_end_tick,
                    block_start_sample,
                    sample_rate: SAMPLE_RATE as f32,
                    tempo_bpm: BPM as f32,
                    num_frames: block_frames as u32,
                    event_buffer: buffer.as_mut_ptr(),
                    event_capacity: capacity,
                    event_count: &mut count,
                    last_overflow_tick: &mut overflow_tick,
                    audio_channels: core::ptr::null_mut(),
                    num_channels: 0,
                    node_config: &config as *const _ as *const c_void,
                    node_config_size: core::mem::size_of::<PatcherRandomDegreeConfig>() as u32,
                    harmony_snapshot: core::ptr::null(),
                    harmony_count: 0,
                    mod_outputs: core::ptr::null_mut(),
                    mod_output_count: 0,
                    mod_output_samples: core::ptr::null_mut(),
                    mod_output_stride: 0,
                    mod_inputs: core::ptr::null_mut(),
                    mod_input_count: 0,
                    mod_input_stride: 0,
                                    node_id,
                    seed: 0,
};
                patcher_process_random_degree(&mut ctx);
                for entry in buffer.iter() {
                    let mut out = [0u8; core::mem::size_of::<MusicalLogicPayload>()];
                    out.copy_from_slice(
                        &entry.payload[..core::mem::size_of::<MusicalLogicPayload>()],
                    );
                    let parsed =
                        unsafe { &*(out.as_ptr() as *const MusicalLogicPayload) };
                    if parsed.metadata[0] == MUSICAL_LOGIC_KIND_DEGREE {
                        degrees.push(parsed.degree);
                    }
                }
            }
            block_start_sample = block_end_sample;
        }
        degrees
    }

    #[test]
    fn random_degree_is_independent_of_buffer_size() {
        // The same music rendered at different buffer sizes must produce the
        // same notes. The old seed mixed block_start_tick and the event's
        // index within the block, so it did not.
        let gates: Vec<u64> = (0..16).map(|i| i * (NANOTICKS_PER_QUARTER / 4)).collect();
        let at_512 = render_degrees(512, &gates, 0);
        let at_128 = render_degrees(128, &gates, 0);
        let at_300 = render_degrees(300, &gates, 0);

        assert_eq!(at_512.len(), gates.len(), "expected one degree per gate");
        assert_eq!(at_512, at_128, "buffer size 512 vs 128 changed the notes");
        assert_eq!(at_512, at_300, "an odd buffer size changed the notes");
        assert!(
            at_512.iter().any(|&d| d != at_512[0]),
            "degrees should vary across positions, got {at_512:?}"
        );
    }

    /// Two generator nodes at the SAME musical position must not emit the same pitch.
    /// Before the node id entered the hash the seed was position alone, so every
    /// random_degree in the song moved in lockstep — two generators on two tracks played
    /// identical lines, which reads as a broken second generator rather than a seeding bug.
    #[test]
    fn random_degree_decorrelates_per_node() {
        let gates: Vec<u64> = (0..16).map(|i| i * (NANOTICKS_PER_QUARTER / 4)).collect();
        let a = render_degrees(512, &gates, 1);
        let b = render_degrees(512, &gates, 2);
        assert_eq!(a.len(), b.len());
        assert!(!a.is_empty(), "no degrees generated");
        // Same node id must still be perfectly reproducible.
        assert_eq!(a, render_degrees(512, &gates, 1));
        // Different node ids must differ somewhere. (Per-event equality can coincide;
        // the whole sequence matching would mean the node id is not in the hash at all.)
        assert_ne!(a, b, "two nodes generated identical sequences — node id not seeded in");
    }

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
