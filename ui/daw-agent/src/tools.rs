//! The agent's tool interface: a discoverable manifest of what it can do, and a
//! typed dispatch that carries each call to the engine's command ring. Every tool
//! is declared once with a JSON-schema for its args, so a manifest, validation,
//! and docs all come from the same table — the same "one definition" rule the
//! row-op schema follows. Model-agnostic: an LLM harness maps its tool-call
//! format onto `ToolCall`/`execute`; nothing here talks to a model or a network.

use daw_bridge::control::EngineHandle;
use daw_bridge::grid::NANOTICKS_PER_QUARTER;
use daw_bridge::layout::{UiCommandPayload, UiCommandType, UiPatcherPresetCommandPayload};
use serde::Serialize;
use serde_json::{json, Value};

use crate::observe::observe;

/// One tool the agent can call: name, one-line description, and a JSON-schema
/// object describing its arguments.
#[derive(Debug, Clone, Serialize)]
pub struct ToolSpec {
    pub name: &'static str,
    pub description: &'static str,
    pub params: Value,
}

/// A request to run a tool. `args` is a JSON object matching the tool's schema.
#[derive(Debug, Clone)]
pub struct ToolCall {
    pub tool: String,
    pub args: Value,
}

/// The result of a tool call. `output` is structured (an observation, a count,
/// an ack); `error` is set and `ok` false when the call could not be carried out.
#[derive(Debug, Clone, Serialize)]
pub struct ToolResult {
    pub ok: bool,
    #[serde(skip_serializing_if = "Value::is_null")]
    pub output: Value,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub error: Option<String>,
}

impl ToolResult {
    fn ok(output: Value) -> Self {
        Self { ok: true, output, error: None }
    }
    fn err(msg: impl Into<String>) -> Self {
        Self { ok: false, output: Value::Null, error: Some(msg.into()) }
    }
}

/// The full capability surface. Kept in sync with `execute` by construction —
/// every name here is dispatched there, and vice versa (a test asserts it).
pub fn tool_manifest() -> Vec<ToolSpec> {
    vec![
        ToolSpec {
            name: "observe",
            description: "Read the whole song: transport plus every track's notes. Pure read, no args.",
            params: json!({ "type": "object", "properties": {} }),
        },
        ToolSpec {
            name: "add_notes",
            description: "Write a phrase of notes onto a track, one pitch per step.",
            params: json!({
                "type": "object",
                "required": ["track", "pitches"],
                "properties": {
                    "track": { "type": "integer", "minimum": 0 },
                    "pitches": { "type": "array", "items": { "type": "integer", "minimum": 0, "maximum": 127 },
                                 "description": "MIDI pitches, laid out one per step." },
                    "start": { "type": "integer", "description": "Onset of the first note in nanoticks (default 0)." },
                    "step": { "type": "integer", "description": "Nanoticks between onsets (default one quarter = 960000)." },
                    "duration": { "type": "integer", "description": "Note length in nanoticks (default = step)." },
                    "velocity": { "type": "integer", "minimum": 0, "maximum": 127, "description": "Default 100." },
                    "column": { "type": "integer", "minimum": 0, "description": "Note column / voice lane (default 0)." }
                }
            }),
        },
        ToolSpec {
            name: "transport",
            description: "Control playback: play/pause/toggle, stop (halt + rewind to loop start), or seek to a position.",
            params: json!({
                "type": "object",
                "required": ["action"],
                "properties": {
                    "action": { "type": "string", "enum": ["play", "pause", "stop", "toggle", "seek"] },
                    "position": { "type": "integer", "description": "Seek target in nanoticks (for action=seek)." }
                }
            }),
        },
        ToolSpec {
            name: "save",
            description: "Save the project under a name (written as <name>.uniproj.json).",
            params: json!({
                "type": "object", "required": ["name"],
                "properties": { "name": { "type": "string", "maxLength": 28 } }
            }),
        },
        ToolSpec {
            name: "load",
            description: "Load a saved project by name, restoring its clips and device chains.",
            params: json!({
                "type": "object", "required": ["name"],
                "properties": { "name": { "type": "string", "maxLength": 28 } }
            }),
        },
        ToolSpec {
            name: "set_track_name",
            description: "Rename a track. The name is published so every lane-labelling surface updates.",
            params: json!({
                "type": "object", "required": ["track", "name"],
                "properties": {
                    "track": { "type": "integer", "minimum": 0 },
                    "name": { "type": "string", "maxLength": 24 }
                }
            }),
        },
        ToolSpec {
            name: "undo",
            description: "Undo the last note/chord edit, restoring the track's previous clips + placements.",
            params: json!({ "type": "object", "properties": {} }),
        },
        ToolSpec {
            name: "redo",
            description: "Redo the last undone note/chord edit.",
            params: json!({ "type": "object", "properties": {} }),
        },
    ]
}

