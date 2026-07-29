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

use crate::observe::{observe_window, Window};

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
            description: "Read the song. With no arguments you get its SHAPE: tempo, meter, key, \
                          and every track's name, note count, beat range and pitch range — which \
                          is what you need to know which track is which. Pass `from_beat` (and \
                          optionally `beats` and `track`) to also get the individual notes in \
                          that window. Enumerating a whole large song is not offered: it can run \
                          to millions of characters, so ask for the part you are working on.",
            params: json!({
                "type": "object",
                "properties": {
                    "from_beat": { "type": "number", "minimum": 0,
                                   "description": "Start of the window, in quarter notes. Omit for shape only." },
                    "beats": { "type": "number", "minimum": 0,
                               "description": "Length of the window in quarter notes. Default 16 (four bars of 4/4)." },
                    "track": { "type": "integer", "minimum": 0,
                               "description": "Only this track's notes. Omit for every track." },
                },
            }),
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
        // The document operations. Until these existed an agent could add a note
        // and not remove one, set no tempo, and touch no fader — so "make the bass
        // quieter" had nothing under it and the model had to say so.
        ToolSpec {
            name: "delete_note",
            description: "Delete the note at a tick on a track. Ticks are absolute nanoticks;                           960000 per quarter note.",
            params: json!({
                "type": "object",
                "required": ["track", "tick"],
                "properties": {
                    "track": { "type": "integer", "minimum": 0 },
                    "tick": { "type": "integer", "minimum": 0 },
                },
            }),
        },
        ToolSpec {
            name: "add_track",
            description: "Append an empty track to the song. It arrives at the end with no \
                          instrument on it; load one with a device command, or write notes \
                          to it straight away.",
            params: json!({ "type": "object", "properties": {} }),
        },
        ToolSpec {
            name: "remove_track",
            description: "Remove a track by its stable id. Its slot is kept as a tombstone so \
                          the tracks after it do NOT renumber — an id you hold stays valid. \
                          This cannot be undone.",
            params: json!({
                "type": "object",
                "required": ["track"],
                "properties": { "track": { "type": "integer", "minimum": 0 } },
            }),
        },
        /*
         * PLACEMENTS. Where a clip sits in the arrangement.
         *
         * Deliberately expressed in BEATS rather than nanoticks. Every other
         * tool here takes ticks because notes are written at tick precision, but
         * an arrangement is worked in bars and phrases, and asking a model to
         * multiply by 960000 for every clip is asking it to make an arithmetic
         * slip that lands a chorus a third of a beat late — visible to nobody
         * and audible to everybody.
         */
        ToolSpec {
            name: "clips",
            description: "List the clips placed in the arrangement: their placement id, which \
                          track they are on, where they start and how long they are, in beats. \
                          The placement id is what the other clip tools take, and it is stable \
                          across edits.",
            params: json!({ "type": "object", "properties": {} }),
        },
        ToolSpec {
            name: "move_clip",
            description: "Move a placed clip to a new position, and optionally to another \
                          track. Its length does not change. A move that would overlap a \
                          neighbour is clamped by the engine rather than refused, so check \
                          `clips` afterwards if the exact position matters.",
            params: json!({
                "type": "object",
                "required": ["id", "track", "beat"],
                "properties": {
                    "id": { "type": "integer", "minimum": 0,
                            "description": "the placement id from `clips`" },
                    "track": { "type": "integer", "minimum": 0,
                               "description": "the track it is on NOW" },
                    "beat": { "type": "number", "minimum": 0,
                              "description": "where it should start, in beats from the top" },
                    "to_track": { "type": "integer", "minimum": 0,
                                  "description": "another track to move it to; omit to stay put" },
                },
            }),
        },
        ToolSpec {
            name: "trim_clip",
            description: "Change where a placed clip starts, how long it is, or both. Omit \
                          `beat` to leave the start alone and only change the length; omit \
                          `beats` to move the start and keep the end where it is. At least \
                          one is required.",
            params: json!({
                "type": "object",
                "required": ["id", "track"],
                "properties": {
                    "id": { "type": "integer", "minimum": 0 },
                    "track": { "type": "integer", "minimum": 0 },
                    "beat": { "type": "number", "minimum": 0,
                              "description": "new start, in beats; omit to leave it" },
                    "beats": { "type": "number", "exclusiveMinimum": 0,
                               "description": "new length, in beats; omit to leave it" },
                },
            }),
        },
        ToolSpec {
            name: "remove_clip",
            description: "Take a clip out of the arrangement. The clip itself and its notes \
                          survive — only the placement goes, so it can be placed again with \
                          add_clip. Undoable.",
            params: json!({
                "type": "object",
                "required": ["id", "track"],
                "properties": {
                    "id": { "type": "integer", "minimum": 0 },
                    "track": { "type": "integer", "minimum": 0 },
                },
            }),
        },
        ToolSpec {
            name: "add_clip",
            description: "Place a clip on a track. `clip` is a CLIP id (the `clip` field from \
                          `clips`), not a placement id — placing the same clip twice is how a \
                          part is repeated, and both placements share its notes.",
            params: json!({
                "type": "object",
                "required": ["clip", "track", "beat", "beats"],
                "properties": {
                    "clip": { "type": "integer", "minimum": 0 },
                    "track": { "type": "integer", "minimum": 0 },
                    "beat": { "type": "number", "minimum": 0 },
                    "beats": { "type": "number", "exclusiveMinimum": 0 },
                },
            }),
        },
        ToolSpec {
            name: "set_harmony",
            description: "Set the key from a point in the song onwards. `root` is a pitch \
                          class, 0 = C through 11 = B. `scale` is the engine's scale id: \
                          1 major, 2 minor, 3 dorian, 4 mixolydian.",
            params: json!({
                "type": "object",
                "required": ["root", "scale"],
                "properties": {
                    "root": { "type": "integer", "minimum": 0, "maximum": 11 },
                    "scale": { "type": "integer", "minimum": 0 },
                    "tick": { "type": "integer", "minimum": 0 },
                },
            }),
        },
        ToolSpec {
            name: "set_tempo",
            description: "Set the tempo in BPM. With no tick the whole song becomes this tempo;                           with a tick it inserts a tempo change at that point.",
            params: json!({
                "type": "object",
                "required": ["bpm"],
                "properties": {
                    "bpm": { "type": "number", "minimum": 10, "maximum": 1000 },
                    "tick": { "type": "integer", "minimum": 0 },
                },
            }),
        },
        ToolSpec {
            name: "set_mixer",
            description: "Set a track's gain, pan, mute or solo. Gain is in dB (0 is unity,                           negative is quieter); pan is -1 hard left to 1 hard right.",
            params: json!({
                "type": "object",
                "required": ["track"],
                "properties": {
                    "track": { "type": "integer", "minimum": 0 },
                    "gain_db": { "type": "number", "minimum": -60, "maximum": 12 },
                    "pan": { "type": "number", "minimum": -1, "maximum": 1 },
                    "mute": { "type": "boolean" },
                    "solo": { "type": "boolean" },
                },
            }),
        },
        ToolSpec {
            name: "set_loop",
            description: "Set the loop range, in absolute nanoticks. The end must be after                           the start.",
            params: json!({
                "type": "object",
                "required": ["start", "end"],
                "properties": {
                    "start": { "type": "integer", "minimum": 0 },
                    "end": { "type": "integer", "minimum": 1 },
                },
            }),
        },
        ToolSpec {
            name: "preview_note",
            description: "Sound a pitch on a track WITHOUT writing it — for auditioning.                           Held: send on=true, then on=false for the same pitch to release it.",
            params: json!({
                "type": "object",
                "required": ["pitch"],
                "properties": {
                    "track": { "type": "integer", "minimum": 0 },
                    "pitch": { "type": "integer", "minimum": 0, "maximum": 127 },
                    "velocity": { "type": "integer", "minimum": 1, "maximum": 127 },
                    "on": { "type": "boolean" },
                },
            }),
        },
    ]
}

