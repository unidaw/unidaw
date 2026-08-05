//! The agent's tool interface: a discoverable manifest of what it can do, and a
//! typed dispatch that carries each call to the engine's command ring. Every tool
//! is declared once with a JSON-schema for its args, so a manifest, validation,
//! and docs all come from the same table — the same "one definition" rule the
//! row-op schema follows. Model-agnostic: an LLM harness maps its tool-call
//! format onto `ToolCall`/`execute`; nothing here talks to a model or a network.

use daw_bridge::control::EngineHandle;
use daw_bridge::grid::NANOTICKS_PER_QUARTER;
use daw_bridge::layout::{UiChainCommandPayload, UiCommandPayload, UiCommandType,
                         UiModLinkCommandPayload, UiModLinkUid16Payload,
                         UiModSourceValuePayload, UiPatcherPresetCommandPayload,
                         UiMarkerCommandPayload, UiArrangeTimeCommandPayload,
                         MOD_LINK_ID_AUTO, MOD_RATE_BLOCK,
                         MOD_SOURCE_MACRO, MOD_TARGET_VST_PARAM};
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
                          to it straight away. RETURNS THE NEW TRACK'S id as `track` — use \
                          that, do not assume it is the highest number you have seen, because \
                          the master track sits after the real ones and the new track takes \
                          the index the master used to have.",
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
        /*
         * THE ARRANGEMENT'S SPINE. A read and a write, because a section has no position of
         * its own — where it begins is the sum of the lengths before it — so an agent asked
         * to "make the chorus longer" has to see the order before it can change anything.
         */
        /*
         * SCRATCH CLIPS, which exist FOR this agent: it writes into its own copy of a clip, the
         * human compares with one command, and neither fork nor swap touches the undo stack — so
         * undo still undoes the last musical edit rather than the comparison.
         */
        ToolSpec {
            name: "shared_clips",
            description: "Which placements play the same clip, and which have been FORKED. Ask \
                          this BEFORE editing notes: two placements of one clip are two separate \
                          appearances of the SAME music, so an edit inside one changes all of \
                          them. If that is not what you want, fork_placement first.",
            params: json!({
                "type": "object",
                "properties": { "track": { "type": "integer", "minimum": 0 } },
            }),
        },
        ToolSpec {
            name: "fork_placement",
            description: "Give one placement its own copy of the clip it plays, keeping the \
                          original as its ALTERNATE. Use this before editing a shared clip when \
                          the change should apply here only. Nothing is lost and nothing \
                          destructive enters the undo stack — the other version is one \
                          swap_placement_clip away.",
            params: json!({
                "type": "object",
                "required": ["track", "placement"],
                "properties": { "track": { "type": "integer", "minimum": 0 },
                                "placement": { "type": "integer", "minimum": 0 } },
            }),
        },
        ToolSpec {
            name: "swap_placement_clip",
            description: "Exchange a placement's clip with its alternate — the A/B. What \
                          PLAYS is always the placement's clip, so there is no audition mode to \
                          leave and nothing to get out of step with what is heard.",
            params: json!({
                "type": "object",
                "required": ["track", "placement"],
                "properties": { "track": { "type": "integer", "minimum": 0 },
                                "placement": { "type": "integer", "minimum": 0 } },
            }),
        },
        ToolSpec {
            name: "keep_placement_clip",
            description: "Drop a placement's alternate and keep what is playing. The decision, \
                          after the comparison — keeping both is doing nothing, which is \
                          the right default while you are unsure.",
            params: json!({
                "type": "object",
                "required": ["track", "placement"],
                "properties": { "track": { "type": "integer", "minimum": 0 },
                                "placement": { "type": "integer", "minimum": 0 } },
            }),
        },
        ToolSpec {
            name: "markers",
            description: "The song's markers, in order: id, name, the bar and beat they fall on, \
                          and the tick. A marker is a NAMED TICK and stores no length — two \
                          adjacent markers are a span, so a section's length is the next \
                          marker's tick minus this one's, and the last one runs to song_end.",
            params: json!({ "type": "object", "properties": {} }),
        },
        ToolSpec {
            name: "edit_marker",
            description: "Add, remove, rename or move a marker. op=add needs tick (and takes \
                          name, color); op=remove/rename/move need id; op=rename needs name; \
                          op=move needs tick. These ops are TOTAL: they move no music and \
                          cannot fail except on a bad id. To move MUSIC, use insert_time.",
            params: json!({
                "type": "object",
                "required": ["op"],
                "properties": {
                    "op": { "type": "string", "enum": ["add", "remove", "rename", "move"] },
                    "id": { "type": "integer", "minimum": 1 },
                    "tick": { "type": "integer", "minimum": 0 },
                    "name": { "type": "string" },
                    "color": { "type": "integer", "minimum": 0 },
                },
            }),
        },
        ToolSpec {
            name: "insert_time",
            description: "Insert or remove BARS of arrangement time at a tick, moving everything \
                          at or after it: every placement on every track, the tempo points, the \
                          key changes, the automation points, the meter points and the markers \
                          — in ONE transaction that is refused whole and undone whole. \
                          Positive bars insert, negative remove. \
                          Removing bars that hold material is REFUSED, naming what is in the way. \
                          Bars and not ticks, because a bar's length depends on the meter in \
                          force there and the engine holds that map.",
            params: json!({
                "type": "object",
                "required": ["tick", "bars"],
                "properties": {
                    "tick": { "type": "integer", "minimum": 0 },
                    "bars": { "type": "integer" },
                },
            }),
        },
        ToolSpec {
            name: "set_time_signature",
            description: "Set the meter at a tick, like 7/8 — mid-song meter changes are \
                          authorable. `flatten` replaces the whole map with this one signature. \
                          Bar NUMBERING is a prefix sum through this map, which is why markers \
                          carry a resolved bar and beat rather than a tick to divide.",
            params: json!({
                "type": "object",
                "required": ["signature"],
                "properties": {
                    "signature": { "type": "string" },
                    "tick": { "type": "integer", "minimum": 0 },
                    "flatten": { "type": "boolean" },
                },
            }),
        },
        /*
         * THE DEVICE RACK. An agent could not touch it at all — not insert a plugin, not
         * remove one, not reorder, not bypass — which meant "put a reverb after the delay"
         * was outside its reach entirely, and ORDER IS WHAT A CHAIN IS.
         */
        ToolSpec {
            name: "device_params",
            description: "The parameters a device publishes: index, name, current value, and \
                          the stable uid the engine addresses it by. Needed before modulating \
                          anything — modulation is addressed by uid, not by index.",
            params: json!({
                "type": "object",
                "properties": { "track": { "type": "integer", "minimum": 0 },
                                "device": { "type": "integer", "minimum": 0 } },
            }),
        },
        ToolSpec {
            name: "add_device",
            description: "Insert a device into a track's chain at a position (0 is first). \
                          kind: sampler, vst_effect, vst_instrument or patcher. `sampler` is the \
                          engine's own instrument and needs nothing else; a VST needs `plugin`, \
                          the name as the plugin catalogue reports it.",
            params: json!({
                "type": "object",
                "required": ["track"],
                "properties": {
                    "track": { "type": "integer", "minimum": 0 },
                    "kind": { "type": "string",
                              "enum": ["sampler", "vst_effect", "vst_instrument", "patcher"] },
                    "position": { "type": "integer", "minimum": 0 },
                },
            }),
        },
        ToolSpec {
            name: "remove_device",
            description: "Take a device out of a chain. NOT undoable, and the device's \
                          settings go with it.",
            params: json!({
                "type": "object",
                "required": ["track", "device"],
                "properties": { "track": { "type": "integer", "minimum": 0 },
                                "device": { "type": "integer", "minimum": 0 } },
            }),
        },
        ToolSpec {
            name: "move_device",
            description: "Reorder a chain: `position` is where the device ENDS UP, from 0. \
                          The same compressor before and after a distortion are two \
                          different sounds.",
            params: json!({
                "type": "object",
                "required": ["track", "device", "position"],
                "properties": { "track": { "type": "integer", "minimum": 0 },
                                "device": { "type": "integer", "minimum": 0 },
                                "position": { "type": "integer", "minimum": 0 } },
            }),
        },
        ToolSpec {
            name: "set_bypass",
            description: "Switch a device off without removing it — the way to hear what it \
                          is doing.",
            params: json!({
                "type": "object",
                "required": ["track", "device", "on"],
                "properties": { "track": { "type": "integer", "minimum": 0 },
                                "device": { "type": "integer", "minimum": 0 },
                                "on": { "type": "boolean" } },
            }),
        },
        /*
         * MODULATION. Three commands make one working link, and every one of them is
         * load-bearing — the description says so, because an agent that sends only the first
         * gets a link the engine accepts and never applies.
         */
        ToolSpec {
            name: "modulate",
            description: "Make a parameter move: a macro knob on one device drives a \
                          parameter on a LATER one. Needs the parameter's uid from \
                          device_params — the engine addresses modulation by uid and ignores \
                          the index. MODULATION FLOWS FORWARD: the source device must sit \
                          STRICTLY EARLIER in the chain than its target, so a parameter on \
                          the first device cannot be modulated. depth 1 sweeps the whole \
                          range. This also turns the knob to `value`, because a macro nobody \
                          has turned is skipped and the link would be inaudible.",
            params: json!({
                "type": "object",
                "required": ["track", "source_device", "target_device", "param_uid"],
                "properties": {
                    "track": { "type": "integer", "minimum": 0 },
                    "source_device": { "type": "integer", "minimum": 0 },
                    "target_device": { "type": "integer", "minimum": 0 },
                    "param_index": { "type": "integer", "minimum": 0 },
                    "param_uid": { "type": "string" },
                    "depth": { "type": "number", "minimum": 0, "maximum": 1 },
                    "bias": { "type": "number", "minimum": -1, "maximum": 1 },
                    "value": { "type": "number", "minimum": 0, "maximum": 1 },
                },
            }),
        },
        ToolSpec {
            name: "unmodulate",
            description: "Remove a modulation link. The engine validates the link's devices \
                          before it will remove it, so source_device and target_device are \
                          required even though the id alone should be enough.",
            params: json!({
                "type": "object",
                "required": ["track", "link", "source_device", "target_device"],
                "properties": {
                    "track": { "type": "integer", "minimum": 0 },
                    "link": { "type": "integer", "minimum": 1 },
                    "source_device": { "type": "integer", "minimum": 0 },
                    "target_device": { "type": "integer", "minimum": 0 },
                    "param_index": { "type": "integer", "minimum": 0 },
                },
            }),
        },
        ToolSpec {
            name: "set_macro",
            description: "Turn a macro knob, 0 to 1. This is what MOVES a modulated \
                          parameter — the link says how far, the knob says where.",
            params: json!({
                "type": "object",
                "required": ["track", "device", "value"],
                "properties": { "track": { "type": "integer", "minimum": 0 },
                                "device": { "type": "integer", "minimum": 0 },
                                "source": { "type": "integer", "minimum": 0 },
                                "value": { "type": "number", "minimum": 0, "maximum": 1 } },
            }),
        },
        ToolSpec {
            name: "automation",
            description: "Which parameters are automated: track, param, how many points, and \
                          whether the curve steps or ramps. The LIST, not the curves — ask for \
                          one lane's points with automation_points.",
            params: json!({
                "type": "object",
                "properties": { "track": { "type": "integer", "minimum": 0 } },
            }),
        },
        ToolSpec {
            name: "automation_points",
            description: "One automation lane's points, as [nanotick, value] with value \
                          normalised 0..1. `found: false` is an ANSWER — nothing automates that \
                          parameter — not a failure. The value BETWEEN points is deliberately \
                          not given: interpolating it here would be a second implementation of \
                          what the engine plays, and the two could disagree.",
            params: json!({
                "type": "object",
                "required": ["track", "param"],
                "properties": {
                    "track": { "type": "integer", "minimum": 0 },
                    "param": { "type": "string" },
                    "target_plugin_index": { "type": "integer", "minimum": 0 },
                },
            }),
        },
        ToolSpec {
            name: "write_automation_point",
            description: "Write one automation point: a parameter reaches `value` (normalised \
                          0..1) at `tick`. Writing the same tick again REPLACES that point \
                          rather than adding a second one, so a sweep is a series of ticks. \
                          `discrete` belongs to the CLIP, not the point — it is applied when the \
                          clip is created and ignored afterwards, because a curve that changed \
                          shape halfway through would be unreadable.",
            params: json!({
                "type": "object",
                "required": ["track", "param", "tick", "value"],
                "properties": {
                    "track": { "type": "integer", "minimum": 0 },
                    "param": { "type": "string" },
                    "tick": { "type": "integer", "minimum": 0 },
                    "value": { "type": "number", "minimum": 0, "maximum": 1 },
                    "discrete": { "type": "boolean" },
                    "target_plugin_index": { "type": "integer", "minimum": 0 },
                },
            }),
        },
        ToolSpec {
            name: "set_lane_quantize",
            description: "Pull a track's notes toward a grid WITHOUT moving them: the \
                          authored ticks are unchanged on disk and only where the notes \
                          SOUND changes. grid is a subdivision name or `off`. strength is a \
                          percentage, swing -50..50.",
            params: json!({
                "type": "object",
                "required": ["track", "grid"],
                "properties": {
                    "track": { "type": "integer", "minimum": 0 },
                    "grid": { "type": "string",
                              "enum": ["off", "1/4", "1/8", "1/16", "1/32", "1/4t", "1/8t", "1/16t"] },
                    "strength": { "type": "integer", "minimum": 0, "maximum": 100 },
                    "swing": { "type": "integer", "minimum": -50, "maximum": 50 },
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

/// Sixteen bytes as 32 hex characters, or "" for the all-zero sentinel.
///
/// Zeros mean UNSET — the engine initialises a uid16 that way — and hexed naively that is 32
/// characters of "0", which is a perfectly truthy string. A caller asking "does this
/// parameter have a stable id?" would answer yes and then modulate nothing.
fn hex16(b: &[u8; 16]) -> String {
    if b.iter().all(|x| *x == 0) { return String::new(); }
    let mut s = String::with_capacity(32);
    for x in b { s.push_str(&format!("{x:02x}")); }
    s
}

fn arg_f64(args: &Value, key: &str) -> Option<f64> {
    args.get(key).and_then(|v| v.as_f64()).filter(|v| v.is_finite())
}

fn arg_str<'a>(args: &'a Value, key: &str) -> Option<&'a str> {
    args.get(key).and_then(|v| v.as_str())
}

/// Which placements share a clip, and which have been forked.
///
/// THE READ COMES FIRST, and it is the half that was missing everywhere: two placements of one
/// clip are two appearances of the SAME music, and nothing said so — so an edit inside one
/// changed all of them silently. A model that can fork but cannot see what is shared will fork
/// the wrong things and leave the right ones alone.
fn shared_clips(handle: &EngineHandle, args: &Value) -> ToolResult {
    const HAS_ALTERNATE: u32 = 1 << 24;      // kUiClipExtentHasAlternate (v31)
    let want = arg_u64(args, "track").map(|t| t as u32);
    let extents = handle.read_clip_extents();
    // Counted over ALL of them, not the filtered set: a clip is shared whether or not its other
    // appearances are on the track being asked about, and a count that changed with the filter
    // would be a different number for the same question.
    let mut uses: std::collections::HashMap<u32, u32> = std::collections::HashMap::new();
    for e in &extents { *uses.entry(e.clip_id).or_insert(0) += 1; }
    let list: Vec<Value> = extents.iter()
        .filter(|e| want.is_none() || want == Some(e.track_id))
        .map(|e| json!({
            "placement": e.placement_id,
            "clip": e.clip_id,
            "track": e.track_id,
            "name": String::from_utf8_lossy(&e.name).trim_end_matches('\0').to_string(),
            "start_tick": e.start_tick,
            "end_tick": e.end_tick,
            // How many appearances play this clip. 1 means an edit here changes only this one.
            "appearances": uses.get(&e.clip_id).copied().unwrap_or(1),
            // Forked: this appearance has its own copy with another version behind it.
            "forked": (e.flags & HAS_ALTERNATE) != 0,
        }))
        .collect();
    ToolResult::ok(json!({ "placements": list }))
}

/// One scratch op on one placement. They all ride UiCommandPayload: track in `track_id`, the
/// PLACEMENT in `value0` — not the clip, because forking is about one appearance and naming the
/// clip would name the thing every appearance shares.
fn scratch(handle: &EngineHandle, args: &Value, cmd: UiCommandType) -> ToolResult {
    let (Some(track), Some(placement)) = (arg_u64(args, "track"), arg_u64(args, "placement")) else {
        return ToolResult::err("this needs \"track\" and \"placement\" — \
                                the appearance to act on, from shared_clips");
    };
    let mut p = blank(cmd);
    p.track_id = track as u32;
    p.value0 = placement as u32;
    send_now(handle, p, json!({ "track": track, "placement": placement }))
}

/// The song's MARKERS, with the span each one begins.
///
/// `span_end` is DERIVED here — the next marker's tick, or the song's end for the last — and it
/// is derived rather than stored because a marker has no length. Given to the caller anyway, so
/// a model asked to "make the chorus longer" does not have to do subtraction to find out where
/// the chorus ends.
fn markers(handle: &EngineHandle) -> ToolResult {
    let Some(sum) = handle.read_arrange_summary() else {
        return ToolResult::err("the engine publishes no arrangement summary");
    };
    let n = (sum.marker_count as usize).min(sum.markers.len());
    let list: Vec<Value> = sum.markers[..n].iter().enumerate().map(|(i, m)| {
        let end = if i + 1 < n { sum.markers[i + 1].nanotick } else { sum.song_end_tick };
        json!({
            "id": m.id,
            "name": String::from_utf8_lossy(&m.name).trim_end_matches('\0').to_string(),
            "bar": m.bar,
            "beat": m.beat,
            "nanotick": m.nanotick,
            // The span this marker begins. Derived, not stored — see the doc.
            "span_end": end,
        })
    }).collect();
    ToolResult::ok(json!({
        "markers": list,
        // Truncation is REPORTED. A short list that says nothing reads as the whole song.
        "truncated": sum.markers_truncated,
        // The furthest PLACEMENT, which is not the last marker: material can sit past every
        // marker, where it plays and is unnamed.
        "song_end_tick": sum.song_end_tick,
    }))
}

/// One of the four marker ops. All TOTAL: no material moves, and only a bad id can refuse them.
fn edit_marker(handle: &EngineHandle, args: &Value) -> ToolResult {
    let Some(op) = arg_str(args, "op") else {
        return ToolResult::err("edit_marker needs \"op\": add, remove, rename or move");
    };
    let (cmd, addressed) = match op {
        "add" => (UiCommandType::AddMarker, false),
        "remove" => (UiCommandType::RemoveMarker, true),
        "rename" => (UiCommandType::RenameMarker, true),
        "move" => (UiCommandType::MoveMarker, true),
        _ => return ToolResult::err("op must be add, remove, rename or move"),
    };
    let id = arg_u64(args, "id").unwrap_or(0) as u32;
    if addressed && id == 0 {
        return ToolResult::err(format!("edit_marker op {op:?} needs the \"id\" of an existing marker"));
    }
    // A move with no destination would put the marker at tick 0 — the one place nobody means.
    if op == "move" && arg_u64(args, "tick").is_none() {
        return ToolResult::err("a marker move needs the \"tick\" to move it to");
    }
    let tick = arg_u64(args, "tick").unwrap_or(0);
    let mut name = [0u8; 20];
    if let Some(t) = arg_str(args, "name") {
        let b = t.as_bytes();
        let take = b.len().min(name.len());
        name[..take].copy_from_slice(&b[..take]);
    } else if op == "rename" {
        return ToolResult::err("a rename needs a \"name\"");
    }
    let p = UiMarkerCommandPayload {
        command_type: cmd as u16,
        flags: 0,
        marker_id: id,
        nanotick_lo: (tick & 0xffff_ffff) as u32,
        nanotick_hi: (tick >> 32) as u32,
        color_rgb: arg_u64(args, "color").unwrap_or(0) as u32,
        name,
    };
    match handle.send_marker_command(p) {
        Ok(()) => ToolResult::ok(json!({ "sent": true, "op": op, "id": id, "tick": tick })),
        Err(e) => ToolResult::err(e),
    }
}

/// Insert or remove bars of arrangement time. The one op that moves music.
fn insert_time(handle: &EngineHandle, args: &Value) -> ToolResult {
    let Some(tick) = arg_u64(args, "tick") else {
        return ToolResult::err("insert_time needs \"tick\" — where the time is inserted or removed");
    };
    let bars = args.get("bars").and_then(|v| v.as_i64()).unwrap_or(0);
    if bars == 0 {
        // Zero bars is not a small edit, it is no edit — and sending it would spend a whole
        // transaction and an undo entry on nothing.
        return ToolResult::err("insert_time needs \"bars\": positive to insert, negative to remove");
    }
    let p = UiArrangeTimeCommandPayload {
        command_type: UiCommandType::InsertRemoveTime as u16,
        flags: 0,
        nanotick_lo: (tick & 0xffff_ffff) as u32,
        nanotick_hi: (tick >> 32) as u32,
        delta: bars.clamp(i32::MIN as i64, i32::MAX as i64) as i32,
        numerator: 0, denominator: 0,
        reserved0: 0, reserved1: 0, reserved2: 0, reserved3: 0,
    };
    match handle.send_arrange_time_command(p) {
        Ok(()) => ToolResult::ok(json!({ "sent": true, "tick": tick, "bars": bars })),
        Err(e) => ToolResult::err(e),
    }
}

/// A meter point. Mid-song signatures are authorable from v29.
fn set_time_signature(handle: &EngineHandle, args: &Value) -> ToolResult {
    let Some(sig) = arg_str(args, "signature") else {
        return ToolResult::err("set_time_signature needs \"signature\", like \"7/8\"");
    };
    let mut parts = sig.split('/');
    let num: u32 = parts.next().and_then(|x| x.trim().parse().ok()).unwrap_or(0);
    let den: u32 = parts.next().and_then(|x| x.trim().parse().ok()).unwrap_or(0);
    // A zero denominator divides by zero in every bar computation downstream, and one that is
    // not a power of two is not a signature music uses.
    if num == 0 || den == 0 || !den.is_power_of_two() {
        return ToolResult::err("a time signature is beats/note-value, like 4/4 or 7/8");
    }
    let tick = arg_u64(args, "tick").unwrap_or(0);
    let p = UiArrangeTimeCommandPayload {
        command_type: UiCommandType::SetTimeSignature as u16,
        flags: if args.get("flatten").and_then(|v| v.as_bool()).unwrap_or(false) {
            daw_bridge::layout::UI_TIME_SIG_FLATTEN
        } else { 0 },
        nanotick_lo: (tick & 0xffff_ffff) as u32,
        nanotick_hi: (tick >> 32) as u32,
        delta: 0,
        numerator: num,
        denominator: den,
        reserved0: 0, reserved1: 0, reserved2: 0, reserved3: 0,
    };
    match handle.send_arrange_time_command(p) {
        Ok(()) => ToolResult::ok(json!({ "sent": true, "signature": sig, "tick": tick })),
        Err(e) => ToolResult::err(e),
    }
}

/// A device's parameters, with the uid modulation is addressed by.
///
/// ASK, THEN READ. The region holds ONE device at a time — whichever host answered last — so
/// reading it without asking returns some other device's parameters, with a plausible name on
/// them. `RequestDeviceParams` makes the engine ask that host and rewrite the region, and the
/// short wait is for the round trip through the plugin host, which is out of process.
///
/// If the region still names a different device when the wait is up, that is REPORTED rather
/// than returned as if it were the answer: a model handed the wrong plugin's parameter list
/// will modulate the wrong plugin, confidently.
fn device_params(handle: &EngineHandle, args: &Value) -> ToolResult {
    let (Some(track), Some(device)) = (arg_u64(args, "track"), arg_u64(args, "device")) else {
        return ToolResult::err("device_params needs \"track\" and \"device\"");
    };
    let mut req = blank(UiCommandType::RequestDeviceParams);
    req.track_id = track as u32;
    req.value0 = device as u32;
    if let Err(e) = handle.send_command(req) { return ToolResult::err(e); }
    // Poll rather than sleep once: a host that answers in 20ms should not cost 400.
    let mut view = handle.read_device_params();
    for _ in 0..40 {
        if view.track_id == track as u32 && view.device_id == device as u32 && !view.params.is_empty() {
            break;
        }
        std::thread::sleep(std::time::Duration::from_millis(25));
        view = handle.read_device_params();
    }
    if view.track_id != track as u32 || view.device_id != device as u32 {
        return ToolResult::err(format!(
            "the engine has not answered for track {track} device {device} — the region still \
             holds track {} device {} ({:?}). The plugin host may still be starting.",
            view.track_id, view.device_id, view.device_name));
    }
    ToolResult::ok(json!({
        "track": view.track_id,
        "device": view.device_id,
        "name": view.device_name,
        // The uid as HEX, not as sixteen numbers. `uid16` is a byte array, and json! would
        // serialise it as `[10,27,...]` — which a model would then have to hex-encode itself
        // before it could pass it back to `modulate`, and would sometimes get wrong. One
        // representation, produced where the bytes are.
        "params": view.params.iter().map(|q| json!({
            "index": q.index, "name": q.name, "value": q.value,
            "uid": hex16(&q.uid16),
        })).collect::<Vec<_>>(),
    }))
}

/// A chain payload with the common fields filled in.
fn chain_blank(cmd: UiCommandType, track: u32) -> UiChainCommandPayload {
    UiChainCommandPayload {
        command_type: cmd as u16,
        flags: 0,
        track_id: track,
        // 0 is "whatever you hold" — the engine arbitrates chain edits on its own version and
        // an agent has no reason to know one.
        base_version: 0,
        device_id: 0,
        device_kind: 0,
        insert_index: 0,
        patcher_node_id: 0,
        host_slot_index: 0,
        bypass: 0,
        reserved: [0u8; 4],
    }
}

fn add_device(handle: &EngineHandle, args: &Value) -> ToolResult {
    let Some(track) = arg_u64(args, "track") else {
        return ToolResult::err("add_device needs \"track\"");
    };
    // The engine's DeviceKind numbering, mirrored from apps/device.h through the chain
    // snapshot's `kind`. Named here rather than passed as a number, for the reason the
    // sidecar's mod kinds are named: a literal that means something else after the next enum
    // change is the kind that outlives its meaning.
    let name = arg_str(args, "kind").unwrap_or("vst_effect");
    let Some(kind) = device_kind_code(name) else {
        return ToolResult::err(format!("unknown device kind {name:?}"));
    };
    let mut p = chain_blank(UiCommandType::AddDevice, track as u32);
    p.device_kind = kind;
    p.insert_index = arg_u64(args, "position").unwrap_or(0) as u32;
    match handle.send_chain_command(p) {
        Ok(()) => ToolResult::ok(json!({ "sent": true, "track": track, "kind": kind })),
        Err(e) => ToolResult::err(e),
    }
}

fn chain_edit(handle: &EngineHandle, args: &Value, cmd: UiCommandType) -> ToolResult {
    let (Some(track), Some(device)) = (arg_u64(args, "track"), arg_u64(args, "device")) else {
        return ToolResult::err("this needs \"track\" and \"device\"");
    };
    let mut p = chain_blank(cmd, track as u32);
    p.device_id = device as u32;
    match handle.send_chain_command(p) {
        Ok(()) => ToolResult::ok(json!({ "sent": true, "track": track, "device": device })),
        Err(e) => ToolResult::err(e),
    }
}

fn move_device(handle: &EngineHandle, args: &Value) -> ToolResult {
    let Some(pos) = arg_u64(args, "position") else {
        return ToolResult::err("move_device needs \"position\" — where the device ends up, from 0");
    };
    let (Some(track), Some(device)) = (arg_u64(args, "track"), arg_u64(args, "device")) else {
        return ToolResult::err("move_device needs \"track\" and \"device\"");
    };
    let mut p = chain_blank(UiCommandType::MoveDevice, track as u32);
    p.device_id = device as u32;
    p.insert_index = pos as u32;
    match handle.send_chain_command(p) {
        Ok(()) => ToolResult::ok(json!({ "sent": true, "device": device, "position": pos })),
        Err(e) => ToolResult::err(e),
    }
}

fn set_bypass(handle: &EngineHandle, args: &Value) -> ToolResult {
    let (Some(track), Some(device)) = (arg_u64(args, "track"), arg_u64(args, "device")) else {
        return ToolResult::err("set_bypass needs \"track\" and \"device\"");
    };
    let on = args.get("on").and_then(|v| v.as_bool()).unwrap_or(true);
    let mut p = chain_blank(UiCommandType::UpdateDevice, track as u32);
    p.device_id = device as u32;
    // bit0 of flags says "the bypass field is meaningful" — an UpdateDevice that does not set
    // it would carry a bypass of 0 and switch every device back on.
    p.flags = 1;
    p.bypass = if on { 1 } else { 0 };
    match handle.send_chain_command(p) {
        Ok(()) => ToolResult::ok(json!({ "sent": true, "device": device, "bypassed": on })),
        Err(e) => ToolResult::err(e),
    }
}

/// Make a modulation link, name its parameter, and turn the knob. THREE commands.
///
/// All three, because a link the engine accepts moves nothing without them: it addresses a
/// VST parameter by uid16 alone (so an unnamed link is inert), and it resolves a source from
/// the track's source states (so a macro nobody has turned is skipped). An agent sending only
/// the first would get an accepted, published, inaudible modulation.
///
/// The uid CANNOT ride along with the add: `SetModLinkUid16` matches a link id exactly and the
/// engine assigns the id, so naming it needs the id back first. Here the agent passes the
/// link id it wants — an explicit id rather than AUTO — which is the one way to name it in the
/// same call. `max(existing) + 1` is not knowable from this side, so an unused id is asked for
/// and a collision is reported rather than guessed around.
fn modulate(handle: &EngineHandle, args: &Value) -> ToolResult {
    let (Some(track), Some(src), Some(dst)) = (arg_u64(args, "track"),
                                               arg_u64(args, "source_device"),
                                               arg_u64(args, "target_device")) else {
        return ToolResult::err("modulate needs \"track\", \"source_device\" and \"target_device\"");
    };
    let Some(uid_hex) = arg_str(args, "param_uid") else {
        return ToolResult::err("modulate needs \"param_uid\" — from device_params. \
                                The engine addresses modulation by uid and ignores the index.");
    };
    if uid_hex.len() != 32 || !uid_hex.bytes().all(|c| c.is_ascii_hexdigit()) {
        return ToolResult::err("a param_uid is 32 hex characters");
    }
    if src == dst {
        // Strictly earlier, which is the APPLIER's rule. The command validator is looser and
        // would accept this, and then nothing would ever move.
        return ToolResult::err("a modulation source must sit STRICTLY EARLIER in the chain \
                                than its target — same device is accepted by the engine and \
                                then never applied");
    }
    let mut uid16 = [0u8; 16];
    for i in 0..16 {
        uid16[i] = u8::from_str_radix(&uid_hex[i * 2..i * 2 + 2], 16).unwrap_or(0);
    }
    let link = arg_u64(args, "link").unwrap_or(0) as u32;
    let flags = (MOD_SOURCE_MACRO & 0x0f) | ((MOD_TARGET_VST_PARAM & 0x0f) << 4)
              | ((MOD_RATE_BLOCK & 0x03) << 8) | (1 << 10);
    let add = UiModLinkCommandPayload {
        command_type: UiCommandType::AddModLink as u16,
        flags,
        track_id: track as u32,
        base_version: 0,
        link_id: if link == 0 { MOD_LINK_ID_AUTO } else { link },
        source_device_id: src as u32,
        source_id: arg_u64(args, "source").unwrap_or(0) as u32,
        target_device_id: dst as u32,
        target_id: arg_u64(args, "param_index").unwrap_or(0) as u32,
        depth: arg_f64(args, "depth").unwrap_or(1.0).clamp(0.0, 1.0) as f32,
        bias: arg_f64(args, "bias").unwrap_or(0.0).clamp(-1.0, 1.0) as f32,
    };
    if let Err(e) = handle.send_mod_link_command(add) { return ToolResult::err(e); }
    // The parameter's name, if the caller chose an explicit id. With AUTO the engine assigns
    // one this side cannot predict, so the naming has to be a second call — said plainly in
    // the result rather than left as a link that quietly does nothing.
    let named = link != 0;
    if named {
        let uid = UiModLinkUid16Payload {
            command_type: UiCommandType::SetModLinkUid16 as u16,
            flags: 0,
            track_id: track as u32,
            base_version: 0,
            link_id: link,
            uid16,
            reserved: [0u8; 8],
        };
        if let Err(e) = handle.send_mod_link_uid16(uid) { return ToolResult::err(e); }
    }
    // The knob. Half by default, so the link is audible: at 0 it is indistinguishable from a
    // link that failed.
    let value = arg_f64(args, "value").unwrap_or(0.5).clamp(0.0, 1.0) as f32;
    let knob = UiModSourceValuePayload {
        command_type: UiCommandType::SetModSourceValue as u16,
        flags: MOD_SOURCE_MACRO,
        track_id: track as u32,
        base_version: 0,
        source_device_id: src as u32,
        source_id: arg_u64(args, "source").unwrap_or(0) as u32,
        value,
        reserved: [0u8; 16],
    };
    if let Err(e) = handle.send_mod_source_value(knob) { return ToolResult::err(e); }
    ToolResult::ok(json!({
        "sent": true, "link": link, "named": named, "macro": value,
        "note": if named { Value::Null } else { json!(
            "no \"link\" id was given, so the engine assigned one and the parameter is NOT \
             named yet — the link will not move anything until SetModLinkUid16 names it. \
             Pass an unused link id to have this call name it.") },
    }))
}

fn unmodulate(handle: &EngineHandle, args: &Value) -> ToolResult {
    let (Some(track), Some(link)) = (arg_u64(args, "track"), arg_u64(args, "link")) else {
        return ToolResult::err("unmodulate needs \"track\" and \"link\"");
    };
    let (Some(src), Some(dst)) = (arg_u64(args, "source_device"), arg_u64(args, "target_device")) else {
        // Required because the ENGINE requires them: RemoveModLink is handled after the same
        // device validation as an add, so a removal naming only the link is refused as
        // invalid_device — on the engine's log, where nothing here can read it.
        return ToolResult::err("unmodulate needs \"source_device\" and \"target_device\" — \
                                the engine validates them before it will remove a link");
    };
    let flags = (MOD_SOURCE_MACRO & 0x0f) | ((MOD_TARGET_VST_PARAM & 0x0f) << 4)
              | ((MOD_RATE_BLOCK & 0x03) << 8) | (1 << 10);
    let p = UiModLinkCommandPayload {
        command_type: UiCommandType::RemoveModLink as u16,
        flags,
        track_id: track as u32,
        base_version: 0,
        link_id: link as u32,
        source_device_id: src as u32,
        source_id: 0,
        target_device_id: dst as u32,
        target_id: arg_u64(args, "param_index").unwrap_or(0) as u32,
        depth: 0.0,
        bias: 0.0,
    };
    match handle.send_mod_link_command(p) {
        Ok(()) => ToolResult::ok(json!({ "sent": true, "link": link })),
        Err(e) => ToolResult::err(e),
    }
}

fn set_macro(handle: &EngineHandle, args: &Value) -> ToolResult {
    let (Some(track), Some(device)) = (arg_u64(args, "track"), arg_u64(args, "device")) else {
        return ToolResult::err("set_macro needs \"track\" and \"device\"");
    };
    let Some(value) = arg_f64(args, "value") else {
        return ToolResult::err("set_macro needs \"value\" from 0 to 1");
    };
    let p = UiModSourceValuePayload {
        command_type: UiCommandType::SetModSourceValue as u16,
        flags: MOD_SOURCE_MACRO,
        track_id: track as u32,
        base_version: 0,
        source_device_id: device as u32,
        source_id: arg_u64(args, "source").unwrap_or(0) as u32,
        value: value.clamp(0.0, 1.0) as f32,
        reserved: [0u8; 16],
    };
    match handle.send_mod_source_value(p) {
        Ok(()) => ToolResult::ok(json!({ "sent": true, "device": device, "value": value })),
        Err(e) => ToolResult::err(e),
    }
}

/// Pull a lane toward a grid, NON-DESTRUCTIVELY.
///
/// Nothing on disk moves: the engine applies this to a separate scheduling copy, so the
/// authored tick is still what is stored, saved and drawn, and only where the note SOUNDS
/// changes. Worth saying to a model, which will otherwise assume a quantize rewrites notes.
fn set_lane_quantize(handle: &EngineHandle, args: &Value) -> ToolResult {
    let Some(track) = arg_u64(args, "track") else {
        return ToolResult::err("set_lane_quantize needs \"track\"");
    };
    // Nanoticks, not a subdivision index: a lane can quantize to something its display grid
    // does not show, and 960000 per quarter means a triplet is a third of the straight value
    // above it rather than the next entry in a list.
    let grid: u64 = match arg_str(args, "grid").unwrap_or("off") {
        "off" => 0,
        "1/4" => 960_000, "1/8" => 480_000, "1/16" => 240_000, "1/32" => 120_000,
        "1/4t" => 640_000, "1/8t" => 320_000, "1/16t" => 160_000,
        other => return ToolResult::err(format!("unknown grid {other:?}")),
    };
    let strength = arg_u64(args, "strength").unwrap_or(100).min(100);
    let swing = args.get("swing").and_then(|v| v.as_i64()).unwrap_or(0).clamp(-50, 50);
    let mut p = blank(UiCommandType::SetLaneQuantize);
    p.track_id = track as u32;
    p.note_nanotick_lo = (grid & 0xffff_ffff) as u32;
    p.note_nanotick_hi = (grid >> 32) as u32;
    // Thousandths on the wire, percent in the argument — nobody should type 850 to mean 85%.
    p.value0 = (strength * 10) as u32;
    // BIASED BY +500 so a negative swing survives an unsigned field. 500 is straight.
    p.note_pitch = ((swing * 10) + 500) as u32;
    send_now(handle, p, json!({ "track": track, "grid": grid, "strength": strength, "swing": swing }))
}

/// Which parameters are automated. A standing region — no request, no wait.
fn automation_lanes(handle: &EngineHandle, args: &Value) -> ToolResult {
    let want = arg_u64(args, "track").map(|t| t as u32);
    let view = handle.read_automation_lanes();
    let lanes: Vec<Value> = view.lanes.iter()
        .filter(|l| want.map_or(true, |t| t == l.track_id))
        .map(|l| json!({
            "track": l.track_id, "param": l.param_id, "points": l.point_count,
            "discrete": l.discrete, "target_plugin_index": l.target_plugin_index,
        }))
        .collect();
    ToolResult::ok(json!({
        "lanes": lanes,
        "version": view.version,
        // An incomplete list that says nothing reads as a complete one.
        "truncated": view.truncated,
    }))
}

/// One lane's points: ask, then read our own slot under its seqlock.
///
/// THE SEQ IS OURS AND EVERY ECHOED FIELD IS CHECKED. Slots are reused mod the slot count, so an
/// answer to somebody else's question is indistinguishable from ours on anything but the echo —
/// and an agent handed another track's curve would edit confidently against the wrong music.
fn automation_points(handle: &EngineHandle, args: &Value) -> ToolResult {
    let Some(track) = arg_u64(args, "track") else {
        return ToolResult::err("automation_points needs \"track\"");
    };
    let Some(param) = arg_str(args, "param").filter(|p| !p.is_empty()) else {
        return ToolResult::err("automation_points needs \"param\" — the id from `automation`");
    };
    let mut param_id = [0u8; 16];
    let b = param.as_bytes();
    let take = b.len().min(param_id.len());
    param_id[..take].copy_from_slice(&b[..take]);
    // Monotonic per process, so two questions in flight land in different slots.
    static NEXT: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(1);
    let seq = NEXT.fetch_add(1, std::sync::atomic::Ordering::AcqRel);
    let target = arg_u64(args, "target_plugin_index").unwrap_or(u32::MAX as u64) as u32;
    let payload = daw_bridge::layout::UiAutomationLaneRequestPayload {
        command_type: UiCommandType::RequestAutomationLane as u16,
        flags: 0, request_seq: seq, track_id: track as u32,
        target_plugin_index: target, param_id, reserved0: 0, reserved1: 0,
    };
    if let Err(e) = handle.send_automation_lane_request(payload) { return ToolResult::err(e); }
    let slot = (seq as usize) % daw_bridge::layout::K_UI_AUTOMATION_SLOTS;
    let deadline = std::time::Instant::now() + std::time::Duration::from_millis(1500);
    loop {
        if let Some(a) = handle.read_automation_slot(slot) {
            if a.request_seq == seq && a.track_id == track as u32 && a.param_id == param {
                return ToolResult::ok(json!({
                    "track": a.track_id, "param": a.param_id,
                    // `found: false` is an ANSWER, and is returned as `ok` so a model reads it
                    // as "nothing automates that" rather than as a call that failed.
                    "found": a.found, "discrete": a.discrete,
                    "truncated": a.points_truncated,
                    "points": a.points.iter().map(|(t, v)| json!([t, v])).collect::<Vec<_>>(),
                }));
            }
        }
        if std::time::Instant::now() >= deadline {
            // A timeout is NOT an empty lane. Returned as an error, because an empty list would
            // be read as "nothing automates that" — an answer the engine gives explicitly.
            return ToolResult::err(format!(
                "the engine did not answer about {param:?} on track {track} within 1.5s"));
        }
        std::thread::sleep(std::time::Duration::from_millis(10));
    }
}

/// Write one automation point.
///
/// Self-addressing: the paramId rides on the payload, so there is no "which parameter am I
/// writing" mode to forget. `SetAutomationTarget` exists and is deliberately unused for that
/// reason — a mode nobody set fails quietly, in the wrong lane.
fn write_automation_point(handle: &EngineHandle, args: &Value) -> ToolResult {
    let Some(track) = arg_u64(args, "track") else {
        return ToolResult::err("write_automation_point needs \"track\"");
    };
    let Some(param) = arg_str(args, "param").filter(|p| !p.is_empty()) else {
        return ToolResult::err("write_automation_point needs \"param\"");
    };
    let Some(value) = arg_f64(args, "value") else {
        return ToolResult::err("write_automation_point needs \"value\" from 0 to 1");
    };
    let Some(tick) = arg_u64(args, "tick") else {
        return ToolResult::err("write_automation_point needs \"tick\" in nanoticks");
    };
    let mut param_id = [0u8; 16];
    let b = param.as_bytes();
    let take = b.len().min(param_id.len());
    param_id[..take].copy_from_slice(&b[..take]);
    let p = daw_bridge::layout::UiAutomationPointPayload {
        command_type: UiCommandType::WriteAutomationPoint as u16,
        flags: if args.get("discrete").and_then(|v| v.as_bool()).unwrap_or(false) { 1 } else { 0 },
        track_id: track as u32,
        // Every plugin on the track that publishes this parameter, unless one is named.
        target_plugin_index: arg_u64(args, "target_plugin_index")
            .unwrap_or(u32::MAX as u64) as u32,
        nanotick_lo: (tick & 0xffff_ffff) as u32,
        nanotick_hi: (tick >> 32) as u32,
        // CLAMPED, not refused: a value past the end of a range is an ordinary thing for a
        // caller to compute, and refusing it would make the extremes of a sweep do nothing.
        value: value.clamp(0.0, 1.0) as f32,
        param_id,
    };
    match handle.send_automation_point(p) {
        Ok(()) => ToolResult::ok(json!({
            "sent": true, "track": track, "param": param, "tick": tick,
            "value": value.clamp(0.0, 1.0),
        })),
        Err(e) => ToolResult::err(e),
    }
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
        "shared_clips" => shared_clips(handle, &call.args),
        "fork_placement" => scratch(handle, &call.args, UiCommandType::ForkPlacementClip),
        "swap_placement_clip" => scratch(handle, &call.args, UiCommandType::SwapPlacementClip),
        "keep_placement_clip" => scratch(handle, &call.args, UiCommandType::ClearPlacementAlternate),
        "markers" => markers(handle),
        "edit_marker" => edit_marker(handle, &call.args),
        "insert_time" => insert_time(handle, &call.args),
        "set_time_signature" => set_time_signature(handle, &call.args),
        "device_params" => device_params(handle, &call.args),
        "add_device" => add_device(handle, &call.args),
        "remove_device" => chain_edit(handle, &call.args, UiCommandType::RemoveDevice),
        "move_device" => move_device(handle, &call.args),
        "set_bypass" => set_bypass(handle, &call.args),
        "modulate" => modulate(handle, &call.args),
        "unmodulate" => unmodulate(handle, &call.args),
        "set_macro" => set_macro(handle, &call.args),
        "set_lane_quantize" => set_lane_quantize(handle, &call.args),
        "automation" => automation_lanes(handle, &call.args),
        "automation_points" => automation_points(handle, &call.args),
        "write_automation_point" => write_automation_point(handle, &call.args),
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
/// A device-kind name from the manifest, as the engine's `DeviceKind` number.
///
/// Mirrored from `apps/device_chain.h` through the chain snapshot's `kind`, and named rather
/// than passed as a number for the reason the sidecar's mod kinds are: a literal that means
/// something else after the next enum change is the kind that outlives its meaning.
///
/// A FUNCTION, not an inline match, so a test can ask it what it accepts. The names it accepts
/// and the names `add_device`'s schema ADVERTISES have to be the same set — a name in the schema
/// that this rejects is a capability the model will try and be refused for, and a name this
/// accepts that the schema omits is one it will never think to try. `add_device` has already
/// been on the wrong side of that: its description promises a `plugin` argument that the schema
/// never declares and the executor never reads.
fn device_kind_code(name: &str) -> Option<u32> {
    match name {
        "patcher" | "patcher_event" => Some(0),
        "vst_instrument" => Some(3),
        "vst_effect" => Some(4),
        // The engine's OWN instrument, and the only one needing no plugin installed. Its absence
        // meant "put a sampler on that track" — a thing the DAW does, the UI offers, and the
        // engine's AddDevice has handled all along — came back as "unknown device kind". The
        // manifest is the agent's entire reach: a capability missing from it does not exist as
        // far as anyone asking is concerned.
        "sampler" => Some(5),
        _ => None,
    }
}

/// Add a track and TELL THE CALLER WHICH ONE IT IS.
///
/// It used to answer `{"added": true}` and nothing else, which made the very first thing anyone
/// asks for — "add a track called Bass" — fail in a way that reads like the DAW ignoring you.
/// With no id in the reply the model has to guess the new track's index, and the guess is wrong:
/// tracks are dense indices with the MASTER LAST, so adding one puts the new track at the index
/// the master used to occupy and shifts the master up. Measured: the model added a track, said
/// "I've added a new track named Bass (track 2)", renamed track 2 — the master — and every later
/// instruction that referred to "Bass" then failed with "I don't see a track named Bass".
///
/// Waiting for the count to grow also makes this tool HONEST. `send_now` returns Ok when the
/// command is enqueued, not when it is accepted, so the old version reported success for an add
/// that never happened.
fn add_track(handle: &EngineHandle) -> ToolResult {
    let before = handle.read_track_names();
    if let Err(e) = handle.send_command(blank(UiCommandType::AddTrack)) {
        return ToolResult::err(e);
    }
    // Bounded: the engine drains the ring on its command thread, so this is milliseconds in the
    // normal case. A budget rather than a fixed sleep, because the answer is "the count grew",
    // not "some time passed".
    for _ in 0..80 {
        let after = handle.read_track_names();
        if after.len() > before.len() {
            // The first index where the two lists disagree IS the new track: the insert shifts
            // the master (and anything after it) up by one. Falling back to the last real slot
            // keeps a sane answer if the names happen to be identical.
            let track = before
                .iter()
                .zip(after.iter())
                .position(|(a, b)| a != b)
                .unwrap_or(after.len().saturating_sub(1));
            return ToolResult::ok(json!({ "added": true, "track": track }));
        }
        std::thread::sleep(std::time::Duration::from_millis(25));
    }
    ToolResult::err(
        "the engine did not add a track (the command was sent but the published track count \
         never grew)",
    )
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

    /// WHAT add_device ADVERTISES AND WHAT IT ACCEPTS MUST BE THE SAME SET.
    ///
    /// Not a tautology — the two live in different places and have already drifted apart in this
    /// very tool. `sampler` was accepted by the engine and offered by the UI but appeared in
    /// neither the schema nor the executor, so asking for one answered "unknown device kind";
    /// meanwhile the description still promises a `plugin` argument that the schema does not
    /// declare and the executor does not read. This compares the two lists by BEHAVIOUR — it
    /// asks the mapping — so it cannot be satisfied by editing a comment.
    #[test]
    fn advertised_device_kinds_are_exactly_the_accepted_ones() {
        let spec = tool_manifest()
            .into_iter()
            .find(|s| s.name == "add_device")
            .expect("add_device is in the manifest");
        let advertised: Vec<String> = spec.params["properties"]["kind"]["enum"]
            .as_array()
            .expect("add_device declares a kind enum")
            .iter()
            .map(|v| v.as_str().expect("enum entries are strings").to_string())
            .collect();
        assert!(!advertised.is_empty(), "add_device advertises no kinds at all");

        for name in &advertised {
            assert!(device_kind_code(name).is_some(),
                    "add_device ADVERTISES kind {name:?} but the executor rejects it — the model \
                     will ask for it and be refused");
        }
        // And the other direction: anything the executor accepts should be offered, or nobody
        // will ever ask for it. `patcher_event` is the documented alias for `patcher`, so it is
        // allowed to be accepted without being advertised twice.
        for name in ["patcher", "vst_instrument", "vst_effect", "sampler"] {
            assert!(advertised.iter().any(|a| a == name),
                    "the executor accepts {name:?} but add_device does not advertise it, so the \
                     model has no way to know the capability exists");
        }
        assert_eq!(device_kind_code("sampler"), Some(5),
                   "the sampler is DeviceKind 5 in apps/device_chain.h");
        assert_eq!(device_kind_code("no_such_kind"), None);
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