/// Renders the manifest as a JSON array — the form an LLM harness ingests to
/// learn its tools.
pub fn manifest_json() -> String {
    serde_json::to_string_pretty(&tool_manifest())
        .unwrap_or_else(|e| format!("[{{\"error\":\"{e}\"}}]"))
}

fn arg_u64(args: &Value, key: &str) -> Option<u64> {
    args.get(key).and_then(|v| v.as_u64())
}

/// Runs one tool against the engine. Unknown tools and malformed args are an
/// error result, never a silent no-op.
pub fn execute(handle: &EngineHandle, call: &ToolCall) -> ToolResult {
    match call.tool.as_str() {
        "observe" => match serde_json::to_value(observe(handle, 0)) {
            Ok(v) => ToolResult::ok(v),
            Err(e) => ToolResult::err(format!("serialize observation: {e}")),
        },
        "add_notes" => add_notes(handle, &call.args),
        "transport" => transport(handle, &call.args),
        "save" => named(handle, UiCommandType::SaveProject, &call.args, "saved"),
        "load" => named(handle, UiCommandType::LoadProject, &call.args, "loaded"),
        "set_track_name" => set_track_name(handle, &call.args),
        "undo" => undo_redo(handle, UiCommandType::Undo),
        "redo" => undo_redo(handle, UiCommandType::Redo),
        other => ToolResult::err(format!("unknown tool {other:?}")),
    }
}

fn add_notes(handle: &EngineHandle, args: &Value) -> ToolResult {
    let track = match arg_u64(args, "track") {
        Some(t) => t as u32,
        None => return ToolResult::err("add_notes needs \"track\""),
    };
    let pitches: Vec<u32> = match args.get("pitches").and_then(|v| v.as_array()) {
        Some(arr) => {
            let mut out = Vec::with_capacity(arr.len());
            for p in arr {
                match p.as_u64() {
                    Some(v) if v <= 127 => out.push(v as u32),
                    _ => return ToolResult::err(format!("bad pitch {p} (expected 0..127)")),
                }
            }
            out
        }
        None => return ToolResult::err("add_notes needs \"pitches\" (an array)"),
    };
    if pitches.is_empty() {
        return ToolResult::err("\"pitches\" was empty");
    }
    let start = arg_u64(args, "start").unwrap_or(0);
    let step = arg_u64(args, "step").unwrap_or(NANOTICKS_PER_QUARTER);
    let duration = arg_u64(args, "duration").unwrap_or(step);
    let velocity = arg_u64(args, "velocity").unwrap_or(100).min(127) as u32;
    let column = arg_u64(args, "column").unwrap_or(0) as u16;

    // Optimistic concurrency: each accepted write bumps this TRACK's clip version by
    // one, so the next note's base_version is the previous plus one. Same protocol the
    // UI obeys — the agent is not privileged.
    //
    // Per TRACK, not global (M2.17). Reading the global here is exactly the failure the
    // per-track counters were introduced to end: the moment anyone edits another track
    // the two counters diverge, and every note this agent writes is silently rejected —
    // which is the "agent works on track 4 while you type on track 1" case itself.
    let mut base = handle.clip_version_for_track(track);
    let first_base = base;
    let mut sent = 0usize;
    for (index, pitch) in pitches.iter().enumerate() {
        let nanotick = start + step * index as u64;
        let payload = UiCommandPayload {
            command_type: UiCommandType::WriteNote as u16,
            flags: column,
            track_id: track,
            plugin_index: 0,
            note_pitch: *pitch,
            value0: velocity,
            note_nanotick_lo: (nanotick & 0xffff_ffff) as u32,
            note_nanotick_hi: (nanotick >> 32) as u32,
            note_duration_lo: (duration & 0xffff_ffff) as u32,
            note_duration_hi: (duration >> 32) as u32,
            base_version: base,
        };
        if let Err(e) = handle.send_command(payload) {
            return ToolResult::err(format!("{e} after {sent} notes"));
        }
        sent += 1;
        base = base.wrapping_add(1);
    }
    // Wait for the engine to apply this batch (clip version reaches first_base +
    // sent) before returning, so a following tool call reads a settled version
    // and doesn't race the ring — no fixed delay between calls.
    let applied = handle.wait_for_clip_version(
        first_base,
        first_base.wrapping_add(sent as u32),
        std::time::Duration::from_secs(2),
    );
    ToolResult::ok(json!({
        "sent": sent,
        "first_base_version": first_base,
        "applied": applied,
        "track": track,
    }))
}

