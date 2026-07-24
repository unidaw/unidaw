//! Control surface for a running engine.
//!
//! Queries are read-only and always safe. Commands write into the UI command
//! ring, which is single-producer: while the UI app is running it owns that
//! ring, so `do` requires --force to acknowledge you are the only writer.

use std::thread;
use std::time::Duration;

use daw_bridge::control::{default_shm_name, EngineHandle};
use daw_bridge::layout::{UiCommandPayload, UiCommandType, UiPatcherPresetCommandPayload};

const USAGE: &str = "\
daw-cli — control surface for a running engine

  daw-cli watch                    stream transport state (default)
  daw-cli get transport            transport + versions as JSON
  daw-cli get tracks               per-track state as JSON
  daw-cli do save [name] --force   save the project (default name: default)
  daw-cli do load [name] --force   load the project
  daw-cli do play --force          toggle transport

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
    println!("  \"track_count\": {},", handle.track_count());
    println!("  \"clip_version\": {}", handle.clip_version());
    println!("}}");
    0
}

fn get_tracks(handle: &EngineHandle) -> i32 {
    let Some(snapshot) = handle.snapshot() else {
        eprintln!("daw-cli: no coherent snapshot available");
        return 1;
    };
    let count = handle.track_count() as usize;
    println!("{{");
    println!("  \"track_count\": {count},");
    println!("  \"tracks\": [");
    for index in 0..count {
        let rms = snapshot
            .ui_track_peak_rms
            .get(index)
            .copied()
            .unwrap_or(0.0);
        let comma = if index + 1 == count { "" } else { "," };
        println!("    {{ \"track_id\": {index}, \"peak_rms\": {rms} }}{comma}");
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
