//! Control surface for a running engine.
//!
//! Queries are read-only and always safe. Commands write into the UI command
//! ring, which is multi-producer (M2.18): writers reserve a slot with a
//! compare-and-swap, so the UI app and any number of CLI invocations can write
//! at the same time without losing each other's commands.

use std::thread;
use std::time::{Duration, Instant};

use daw_bridge::control::{default_shm_name, EngineHandle};
use daw_bridge::layout::{
    clip_appearances, UI_CLIP_EXTENT_HAS_ALTERNATE,
    UiChainCommandPayload, UiChordCommandPayload, UiClipWindowCommandPayload, UiCommandPayload,
    UiCommandType, UiPatcherPresetCommandPayload, UiSamplerKitRequestPayload,
    UiSamplerEmitRowsPayload, UiSamplerLoadPayload, UiSamplerMarkerPayload, UiSamplerSetSlotPayload, UiSamplerSlicePayload,
    UiSamplerEnvelopePayload, UiSamplerFilterPayload, UiSetRowOpsPayload, UiWaveformRequestPayload,
    MASTER_TRACK_ID, SAMPLER_ENV_BY_TARGET, SAMPLER_FILTER_SET_CUTOFF, SAMPLER_FILTER_SET_RESONANCE,
    ROW_OP_MASK_DELAY,
    ROW_OP_MASK_RETRIG_RAMP,
    ROW_OP_MASK_TRIG_CONDITION,
    ROW_OP_MASK_PROBABILITY, ROW_OP_MASK_RETRIGGER, ROW_OP_MASK_SOUND, ROW_OP_MASK_SOUND_OFFSET,
    SAMPLER_LOAD_FIXED_PITCH, SAMPLER_MARKER_ADD,
    SAMPLER_MARKER_MOVE, SAMPLER_MARKER_REMOVE, SAMPLER_SLICE_CLEAR, SAMPLER_SLICE_EQUAL,
    SAMPLER_DEVICE_FIELDS, SAMPLER_SLICE_TRANSIENT, SAMPLER_SLOT_FIELDS, UI_SAMPLER_KIT_SLOTS,
    UI_SAMPLER_SLOT_SOURCE_MISSING,
    UI_SAMPLER_SLOT_SLICE_MISSING,
};

const USAGE: &str = "\
daw-cli — control surface for a running engine

  daw-cli watch                    stream transport state (default)
  daw-cli get transport            transport + versions as JSON
  daw-cli get tracks               per-track state as JSON
  daw-cli get diffs                what the engine has reported on its outbound ring (peek, not drain)
  daw-cli do add-device --track N --kind sampler
  daw-cli do sampler-load --track N --device D --file NAME [--root 60] [--fixed-pitch]
                                   load a sample (project-relative name) and mint a slot
  daw-cli do sampler-load --track N --device D --files a.wav,b.wav --root 36
                                   lay a KIT: N samples on N consecutive keys, fixed pitch
  daw-cli do sampler-slot --track N --device D --slot S --field voice-group --value 1
                                   edit one slot field (--field with no match lists them all)
  daw-cli do sampler-slot-name --track N --device D --slot S --name 'kick 01'
                                   rename a pad. --name '' clears it; a name too long for the
                                   published field is REFUSED, never truncated
  daw-cli do sampler-vintage --track N [--device D] [--mod-set M] [--bits 0-16] [--rate HZ]
                                   bit/rate reduction before the filter; 0 turns one off
  daw-cli get sampler-kit --track N [--device D]
  daw-cli get sampler-envelope --track N [--device D] [--mod-set M]
                          [--modulator ID | --target amp|pan|pitch|cutoff|res]
                                   one modulator's SHAPE: points, both loop ranges, the
                                   release fade, time base and rate. Opcode 84 could write
                                   all of it and nothing could read it back
  daw-cli get patcher              the assembled patcher pool, with each node's OWNING DEVICE
                                   the device's slots, as the ENGINE has them
  daw-cli do sampler-slice --track N --source 1 [--mode transient|equal|clear] [--count 16] [--no-slots]
                          [--sensitivity 0-1000] [--count 16] [--snap TICKS] [--slots]
                                   chop a source; --slots makes one playable slot per slice
  daw-cli do sampler-marker --track N --source 1 --op add|move|remove [--marker ID] [--frame F]
  daw-cli do set-row-ops --track N --note ID [--clip ID] [--ret N] [--prob N] [--sound N] [--offset N] [--delay TICKS]
                         [--retrig-ramp -100..100] [--condition A:B] [--clear ret,prob,sound,offset,delay,ramp,cond]
  daw-cli do sampler-env --track N [--device ID] [--mod-set ID] [--amp|--modulator ID] --attack US --decay US --sustain MILLI --release US [--sync] [--rate MILLI] [--target amp|pan|pitch|cutoff|res] [--depth MILLI]
  daw-cli do sampler-env-draw --track N [--target amp|pan|pitch|cutoff|res] --points t,v[,tension[,step]];...
  daw-cli do sampler-filter --track N [--device D] [--mod-set ID] --type off|lp12|lp24|hp|bp [--cutoff 0..1000] [--resonance 0..1000]
  daw-cli do sampler-lfo --track N [--target amp|pan|pitch|cutoff|res] --hz F [--depth F] [--bias F] [--phase F] [--amount MILLI] [--sustain-loop A,B] [--release-loop A,B] [--release-fade US] [--sync] [--rate MILLI]
                                   nudge one boundary — ids are stable, so no row moves
  daw-cli do sampler-emit-rows --track N --source 1 [--at TICK] [--step TICKS] [--column C]
                                   write the pattern that reproduces the chop
  daw-cli do save-module NAME      write NAME.uni — one file, samples inside, sendable
  daw-cli do load-module NAME      unpack NAME.uni beside itself and open it
  daw-cli get arrangement          the markers (bar AND tick, resolved) + the meter map,
                                   the meter map, and the song end
  daw-cli get notes --track N      that track's notes from the published region
                                   (read-only: reads the published region)
  daw-cli get clip [--track N] [--bars N] [--grid]
                                   notes and chords in a window, as JSON or as
                                   a tracker-style text grid
  daw-cli do save [name]           save the project (default name: default)
  daw-cli do load [name]           load the project
  daw-cli do play                  toggle transport
  daw-cli do panic                 all sound off (CC120+CC123 everywhere)
  daw-cli do note --track N --nanotick T --pitch P
                  [--velocity V] [--duration D] [--column C] [--base V] [--local]
                                   --local records the edit on THIS APPEARANCE of the
                                   clip (an add or a mute on the placement) instead of
                                   on the clip, which every placement of it shares
  daw-cli do delete-note --track N --nanotick T --pitch P [--column C]
  daw-cli do notes --track N --pitches 60,64,67 [--start T] [--step S]
                   [--duration D] [--velocity V] [--column C]
                                   writes a phrase in one invocation
  daw-cli do chord --track N --nanotick T --degree D
                   [--quality Q] [--inversion I] [--octave O] [--duration D]
                   [--spread NANOTICKS] [--humanize-timing 0-255]
                   [--humanize-velocity 0-255] [--delete]
                                   --spread strums the chord; the humanize pair jitters each
                                   strike. All three persist and all three sound, and until
                                   now this surface sent zero for every one of them
  daw-cli do harmony --nanotick T --root R --scale S [--delete]
  daw-cli do stop                  halt the transport
  daw-cli do position --nanotick T move the playhead
  daw-cli do loop --start T --end T set the loop range
  daw-cli do harmony-quantize --track N [--on 0|1]
  daw-cli do note-overlap --track N [--on 0|1]
                                   cut-on-next OFF: a note entered over a sounding one no longer
                                   TRUNCATES it in the document. Off by default
  daw-cli do clip-grid --track N --clip ID [--lines M] [--num N] [--den D]
                                   a CLIP's own subdivision and meter — drawn BEFORE the track's
  daw-cli do audio-clip --track N --clip ID --field <start|gain|fade-in|fade-out> --value V
                                   an audio region's in-point (frames), gain (dB*100, signed,
                                   clamped -9600..2400) or either fade (nanoticks). One field
                                   per call; --value is required because 0 is legal for all four
  daw-cli do lines-per-beat --track N --lines M
                                   a lane's SUBDIVISION: tracker rows per beat, 1..31. Out of
                                   range is refused — the clip grid packs it in five bits
  daw-cli do sound-addressed --track N [--on 0|1]
  daw-cli do collapse --track N [--on 0|1]
  daw-cli do automation --track N --param ID --nanotick T --value V
                        [--discrete] [--device D]
                                   writes one automation point. --discrete makes the
                                   value STEP at each point instead of interpolating,
                                   and is fixed when the clip is created.
  daw-cli do placement-scope --track N --placement P [--on 0|1]
                                   mark ONE appearance as taking edits locally: a note
                                   typed into it becomes an override on it rather than a
                                   change to the clip every appearance shares. Chosen over
                                   a global mode because forgetting the toggle fails loudly
                                   (the note appears everywhere) while the wrong mode fails
                                   quietly (a fix that does not propagate).
  daw-cli do delete-automation --track N --param ID --nanotick T [--device D]
  daw-cli do move-automation --track N --param ID --from T --to T2 --value V [--device D]
                                   the other direction of `automation`: a lane was draw-only,
                                   so a point at the wrong tick could only be neutralised by
                                   writing another beside it. A move is delete + write.
  daw-cli do revert-overrides --track N --placement P
                                   clears every add and mute on one appearance, in one
                                   step — possible only because overrides are
                                   additive-only, so reverting is dropping two lists
  daw-cli do marker add --nanotick T [--name X] [--color RGB]
  daw-cli do marker remove --id ID
  daw-cli do marker rename --id ID --name X
  daw-cli do marker move --id ID --nanotick T
                                   a marker NAMES a tick. Total: moves no material,
                                   refuses nothing but a bad id. The span between two
                                   markers is implicit — see `get arrangement`.
  daw-cli do time-sig --sig 7/8 [--nanotick T] [--flatten]
                                   the SONG's meter. A point at T changes it from there;
                                   --flatten replaces the whole map with one signature.
  daw-cli do time insert|remove --nanotick T --bars N
                                   THE RIPPLE: inserts or removes N bars of arrangement
                                   time at T, carrying every placement, tempo point, key
                                   change, automation point, meter point and marker at or
                                   after it — one refusable, undoable transaction.
                                   Refuses a removal whose bars hold anything, and an
                                   insertion whose point falls inside a placement.
  daw-cli do add-placement --track N --clip C --at T --length L
                                   --at and --length are REQUIRED; the engine refuses
                                   the leave-unchanged sentinel as a position
  daw-cli do routing --track N [--audio-out none|master|track:M|input:M]
                     [--midi-out ...] [--audio-in ...] [--midi-in ...]
                     [--pre-fader 0|1]
                                   REPLACES every route on the track. Anything not
                                   named goes to its DEFAULT (audio-out master, the
                                   rest none) — the engine has no partial form and no
                                   routing read-back to merge against.
  daw-cli do patcher-node --track N --type euclidean|lfo|random-degree|slice-select|
                          passthrough|audio-passthrough|event-out
                                   --device D edits THAT DEVICE's own graph, which is the
                                   only form that is saved for a project with per-device
                                   graphs; without it the edit goes to the shared pool
  daw-cli do patcher-unnode --track N --node ID [--device D]
  daw-cli do patcher-connect --track N --src ID --dst ID
                             [--src-port P] [--dst-port P] [--kind event|audio|cv]
  daw-cli do patcher-config --track N --node ID --type T [type-specific flags]
                            euclidean: --steps --hits --offset --degree
                                       --octave-offset --velocity --base-octave
                                       --duration
                            random-degree: --degree --velocity --duration
                            slice-select: --base --count
                            lfo: --freq --depth --bias --phase
  daw-cli do patcher-save [name]
  daw-cli do mod-link --track N --source-device D --target-device D2
                      [--source-kind macro|lfo|envelope|patcher]
                      [--target-kind vst|patcher-param|patcher-macro]
                      [--source-id N] [--target-id N] [--depth X] [--bias X]
                      [--rate block|sample] [--enabled 0|1] [--link ID]
                                   modulation flows FORWARD: the source must not be
                                   later in the chain than the target (same device
                                   is fine). A refusal is named in the engine log.
  daw-cli do unmod-link --track N --link ID
  daw-cli do mod-target --track N --link ID --uid16 <32 hex chars>
                                   name the VST parameter a link drives
  daw-cli do macro --track N --device D --source-id N --value X
                                   turn a modulation source (a macro knob)
  daw-cli do remove-device --track N|master --device D
  daw-cli do move-device --track N|master --device D --index I
  daw-cli do mixer --track N [--gain-db X] [--pan Y]
                   [--mute 0|1] [--solo 0|1]
  daw-cli do quantize --track N [--grid T] [--strength 0..1000] [--swing -500..500]
                                   non-destructive: changes what SOUNDS, never the
                                   stored notes. --grid 0 turns it off.

`get clip` writes too: reading a window means asking the engine for one, and any
request is a write to the command ring. `get notes` does not — it reads the
published region, so it can never disturb a writer.

Write a phrase with `do notes`, not a shell loop over `do note`. The ring
carries all of them now, but the engine still accepts one clip edit per version
per track, and each invocation reads that version once at startup — so
back-to-back processes on the SAME track all claim the same base and only the
first survives. `do notes` numbers them itself. Different tracks do not collide
at all (M2.17); `get tracks` reports each track's own clip_version.

Queries are read-only. `do` writes to the UI command ring, which is
multi-producer as of M2.18: writers reserve a slot with a compare-and-swap and
publish it, so the UI app and any number of CLI invocations can write at once.
--force is gone; it is still accepted and ignored so old scripts keep working.

Environment:
  DAW_UI_SHM_NAME / DAW_SHM_NAME   shared memory name (default /daw_engine_ui)
  DAW_PROJECT_DIR                  where the engine reads/writes projects
";

fn escape(value: &str) -> String {
    value.replace('\\', "\\\\").replace('"', "\\\"")
}

/// THE NAME OUT OF A FIXED BYTE ARRAY, as a JSON string literal — quotes included.
///
/// One function because there were two, and they disagreed. `get extents` truncated at the first
/// NUL, which is what a C string means; `get shared` trimmed only TRAILING NULs, so a name like
/// `ab\0cd` came back with the NUL still in it — and Rust's `{:?}` renders that as `\u{0}`, which
/// is not valid JSON and would have made the whole array unparseable. Two readers of one field
/// agreeing on the common case and differing on the edge is the shape that costs the most here.
fn json_name(bytes: &[u8]) -> String {
    let end = bytes.iter().position(|&b| b == 0).unwrap_or(bytes.len());
    // `escape` handles the two characters that can end a JSON string early. Control characters
    // below 0x20 are not legal in a JSON string at all, so they are dropped rather than passed
    // through as themselves — a name is a label, and an unparseable array is worse than a label
    // missing a byte nobody can see.
    let text: String = String::from_utf8_lossy(&bytes[..end])
        .chars()
        .filter(|c| !c.is_control())
        .collect();
    format!("\"{}\"", escape(&text))
}

/// Reads `--key value` from the argument list.
fn flag(args: &[String], key: &str) -> Option<String> {
    let mut iter = args.iter();
    while let Some(arg) = iter.next() {
        if arg == key {
            return iter.next().cloned();
        }
        if let Some(rest) = arg.strip_prefix(&format!("{key}=")) {
            return Some(rest.to_string());
        }
    }
    None
}

fn flag_u64(args: &[String], key: &str, default: Option<u64>) -> Result<u64, String> {
    match flag(args, key) {
        Some(raw) => raw
            .parse::<u64>()
            .map_err(|_| format!("{key} expects a number, got {raw:?}")),
        None => default.ok_or_else(|| format!("{key} is required")),
    }
}

/// Builds a note command. `base_version` must equal the engine's current clip
/// version or the edit is rejected and a resync is requested — that is the
/// concurrency control, and an agent is subject to it exactly like the UI.
fn note_command(
    command: UiCommandType,
    args: &[String],
    base_version: u32,
) -> Result<UiCommandPayload, String> {
    let track = flag_u64(args, "--track", Some(0))? as u32;
    let nanotick = flag_u64(args, "--nanotick", None)?;
    let pitch = flag_u64(args, "--pitch", None)?;
    if pitch > 127 {
        return Err(format!("--pitch must be 0..127, got {pitch}"));
    }
    let velocity = flag_u64(args, "--velocity", Some(100))?;
    let duration = flag_u64(args, "--duration", Some(0))?;
    let column = daw_bridge::layout::edit_column(flag_u64(args, "--column", Some(0))?)
        .map_err(|e| format!("daw-cli: {e}"))?;
    // M3.24: --local records the edit on THIS APPEARANCE (an add or a mute on the
    // placement) instead of on the clip. Default is clip scope, which reaches every
    // placement — today's behaviour, unchanged. It is a flag and never inferred: which
    // one you meant is the difference between "fix the bass in chorus 1 and all three
    // choruses change" and "the hat you added to chorus 3 stays in chorus 3".
    let local = args.iter().any(|a| a == "--local");
    Ok(UiCommandPayload {
        command_type: command as u16,
        flags: column | if local { daw_bridge::layout::UI_EDIT_SCOPE_LOCAL } else { 0 },
        track_id: track,
        plugin_index: 0,
        note_pitch: pitch as u32,
        value0: velocity.min(127) as u32,
        note_nanotick_lo: (nanotick & 0xffff_ffff) as u32,
        note_nanotick_hi: (nanotick >> 32) as u32,
        note_duration_lo: (duration & 0xffff_ffff) as u32,
        note_duration_hi: (duration >> 32) as u32,
        base_version,
    })
}

fn get_transport(handle: &EngineHandle) -> i32 {
    let Some(snapshot) = handle.snapshot() else {
        eprintln!("daw-cli: no coherent snapshot available");
        return 1;
    };
    println!("{{");
    println!("  \"ui_version\": {},", snapshot.version);
    println!(
        "  \"playhead_nanotick\": {},",
        snapshot.ui_global_nanotick_playhead
    );
    println!("  \"transport_state\": {},", snapshot.ui_transport_state);
    println!(
        "  \"tempo_bpm\": {:.3},",
        snapshot.ui_tempo_milli_bpm as f64 / 1000.0
    );
    println!("  \"tempo_point_count\": {},", snapshot.ui_tempo_point_count);
    println!(
        "  \"song_time_sig\": \"{}/{}\",",
        snapshot.ui_song_time_sig_num, snapshot.ui_song_time_sig_den
    );
    println!("  \"track_count\": {},", handle.track_count());
    // The loop span drives what actually PLAYS, so it belongs in the transport report;
    // without it "why is my new section silent" has no observable answer.
    let (loop_start, loop_end) = handle.loop_range();
    println!("  \"loop_start\": {loop_start},");
    println!("  \"loop_end\": {loop_end},");
    println!("  \"clip_version\": {}", handle.clip_version());
    println!("}}");
    0
}

// do set-param <track> <device> <uid16hex> <milli>
// uid16hex is the 32-char hex of the param's durable id (from `get device-params`);
// milli is the normalized value in milli (0..1000).
fn set_param(handle: &EngineHandle, args: &[&str]) -> i32 {
    use daw_bridge::layout::UiSetParamPayload;
    let track: u32 = args.get(1).and_then(|s| s.parse().ok()).unwrap_or(0);
    let device: u32 = args.get(2).and_then(|s| s.parse().ok()).unwrap_or(0);
    let Some(hex) = args.get(3) else {
        eprintln!("daw-cli: usage: do set-param <track> <device> <uid16hex> <milli>");
        return 2;
    };
    let bytes: Vec<u8> = (0..hex.len())
        .step_by(2)
        .filter_map(|i| u8::from_str_radix(hex.get(i..i + 2)?, 16).ok())
        .collect();
    if bytes.len() != 16 {
        eprintln!("daw-cli: uid16 must be 32 hex chars (got {})", hex.len());
        return 2;
    }
    let mut uid16 = [0u8; 16];
    uid16.copy_from_slice(&bytes);
    let milli: u32 = args.get(4).and_then(|s| s.parse().ok()).unwrap_or(0);
    let p = UiSetParamPayload {
        command_type: UiCommandType::SetDeviceParam as u16,
        flags: 0,
        track_id: track,
        device_id: device,
        value_milli: milli,
        uid16,
        reserved: [0u8; 8],
    };
    match handle.send_set_param(p) {
        Ok(()) => {
            println!("{{ \"set_param\": {{ \"track\": {track}, \"device\": {device}, \"milli\": {milli} }} }}");
            0
        }
        Err(err) => {
            eprintln!("daw-cli: {err}");
            1
        }
    }
}

// do set-tempo <bpm> [position_nanotick]
// Flatten to <bpm> when no position, else insert/replace a point at position.
fn set_tempo(handle: &EngineHandle, args: &[&str]) -> i32 {
    let bpm: f64 = match args.get(1).and_then(|s| s.parse().ok()) {
        Some(b) if b > 0.0 => b,
        _ => {
            eprintln!("daw-cli: usage: do set-tempo <bpm> [position_nanotick]");
            return 2;
        }
    };
    let position: u64 = args.get(2).and_then(|s| s.parse().ok()).unwrap_or(0);
    // flags: 1 = flatten to a single tempo (no position given), 0 = point at position.
    let flags: u16 = if args.get(2).is_some() { 0 } else { 1 };
    let milli = (bpm * 1000.0).round() as u32;
    let p = UiCommandPayload {
        command_type: UiCommandType::SetTempo as u16,
        flags,
        track_id: 0,
        plugin_index: 0,
        note_pitch: 0,
        value0: milli,
        note_nanotick_lo: (position & 0xffff_ffff) as u32,
        note_nanotick_hi: (position >> 32) as u32,
        note_duration_lo: 0,
        note_duration_hi: 0,
        base_version: 0,
    };
    match handle.send_command(p) {
        Ok(()) => {
            println!("{{ \"set_tempo\": {:.3}, \"position\": {}, \"flags\": {} }}", bpm, position, flags);
            0
        }
        Err(err) => {
            eprintln!("daw-cli: {err}");
            1
        }
    }
}

/// Read-only note listing, straight out of the all-tracks published region. Unlike
/// `get clip` this asks the engine for nothing, so it writes nothing at all and cannot
/// perturb what it is measuring — which is what makes it usable as a test oracle and
/// as an observer while someone else is writing.
fn get_notes(handle: &EngineHandle, args: &[String]) -> i32 {
    let track = match flag_u64(args, "--track", Some(0)) {
        Ok(v) => v as u32,
        Err(err) => {
            eprintln!("daw-cli: {err}");
            return 2;
        }
    };
    let Some(snapshot) = handle.read_track_clip(track) else {
        eprintln!("daw-cli: no published clip region for track {track}");
        return 1;
    };
    let count = (snapshot.note_count as usize).min(snapshot.notes.len());
    println!("{{");
    println!("  \"track_id\": {},", snapshot.track_id);
    println!("  \"clip_version\": {},", snapshot.clip_version);
    println!("  \"note_count\": {count},");
    println!("  \"notes\": [");
    for index in 0..count {
        let note = snapshot.notes[index];
        let comma = if index + 1 == count { "" } else { "," };
        // DURATION, derived here from the published tOn/tOff rather than stored twice. Its
        // absence was a real gap: the scheduler skips a zero-duration event outright, so a note
        // with no length is published, saved and badge-counted while being permanently silent —
        // and this output could not tell it from a note that sounds. A test written against it
        // asserted the note EXISTED and passed against an engine that stored length 0.
        let duration = note.t_off.saturating_sub(note.t_on);
        // THE ROW OPS, AND THE ID THAT ADDRESSES THEM. All six ops are published in UiClipNote
        // and all six were shown by nothing: `do set-row-ops` could write them and the only way
        // to read one back was to save the project and read the file — which is the exact
        // complaint sampler_kit_check's header makes about the kit, one feature along.
        //
        // note_id matters most and is the reason this was a hole rather than an inconvenience:
        // set-row-ops is ADDRESSED BY IT (`--note ID`), so the write path needed an identifier
        // the read path would not give you. An agent had to guess, or track ids it never saw.
        //
        // Provenance too: a note that is MUTED on this appearance, or an ADD that exists only
        // here, is a different note from one the clip carries — and both drew identically.
        let muted = note.placement_flags & 1 != 0;
        let is_add = note.placement_flags & 2 != 0;
        println!(
            "    {{ \"nanotick\": {}, \"duration\": {duration}, \"pitch\": {}, \"velocity\": {}, \"column\": {}, \
             \"dev\": {}, \"delay\": {}, \"sounds_at\": {}, \"note_id\": {}, \"retrigger\": {}, \
             \"probability\": {}, \"sound\": {}, \"sound_offset\": {}, \"retrig_ramp\": {}, \
             \"trig_condition\": {}, \"placement_id\": {}, \"muted\": {muted}, \"is_add\": {is_add} }}{comma}",
            note.t_on, note.pitch, note.velocity, note.column,
            note.dev_nanoticks, note.delay_nanoticks,
            // The AUTHORED tick plus both offsets — quantize moves the tick and the row-op
            // delay is added after it, so the sounding position is the sum.
            note.t_on as i64 + note.dev_nanoticks as i64 + note.delay_nanoticks as i64,
            note.note_id, note.retrigger, note.probability, note.sound, note.sound_offset,
            note.retrig_ramp, note.trig_condition, note.placement_id
        );
    }
    println!("  ]");
    println!("}}");
    0
}

fn get_tracks(handle: &EngineHandle) -> i32 {
    let Some(snapshot) = handle.snapshot() else {
        eprintln!("daw-cli: no coherent snapshot available");
        return 1;
    };
    let count = handle.track_count() as usize;
    let names = handle.read_track_names();
    let devices = handle.read_track_device_names();
    let (ids, flags) = handle.read_track_ids_and_flags();
    // The per-track boolean byte, which carries harmony-quantize alongside mute/solo.
    let mixers = handle.read_mixer();
    println!("{{");
    // THE MIXER VERSION, which nothing could read until now. It is the word an optimistic UI
    // strip waits on to clear a pending fader move, and it was invisible from every surface —
    // so the bug where a master-only edit published a correct value and never moved this word
    // was not merely uncaught, it was unobservable.
    println!("  \"mixer_version\": {},", handle.mixer_version());
    println!("  \"track_count\": {count},");
    println!("  \"tracks\": [");
    for index in 0..count {
        let rms = snapshot
            .ui_track_peak_rms
            .get(index)
            .copied()
            .unwrap_or(0.0);
        let name = names.get(index).map(String::as_str).unwrap_or("");
        let device = devices.get(index).map(String::as_str).unwrap_or("");
        // The stable id the UI keys on (kMasterTrackId for the master strip), not the
        // moving slot; plus a master marker decoded from the flags.
        let id = ids.get(index).copied().unwrap_or(index as u32);
        let flag = flags.get(index).copied().unwrap_or(0);
        let is_master = flag & daw_bridge::layout::UI_TRACK_FLAG_MASTER != 0;
        // `track_count` is the EXTENT, not a count of live tracks: ids never renumber, so a
        // removed track leaves a tombstone that keeps its id put and is published with
        // ABSENT for readers to skip. Printing it matters — without it a tombstoned slot and
        // a real empty track look identical here, which is exactly what made "a track
        // disappears on load" hard to pin down: the sparse-id load bug published a phantom
        // lane in an unclaimed slot and this output could not tell it from a real one.
        let is_absent = flag & daw_bridge::layout::UI_TRACK_FLAG_ABSENT != 0;
        // SetTrackHarmonyQuantize had no read-back at all, so a toggle for it could only be
        // write-only. Printed here for the same reason `absent` is: state you cannot read is
        // state a control has to invent.
        let harmony_q = mixers
            .get(index)
            .map(|m| m.flags & daw_bridge::layout::MIX_FLAG_HARMONY_QUANTIZE != 0)
            .unwrap_or(false);
        // Published for the same reason harmony_quantize is, and it matters more: this flag
        // decides which SLOT a note plays, so a UI that had to guess would draw the kit's
        // mapping backwards.
        let collapsed = flag & daw_bridge::layout::UI_TRACK_FLAG_COLLAPSED != 0;
        // v34: sized for the TRACK, not for the rows on screen — a column computed from a
        // window reflows as you scroll.
        let ops_width = mixers.get(index).map(|m| m.ops_width).unwrap_or(0);
        let sound_addressed = mixers
            .get(index)
            .map(|m| m.flags & daw_bridge::layout::MIX_FLAG_SOUND_ADDRESSED != 0)
            .unwrap_or(false);
        // THE MIXER, THE LANE QUANTIZE AND THE PARENTAGE — all published per track since v12/v20
        // and readable from no `get` verb at all. Every one of them is SETTABLE (`do mixer`,
        // `do quantize`, the child-track derivation), so each was a control whose own state this
        // surface could not show — the same defect this repo has spent two days closing on the
        // other side of the wire, here on the reading side of the CLI.
        //
        // Units are the ones a caller SENDS, not the wire's: gain in dB and pan in -1..1, because
        // `do mixer --gain-db` speaks dB, and a read-back in millibels would be a second unit for
        // one fact. Swing is the signed value the model holds; the wire's +500 bias is the
        // payload's business and stops at the engine.
        let gain_db = mixers.get(index).map(|m| m.gain_millibels as f64 / 100.0).unwrap_or(0.0);
        let pan = mixers.get(index).map(|m| m.pan_thousandths as f64 / 1000.0).unwrap_or(0.0);
        let mute = mixers
            .get(index)
            .map(|m| m.flags & daw_bridge::layout::MIX_FLAG_MUTE != 0)
            .unwrap_or(false);
        let solo = mixers
            .get(index)
            .map(|m| m.flags & daw_bridge::layout::MIX_FLAG_SOLO != 0)
            .unwrap_or(false);
        // parent_id 0 is ambiguous on its own — it is both "top level" and "child of track 0" —
        // which is why the flag exists and why both are printed rather than just the id.
        let has_parent = flag & daw_bridge::layout::UI_TRACK_FLAG_HAS_PARENT != 0;
        let parent_id = snapshot.ui_track_parent_id.get(index).copied().unwrap_or(0);
        let q_grid = snapshot.ui_track_quantize_grid.get(index).copied().unwrap_or(0);
        let q_strength = snapshot.ui_track_quantize_strength.get(index).copied().unwrap_or(0);
        let q_swing = snapshot.ui_track_quantize_swing.get(index).copied().unwrap_or(0);
        // Published since v10 and honoured by the tracker's per-lane grid — and until opcode 92
        // nothing could SET it, so it was read-only state nobody could reach. Printed here for
        // the same reason harmony_quantize is: a control that cannot read its own value has to
        // invent one, and for a subdivision that means drawing the lane on the wrong grid.
        let lines_per_beat = snapshot
            .ui_lines_per_beat
            .get(index)
            .copied()
            .unwrap_or(0);
        // Whether an edit in this lane destroys the note above it. A UI that had to guess this
        // could not tell the player whether the row they are about to type will shorten another.
        let allow_note_overlap = mixers
            .get(index)
            .map(|m| m.flags & daw_bridge::layout::MIX_FLAG_ALLOW_NOTE_OVERLAP != 0)
            .unwrap_or(false);
        let comma = if index + 1 == count { "" } else { "," };
        // M2.17: this track's OWN clip version — the base an edit to this track must
        // present. The global `clip_version` in `get transport` moves whenever ANY
        // track changes and is no longer the right base for a track-scoped edit.
        let clip_version = handle.clip_version_for_track(id);
        println!(
            "    {{ \"track_id\": {id}, \"name\": {name:?}, \"device\": {device:?}, \"master\": {is_master}, \"absent\": {is_absent}, \"harmony_quantize\": {harmony_q}, \"sound_addressed\": {sound_addressed}, \"collapsed\": {collapsed}, \"lines_per_beat\": {lines_per_beat}, \"gain_db\": {gain_db:.2}, \"pan\": {pan:.3}, \"mute\": {mute}, \"solo\": {solo}, \"has_parent\": {has_parent}, \"parent_id\": {parent_id}, \"quantize_grid\": {q_grid}, \"quantize_strength\": {q_strength}, \"quantize_swing\": {q_swing}, \"allow_note_overlap\": {allow_note_overlap}, \"ops_width\": {ops_width}, \"clip_version\": {clip_version}, \"peak_rms\": {rms} }}{comma}"
        );
    }
    println!("  ]");
    println!("}}");
    0
}