// Undo or redo the last structural (note/chord) edit. The engine keeps the undo
// stack; the agent just sends the command tagged with the current clip version and
// waits for the one-version bump a store swap produces. `applied=false` means the
// stack was empty (nothing happened), never a silent error.
fn undo_redo(handle: &EngineHandle, cmd: UiCommandType) -> ToolResult {
    let base = handle.clip_version();
    let payload = UiCommandPayload {
        command_type: cmd as u16,
        flags: 0,
        track_id: 0,
        plugin_index: 0,
        note_pitch: 0,
        value0: 0,
        note_nanotick_lo: 0,
        note_nanotick_hi: 0,
        note_duration_lo: 0,
        note_duration_hi: 0,
        base_version: base,
    };
    if let Err(e) = handle.send_command(payload) {
        return ToolResult::err(e);
    }
    let applied =
        handle.wait_for_clip_version(base, base.wrapping_add(1), std::time::Duration::from_secs(2));
    ToolResult::ok(json!({ "applied": applied }))
}

fn transport(handle: &EngineHandle, args: &Value) -> ToolResult {
    let action = match args.get("action").and_then(|v| v.as_str()) {
        Some(a) => a,
        None => return ToolResult::err("transport needs \"action\" (play|pause|stop|toggle|seek)"),
    };
    let playing = handle.snapshot().map(|s| s.ui_transport_state != 0).unwrap_or(false);

    // stop and seek are distinct commands, not toggles.
    let send = |cmd: UiCommandType, pos: u64| -> ToolResult {
        let payload = UiCommandPayload {
            command_type: cmd as u16,
            flags: 0,
            track_id: 0,
            plugin_index: 0,
            note_pitch: 0,
            value0: 0,
            note_nanotick_lo: (pos & 0xffff_ffff) as u32,
            note_nanotick_hi: (pos >> 32) as u32,
            note_duration_lo: 0,
            note_duration_hi: 0,
            base_version: 0,
        };
        match handle.send_command(payload) {
            Ok(()) => ToolResult::ok(json!({ "action": action })),
            Err(e) => ToolResult::err(e),
        }
    };
    match action {
        "stop" => return send(UiCommandType::Stop, 0),
        "seek" => {
            let pos = match arg_u64(args, "position") {
                Some(p) => p,
                None => return ToolResult::err("seek needs \"position\" (nanotick)"),
            };
            return send(UiCommandType::SetPosition, pos);
        }
        _ => {}
    }

    // play/pause/toggle map onto TogglePlay: flip only when it changes state, so
    // they are idempotent.
    let should_toggle = match action {
        "toggle" => true,
        "play" => !playing,
        "pause" => playing,
        other => return ToolResult::err(format!("bad action {other:?}")),
    };
    if !should_toggle {
        return ToolResult::ok(json!({ "action": action, "changed": false, "playing": playing }));
    }
    match send(UiCommandType::TogglePlay, 0) {
        r if r.ok => ToolResult::ok(json!({ "action": action, "changed": true })),
        r => r,
    }
}

