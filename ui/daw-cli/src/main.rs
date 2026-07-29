//! Control surface for a running engine.
//!
//! Queries are read-only and always safe. Commands write into the UI command
//! ring, which is single-producer: while the UI app is running it owns that
//! ring, so `do` requires --force to acknowledge you are the only writer.

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
  daw-cli get clip --force [--track N] [--bars N] [--grid]
                                   notes and chords in a window, as JSON or as
                                   a tracker-style text grid
  daw-cli do save [name] --force   save the project (default name: default)
  daw-cli do load [name] --force   load the project
  daw-cli do play --force          toggle transport
  daw-cli do panic --force         all sound off (CC120+CC123 everywhere)
  daw-cli do note --force --track N --nanotick T --pitch P
                  [--velocity V] [--duration D] [--column C]
  daw-cli do delete-note --force --track N --nanotick T --pitch P [--column C]
  daw-cli do notes --force --track N --pitches 60,64,67 [--start T] [--step S]
                   [--duration D] [--velocity V] [--column C]
                                   writes a phrase in one invocation
  daw-cli do chord --force --track N --nanotick T --degree D
                   [--quality Q] [--inversion I] [--octave O] [--duration D]
  daw-cli do harmony --force --nanotick T --root R --scale S
  daw-cli do mixer --force --track N [--gain-db X] [--pan Y]
                   [--mute 0|1] [--solo 0|1]

`get clip` needs --force too: reading a window means asking the engine for one,
and any request is a write to the single-producer command ring.

Write a phrase with `do notes`, not a shell loop over `do note`. The engine
accepts one clip edit per version, and each invocation reads the version once
at startup, so back-to-back processes all claim the same version and only the
first survives. `do notes` numbers them itself.

Queries are read-only. `do` writes to the UI command ring, which allows a
single producer: if the UI app is running it already owns that ring, so pass
--force only when nothing else is writing.

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

// do set-param <track> <device> <uid16hex> <milli> [--force]
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

// do set-tempo <bpm> [position_nanotick] [--force]
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
        println!(
            "    {{ \"track_id\": {id}, \"name\": {name:?}, \"device\": {device:?}, \"master\": {is_master}, \"peak_rms\": {rms} }}{comma}"
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

    let mut base = handle.clip_version();
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
    let degree = flag_u64(args, "--degree", None)?;
    let duration = flag_u64(args, "--duration", Some(0))?;
    Ok(UiChordCommandPayload {
        command_type: UiCommandType::WriteChord as u16,
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
    let root = flag_u64(args, "--root", None)?;
    let scale = flag_u64(args, "--scale", None)?;
    Ok(UiCommandPayload {
        command_type: UiCommandType::WriteHarmony as u16,
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
    // Any nonzero id works; it only has to match what comes back.
    let request_id = 0x5ADD_u32;

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
    let force = args.iter().any(|arg| arg == "--force");
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
        // `get clip` has to ask the engine for a window, and a request is a
        // ring write, so it needs the same acknowledgement as `do`.
        Some((&"get", rest)) if rest.first() == Some(&"clip") => {
            if !force {
                eprintln!(
                    "daw-cli: `get clip` asks the engine for a window, which writes to the\n\
                     single-producer command ring. Pass --force when nothing else is writing."
                );
                std::process::exit(2);
            }
            match EngineHandle::attach(&name, true) {
                Ok(handle) => get_clip(&handle, &args),
                Err(err) => {
                    eprintln!("daw-cli: {err}");
                    1
                }
            }
        }
        // Reads its own read-back, but first SENDS RequestDeviceParams — that is a
        // write to the single-producer command ring, so it needs a writable handle
        // (a read-only mmap makes send_command a silent no-op) and the --force guard.
        Some((&"get", rest)) if rest.first() == Some(&"device-params") => {
            if !force {
                eprintln!(
                    "daw-cli: `get device-params` writes RequestDeviceParams to the\n\
                     single-producer command ring. Pass --force when nothing else is writing."
                );
                std::process::exit(2);
            }
            match EngineHandle::attach(&name, true) {
                Ok(handle) => get_device_params(&handle, rest),
                Err(err) => {
                    eprintln!("daw-cli: {err}");
                    1
                }
            }
        }
        Some((&"get", rest)) if rest.first() == Some(&"waveform") => {
            if !force {
                eprintln!(
                    "daw-cli: `get waveform` writes RequestWaveform to the\n\
                     single-producer command ring. Pass --force when nothing else is writing."
                );
                std::process::exit(2);
            }
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
            if !force {
                eprintln!(
                    "daw-cli: `do` writes to the single-producer UI command ring.\n\
                     If the UI app is running it already owns that ring and a second\n\
                     writer would corrupt it. Pass --force when nothing else is writing."
                );
                std::process::exit(2);
            }
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
                    // the current one immediately before sending.
                    let base = handle.clip_version();
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
                    let base = handle.clip_version();
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