fn send_named(handle: &EngineHandle, command: UiCommandType, name: &str) -> i32 {
    // Built by daw-bridge, not here: the sidecar needs the same command, and two
    // copies of a transmute into a shared-memory slot is one copy too many.
    match handle.send_command(UiPatcherPresetCommandPayload::named(command, name).as_command()) {
        Ok(()) => 0,
        Err(err) => {
            eprintln!("daw-cli: {err}");
            1
        }
    }
}

/// Writes a phrase in one invocation, numbering the base versions itself.
/// The engine advances one clip version per applied edit and rejects anything
/// stale, so a shell loop over `do note` loses every note after the first.
/// M1.13 lane quantize. No base_version: this moves no authored note, so gating it on
/// a clip version would reject it whenever someone else was mid-edit, for a change that
/// cannot conflict with theirs.
/// Patcher node types, by name.
/// Encode `--device N` into the patcher payload's `flags`, which is where per-device addressing
/// lives (the payload is exactly 40 bytes and full).
///
/// Bit 15 marks the id as PRESENT and is not decoration: device ids start at 0, so a bare 0 cannot
/// mean "unspecified". Without `--device` the command takes the legacy whole-pool path — which for
/// a project with per-device graphs means the edit is applied to the shared pool and never saved,
/// so pass it.
fn patcher_device_flags(args: &[String]) -> u16 {
    use daw_bridge::layout as L;
    match flag_u64(args, "--device", None) {
        Ok(id) => L::UI_PATCHER_FLAG_HAS_DEVICE_ID | ((id as u16) & L::UI_PATCHER_DEVICE_ID_MASK),
        Err(_) => 0,
    }
}

fn parse_node_type(raw: &str) -> Result<u32, String> {
    use daw_bridge::layout as L;
    Ok(match raw {
        "euclidean" => L::PATCHER_NODE_EUCLIDEAN,
        "lfo" => L::PATCHER_NODE_LFO,
        "random-degree" => L::PATCHER_NODE_RANDOM_DEGREE,
        "slice-select" => L::PATCHER_NODE_SLICE_SELECT,
        "passthrough" => L::PATCHER_NODE_PASSTHROUGH,
        "audio-passthrough" => L::PATCHER_NODE_AUDIO_PASSTHROUGH,
        "event-out" => L::PATCHER_NODE_EVENT_OUT,
        "rust-kernel" => L::PATCHER_NODE_RUST_KERNEL,
        other => return Err(format!(
            "--type: expected euclidean|lfo|random-degree|slice-select|passthrough|\
             audio-passthrough|event-out|rust-kernel, got {other:?}"
        )),
    })
}

/// SetPatcherNodeConfig's `config` block. EXPLICIT little-endian per node type — the
/// engine reads it field by field rather than memcpy'ing a struct, because a raw copy
/// truncated Euclidean and coupled the wire to C++ padding. Building it here by the same
/// documented layout keeps the two ends honest.
/// The 16-byte node config, from this verb's flags.
///
/// READS THE FLAGS, PACKS NOWHERE. `daw_bridge::layout::pack_patcher_node_config` owns the byte
/// layout and the clamping; this only decides which flag supplies which of the eight positional
/// values, and what a missing flag defaults to. Before that split this function cast straight
/// into each field's width while the sidecar clamped, so `--steps 70000` set 4464 here and 65535
/// through the browser — the same op, the same engine, two answers, and a wrapped value is an
/// ordinary-looking number on arrival.
fn build_node_config(args: &[String], node_type: u32) -> Result<[u8; 16], String> {
    use daw_bridge::layout as L;
    // 0 MEANS 0 (M0.6): these are sent verbatim, so `--hits 0` is silence and not "use the
    // default five". The defaults below are the ENGINE'S, from default_patcher_node_config.
    let fields: [i64; 8] = if node_type == L::PATCHER_NODE_EUCLIDEAN {
        [
            flag_u64(args, "--steps", Some(16))? as i64,
            flag_u64(args, "--hits", Some(5))? as i64,
            flag_u64(args, "--offset", Some(0))? as i64,
            flag_u64(args, "--degree", Some(1))? as i64,
            flag_i64(args, "--octave-offset", 0)?,
            flag_u64(args, "--velocity", Some(100))? as i64,
            flag_u64(args, "--base-octave", Some(4))? as i64,
            flag_u64(args, "--duration", Some(0))? as i64,
        ]
    } else if node_type == L::PATCHER_NODE_RANDOM_DEGREE {
        [
            flag_u64(args, "--degree", Some(8))? as i64,
            flag_u64(args, "--velocity", Some(100))? as i64,
            flag_u64(args, "--duration", Some(0))? as i64,
            0, 0, 0, 0, 0,
        ]
    } else if node_type == L::PATCHER_NODE_SLICE_SELECT {
        [
            flag_u64(args, "--base", Some(1))? as i64,
            flag_u64(args, "--count", Some(8))? as i64,
            0, 0, 0, 0, 0, 0,
        ]
    } else if node_type == L::PATCHER_NODE_LFO {
        // Milli-units on the wire, mirroring the read-back; the engine stores floats.
        let milli = |v: f64| (v * 1000.0).round() as i64;
        [
            milli(flag_f64(args, "--freq", 1.0)?),
            milli(flag_f64(args, "--depth", 1.0)?),
            milli(flag_f64(args, "--bias", 0.0)?),
            milli(flag_f64(args, "--phase", 0.0)?),
            0, 0, 0, 0,
        ]
    } else {
        return Err(
            "--type: only euclidean, random-degree, slice-select and lfo carry a config"
                .to_string(),
        );
    };
    L::pack_patcher_node_config(node_type, &fields).map_err(|e| e.to_string())
}

/// AddModLink / RemoveModLink. The engine validates that both devices exist and that
/// modulation flows FORWARD (source not later in the chain than target; same device is
/// legal and is the common case with per-device patchers). A refusal is reported as
/// `modlink.rejected` in the engine log and history.jsonl with a named reason.
fn mod_link_command(
    args: &[String],
    removing: bool,
) -> Result<daw_bridge::layout::UiModLinkCommandPayload, String> {
    use daw_bridge::layout as L;
    let track = flag_u64(args, "--track", Some(0))? as u32;
    let link = flag_u64(args, "--link", Some(L::MOD_LINK_ID_AUTO as u64))? as u32;
    if removing {
        // A remove needs only the id; sending kinds would imply they are matched on.
        return Ok(L::UiModLinkCommandPayload {
            command_type: UiCommandType::RemoveModLink as u16,
            track_id: track,
            link_id: link,
            ..Default::default()
        });
    }
    let source_kind = match flag(args, "--source-kind").as_deref().unwrap_or("macro") {
        "macro" => L::MOD_SOURCE_MACRO,
        "lfo" => L::MOD_SOURCE_LFO,
        "envelope" => L::MOD_SOURCE_ENVELOPE,
        "patcher" => L::MOD_SOURCE_PATCHER_NODE_OUTPUT,
        other => return Err(format!(
            "--source-kind: expected macro|lfo|envelope|patcher, got {other:?}"
        )),
    };
    let target_kind = match flag(args, "--target-kind").as_deref().unwrap_or("vst") {
        "vst" => L::MOD_TARGET_VST_PARAM,
        "patcher-param" => L::MOD_TARGET_PATCHER_PARAM,
        "patcher-macro" => L::MOD_TARGET_PATCHER_MACRO,
        other => return Err(format!(
            "--target-kind: expected vst|patcher-param|patcher-macro, got {other:?}"
        )),
    };
    let rate = match flag(args, "--rate").as_deref().unwrap_or("block") {
        "block" => L::MOD_RATE_BLOCK,
        "sample" => L::MOD_RATE_SAMPLE,
        other => return Err(format!("--rate: expected block|sample, got {other:?}")),
    };
    let enabled = flag_u64(args, "--enabled", Some(1))? != 0;
    // The engine packs these four into `flags`; the layout is stated in the payload's
    // comment on both sides, and getting it wrong here would silently make every link a
    // block-rate macro->vst link.
    let flags = (source_kind & 0x0F)
        | ((target_kind & 0x0F) << 4)
        | ((rate & 0x03) << 8)
        | (if enabled { 1u16 << 10 } else { 0 });
    Ok(L::UiModLinkCommandPayload {
        command_type: UiCommandType::AddModLink as u16,
        flags,
        track_id: track,
        base_version: 0,
        link_id: link,
        source_device_id: flag_u64(args, "--source-device", None)? as u32,
        source_id: flag_u64(args, "--source-id", Some(0))? as u32,
        target_device_id: flag_u64(args, "--target-device", None)? as u32,
        target_id: flag_u64(args, "--target-id", Some(0))? as u32,
        depth: flag_f64(args, "--depth", 1.0)? as f32,
        bias: flag_f64(args, "--bias", 0.0)? as f32,
    })
}

/// A 32-hex-character plugin parameter uid.
fn parse_uid16(raw: &str) -> Result<[u8; 16], String> {
    let clean: String = raw.chars().filter(|c| !c.is_whitespace()).collect();
    if clean.len() != 32 || !clean.chars().all(|c| c.is_ascii_hexdigit()) {
        return Err(format!(
            "--uid16 expects 32 hex characters (16 bytes), got {} character(s)",
            clean.len()
        ));
    }
    let mut out = [0u8; 16];
    for i in 0..16 {
        out[i] = u8::from_str_radix(&clean[i * 2..i * 2 + 2], 16)
            .map_err(|_| "--uid16: not hex".to_string())?;
    }
    Ok(out)
}

/// One route spec: `none`, `master`, `track:N`, or `input:N`.
fn parse_route(raw: &str, what: &str) -> Result<(u8, u32), String> {
    use daw_bridge::layout::{
        TRACK_ROUTE_EXTERNAL_INPUT, TRACK_ROUTE_MASTER, TRACK_ROUTE_NONE, TRACK_ROUTE_TRACK,
    };
    if raw == "none" {
        return Ok((TRACK_ROUTE_NONE, 0));
    }
    if raw == "master" {
        return Ok((TRACK_ROUTE_MASTER, 0));
    }
    if let Some(rest) = raw.strip_prefix("track:") {
        let id = rest
            .parse::<u32>()
            .map_err(|_| format!("{what}: track:N needs a number, got {rest:?}"))?;
        return Ok((TRACK_ROUTE_TRACK, id));
    }
    if let Some(rest) = raw.strip_prefix("input:") {
        let id = rest
            .parse::<u32>()
            .map_err(|_| format!("{what}: input:N needs a number, got {rest:?}"))?;
        return Ok((TRACK_ROUTE_EXTERNAL_INPUT, id));
    }
    Err(format!(
        "{what}: expected none | master | track:N | input:N, got {raw:?}"
    ))
}

/// SetTrackRouting. REPLACE, not merge — the engine writes all four routes from one
/// payload, and there is no routing read-back to merge against, so anything not named
/// here goes to its DEFAULT (audio-out master, everything else none) rather than to
/// whatever it happens to be. That is stated in the usage text and echoed in the output,
/// because a command that silently resets three routes while you set one is a trap.
fn routing_command(args: &[String]) -> Result<daw_bridge::layout::UiTrackRoutingPayload, String> {
    use daw_bridge::layout::{TRACK_ROUTE_MASTER, TRACK_ROUTE_NONE};
    let track = flag_u64(args, "--track", Some(0))? as u32;
    let (midi_in_kind, midi_in_id) = match flag(args, "--midi-in") {
        Some(v) => parse_route(&v, "--midi-in")?,
        None => (TRACK_ROUTE_NONE, 0),
    };
    let (midi_out_kind, midi_out_id) = match flag(args, "--midi-out") {
        Some(v) => parse_route(&v, "--midi-out")?,
        None => (TRACK_ROUTE_NONE, 0),
    };
    let (audio_in_kind, audio_in_id) = match flag(args, "--audio-in") {
        Some(v) => parse_route(&v, "--audio-in")?,
        None => (TRACK_ROUTE_NONE, 0),
    };
    // The engine's own default for a track's output is the master bus, so an omitted
    // --audio-out means master rather than none. Defaulting it to none would silence the
    // track, which is not what "I did not mention it" should mean.
    let (audio_out_kind, audio_out_id) = match flag(args, "--audio-out") {
        Some(v) => parse_route(&v, "--audio-out")?,
        None => (TRACK_ROUTE_MASTER, 0),
    };
    let pre_fader = flag_u64(args, "--pre-fader", Some(1))? != 0;
    Ok(daw_bridge::layout::UiTrackRoutingPayload {
        command_type: UiCommandType::SetTrackRouting as u16,
        flags: if pre_fader { 1 } else { 0 },
        track_id: track,
        base_version: 0,
        midi_in_kind,
        midi_out_kind,
        audio_in_kind,
        audio_out_kind,
        midi_in_track_id: midi_in_id,
        midi_out_track_id: midi_out_id,
        audio_in_track_id: audio_in_id,
        audio_out_track_id: audio_out_id,
        midi_in_input_id: midi_in_id,
        audio_in_input_id: audio_in_id,
    })
}

fn quantize_command(args: &[String]) -> Result<UiCommandPayload, String> {
    let track = flag_u64(args, "--track", Some(0))? as u32;
    let grid = flag_u64(args, "--grid", Some(0))?;
    let strength = flag_u64(args, "--strength", Some(1000))?.min(1000) as u32;
    let swing_raw = flag_i64(args, "--swing", 0)?;
    if !(-500..=500).contains(&swing_raw) {
        return Err(format!("--swing must be -500..500, got {swing_raw}"));
    }
    let swing = (swing_raw + daw_bridge::layout::LANE_QUANTIZE_SWING_BIAS as i64) as u32;
    Ok(UiCommandPayload {
        command_type: UiCommandType::SetLaneQuantize as u16,
        flags: 0,
        track_id: track,
        plugin_index: 0,
        note_pitch: swing,
        value0: strength,
        note_nanotick_lo: (grid & 0xffff_ffff) as u32,
        note_nanotick_hi: (grid >> 32) as u32,
        note_duration_lo: 0,
        note_duration_hi: 0,
        base_version: 0,
    })
}

fn write_notes(handle: &EngineHandle, args: &[String]) -> i32 {
    let Some(raw) = flag(args, "--pitches") else {
        eprintln!("daw-cli: --pitches is required, e.g. --pitches 60,64,67");
        return 2;
    };
    let mut pitches = Vec::new();
    for token in raw.split(',').filter(|t| !t.is_empty()) {
        match token.trim().parse::<u32>() {
            Ok(pitch) if pitch <= 127 => pitches.push(pitch),
            _ => {
                eprintln!("daw-cli: bad pitch {token:?} (expected 0..127)");
                return 2;
            }
        }
    }
    if pitches.is_empty() {
        eprintln!("daw-cli: --pitches listed no notes");
        return 2;
    }

    let track = match flag_u64(args, "--track", Some(0)) {
        Ok(value) => value as u32,
        Err(err) => {
            eprintln!("daw-cli: {err}");
            return 2;
        }
    };
    let start = flag_u64(args, "--start", Some(0)).unwrap_or(0);
    let step = flag_u64(args, "--step", Some(NANOTICKS_PER_QUARTER))
        .unwrap_or(NANOTICKS_PER_QUARTER);
    let duration = flag_u64(args, "--duration", Some(step)).unwrap_or(step);
    let velocity = flag_u64(args, "--velocity", Some(100)).unwrap_or(100).min(127);
    let column = match daw_bridge::layout::edit_column(
            flag_u64(args, "--column", Some(0)).unwrap_or(0)) {
        Ok(c) => c,
        Err(e) => { eprintln!("daw-cli: {e}"); std::process::exit(2) }
    };

    // M2.17: the base is this TRACK's version. Each note consumes one, so the run
    // numbers itself from there — and because acceptance is per track, a phrase written
    // here is no longer invalidated by someone editing a different track mid-run.
    let mut base = handle.clip_version_for_track(track);
    let mut sent = 0usize;
    for (index, pitch) in pitches.iter().enumerate() {
        let nanotick = start + step * index as u64;
        let payload = UiCommandPayload {
            command_type: UiCommandType::WriteNote as u16,
            flags: column,
            track_id: track,
            plugin_index: 0,
            note_pitch: *pitch,
            value0: velocity as u32,
            note_nanotick_lo: (nanotick & 0xffff_ffff) as u32,
            note_nanotick_hi: (nanotick >> 32) as u32,
            note_duration_lo: (duration & 0xffff_ffff) as u32,
            note_duration_hi: (duration >> 32) as u32,
            base_version: base,
        };
        if let Err(err) = handle.send_command(payload) {
            eprintln!("daw-cli: {err} after {sent} notes");
            return 1;
        }
        sent += 1;
        base = base.wrapping_add(1);
    }
    println!("{{ \"sent\": {sent}, \"first_base_version\": {} }}", base - sent as u32);
    0
}

fn flag_i64(args: &[String], key: &str, default: i64) -> Result<i64, String> {
    match flag(args, key) {
        Some(raw) => raw
            .parse::<i64>()
            .map_err(|_| format!("{key} expects a whole number, got {raw:?}")),
        None => Ok(default),
    }
}

/// A param id packed into the 16-byte wire field, REFUSING anything that would not survive it.
///
/// 15 bytes, not 16: the read-back slot nul-terminates within its own 16-byte array, so a name
/// that filled all 16 would be written in full and read back one byte short — the write and the
/// answer would name different lanes forever, with nothing reporting it. Truncating silently is
/// worse than refusing, because the caller is told the lane it asked for was the lane it got.
fn param_id_bytes(param: &str) -> Result<[u8; 16], String> {
    let b = param.as_bytes();
    if b.len() > 15 {
        return Err(format!(
            "--param {param:?} is {} bytes; the wire field holds 15. Truncating it would write \
to a different lane than the one you named, so this is refused rather than guessed.",
            b.len()
        ));
    }
    let mut out = [0u8; 16];
    out[..b.len()].copy_from_slice(b);
    Ok(out)
}

fn flag_f64(args: &[String], key: &str, default: f64) -> Result<f64, String> {
    match flag(args, key) {
        Some(raw) => raw
            .parse::<f64>()
            .map_err(|_| format!("{key} expects a number, got {raw:?}")),
        None => Ok(default),
    }
}

fn preview_command(args: &[String]) -> Result<UiCommandPayload, String> {
    let track = flag_u64(args, "--track", Some(0))? as u32;
    let pitch = flag_u64(args, "--pitch", Some(60))?.min(127) as u32;
    let velocity = flag_u64(args, "--velocity", Some(100))?.min(127) as u32;
    // --on 1 = note-on (default), --on 0 = note-off for this pitch.
    let on = flag_u64(args, "--on", Some(1))? != 0;
    Ok(UiCommandPayload {
        command_type: UiCommandType::PreviewNote as u16,
        flags: if on { daw_bridge::layout::PREVIEW_NOTE_FLAG_ON } else { 0 },
        track_id: track,
        plugin_index: 0,
        note_pitch: pitch,
        value0: velocity,
        note_nanotick_lo: 0,
        note_nanotick_hi: 0,
        note_duration_lo: 0,
        note_duration_hi: 0,
        base_version: 0,
    })
}

fn open_editor_command(track: u32, device: u32) -> UiCommandPayload {
    UiCommandPayload {
        command_type: UiCommandType::OpenPluginEditor as u16,
        flags: 0,
        track_id: track,
        plugin_index: 0,
        note_pitch: 0,
        value0: device,
        note_nanotick_lo: 0,
        note_nanotick_hi: 0,
        note_duration_lo: 0,
        note_duration_hi: 0,
        base_version: 0,
    }
}

#[allow(clippy::too_many_arguments)]
fn placement_command(
    cmd: UiCommandType,
    track: u32,
    value0: u32,
    at: u64,
    length: u64,
    note_pitch: u32,
) -> UiCommandPayload {
    UiCommandPayload {
        command_type: cmd as u16,
        flags: 0,
        track_id: track,
        plugin_index: 0,
        note_pitch,
        value0,
        note_nanotick_lo: (at & 0xffff_ffff) as u32,
        note_nanotick_hi: (at >> 32) as u32,
        note_duration_lo: (length & 0xffff_ffff) as u32,
        note_duration_hi: (length >> 32) as u32,
        base_version: 0,
    }
}

fn track_structure_command(command: UiCommandType, track: u32) -> UiCommandPayload {
    UiCommandPayload {
        command_type: command as u16,
        flags: 0,
        track_id: track,
        plugin_index: 0,
        note_pitch: 0,
        value0: 0,
        note_nanotick_lo: 0,
        note_nanotick_hi: 0,
        note_duration_lo: 0,
        note_duration_hi: 0,
        base_version: 0,
    }
}

fn mixer_command(args: &[String]) -> Result<UiCommandPayload, String> {
    // `--track master`, like add-device, remove-device and move-device already accept. It did
    // not, so the one fader every track passes through was unreachable from this surface by
    // name — you had to know that 4294901760 is the master's stable id. `get tracks` prints that
    // row as "master": true and never prints the number in a form anyone would type.
    let track = match flag(args, "--track").as_deref() {
        Some("master") => MASTER_TRACK_ID,
        _ => flag_u64(args, "--track", Some(0))? as u32,
    };
    let gain_db = flag_f64(args, "--gain-db", 0.0)?;
    let pan = flag_f64(args, "--pan", 0.0)?.clamp(-1.0, 1.0);
    // THE COMMAND family (u16, in a payload's flags word), NOT the published one (u8, in the
    // per-track byte). Same bit positions, different widths and different homes — a blanket
    // rename across this file swapped these two and the compiler caught it, which is the argument
    // for both families existing rather than one being reused for the other.
    let mut flags = 0u16;
    if flag_u64(args, "--mute", Some(0))? != 0 {
        flags |= daw_bridge::layout::MIXER_FLAG_MUTE;
    }
    if flag_u64(args, "--solo", Some(0))? != 0 {
        flags |= daw_bridge::layout::MIXER_FLAG_SOLO;
    }
    Ok(UiCommandPayload {
        command_type: UiCommandType::SetTrackMixer as u16,
        flags,
        track_id: track,
        // Signed integers carried in unsigned fields; the engine casts back.
        plugin_index: ((pan * 1000.0).round() as i32) as u32,
        note_pitch: 0,
        value0: ((gain_db * 100.0).round() as i32) as u32,
        note_nanotick_lo: 0,
        note_nanotick_hi: 0,
        note_duration_lo: 0,
        note_duration_hi: 0,
        base_version: 0,
    })
}

/// WHAT THE ENGINE DID WITH THE COMMAND WE JUST SENT.
///
/// A clip edit carries the base version the caller read. If the engine has moved on, the edit is
/// REFUSED and a `ClipRejected` diff comes back carrying `currentBase` — which the payload's own
/// comment calls "the value to retry with". Nothing on this side read it, so a refused edit
/// printed `{"sent": ...}` and exited 0: the edit was lost and the caller was told it worked.
///
/// ONLY DIFFS NEWER THAN OUR SEND ARE CONSIDERED. The ring is PEEKED, not drained — the real UI
/// is its consumer and a tool that drained here would steal diffs from the app it is observing —
/// so refusals from earlier commands stay visible indefinitely. Matching on (track, command,
/// sentBase) alone therefore matches somebody else's refusal, and the first version of this did:
/// it re-sent an edit that had already been applied, and `override` failed with a redo that
/// restored nothing. `before_len` is the ring's length at send time; anything at or past it is
/// ours.
///
/// ACCEPTANCE HAS A POSITIVE SIGNAL, so the common path does not pay the timeout. The engine
/// advances the track's clip version on every applied edit, so a version that has moved is an
/// acknowledgement. Without that, every successful command waited out the full poll window —
/// which is not just slow, it changes the timing of anything driving several edits in sequence.
enum ClipOutcome {
    Applied,
    Refused { reason: u16, current: u32 },
    /// Neither signal arrived in time. Treated as applied, which is what this tool did for its
    /// whole life before the refusal was readable at all — reporting a refusal we did not observe
    /// would be worse than the silence it replaces.
    Unknown,
}

fn await_clip_outcome(
    handle: &EngineHandle,
    track: u32,
    command_type: u16,
    sent_base: u32,
    before_len: usize,
    version_before: u32,
) -> ClipOutcome {
    for _ in 0..120 {
        let diffs = handle.peek_ui_diffs();
        for (diff_type, payload) in diffs.iter().skip(before_len) {
            if *diff_type != daw_bridge::layout::UiDiffType::ClipRejected as u16 {
                continue;
            }
            let u16at = |o: usize| u16::from_le_bytes([payload[o], payload[o + 1]]);
            let u32at = |o: usize| {
                u32::from_le_bytes([payload[o], payload[o + 1], payload[o + 2], payload[o + 3]])
            };
            if u32at(4) == track && u16at(16) == command_type && u32at(8) == sent_base {
                return ClipOutcome::Refused { reason: u16at(2), current: u32at(12) };
            }
        }
        if handle.clip_version_for_track(track) != version_before {
            return ClipOutcome::Applied;
        }
        std::thread::sleep(std::time::Duration::from_millis(5));
    }
    ClipOutcome::Unknown
}

/// The engine's refusal reasons, worded.
///
/// The reason NAMES come from the engine — `errorScopeName` in engine_pure.cpp writes them into
/// the journal as `rejected:<name>` — so this maps a name to a sentence rather than a number to a
/// name, and an unmapped reason falls through to the engine's own word, which is already better
/// than a code. What a sentence adds is what the reader can DO: "order_violation" and "modulation
/// flows forward, so the source must sit before its target" are the same fact and only one of them
/// is actionable.
fn refusal_sentence(reason: &str) -> String {
    match reason {
        "add_failed" => "the device could not be added — a track takes one head-of-chain \
                         instrument, so a second sampler or VST instrument on a track that has \
                         one is refused",
        "remove_failed" => "there is no such device to remove",
        "move_failed" => "the device could not be moved to that position",
        "update_failed" => "the update changed nothing — the device id may not exist on that track",
        "track_missing" => "there is no such track",
        "link_missing" => "there is no such modulation link",
        "link_exists" => "that link already exists",
        "invalid_kind" => "that is not a kind this engine knows",
        "invalid_target" => "the route has no such target",
        "invalid_device" => "one of the devices in the link does not exist on that track",
        "order_violation" => "modulation flows FORWARD along the chain, so a source that sits \
                              after its target is refused — move the source earlier or pick a \
                              later target",
        "version" => "the edit quoted a base version the engine has already moved past",
        // The sampler family, now that its refusals reach the journal. Same rule as the rest:
        // the engine's word mapped to a sentence a reader can act on.
        "no_such_slot" => "there is no slot with that id on this sampler — slot ids start at 1 \
                           and `get sampler-kit` lists the ones that exist",
        "no_such_device" => "there is no device with that id on this track",
        "not_a_sampler" => "that device is not a sampler — check the id, since a chain can hold \
                            effects and patchers beside it",
        "no_such_mod_set" => "there is no mod set with that id",
        "no_such_modulator" => "there is no modulator with that id in the mod set",
        "no_such_source" => "there is no loaded source with that id — load a sample first",
        "no_such_slice" => "there is no slice set with that id — chop the source first",
        "load_failed" => "the sample would not load: the file is missing, or not audio this \
                          build can decode",
        // The patcher graph family.
        "invalid_type" => "that is not a node type this engine knows",
        "invalid_node" => "there is no node with that id in this graph",
        "cycle" => "that connection would make a cycle, and the graph must stay acyclic",
        "add_failed" => "the node could not be added",
        "invalid_connection" => "those two ports cannot be connected",
        "invalid_port" => "there is no such port on that node",
        "invalid_signature" => "that is not a time signature — the denominator must be a power of \
                                two, and 4/5 is refused rather than quietly clamped to 4/4, which \
                                would put the ruler somewhere nobody asked for",
        "zero_delta" => "a ripple of zero bars would change nothing",
        "no_track" => "there is no such track",
        "automation_in_removed_bars" => "the bars being removed carry automation, which would be \
                                         destroyed — clear it first, or remove a range that does \
                                         not cover it",
        other => return format!("the engine called it {other:?}"),
    }
    .to_string()
}

/// The diff type the engine publishes for this family when it ACTED. Used as the fast positive
/// signal only — see `await_outcome` for why the refusal does not come from the ring.
struct DiffFamily {
    ok_type: u16,
}

const CHAIN_FAMILY: DiffFamily =
    DiffFamily { ok_type: daw_bridge::layout::UiDiffType::ChainSnapshot as u16 };
const ROUTING_FAMILY: DiffFamily =
    DiffFamily { ok_type: daw_bridge::layout::UiDiffType::RoutingSnapshot as u16 };
/// GLOBAL-scope commands have no per-track snapshot to wait on, so `ok_type` is None (0), which
/// never matches a real diff: the journal carries both answers for these — "received" when the
/// engine acted, "rejected:<reason>" when it did not.
const GLOBAL_FAMILY: DiffFamily =
    DiffFamily { ok_type: daw_bridge::layout::UiDiffType::None as u16 };

const MOD_FAMILY: DiffFamily =
    DiffFamily { ok_type: daw_bridge::layout::UiDiffType::ModSnapshot as u16 };

fn history_path() -> std::path::PathBuf {
    std::path::Path::new(&daw_bridge::project::engine_project_dir()).join("history.jsonl")
}

/// What the journal says about `scope` after `offset`.
///
/// Substring matching on a line the engine writes without spaces
/// (`{"seq":1,...,"scope":"track:0","op":"chain","outcome":"rejected:add_failed",...}`) rather
/// than a JSON parse: the two keys are unambiguous and this avoids taking a dependency to read one
/// field out of one line.
///
/// A REJECTION ANYWHERE IN THE TAIL BEATS AN ACCEPTANCE, whatever their order, because some
/// handlers journal "received" as the command arrives and the refusal only afterwards
/// (engine_clip_edit.cpp writes exactly that pair). Returning on the first line seen would read
/// the arrival as the answer.
enum JournalSays {
    Refused(String),
    Received,
}

fn journal_since(offset: u64, scope: &str) -> Option<JournalSays> {
    use std::io::{Read, Seek};
    let mut file = std::fs::File::open(history_path()).ok()?;
    file.seek(std::io::SeekFrom::Start(offset)).ok()?;
    let mut tail = String::new();
    file.read_to_string(&mut tail).ok()?;
    let want_scope = format!("\"scope\":\"{scope}\"");
    let mut received = false;
    for line in tail.lines() {
        if !line.contains(&want_scope) {
            continue;
        }
        if let Some(at) = line.find("\"outcome\":\"rejected:") {
            let rest = &line[at + "\"outcome\":\"rejected:".len()..];
            return Some(JournalSays::Refused(
                rest.split('"').next().unwrap_or("").to_string(),
            ));
        }
        if line.contains("\"outcome\":\"received\"") || line.contains("\"outcome\":\"ok\"") {
            received = true;
        }
    }
    received.then_some(JournalSays::Received)
}

