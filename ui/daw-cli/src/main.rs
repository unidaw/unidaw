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
    UiChainCommandPayload, UiChordCommandPayload, UiClipWindowCommandPayload, UiCommandPayload,
    UiCommandType, UiPatcherPresetCommandPayload, UiWaveformRequestPayload, MASTER_TRACK_ID,
};

const USAGE: &str = "\
daw-cli — control surface for a running engine

  daw-cli watch                    stream transport state (default)
  daw-cli get transport            transport + versions as JSON
  daw-cli get tracks               per-track state as JSON
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
                  [--velocity V] [--duration D] [--column C] [--base V]
  daw-cli do delete-note --track N --nanotick T --pitch P [--column C]
  daw-cli do notes --track N --pitches 60,64,67 [--start T] [--step S]
                   [--duration D] [--velocity V] [--column C]
                                   writes a phrase in one invocation
  daw-cli do chord --track N --nanotick T --degree D
                   [--quality Q] [--inversion I] [--octave O] [--duration D]
                   [--delete]
  daw-cli do harmony --nanotick T --root R --scale S [--delete]
  daw-cli do stop                  halt the transport
  daw-cli do position --nanotick T move the playhead
  daw-cli do loop --start T --end T set the loop range
  daw-cli do harmony-quantize --track N [--on 0|1]
  daw-cli do routing --track N [--audio-out none|master|track:M|input:M]
                     [--midi-out ...] [--audio-in ...] [--midi-in ...]
                     [--pre-fader 0|1]
                                   REPLACES every route on the track. Anything not
                                   named goes to its DEFAULT (audio-out master, the
                                   rest none) — the engine has no partial form and no
                                   routing read-back to merge against.
  daw-cli do patcher-node --track N --type euclidean|lfo|random-degree|
                          passthrough|audio-passthrough|event-out
  daw-cli do patcher-unnode --track N --node ID
  daw-cli do patcher-connect --track N --src ID --dst ID
                             [--src-port P] [--dst-port P] [--kind event|audio|cv]
  daw-cli do patcher-config --track N --node ID --type T [type-specific flags]
                            euclidean: --steps --hits --offset --degree
                                       --octave-offset --velocity --base-octave
                                       --duration
                            random-degree: --degree --velocity --duration
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
    let column = flag_u64(args, "--column", Some(0))?;
    Ok(UiCommandPayload {
        command_type: command as u16,
        flags: column as u16,
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
        println!(
            "    {{ \"nanotick\": {}, \"pitch\": {}, \"velocity\": {}, \"column\": {}, \
             \"dev\": {}, \"delay\": {}, \"sounds_at\": {} }}{comma}",
            note.t_on, note.pitch, note.velocity, note.column,
            note.dev_nanoticks, note.delay_nanoticks,
            // The AUTHORED tick plus both offsets — quantize moves the tick and the row-op
            // delay is added after it, so the sounding position is the sum.
            note.t_on as i64 + note.dev_nanoticks as i64 + note.delay_nanoticks as i64
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
    println!("{{");
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
        let comma = if index + 1 == count { "" } else { "," };
        // M2.17: this track's OWN clip version — the base an edit to this track must
        // present. The global `clip_version` in `get transport` moves whenever ANY
        // track changes and is no longer the right base for a track-scoped edit.
        let clip_version = handle.clip_version_for_track(id);
        println!(
            "    {{ \"track_id\": {id}, \"name\": {name:?}, \"device\": {device:?}, \"master\": {is_master}, \"clip_version\": {clip_version}, \"peak_rms\": {rms} }}{comma}"
        );
    }
    println!("  ]");
    println!("}}");
    0
}

fn named_command(command: UiCommandType, name: &str) -> UiPatcherPresetCommandPayload {
    let mut bytes = [0u8; 28];
    let source = name.as_bytes();
    let len = source.len().min(bytes.len());
    bytes[..len].copy_from_slice(&source[..len]);
    UiPatcherPresetCommandPayload {
        command_type: command as u16,
        flags: 0,
        track_id: 0,
        base_version: 0,
        name: bytes,
    }
}

fn send_named(handle: &EngineHandle, command: UiCommandType, name: &str) -> i32 {
    // Reuses the 40-byte command slot; the engine reads it as a named command.
    let payload = named_command(command, name);
    let as_ui: UiCommandPayload = unsafe { std::mem::transmute(payload) };
    match handle.send_command(as_ui) {
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
fn parse_node_type(raw: &str) -> Result<u32, String> {
    use daw_bridge::layout as L;
    Ok(match raw {
        "euclidean" => L::PATCHER_NODE_EUCLIDEAN,
        "lfo" => L::PATCHER_NODE_LFO,
        "random-degree" => L::PATCHER_NODE_RANDOM_DEGREE,
        "passthrough" => L::PATCHER_NODE_PASSTHROUGH,
        "audio-passthrough" => L::PATCHER_NODE_AUDIO_PASSTHROUGH,
        "event-out" => L::PATCHER_NODE_EVENT_OUT,
        "rust-kernel" => L::PATCHER_NODE_RUST_KERNEL,
        other => return Err(format!(
            "--type: expected euclidean|lfo|random-degree|passthrough|\
             audio-passthrough|event-out|rust-kernel, got {other:?}"
        )),
    })
}

/// SetPatcherNodeConfig's `config` block. EXPLICIT little-endian per node type — the
/// engine reads it field by field rather than memcpy'ing a struct, because a raw copy
/// truncated Euclidean and coupled the wire to C++ padding. Building it here by the same
/// documented layout keeps the two ends honest.
fn build_node_config(args: &[String], node_type: u32) -> Result<[u8; 16], String> {
    use daw_bridge::layout as L;
    let mut cfg = [0u8; 16];
    let put_u16 = |c: &mut [u8; 16], at: usize, v: u16| {
        c[at] = (v & 0xff) as u8;
        c[at + 1] = (v >> 8) as u8;
    };
    let put_u32 = |c: &mut [u8; 16], at: usize, v: u32| {
        c[at] = (v & 0xff) as u8;
        c[at + 1] = ((v >> 8) & 0xff) as u8;
        c[at + 2] = ((v >> 16) & 0xff) as u8;
        c[at + 3] = ((v >> 24) & 0xff) as u8;
    };
    if node_type == L::PATCHER_NODE_EUCLIDEAN {
        // 0 MEANS 0 (M0.6): these are sent verbatim, so `--hits 0` is silence and not
        // "use the default five".
        put_u16(&mut cfg, 0, flag_u64(args, "--steps", Some(16))? as u16);
        put_u16(&mut cfg, 2, flag_u64(args, "--hits", Some(5))? as u16);
        put_u16(&mut cfg, 4, flag_u64(args, "--offset", Some(0))? as u16);
        cfg[6] = flag_u64(args, "--degree", Some(1))? as u8;
        cfg[7] = (flag_i64(args, "--octave-offset", 0)? as i8) as u8;
        cfg[8] = flag_u64(args, "--velocity", Some(100))? as u8;
        cfg[9] = flag_u64(args, "--base-octave", Some(4))? as u8;
        put_u32(&mut cfg, 12, flag_u64(args, "--duration", Some(0))? as u32);
    } else if node_type == L::PATCHER_NODE_RANDOM_DEGREE {
        cfg[0] = flag_u64(args, "--degree", Some(8))? as u8;
        cfg[1] = flag_u64(args, "--velocity", Some(100))? as u8;
        put_u32(&mut cfg, 4, flag_u64(args, "--duration", Some(0))? as u32);
    } else if node_type == L::PATCHER_NODE_LFO {
        // Milli-units on the wire, mirroring the read-back; the engine stores floats.
        let milli = |v: f64| ((v * 1000.0).round() as i32) as u32;
        put_u32(&mut cfg, 0, milli(flag_f64(args, "--freq", 1.0)?));
        put_u32(&mut cfg, 4, milli(flag_f64(args, "--depth", 1.0)?));
        put_u32(&mut cfg, 8, milli(flag_f64(args, "--bias", 0.0)?));
        put_u32(&mut cfg, 12, milli(flag_f64(args, "--phase", 0.0)?));
    } else {
        return Err(
            "--type: only euclidean, random-degree and lfo carry a config".to_string(),
        );
    }
    Ok(cfg)
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
    let column = flag_u64(args, "--column", Some(0)).unwrap_or(0);

    // M2.17: the base is this TRACK's version. Each note consumes one, so the run
    // numbers itself from there — and because acceptance is per track, a phrase written
    // here is no longer invalidated by someone editing a different track mid-run.
    let mut base = handle.clip_version_for_track(track);
    let mut sent = 0usize;
    for (index, pitch) in pitches.iter().enumerate() {
        let nanotick = start + step * index as u64;
        let payload = UiCommandPayload {
            command_type: UiCommandType::WriteNote as u16,
            flags: column as u16,
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
    let track = flag_u64(args, "--track", Some(0))? as u32;
    let gain_db = flag_f64(args, "--gain-db", 0.0)?;
    let pan = flag_f64(args, "--pan", 0.0)?.clamp(-1.0, 1.0);
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
        flags: flag_u64(args, "--column", Some(0))? as u16,
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
        humanize_timing: 0,
        humanize_velocity: 0,
        reserved: 0,
        spread_nanoticks: 0,
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
            let (first, uid, val) = v
                .params
                .first()
                .map(|p| {
                    let hex: String = p.uid16.iter().map(|b| format!("{b:02x}")).collect();
                    (p.name.clone(), hex, p.value)
                })
                .unwrap_or_default();
            println!(
                "{{ \"track\": {}, \"device\": {}, \"name\": {:?}, \"version\": {}, \"count\": {}, \"first\": {:?}, \"first_uid16\": {:?}, \"first_value\": {:.3} }}",
                v.track_id, v.device_id, v.device_name, v.version, v.params.len(), first, uid, val
            );
            return 0;
        }
    }
}

// get extents — dump the published clip rails, decoding each clip's packed grid.
fn get_extents(handle: &EngineHandle) -> i32 {
    let extents = handle.read_clip_extents();
    println!("[");
    for e in &extents {
        let grid = daw_bridge::layout::unpack_clip_grid(e.flags)
            .map(|(lpb, n, d)| format!("{{ \"lpb\": {lpb}, \"time_sig\": \"{n}/{d}\" }}"))
            .unwrap_or_else(|| "null".to_string());
        let audio = e.flags & daw_bridge::layout::UI_CLIP_EXTENT_AUDIO != 0;
        println!(
            "  {{ \"placement\": {}, \"clip\": {}, \"track\": {}, \"audio\": {}, \"start\": {}, \"end\": {}, \"grid\": {} }},",
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
// Sends RequestWaveform and reads the seqlocked answer slot back.
fn get_waveform(handle: &EngineHandle, args: &[&str]) -> i32 {
    let source_id: u32 = args.get(1).and_then(|s| s.parse().ok()).unwrap_or(1);
    let decimation: u32 = args.get(2).and_then(|s| s.parse().ok()).unwrap_or(64);
    let first_frame: u64 = args.get(3).and_then(|s| s.parse().ok()).unwrap_or(0);
    let columns: u32 = args.get(4).and_then(|s| s.parse().ok()).unwrap_or(16);
    let channel_mask: u32 = args.get(5).and_then(|s| s.parse().ok()).unwrap_or(1);
    let request_seq: u32 = 0x7A5E;
    let slot_index = (request_seq as usize) % 4; // K_UI_WAVEFORM_SLOTS
    let payload = UiWaveformRequestPayload {
        command_type: UiCommandType::RequestWaveform as u16,
        flags: 0,
        request_seq,
        source_id,
        decimation,
        first_frame_lo: (first_frame & 0xffff_ffff) as u32,
        first_frame_hi: (first_frame >> 32) as u32,
        columns,
        channel_mask,
        reserved0: 0,
        reserved1: 0,
    };
    let deadline = Instant::now() + Duration::from_secs(5);
    loop {
        let _ = handle.send_waveform_request(payload);
        thread::sleep(Duration::from_millis(100));
        if let Some(v) = handle.read_waveform_slot(slot_index) {
            if v.request_seq == request_seq {
                let pairs: Vec<String> = v.pairs.iter().map(|p| p.to_string()).collect();
                println!(
                    "{{ \"requestSeq\": {}, \"sourceId\": {}, \"status\": {}, \"decimation\": {}, \"columns\": {}, \"channels\": {}, \"firstFrame\": {}, \"frameCount\": {}, \"contentKey\": {}, \"flags\": {}, \"pairs\": [{}] }}",
                    v.request_seq, v.source_id, v.status, v.decimation, v.columns,
                    v.channels, v.first_frame, v.frame_count, v.content_key, v.flags,
                    pairs.join(",")
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
    let request_id = {
        let pid = std::process::id();
        let nanos = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.subsec_nanos())
            .unwrap_or(0);
        let id = pid.rotate_left(11) ^ nanos;
        if id == 0 { 1 } else { id }
    };

    let request = UiClipWindowCommandPayload {
        command_type: UiCommandType::RequestClipWindow as u16,
        flags: 0,
        track_id: track,
        request_id,
        window_start_lo: 0,
        window_start_hi: 0,
        window_end_lo: (window_end & 0xffff_ffff) as u32,
        window_end_hi: (window_end >> 32) as u32,
        cursor_event_index: 0,
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
        eprintln!("daw-cli: timed out waiting for a clip window for track {track}");
        return 1;
    };

    let note_count = (snapshot.note_count as usize).min(snapshot.notes.len());
    let chord_count = (snapshot.chord_count as usize).min(snapshot.chords.len());

    if args.iter().any(|a| a == "--grid") {
        print_grid(&snapshot, note_count, chord_count, bars);
        return 0;
    }

    println!("{{");
    println!("  \"track_id\": {},", snapshot.track_id);
    println!("  \"clip_version\": {},", snapshot.clip_version);
    println!("  \"window_end_nanotick\": {},", snapshot.window_end_nanotick);
    println!("  \"notes\": [");
    for index in 0..note_count {
        let note = snapshot.notes[index];
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
        let chord = snapshot.chords[index];
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
fn print_grid(
    snapshot: &daw_bridge::layout::UiClipWindowSnapshot,
    note_count: usize,
    chord_count: usize,
    bars: u64,
) {
    let row = NANOTICKS_PER_QUARTER / 4; // 16th notes
    let rows = (bars * 16) as usize;
    let mut cells: Vec<Vec<String>> = vec![vec![".".to_string(); 4]; rows];

    for index in 0..note_count {
        let note = snapshot.notes[index];
        let r = (note.t_on / row) as usize;
        let c = (note.column as usize).min(3);
        if r < rows {
            cells[r][c] = pitch_name(note.pitch);
        }
    }
    for index in 0..chord_count {
        let chord = snapshot.chords[index];
        let r = (chord.nanotick / row) as usize;
        if r < rows {
            cells[r][3] = format!("~{}", chord.degree);
        }
    }

    println!("track {}  clip_version {}", snapshot.track_id, snapshot.clip_version);
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
        Some((&"get", rest)) if rest.first() == Some(&"waveform") => {
            match EngineHandle::attach(&name, true) {
                Ok(handle) => get_waveform(&handle, rest),
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
                Some(&"extents") => get_extents(&handle),
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
                Some(&"load") => {
                    let project = rest.get(1).copied().unwrap_or("default");
                    let code = send_named(&handle, UiCommandType::LoadProject, project);
                    if code == 0 {
                        println!("{{ \"loaded\": \"{}\" }}", escape(project));
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
                    match note_command(command, &args, base) {
                        Ok(payload) => match handle.send_command(payload) {
                            Ok(()) => {
                                let label = if is_write { "note" } else { "delete-note" };
                                println!(
                                    "{{ \"sent\": \"{label}\", \"base_version\": {base} }}"
                                );
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
                    let name = flag(&args, "--name").unwrap_or_default();
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
                    match handle.send_chain_command(payload) {
                        Ok(()) => { println!("{{ \"sent\": \"set-bypass\", \"track\": {track}, \"device\": {device}, \"bypass\": {bypass} }}"); 0 }
                        Err(err) => { eprintln!("daw-cli: {err}"); 1 }
                    }
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
                            match handle.send_chain_command(payload) {
                                Ok(()) => {
                                    println!(
                                        "{{ \"sent\": \"add-device\", \"track\": {track}, \"kind\": {kind_arg:?} }}"
                                    );
                                    0
                                }
                                Err(err) => {
                                    eprintln!("daw-cli: {err}");
                                    1
                                }
                            }
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
                    match handle.send_chain_command(payload) {
                        Ok(()) => {
                            let verb = if removing { "remove-device" } else { "move-device" };
                            println!("{{ \"sent\": {verb:?}, \"device\": {device} }}");
                            0
                        }
                        Err(err) => { eprintln!("daw-cli: {err}"); 1 }
                    }
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
                Some(&"mod-link") | Some(&"unmod-link") => {
                    let removing = rest.first() == Some(&"unmod-link");
                    match mod_link_command(&args, removing) {
                        Ok(payload) => match handle.send_mod_link_command(payload) {
                            Ok(()) => {
                                println!(
                                    "{{ \"sent\": {:?}, \"track\": {}, \"link\": {} }}",
                                    if removing { "unmod-link" } else { "mod-link" },
                                    payload.track_id, payload.link_id
                                );
                                0
                            }
                            Err(err) => { eprintln!("daw-cli: {err}"); 1 }
                        },
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
                Some(&"routing") => match routing_command(&args) {
                    Ok(payload) => match handle.send_routing_command(payload) {
                        Ok(()) => {
                            println!(
                                "{{ \"sent\": \"routing\", \"track\": {}, \"replaced_all_routes\": true }}",
                                payload.track_id
                            );
                            0
                        }
                        Err(err) => { eprintln!("daw-cli: {err}"); 1 }
                    },
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
                    let base = handle.clip_version_for_track(track);  // M2.17: per track
                    match chord_command(&args, base) {
                        Ok(payload) => match handle.send_chord_command(payload) {
                            Ok(()) => {
                                println!("{{ \"sent\": \"chord\", \"base_version\": {base} }}");
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
                    }
                }
                Some(&"harmony") => {
                    // Harmony has its own version counter, not the clip one.
                    let base = handle.harmony_version();
                    match harmony_command(&args, base) {
                        Ok(payload) => match handle.send_command(payload) {
                            Ok(()) => {
                                println!("{{ \"sent\": \"harmony\", \"base_version\": {base} }}");
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