/// Renders the manifest as a JSON array — the form an LLM harness ingests to
/// learn its tools.
pub fn manifest_json() -> String {
    serde_json::to_string_pretty(&tool_manifest())
        .unwrap_or_else(|e| format!("[{{\"error\":\"{e}\"}}]"))
}

/// `observe`, with or without a window.
///
/// Shape by default. The whole song's notes used to come back here and be pasted
/// into a model's context — 2.2 MB on a large session, past what can be sent at
/// all, and silently: the caller saw a prefix and believed it was the song.
fn observe_tool(handle: &EngineHandle, args: &Value) -> ToolResult {
    let window = args.get("from_beat").and_then(|v| v.as_f64()).map(|from| {
        let len = args
            .get("beats")
            .and_then(|v| v.as_f64())
            .filter(|v| *v > 0.0)
            .unwrap_or(crate::observe::DEFAULT_WINDOW_BEATS);
        let w = Window::beats(from, len);
        match arg_u64(args, "track") {
            Some(t) => w.on_track(t as u32),
            None => w,
        }
    });
    match serde_json::to_value(observe_window(handle, window)) {
        Ok(v) => ToolResult::ok(v),
        Err(e) => ToolResult::err(format!("serialize observation: {e}")),
    }
}

fn arg_u64(args: &Value, key: &str) -> Option<u64> {
    args.get(key).and_then(|v| v.as_u64())
}