/// WHAT THE ENGINE DID WITH THE COMMAND WE JUST SENT.
///
/// `add-device --kind sampler` on a track that already has one is REFUSED — a track takes one
/// head-of-chain instrument — and this tool printed `{"sent": "add-device"}` and exited 0. The
/// refusal existed: the engine logged `chain.rejected reason=add_failed`, journalled it, and put a
/// `ChainError` on the ring for the UI. Every surface learned about it except the one that asked.
/// Found by driving the verb (ui-web/test/cli-verbs.mjs), not by reading it.
///
/// THE REFUSAL COMES FROM THE JOURNAL, NOT THE RING, and that is the whole design. The first
/// version of this read `ChainError` off the ring like `await_clip_outcome` does, and it worked
/// for chain and mod and MISSED the routing refusal entirely — `peek_ui_diffs` returns only the
/// window between the ring's read and write cursors, so a diff the sidecar has already consumed is
/// gone before a bystander can see it. The refusal was real and recorded (routing.rejected,
/// track_missing) and the tool still printed "unknown". A signal that a live UI can eat is not a
/// signal a correctness check may rest on. history.jsonl is an append-only FILE with no consumer to
/// race, it carries the engine's own reason name, and it covers families the ring has no error type
/// for at all.
///
/// THE RING IS STILL READ, for the POSITIVE answer only. A snapshot for the track means the engine
/// rebuilt it, and without that every successful command would sit out the full refusal window —
/// which is not merely slow, it changes the timing of anything driving several edits in sequence.
/// Losing that one to the same race costs latency and nothing else, which is the difference.
///
/// CORRELATION IS BY SCOPE, i.e. by track, for one operator driving one CLI. It is NOT exact
/// against something else editing the same track at the same moment; sharpening it needs a command
/// id on the wire, and the 40-byte payload has no room for one.
enum ChainOutcome {
    Applied,
    Refused { reason: String },
    /// Neither answer arrived in time. Reported as unknown and exits 0 — announcing a refusal we
    /// did not observe would be worse than the silence it replaces. Also where a build with
    /// DAW_NO_HISTORY lands when the ring race goes against it.
    Unknown,
}

/// The journal's word for a command's scope. NOT `format!("track:{id}")` — the master track is
/// written as "master" and a global command as "global" (engine_history_journal.cpp), so a matcher
/// that only knew the track form would silently miss every refusal on the master track and every
/// global one. `kUiGlobalScope` is the id the engine itself passes for global.
const UI_GLOBAL_SCOPE: u32 = 0xFFFF_FFFF;

fn journal_scope(track: u32) -> String {
    match track {
        UI_GLOBAL_SCOPE => "global".to_string(),
        daw_bridge::layout::MASTER_TRACK_ID => "master".to_string(),
        id => format!("track:{id}"),
    }
}

/// A REFUSAL-ONLY WAIT, for families that publish no acknowledgement.
///
/// chain, routing and mod each have a snapshot the engine republishes on success, so the waiter
/// above can return the instant it sees one. The SAMPLER has nothing of the sort: its refusals are
/// journalled (as of the engine change that added reportSamplerReject's history line) but its
/// SUCCESSES are not, and there is no single choke point to add one to. So there is no positive
/// signal to wait for, and the 750ms waiter would make every successful sampler command sit out
/// the whole window before printing "unknown".
///
/// This waits a SHORT time for a refusal and treats silence as success. The tradeoff, stated
/// plainly because it is a real one: a refusal that arrives later than the window is missed, and
/// the command reports success. That is strictly better than what these verbs did before — which
/// was to exit 0 unconditionally, i.e. to claim success for every refusal, always — but it is not
/// the same as knowing. 250ms is chosen against an engine that reads its command ring every audio
/// block (~11.6ms at 512/44.1k), so the refusal is normally written within a few milliseconds;
/// the margin is for a loaded machine, and `sampler-load --files` writes a whole kit one command
/// at a time, so a longer window would be felt.
fn await_refusal_only(track: u32, journal_at: u64, op: &str) -> ChainOutcome {
    let scope = journal_scope(track);
    let want_op = format!("\"op\":\"{op}\"");
    for _ in 0..50 {
        if let Some(reason) = journal_refusal_for(journal_at, &scope, &want_op) {
            return ChainOutcome::Refused { reason };
        }
        std::thread::sleep(std::time::Duration::from_millis(5));
    }
    ChainOutcome::Applied
}

/// The first refusal of THIS op in THIS scope after `offset`.
///
/// THE OP FILTER IS NOT OPTIONAL, and a test found out why. A `sampler-load` of a file that does
/// not exist leaves a dangling source that the engine RE-RESOLVES on every project load, so the
/// journal collects a fresh `sampler_load rejected:load_failed` long afterwards, interleaved with
/// whatever else is running. Without this filter, nine later verbs on the same track each adopted
/// that unrelated refusal and reported "the sample would not load" about a slot rename.
///
/// The residual hole, stated rather than hidden: two commands of the SAME op on the same track
/// within one window are still indistinguishable, because the journal line carries no device or
/// file to tell them apart. `sampler-load` is the only verb the retry behaviour makes likely, and
/// a wrong attribution there still names a real refusal of a real load.
fn journal_refusal_for(offset: u64, scope: &str, want_op: &str) -> Option<String> {
    use std::io::{Read, Seek};
    let mut file = std::fs::File::open(history_path()).ok()?;
    file.seek(std::io::SeekFrom::Start(offset)).ok()?;
    let mut tail = String::new();
    file.read_to_string(&mut tail).ok()?;
    let want_scope = format!("\"scope\":\"{scope}\"");
    for line in tail.lines() {
        if !line.contains(&want_scope) || !line.contains(want_op) {
            continue;
        }
        if let Some(at) = line.find("\"outcome\":\"rejected:") {
            let rest = &line[at + "\"outcome\":\"rejected:".len()..];
            return Some(rest.split('"').next().unwrap_or("").to_string());
        }
    }
    None
}

/// The journal mark to take BEFORE sending, and the report to make after.
///
/// Two functions rather than one that owns the send, because every sampler verb has its own
/// typed send method — send_sampler_set_slot, send_sampler_device, send_named and so on — and a
/// helper that swallowed the send would need a closure per arm to say nothing extra.
fn sampler_journal_mark() -> u64 {
    std::fs::metadata(history_path()).map(|m| m.len()).unwrap_or(0)
}

fn report_sampler_outcome(verb: &str, op: &str, track: u32, journal_at: u64, extra: &str) -> i32 {
    match await_refusal_only(track, journal_at, op) {
        ChainOutcome::Refused { reason } => {
            eprintln!("daw-cli: the engine refused {verb} on track {track}: {}",
                      refusal_sentence(&reason));
            1
        }
        _ => {
            println!("{{ \"applied\": {verb:?}, \"track\": {track}{extra} }}");
            0
        }
    }
}

fn await_outcome(
    handle: &EngineHandle,
    family: &DiffFamily,
    track: u32,
    before_len: usize,
    journal_at: u64,
) -> ChainOutcome {
    let scope = journal_scope(track);
    for _ in 0..150 {
        match journal_since(journal_at, &scope) {
            Some(JournalSays::Refused(reason)) => return ChainOutcome::Refused { reason },
            // An acceptance is only believed after a grace period, for the reason journal_since
            // gives: the pair is written back to back under one lock, so 20ms is generous, and
            // believing the arrival immediately would report a refused edit as applied.
            Some(JournalSays::Received) => {
                std::thread::sleep(std::time::Duration::from_millis(20));
                return match journal_since(journal_at, &scope) {
                    Some(JournalSays::Refused(reason)) => ChainOutcome::Refused { reason },
                    _ => ChainOutcome::Applied,
                };
            }
            None => {}
        }
        // A family with no snapshot type has no ring signal to wait on — the journal is the whole
        // answer, and `ok_type` of None (0) never matches a real diff.
        for (diff_type, payload) in handle.peek_ui_diffs().iter().skip(before_len) {
            let track_id =
                u32::from_le_bytes([payload[4], payload[5], payload[6], payload[7]]);
            // trackId sits at offset 4 in every one of these payloads (event_payloads.h). Dispatch
            // on the TYPE and never on the size: a device_chain_ui_tests comment records a version
            // that keyed on size, and since UiChainDiffPayload and UiChainErrorPayload are both 40
            // bytes it routed every error into the snapshot path.
            if track_id == track && *diff_type == family.ok_type {
                return ChainOutcome::Applied;
            }
        }
        std::thread::sleep(std::time::Duration::from_millis(5));
    }
    ChainOutcome::Unknown
}

/// Reports what the engine did with a command already sent. Returns the exit code.
///
/// ONE function rather than the same block pasted into add-device, remove-device, move-device,
/// set-bypass, routing, mod-link and mod-depth: seven copies of a rule is how this project got a
/// chord tee that disagreed with itself and a mixer with two writers. `extra` is a pre-formatted
/// JSON fragment because the verbs each have their own interesting fields, and that is the only
/// part that genuinely differs.
///
/// The caller takes `before_len` and `journal_at` BEFORE sending: both are "everything past here is
/// mine" marks, and a waiter that started from zero would report somebody else's refusal.
fn report_outcome_from(
    handle: &EngineHandle,
    family: &DiffFamily,
    verb: &str,
    track: u32,
    extra: &str,
    before_len: usize,
    journal_at: u64,
) -> i32 {
    // NEVER print the global sentinel as if it were a track. 4294967295 in a "track" field is a
    // number a reader will try to use, and the master track's 4294901760 is no better — both get
    // the journal's own word for the scope instead.
    let (where_json, where_prose) = match track {
        UI_GLOBAL_SCOPE => ("\"scope\": \"global\"".to_string(), "for the song".to_string()),
        daw_bridge::layout::MASTER_TRACK_ID => {
            ("\"track\": \"master\"".to_string(), "on the master track".to_string())
        }
        id => (format!("\"track\": {id}"), format!("on track {id}")),
    };
    match await_outcome(handle, family, track, before_len, journal_at) {
        ChainOutcome::Refused { reason } => {
            eprintln!(
                "daw-cli: the engine refused {verb} {where_prose}: {}",
                refusal_sentence(&reason)
            );
            1
        }
        ChainOutcome::Applied => {
            println!("{{ \"applied\": {verb:?}, {where_json}{extra} }}");
            0
        }
        ChainOutcome::Unknown => {
            println!("{{ \"sent\": {verb:?}, {where_json}{extra}, \"applied\": \"unknown\" }}");
            0
        }
    }
}

/// The marks a caller must take before sending: the ring's length and the journal's length.
fn outcome_marks(handle: &EngineHandle) -> (usize, u64) {
    (
        handle.peek_ui_diffs().len(),
        std::fs::metadata(history_path()).map(|m| m.len()).unwrap_or(0),
    )
}

fn send_chain_reporting(
    handle: &EngineHandle,
    payload: UiChainCommandPayload,
    verb: &str,
    track: u32,
    extra: &str,
) -> i32 {
    let (before_len, journal_at) = outcome_marks(handle);
    match handle.send_chain_command(payload) {
        Ok(()) => report_outcome_from(
            handle, &CHAIN_FAMILY, verb, track, extra, before_len, journal_at),
        Err(err) => {
            eprintln!("daw-cli: {err}");
            1
        }
    }
}

fn chord_command(
    args: &[String],
    base_version: u32,
) -> Result<UiChordCommandPayload, String> {
    let track = flag_u64(args, "--track", Some(0))? as u32;
    let nanotick = flag_u64(args, "--nanotick", None)?;
    // --delete turns the same addressing into a removal, so a script never has to build
    // a second command shape to undo what it just wrote.
    let deleting = args.iter().any(|a| a == "--delete");
    let degree = if deleting {
        flag_u64(args, "--degree", Some(0))?
    } else {
        flag_u64(args, "--degree", None)?
    };
    let duration = flag_u64(args, "--duration", Some(0))?;
    Ok(UiChordCommandPayload {
        command_type: if deleting {
            UiCommandType::DeleteChord as u16
        } else {
            UiCommandType::WriteChord as u16
        },
        flags: daw_bridge::layout::edit_column(flag_u64(args, "--column", Some(0))?)
            .map_err(|e| format!("daw-cli: {e}"))?,
        track_id: track,
        base_version,
        nanotick_lo: (nanotick & 0xffff_ffff) as u32,
        nanotick_hi: (nanotick >> 32) as u32,
        duration_lo: (duration & 0xffff_ffff) as u32,
        duration_hi: (duration >> 32) as u32,
        degree: degree as u16,
        quality: flag_u64(args, "--quality", Some(1))? as u8,
        inversion: flag_u64(args, "--inversion", Some(0))? as u8,
        base_octave: flag_u64(args, "--octave", Some(4))? as u8,
        // THE STRUM AND THE HUMANIZE PAIR, which were hardcoded to zero here. All three persist
        // in the project file, all three are read by the scheduler, and all three change what
        // SOUNDS — spread staggers the chord's notes, the humanize pair jitters each strike's
        // timing and velocity. The engine's applyAddChord has taken them as parameters since it
        // was written; this surface, the only one that sends the command, filled them with zeros.
        // So no project could contain a strummed or humanized chord unless it was hand-written.
        //
        // 0..255 each, because both humanize fields are a BYTE on the wire (they are widened to
        // uint16_t inside the engine, which is why a larger number looks like it would fit).
        // Refused rather than truncated: 300 silently becoming 44 is a different feel than the
        // one asked for, with nothing reporting it.
        humanize_timing: match flag_u64(args, "--humanize-timing", Some(0))? {
            v if v <= 255 => v as u8,
            v => return Err(format!(
                "--humanize-timing is 0..255 (the wire field is a byte), got {v}")),
        },
        humanize_velocity: match flag_u64(args, "--humanize-velocity", Some(0))? {
            v if v <= 255 => v as u8,
            v => return Err(format!(
                "--humanize-velocity is 0..255 (the wire field is a byte), got {v}")),
        },
        reserved: 0,
        // SPREAD IS ONLY A SPREAD ON A WRITE. On a DELETE the engine reads this same field as the
        // chord id (see applyRemoveChordAt), so sending a strum width with --delete would address
        // a chord nobody meant. Zero on a delete keeps the existing "delete at this tick and
        // column" behaviour, which is what the flag has always meant there.
        spread_nanoticks: if deleting {
            0
        } else {
            flag_u64(args, "--spread", Some(0))? as u32
        },
    })
}

fn harmony_command(args: &[String], base_version: u32) -> Result<UiCommandPayload, String> {
    let nanotick = flag_u64(args, "--nanotick", Some(0))?;
    let deleting = args.iter().any(|a| a == "--delete");
    let root = if deleting { flag_u64(args, "--root", Some(0))? } else { flag_u64(args, "--root", None)? };
    let scale = if deleting { flag_u64(args, "--scale", Some(0))? } else { flag_u64(args, "--scale", None)? };
    Ok(UiCommandPayload {
        command_type: if deleting {
            UiCommandType::DeleteHarmony as u16
        } else {
            UiCommandType::WriteHarmony as u16
        },
        flags: 0,
        track_id: 0,
        plugin_index: 0,
        // The engine reads the root from note_pitch and the scale from value0.
        note_pitch: (root % 12) as u32,
        value0: scale as u32,
        note_nanotick_lo: (nanotick & 0xffff_ffff) as u32,
        note_nanotick_hi: (nanotick >> 32) as u32,
        note_duration_lo: 0,
        note_duration_hi: 0,
        base_version,
    })
}

const NANOTICKS_PER_QUARTER: u64 = 960_000;
const NOTE_NAMES: [&str; 12] = [
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B",
];

fn pitch_name(pitch: u8) -> String {
    format!("{}{}", NOTE_NAMES[(pitch % 12) as usize], pitch as i32 / 12 - 1)
}

/// Asks the engine for a clip window and waits for the matching snapshot.
// get device-params <track> <device> — request one device's params + read them
// back. Sends RequestDeviceParams and polls the region (the plugin loads async).
fn get_device_params(handle: &EngineHandle, args: &[&str]) -> i32 {
    let track: u32 = args.get(1).and_then(|s| s.parse().ok()).unwrap_or(0);
    let device: u32 = args.get(2).and_then(|s| s.parse().ok()).unwrap_or(0);
    let req = || {
        let p = UiCommandPayload {
            command_type: UiCommandType::RequestDeviceParams as u16,
            flags: 0,
            track_id: track,
            plugin_index: 0,
            note_pitch: 0,
            value0: device,
            note_nanotick_lo: 0,
            note_nanotick_hi: 0,
            note_duration_lo: 0,
            note_duration_hi: 0,
            base_version: 0,
        };
        let _ = handle.send_command(p);
    };
    let deadline = Instant::now() + Duration::from_secs(10);
    loop {
        req();
        thread::sleep(Duration::from_millis(250));
        let v = handle.read_device_params();
        if (v.version > 0 && !v.params.is_empty()) || Instant::now() >= deadline {
            // EVERY parameter, with what it IS — not just the first one's name. A rack that
            // can only report "param 0 is called Gain and reads 0.62" is the state this
            // replaces: no unit, no range, no default, no way to know a 5-way switch from a
            // continuous knob, so setting a value in real units was a binary search against the
            // display text.
            println!(
                "{{ \"track\": {}, \"device\": {}, \"name\": {:?}, \"version\": {}, \"count\": {},",
                v.track_id, v.device_id, v.device_name, v.version, v.params.len()
            );
            println!("  \"params\": [");
            for (i, p) in v.params.iter().enumerate() {
                let hex: String = p.uid16.iter().map(|b| format!("{b:02x}")).collect();
                let comma = if i + 1 == v.params.len() { "" } else { "," };
                println!(
                    "    {{ \"index\": {}, \"name\": {:?}, \"uid16\": {:?}, \"value\": {:.3}, \"display\": {:?}, \"unit\": {:?}, \"range\": [{:?}, {:?}], \"default\": {:.3}, \"steps\": {}, \"discrete\": {}, \"automatable\": {} }}{comma}",
                    p.index, p.name, hex, p.value, p.display, p.unit, p.min_text, p.max_text,
                    p.default_value, p.step_count, p.discrete, p.automatable
                );
            }
            println!("  ]");
            println!("}}");
            return 0;
        }
    }
}

// get extents — dump the published clip rails, decoding each clip's packed grid.
/// WHICH CLIPS ARE SHARED, and how many appearances each has.
///
/// `get extents` already lists every placement; this answers the different question the shared
/// -clip design makes possible and surprising — "if I edit here, what else changes?". Two
/// placements of one clip are the same notes seen twice, so an edit to either reaches both. The
/// engine counts that reach on every edit (`placements_touched`), and until now the only way to
/// know it in ADVANCE was to group extents by clip id yourself.
///
/// COUNTED OVER ALL EXTENTS, not the filtered set. A clip is shared whether or not its other
/// appearances are on the track being asked about, and a count that changed with the filter would
/// be a different number for the same question. That is the agent's `shared_clips` rule and this
/// is deliberately the same one — two surfaces answering one question have to agree, and the way
/// they drift is each deciding what "shared" means.
fn get_shared(handle: &EngineHandle, track: Option<u32>) -> i32 {
    let (extents, truncated) = handle.read_clip_extents_with_truncation();
    if truncated > 0 {
        eprintln!(
            "daw-cli: WARNING {truncated} clip extent(s) did not fit — the counts below are \
             LOW, because an appearance that did not fit is an appearance not counted"
        );
    }
    let uses = clip_appearances(&extents);

    let shown: Vec<_> = extents.iter()
        .filter(|e| track.is_none() || track == Some(e.track_id))
        .collect();
    println!("[");
    for (i, e) in shown.iter().enumerate() {
        let comma = if i + 1 == shown.len() { "" } else { "," };
        let name = json_name(&e.name);
        println!(
            "  {{ \"placement\": {}, \"clip\": {}, \"track\": {}, \"name\": {}, \
             \"start_tick\": {}, \"end_tick\": {}, \"appearances\": {}, \"forked\": {} }}{comma}",
            e.placement_id, e.clip_id, e.track_id, name, e.start_tick, e.end_tick,
            uses.get(&e.clip_id).copied().unwrap_or(1),
            (e.flags & UI_CLIP_EXTENT_HAS_ALTERNATE) != 0);
    }
    println!("]");
    0
}

fn get_extents(handle: &EngineHandle) -> i32 {
    let (extents, truncated) = handle.read_clip_extents_with_truncation();
    // Truncation FIRST, and as a comment line before the array, so it cannot be missed by
    // something that only reads the entries. A truncated list nobody notices reads as a
    // complete one.
    if truncated > 0 {
        eprintln!(
            "daw-cli: WARNING {truncated} clip extent(s) did not fit and are NOT in this list — \
             the rails are incomplete"
        );
    }
    println!("[");
    for (i, e) in extents.iter().enumerate() {
        // NO TRAILING COMMA. This printed one after every entry including the last, so the output
        // announced itself as JSON and would not parse — every check that consumed it grepped
        // instead, so nothing noticed until one tried json.load. A read-back that cannot be
        // parsed is a read-back you have to write a parser for, which is how a caller ends up
        // scanning for a key and matching a value.
        let comma = if i + 1 == extents.len() { "" } else { "," };
        let grid = daw_bridge::layout::unpack_clip_grid(e.flags)
            .map(|(lpb, n, d)| format!("{{ \"lpb\": {lpb}, \"time_sig\": \"{n}/{d}\" }}"))
            .unwrap_or_else(|| "null".to_string());
        let audio = e.flags & daw_bridge::layout::UI_CLIP_EXTENT_AUDIO != 0;
        // The appearance's own edit scope, printed for the same reason `absent` and
        // `harmony_quantize` are: a toggle whose state cannot be read is one the interface has to
        // guess at.
        let local = e.flags & daw_bridge::layout::UI_CLIP_EXTENT_LOCAL_EDITS != 0;
        // The OVERRIDE BADGE, published since M3.24 and readable from nowhere until now. It is
        // what a UI draws to say "this appearance is customised", so a stale one — a mute whose
        // base note a later clip edit removed — lit it over nothing and no test could see that.
        let (overrides, has_overrides) = daw_bridge::layout::unpack_clip_overrides(e.flags);
        // M2.57: is there another version of this appearance to swap to? An alternate nobody can
        // see is the same as not having one.
        let alt = e.flags & daw_bridge::layout::UI_CLIP_EXTENT_HAS_ALTERNATE != 0;
        // THE NAME, published in UiClipExtent since the region existed and printed by nothing.
        // A field that is written, mirrored in Rust and readable from no client is invisible in
        // exactly the way a field with no writer is — which is what `name` also was until
        // SetClipText (98). Now that a command can change it, a caller has to be able to see it.
        let name = json_name(&e.name);
        println!(
            "  {{ \"placement\": {}, \"clip\": {}, \"track\": {}, \"name\": {name}, \"audio\": {}, \"local_edits\": {local}, \"overrides\": {overrides}, \"has_overrides\": {has_overrides}, \"has_alternate\": {alt}, \"start\": {}, \"end\": {}, \"grid\": {} }}{comma}",
            e.placement_id, e.clip_id, e.track_id, audio, e.start_tick, e.end_tick, grid
        );
    }
    println!("]");
    0
}

// get audio-sources — dump the published UiAudioSourceRegion (sources + clips).
fn get_audio_sources(handle: &EngineHandle) -> i32 {
    let v = handle.read_audio_sources();
    println!(
        "{{ \"version\": {}, \"audioMapBpmMilli\": {}, \"formatVersion\": {}, \"sourceCount\": {}, \"clipCount\": {},",
        v.version, v.audio_map_bpm_milli, v.format_version, v.sources.len(), v.clips.len()
    );
    println!("  \"sources\": [");
    for s in &v.sources {
        println!(
            "    {{ \"sourceId\": {}, \"status\": {}, \"channels\": {}, \"waveChannels\": {}, \"frames\": {}, \"rateHz\": {}, \"absPeak\": {:.6}, \"levelMask\": {}, \"contentKey\": {}, \"flags\": {}, \"path\": {:?} }},",
            s.source_id, s.status, s.source_channels, s.wave_channels, s.source_frames,
            s.source_rate_hz, s.abs_peak, s.level_mask, s.content_key, s.flags, s.path
        );
    }
    println!("  ],\n  \"clips\": [");
    for c in &v.clips {
        println!(
            "    {{ \"clipId\": {}, \"sourceId\": {}, \"sourceStartFrame\": {}, \"clipLengthTicks\": {}, \"fadeIn\": {}, \"fadeOut\": {}, \"gainDb\": {} }},",
            c.clip_id, c.source_id, c.source_start_frame, c.clip_length_ticks,
            c.fade_in_ticks, c.fade_out_ticks, c.gain_db
        );
    }
    println!("  ]\n}}");
    0
}

// get waveform <sourceId> <decimation> <firstFrame> <columns> [channelMask]
//                    [--track N [--device D]]  <- sourceId is then a SAMPLER LOCAL id
// Sends RequestWaveform and reads the seqlocked answer slot back.
/// v32: asks the engine to publish one sampler device's kit and prints the answer.
///
/// The request sequence is CLIENT-OWNED and it picks the slot the answer lands in, so this reads
/// ONE place rather than scanning — which is the whole reason that pattern exists.
fn get_sampler_kit(handle: &EngineHandle, args: &[String]) -> i32 {
    let track = flag_u64(args, "--track", Some(0)).unwrap_or(0) as u32;
    let device = flag_u64(args, "--device", Some(0)).unwrap_or(0) as u32;
    // UNIQUE PER INVOCATION, from the pid. This defaulted to the constant 1, and the comment
    // right here said what that costs — "a caller polling repeatedly should vary it so a stale
    // answer from a previous question is distinguishable from this one's" — and then did not.
    //
    // With every request carrying seq 1, the echo check below (`v.request_seq == seq`) matched
    // the slot's EXISTING contents immediately, so each invocation printed the PREVIOUS one's
    // answer. Asking for track 1 returned a kit stamped "track": 0. A UI reading two tracks'
    // kits in turn would show each one the other's, and with a single sampler track — which is
    // every test in this repo until today — nothing looks wrong at all.
    //
    // The pid also picks the answer slot (index = seq % slots), which spreads concurrent readers
    // across slots instead of piling them on slot 1. Forced non-zero: zero means "no answer here".
    let seq = flag_u64(args, "--seq", None)
        .unwrap_or_else(|_| u64::from(std::process::id()) | 1) as u32;
    let payload = UiSamplerKitRequestPayload {
        command_type: UiCommandType::RequestSamplerKit as u16,
        flags: 0,
        track_id: track,
        device_id: device,
        request_seq: seq,
        reserved: [0; 24],
    };
    if let Err(err) = handle.send_sampler_kit_request(payload) {
        eprintln!("daw-cli: {err}");
        return 1;
    }
    let index = (seq as usize) % UI_SAMPLER_KIT_SLOTS;
    for _ in 0..200 {
        if let Some(v) = handle.read_sampler_kit_slot(index) {
            // The echo is the point: a slot reused for a DIFFERENT question looks exactly like an
            // answer to this one without it.
            if v.request_seq == seq {
                if !v.found {
                    println!("{{ \"found\": false, \"track\": {track}, \"device\": {device} }}");
                    return 0;
                }
                println!("{{");
                println!("  \"found\": true,");
                // The poll counter, so a caller can cache this answer and re-ask only when it
                // moves rather than re-requesting a 2 KB kit on a timer.
                println!("  \"kit_version\": {},", handle.sampler_kit_version());
                // WHAT THIS ANSWER ACTUALLY SHOWS, which is not always kit_version. The poll
                // counter is written every publish cycle from the model; this is stamped into
                // the answer when it is built. When they differ, the kit moved after this was
                // made and re-asking will get something newer.
                println!("  \"content_version\": {},", v.content_version);
                println!("  \"track\": {},", v.track_id);
                println!("  \"device\": {},", v.device_id);
                println!("  \"voice_cap\": {},", v.voice_cap);
                println!("  \"default_gate\": {},", v.default_gate);
                println!("  \"default_view\": {},", v.default_view);
                println!("  \"active_voices\": {},", v.active_voices);
                println!("  \"steals\": {},", v.steals);
                println!("  \"unmapped\": {},", v.unmapped);
                println!("  \"slots_truncated\": {},", v.slots_truncated);
                let body: Vec<String> = v.slots.iter().map(|e| {
                    // v36. Stops at the first NUL rather than trusting the whole array: the
                    // engine nul-terminates inside the field, and a name that filled it would
                    // otherwise drag the padding into the JSON.
                    let raw: Vec<u8> = e.name.iter().map(|&c| c as u8)
                        .take_while(|&c| c != 0).collect();
                    let name = String::from_utf8_lossy(&raw)
                        .replace('\\', "\\\\").replace('"', "\\\"");
                    format!(
                        "    {{ \"slot\": {}, \"name\": \"{}\", \"source\": {}, \"key_low\": {}, \"key_high\": {}, \"root\": {}, \"vel_low\": {}, \"vel_high\": {}, \"voice_group\": {}, \"nna\": {}, \"gate\": {}, \"reverse\": {}, \"source_missing\": {}, \"slice_missing\": {}, \"gain_mb\": {}, \"pan\": {}, \"mod_set\": {}, \"stem\": {}, \"quality\": {}, \"slice\": {}, \"length_frames\": {}, \"slice_begin\": {}, \"slice_end\": {}, \"mod_mask\": {}, \"filter_type\": {}, \"vintage_bits\": {}, \"vintage_rate_hz\": {} }}",
                        e.slotId, name, e.sourceLocalId, e.keyLow, e.keyHigh, e.rootKey,
                        e.velLow, e.velHigh, e.voiceGroup, e.nna,
                        (e.flags & 1) != 0, (e.flags & 2) != 0,
                        (e.flags & UI_SAMPLER_SLOT_SOURCE_MISSING) != 0,
                        (e.flags & UI_SAMPLER_SLOT_SLICE_MISSING) != 0,
                        e.gainMillibels, e.panThousandths, e.modSetId, e.outputStem,
                        e.quality, e.sliceId, e.lengthFrames, e.sliceBeginFrame,
                        e.sliceEndFrame, e.modMask, e.filterType,
                        e.vintageBits, e.vintageRateHz)
                }).collect();
                println!("  \"slots\": [\n{}\n  ]", body.join(",\n"));
                println!("}}");
                return 0;
            }
        }
        std::thread::sleep(std::time::Duration::from_millis(10));
    }
    eprintln!("daw-cli: no answer for sampler-kit seq {seq} within 2s");
    1
}