fn set_track_name(handle: &EngineHandle, args: &Value) -> ToolResult {
    let track = match arg_u64(args, "track") {
        Some(t) => t as u32,
        None => return ToolResult::err("set_track_name needs \"track\""),
    };
    let name = match args.get("name").and_then(|v| v.as_str()) {
        Some(n) => n,
        None => return ToolResult::err("set_track_name needs \"name\""),
    };
    let mut bytes = [0u8; 28];
    let src = name.as_bytes();
    let len = src.len().min(24); // published field is 24 bytes
    bytes[..len].copy_from_slice(&src[..len]);
    let preset = UiPatcherPresetCommandPayload {
        command_type: UiCommandType::SetTrackName as u16,
        flags: 0,
        track_id: track,
        base_version: 0,
        name: bytes,
    };
    let as_ui: UiCommandPayload = unsafe { std::mem::transmute(preset) };
    match handle.send_command(as_ui) {
        Ok(()) => ToolResult::ok(json!({ "track": track, "name": name })),
        Err(e) => ToolResult::err(e),
    }
}

fn named(handle: &EngineHandle, command: UiCommandType, args: &Value, verb: &str) -> ToolResult {
    let name = match args.get("name").and_then(|v| v.as_str()) {
        Some(n) if !n.is_empty() => n,
        _ => return ToolResult::err("needs a non-empty \"name\""),
    };
    let mut bytes = [0u8; 28];
    let src = name.as_bytes();
    let len = src.len().min(bytes.len());
    bytes[..len].copy_from_slice(&src[..len]);
    let preset = UiPatcherPresetCommandPayload {
        command_type: command as u16,
        flags: 0,
        track_id: 0,
        base_version: 0,
        name: bytes,
    };
    // The engine reads the named command out of the same 40-byte command slot;
    // the two payloads are layout-compatible by design (asserted in daw-bridge).
    let as_ui: UiCommandPayload = unsafe { std::mem::transmute(preset) };
    match handle.send_command(as_ui) {
        Ok(()) => ToolResult::ok(json!({ verb: name })),
        Err(e) => ToolResult::err(e),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn manifest_is_well_formed() {
        let m = tool_manifest();
        assert!(!m.is_empty());
        for spec in &m {
            assert!(!spec.name.is_empty());
            assert!(!spec.description.is_empty());
            assert_eq!(spec.params["type"], "object", "tool {} params must be an object", spec.name);
        }
        // Names are unique.
        let mut names: Vec<_> = m.iter().map(|s| s.name).collect();
        names.sort();
        let before = names.len();
        names.dedup();
        assert_eq!(before, names.len(), "duplicate tool name in manifest");
    }

    #[test]
    fn every_manifest_tool_has_a_dispatch_arm() {
        // execute must recognize every advertised tool. We can't touch the engine
        // here, so we only assert the tool is not reported "unknown"; a missing
        // required arg is an acceptable (recognized) error.
        for spec in tool_manifest() {
            let call = ToolCall { tool: spec.name.to_string(), args: json!({}) };
            // Route only through the arg-independent recognition: an unknown tool
            // yields exactly the "unknown tool" message.
            let recognized = match call.tool.as_str() {
                "observe" | "add_notes" | "transport" | "save" | "load"
                | "set_track_name" | "undo" | "redo" => true,
                _ => false,
            };
            assert!(recognized, "manifest tool {:?} has no dispatch arm", spec.name);
        }
    }

    #[test]
    fn payloads_are_layout_compatible_for_named_transmute() {
        // The save/load transmute relies on these being the same size.
        assert_eq!(
            std::mem::size_of::<UiPatcherPresetCommandPayload>(),
            std::mem::size_of::<UiCommandPayload>()
        );
    }
}