/// Runs one tool against the engine. Unknown tools and malformed args are an
/// error result, never a silent no-op.
pub fn execute(handle: &EngineHandle, call: &ToolCall) -> ToolResult {
    match call.tool.as_str() {
        "observe" => observe_tool(handle, &call.args),
        "add_notes" => add_notes(handle, &call.args),
        "transport" => transport(handle, &call.args),
        "save" => named(handle, UiCommandType::SaveProject, &call.args, "saved"),
        "load" => named(handle, UiCommandType::LoadProject, &call.args, "loaded"),
        "set_track_name" => set_track_name(handle, &call.args),
        "undo" => undo_redo(handle, UiCommandType::Undo),
        "redo" => undo_redo(handle, UiCommandType::Redo),
        "delete_note" => delete_note(handle, &call.args),
        "add_track" => add_track(handle),
        "remove_track" => remove_track(handle, &call.args),
        "clips" => clips(handle),
        "move_clip" => move_clip(handle, &call.args),
        "trim_clip" => trim_clip(handle, &call.args),
        "remove_clip" => remove_clip(handle, &call.args),
        "add_clip" => add_clip(handle, &call.args),
        "set_harmony" => set_harmony(handle, &call.args),
        "set_tempo" => set_tempo(handle, &call.args),
        "set_mixer" => set_mixer(handle, &call.args),
        "set_loop" => set_loop(handle, &call.args),
        "preview_note" => preview_note(handle, &call.args),
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

    // Optimistic concurrency: each accepted write bumps the clip version by one,
    // so the next note's base_version is the previous plus one. Same protocol the
    // UI obeys — the agent is not privileged.
    let mut base = handle.clip_version();
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
/// One command with no arguments beyond a track and a tick, sent and awaited.
///
/// Every document tool below has the same shape — build a payload, send it, wait
/// for the clip version to move — so it is written once. The alternative is nine
/// copies of a twelve-field struct literal, and nine chances for one field to be
/// wrong in a way nothing catches: `base_version` in particular, which is what
/// makes an edit reconcile rather than race.
/// Send a command that does NOT change the clip, and do not wait for one.
///
/// `send_edit` below waits for the clip version to advance, which is right for a
/// note write and wrong for everything else: SetTrackMixer, SetTempo, AddTrack,
/// RemoveTrack and SetLoop move their own state and leave the clip version
/// alone. All five went through `send_edit`, stalled the full two second
/// timeout, and then reported `applied: false` — telling a model its edit had
/// failed when the edit had worked. A model that believes that tries again.
///
/// There is nothing to wait FOR here: the ring is ordered, so the command is
/// queued by the time this returns, and the next `observe` shows the result.
fn send_now(handle: &EngineHandle, p: UiCommandPayload, out: Value) -> ToolResult {
    if let Err(e) = handle.send_command(p) {
        return ToolResult::err(e);
    }
    let mut v = out;
    if let Value::Object(ref mut m) = v {
        m.insert("sent".into(), json!(true));
    }
    ToolResult::ok(v)
}

/// Send a CLIP edit and wait for the engine to apply it.
fn send_edit(handle: &EngineHandle, mut p: UiCommandPayload, out: Value) -> ToolResult {
    let base = handle.clip_version();
    p.base_version = base;
    if let Err(e) = handle.send_command(p) {
        return ToolResult::err(e);
    }
    let applied =
        handle.wait_for_clip_version(base, base.wrapping_add(1), std::time::Duration::from_secs(2));
    let mut v = out;
    if let Value::Object(ref mut m) = v {
        m.insert("applied".into(), json!(applied));
    }
    ToolResult::ok(v)
}

/// A payload with everything zeroed but the command. The struct has twelve fields
/// and most tools set three of them.
fn blank(cmd: UiCommandType) -> UiCommandPayload {
    UiCommandPayload {
        command_type: cmd as u16,
        flags: 0, track_id: 0, plugin_index: 0, note_pitch: 0, value0: 0,
        note_nanotick_lo: 0, note_nanotick_hi: 0,
        note_duration_lo: 0, note_duration_hi: 0, base_version: 0,
    }
}

fn delete_note(handle: &EngineHandle, args: &Value) -> ToolResult {
    let Some(track) = arg_u64(args, "track") else {
        return ToolResult::err("delete_note needs \"track\"");
    };
    let Some(tick) = arg_u64(args, "tick") else {
        return ToolResult::err("delete_note needs \"tick\"");
    };
    let mut p = blank(UiCommandType::DeleteNote);
    p.track_id = track as u32;
    p.note_nanotick_lo = (tick & 0xffff_ffff) as u32;
    p.note_nanotick_hi = (tick >> 32) as u32;
    send_edit(handle, p, json!({ "deleted": { "track": track, "tick": tick } }))
}

/// Append a track. No arguments: v1 of AddTrack always appends, because
/// inserting needs a display-order field the engine does not have yet.
fn add_track(handle: &EngineHandle) -> ToolResult {
    send_now(handle, blank(UiCommandType::AddTrack), json!({ "added": true }))
}

/// Remove a track by its STABLE id.
///
/// The engine tombstones the slot rather than compacting, so ids an agent is
/// holding stay valid across a removal — which is the whole reason to address
/// tracks by id rather than by position.
fn remove_track(handle: &EngineHandle, args: &Value) -> ToolResult {
    let Some(track) = arg_u64(args, "track") else {
        return ToolResult::err("remove_track needs \"track\"");
    };
    let mut p = blank(UiCommandType::RemoveTrack);
    p.track_id = track as u32;
    send_now(handle, p, json!({ "removed": track }))
}

/*
 * ── PLACEMENTS ─────────────────────────────────────────────────────────────
 *
 * BEATS IN, TICKS OUT. The model is given beats and this multiplies. A tool
 * that took nanoticks would be handing a language model a 960000x multiplication
 * on every call, and the failure mode of getting one wrong is not an error — it
 * is a clip a third of a beat late, which nothing reports and everything hears.
 *
 * Rounded rather than truncated, so `beat: 1.9999999` from a model that divided
 * something is bar 2 rather than one tick short of it.
 */
fn beats_to_ticks(b: f64) -> u64 {
    (b * NANOTICKS_PER_QUARTER as f64).round().max(0.0) as u64
}
fn ticks_to_beats(t: u64) -> f64 {
    // Two decimals: a beat is the unit, and a model reading "4.0" acts on it
    // more reliably than one reading "4.000000000000001".
    ((t as f64 / NANOTICKS_PER_QUARTER as f64) * 100.0).round() / 100.0
}

fn split_tick(p: &mut UiCommandPayload, v: u64, duration: bool) {
    if duration { p.note_duration_lo = v as u32; p.note_duration_hi = (v >> 32) as u32; }
    else { p.note_nanotick_lo = v as u32; p.note_nanotick_hi = (v >> 32) as u32; }
}

/// What is placed where. The one read that makes the other four addressable.
fn clips(handle: &EngineHandle) -> ToolResult {
    let mut out = Vec::new();
    for e in handle.read_clip_extents() {
        let end = e.name.iter().position(|&c| c == 0).unwrap_or(e.name.len());
        out.push(json!({
            "id": e.placement_id,
            "clip": e.clip_id,
            "track": e.track_id,
            "beat": ticks_to_beats(e.start_tick),
            "beats": ticks_to_beats(e.end_tick.saturating_sub(e.start_tick)),
            "name": String::from_utf8_lossy(&e.name[..end]).to_string(),
            // An audio region holds no notes, so a model must not try to write
            // any into it — and the refusal it would get names nothing useful.
            "audio": e.flags & 1 != 0,
        }));
    }
    ToolResult::ok(json!({ "clips": out }))
}

fn move_clip(handle: &EngineHandle, args: &Value) -> ToolResult {
    let (Some(id), Some(track)) = (arg_u64(args, "id"), arg_u64(args, "track")) else {
        return ToolResult::err("move_clip needs \"id\" and \"track\"");
    };
    let Some(beat) = args.get("beat").and_then(|v| v.as_f64()) else {
        return ToolResult::err("move_clip needs \"beat\" — where it should start");
    };
    if beat < 0.0 { return ToolResult::err("a clip cannot start before the song does"); }
    let mut p = blank(UiCommandType::MovePlacement);
    p.track_id = track as u32;
    p.value0 = id as u32;
    split_tick(&mut p, beats_to_ticks(beat), false);
    p.note_pitch = match arg_u64(args, "to_track") {
        Some(t) => t as u32,
        None => daw_bridge::layout::PLACEMENT_SAME_TRACK,
    };
    send_now(handle, p, json!({ "id": id, "beat": beat,
                               "to_track": args.get("to_track").cloned() }))
}

fn trim_clip(handle: &EngineHandle, args: &Value) -> ToolResult {
    let (Some(id), Some(track)) = (arg_u64(args, "id"), arg_u64(args, "track")) else {
        return ToolResult::err("trim_clip needs \"id\" and \"track\"");
    };
    let at = args.get("beat").and_then(|v| v.as_f64());
    let len = args.get("beats").and_then(|v| v.as_f64());
    // Both absent is a command that does nothing, and the model would read the
    // `sent: true` as "the trim worked".
    if at.is_none() && len.is_none() {
        return ToolResult::err("trim_clip needs \"beat\", \"beats\", or both —                                 with neither it would change nothing");
    }
    if at.is_some_and(|v| v < 0.0) { return ToolResult::err("a clip cannot start before 0"); }
    if len.is_some_and(|v| v <= 0.0) { return ToolResult::err("a clip must be longer than nothing"); }
    let un = daw_bridge::layout::PLACEMENT_UNCHANGED;
    let mut p = blank(UiCommandType::ResizePlacement);
    p.track_id = track as u32;
    p.value0 = id as u32;
    split_tick(&mut p, at.map_or(un, beats_to_ticks), false);
    split_tick(&mut p, len.map_or(un, beats_to_ticks), true);
    send_now(handle, p, json!({ "id": id, "beat": at, "beats": len }))
}

fn remove_clip(handle: &EngineHandle, args: &Value) -> ToolResult {
    let (Some(id), Some(track)) = (arg_u64(args, "id"), arg_u64(args, "track")) else {
        return ToolResult::err("remove_clip needs \"id\" and \"track\"");
    };
    let mut p = blank(UiCommandType::RemovePlacement);
    p.track_id = track as u32;
    p.value0 = id as u32;
    send_now(handle, p, json!({ "removed": id }))
}

fn add_clip(handle: &EngineHandle, args: &Value) -> ToolResult {
    let (Some(clip), Some(track)) = (arg_u64(args, "clip"), arg_u64(args, "track")) else {
        return ToolResult::err("add_clip needs \"clip\" and \"track\"");
    };
    let (Some(beat), Some(beats)) = (args.get("beat").and_then(|v| v.as_f64()),
                                     args.get("beats").and_then(|v| v.as_f64())) else {
        return ToolResult::err("add_clip needs \"beat\" and \"beats\"");
    };
    if beat < 0.0 { return ToolResult::err("a clip cannot start before the song does"); }
    if beats <= 0.0 { return ToolResult::err("a clip must be longer than nothing"); }
    let mut p = blank(UiCommandType::AddPlacement);
    p.track_id = track as u32;
    p.value0 = clip as u32;
    split_tick(&mut p, beats_to_ticks(beat), false);
    split_tick(&mut p, beats_to_ticks(beats), true);
    send_now(handle, p, json!({ "clip": clip, "track": track, "beat": beat, "beats": beats }))
}

/// Set the key at a point on the harmony timeline.
///
/// Root and scale ride in note_pitch and value0 — where the engine reads them —
/// and the command is validated against the HARMONY version, which is a
/// different counter from the clip's.
fn set_harmony(handle: &EngineHandle, args: &Value) -> ToolResult {
    let Some(root) = arg_u64(args, "root") else {
        return ToolResult::err("set_harmony needs \"root\" (0 = C .. 11 = B)");
    };
    let Some(scale) = arg_u64(args, "scale") else {
        return ToolResult::err("set_harmony needs \"scale\" (1 major, 2 minor, 3 dorian, 4 mixolydian)");
    };
    if root > 11 {
        return ToolResult::err("root is a pitch class: 0 = C through 11 = B");
    }
    let mut p = blank(UiCommandType::WriteHarmony);
    p.note_pitch = root as u32;
    p.value0 = scale as u32;
    let tick = arg_u64(args, "tick").unwrap_or(0);
    p.note_nanotick_lo = (tick & 0xffff_ffff) as u32;
    p.note_nanotick_hi = (tick >> 32) as u32;
    /*
     * The HARMONY version, not the clip's — and this is why it needs its own
     * send path rather than `send_edit`.
     *
     * `requireMatchingHarmonyVersion` guards WriteHarmony, and the only thing
     * that moves that counter is a harmony write. `send_edit` stamps
     * `clip_version()` and then waits for the CLIP version to advance, so this
     * tool quoted the wrong number and then waited for a counter that was never
     * going to move: refused by the engine, and reported as `applied: false`
     * after a two second stall. The doc comment above said as much while the
     * code did the opposite.
     *
     * The page had exactly this bug on the same command, from the other
     * direction — its socket layer overwrote the base with the clip version on
     * the way out. Same mistake, two codebases, because "base version" reads as
     * one idea and is two.
     */
    let base = handle.harmony_version();
    p.base_version = base;
    if let Err(e) = handle.send_command(p) {
        return ToolResult::err(e);
    }
    ToolResult::ok(json!({ "root": root, "scale": scale, "tick": tick, "base": base }))
}

fn set_tempo(handle: &EngineHandle, args: &Value) -> ToolResult {
    let Some(bpm) = args.get("bpm").and_then(|v| v.as_f64()) else {
        return ToolResult::err("set_tempo needs \"bpm\"");
    };
    if !(10.0..=1000.0).contains(&bpm) {
        return ToolResult::err("tempo must be between 10 and 1000 BPM");
    }
    let mut p = blank(UiCommandType::SetTempo);
    p.value0 = (bpm * 1000.0).round() as u32;
    // flags 1 = flatten the map to this one tempo, which is what "set the tempo"
    // means with no position given. A tick makes it a point instead.
    match arg_u64(args, "tick") {
        Some(t) => {
            p.note_nanotick_lo = (t & 0xffff_ffff) as u32;
            p.note_nanotick_hi = (t >> 32) as u32;
        }
        None => p.flags = 1,
    }
    send_now(handle, p, json!({ "bpm": bpm }))
}

fn set_mixer(handle: &EngineHandle, args: &Value) -> ToolResult {
    let Some(track) = arg_u64(args, "track") else {
        return ToolResult::err("set_mixer needs \"track\"");
    };
    let mut p = blank(UiCommandType::SetTrackMixer);
    p.track_id = track as u32;
    // Gain in millibels and pan in thousandths, as the engine carries them; the
    // tool takes dB and -1..1 because that is what a caller means, and converting
    // here keeps the unit confusion in one place rather than in every prompt.
    let gain_db = args.get("gain_db").and_then(|v| v.as_f64());
    let pan = args.get("pan").and_then(|v| v.as_f64());
    let mute = args.get("mute").and_then(|v| v.as_bool());
    let solo = args.get("solo").and_then(|v| v.as_bool());
    if gain_db.is_none() && pan.is_none() && mute.is_none() && solo.is_none() {
        return ToolResult::err("set_mixer needs at least one of gain_db, pan, mute, solo");
    }
    let millibels = (gain_db.unwrap_or(0.0) * 100.0).round() as i32;
    let thousandths = (pan.unwrap_or(0.0).clamp(-1.0, 1.0) * 1000.0).round() as i32;
    p.value0 = millibels as u32;
    p.note_pitch = thousandths as u32;
    p.flags = (if mute.unwrap_or(false) { 1 } else { 0 })
            | (if solo.unwrap_or(false) { 2 } else { 0 });
    send_now(handle, p, json!({
        "track": track, "gain_db": gain_db, "pan": pan, "mute": mute, "solo": solo }))
}

fn set_loop(handle: &EngineHandle, args: &Value) -> ToolResult {
    let (Some(start), Some(end)) = (arg_u64(args, "start"), arg_u64(args, "end")) else {
        return ToolResult::err("set_loop needs \"start\" and \"end\" in nanoticks");
    };
    if end <= start {
        return ToolResult::err("the loop's end must be after its start");
    }
    let mut p = blank(UiCommandType::SetLoopRange);
    p.note_nanotick_lo = (start & 0xffff_ffff) as u32;
    p.note_nanotick_hi = (start >> 32) as u32;
    p.note_duration_lo = (end & 0xffff_ffff) as u32;
    p.note_duration_hi = (end >> 32) as u32;
    send_now(handle, p, json!({ "start": start, "end": end }))
}

/// Sound a pitch WITHOUT writing it (kUiCommandType 45).
///
/// Deliberately not awaited on the clip version: a preview never touches the clip
/// store, so waiting for a version that will not move would block for the timeout
/// and then report `applied: false` about a note that played perfectly.
fn preview_note(handle: &EngineHandle, args: &Value) -> ToolResult {
    let Some(pitch) = arg_u64(args, "pitch").filter(|p| *p <= 127) else {
        return ToolResult::err("preview_note needs \"pitch\" in 0..127");
    };
    let mut p = blank(UiCommandType::PreviewNote);
    p.track_id = arg_u64(args, "track").unwrap_or(0) as u32;
    p.note_pitch = pitch as u32;
    p.value0 = arg_u64(args, "velocity").unwrap_or(100).min(127) as u32;
    p.flags = if args.get("on").and_then(|v| v.as_bool()).unwrap_or(true) { 1 } else { 0 };
    match handle.send_command(p) {
        Ok(()) => ToolResult::ok(json!({ "pitch": pitch, "on": p.flags == 1 })),
        Err(e) => ToolResult::err(e),
    }
}

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
        // The arms are read from execute() ITSELF rather than copied into a second
        // list here. The copy is what this test used to be, and it is the shape it
        // exists to prevent: adding a tool to the manifest and to the dispatch left
        // the test's private list stale, so it failed on a tool that was in fact
        // wired. A test that keeps its own copy of the thing it checks is checking
        // the copy.
        let src = include_str!("tools.rs");
        let body = &src[src.find("pub fn execute(").expect("execute exists")..];
        let body = &body[..body.find("\n}").expect("execute ends")];
        let arms: Vec<&str> = body
            .match_indices("\" =>")
            .filter_map(|(i, _)| {
                let head = &body[..i];
                head.rfind('"').map(|q| &head[q + 1..])
            })
            .collect();
        assert!(arms.len() > 5, "execute()'s arms were parsed: {arms:?}");
        // execute must recognize every advertised tool. We can't touch the engine
        // here, so we only assert the tool is not reported "unknown"; a missing
        // required arg is an acceptable (recognized) error.
        for spec in tool_manifest() {
            assert!(arms.contains(&spec.name),
                    "manifest tool {:?} has no dispatch arm in execute()", spec.name);
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