// TAKES BOTH LISTS, and it has to. `positional` is built by FILTERING OUT every "--" argument,
// so a flag's NAME is gone from it while its VALUE is still there as a stray positional — read
// flags from `positional` and --track is invisible, which is exactly how the sampler form
// silently sent flags=0 and got badrequest back. `all` is the unfiltered argv, the same list
// get_automation_points reads for the same reason.
fn get_waveform(handle: &EngineHandle, args: &[&str], all: &[String]) -> i32 {
    let source_id: u32 = args.get(1).and_then(|s| s.parse().ok()).unwrap_or(1);
    let decimation: u32 = args.get(2).and_then(|s| s.parse().ok()).unwrap_or(64);
    let first_frame: u64 = args.get(3).and_then(|s| s.parse().ok()).unwrap_or(0);
    let columns: u32 = args.get(4).and_then(|s| s.parse().ok()).unwrap_or(16);
    let channel_mask: u32 = args.get(5).and_then(|s| s.parse().ok()).unwrap_or(1);
    // ADDRESSING A SAMPLER'S SOURCE. Without --track the first argument is a waveform-store id,
    // exactly as it always was; with it, the first argument is the sampler source's LOCAL id (the
    // "source" the kit read-back gives you) and the engine translates the triple on its side.
    //
    // Two id spaces was the whole bug: the store interns by resolved PATH and a sampler's local
    // id is a per-device counter, so a sample view's request addressed nothing and answered
    // nothing while its model looked perfect.
    let track = flag_u64(all, "--track", None).ok();
    let device = flag_u64(all, "--device", Some(0)).unwrap_or(0) as u32;
    let (flags, reserved0, reserved1) = match track {
        Some(t) => (
            daw_bridge::layout::WAVEFORM_REQUEST_SAMPLER_SOURCE,
            t as u32,
            device,
        ),
        None => (0u16, 0u32, 0u32),
    };
    // Was a CONSTANT 0x7A5E, which is the trap documented in get_clip: the echo then matches on
    // every call after the first, so a query for source 2 can take delivery of source 1's answer.
    // Unique per invocation, and the echoed sourceId is checked too.
    let request_seq: u32 = {
        let pid = std::process::id();
        let nanos = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.subsec_nanos())
            .unwrap_or(0);
        let id = pid.rotate_left(11) ^ nanos;
        if id == 0 { 1 } else { id }
    };
    let slot_index = (request_seq as usize) % 4; // K_UI_WAVEFORM_SLOTS
    let payload = UiWaveformRequestPayload {
        command_type: UiCommandType::RequestWaveform as u16,
        flags,
        request_seq,
        source_id,
        decimation,
        first_frame_lo: (first_frame & 0xffff_ffff) as u32,
        first_frame_hi: (first_frame >> 32) as u32,
        columns,
        channel_mask,
        reserved0,
        reserved1,
    };
    let deadline = Instant::now() + Duration::from_secs(5);
    loop {
        let _ = handle.send_waveform_request(payload);
        thread::sleep(Duration::from_millis(100));
        if let Some(v) = handle.read_waveform_slot(slot_index) {
            if v.request_seq == request_seq && v.source_id == source_id {
                let pairs: Vec<String> = v.pairs.iter().map(|p| p.to_string()).collect();
                println!(
                    "{{ \"requestSeq\": {}, \"sourceId\": {}, \"status\": {}, \"decimation\": {}, \"columns\": {}, \"channels\": {}, \"firstFrame\": {}, \"frameCount\": {}, \"contentKey\": {}, \"flags\": {}, \"samplerAddr\": {}, \"pairs\": [{}] }}",
                    v.request_seq, v.source_id, v.status, v.decimation, v.columns,
                    v.channels, v.first_frame, v.frame_count, v.content_key, v.flags,
                    v.sampler_addr, pairs.join(",")
                );
                return 0;
            }
        }
        if Instant::now() >= deadline {
            eprintln!("daw-cli: no waveform answer for source {source_id} (slot {slot_index})");
            return 1;
        }
    }
}

/// v28: the standing automation lane list — "which params are automated, and on what track".
/// A plain version-gated read: no request, no slot, nothing to match up.
fn get_automation(handle: &EngineHandle) -> i32 {
    let v = handle.read_automation_lanes();
    println!("{{");
    println!("  \"version\": {},", v.version);
    // Truncation is REPORTED. An incomplete list that says nothing reads as a complete one, and
    // "the automation lanes are missing" then arrives as a bug report about the lanes.
    println!("  \"lanes_truncated\": {},", v.truncated);
    println!("  \"lanes\": [");
    for (i, lane) in v.lanes.iter().enumerate() {
        let comma = if i + 1 == v.lanes.len() { "" } else { "," };
        println!(
            "    {{ \"track_id\": {}, \"param\": {:?}, \"device\": {}, \"points\": {}, \"discrete\": {} }}{comma}",
            lane.track_id, lane.param_id, lane.target_plugin_index, lane.point_count,
            lane.discrete
        );
    }
    println!("  ]");
    println!("}}");
    0
}

/// v28: one lane's POINTS. Sends RequestAutomationLane and reads the slot it addressed.
// get sampler-envelope — one modulator's shape, request/answer through a seqlock slot.
fn get_sampler_envelope(handle: &EngineHandle, args: &[String]) -> i32 {
    use daw_bridge::layout as L;
    let track = flag_u64(args, "--track", Some(0)).unwrap_or(0) as u32;
    let device = flag_u64(args, "--device", Some(0)).unwrap_or(0) as u32;
    let mod_set = flag_u64(args, "--mod-set", Some(0)).unwrap_or(0) as u32;
    // ADDRESSED BY MODULATOR ID, OR BY TARGET. Same choice the WRITE offers, and the same flag
    // bit carries it — asking a different way than writing is how a read-back ends up answering
    // about a different object.
    let modulator = flag_u64(args, "--modulator", Some(0)).unwrap_or(0) as u16;
    let (flags, target) = match flag(args, "--target").as_deref() {
        None => (0u16, 0u8),
        Some(t) => {
            let id = match t {
                "amp" | "volume" | "vol" => 0u8,
                "pan" | "panning" => 1u8,
                "pitch" => 2u8,
                "cutoff" | "filter" => 3u8,
                "res" | "resonance" => 4u8,
                other => {
                    eprintln!("daw-cli: --target expects amp|pan|pitch|cutoff|res, got {other:?}");
                    return 2;
                }
            };
            (L::SAMPLER_ENV_BY_TARGET, id)
        }
    };
    // UNIQUE PER INVOCATION, for the reason get_clip and get_automation_points both spell out: a
    // constant request id makes every call after the first match the PREVIOUS call's answer the
    // instant it is read, so asking about modulator B returns modulator A.
    let request_seq = {
        let pid = std::process::id();
        let nanos = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.subsec_nanos())
            .unwrap_or(0);
        let id = pid.rotate_left(11) ^ nanos;
        if id == 0 { 1 } else { id }
    };
    let slot_index = (request_seq as usize) % L::K_UI_SAMPLER_ENVELOPE_SLOTS;
    let payload = L::UiSamplerEnvelopeRequestPayload {
        command_type: UiCommandType::RequestSamplerEnvelope as u16,
        flags,
        track_id: track,
        device_id: device,
        mod_set_id: mod_set,
        request_seq,
        modulator_id: modulator,
        reserved0: 0,
        target,
        reserved1: [0; 4],
    };
    let deadline = Instant::now() + Duration::from_secs(5);
    loop {
        let _ = handle.send_sampler_envelope_request(payload);
        thread::sleep(Duration::from_millis(80));
        if let Some(a) = handle.read_sampler_envelope_slot(slot_index) {
            if a.request_seq == request_seq {
                let pts: Vec<String> = a
                    .points
                    .iter()
                    .map(|(t, v, tension, f)| format!(
                        "{{ \"time\": {t}, \"value_milli\": {v}, \"tension\": {tension}, \"flags\": {f} }}"))
                    .collect();
                println!(
                    "{{ \"request_seq\": {}, \"track_id\": {}, \"device_id\": {}, \"mod_set_id\": {}, \"modulator_id\": {}, \"target\": {}, \"found\": {}, \"time_base\": {}, \"rate_milli\": {}, \"sustain_loop\": [{}, {}], \"release_loop\": [{}, {}], \"release_fade\": {}, \"points_truncated\": {}, \"points\": [{}] }}",
                    a.request_seq, a.track_id, a.device_id, a.mod_set_id, a.modulator_id,
                    a.target, a.found, a.time_base, a.rate_milli,
                    a.sustain_loop.0, a.sustain_loop.1, a.release_loop.0, a.release_loop.1,
                    a.release_fade, a.points_truncated, pts.join(", ")
                );
                // `found: false` is an ANSWER and exit 0 says so — "there is no envelope on this
                // target" is a thing a UI draws. Only a request never answered at all is a
                // failure of this command.
                return 0;
            }
        }
        if Instant::now() >= deadline {
            eprintln!("daw-cli: no envelope answer for track {track} (slot {slot_index})");
            return 1;
        }
    }
}

fn get_automation_points(handle: &EngineHandle, args: &[String]) -> i32 {
    use daw_bridge::layout as L;
    let track = flag_u64(args, "--track", Some(0)).unwrap_or(0) as u32;
    let param = match flag(args, "--param") {
        Some(p) if !p.is_empty() => p,
        _ => {
            eprintln!("daw-cli: --param is required (the automation clip's id, e.g. index:0)");
            return 2;
        }
    };
    let param_id = match param_id_bytes(&param) {
        Ok(v) => v,
        Err(e) => { eprintln!("daw-cli: {e}"); return 2 }
    };
    // UNIQUE PER INVOCATION, for the reason spelled out in get_clip: a constant request id makes
    // every call after the first match the PREVIOUS call's answer the instant it is read, so
    // asking about lane B returns lane A and the caller concludes its write was lost.
    let request_seq = {
        let pid = std::process::id();
        let nanos = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.subsec_nanos())
            .unwrap_or(0);
        let id = pid.rotate_left(11) ^ nanos;
        if id == 0 { 1 } else { id }
    };
    let slot_index = (request_seq as usize) % 4; // K_UI_AUTOMATION_SLOTS
    let payload = L::UiAutomationLaneRequestPayload {
        command_type: UiCommandType::RequestAutomationLane as u16,
        flags: 0,
        request_seq,
        track_id: track,
        target_plugin_index: flag_u64(args, "--device", Some(0xFFFF_FFFF))
            .unwrap_or(0xFFFF_FFFF) as u32,
        param_id,
        reserved0: 0,
        reserved1: 0,
    };
    let deadline = Instant::now() + Duration::from_secs(5);
    loop {
        let _ = handle.send_automation_lane_request(payload);
        thread::sleep(Duration::from_millis(80));
        if let Some(a) = handle.read_automation_slot(slot_index) {
            if a.request_seq == request_seq {
                let pts: Vec<String> = a
                    .points
                    .iter()
                    .map(|(t, v)| format!("{{ \"nanotick\": {t}, \"value\": {v} }}"))
                    .collect();
                println!(
                    "{{ \"request_seq\": {}, \"track_id\": {}, \"param\": {:?}, \"found\": {}, \"discrete\": {}, \"points_truncated\": {}, \"points\": [{}] }}",
                    a.request_seq, a.track_id, a.param_id, a.found, a.discrete,
                    a.points_truncated, pts.join(", ")
                );
                // `found: false` is an ANSWER, and exit 0 says so. Only a request that was never
                // answered at all is a failure of this command.
                return 0;
            }
        }
        if Instant::now() >= deadline {
            eprintln!("daw-cli: no automation answer for track {track} param {param:?} (slot {slot_index})");
            return 1;
        }
    }
}

fn get_clip(handle: &EngineHandle, args: &[String]) -> i32 {
    let track = match flag_u64(args, "--track", Some(0)) {
        Ok(value) => value as u32,
        Err(err) => {
            eprintln!("daw-cli: {err}");
            return 2;
        }
    };
    let bars = flag_u64(args, "--bars", Some(4)).unwrap_or(4).max(1);
    let window_end = bars * 4 * NANOTICKS_PER_QUARTER;
    // UNIQUE PER INVOCATION. The read-back region is a single persistent slot that
    // keeps the last answer, so a constant id (this was 0x5ADD) matches the PREVIOUS
    // call's snapshot the instant it is read — every `get clip` after the first
    // returned the answer to the last one. Measured: asking for 2 bars right after 8
    // printed the 8-bar window, and a `get clip` straight after a `do note` reported
    // the note absent, which is exactly the observation that makes an agent conclude
    // its write was lost. Mixing the pid with the clock also stops two concurrent
    // requesters from taking delivery of each other's answers, which matters now that
    // `do` no longer needs --force.
    // Mixes the PAGE in as well as the clock, because the pages of one pagination run are issued
    // back to back and two of them landing on the same id is the stale-read bug above one level
    // down: page 2 would take delivery of page 1's answer and the loop would never advance.
    let request_id_for = |page: u32| {
        let pid = std::process::id();
        let nanos = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.subsec_nanos())
            .unwrap_or(0);
        let id = pid.rotate_left(11) ^ nanos ^ page.rotate_left(3);
        if id == 0 { 1 } else { id }
    };

    // THE WINDOW IS PAGINATED AND THIS USED TO READ ONE PAGE. An answer carries at most
    // K_UI_MAX_CLIP_NOTES (4096) notes; past that the engine stops early, reports where it
    // stopped in next_event_index and WITHHOLDS UI_CLIP_WINDOW_FLAG_COMPLETE. Both fields have
    // been published since the protocol was written and no client has ever read either, so
    // `get clip` printed the first page and exited 0 — a clip with more notes than that in the
    // window was reported as a shorter clip, with nothing on stderr. That is indistinguishable
    // from a clip that really is short, which is what makes it worth a loop rather than a
    // warning: the caller cannot tell the two apart afterwards.
    //
    // The chord array has its own smaller cap and breaks the same loop, so a chord-dense window
    // paginates too even when its note count is nowhere near 4096.
    const MAX_PAGES: u32 = 1024; // 4M notes; a backstop against a cursor that never completes.
    let mut attempt: u32 = 0;
    let (notes, chords, head_track, head_version, head_window_end) = loop {
        attempt += 1;
        let mut notes: Vec<daw_bridge::layout::UiClipNote> = Vec::new();
        let mut chords: Vec<daw_bridge::layout::UiClipChord> = Vec::new();
        let mut head: Option<(u32, u32, u64)> = None;
        let mut cursor: u32 = 0;
        let mut page: u32 = 0;
        let mut torn = false;
        loop {
            let request_id = request_id_for(page);
            let request = UiClipWindowCommandPayload {
                command_type: UiCommandType::RequestClipWindow as u16,
                flags: 0,
                track_id: track,
                request_id,
                window_start_lo: 0,
                window_start_hi: 0,
                window_end_lo: (window_end & 0xffff_ffff) as u32,
                window_end_hi: (window_end >> 32) as u32,
                cursor_event_index: cursor,
                reserved: 0,
                reserved2: 0,
            };
            if let Err(err) = handle.send_clip_window_request(request) {
                eprintln!("daw-cli: {err}");
                return 1;
            }

            let deadline = Instant::now() + Duration::from_secs(3);
            let mut snapshot = None;
            while Instant::now() < deadline {
                if let Some(candidate) = handle.read_clip_window() {
                    if candidate.request_id == request_id && candidate.track_id == track {
                        snapshot = Some(candidate);
                        break;
                    }
                }
                thread::sleep(Duration::from_millis(20));
            }
            let Some(snapshot) = snapshot else {
                eprintln!(
                    "daw-cli: timed out waiting for a clip window for track {track} \
                     (page {page}, cursor {cursor})"
                );
                return 1;
            };

            // A clip edited between two pages makes them describe two different clips, and
            // concatenating those invents a clip that never existed. clip_version is the
            // engine's own answer to "is this still the same clip", so start over rather than
            // splice.
            match head {
                Some((_, version, _)) if version != snapshot.clip_version => {
                    torn = true;
                    break;
                }
                None => {
                    head = Some((
                        snapshot.track_id,
                        snapshot.clip_version,
                        snapshot.window_end_nanotick,
                    ));
                }
                _ => {}
            }

            let note_count = (snapshot.note_count as usize).min(snapshot.notes.len());
            let chord_count = (snapshot.chord_count as usize).min(snapshot.chords.len());
            notes.extend_from_slice(&snapshot.notes[..note_count]);
            chords.extend_from_slice(&snapshot.chords[..chord_count]);

            if snapshot.flags & daw_bridge::layout::UI_CLIP_WINDOW_FLAG_COMPLETE != 0 {
                break;
            }
            // An incomplete answer that did not advance the cursor would loop forever asking the
            // same question. Say so instead of hanging, and keep what was read.
            if snapshot.next_event_index <= cursor {
                eprintln!(
                    "daw-cli: the engine reported the clip window incomplete but did not advance \
                     the cursor (page {page}, cursor {cursor}, next {}); reporting the {} note(s) \
                     read so far rather than looping",
                    snapshot.next_event_index,
                    notes.len()
                );
                break;
            }
            cursor = snapshot.next_event_index;
            page += 1;
            if page >= MAX_PAGES {
                eprintln!(
                    "daw-cli: the clip window is still incomplete after {MAX_PAGES} pages \
                     ({} notes); reporting what was read",
                    notes.len()
                );
                break;
            }
        }
        if !torn {
            let (t, v, w) = head.expect("the first page always sets the header");
            break (notes, chords, t, v, w);
        }
        if attempt >= 3 {
            eprintln!(
                "daw-cli: the clip kept changing while it was being read ({attempt} attempts); \
                 stop editing track {track} and try again"
            );
            return 1;
        }
    };

    let note_count = notes.len();
    let chord_count = chords.len();

    if args.iter().any(|a| a == "--grid") {
        print_grid(head_track, head_version, &notes, &chords, bars);
        return 0;
    }

    println!("{{");
    println!("  \"track_id\": {head_track},");
    println!("  \"clip_version\": {head_version},");
    println!("  \"window_end_nanotick\": {head_window_end},");
    println!("  \"notes\": [");
    for index in 0..note_count {
        let note = notes[index];
        let comma = if index + 1 == note_count { "" } else { "," };
        println!(
            "    {{ \"nanotick\": {}, \"duration\": {}, \"pitch\": {}, \"name\": \"{}\", \
             \"velocity\": {}, \"column\": {}, \"placement\": {} }}{comma}",
            note.t_on,
            note.t_off.saturating_sub(note.t_on),
            note.pitch,
            pitch_name(note.pitch),
            note.velocity,
            note.column,
            note.placement_id
        );
    }
    println!("  ],");
    println!("  \"chords\": [");
    for index in 0..chord_count {
        let chord = chords[index];
        let comma = if index + 1 == chord_count { "" } else { "," };
        println!(
            "    {{ \"nanotick\": {}, \"degree\": {}, \"quality\": {}, \"inversion\": {}, \
             \"base_octave\": {} }}{comma}",
            chord.nanotick, chord.degree, chord.quality, chord.inversion, chord.base_octave
        );
    }
    println!("  ]");
    println!("}}");
    0
}

/// A tracker-style text grid of the window. Not the UI's view — no cursor, no
/// pending edits — but the same shape, and readable in a terminal or a diff.
/// Takes the notes and chords as slices rather than the snapshot, because a window wider than
/// one page is assembled from several snapshots and no single one of them holds the clip.
fn print_grid(
    track_id: u32,
    clip_version: u32,
    notes: &[daw_bridge::layout::UiClipNote],
    chords: &[daw_bridge::layout::UiClipChord],
    bars: u64,
) {
    let row = NANOTICKS_PER_QUARTER / 4; // 16th notes
    let rows = (bars * 16) as usize;
    let mut cells: Vec<Vec<String>> = vec![vec![".".to_string(); 4]; rows];

    for note in notes {
        let r = (note.t_on / row) as usize;
        let c = (note.column as usize).min(3);
        if r < rows {
            cells[r][c] = pitch_name(note.pitch);
        }
    }
    for chord in chords {
        let r = (chord.nanotick / row) as usize;
        if r < rows {
            cells[r][3] = format!("~{}", chord.degree);
        }
    }

    println!("track {track_id}  clip_version {clip_version}");
    println!("row |  bar.beat | c0     c1     c2     | chord");
    println!("{}", "-".repeat(52));
    for (index, cell) in cells.iter().enumerate() {
        let beat = index / 4;
        let sub = index % 4;
        println!(
            "{:03} | {:>3}.{}.{} | {:<6} {:<6} {:<6} | {}",
            index,
            beat / 4 + 1,
            beat % 4 + 1,
            sub,
            cell[0],
            cell[1],
            cell[2],
            cell[3]
        );
    }
}

fn watch(handle: &EngineHandle) -> i32 {
    loop {
        if let Some(snapshot) = handle.snapshot() {
            println!(
                "uiVersion={} playhead={} rms0={}",
                snapshot.version,
                snapshot.ui_global_nanotick_playhead,
                snapshot.ui_track_peak_rms[0]
            );
        }
        thread::sleep(Duration::from_millis(16));
    }
}

fn main() {
    // DIE QUIETLY ON A CLOSED PIPE, like every other Unix tool.
    //
    // Rust ignores SIGPIPE and turns the resulting EPIPE into a panic on write, so
    // `daw-cli get notes | head -1` printed a panic and a backtrace note the moment its output
    // outran the reader. Inside a check that read as "no published notes — the fixture did not
    // load", i.e. the tool blaming the engine for the tool's own crash. It showed up as an
    // intermittent failure because whether the write lands before or after `head` exits is a
    // race, and the suite's flake reporting is what surfaced it.
    //
    // Restoring the default disposition makes a closed pipe kill the process silently, which is
    // what a caller composing with `head`, `grep -q` or `paste` already assumes.
    unsafe {
        libc::signal(libc::SIGPIPE, libc::SIG_DFL);
    }
    let args: Vec<String> = std::env::args().skip(1).collect();
    // M2.18: the ring is multi-producer now, so nothing needs acknowledging. The flag
    // is still accepted (and ignored) because it appears in a lot of scripts and notes.
    let _force = args.iter().any(|arg| arg == "--force");
    let positional: Vec<&str> = args
        .iter()
        .map(String::as_str)
        .filter(|arg| !arg.starts_with("--"))
        .collect();
    let name = default_shm_name();

    let code = match positional.split_first() {
        None | Some((&"watch", _)) => match EngineHandle::attach(&name, false) {
            Ok(handle) => watch(&handle),
            Err(err) => {
                eprintln!("daw-cli: {err}");
                1
            }
        },
        // `get clip` has to ask the engine for a window, and a request is a ring
        // write, so it needs a writable handle just like `do`.
        Some((&"get", rest)) if rest.first() == Some(&"clip") => {
            match EngineHandle::attach(&name, true) {
                Ok(handle) => get_clip(&handle, &args),
                Err(err) => {
                    eprintln!("daw-cli: {err}");
                    1
                }
            }
        }
        // Reads its own read-back, but first SENDS RequestDeviceParams — a ring write,
        // so it needs a writable handle (a read-only mmap makes send_command a silent
        // no-op).
        Some((&"get", rest)) if rest.first() == Some(&"device-params") => {
            match EngineHandle::attach(&name, true) {
                Ok(handle) => get_device_params(&handle, rest),
                Err(err) => {
                    eprintln!("daw-cli: {err}");
                    1
                }
            }
        }
        // Sends RequestAutomationLane before reading its answer, so it needs a writable
        // handle — a read-only mmap makes the send a silent no-op and the wait always times out.
        // v37: one modulator's envelope SHAPE. SamplerSetEnvelopePoints (84) could write a full
        // multi-segment envelope and nothing could read one back, so a pencil editor built on it
        // would have been write-only — able to send a curve and never to draw the one already in
        // the project.
        Some((&"get", rest)) if rest.first() == Some(&"sampler-envelope") => {
            match EngineHandle::attach(&name, true) {
                Ok(handle) => get_sampler_envelope(&handle, &args),
                Err(err) => { eprintln!("daw-cli: {err}"); 1 }
            }
        }
        Some((&"get", rest)) if rest.first() == Some(&"automation-points") => {
            match EngineHandle::attach(&name, true) {
                Ok(handle) => get_automation_points(&handle, &args),
                Err(err) => {
                    eprintln!("daw-cli: {err}");
                    1
                }
            }
        }
        // `get sampler-kit` needs to WRITE (the request) as well as read, so it attaches
        // read-write like `get waveform` does rather than through the read-only path below.
        Some((&"get", rest)) if rest.first() == Some(&"sampler-kit") => {
            match EngineHandle::attach(&name, true) {
                Ok(handle) => get_sampler_kit(&handle, &args),
                Err(err) => {
                    eprintln!("daw-cli: {err}");
                    1
                }
            }
        }
        Some((&"get", rest)) if rest.first() == Some(&"waveform") => {
            match EngineHandle::attach(&name, true) {
                Ok(handle) => get_waveform(&handle, rest, &args),
                Err(err) => {
                    eprintln!("daw-cli: {err}");
                    1
                }
            }
        }
        Some((&"get", rest)) => {
            let handle = match EngineHandle::attach(&name, false) {
                Ok(handle) => handle,
                Err(err) => {
                    eprintln!("daw-cli: {err}");
                    std::process::exit(1);
                }
            };
            match rest.first() {
                Some(&"transport") => get_transport(&handle),
                Some(&"tracks") => get_tracks(&handle),
                // WHAT THE ENGINE HAS REPORTED ON ITS OUTBOUND DIFF RING, sampled.
                //
                // Added because sampler refusals became reportable (UiDiffType::SamplerRejected)
                // and nothing on this side could see one — a channel with no reader is a claim,
                // not a feature, and the check for it would have had to read the engine's log,
                // which is the very thing the channel exists to replace.
                //
                // PEEK, NOT DRAIN: the ring is single-consumer and the real UI advances it, so a
                // tool that consumed here would steal diffs from the app it is observing.
                Some(&"diffs") => {
                    let diffs = handle.peek_ui_diffs();
                    println!("{{");
                    println!("  \"count\": {},", diffs.len());
                    println!("  \"diffs\": [");
                    for (i, (diff_type, payload)) in diffs.iter().enumerate() {
                        let comma = if i + 1 == diffs.len() { "" } else { "," };
                        let u16at = |o: usize| u16::from_le_bytes([payload[o], payload[o + 1]]);
                        let u32at = |o: usize| u32::from_le_bytes([
                            payload[o], payload[o + 1], payload[o + 2], payload[o + 3]]);
                        if *diff_type == daw_bridge::layout::UiDiffType::SamplerRejected as u16 {
                            println!(
                                "    {{ \"type\": \"sampler_rejected\", \"reason\": {}, \
                                 \"command\": {}, \"target\": {}, \"track\": {}, \
                                 \"device\": {} }}{comma}",
                                u16at(2), u16at(4), u16at(6), u32at(8), u32at(12));
                        } else {
                            println!("    {{ \"type\": {diff_type} }}{comma}");
                        }
                    }
                    println!("  ]");
                    println!("}}");
                    0
                }
                // THE PATCHER POOL AS THE ENGINE HAS IT, including each node's OWNING DEVICE.
                //
                // Added because the owner was the one fact a UI could not get: the region
                // publishes the assembled pool, so its region-level deviceId has no answer to
                // give, and without a per-node owner every patcher edit from a surface is
                // pool-scoped — which since patcher-is-a-device is not the graph a project
                // renders.
                Some(&"patcher") => {
                    let v = handle.read_patcher();
                    println!("{{");
                    println!("  \"version\": {},", v.version);
                    println!("  \"region_device\": {},", v.device_id);
                    let nodes: Vec<String> = v.nodes.iter().map(|n| {
                        format!(
                            "    {{ \"id\": {}, \"type\": {}, \"owner_device\": {}, \"has_config\": {} }}",
                            n.id, n.node_type, n.owner_device_id, n.has_config)
                    }).collect();
                    println!("  \"nodes\": [\n{}\n  ],", nodes.join(",\n"));
                    let edges: Vec<String> = v.edges.iter().map(|e| {
                        format!("    {{ \"src\": {}, \"dst\": {}, \"kind\": {} }}",
                                e.src_node, e.dst_node, e.kind)
                    }).collect();
                    println!("  \"edges\": [\n{}\n  ]", edges.join(",\n"));
                    println!("}}");
                    0
                }
                Some(&"notes") => get_notes(&handle, &args),
                Some(&"meters") => {
                    // v24 per-insert meters, dBFS millibels. device_id matches the chain
                    // snapshot's device, not a position.
                    println!("{{");
                    let count = handle.track_count() as usize;
                    let (ids, _flags) = handle.read_track_ids_and_flags();
                    for slot in 0..count {
                        let m = handle.read_device_meters(slot);
                        if m.is_empty() { continue; }
                        let tid = ids.get(slot).copied().unwrap_or(slot as u32);
                        let body: Vec<String> = m.iter().map(|(d, ip, op, ir, orms)| {
                            format!("{{ \"device\": {d}, \"in_peak_mb\": {ip}, \"out_peak_mb\": {op}, \"in_rms_mb\": {ir}, \"out_rms_mb\": {orms} }}")
                        }).collect();
                        println!("  \"track:{tid}\": [{}],", body.join(", "));
                    }
                    println!("}}");
                    0
                }
                Some(&"audio-sources") => get_audio_sources(&handle),
                Some(&"automation") => get_automation(&handle),
                /*
                 * `get harmony` — the PUBLISHED timeline, which is a different thing from the one
                 * in the saved document and that difference is the whole reason this exists.
                 *
                 * A harmony write bumps the harmony VERSION, which is what set_harmony waits on;
                 * the region every client draws from is republished separately. So "applied: true"
                 * and "the lane shows it" are two claims, and until now no surface could compare
                 * them. Reported from live use: four key changes written, all acknowledged, two
                 * visible.
                 */
                Some(&"harmony") => {
                    let events = handle.read_harmony();
                    println!("[");
                    for (i, e) in events.iter().enumerate() {
                        let comma = if i + 1 == events.len() { "" } else { "," };
                        println!("  {{ \"nanotick\": {}, \"root\": {}, \"scale_id\": {} }}{comma}",
                                 e.nanotick, e.root, e.scale_id);
                    }
                    println!("]");
                    0
                }
                Some(&"extents") => get_extents(&handle),
                // `get shared [--track N]`. The agent has had `shared_clips` since the
                // scratch-placement work; this is the same answer from the other surface.
                Some(&"shared") => {
                    let t = flag_u64(&args, "--track", None).ok().map(|v| v as u32);
                    get_shared(&handle, t)
                }
                Some(&"arrangement") => {
                    match handle.read_arrange_summary() {
                        Some(r) => {
                            println!("{{");
                            println!("  \"version\": {},", r.version);
                            println!("  \"song_end_tick\": {},", r.song_end_tick);
                            // Truncation is reported, not hidden: an incomplete list that
                            // says nothing reads as a complete one.
                            println!("  \"markers_truncated\": {},", r.markers_truncated);
                            println!("  \"time_sig_truncated\": {},", r.time_sig_truncated);
                            println!("  \"markers\": [");
                            let n = (r.marker_count as usize).min(r.markers.len());
                            for i in 0..n {
                                let m = r.markers[i];
                                let name = String::from_utf8_lossy(&m.name);
                                let name = name.trim_end_matches('\0');
                                let comma = if i + 1 == n { "" } else { "," };
                                // `span_end` is the NEXT marker's tick, or the song end for the
                                // last one — the implicit span a marker list gives you for free.
                                // Published as a convenience, not as a second source of truth: it
                                // is derived here from two numbers that are already in the list.
                                let span_end = if i + 1 < n {
                                    r.markers[i + 1].nanotick
                                } else {
                                    r.song_end_tick
                                };
                                // COLOUR, published in UiMarker since the region existed and
                                // printed by nothing — unreadable from every client, in the same
                                // way a field with no writer is unreachable. It had no writer
                                // either until SetMarkerColor (99); now that one exists, a caller
                                // has to be able to see the result.
                                println!(
                                    "    {{ \"id\": {}, \"name\": {:?}, \"bar\": {}, \"beat\": {}, \"nanotick\": {}, \"color_rgb\": {}, \"span_end\": {} }}{comma}",
                                    m.id, name, m.bar, m.beat, m.nanotick, m.color_rgb, span_end
                                );
                            }
                            println!("  ],");
                            println!("  \"time_sig\": [");
                            let m = (r.time_sig_count as usize).min(r.time_sig_points.len());
                            for i in 0..m {
                                let p = r.time_sig_points[i];
                                let comma = if i + 1 == m { "" } else { "," };
                                println!(
                                    "    {{ \"nanotick\": {}, \"sig\": \"{}/{}\" }}{comma}",
                                    p.nanotick, p.numerator, p.denominator
                                );
                            }
                            println!("  ]");
                            println!("}}");
                            0
                        }
                        None => {
                            eprintln!("daw-cli: no arrangement summary (older engine, or a write was in flight)");
                            1
                        }
                    }
                }
                other => {
                    eprintln!("daw-cli: unknown query {:?}\n\n{USAGE}", other.unwrap_or(&""));
                    2
                }
            }
        }
        Some((&"do", rest)) => {
            let handle = match EngineHandle::attach(&name, true) {
                Ok(handle) => handle,
                Err(err) => {
                    eprintln!("daw-cli: {err}");
                    std::process::exit(1);
                }
            };
            match rest.first() {
                Some(&"save") => {
                    let project = rest.get(1).copied().unwrap_or("default");
                    let code = send_named(&handle, UiCommandType::SaveProject, project);
                    if code == 0 {
                        println!("{{ \"saved\": \"{}\" }}", escape(project));
                    }
                    code
                }
                /*
                 * `do new NAME` — a new, empty project, and then load it.
                 *
                 * The only op on the console that had no CLI verb and was not a fair exemption.
                 * The browser cannot write files, so `new` was built as a sidecar message; a
                 * native process has no such excuse, and "start a song" is not a thing that
                 * should require a browser.
                 *
                 * The DOCUMENT comes from daw_bridge::project so this cannot drift from what the
                 * sidecar writes — a second definition of "empty project" fails quietly, because
                 * every version of it still loads.
                 */
                Some(&"new") => match rest.get(1).copied() {
                    None => {
                        eprintln!("daw-cli: do new NAME");
                        1
                    }
                    Some(name) => {
                    let dir = daw_bridge::project::engine_project_dir();
                    match daw_bridge::project::new_project(&dir, name) {
                        Err(e) => {
                            eprintln!("daw-cli: {e}");
                            1
                        }
                        Ok(path) => {
                            // LOADED THROUGH THE ORDINARY PATH, exactly as the sidecar does it,
                            // so a new song arrives by the same route as an opened one.
                            let code = send_named(&handle, UiCommandType::LoadProject, name);
                            if code == 0 {
                                println!("{{ \"new\": \"{}\", \"path\": \"{}\" }}",
                                         escape(name), escape(&path.to_string_lossy()));
                            }
                            code
                        }
                    }
                    }
                },
                /*
                 * `do transpose --track N --semitones S [--from T] [--to T]`
                 *
                 * A RANGE, because the console's transpose acts on a SELECTION and a selection is
                 * view state this surface does not have. Same arithmetic and the same two edge
                 * rules as the agent's tool — both call plan_transpose — so the answer cannot
                 * differ by which surface asked.
                 */
                /*
                 * `do copy|cut|paste` — a RANGE, and a clipboard that outlives the process.
                 *
                 * These sat on the parity lists longer than anything else, and the obstacle was
                 * never the ops: it was that daw-cli EXITS between the copy and the paste, so
                 * there was nowhere to put it. `daw_bridge::clipboard` is a file beside the
                 * projects, which also means the CLI and the agent share one clipboard — copy
                 * with either, paste with the other.
                 *
                 * Notes are stored RELATIVE to the copy's origin, exactly as the page's clipboard
                 * does. An absolute clipboard can only be pasted back where it came from.
                 */
                Some(&"copy") | Some(&"cut") => {
                    let cutting = rest.first().copied() == Some(&"cut");
                    let track = flag_u64(&args, "--track", Some(0)).unwrap_or(0) as u32;
                    let from = flag_u64(&args, "--from", Some(0)).unwrap_or(0);
                    let to = flag_u64(&args, "--to", Some(u64::MAX)).unwrap_or(u64::MAX);
                    if to <= from {
                        eprintln!("daw-cli: --to must be after --from (the range is half-open)");
                        std::process::exit(2);
                    }
                    let Some(snap) = handle.read_track_clip(track) else {
                        eprintln!("daw-cli: track {track} has no clip to read");
                        std::process::exit(1);
                    };
                    let n = (snap.note_count as usize).min(snap.notes.len());
                    let picked: Vec<_> = snap.notes[..n].iter()
                        .filter(|x| x.t_on >= from && x.t_on < to)
                        .collect();
                    if picked.is_empty() {
                        eprintln!("daw-cli: no notes on track {track} between {from} and {to}");
                        std::process::exit(1);
                    }
                    // RELATIVE TO THE FIRST NOTE, not to `from`: a copy that starts at a bar line
                    // and a copy that starts at the first note must paste identically, which is
                    // what a person means by "copy this phrase".
                    let origin = picked.iter().map(|x| x.t_on).min().unwrap_or(0);
                    let notes: Vec<_> = picked.iter().map(|x| {
                        daw_bridge::clipboard::ClipboardNote {
                            dt: x.t_on - origin,
                            duration: x.t_off.saturating_sub(x.t_on).max(1),
                            pitch: x.pitch, velocity: x.velocity, column: x.column,
                            d_track: 0,
                        }
                    }).collect();
                    let dir = daw_bridge::project::engine_project_dir();
                    match daw_bridge::clipboard::store(&dir, &notes) {
                        Err(e) => { eprintln!("daw-cli: {e}"); 1 }
                        Ok(path) => {
                            let mut deleted = 0usize;
                            if cutting {
                                let first = handle.clip_version_for_track(track);
                                let mut base = first;
                                for x in &picked {
                                    let column = match daw_bridge::layout::edit_column(
                                        u64::from(x.column)) { Ok(c) => c, Err(_) => 0 };
                                    let mut p = UiCommandPayload {
                                        command_type: UiCommandType::DeleteNote as u16,
                                        flags: column, track_id: track, plugin_index: 0,
                                        note_pitch: 0, value0: 0,
                                        note_nanotick_lo: (x.t_on & 0xffff_ffff) as u32,
                                        note_nanotick_hi: (x.t_on >> 32) as u32,
                                        note_duration_lo: 0, note_duration_hi: 0,
                                        base_version: base,
                                    };
                                    p.base_version = base;
                                    if handle.send_command(p).is_err() { break }
                                    deleted += 1;
                                    base = base.wrapping_add(1);
                                }
                                handle.wait_for_track_clip_version(
                                    track, first, first.wrapping_add(deleted as u32),
                                    Duration::from_secs(2));
                            }
                            println!("{{ \"{}\": {}, \"deleted\": {deleted}, \"track\": {track}, \"clipboard\": \"{}\" }}",
                                     if cutting { "cut" } else { "copied" }, notes.len(),
                                     escape(&path.to_string_lossy()));
                            0
                        }
                    }
                }
                Some(&"paste") => {
                    let track = flag_u64(&args, "--track", Some(0)).unwrap_or(0) as u32;
                    let at = flag_u64(&args, "--at", Some(0)).unwrap_or(0);
                    let dir = daw_bridge::project::engine_project_dir();
                    let notes = match daw_bridge::clipboard::load(&dir) {
                        Ok(v) => v,
                        Err(e) => { eprintln!("daw-cli: {e}"); std::process::exit(1) }
                    };
                    if notes.is_empty() {
                        // "Nothing copied" and "clipboard corrupt" are different answers; load()
                        // keeps them apart and so does this.
                        eprintln!("daw-cli: the clipboard is empty — copy something first");
                        std::process::exit(1);
                    }
                    let first = handle.clip_version_for_track(track);
                    let mut base = first;
                    let mut sent = 0usize;
                    for m in &notes {
                        let column = match daw_bridge::layout::edit_column(u64::from(m.column)) {
                            Ok(c) => c,
                            Err(e) => { eprintln!("daw-cli: {e}"); break }
                        };
                        let tick = at.saturating_add(m.dt);
                        let p = UiCommandPayload {
                            command_type: UiCommandType::WriteNote as u16,
                            flags: column, track_id: track, plugin_index: 0,
                            note_pitch: u32::from(m.pitch), value0: u32::from(m.velocity),
                            note_nanotick_lo: (tick & 0xffff_ffff) as u32,
                            note_nanotick_hi: (tick >> 32) as u32,
                            note_duration_lo: (m.duration & 0xffff_ffff) as u32,
                            note_duration_hi: (m.duration >> 32) as u32,
                            base_version: base,
                        };
                        if handle.send_command(p).is_err() { break }
                        sent += 1;
                        base = base.wrapping_add(1);
                    }
                    let applied = handle.wait_for_track_clip_version(
                        track, first, first.wrapping_add(sent as u32), Duration::from_secs(2));
                    println!("{{ \"pasted\": {sent}, \"track\": {track}, \"at\": {at}, \"applied\": {applied} }}");
                    if sent == notes.len() { 0 } else { 1 }
                }
                Some(&"transpose") => {
                    let track = flag_u64(&args, "--track", Some(0)).unwrap_or(0) as u32;
                    let semitones = match flag_i64(&args, "--semitones", 0) {
                        Ok(v) if v != 0 => v as i32,
                        Ok(_) => { eprintln!("daw-cli: --semitones is required and cannot be 0"); std::process::exit(2) }
                        Err(e) => { eprintln!("daw-cli: {e}"); std::process::exit(2) }
                    };
                    let from = flag_u64(&args, "--from", Some(0)).unwrap_or(0);
                    let to = flag_u64(&args, "--to", Some(u64::MAX)).unwrap_or(u64::MAX);
                    if to <= from {
                        eprintln!("daw-cli: --to must be after --from (the range is half-open)");
                        std::process::exit(2);
                    }
                    let Some(snap) = handle.read_track_clip(track) else {
                        eprintln!("daw-cli: track {track} has no clip to read");
                        std::process::exit(1);
                    };
                    // THE PUBLISHED WINDOW, not the track — see the agent tool's note. The
                    // range is intersected with it and the reply states the span acted on, so a
                    // partial transpose cannot report as a whole one.
                    let win_from = from.max(snap.window_start_nanotick);
                    let win_to = if snap.window_end_nanotick > snap.window_start_nanotick {
                        to.min(snap.window_end_nanotick)
                    } else { to };
                    if win_to <= win_from {
                        eprintln!("daw-cli: the range {from}..{to} lies outside the published window {}..{}",
                                  snap.window_start_nanotick, snap.window_end_nanotick);
                        std::process::exit(1);
                    }
                    // ONE EDIT PER CLIP CELL, not one per appearance — see the agent tool's
                    // note. A shared clip is transposed once and every appearance follows, which
                    // is what sharing means; writing it once per appearance is what turned two
                    // notes into three.
                    let n = (snap.note_count as usize).min(snap.notes.len());
                    let extents = handle.read_clip_extents();
                    let unique = daw_bridge::layout::dedupe_by_clip_cell(
                        &snap.notes[..n], &extents, track);
                    let folded = n.saturating_sub(unique.len());
                    let plan = daw_bridge::layout::plan_transpose(
                        &unique, win_from, win_to, semitones);
                    if plan.moved.is_empty() {
                        eprintln!("daw-cli: nothing to transpose ({} skipped as out of MIDI range)",
                                  plan.skipped);
                        std::process::exit(1);
                    }
                    let first_base = handle.clip_version_for_track(track);
                    let mut base = first_base;
                    let mut sent = 0usize;
                    let mut failed = None;
                    for m in &plan.moved {
                        let column = match daw_bridge::layout::edit_column(u64::from(m.column)) {
                            Ok(c) => c,
                            Err(e) => { failed = Some(e); break }
                        };
                        let payload = UiCommandPayload {
                            command_type: UiCommandType::WriteNote as u16,
                            flags: column,
                            track_id: track,
                            plugin_index: 0,
                            note_pitch: u32::from(m.pitch),
                            value0: u32::from(m.velocity),
                            note_nanotick_lo: (m.tick & 0xffff_ffff) as u32,
                            note_nanotick_hi: (m.tick >> 32) as u32,
                            note_duration_lo: (m.duration & 0xffff_ffff) as u32,
                            note_duration_hi: (m.duration >> 32) as u32,
                            base_version: base,
                        };
                        if let Err(e) = handle.send_command(payload) { failed = Some(e); break }
                        sent += 1;
                        base = base.wrapping_add(1);
                    }
                    if let Some(e) = failed {
                        eprintln!("daw-cli: {e} after {sent} notes");
                        1
                    } else {
                        let applied = handle.wait_for_track_clip_version(
                            track, first_base, first_base.wrapping_add(sent as u32),
                            Duration::from_secs(2));
                        println!("{{ \"transposed\": {sent}, \"skipped\": {}, \"semitones\": {semitones}, \"track\": {track}, \"applied\": {applied}, \"from\": {win_from}, \"to\": {win_to}, \"clipped_to_window\": {}, \"shared_appearances_folded\": {} }}",
                                 plan.skipped, win_from != from || win_to != to, folded);
                        0
                    }
                }
                Some(&"load") => {
                    let project = rest.get(1).copied().unwrap_or("default");
                    let code = send_named(&handle, UiCommandType::LoadProject, project);
                    if code == 0 {
                        println!("{{ \"loaded\": \"{}\" }}", escape(project));
                    }
                    code
                }
                // THE `.uni` MODULE. `save` writes a directory you edit; `save-module` writes a
                // FILE you send. Both forms stay on disk and they are the same document.
                Some(&"save-module") => {
                    let project = rest.get(1).copied().unwrap_or("default");
                    let code = send_named(&handle, UiCommandType::SaveModule, project);
                    if code == 0 {
                        println!("{{ \"saved_module\": \"{}.uni\" }}", escape(project));
                    }
                    code
                }
                Some(&"load-module") => {
                    let project = rest.get(1).copied().unwrap_or("default");
                    let code = send_named(&handle, UiCommandType::LoadModule, project);
                    if code == 0 {
                        println!("{{ \"loaded_module\": \"{}.uni\" }}", escape(project));
                    }
                    code
                }
                Some(&"play") => {
                    let payload = UiCommandPayload {
                        command_type: UiCommandType::TogglePlay as u16,
                        flags: 0,
                        track_id: 0,
                        plugin_index: 0,
                        note_pitch: 0,
                        value0: 0,
                        note_nanotick_lo: 0,
                        note_nanotick_hi: 0,
                        note_duration_lo: 0,
                        note_duration_hi: 0,
                        base_version: 0,
                    };
                    match handle.send_command(payload) {
                        Ok(()) => {
                            println!("{{ \"toggled\": \"play\" }}");
                            0
                        }
                        Err(err) => {
                            eprintln!("daw-cli: {err}");
                            1
                        }
                    }
                }
                Some(&"set-tempo") => set_tempo(&handle, rest),
                Some(&"set-param") => set_param(&handle, rest),
                Some(&"note") | Some(&"delete-note") => {
                    let is_write = rest.first() == Some(&"note");
                    let command = if is_write {
                        UiCommandType::WriteNote
                    } else {
                        UiCommandType::DeleteNote
                    };
                    // The engine advances one version per applied edit, so read
                    // the current one immediately before sending — and read the
                    // version of the TRACK being edited (M2.17), not the global one,
                    // so a concurrent author on another track does not invalidate
                    // this edit.
                    let track = flag_u64(&args, "--track", Some(0)).unwrap_or(0) as u32;
                    // --base lets a caller present a base it read EARLIER (that is what
                    // a real concurrent author does: read, think, then write). Without
                    // it every invocation re-reads immediately before sending and can
                    // never exercise staleness at all.
                    let base = match flag_u64(&args, "--base", Some(u64::MAX)) {
                        Ok(v) if v != u64::MAX => v as u32,
                        _ => handle.clip_version_for_track(track),
                    };
                    // RETRY UNLESS THE CALLER PINNED A BASE, and that distinction is the whole
                    // policy. Passing --base means "I read this version earlier and I am writing
                    // against it" — a concurrent author, deliberately testing staleness — so a
                    // refusal is the ANSWER they asked for and must be reported, not papered over.
                    // Omitting it means "apply this now"; the caller has no opinion about a
                    // version, so a stale-base refusal is pure noise from a publish that had not
                    // caught up, and retrying against the version the engine handed back is
                    // exactly what it asked for with resync_requested.
                    let pinned_base = matches!(flag_u64(&args, "--base", Some(u64::MAX)),
                                               Ok(v) if v != u64::MAX);
                    let retry_stale = !pinned_base || args.iter().any(|a| *a == "--retry-stale");
                    let before_len = handle.peek_ui_diffs().len();
                    let ver_before = handle.clip_version_for_track(track);
                    match note_command(command, &args, base) {
                        Ok(payload) => match handle.send_command(payload) {
                            Ok(()) => {
                                let label = if is_write { "note" } else { "delete-note" };
                                let cmd = command as u16;
                                match await_clip_outcome(&handle, track, cmd, base, before_len, ver_before) {
                                    ClipOutcome::Applied | ClipOutcome::Unknown => {
                                        println!(
                                            "{{ \"sent\": \"{label}\", \"base_version\": {base} }}"
                                        );
                                        0
                                    }
                                    ClipOutcome::Refused { reason, current } if retry_stale
                                        && reason == daw_bridge::layout::UiClipRejectReason::StaleBase as u16 => {
                                        match note_command(command, &args, current) {
                                            Ok(again) => match handle.send_command(again) {
                                                Ok(()) => match await_clip_outcome(&handle, track, cmd, current, handle.peek_ui_diffs().len(), handle.clip_version_for_track(track)) {
                                                    ClipOutcome::Applied | ClipOutcome::Unknown => {
                                                        eprintln!("daw-cli: base {base} was stale; retried at {current}");
                                                        println!("{{ \"sent\": \"{label}\", \"base_version\": {current}, \"retried\": true }}");
                                                        0
                                                    }
                                                    ClipOutcome::Refused { reason: r2, current: c2 } => {
                                                        eprintln!("daw-cli: {label} REFUSED again after retry (reason {r2}, engine now at {c2})");
                                                        3
                                                    }
                                                },
                                                Err(err) => { eprintln!("daw-cli: {err}"); 1 }
                                            },
                                            Err(err) => { eprintln!("daw-cli: {err}"); 2 }
                                        }
                                    }
                                    ClipOutcome::Refused { reason, current } => {
                                        eprintln!("daw-cli: the engine REFUSED this {label} — reason {reason}, \
                                                   presented base {base}, engine holds {current}. \
                                                   The edit was NOT applied. Re-read the clip version \
                                                   and send again, or pass --retry-stale.");
                                        3
                                    }
                                }
                            }
                            Err(err) => {
                                eprintln!("daw-cli: {err}");
                                1
                            }
                        },
                        Err(err) => {
                            eprintln!("daw-cli: {err}");
                            2
                        }
                    }
                }
                Some(&"preview") => match preview_command(&args) {
                    Ok(payload) => match handle.send_command(payload) {
                        Ok(()) => {
                            println!("{{ \"sent\": \"preview\" }}");
                            0
                        }
                        Err(err) => {
                            eprintln!("daw-cli: {err}");
                            1
                        }
                    },
                    Err(err) => {
                        eprintln!("daw-cli: {err}");
                        2
                    }
                },
                Some(&"rename") => {
                    let track = flag_u64(&args, "--track", Some(0)).unwrap_or(0) as u32;
                    // REQUIRED. Defaulting to an empty string sent a rename the engine
                    // then ignored, while the CLI printed `"sent": "rename"` — the exact
                    // silent-success shape being hunted out of this codebase.
                    let name = match flag(&args, "--name") {
                        Some(n) if !n.is_empty() => n,
                        _ => {
                            eprintln!("daw-cli: --name is required and must not be empty");
                            std::process::exit(2)
                        }
                    };
                    let mut bytes = [0u8; 28];
                    let src = name.as_bytes();
                    let len = src.len().min(bytes.len());
                    bytes[..len].copy_from_slice(&src[..len]);
                    let payload = UiPatcherPresetCommandPayload {
                        command_type: UiCommandType::SetTrackName as u16,
                        flags: 0,
                        track_id: track,
                        base_version: 0,
                        name: bytes,
                    };
                    let as_ui: UiCommandPayload = unsafe { std::mem::transmute(payload) };
                    match handle.send_command(as_ui) {
                        Ok(()) => {
                            println!("{{ \"sent\": \"rename\", \"track\": {track}, \"name\": {name:?} }}");
                            0
                        }
                        Err(err) => {
                            eprintln!("daw-cli: {err}");
                            1
                        }
                    }
                }
                Some(&"undo") | Some(&"redo") => {
                    let is_undo = rest.first() == Some(&"undo");
                    let cmd = if is_undo {
                        UiCommandType::Undo
                    } else {
                        UiCommandType::Redo
                    };
                    // base_version must match the engine's current clip version.
                    let base = handle.clip_version();
                    let mut payload = track_structure_command(cmd, 0);
                    payload.base_version = base;
                    match handle.send_command(payload) {
                        Ok(()) => {
                            println!("{{ \"sent\": {:?}, \"base_version\": {base} }}", if is_undo { "undo" } else { "redo" });
                            0
                        }
                        Err(err) => {
                            eprintln!("daw-cli: {err}");
                            1
                        }
                    }
                }
                Some(&"panic") => {
                    // All sound off: CC120 + CC123 on every channel of every hosted
                    // plugin, and all pending/active note state dropped.
                    match handle.send_command(UiCommandPayload {
                        command_type: UiCommandType::Panic as u16,
                        flags: 0,
                        track_id: 0,
                        plugin_index: 0,
                        note_pitch: 0,
                        value0: 0,
                        note_nanotick_lo: 0,
                        note_nanotick_hi: 0,
                        note_duration_lo: 0,
                        note_duration_hi: 0,
                        base_version: 0,
                    }) {
                        Ok(()) => {
                            println!("{{ \"sent\": \"panic\" }}");
                            0
                        }
                        Err(err) => {
                            eprintln!("daw-cli: {err}");
                            1
                        }
                    }
                }
                Some(&"add-track") => {
                    match handle.send_command(track_structure_command(
                        UiCommandType::AddTrack,
                        0,
                    )) {
                        Ok(()) => {
                            println!("{{ \"sent\": \"add-track\" }}");
                            0
                        }
                        Err(err) => {
                            eprintln!("daw-cli: {err}");
                            1
                        }
                    }
                }
                Some(&"remove-track") => match flag_u64(&args, "--track", None) {
                    Ok(track) => match handle.send_command(track_structure_command(
                        UiCommandType::RemoveTrack,
                        track as u32,
                    )) {
                        Ok(()) => {
                            println!("{{ \"sent\": \"remove-track\", \"track\": {track} }}");
                            0
                        }
                        Err(err) => {
                            eprintln!("daw-cli: {err}");
                            1
                        }
                    },
                    Err(err) => {
                        eprintln!("daw-cli: {err}");
                        2
                    }
                },
                Some(&"set-bypass") => {
                    // Toggle an insert's bypass live: --track <id|master> --device N
                    // --bypass 0|1. UpdateDevice flags bit0 = "apply bypass".
                    let track_arg = args.iter().position(|a| a == "--track")
                        .and_then(|i| args.get(i + 1)).map(String::as_str).unwrap_or("0");
                    let track = if track_arg == "master" { MASTER_TRACK_ID }
                                else { track_arg.parse::<u32>().unwrap_or(0) };
                    let device = flag_u64(&args, "--device", Some(0)).unwrap_or(0) as u32;
                    let bypass = flag_u64(&args, "--bypass", Some(1)).unwrap_or(1) as u32;
                    let payload = UiChainCommandPayload {
                        command_type: UiCommandType::UpdateDevice as u16,
                        flags: 0x1,
                        track_id: track,
                        base_version: 0,
                        device_id: device,
                        device_kind: 0,
                        insert_index: 0,
                        patcher_node_id: 0,
                        host_slot_index: 0,
                        bypass,
                        reserved: [0; 4],
                    };
                    send_chain_reporting(&handle, payload, "set-bypass", track,
                                        &format!(", \"device\": {device}, \"bypass\": {bypass}"))
                }
                Some(&"add-device") => {
                    // --track accepts a numeric id or "master"; --kind is a device kind
                    // string; --at is the insert index (default = append); --plugin is a
                    // plugin-cache slot for a VST device (default = none).
                    let track_arg = args
                        .iter()
                        .position(|a| a == "--track")
                        .and_then(|i| args.get(i + 1))
                        .map(String::as_str)
                        .unwrap_or("0");
                    let track = if track_arg == "master" {
                        MASTER_TRACK_ID
                    } else {
                        track_arg.parse::<u32>().unwrap_or(0)
                    };
                    let kind_arg = args
                        .iter()
                        .position(|a| a == "--kind")
                        .and_then(|i| args.get(i + 1))
                        .map(String::as_str)
                        .unwrap_or("patcher_event");
                    let kind = match kind_arg {
                        "patcher_event" => Some(0u32),
                        "patcher_instrument" => Some(1),
                        "patcher_audio" => Some(2),
                        "vst_instrument" => Some(3),
                        "vst_effect" => Some(4),
                        // The built-in sampler. A head-of-chain instrument like a VST
                        // instrument, but rendered in the engine, so a VST effect can follow it
                        // on the same track.
                        "sampler" => Some(5),
                        _ => None,
                    };
                    match kind {
                        None => {
                            eprintln!("daw-cli: unknown --kind {kind_arg}");
                            2
                        }
                        Some(kind) => {
                            let device_id =
                                flag_u64(&args, "--device-id", Some(0)).unwrap_or(0) as u32;
                            let insert = flag_u64(&args, "--at", Some(0xFFFF_FFFF))
                                .unwrap_or(0xFFFF_FFFF)
                                as u32;
                            let plugin = flag_u64(&args, "--plugin", Some(0xFFFF_FFFE))
                                .unwrap_or(0xFFFF_FFFE)
                                as u32;
                            let payload = UiChainCommandPayload {
                                command_type: UiCommandType::AddDevice as u16,
                                flags: 0,
                                track_id: track,
                                base_version: 0,
                                device_id,
                                device_kind: kind,
                                insert_index: insert,
                                patcher_node_id: 0xFFFF_FFFF,
                                host_slot_index: plugin,
                                bypass: 0,
                                reserved: [0; 4],
                            };
                            send_chain_reporting(&handle, payload, "add-device", track,
                                                &format!(", \"kind\": {kind_arg:?}"))
                        }
                    }
                }
                Some(&"sampler-load") => {
                    // --file is a name RELATIVE TO THE PROJECT DIRECTORY, capped at 24 bytes by
                    // the command payload. Absolute paths are refused rather than truncated: a
                    // silently shortened path resolves to nothing and the slot is mysteriously
                    // silent, while a refusal says what to do about it.
                    let track = flag_u64(&args, "--track", Some(0)).unwrap_or(0) as u32;
                    let device = flag_u64(&args, "--device", Some(0)).unwrap_or(0) as u32;
                    let root = flag_u64(&args, "--root", Some(60)).unwrap_or(60) as u8;
                    // --files lays a whole KIT down in one gesture: N samples on N consecutive
                    // keys from --root. It sends N separate commands rather than inventing a
                    // bulk payload, which keeps the 40-byte ring exactly as it is — the ring is
                    // one cache line for lock-free reasons and should not grow to carry a list.
                    let files_arg = args
                        .iter()
                        .position(|a| a == "--files")
                        .and_then(|i| args.get(i + 1))
                        .cloned();
                    let file = args
                        .iter()
                        .position(|a| a == "--file")
                        .and_then(|i| args.get(i + 1))
                        .cloned()
                        .unwrap_or_default();
                    let fixed = args.iter().any(|a| a == "--fixed-pitch");
                    if let Some(list) = files_arg {
                        let names: Vec<&str> =
                            list.split(',').map(str::trim).filter(|s| !s.is_empty()).collect();
                        if names.is_empty() {
                            eprintln!("daw-cli: --files was empty");
                            2
                        } else if names.len() > 128 {
                            eprintln!("daw-cli: --files takes at most 128 names, got {}", names.len());
                            2
                        } else {
                            let mut sent = 0usize;
                            let mut bad: Option<String> = None;
                            let journal_at = sampler_journal_mark();
                            for (i, n) in names.iter().enumerate() {
                                let key = (root as usize).saturating_add(i);
                                if key > 127 {
                                    bad = Some(format!(
                                        "{} names from root {root} runs past key 127",
                                        names.len()
                                    ));
                                    break;
                                }
                                if n.starts_with('/') || n.contains("..") {
                                    bad = Some(format!("{n:?} is not project-relative"));
                                    break;
                                }
                                if n.len() >= 24 {
                                    bad = Some(format!("{n:?} is longer than 23 bytes"));
                                    break;
                                }
                                let mut name = [0u8; 24];
                                name[..n.len()].copy_from_slice(n.as_bytes());
                                let payload = UiSamplerLoadPayload {
                                    command_type: UiCommandType::SamplerLoad as u16,
                                    // A KIT IS FIXED-PITCH BY DEFAULT. Eight one-shots laid on
                                    // eight keys are eight drums, not eight overlapping zones —
                                    // and overlapping full-range zones would make every key play
                                    // all eight, which is a confusing first experience.
                                    flags: SAMPLER_LOAD_FIXED_PITCH,
                                    track_id: track,
                                    device_id: device,
                                    root_key: key as u8,
                                    reserved: [0; 3],
                                    name,
                                };
                                if let Err(err) = handle.send_sampler_load(payload) {
                                    bad = Some(err);
                                    break;
                                }
                                sent += 1;
                            }
                            match bad {
                                // PARTIAL PROGRESS IS REPORTED. Stopping halfway and saying
                                // nothing would leave a half-built kit that looks like a bug in
                                // the engine rather than a bad argument.
                                Some(why) => {
                                    eprintln!("daw-cli: sampler-load --files stopped after {sent} of {}: {why}", names.len());
                                    1
                                }
                                None => {
                                    report_sampler_outcome("sampler-load", "sampler_load", track, journal_at,
                                                          &format!(", \"device\": {device}, \"files\": {sent}, \"base_key\": {root}"))
                                }
                            }
                        }
                    } else if file.is_empty() {
                        eprintln!("daw-cli: sampler-load needs --file <name> or --files a,b,c");
                        2
                    } else if file.starts_with('/') || file.contains("..") {
                        eprintln!(
                            "daw-cli: --file is relative to the project directory, not a path \
                             (a project that names a sample by absolute path stops playing the \
                             moment you send someone the module)"
                        );
                        2
                    } else if file.len() >= 24 {
                        eprintln!(
                            "daw-cli: --file is capped at 23 bytes by the command payload, got {}",
                            file.len()
                        );
                        2
                    } else {
                        let mut name = [0u8; 24];
                        name[..file.len()].copy_from_slice(file.as_bytes());
                        let payload = UiSamplerLoadPayload {
                            command_type: UiCommandType::SamplerLoad as u16,
                            flags: if fixed { SAMPLER_LOAD_FIXED_PITCH } else { 0 },
                            track_id: track,
                            device_id: device,
                            root_key: root,
                            reserved: [0; 3],
                            name,
                        };
                        let journal_at = sampler_journal_mark();
                        match handle.send_sampler_load(payload) {
                            Ok(()) => {
                                report_sampler_outcome("sampler-load", "sampler_load", track, journal_at,
                                                      &format!(", \"device\": {device}, \"file\": {file:?}, \"root\": {root}, \"fixed_pitch\": {fixed}"))
                            }
                            Err(err) => {
                                eprintln!("daw-cli: {err}");
                                1
                            }
                        }
                    }
                }
                Some(&"sampler-device") => {
                    // The DEVICE's own fields, not a slot's. `default-gate` is the per-bank
                    // "ignore note-offs" default — it seeds a slot when one is MINTED and stops
                    // mattering, so setting it changes what the NEXT load or slice produces and
                    // leaves every existing slot alone. That is deliberate; see
                    // SamplerState::defaultGate.
                    let track = flag_u64(&args, "--track", Some(0)).unwrap_or(0) as u32;
                    let device = flag_u64(&args, "--device", Some(0)).unwrap_or(0) as u32;
                    let field_arg = args
                        .iter()
                        .position(|a| a == "--field")
                        .and_then(|i| args.get(i + 1))
                        .map(String::as_str)
                        .unwrap_or("");
                    let value = args
                        .iter()
                        .position(|a| a == "--value")
                        .and_then(|i| args.get(i + 1))
                        .and_then(|v| v.parse::<i32>().ok());
                    let field = SAMPLER_DEVICE_FIELDS
                        .iter()
                        .find(|(n, _)| *n == field_arg)
                        .map(|(_, id)| *id);
                    match (field, value) {
                        // An unknown field LISTS the set rather than doing nothing, the same rule
                        // sampler-slot follows: a caller who mistypes should not have to read the
                        // source to find out what is available.
                        (None, _) => {
                            let names: Vec<&str> =
                                SAMPLER_DEVICE_FIELDS.iter().map(|(n, _)| *n).collect();
                            eprintln!("daw-cli: --field must be one of: {}", names.join(", "));
                            2
                        }
                        (_, None) => {
                            eprintln!("daw-cli: sampler-device needs --value <int>");
                            2
                        }
                        (Some(f), Some(v)) => {
                            let payload = daw_bridge::layout::UiSamplerSetDevicePayload {
                                command_type: UiCommandType::SamplerSetDevice as u16,
                                field: f,
                                track_id: track,
                                device_id: device,
                                value: v,
                                reserved: [0; 24],
                            };
                            let journal_at = sampler_journal_mark();
                            match handle.send_sampler_set_device(payload) {
                                Ok(()) => {
                                    report_sampler_outcome("sampler-device", "sampler_set_device", track, journal_at, &format!(", \"device\": {device}, \"field\": {field_arg:?}, \"value\": {v}"))
                                }
                                Err(err) => { eprintln!("daw-cli: {err}"); 1 }
                            }
                        }
                    }
                }
                // RENAME A PAD (opcode 90), over the bulk carrier. `name` is not an int, so it
                // cannot ride `sampler-slot --field`: that payload's `value` is an i32.
                Some(&"sampler-slot-name") => {
                    let track = flag_u64(&args, "--track", Some(0)).unwrap_or(0) as u32;
                    let device = flag_u64(&args, "--device", Some(0)).unwrap_or(0) as u32;
                    let slot = flag_u64(&args, "--slot", Some(0)).unwrap_or(0) as u32;
                    // An ABSENT --name is an error; an EMPTY one is a legal rename to unnamed.
                    // Defaulting the missing case to "" would make a typo'd flag silently erase
                    // the name instead of saying so.
                    let name = args
                        .iter()
                        .position(|a| a == "--name")
                        .and_then(|i| args.get(i + 1))
                        .map(String::as_str);
                    match name {
                        None => {
                            eprintln!("daw-cli: sampler-slot-name needs --name <text> (--name '' clears it)");
                            2
                        }
                        // Refused HERE too, with the same limit and the same reason, so a name
                        // that cannot land fails at the keyboard rather than becoming a log line
                        // the caller never reads.
                        //
                        // `--oversize-anyway` skips THIS guard and sends it regardless, so the
                        // ENGINE's refusal can be tested through the ordinary client. Without it
                        // the only oversize name the engine ever sees comes from a hand-rolled
                        // ring writer, and "the engine refuses it" would be an untested claim
                        // resting on a check in the wrong process — every other client, the web
                        // UI included, goes straight to the ring.
                        Some(n)
                            if n.len() >= daw_bridge::layout::UI_SAMPLER_SLOT_NAME_BYTES
                                && !args.iter().any(|a| a == "--oversize-anyway") =>
                        {
                            eprintln!(
                                "daw-cli: name is {} bytes; the published field holds {} (the engine refuses rather than truncating)",
                                n.len(),
                                daw_bridge::layout::UI_SAMPLER_SLOT_NAME_BYTES - 1);
                            2
                        }
                        Some(n) => {
                            let bytes = n.as_bytes();
                            let header = daw_bridge::layout::UiSamplerSlotNameHeader {
                                command_type: UiCommandType::SamplerSetSlotName as u16,
                                device_id: device as u16,
                                track_id: track,
                                slot_id: slot as u16,
                                name_bytes: bytes.len() as u16,
                            };
                            let mut buf = Vec::with_capacity(12 + bytes.len());
                            buf.extend_from_slice(unsafe {
                                std::slice::from_raw_parts(
                                    &header as *const daw_bridge::layout::UiSamplerSlotNameHeader
                                        as *const u8,
                                    std::mem::size_of::<
                                        daw_bridge::layout::UiSamplerSlotNameHeader,
                                    >(),
                                )
                            });
                            buf.extend_from_slice(bytes);
                            let journal_at = sampler_journal_mark();
                            match handle.send_bulk(&buf) {
                                Ok(()) => {
                                    report_sampler_outcome("sampler-slot-name", "sampler_set_slot_name", track, journal_at, &format!(", \"device\": {device}, \"slot\": {slot}, \"name\": {n:?}"))
                                }
                                Err(err) => { eprintln!("daw-cli: {err}"); 1 }
                            }
                        }
                    }
                }
                // A CLIP'S NAME OR ITS AUDIO SOURCE PATH (opcode 98), over the bulk carrier.
                // Neither fits the 40-byte ring payload, which is the only reason either was
                // unreachable — they were the last two GAPs in persisted_field_reach.
                //
                // SHAPE HERE, DOMAIN IN THE ENGINE. This refuses a missing flag and a name that
                // cannot fit the published field; whether the clip exists, is audio, or the path
                // resolves is the engine's to answer, because daw-cli is not the only producer —
                // the web UI's sidecar writes the ring directly and never runs this binary.
                Some(&"clip-name") | Some(&"clip-source") => {
                    let is_name = rest.first() == Some(&"clip-name");
                    let track = flag_u64(&args, "--track", Some(0)).unwrap_or(0) as u32;
                    // REQUIRED, not defaulted to 0. An absent --clip falling back to clip 0 would
                    // rename whichever clip happens to be there on a typo, which is the same
                    // silent-wrong-target trap the --name handling below refuses to allow.
                    let clip_arg = flag_u64(&args, "--clip", None).ok();
                    let clip = clip_arg.unwrap_or(0) as u32;
                    let flag = if is_name { "--name" } else { "--path" };
                    // An ABSENT value is an error; an EMPTY --name is a legal clear. Defaulting
                    // the missing case to "" would make a typo'd flag erase a name silently.
                    let text = args
                        .iter()
                        .position(|a| a == flag)
                        .and_then(|i| args.get(i + 1))
                        .map(String::as_str);
                    // Defaults to the track's CURRENT clip version, so an ordinary edit does not
                    // have to know about the gate; --base-version is there to stamp a stale one
                    // deliberately and see the engine refuse it.
                    let base = flag_u64(&args, "--base-version", None)
                        .map(|v| v as u32)
                        .unwrap_or_else(|_| handle.clip_version_for_track(track));
                    match text {
                        // Checked before the text, so a call missing both flags names the
                        // addressing problem first — a rename that cannot say WHICH clip is not
                        // a rename with a missing name.
                        _ if clip_arg.is_none() => {
                            eprintln!("daw-cli: {} needs --clip <id>",
                                      if is_name { "clip-name" } else { "clip-source" });
                            2
                        }
                        None => {
                            eprintln!("daw-cli: {} needs {flag} <text>",
                                      if is_name { "clip-name" } else { "clip-source" });
                            2
                        }
                        // Same limit and same reason as the engine's, so a name that cannot land
                        // fails at the keyboard instead of becoming a log line nobody reads.
                        // `--oversize-anyway` skips this and sends it regardless, so the ENGINE's
                        // refusal is reachable through the ordinary client — without it, the only
                        // oversize name the engine ever sees comes from a hand-rolled ring writer
                        // and "the engine refuses it" is an untested claim about another process.
                        Some(t)
                            if is_name
                                && t.len() >= daw_bridge::layout::UI_CLIP_EXTENT_NAME_BYTES
                                && !args.iter().any(|a| a == "--oversize-anyway") =>
                        {
                            eprintln!(
                                "daw-cli: name is {} bytes; the published field holds {} (the engine refuses rather than truncating)",
                                t.len(),
                                daw_bridge::layout::UI_CLIP_EXTENT_NAME_BYTES - 1);
                            2
                        }
                        Some(t) => {
                            let bytes = t.as_bytes();
                            let header = daw_bridge::layout::UiClipTextHeader {
                                command_type: UiCommandType::SetClipText as u16,
                                field: if is_name {
                                    daw_bridge::layout::CLIP_TEXT_FIELD_NAME
                                } else {
                                    daw_bridge::layout::CLIP_TEXT_FIELD_SOURCE_PATH
                                },
                                track_id: track,
                                clip_id: clip,
                                text_bytes: bytes.len() as u32,
                                base_version: base,
                            };
                            let mut buf = Vec::with_capacity(20 + bytes.len());
                            buf.extend_from_slice(unsafe {
                                std::slice::from_raw_parts(
                                    &header as *const daw_bridge::layout::UiClipTextHeader
                                        as *const u8,
                                    std::mem::size_of::<daw_bridge::layout::UiClipTextHeader>(),
                                )
                            });
                            buf.extend_from_slice(bytes);
                            match handle.send_bulk(&buf) {
                                Ok(()) => {
                                    let field = if is_name { "name" } else { "source_path" };
                                    println!("{{ \"sent\": \"clip-text\", \"track\": {track}, \"clip\": {clip}, \"field\": {field:?}, \"text\": {t:?}, \"base_version\": {base} }}");
                                    0
                                }
                                Err(err) => { eprintln!("daw-cli: {err}"); 1 }
                            }
                        }
                    }
                }
                Some(&"sampler-slot") => {
                    let track = flag_u64(&args, "--track", Some(0)).unwrap_or(0) as u32;
                    let device = flag_u64(&args, "--device", Some(0)).unwrap_or(0) as u32;
                    let slot = flag_u64(&args, "--slot", Some(0)).unwrap_or(0) as u32;
                    let field_arg = args
                        .iter()
                        .position(|a| a == "--field")
                        .and_then(|i| args.get(i + 1))
                        .map(String::as_str)
                        .unwrap_or("");
                    let value = args
                        .iter()
                        .position(|a| a == "--value")
                        .and_then(|i| args.get(i + 1))
                        .and_then(|v| v.parse::<i32>().ok());
                    let field = SAMPLER_SLOT_FIELDS
                        .iter()
                        .find(|(n, _)| *n == field_arg)
                        .map(|(_, id)| *id);
                    match (field, value, slot) {
                        // A field NAME rather than a number, and an unknown one LISTS the set
                        // rather than doing nothing — a caller who mistypes should not have to
                        // read the source to find out what is available.
                        (None, _, _) => {
                            let names: Vec<&str> =
                                SAMPLER_SLOT_FIELDS.iter().map(|(n, _)| *n).collect();
                            eprintln!("daw-cli: --field must be one of: {}", names.join(", "));
                            2
                        }
                        (_, None, _) => {
                            eprintln!("daw-cli: sampler-slot needs --value <int>");
                            2
                        }
                        (_, _, 0) => {
                            eprintln!("daw-cli: sampler-slot needs --slot <id> (ids start at 1)");
                            2
                        }
                        (Some(field), Some(value), slot) => {
                            let payload = UiSamplerSetSlotPayload {
                                command_type: UiCommandType::SamplerSetSlot as u16,
                                field,
                                track_id: track,
                                device_id: device,
                                slot_id: slot,
                                value,
                                reserved: [0; 20],
                            };
                            let journal_at = sampler_journal_mark();
                            match handle.send_sampler_set_slot(payload) {
                                Ok(()) => report_sampler_outcome(
                                    "sampler-slot", "sampler_set_slot", track, journal_at,
                                    &format!(", \"device\": {device}, \"slot\": {slot}, \"field\": {field_arg:?}, \"value\": {value}")),
                                Err(err) => {
                                    eprintln!("daw-cli: {err}");
                                    1
                                }
                            }
                        }
                    }
                }
                Some(&"sampler-slice") => {
                    let track = flag_u64(&args, "--track", Some(0)).unwrap_or(0) as u32;
                    let device = flag_u64(&args, "--device", Some(0)).unwrap_or(0) as u32;
                    let source = flag_u64(&args, "--source", Some(1)).unwrap_or(1) as u32;
                    let sensitivity = flag_u64(&args, "--sensitivity", Some(500)).unwrap_or(500) as u32;
                    let count = flag_u64(&args, "--count", Some(16)).unwrap_or(16) as u32;
                    let max = flag_u64(&args, "--max", Some(64)).unwrap_or(64) as u32;
                    let snap = flag_u64(&args, "--snap", Some(0)).unwrap_or(0) as u32;
                    let base = flag_u64(&args, "--base-key", Some(36)).unwrap_or(36) as u8;
                    // SLOTS ON BY DEFAULT. A slice set with no slots is a cut nothing plays,
                    // which is a surprising thing to be handed when you asked for a break;
                    // re-cutting the markers without disturbing the slots is the rarer intent
                    // and now says so with --no-slots. --slots is still accepted so existing
                    // scripts keep working. The web-UI's slice verb defaults the same way, and
                    // two surfaces onto one command disagreeing about its default is its own
                    // small bug.
                    let make_slots = !args.iter().any(|a| a == "--no-slots");
                    let mode_arg = args
                        .iter()
                        .position(|a| a == "--mode")
                        .and_then(|i| args.get(i + 1))
                        .map(String::as_str)
                        .unwrap_or("transient");
                    let mode = match mode_arg {
                        "transient" => Some(SAMPLER_SLICE_TRANSIENT),
                        "equal" => Some(SAMPLER_SLICE_EQUAL),
                        "clear" => Some(SAMPLER_SLICE_CLEAR),
                        _ => None,
                    };
                    match mode {
                        None => {
                            eprintln!("daw-cli: --mode must be transient, equal or clear");
                            2
                        }
                        Some(mode) => {
                            let payload = UiSamplerSlicePayload {
                                command_type: UiCommandType::SamplerSlice as u16,
                                mode,
                                track_id: track,
                                device_id: device,
                                source_local_id: source,
                                sensitivity,
                                count,
                                max_slices: max,
                                snap_nanoticks: snap,
                                make_slots: if make_slots { 1 } else { 0 },
                                slot_base_key: base,
                                reserved: [0; 6],
                            };
                            let journal_at = sampler_journal_mark();
                            match handle.send_sampler_slice(payload) {
                                Ok(()) => {
                                    report_sampler_outcome("sampler-slice", "sampler_slice", track, journal_at, &format!(", \"source\": {source}, \"mode\": {mode_arg:?}, \"slots\": {make_slots}"))
                                }
                                Err(err) => {
                                    eprintln!("daw-cli: {err}");
                                    1
                                }
                            }
                        }
                    }
                }
                Some(&"sampler-env-draw") => {
                    use daw_bridge::layout as L;
                    // The PENCIL. Goes over the bulk carrier because N points do not fit in the
                    // ring's 40-byte payload — the reason opcode 83 exists.
                    let track = flag_u64(&args, "--track", Some(0)).unwrap_or(0) as u32;
                    let device = flag_u64(&args, "--device", Some(0)).unwrap_or(0) as u32;
                    let mod_set = flag_u64(&args, "--mod-set", Some(0)).unwrap_or(0) as u32;
                    let rate = flag_u64(&args, "--rate", Some(1000)).unwrap_or(1000) as u16;
                    let release_fade = flag_u64(&args, "--release-fade", Some(0)).unwrap_or(0) as u32;
                    // WHICH DOMAIN. Defaults to the amp envelope, which is what every caller
                    // meant before other targets were reachable.
                    let target = match flag(&args, "--target").as_deref() {
                        None | Some("amp") | Some("volume") | Some("vol") => 0u8,
                        Some("pan") | Some("panning") => 1u8,
                        Some("pitch") => 2u8,
                        Some("cutoff") | Some("filter") => 3u8,
                        Some("res") | Some("resonance") => 4u8,
                        Some(other) => {
                            eprintln!("daw-cli: --target expects amp|pan|pitch|cutoff|res, got {other:?}");
                            std::process::exit(2);
                        }
                    };
                    let sync = args.iter().any(|a| a == "--sync");
                    let pair = |key: &str| -> (u8, u8) {
                        match flag(&args, key) {
                            Some(raw) => {
                                let mut it = raw.split(',');
                                let a = it.next().and_then(|x| x.trim().parse::<u8>().ok());
                                let b = it.next().and_then(|x| x.trim().parse::<u8>().ok());
                                match (a, b) {
                                    (Some(a), Some(b)) => (a, b),
                                    _ => (255, 255),
                                }
                            }
                            None => (255, 255),
                        }
                    };
                    let (sus_a, sus_b) = pair("--sustain-loop");
                    let (rel_a, rel_b) = pair("--release-loop");
                    let points_raw = flag(&args, "--points").unwrap_or_default();
                    let mut pts: Vec<L::UiEnvPointWire> = Vec::new();
                    let mut bad: Option<String> = None;
                    for spec in points_raw.split(';').filter(|x| !x.trim().is_empty()) {
                        let f: Vec<&str> = spec.split(',').map(|x| x.trim()).collect();
                        if f.len() < 2 {
                            bad = Some(format!("point {spec:?} needs at least time,value"));
                            break;
                        }
                        let t = f[0].parse::<u32>();
                        let v = f[1].parse::<i16>();
                        let tension = if f.len() > 2 { f[2].parse::<i8>().unwrap_or(0) } else { 0 };
                        let step = if f.len() > 3 { f[3].parse::<u8>().unwrap_or(0) } else { 0 };
                        match (t, v) {
                            (Ok(t), Ok(v)) => pts.push(L::UiEnvPointWire {
                                time: t,
                                value_milli: v,
                                tension,
                                flags: if step != 0 { 1 } else { 0 },
                            }),
                            _ => {
                                bad = Some(format!("point {spec:?} is not time,value"));
                                break;
                            }
                        }
                    }
                    if let Some(err) = bad {
                        eprintln!("daw-cli: {err}");
                        2
                    } else if pts.len() < 2 {
                        // Refused, not padded: one point is not a shape, and inventing a second
                        // would be the tool deciding what the envelope means.
                        eprintln!("daw-cli: --points needs at least two points, e.g. --points 0,0;300000,1000");
                        2
                    } else {
                        let header = L::UiSamplerEnvPointsHeader {
                            command_type: UiCommandType::SamplerSetEnvelopePoints as u16,
                            flags: SAMPLER_ENV_BY_TARGET,
                            track_id: track,
                            device_id: device,
                            mod_set_id: mod_set,
                            modulator_id: 0,
                            time_base: if sync { 1 } else { 0 },
                            target,
                            rate_milli: rate,
                            point_count: pts.len() as u16,
                            sustain_loop_start: sus_a,
                            sustain_loop_end: sus_b,
                            release_loop_start: rel_a,
                            release_loop_end: rel_b,
                            release_fade,
                        };
                        let mut buf = Vec::with_capacity(32 + pts.len() * 8);
                        buf.extend_from_slice(unsafe {
                            std::slice::from_raw_parts(
                                &header as *const L::UiSamplerEnvPointsHeader as *const u8,
                                std::mem::size_of::<L::UiSamplerEnvPointsHeader>(),
                            )
                        });
                        for p in &pts {
                            buf.extend_from_slice(unsafe {
                                std::slice::from_raw_parts(
                                    p as *const L::UiEnvPointWire as *const u8,
                                    std::mem::size_of::<L::UiEnvPointWire>(),
                                )
                            });
                        }
                        // The stream id is send_bulk's to pick — see its comment. This used
                        // to derive one from the pid here, which is constant for a process
                        // sending twice and would have interleaved two draws into one buffer.
                        let journal_at = sampler_journal_mark();
                        match handle.send_bulk(&buf) {
                            Ok(()) => {
                                let n = pts.len();
                                let bytes = buf.len();
                                report_sampler_outcome("sampler-env-draw", "sampler_set_envelope_points", track, journal_at, &format!(", \"points\": {n}, \"bytes\": {bytes}"))
                            }
                            Err(err) => {
                                eprintln!("daw-cli: {err}");
                                1
                            }
                        }
                    }
                }
                // VINTAGE: bit depth and rate reduction on a mod set, applied BEFORE the
                // filter. Both off by default and independently settable — see the flags.
                Some(&"sampler-vintage") => {
                    let track = flag_u64(&args, "--track", Some(0)).unwrap_or(0) as u32;
                    let device = flag_u64(&args, "--device", Some(0)).unwrap_or(0) as u32;
                    let mod_set = flag_u64(&args, "--mod-set", Some(0)).unwrap_or(0) as u32;
                    // ABSENT IS NOT ZERO, the same rule sampler-filter follows: zero is a legal
                    // value for both (it means OFF), so "leave it alone" needs a flag rather than
                    // being inferred from the value. Otherwise setting the rate would silently
                    // restore full bit depth.
                    let mut flags = 0u16;
                    // 0..16 IS THE ENGINE'S RULE, and it used to be checked here too. Deleting the
                    // ENGINE's guard left sampler_vintage_check passing, because this surface
                    // refused first — the same blind spot found on audio-clip and shared by
                    // clip-grid and lines-per-beat. The engine's copy is the one the web UI's
                    // sidecar meets, so it is the one worth testing; refusals are named in
                    // sampler.vintage_rejected with a reason.
                    //
                    // 255 rather than 16 here, and that is not the range creeping back: the wire
                    // field is a u8, so a larger number would be TRUNCATED into a different, legal
                    // bit depth and the engine would accept a value the caller never typed. That
                    // is a shape rule about the payload, not a domain rule about the sound — the
                    // same reason --rate still checks 65535 against its u16.
                    let bits = match flag_u64(&args, "--bits", None).ok() {
                        Some(v) => {
                            if v > 255 {
                                eprintln!("daw-cli: --bits does not fit the payload's byte, got \
                                           {v} — refused rather than truncated into a bit depth \
                                           you did not ask for");
                                std::process::exit(2);
                            }
                            flags |= daw_bridge::layout::SAMPLER_VINTAGE_SET_BITS;
                            v as u8
                        }
                        None => 0u8,
                    };
                    let rate = match flag_u64(&args, "--rate", None).ok() {
                        Some(v) => {
                            if v > 65535 {
                                eprintln!("daw-cli: --rate does not fit the payload's u16, got \
                                           {v} — refused rather than truncated");
                                std::process::exit(2);
                            }
                            flags |= daw_bridge::layout::SAMPLER_VINTAGE_SET_RATE;
                            v as u16
                        }
                        None => 0u16,
                    };
                    if flags == 0 {
                        eprintln!("daw-cli: sampler-vintage needs --bits N and/or --rate HZ (0 turns one off)");
                        std::process::exit(2);
                    }
                    let payload = daw_bridge::layout::UiSamplerVintagePayload {
                        command_type: UiCommandType::SamplerSetVintage as u16,
                        flags,
                        track_id: track,
                        device_id: device,
                        mod_set_id: mod_set,
                        bit_depth: bits,
                        reserved0: 0,
                        rate_hz: rate,
                        reserved1: [0; 5],
                    };
                    let journal_at = sampler_journal_mark();
                    match handle.send_sampler_vintage(payload) {
                        Ok(()) => {
                            report_sampler_outcome("sampler-vintage", "sampler_set_vintage", track, journal_at, &format!(", \"device\": {device}, \"mod_set\": {mod_set}, \"bits\": {bits}, \"rate_hz\": {rate}"))
                        }
                        Err(err) => { eprintln!("daw-cli: {err}"); 1 }
                    }
                }
                Some(&"sampler-filter") => {
                    // THE FIELD NOTHING COULD WRITE. modSet.filterType was read at the kit
                    // publish site and written nowhere, so a cutoff or resonance envelope built
                    // through this CLI always modulated a filter that was off.
                    let track = flag_u64(&args, "--track", Some(0)).unwrap_or(0) as u32;
                    let device = flag_u64(&args, "--device", Some(0)).unwrap_or(0) as u32;
                    let mod_set = flag_u64(&args, "--mod-set", Some(0)).unwrap_or(0) as u32;
                    let filter_type = match flag(&args, "--type").as_deref() {
                        Some("off") | Some("none") | None => 0u8,
                        Some("lp12") | Some("lp") => 1u8,
                        Some("lp24") => 2u8,
                        Some("hp") | Some("hp12") => 3u8,
                        Some("bp") | Some("bp12") => 4u8,
                        Some(other) => {
                            eprintln!("daw-cli: --type expects off|lp12|lp24|hp|bp, got {other:?}");
                            std::process::exit(2);
                        }
                    };
                    // ABSENT IS NOT ZERO. Zero is a legal cutoff, so "leave it alone" has to be
                    // carried by a flag rather than inferred from the value — otherwise changing
                    // the filter TYPE would silently slam the cutoff shut.
                    let mut flags = 0u16;
                    let cutoff = match flag_u64(&args, "--cutoff", None).ok() {
                        Some(v) => { flags |= SAMPLER_FILTER_SET_CUTOFF; v.min(1000) as u16 }
                        None => 0,
                    };
                    let resonance = match flag_u64(&args, "--resonance", None).ok() {
                        Some(v) => { flags |= SAMPLER_FILTER_SET_RESONANCE; v.min(1000) as u16 }
                        None => 0,
                    };
                    let payload = UiSamplerFilterPayload {
                        command_type: UiCommandType::SamplerSetFilter as u16,
                        flags,
                        track_id: track,
                        device_id: device,
                        mod_set_id: mod_set,
                        filter_type,
                        reserved0: 0,
                        cutoff_milli: cutoff,
                        resonance_milli: resonance,
                        reserved1: 0,
                        reserved2: [0; 4],
                    };
                    let journal_at = sampler_journal_mark();
                    match handle.send_sampler_filter(payload) {
                        Ok(()) => {
                            report_sampler_outcome("sampler-filter", "sampler_set_filter", track, journal_at, &format!(", \"mod_set\": {mod_set}, \"type\": {filter_type}, \"cutoff\": {cutoff}, \"resonance\": {resonance}"))
                        }
                        Err(err) => {
                            eprintln!("daw-cli: {err}");
                            1
                        }
                    }
                }
                Some(&"sampler-lfo") => {
                    use daw_bridge::layout as L;
                    let track = flag_u64(&args, "--track", Some(0)).unwrap_or(0) as u32;
                    let device = flag_u64(&args, "--device", Some(0)).unwrap_or(0) as u32;
                    let mod_set = flag_u64(&args, "--mod-set", Some(0)).unwrap_or(0) as u32;
                    let amount = flag_u64(&args, "--amount", Some(1000)).unwrap_or(1000) as i16;
                    let fl = |key: &str, dflt: f32| -> f32 {
                        flag(&args, key).and_then(|r| r.parse::<f32>().ok()).unwrap_or(dflt)
                    };
                    let target = match flag(&args, "--target").as_deref() {
                        None | Some("amp") | Some("volume") | Some("vol") => 0u8,
                        Some("pan") | Some("panning") => 1u8,
                        Some("pitch") => 2u8,
                        Some("cutoff") | Some("filter") => 3u8,
                        Some("res") | Some("resonance") => 4u8,
                        Some(other) => {
                            eprintln!("daw-cli: --target expects amp|pan|pitch|cutoff|res, got {other:?}");
                            std::process::exit(2);
                        }
                    };
                    let payload = L::UiSamplerLfoPayload {
                        command_type: UiCommandType::SamplerSetLfo as u16,
                        flags: SAMPLER_ENV_BY_TARGET,
                        track_id: track,
                        device_id: device,
                        mod_set_id: mod_set,
                        modulator_id: 0,
                        target,
                        reserved: 0,
                        frequency_hz: fl("--hz", 1.0),
                        depth: fl("--depth", 1.0),
                        bias: fl("--bias", 0.0),
                        phase_offset: fl("--phase", 0.0),
                        depth_milli: amount,
                        reserved2: 0,
                    };
                    let journal_at = sampler_journal_mark();
                    match handle.send_sampler_lfo(payload) {
                        Ok(()) => {
                            report_sampler_outcome("sampler-lfo", "sampler_set_lfo", track, journal_at, &format!(", \"target\": {target}"))
                        }
                        Err(err) => {
                            eprintln!("daw-cli: {err}");
                            1
                        }
                    }
                }
                Some(&"sampler-env") => {
                    // Times are in the modulator's own unit: microseconds by default, nanoticks
                    // with --sync. The unit travels WITH the numbers, so "300 ms attack" cannot
                    // silently become 300 nanoticks against a mod set someone switched to sync.
                    let track = flag_u64(&args, "--track", Some(0)).unwrap_or(0) as u32;
                    let device = flag_u64(&args, "--device", Some(0)).unwrap_or(0) as u32;
                    let mod_set = flag_u64(&args, "--mod-set", Some(0)).unwrap_or(0) as u32;
                    let modulator = flag_u64(&args, "--modulator", Some(0)).unwrap_or(0) as u16;
                    let attack = flag_u64(&args, "--attack", Some(0)).unwrap_or(0) as u32;
                    let decay = flag_u64(&args, "--decay", Some(0)).unwrap_or(0) as u32;
                    let release = flag_u64(&args, "--release", Some(0)).unwrap_or(0) as u32;
                    let sustain = flag_u64(&args, "--sustain", Some(1000)).unwrap_or(1000) as i16;
                    let rate = flag_u64(&args, "--rate", Some(1000)).unwrap_or(1000) as u16;
                    let depth = flag_u64(&args, "--depth", Some(1000)).unwrap_or(1000) as i16;
                    // WHICH DOMAIN. Defaults to the amp envelope, which is what every caller
                    // meant before other targets were reachable.
                    let target = match flag(&args, "--target").as_deref() {
                        None | Some("amp") | Some("volume") | Some("vol") => 0u8,
                        Some("pan") | Some("panning") => 1u8,
                        Some("pitch") => 2u8,
                        Some("cutoff") | Some("filter") => 3u8,
                        Some("res") | Some("resonance") => 4u8,
                        Some(other) => {
                            eprintln!("daw-cli: --target expects amp|pan|pitch|cutoff|res, got {other:?}");
                            std::process::exit(2);
                        }
                    };
                    let sync = args.iter().any(|a| a == "--sync");
                    // --amp is the default: naming a modulator by id is the exception, and a
                    // caller who has not read the mod set has no id to name.
                    let amp = modulator == 0 || args.iter().any(|a| a == "--amp");
                    let payload = UiSamplerEnvelopePayload {
                        command_type: UiCommandType::SamplerSetEnvelope as u16,
                        flags: if amp { SAMPLER_ENV_BY_TARGET } else { 0 },
                        track_id: track,
                        device_id: device,
                        mod_set_id: mod_set,
                        modulator_id: modulator,
                        time_base: if sync { 1 } else { 0 },
                        reserved1: 0,
                        attack,
                        decay,
                        release,
                        sustain_milli: sustain,
                        rate_milli: rate,
                        target,
                        reserved2: 0,
                        depth_milli: depth,
                    };
                    let journal_at = sampler_journal_mark();
                    match handle.send_sampler_envelope(payload) {
                        Ok(()) => {
                            report_sampler_outcome("sampler-env", "sampler_set_envelope", track, journal_at, &format!(", \"attack\": {attack}, \"decay\": {decay}, \"sustain\": {sustain}, \"release\": {release}, \"sync\": {sync}"))
                        }
                        Err(err) => {
                            eprintln!("daw-cli: {err}");
                            1
                        }
                    }
                }
                Some(&"set-row-ops") => {
                    // ONLY the ops named on the command line are touched. --clear names ops to
                    // REMOVE. That is the mask: a flag absent leaves the op alone, a flag present
                    // sets it, --clear zeroes it. Without the distinction there is no way to drop
                    // one op from a note without restating the other four.
                    let track = flag_u64(&args, "--track", Some(0)).unwrap_or(0) as u32;
                    let clip = flag_u64(&args, "--clip", Some(0)).unwrap_or(0) as u32;
                    let note = flag_u64(&args, "--note", Some(0)).unwrap_or(0);
                    let clear_arg = args
                        .iter()
                        .position(|a| a == "--clear")
                        .and_then(|i| args.get(i + 1))
                        .cloned()
                        .unwrap_or_default();
                    let mut mask: u16 = 0;
                    let mut retrigger: u8 = 0;
                    let mut probability: u8 = 0;
                    let mut sound: u16 = 0;
                    let mut sound_offset: u16 = 0;
                    let mut delay_nanoticks: u32 = 0;
                    let mut retrig_ramp: i8 = 0;
                    let mut trig_condition: u8 = 0;
                    let mut parse_error: Option<String> = None;
                    // PRESENCE is what sets the mask bit, so a value must be read with `flag`
                    // (was it given?) rather than flag_u64 (what is it, or a default?). A parse
                    // failure is REFUSED rather than defaulted: "--ret banana" silently becoming
                    // "--ret 0" would clear an op the caller was trying to set.
                    let mut take = |key: &str, bit: u16| -> Option<u64> {
                        let raw = flag(&args, key)?;
                        match raw.parse::<u64>() {
                            Ok(v) => {
                                mask |= bit;
                                Some(v)
                            }
                            Err(_) => {
                                parse_error = Some(format!("{key} expects a number, got {raw:?}"));
                                None
                            }
                        }
                    };
                    if let Some(v) = take("--ret", ROW_OP_MASK_RETRIGGER) {
                        retrigger = v as u8;
                    }
                    if let Some(v) = take("--prob", ROW_OP_MASK_PROBABILITY) {
                        probability = v as u8;
                    }
                    if let Some(v) = take("--sound", ROW_OP_MASK_SOUND) {
                        sound = v as u16;
                    }
                    if let Some(v) = take("--offset", ROW_OP_MASK_SOUND_OFFSET) {
                        sound_offset = v as u16;
                    }
                    if let Some(v) = take("--delay", ROW_OP_MASK_DELAY) {
                        delay_nanoticks = v as u32;
                    }
                    // THE RAMP IS SIGNED, so it cannot go through `take`, which parses u64 — and
                    // "--retrig-ramp -60" landing as a parse error that defaulted to 0 would
                    // clear a ramp the caller was setting, which is the exact failure the
                    // presence/parse split above exists to avoid.
                    if let Some(raw) = flag(&args, "--retrig-ramp") {
                        match raw.parse::<i32>() {
                            Ok(v) if (-100..=100).contains(&v) => {
                                mask |= ROW_OP_MASK_RETRIG_RAMP;
                                retrig_ramp = v as i8;
                            }
                            Ok(v) => {
                                parse_error =
                                    Some(format!("--retrig-ramp must be -100..100, got {v}"));
                            }
                            Err(_) => {
                                parse_error = Some(format!(
                                    "--retrig-ramp expects a signed number, got {raw:?}"));
                            }
                        }
                    }
                    // A:B, in the notation people type, not the packed code — the packing is the
                    // wire's business and a caller that had to compute it would be a second
                    // implementation of make_trig_condition waiting to disagree with the first.
                    if let Some(raw) = flag(&args, "--condition") {
                        // The stateful forms have no colon. Checked first so the A:B branch's
                        // error message stays about A:B.
                        if raw == "pre" || raw == "npre" {
                            mask |= ROW_OP_MASK_TRIG_CONDITION;
                            trig_condition = if raw == "pre" {
                                daw_bridge::rowop::TRIG_CONDITION_PRE
                            } else {
                                daw_bridge::rowop::TRIG_CONDITION_NOT_PRE
                            };
                        } else {
                        match raw.split_once(':') {
                            Some((a_text, b_text)) => {
                                match (a_text.parse::<u8>(), b_text.parse::<u8>()) {
                                    (Ok(a), Ok(b)) => {
                                        let code = daw_bridge::rowop::make_trig_condition(a, b);
                                        if code == 0 {
                                            parse_error = Some(format!(
                                                "--condition must be A:B with 1 <= A <= B <= 8, got {raw:?}"));
                                        } else {
                                            mask |= ROW_OP_MASK_TRIG_CONDITION;
                                            trig_condition = code;
                                        }
                                    }
                                    _ => {
                                        parse_error = Some(format!(
                                            "--condition expects numbers either side of the colon, got {raw:?}"));
                                    }
                                }
                            }
                            None => {
                                parse_error = Some(format!(
                                    "--condition needs A:B (e.g. 1:2), or pre/npre — got {raw:?}"));
                            }
                        }
                        }
                    }
                    // --clear names ops to REMOVE: the mask bit is set and the value stays zero.
                    for (name, bit) in [
                        ("ret", ROW_OP_MASK_RETRIGGER),
                        ("prob", ROW_OP_MASK_PROBABILITY),
                        ("sound", ROW_OP_MASK_SOUND),
                        ("offset", ROW_OP_MASK_SOUND_OFFSET),
                        ("delay", ROW_OP_MASK_DELAY),
                        ("ramp", ROW_OP_MASK_RETRIG_RAMP),
                        ("cond", ROW_OP_MASK_TRIG_CONDITION),
                    ] {
                        if clear_arg.split(',').any(|c| c.trim() == name) {
                            mask |= bit;
                            match name {
                                "ret" => retrigger = 0,
                                "prob" => probability = 0,
                                "sound" => sound = 0,
                                "offset" => sound_offset = 0,
                                "ramp" => retrig_ramp = 0,
                                "cond" => trig_condition = 0,
                                _ => delay_nanoticks = 0,
                            }
                        }
                    }
                    if let Some(err) = parse_error {
                        eprintln!("daw-cli: {err}");
                        2
                    } else if note == 0 {
                        eprintln!("daw-cli: set-row-ops needs --note <id>");
                        2
                    } else if mask == 0 {
                        // Refused rather than sent as a no-op: a command that names no op is a
                        // typo, and silently succeeding would report a write that never happened.
                        eprintln!("daw-cli: set-row-ops names no op — pass at least one of --ret --prob --sound --offset --delay --retrig-ramp --condition, or --clear <names>");
                        2
                    } else {
                        let payload = UiSetRowOpsPayload {
                            command_type: UiCommandType::SetRowOps as u16,
                            mask,
                            track_id: track,
                            clip_id: clip,
                            note_id_lo: note as u32,
                            note_id_hi: (note >> 32) as u32,
                            retrig_ramp,
                            trig_condition,
                            delay_nanoticks,
                            sound,
                            sound_offset,
                            retrigger,
                            probability,
                            reserved: [0; 8],
                        };
                        match handle.send_set_row_ops(payload) {
                            Ok(()) => {
                                println!("{{ \"sent\": \"set-row-ops\", \"track\": {track}, \"note\": {note}, \"mask\": {mask} }}");
                                0
                            }
                            Err(err) => {
                                eprintln!("daw-cli: {err}");
                                1
                            }
                        }
                    }
                }
                Some(&"sampler-marker") => {
                    let track = flag_u64(&args, "--track", Some(0)).unwrap_or(0) as u32;
                    let device = flag_u64(&args, "--device", Some(0)).unwrap_or(0) as u32;
                    let source = flag_u64(&args, "--source", Some(1)).unwrap_or(1) as u32;
                    let marker = flag_u64(&args, "--marker", Some(0)).unwrap_or(0) as u32;
                    let frame = flag_u64(&args, "--frame", Some(0)).unwrap_or(0);
                    let op_arg = args
                        .iter()
                        .position(|a| a == "--op")
                        .and_then(|i| args.get(i + 1))
                        .map(String::as_str)
                        .unwrap_or("");
                    let op = match op_arg {
                        "add" => Some(SAMPLER_MARKER_ADD),
                        "move" => Some(SAMPLER_MARKER_MOVE),
                        "remove" => Some(SAMPLER_MARKER_REMOVE),
                        _ => None,
                    };
                    match op {
                        None => {
                            eprintln!("daw-cli: --op must be add, move or remove");
                            2
                        }
                        Some(op) if op != SAMPLER_MARKER_ADD && marker == 0 => {
                            eprintln!("daw-cli: --op {op_arg} needs --marker <id>");
                            2
                        }
                        Some(op) => {
                            let payload = UiSamplerMarkerPayload {
                                command_type: UiCommandType::SamplerMarker as u16,
                                op,
                                track_id: track,
                                device_id: device,
                                source_local_id: source,
                                marker_id: marker,
                                frame,
                                reserved: [0; 8],
                            };
                            let journal_at = sampler_journal_mark();
                            match handle.send_sampler_marker(payload) {
                                Ok(()) => {
                                    report_sampler_outcome("sampler-marker", "sampler_marker", track, journal_at, &format!(", \"source\": {source}, \"op\": {op_arg:?}, \"marker\": {marker}, \"frame\": {frame}"))
                                }
                                Err(err) => {
                                    eprintln!("daw-cli: {err}");
                                    1
                                }
                            }
                        }
                    }
                }
                Some(&"sampler-emit-rows") => {
                    let track = flag_u64(&args, "--track", Some(0)).unwrap_or(0) as u32;
                    let device = flag_u64(&args, "--device", Some(0)).unwrap_or(0) as u32;
                    let source = flag_u64(&args, "--source", Some(1)).unwrap_or(1) as u32;
                    let at = flag_u64(&args, "--at", Some(0)).unwrap_or(0);
                    // 0 = derive each row's length from its slice, which reproduces the break as
                    // recorded. An explicit --step re-fits it to a grid instead.
                    let step = flag_u64(&args, "--step", Some(0)).unwrap_or(0);
                    // A real u8 field here rather than a shared flags word, so an overflow is
                    // only a wrong column and not a changed edit scope — but it is still silent,
                    // and the rule has one owner now.
                    let column = match daw_bridge::layout::edit_column(
                            flag_u64(&args, "--column", Some(0)).unwrap_or(0)) {
                        Ok(c) => c as u8,
                        Err(e) => { eprintln!("daw-cli: {e}"); std::process::exit(2) }
                    };
                    let velocity = flag_u64(&args, "--velocity", Some(100)).unwrap_or(100) as u8;
                    let payload = UiSamplerEmitRowsPayload {
                        command_type: UiCommandType::SamplerEmitRows as u16,
                        flags: 0,
                        track_id: track,
                        device_id: device,
                        source_local_id: source,
                        at_nanotick: at,
                        step_nanoticks: step,
                        column,
                        velocity,
                        reserved: [0; 6],
                    };
                    let journal_at = sampler_journal_mark();
                    match handle.send_sampler_emit_rows(payload) {
                        Ok(()) => {
                            report_sampler_outcome("sampler-emit-rows", "sampler_emit_rows", track, journal_at, &format!(", \"source\": {source}, \"at\": {at}, \"step\": {step}"))
                        }
                        Err(err) => {
                            eprintln!("daw-cli: {err}");
                            1
                        }
                    }
                }
                Some(&"open-editor") => {
                    let track = flag_u64(&args, "--track", Some(0)).unwrap_or(0) as u32;
                    let device = flag_u64(&args, "--device", Some(0)).unwrap_or(0) as u32;
                    match handle.send_command(open_editor_command(track, device)) {
                        Ok(()) => {
                            println!("{{ \"sent\": \"open-editor\", \"track\": {track}, \"device\": {device} }}");
                            0
                        }
                        Err(err) => {
                            eprintln!("daw-cli: {err}");
                            1
                        }
                    }
                }
                Some(&"move-placement")
                | Some(&"remove-placement")
                | Some(&"resize-placement")
                | Some(&"add-placement") => {
                    const UNCHANGED: u64 = u64::MAX;
                    let verb = *rest.first().unwrap();
                    let track = flag_u64(&args, "--track", Some(0)).unwrap_or(0) as u32;
                    let at = flag_u64(&args, "--at", Some(UNCHANGED)).unwrap_or(UNCHANGED);
                    let length =
                        flag_u64(&args, "--length", Some(UNCHANGED)).unwrap_or(UNCHANGED);
                    let (cmd, value0, np) = match verb {
                        "move-placement" => (
                            UiCommandType::MovePlacement,
                            flag_u64(&args, "--placement", Some(0)).unwrap_or(0) as u32,
                            // --to-track T for a cross-track lane drag; omitted = same track.
                            flag_u64(&args, "--to-track", Some(0xFFFF_FFFF)).unwrap_or(0xFFFF_FFFF)
                                as u32,
                        ),
                        "remove-placement" => (
                            UiCommandType::RemovePlacement,
                            flag_u64(&args, "--placement", Some(0)).unwrap_or(0) as u32,
                            0,
                        ),
                        "resize-placement" => (
                            UiCommandType::ResizePlacement,
                            flag_u64(&args, "--placement", Some(0)).unwrap_or(0) as u32,
                            0,
                        ),
                        _ => (
                            UiCommandType::AddPlacement,
                            flag_u64(&args, "--clip", Some(0)).unwrap_or(0) as u32,
                            0,
                        ),
                    };
                    let at = if verb == "move-placement" && at == UNCHANGED { 0 } else { at };
                    match handle
                        .send_command(placement_command(cmd, track, value0, at, length, np))
                    {
                        Ok(()) => {
                            println!("{{ \"sent\": {verb:?} }}");
                            0
                        }
                        Err(err) => {
                            eprintln!("daw-cli: {err}");
                            1
                        }
                    }
                }
                Some(&"notes") => write_notes(&handle, &args),
                // M2.21: an op with no CLI path cannot be scripted, tested from a
                // shell, or driven by an agent — so the registry check
                // (tools/op_registry_check.sh) requires one for every opcode that is
                // not explicitly declared UI-only. These close the cheap half of that
                // gap; they all reuse payload shapes that already had a sender.
                Some(&"remove-device") | Some(&"move-device") => {
                    let removing = rest.first() == Some(&"remove-device");
                    let track = match flag(&args, "--track") {
                        Some(t) if t == "master" => MASTER_TRACK_ID,
                        Some(t) => t.parse::<u32>().unwrap_or(0),
                        None => 0,
                    };
                    let device = flag_u64(&args, "--device", Some(0)).unwrap_or(0) as u32;
                    let index = flag_u64(&args, "--index", Some(0)).unwrap_or(0) as u32;
                    let payload = UiChainCommandPayload {
                        command_type: if removing {
                            UiCommandType::RemoveDevice as u16
                        } else {
                            UiCommandType::MoveDevice as u16
                        },
                        flags: 0,
                        track_id: track,
                        base_version: 0,
                        device_id: device,
                        device_kind: 0,
                        insert_index: index,
                        patcher_node_id: 0,
                        host_slot_index: 0,
                        bypass: 0,
                        reserved: [0u8; 4],
                    };
                    send_chain_reporting(&handle, payload,
                                        if removing { "remove-device" } else { "move-device" },
                                        track, &format!(", \"device\": {device}"))
                }
                Some(&"patcher-node") | Some(&"patcher-unnode") => {
                    let removing = rest.first() == Some(&"patcher-unnode");
                    let track = flag_u64(&args, "--track", Some(0)).unwrap_or(0) as u32;
                    let node_type = if removing {
                        0
                    } else {
                        match flag(&args, "--type") {
                            Some(t) => match parse_node_type(&t) {
                                Ok(v) => v,
                                Err(e) => { eprintln!("daw-cli: {e}"); std::process::exit(2) }
                            },
                            None => { eprintln!("daw-cli: --type is required"); std::process::exit(2) }
                        }
                    };
                    let payload = daw_bridge::layout::UiPatcherGraphCommandPayload {
                        command_type: if removing {
                            UiCommandType::RemovePatcherNode as u16
                        } else {
                            UiCommandType::AddPatcherNode as u16
                        },
                        track_id: track,
                        flags: patcher_device_flags(&args),
                        node_id: flag_u64(&args, "--node", Some(0)).unwrap_or(0) as u32,
                        node_type,
                        ..Default::default()
                    };
                    match handle.send_patcher_graph_command(payload) {
                        Ok(()) => {
                            println!("{{ \"sent\": {:?} }}",
                                     if removing { "patcher-unnode" } else { "patcher-node" });
                            0
                        }
                        Err(err) => { eprintln!("daw-cli: {err}"); 1 }
                    }
                }
                Some(&"patcher-connect") => {
                    use daw_bridge::layout as L;
                    let track = flag_u64(&args, "--track", Some(0)).unwrap_or(0) as u32;
                    let src = match flag_u64(&args, "--src", None) {
                        Ok(v) => v as u32,
                        Err(e) => { eprintln!("daw-cli: {e}"); std::process::exit(2) }
                    };
                    let dst = match flag_u64(&args, "--dst", None) {
                        Ok(v) => v as u32,
                        Err(e) => { eprintln!("daw-cli: {e}"); std::process::exit(2) }
                    };
                    let kind = match flag(&args, "--kind").as_deref().unwrap_or("event") {
                        "event" => L::PATCHER_PORT_EVENT,
                        "audio" => L::PATCHER_PORT_AUDIO,
                        "cv" => L::PATCHER_PORT_CV,
                        other => { eprintln!("daw-cli: --kind: expected event|audio|cv, got {other:?}"); std::process::exit(2) }
                    };
                    let payload = L::UiPatcherGraphCommandPayload {
                        command_type: UiCommandType::ConnectPatcherNodes as u16,
                        // --device WAS ACCEPTED AND SILENTLY IGNORED. This line was missing while
                        // patcher-node, patcher-unnode and patcher-config all had it, so the one
                        // verb that JOINS two nodes was the one that could not say which graph it
                        // meant. Without the flag the command takes the legacy whole-pool path:
                        // the nodes went onto the device's graph and the edge between them went
                        // into the shared pool, which is never saved. The graph reloaded with two
                        // nodes and nothing connecting them, and every surface reported success.
                        //
                        // Found by cli-verbs.mjs, which is the first thing ever to run this verb.
                        // The sidecar sets these flags for every patcher edit including connect,
                        // so daw-cli was the only producer with the hole — a second copy agreeing
                        // on names and differing in behaviour.
                        flags: patcher_device_flags(&args),
                        track_id: track,
                        src_node_id: src,
                        dst_node_id: dst,
                        // Port 1 is the conventional event OUTPUT and 0 the event INPUT
                        // (see kPatcherEventOutputPort / kPatcherEventInputPort).
                        src_port_id: flag_u64(&args, "--src-port", Some(1)).unwrap_or(1) as u32,
                        dst_port_id: flag_u64(&args, "--dst-port", Some(0)).unwrap_or(0) as u32,
                        edge_kind: kind,
                        ..Default::default()
                    };
                    match handle.send_patcher_graph_command(payload) {
                        Ok(()) => { println!("{{ \"sent\": \"patcher-connect\", \"src\": {src}, \"dst\": {dst} }}"); 0 }
                        Err(err) => { eprintln!("daw-cli: {err}"); 1 }
                    }
                }
                Some(&"patcher-config") => {
                    let track = flag_u64(&args, "--track", Some(0)).unwrap_or(0) as u32;
                    let node = match flag_u64(&args, "--node", None) {
                        Ok(v) => v as u32,
                        Err(e) => { eprintln!("daw-cli: {e}"); std::process::exit(2) }
                    };
                    let node_type = match flag(&args, "--type") {
                        Some(t) => match parse_node_type(&t) {
                            Ok(v) => v,
                            Err(e) => { eprintln!("daw-cli: {e}"); std::process::exit(2) }
                        },
                        None => { eprintln!("daw-cli: --type is required"); std::process::exit(2) }
                    };
                    let cfg = match build_node_config(&args, node_type) {
                        Ok(c) => c,
                        Err(e) => { eprintln!("daw-cli: {e}"); std::process::exit(2) }
                    };
                    let payload = daw_bridge::layout::UiPatcherNodeConfigPayload {
                        command_type: UiCommandType::SetPatcherNodeConfig as u16,
                        // --device WAS ACCEPTED AND DROPPED. flags stayed 0 through
                        // ..Default::default(), so every config edit went to the shared pool —
                        // which, since patcher-is-a-device, is not the graph anything renders.
                        // The other patcher verbs have carried this flag since #73; this one was
                        // never brought along, and it reported success the whole time.
                        flags: patcher_device_flags(&args),
                        track_id: track,
                        node_id: node,
                        config_type: node_type,
                        config: cfg,
                        ..Default::default()
                    };
                    match handle.send_patcher_node_config(payload) {
                        Ok(()) => { println!("{{ \"sent\": \"patcher-config\", \"node\": {node} }}"); 0 }
                        Err(err) => { eprintln!("daw-cli: {err}"); 1 }
                    }
                }
                Some(&"patcher-save") => {
                    let name = rest.get(1).copied().unwrap_or("default");
                    let code = send_named(&handle, UiCommandType::SavePatcherPreset, name);
                    if code == 0 {
                        println!("{{ \"sent\": \"patcher-save\", \"name\": {name:?} }}");
                    }
                    code
                }
                // Change an existing link's depth/bias/enabled IN PLACE. Remove+add was the
                // only way, and it changed the id, dropped the uid16 (which silently disables
                // the modulation) and was not atomic — so a depth SLIDER was impossible: a
                // continuous gesture would tear the link down and rebuild it every frame.
                Some(&"mod-depth") => {
                    use daw_bridge::layout as L;
                    let link = match flag_u64(&args, "--link", None) {
                        Ok(v) => v as u32,
                        Err(e) => { eprintln!("daw-cli: {e} (--link is required — this addresses an EXISTING link)"); std::process::exit(2) }
                    };
                    let depth = match flag_f64(&args, "--depth", f64::NAN) {
                        Ok(v) if !v.is_nan() => v as f32,
                        _ => { eprintln!("daw-cli: --depth is required"); std::process::exit(2) }
                    };
                    let enabled = flag_u64(&args, "--enabled", Some(1)).unwrap_or(1) != 0;
                    let payload = L::UiModLinkCommandPayload {
                        command_type: UiCommandType::SetModLinkDepth as u16,
                        // Only bit 10 (enabled) is read for a depth change; the kind bits are
                        // ignored, which is the whole point of the opcode existing.
                        flags: if enabled { 1u16 << 10 } else { 0 },
                        track_id: flag_u64(&args, "--track", Some(0)).unwrap_or(0) as u32,
                        link_id: link,
                        depth,
                        bias: flag_f64(&args, "--bias", 0.0).unwrap_or(0.0) as f32,
                        ..Default::default()
                    };
                    {
                        let (before_len, journal_at) = outcome_marks(&handle);
                        match handle.send_mod_link_command(payload) {
                            Ok(()) => report_outcome_from(
                                &handle, &MOD_FAMILY, "mod-depth", payload.track_id,
                                &format!(", \"link\": {link}, \"depth\": {depth}"), before_len,
                                journal_at),
                            Err(err) => { eprintln!("daw-cli: {err}"); 1 }
                        }
                    }
                }
                Some(&"mod-link") | Some(&"unmod-link") => {
                    let removing = rest.first() == Some(&"unmod-link");
                    match mod_link_command(&args, removing) {
                        Ok(payload) => {
                            let (before_len, journal_at) = outcome_marks(&handle);
                            match handle.send_mod_link_command(payload) {
                                Ok(()) => {
                                    // NEVER print the AUTO sentinel as if it were the id. It read
                                    // back as 4294967295, which a caller would then pass to
                                    // `unmod-link --link` and match nothing. The engine now names
                                    // the id it assigned on the event stream (modlink.added).
                                    let auto =
                                        payload.link_id == daw_bridge::layout::MOD_LINK_ID_AUTO;
                                    let link = if auto {
                                        "\"auto — see modlink.added on the event stream\"".to_string()
                                    } else {
                                        payload.link_id.to_string()
                                    };
                                    report_outcome_from(
                                        &handle, &MOD_FAMILY,
                                        if removing { "unmod-link" } else { "mod-link" },
                                        payload.track_id, &format!(", \"link\": {link}"),
                                        before_len, journal_at)
                                }
                                Err(err) => { eprintln!("daw-cli: {err}"); 1 }
                            }
                        }
                        Err(err) => { eprintln!("daw-cli: {err}"); 2 }
                    }
                }
                Some(&"mod-target") => {
                    let track = flag_u64(&args, "--track", Some(0)).unwrap_or(0) as u32;
                    let link = match flag_u64(&args, "--link", None) {
                        Ok(v) => v as u32,
                        Err(err) => { eprintln!("daw-cli: {err}"); std::process::exit(2) }
                    };
                    let uid = match flag(&args, "--uid16") {
                        Some(raw) => match parse_uid16(&raw) {
                            Ok(u) => u,
                            Err(err) => { eprintln!("daw-cli: {err}"); std::process::exit(2) }
                        },
                        None => { eprintln!("daw-cli: --uid16 is required"); std::process::exit(2) }
                    };
                    let payload = daw_bridge::layout::UiModLinkUid16Payload {
                        command_type: UiCommandType::SetModLinkUid16 as u16,
                        track_id: track,
                        link_id: link,
                        uid16: uid,
                        ..Default::default()
                    };
                    match handle.send_mod_link_uid16(payload) {
                        Ok(()) => { println!("{{ \"sent\": \"mod-target\", \"link\": {link} }}"); 0 }
                        Err(err) => { eprintln!("daw-cli: {err}"); 1 }
                    }
                }
                Some(&"macro") => {
                    let track = flag_u64(&args, "--track", Some(0)).unwrap_or(0) as u32;
                    let device = flag_u64(&args, "--device", Some(0)).unwrap_or(0) as u32;
                    let source = flag_u64(&args, "--source-id", Some(0)).unwrap_or(0) as u32;
                    let value = match flag_f64(&args, "--value", f64::NAN) {
                        Ok(v) if !v.is_nan() => v as f32,
                        _ => { eprintln!("daw-cli: --value is required"); std::process::exit(2) }
                    };
                    let payload = daw_bridge::layout::UiModSourceValuePayload {
                        command_type: UiCommandType::SetModSourceValue as u16,
                        track_id: track,
                        source_device_id: device,
                        source_id: source,
                        value,
                        ..Default::default()
                    };
                    match handle.send_mod_source_value(payload) {
                        Ok(()) => { println!("{{ \"sent\": \"macro\", \"value\": {value} }}"); 0 }
                        Err(err) => { eprintln!("daw-cli: {err}"); 1 }
                    }
                }
                // M3.23 sections. There is no "move a section to bar N": a section's
                // position is derived from the lengths before it, so you change a length
                // or the ORDER and everything after follows.
                Some(&"automation") => {
                    use daw_bridge::layout as L;
                    let track = flag_u64(&args, "--track", Some(0)).unwrap_or(0) as u32;
                    let param = match flag(&args, "--param") {
                        Some(p) if !p.is_empty() => p,
                        _ => { eprintln!("daw-cli: --param is required (the automation clip's id, e.g. index:0)"); std::process::exit(2) }
                    };
                    let tick = match flag_u64(&args, "--nanotick", None) {
                        Ok(v) => v,
                        Err(e) => { eprintln!("daw-cli: {e}"); std::process::exit(2) }
                    };
                    let value = match flag_f64(&args, "--value", f64::NAN) {
                        Ok(v) if !v.is_nan() => v as f32,
                        _ => { eprintln!("daw-cli: --value is required (normalised 0..1)"); std::process::exit(2) }
                    };
                    // REFUSED, not truncated. The wire field is 16 bytes and the read-back
                    // nul-terminates within it, so 15 is the real limit. Silently cutting a
                    // longer id writes to a DIFFERENT lane than the one named — and then reads
                    // back a third thing — with the caller told it succeeded.
                    let param_id = match param_id_bytes(&param) {
                        Ok(v) => v,
                        Err(e) => { eprintln!("daw-cli: {e}"); std::process::exit(2) }
                    };
                    let discrete = args.iter().any(|a| a == "--discrete");
                    let payload = L::UiAutomationPointPayload {
                        command_type: UiCommandType::WriteAutomationPoint as u16,
                        flags: if discrete { L::UI_AUTOMATION_DISCRETE } else { 0 },
                        track_id: track,
                        target_plugin_index: flag_u64(&args, "--device", Some(0xFFFF_FFFF))
                            .unwrap_or(0xFFFF_FFFF) as u32,
                        nanotick_lo: (tick & 0xffff_ffff) as u32,
                        nanotick_hi: (tick >> 32) as u32,
                        value,
                        param_id,
                    };
                    match handle.send_automation_point(payload) {
                        Ok(()) => { println!("{{ \"sent\": \"automation\", \"param\": {param:?}, \"nanotick\": {tick}, \"value\": {value} }}"); 0 }
                        Err(err) => { eprintln!("daw-cli: {err}"); 1 }
                    }
                }
                // THE OTHER DIRECTION OF THE SAME EDIT (opcode 96). Until this, an automation
                // lane was draw-only: 60 creates a point and re-values one, and nothing removed
                // one, so a point written at the wrong tick could only be neutralised by writing
                // another beside it and leaving the mistake in the curve.
                Some(&"delete-automation") | Some(&"move-automation") => {
                    use daw_bridge::layout as L;
                    let moving = rest.first().copied() == Some("move-automation");
                    let track = flag_u64(&args, "--track", Some(0)).unwrap_or(0) as u32;
                    let param = match flag(&args, "--param") {
                        Some(p) if !p.is_empty() => p,
                        _ => { eprintln!("daw-cli: --param is required (the automation clip's id, e.g. index:0)"); std::process::exit(2) }
                    };
                    let param_id = match param_id_bytes(&param) {
                        Ok(v) => v,
                        Err(e) => { eprintln!("daw-cli: {e}"); std::process::exit(2) }
                    };
                    let device = flag_u64(&args, "--device", Some(0xFFFF_FFFF))
                        .unwrap_or(0xFFFF_FFFF) as u32;
                    // `--nanotick` for a delete, `--from` for a move; the move also needs where
                    // it is going and what value to carry there.
                    let from = match flag_u64(&args, if moving { "--from" } else { "--nanotick" }, None) {
                        Ok(v) => v,
                        Err(e) => { eprintln!("daw-cli: {e}"); std::process::exit(2) }
                    };
                    let mut payload = L::UiAutomationPointPayload {
                        command_type: UiCommandType::DeleteAutomationPoint as u16,
                        flags: 0,
                        track_id: track,
                        target_plugin_index: device,
                        nanotick_lo: (from & 0xffff_ffff) as u32,
                        nanotick_hi: (from >> 32) as u32,
                        value: 0.0,
                        param_id,
                    };
                    if let Err(err) = handle.send_automation_point(payload) {
                        eprintln!("daw-cli: {err}");
                        std::process::exit(1);
                    }
                    if !moving {
                        println!("{{ \"sent\": \"delete-automation\", \"param\": {param:?}, \"nanotick\": {from} }}");
                        0
                    } else {
                        // A MOVE IS DELETE THEN WRITE, in that order, and it is two ring entries
                        // rather than one command because the pair does not fit: commandType,
                        // flags, trackId, targetPluginIndex and a 16-byte paramId leave twelve
                        // bytes for two eight-byte ticks. Dropping the device index would fit and
                        // would silently break automation of a specific plugin's parameter.
                        //
                        // The ring is ordered and single-consumer, so from one producer these
                        // arrive in order; the intermediate state is one command wide.
                        let to = match flag_u64(&args, "--to", None) {
                            Ok(v) => v,
                            Err(e) => { eprintln!("daw-cli: {e}"); std::process::exit(2) }
                        };
                        let value = match flag_f64(&args, "--value", f64::NAN) {
                            Ok(v) if !v.is_nan() => v as f32,
                            _ => { eprintln!("daw-cli: move-automation needs --value: the point is re-written at its new tick, and the engine has no way to carry the old value across a delete"); std::process::exit(2) }
                        };
                        payload.command_type = UiCommandType::WriteAutomationPoint as u16;
                        payload.nanotick_lo = (to & 0xffff_ffff) as u32;
                        payload.nanotick_hi = (to >> 32) as u32;
                        payload.value = value;
                        match handle.send_automation_point(payload) {
                            Ok(()) => { println!("{{ \"sent\": \"move-automation\", \"param\": {param:?}, \"from\": {from}, \"to\": {to}, \"value\": {value} }}"); 0 }
                            Err(err) => { eprintln!("daw-cli: {err}"); 1 }
                        }
                    }
                }
                Some(&"revert-overrides") => {
                    let track = flag_u64(&args, "--track", Some(0)).unwrap_or(0) as u32;
                    let placement = match flag_u64(&args, "--placement", None) {
                        Ok(v) => v as u32,
                        Err(e) => { eprintln!("daw-cli: {e}"); std::process::exit(2) }
                    };
                    let mut payload = track_structure_command(
                        UiCommandType::RevertPlacementOverrides, track);
                    payload.value0 = placement;
                    payload.base_version = handle.clip_version_for_track(track);
                    match handle.send_command(payload) {
                        Ok(()) => { println!("{{ \"sent\": \"revert-overrides\", \"placement\": {placement} }}"); 0 }
                        Err(err) => { eprintln!("daw-cli: {err}"); 1 }
                    }
                }
                // M2.57 SCRATCH CLIPS. `fork` gives an agent its own copy to write into and keeps
                // yours as the alternate; `swap` is the A/B; `keep` drops the other once you have
                // decided. What PLAYS is always the placement's clip, so there is no auditioning
                // mode to get out of step with what you hear.
                Some(&"scratch") => {
                    let sub = rest.get(1).copied().unwrap_or("");
                    let cmd = match sub {
                        "fork" => UiCommandType::ForkPlacementClip,
                        "swap" => UiCommandType::SwapPlacementClip,
                        "keep" => UiCommandType::ClearPlacementAlternate,
                        other => {
                            eprintln!("daw-cli: scratch {other:?}: expected fork|swap|keep");
                            std::process::exit(2)
                        }
                    };
                    let placement = match flag_u64(&args, "--placement", None) {
                        Ok(v) => v as u32,
                        Err(e) => {
                            eprintln!("daw-cli: {e} (--placement names the appearance to fork or swap)");
                            std::process::exit(2)
                        }
                    };
                    let payload = track_structure_command(cmd, flag_u64(&args, "--track", Some(0)).unwrap_or(0) as u32);
                    let payload = UiCommandPayload { value0: placement, ..payload };
                    match handle.send_command(payload) {
                        Ok(()) => {
                            println!("{{ \"sent\": \"scratch {sub}\", \"placement\": {placement} }}");
                            0
                        }
                        Err(err) => { eprintln!("daw-cli: {err}"); 1 }
                    }
                }
                Some(&"placement-scope") => {
                    let track = flag_u64(&args, "--track", Some(0)).unwrap_or(0) as u32;
                    let placement = match flag_u64(&args, "--placement", None) {
                        Ok(v) => v as u32,
                        Err(e) => { eprintln!("daw-cli: {e}"); std::process::exit(2) }
                    };
                    // Default ON: the verb exists to MARK an appearance as taking local edits,
                    // so the bare form does the thing its name says. `--on 0` clears it.
                    let on = flag_u64(&args, "--on", Some(1)).unwrap_or(1);
                    let mut payload = track_structure_command(
                        UiCommandType::SetPlacementEditScope, track);
                    payload.value0 = placement;
                    payload.flags = if on != 0 { 1 } else { 0 };
                    match handle.send_command(payload) {
                        Ok(()) => {
                            println!("{{ \"sent\": \"placement-scope\", \"placement\": {placement}, \"local\": {} }}",
                                     on != 0);
                            0
                        }
                        Err(err) => { eprintln!("daw-cli: {err}"); 1 }
                    }
                }
                // v29: MARKER ops. A marker names a tick and moves no material, so these are
                // total — nothing to plan, refuse or undo beyond the list.
                Some(&"marker") => {
                    use daw_bridge::layout as L;
                    let sub = rest.get(1).copied().unwrap_or("");
                    let mut name = [0u8; 20];
                    if let Some(n) = flag(&args, "--name") {
                        let b = n.as_bytes();
                        let len = b.len().min(name.len());
                        name[..len].copy_from_slice(&b[..len]);
                    }
                    let cmd = match sub {
                        "add" => UiCommandType::AddMarker,
                        "remove" => UiCommandType::RemoveMarker,
                        "rename" => UiCommandType::RenameMarker,
                        "move" => UiCommandType::MoveMarker,
                        "color" => UiCommandType::SetMarkerColor,
                        other => {
                            eprintln!("daw-cli: marker {other:?}: expected add|remove|rename|move|color");
                            std::process::exit(2)
                        }
                    };
                    // Required where it is the whole point of the command. A default here would
                    // silently put the marker at tick 0, which looks like a no-op and is not.
                    if matches!(cmd, UiCommandType::AddMarker | UiCommandType::MoveMarker)
                        && flag(&args, "--nanotick").is_none()
                    {
                        eprintln!("daw-cli: --nanotick is required for marker {sub}");
                        std::process::exit(2);
                    }
                    if matches!(cmd, UiCommandType::RenameMarker) && flag(&args, "--name").is_none()
                    {
                        eprintln!("daw-cli: --name is required for marker rename");
                        std::process::exit(2);
                    }
                    // REQUIRED, because the default below is 0 and 0 is BLACK, not "leave it
                    // alone". Every 24-bit value is a legal colour, so an omitted --color cannot
                    // be told from a chosen one once it is on the wire — the same reason this is
                    // its own opcode rather than a flag on rename.
                    if matches!(cmd, UiCommandType::SetMarkerColor)
                        && flag(&args, "--color").is_none()
                    {
                        eprintln!("daw-cli: --color is required for marker color (0 is black, not \"unchanged\")");
                        std::process::exit(2);
                    }
                    if matches!(cmd,
                                UiCommandType::RemoveMarker | UiCommandType::RenameMarker
                                    | UiCommandType::MoveMarker | UiCommandType::SetMarkerColor)
                        && flag(&args, "--id").is_none()
                    {
                        eprintln!("daw-cli: --id is required for marker {sub}");
                        std::process::exit(2);
                    }
                    let tick = flag_u64(&args, "--nanotick", Some(0)).unwrap_or(0);
                    let payload = L::UiMarkerCommandPayload {
                        command_type: cmd as u16,
                        flags: 0,
                        marker_id: flag_u64(&args, "--id", Some(0)).unwrap_or(0) as u32,
                        nanotick_lo: (tick & 0xffff_ffff) as u32,
                        nanotick_hi: (tick >> 32) as u32,
                        color_rgb: flag_u64(&args, "--color", Some(0)).unwrap_or(0) as u32,
                        name,
                    };
                    match handle.send_marker_command(payload) {
                        Ok(()) => {
                            // The engine names the id it assigned on the event stream
                            // (marker.changed); an auto-id add has none to report here.
                            println!("{{ \"sent\": \"marker {sub}\" }}");
                            0
                        }
                        Err(err) => { eprintln!("daw-cli: {err}"); 1 }
                    }
                }
                // v29: the song's METER. This is where a mid-song time signature is authored — a
                // Section's meter was reachable from no command at all.
                Some(&"time-sig") => {
                    use daw_bridge::layout as L;
                    let sig = match flag(&args, "--sig") {
                        Some(v) => v,
                        None => {
                            eprintln!("daw-cli: --sig is required, e.g. --sig 7/8");
                            std::process::exit(2)
                        }
                    };
                    let (num, den) = match sig.split_once('/') {
                        Some((n, d)) => match (n.trim().parse::<u32>(), d.trim().parse::<u32>()) {
                            (Ok(n), Ok(d)) => (n, d),
                            _ => {
                                eprintln!("daw-cli: --sig {sig:?}: expected N/D, e.g. 7/8");
                                std::process::exit(2)
                            }
                        },
                        None => {
                            eprintln!("daw-cli: --sig {sig:?}: expected N/D, e.g. 7/8");
                            std::process::exit(2)
                        }
                    };
                    let flatten = args.iter().any(|a| a == "--flatten");
                    let tick = flag_u64(&args, "--nanotick", Some(0)).unwrap_or(0);
                    let payload = L::UiArrangeTimeCommandPayload {
                        command_type: UiCommandType::SetTimeSignature as u16,
                        flags: if flatten { L::UI_TIME_SIG_FLATTEN } else { 0 },
                        nanotick_lo: (tick & 0xffff_ffff) as u32,
                        nanotick_hi: (tick >> 32) as u32,
                        numerator: num,
                        denominator: den,
                        ..Default::default()
                    };
                    {
                        let (before_len, journal_at) = outcome_marks(&handle);
                        match handle.send_arrange_time_command(payload) {
                            Ok(()) => report_outcome_from(
                                &handle, &GLOBAL_FAMILY, "time-sig", UI_GLOBAL_SCOPE,
                                &format!(", \"nanotick\": {tick}, \"sig\": \"{num}/{den}\", \"flatten\": {flatten}"),
                                before_len, journal_at),
                            Err(err) => { eprintln!("daw-cli: {err}"); 1 }
                        }
                    }
                }
                // v29: THE RIPPLE, as its own command. `--bars` is signed: positive inserts,
                // negative removes. It replaces `section length`, whose gesture ("drag the
                // boundary") this is the honest name for.
                Some(&"time") => {
                    use daw_bridge::layout as L;
                    let sub = rest.get(1).copied().unwrap_or("");
                    if sub != "insert" && sub != "remove" {
                        eprintln!("daw-cli: time {sub:?}: expected insert|remove");
                        std::process::exit(2);
                    }
                    let bars = match flag_u64(&args, "--bars", None) {
                        Ok(v) if v > 0 => v as i32,
                        _ => {
                            eprintln!("daw-cli: --bars is required and must be > 0 (the direction \
comes from insert|remove, so a negative here would be ambiguous)");
                            std::process::exit(2)
                        }
                    };
                    if flag(&args, "--nanotick").is_none() {
                        eprintln!("daw-cli: --nanotick is required — WHERE the time is inserted or \
removed is the whole command");
                        std::process::exit(2);
                    }
                    let tick = flag_u64(&args, "--nanotick", Some(0)).unwrap_or(0);
                    let payload = L::UiArrangeTimeCommandPayload {
                        command_type: UiCommandType::InsertRemoveTime as u16,
                        flags: 0,  // delta is in BARS; the engine knows the meter there
                        nanotick_lo: (tick & 0xffff_ffff) as u32,
                        nanotick_hi: (tick >> 32) as u32,
                        delta: if sub == "insert" { bars } else { -bars },
                        ..Default::default()
                    };
                    {
                        let (before_len, journal_at) = outcome_marks(&handle);
                        match handle.send_arrange_time_command(payload) {
                            Ok(()) => report_outcome_from(
                                &handle, &GLOBAL_FAMILY, &format!("time {sub}"), UI_GLOBAL_SCOPE,
                                &format!(", \"nanotick\": {tick}, \"bars\": {bars}"),
                                before_len, journal_at),
                            Err(err) => { eprintln!("daw-cli: {err}"); 1 }
                        }
                    }
                }
                Some(&"routing") => match routing_command(&args) {
                    Ok(payload) => {
                        let (before_len, journal_at) = outcome_marks(&handle);
                        match handle.send_routing_command(payload) {
                            Ok(()) => report_outcome_from(
                                &handle, &ROUTING_FAMILY, "routing", payload.track_id,
                                ", \"replaced_all_routes\": true", before_len, journal_at),
                            Err(err) => { eprintln!("daw-cli: {err}"); 1 }
                        }
                    }
                    Err(err) => { eprintln!("daw-cli: {err}"); 2 }
                },
                Some(&"stop") => {
                    let payload = track_structure_command(UiCommandType::Stop, 0);
                    match handle.send_command(payload) {
                        Ok(()) => { println!("{{ \"sent\": \"stop\" }}"); 0 }
                        Err(err) => { eprintln!("daw-cli: {err}"); 1 }
                    }
                }
                Some(&"position") => {
                    let tick = match flag_u64(&args, "--nanotick", None) {
                        Ok(v) => v,
                        Err(err) => { eprintln!("daw-cli: {err}"); std::process::exit(2) }
                    };
                    let mut payload = track_structure_command(UiCommandType::SetPosition, 0);
                    payload.note_nanotick_lo = (tick & 0xffff_ffff) as u32;
                    payload.note_nanotick_hi = (tick >> 32) as u32;
                    match handle.send_command(payload) {
                        Ok(()) => { println!("{{ \"sent\": \"position\", \"nanotick\": {tick} }}"); 0 }
                        Err(err) => { eprintln!("daw-cli: {err}"); 1 }
                    }
                }
                Some(&"loop") => {
                    let start = flag_u64(&args, "--start", Some(0)).unwrap_or(0);
                    let end = match flag_u64(&args, "--end", None) {
                        Ok(v) => v,
                        Err(err) => { eprintln!("daw-cli: {err}"); std::process::exit(2) }
                    };
                    if end <= start {
                        eprintln!("daw-cli: --end must be after --start");
                        std::process::exit(2);
                    }
                    let mut payload =
                        track_structure_command(UiCommandType::SetLoopRange, 0);
                    payload.note_nanotick_lo = (start & 0xffff_ffff) as u32;
                    payload.note_nanotick_hi = (start >> 32) as u32;
                    payload.note_duration_lo = (end & 0xffff_ffff) as u32;
                    payload.note_duration_hi = (end >> 32) as u32;
                    match handle.send_command(payload) {
                        Ok(()) => { println!("{{ \"sent\": \"loop\", \"start\": {start}, \"end\": {end} }}"); 0 }
                        Err(err) => { eprintln!("daw-cli: {err}"); 1 }
                    }
                }
                Some(&"collapse") => {
                    // The same shape as sound-addressed and harmony-quantize: a per-track
                    // boolean. `collapsed` was persisted, published and restored on load and
                    // settable by nothing, so the fold could be drawn and never set.
                    let track = flag_u64(&args, "--track", Some(0)).unwrap_or(0) as u32;
                    let on = flag_u64(&args, "--on", Some(1)).unwrap_or(1);
                    let mut payload = track_structure_command(
                        UiCommandType::SetTrackCollapsed, track);
                    payload.value0 = if on != 0 { 1 } else { 0 };
                    match handle.send_command(payload) {
                        Ok(()) => { println!("{{ \"sent\": \"collapse\", \"track\": {track}, \"on\": {on} }}"); 0 }
                        Err(err) => { eprintln!("daw-cli: {err}"); 1 }
                    }
                }
                Some(&"clip-grid") => {
                    // A CLIP's own grid — task #43 phase 2. The clip's subdivision and meter are
                    // persisted, published packed into the extent's flags, and drawn BEFORE the
                    // track's, so this is the authoritative one and it had no writer at all.
                    let track = flag_u64(&args, "--track", Some(0)).unwrap_or(0) as u32;
                    let clip = match flag_u64(&args, "--clip", None) {
                        Ok(v) => v as u32,
                        Err(_) => {
                            eprintln!("daw-cli: clip-grid needs --clip <id>");
                            std::process::exit(2);
                        }
                    };
                    // ABSENT IS NOT ZERO, the same rule vintage follows: 0 is the packer's "no
                    // grid on this extent" sentinel, so it cannot double as "leave this alone".
                    //
                    // THE RANGES ARE THE ENGINE'S TO JUDGE, and they used to be checked here too.
                    // That duplicate was not free: with the engine's guard deleted entirely,
                    // clip_grid_check still passed, because this surface refused first and the
                    // engine's copy — the one on the path EVERY producer takes, including the web
                    // UI's sidecar writing to the ring directly — was never reached. Found on
                    // audio-clip's guard and true here for the same reason. So this validates
                    // SHAPE (a --clip, at least one field named) and the engine validates DOMAIN,
                    // naming any refusal in clip.grid_rejected with a reason.
                    let mut flags = 0u16;
                    let mut lines = 0u32;
                    let mut num = 0u32;
                    let mut den = 0u32;
                    if let Ok(v) = flag_u64(&args, "--lines", None) {
                        flags |= daw_bridge::layout::CLIP_GRID_SET_LINES;
                        lines = v as u32;
                    }
                    if let Ok(v) = flag_u64(&args, "--num", None) {
                        flags |= daw_bridge::layout::CLIP_GRID_SET_NUMERATOR;
                        num = v as u32;
                    }
                    if let Ok(v) = flag_u64(&args, "--den", None) {
                        flags |= daw_bridge::layout::CLIP_GRID_SET_DENOMINATOR;
                        den = v as u32;
                    }
                    if flags == 0 {
                        eprintln!("daw-cli: clip-grid needs at least one of --lines --num --den");
                        std::process::exit(2);
                    }
                    let payload = daw_bridge::layout::UiSetClipGridPayload {
                        command_type: UiCommandType::SetClipGrid as u16,
                        flags,
                        track_id: track,
                        clip_id: clip,
                        lines_per_beat: lines,
                        time_sig_numerator: num,
                        time_sig_denominator: den,
                        reserved: [0; 4],
                    };
                    match handle.send_clip_grid(payload) {
                        Ok(()) => {
                            println!("{{ \"sent\": \"clip-grid\", \"track\": {track}, \"clip\": {clip}, \"lines\": {lines}, \"num\": {num}, \"den\": {den} }}");
                            0
                        }
                        Err(err) => { eprintln!("daw-cli: {err}"); 1 }
                    }
                }
                Some(&"audio-clip") => {
                    // The audio clip's in-point, gain and fades. All four persisted, all four
                    // published, all four honoured by the renderer, and written by nothing — an
                    // audio region was read-only from every surface until this.
                    let track = flag_u64(&args, "--track", Some(0)).unwrap_or(0) as u32;
                    let clip = match flag_u64(&args, "--clip", None) {
                        Ok(v) => v as u32,
                        Err(_) => {
                            eprintln!("daw-cli: audio-clip needs --clip <id>");
                            std::process::exit(2);
                        }
                    };
                    // NAMED, not numbered. `--field 1` reads as a clip id at a glance and the
                    // engine cannot tell a mistyped enum from a deliberate one — the same reason
                    // `vintage 0 9 8 2` once read a bit depth as a sample rate.
                    let field_name = match flag(&args, "--field") {
                        Some(v) => v,
                        None => {
                            eprintln!("daw-cli: audio-clip needs --field <start|gain|fade-in|fade-out>");
                            std::process::exit(2);
                        }
                    };
                    let field = match field_name.as_str() {
                        "start" | "source-start" | "source-start-frame" => {
                            daw_bridge::layout::AUDIO_CLIP_FIELD_SOURCE_START_FRAME
                        }
                        "gain" | "gain-millibels" => daw_bridge::layout::AUDIO_CLIP_FIELD_GAIN_MILLIBELS,
                        "fade-in" => daw_bridge::layout::AUDIO_CLIP_FIELD_FADE_IN_NANOTICKS,
                        "fade-out" => daw_bridge::layout::AUDIO_CLIP_FIELD_FADE_OUT_NANOTICKS,
                        other => {
                            eprintln!(
                                "daw-cli: --field {other:?} is not one of start, gain, fade-in, \
                                 fade-out"
                            );
                            std::process::exit(2);
                        }
                    };
                    // ABSENT IS NOT ZERO here either, and the stakes are higher than usual: 0 is a
                    // legal value for every one of these four (unity gain, no fade, the file's
                    // start), so a defaulted --value would be a silent reset that looks like a
                    // successful call.
                    if flag(&args, "--value").is_none() {
                        eprintln!("daw-cli: audio-clip needs --value N (0 is a legal value for \
                                   every field, so it cannot double as 'unset')");
                        std::process::exit(2);
                    }
                    let value = match flag_i64(&args, "--value", 0) {
                        Ok(v) => v,
                        Err(err) => {
                            eprintln!("daw-cli: {err}");
                            std::process::exit(2);
                        }
                    };
                    // THE RANGE IS THE ENGINE'S TO JUDGE, DELIBERATELY, and this is a split from
                    // what `clip-grid` just above does. That command validates its ranges here AND
                    // in the engine, and the consequence showed up the moment its sibling was
                    // tested: with the engine's guard deleted entirely the check still passed,
                    // because the CLI refused first and the engine's copy was never reached. Two
                    // copies of one rule, where only one of them is on the path every producer
                    // takes — the web UI's sidecar writes payloads to the ring directly.
                    //
                    // So the division here is by KIND, not by convenience: this surface validates
                    // SHAPE (a field name it knows, a --value that was actually supplied) and the
                    // engine validates DOMAIN (range, clip kind, existence). A refused command is
                    // named in the engine log as audio_clip.field_rejected with a reason, exactly
                    // like no_such_clip and not_an_audio_clip already are.
                    let payload = daw_bridge::layout::UiAudioClipFieldPayload {
                        command_type: UiCommandType::SetAudioClipField as u16,
                        field,
                        track_id: track,
                        clip_id: clip,
                        reserved0: 0,
                        value,
                        reserved1: [0; 4],
                    };
                    match handle.send_audio_clip_field(payload) {
                        Ok(()) => {
                            println!("{{ \"sent\": \"audio-clip\", \"track\": {track}, \"clip\": {clip}, \"field\": {field_name:?}, \"value\": {value} }}");
                            0
                        }
                        Err(err) => { eprintln!("daw-cli: {err}"); 1 }
                    }
                }
                Some(&"lines-per-beat") => {
                    // The last piece of per-lane grids. `lines_per_beat` was persisted, published
                    // and honoured by the tracker's grid, and settable by nothing — a project
                    // could CARRY a 3-rows-per-beat lane and no surface could MAKE one.
                    let track = flag_u64(&args, "--track", Some(0)).unwrap_or(0) as u32;
                    let lines = match flag_u64(&args, "--lines", None) {
                        Ok(v) => v,
                        Err(_) => {
                            eprintln!("daw-cli: lines-per-beat needs --lines N (1..31)");
                            std::process::exit(2);
                        }
                    };
                    // THE RANGE IS THE ENGINE'S TO JUDGE, and it used to be checked here too. The
                    // check's own comment recorded the consequence honestly: "daw-cli refuses 0
                    // and 32 before anything is sent, so the ENGINE's identical guard cannot be
                    // reached from here and is not covered". A guard nothing in ctest exercises
                    // is a guard that rots, and it is the one every producer meets — the web UI's
                    // sidecar writes to the ring directly and never sees this line. So this
                    // surface validates SHAPE and the engine validates DOMAIN, naming a refusal
                    // in track.lines_per_beat_rejected with a reason.
                    let mut payload = track_structure_command(
                        UiCommandType::SetTrackLinesPerBeat, track);
                    payload.value0 = lines as u32;
                    match handle.send_command(payload) {
                        Ok(()) => {
                            println!("{{ \"sent\": \"lines-per-beat\", \"track\": {track}, \"lines\": {lines} }}");
                            0
                        }
                        Err(err) => { eprintln!("daw-cli: {err}"); 1 }
                    }
                }
                Some(&"note-overlap") => {
                    // CUT-ON-NEXT, OR LET IT RING. The one setting here that decides whether an
                    // EDIT loses data: off, entering a note over a sounding one truncates the
                    // sounding note in the DOCUMENT and the typed length is gone for good.
                    //
                    // If you turn this on and hear no difference, check the sampler slot's `nna`
                    // — it defaults to 0 (Cut) and cuts the previous voice one layer down, which
                    // looks exactly like this flag not working.
                    let track = flag_u64(&args, "--track", Some(0)).unwrap_or(0) as u32;
                    let on = flag_u64(&args, "--on", Some(1)).unwrap_or(1);
                    let mut payload = track_structure_command(
                        UiCommandType::SetTrackAllowNoteOverlap, track);
                    payload.value0 = if on != 0 { 1 } else { 0 };
                    match handle.send_command(payload) {
                        Ok(()) => {
                            println!("{{ \"sent\": \"note-overlap\", \"track\": {track}, \"on\": {on} }}");
                            0
                        }
                        Err(err) => { eprintln!("daw-cli: {err}"); 1 }
                    }
                }
                Some(&"sound-addressed") => {
                    // The same shape as harmony-quantize, because it is the same kind of thing:
                    // a per-track rule about how this track's notes are read.
                    let track = flag_u64(&args, "--track", Some(0)).unwrap_or(0) as u32;
                    let on = flag_u64(&args, "--on", Some(1)).unwrap_or(1);
                    let mut payload = track_structure_command(
                        UiCommandType::SetTrackSoundAddressed, track);
                    payload.value0 = if on != 0 { 1 } else { 0 };
                    match handle.send_command(payload) {
                        Ok(()) => { println!("{{ \"sent\": \"sound-addressed\", \"track\": {track}, \"on\": {on} }}"); 0 }
                        Err(err) => { eprintln!("daw-cli: {err}"); 1 }
                    }
                }
                Some(&"harmony-quantize") => {
                    let track = flag_u64(&args, "--track", Some(0)).unwrap_or(0) as u32;
                    let on = flag_u64(&args, "--on", Some(1)).unwrap_or(1);
                    let mut payload = track_structure_command(
                        UiCommandType::SetTrackHarmonyQuantize, track);
                    payload.value0 = if on != 0 { 1 } else { 0 };
                    match handle.send_command(payload) {
                        Ok(()) => { println!("{{ \"sent\": \"harmony-quantize\", \"on\": {on} }}"); 0 }
                        Err(err) => { eprintln!("daw-cli: {err}"); 1 }
                    }
                }
                Some(&"quantize") => match quantize_command(&args) {
                    Ok(payload) => match handle.send_command(payload) {
                        Ok(()) => {
                            println!("{{ \"sent\": \"quantize\" }}");
                            0
                        }
                        Err(err) => {
                            eprintln!("daw-cli: {err}");
                            1
                        }
                    },
                    Err(err) => {
                        eprintln!("daw-cli: {err}");
                        2
                    }
                },
                Some(&"mixer") => match mixer_command(&args) {
                    Ok(payload) => match handle.send_command(payload) {
                        Ok(()) => {
                            println!("{{ \"sent\": \"mixer\" }}");
                            0
                        }
                        Err(err) => {
                            eprintln!("daw-cli: {err}");
                            1
                        }
                    },
                    Err(err) => {
                        eprintln!("daw-cli: {err}");
                        2
                    }
                },
                Some(&"chord") => {
                    let track = flag_u64(&args, "--track", Some(0)).unwrap_or(0) as u32;
                    // `do chord` has no --base, so the caller never pins a version: every send
                    // means "apply this now". A stale-base refusal here is therefore always noise
                    // from a publish that had not caught up, and retrying against the version the
                    // engine handed back is what resync_requested asks for. The refusal is still
                    // reported if the RETRY is refused too, which is the case that means something.
                    let retry_stale = true;
                    let base = handle.clip_version_for_track(track);  // M2.17: per track
                    // Sampled BEFORE the send, so the outcome check can tell our own refusal from
                    // one already sitting in the peeked ring, and can recognise acceptance by the
                    // version moving.
                    let before_len = handle.peek_ui_diffs().len();
                    let ver_before = handle.clip_version_for_track(track);
                    match chord_command(&args, base) {
                        Ok(payload) => match handle.send_chord_command(payload) {
                            Ok(()) => {
                                let cmd = payload.command_type;
                                match await_clip_outcome(&handle, track, cmd, base, before_len, ver_before) {
                                    ClipOutcome::Applied | ClipOutcome::Unknown => {
                                        println!("{{ \"sent\": \"chord\", \"base_version\": {base} }}");
                                        0
                                    }
                                    ClipOutcome::Refused { reason, current } if retry_stale
                                        && reason == daw_bridge::layout::UiClipRejectReason::StaleBase as u16 => {
                                        // Rebuilt against the version the engine handed back,
                                        // rather than re-sending the payload with a patched field:
                                        // the base is not the only thing derived from it.
                                        match chord_command(&args, current) {
                                            Ok(again) => match handle.send_chord_command(again) {
                                                Ok(()) => match await_clip_outcome(&handle, track, cmd, current, handle.peek_ui_diffs().len(), handle.clip_version_for_track(track)) {
                                                    ClipOutcome::Applied | ClipOutcome::Unknown => {
                                                        eprintln!("daw-cli: base {base} was stale; retried at {current}");
                                                        println!("{{ \"sent\": \"chord\", \"base_version\": {current}, \"retried\": true }}");
                                                        0
                                                    }
                                                    ClipOutcome::Refused { reason: r2, current: c2 } => {
                                                        eprintln!("daw-cli: chord REFUSED again after retry (reason {r2}, engine now at {c2}). The version is moving faster than a retry can follow.");
                                                        3
                                                    }
                                                },
                                                Err(err) => { eprintln!("daw-cli: {err}"); 1 }
                                            },
                                            Err(err) => { eprintln!("daw-cli: {err}"); 2 }
                                        }
                                    }
                                    ClipOutcome::Refused { reason, current } => {
                                        // LOUD, AND NON-ZERO. This used to print "sent" and exit
                                        // 0 on an edit the engine had thrown away.
                                        eprintln!("daw-cli: the engine REFUSED this chord — reason {reason}, \
                                                   presented base {base}, engine holds {current}. \
                                                   The edit was NOT applied. Re-read the clip version \
                                                   and send again, or pass --retry-stale.");
                                        3
                                    }
                                }
                            }
                            Err(err) => {
                                eprintln!("daw-cli: {err}");
                                1
                            }
                        },
                        Err(err) => {
                            eprintln!("daw-cli: {err}");
                            2
                        }
                    }
                }
                Some(&"harmony") => {
                    // Harmony has its own version counter, not the clip one.
                    let base = handle.harmony_version();
                    match harmony_command(&args, base) {
                        Ok(payload) => match handle.send_command(payload) {
                            Ok(()) => {
                                /*
                                 * WAIT FOR THE ENGINE TO TAKE IT, and say so — this used to print
                                 * "sent" and exit 0 the instant the command was queued.
                                 *
                                 * A PROCESS BOUNDARY IS NOT A SYNCHRONISATION PRIMITIVE, which is
                                 * the assumption that made this look safe. Each invocation spawns,
                                 * attaches, reads the counter and sends — milliseconds of real
                                 * work — so it seemed impossible for two of them to quote the same
                                 * base. Measured (ui-web/test/cli-harmony-rapid.mjs), four
                                 * back-to-back processes quoted `1,1,1,2` and TWO of the four key
                                 * changes reached the document. The engine refused the losers in
                                 * silence and every process exited 0.
                                 *
                                 * Same defect the page had and the sidecar had, third surface.
                                 * Optimistic concurrency needs the base you quote to be the state
                                 * your edit was composed against; a writer that fires again before
                                 * its previous edit lands has invalidated its own base, and no
                                 * amount of reading harder fixes that.
                                 *
                                 * BOUNDED, and a timeout is reported as FAILURE rather than
                                 * assumed to be success: a write that may or may not have landed
                                 * is the one outcome a script can do nothing with.
                                 */
                                let deadline = Instant::now() + Duration::from_millis(750);
                                let mut applied = false;
                                while Instant::now() < deadline {
                                    // Any move off the quoted value means the engine went through
                                    // it. Testing for `base + 1` would be wrong as soon as a
                                    // second writer exists.
                                    if handle.harmony_version() != base { applied = true; break; }
                                    std::thread::sleep(Duration::from_millis(1));
                                }
                                if applied {
                                    println!(
                                        "{{ \"applied\": true, \"harmony\": true, \"base_version\": {base} }}");
                                    0
                                } else {
                                    eprintln!(
                                        "daw-cli: the harmony write was not acknowledged — the \
                                         engine's harmony version is still {base}. It was most \
                                         likely refused for a stale base; nothing was written.");
                                    1
                                }
                            }
                            Err(err) => {
                                eprintln!("daw-cli: {err}");
                                1
                            }
                        },
                        Err(err) => {
                            eprintln!("daw-cli: {err}");
                            2
                        }
                    }
                }
                other => {
                    eprintln!(
                        "daw-cli: unknown command {:?}\n\n{USAGE}",
                        other.unwrap_or(&"")
                    );
                    2
                }
            }
        }
        Some((&"help", _)) | Some((&"-h", _)) | Some((&"--help", _)) => {
            print!("{USAGE}");
            0
        }
        Some((other, _)) => {
            eprintln!("daw-cli: unknown subcommand {other:?}\n\n{USAGE}");
            2
        }
    };
    std::process::exit(code);
}
