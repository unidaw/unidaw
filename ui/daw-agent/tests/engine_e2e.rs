//! End-to-end tests that spawn the real engine and drive it through the agent —
//! re-homing the coverage that left with daw-app, against the go-forward harness.
//!
//! These are the asserting versions of the daw-agent examples (roundtrip,
//! segtest): they start `build/daw_engine` in test mode, drive it over the
//! command ring, and check what it publishes and writes to disk. Serialized with
//! a global lock — parallel engines contend on host sockets/SHM and flake.
//!
//! Requires the C++ targets built first (`cmake --build build`). If the engine
//! binary is missing the tests fail fast with that message.

use std::path::PathBuf;
use std::process::{Child, Command, Stdio};
use std::sync::Mutex;
use std::time::{Duration, Instant};

use daw_agent::{observe::observe_track, AgentSession, ToolCall};
use daw_bridge::layout::{UiCommandPayload, UiCommandType};
use serde_json::{json, Value};

// One engine at a time.
static SERIAL: Mutex<()> = Mutex::new(());

struct Engine {
    child: Child,
    proj: PathBuf,
}

impl Drop for Engine {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

fn build_dir() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../../build")
        .canonicalize()
        .expect("build dir not found — build the C++ targets first")
}

/// Spawns the engine in test mode with a private SHM name and project dir, and
/// returns once a track is ready to take edits. The engine self-exits after
/// --run-seconds as a backstop; Drop kills it at test end.
fn start_engine(tag: &str) -> (Engine, AgentSession) {
    let build = build_dir();
    let engine_bin = build.join("daw_engine");
    assert!(
        engine_bin.exists(),
        "daw_engine not built at {} — run `cmake --build build`",
        engine_bin.display()
    );
    let pid = std::process::id();
    let shm = format!("/daw_e2e_{pid}_{tag}");
    let proj = std::env::temp_dir().join(format!("daw_e2e_{pid}_{tag}"));
    let _ = std::fs::remove_dir_all(&proj);
    std::fs::create_dir_all(&proj).expect("create project dir");

    let child = Command::new(&engine_bin)
        .current_dir(&build) // it spawns ./juce_host_process relative to cwd
        .env("DAW_UI_SHM_NAME", &shm)
        .env("DAW_PROJECT_DIR", &proj)
        .env("DAW_ENGINE_TEST_MODE", "1")
        .arg("--run-seconds")
        .arg("45")
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()
        .expect("spawn daw_engine");

    let deadline = Instant::now() + Duration::from_secs(20);
    let session = loop {
        if let Ok(s) = AgentSession::attach(&shm) {
            break s;
        }
        assert!(Instant::now() < deadline, "engine SHM never appeared");
        std::thread::sleep(Duration::from_millis(150));
    };
    while session.handle().track_count() < 1 {
        assert!(Instant::now() < deadline, "engine track never became ready");
        std::thread::sleep(Duration::from_millis(150));
    }
    (Engine { child, proj }, session)
}

fn read_project(proj: &PathBuf, name: &str) -> Value {
    let path = proj.join(format!("{name}.uniproj.json"));
    let deadline = Instant::now() + Duration::from_secs(5);
    while !path.exists() {
        assert!(Instant::now() < deadline, "project {name} was not written");
        std::thread::sleep(Duration::from_millis(50));
    }
    let text = std::fs::read_to_string(&path).expect("read saved project");
    serde_json::from_str(&text).expect("parse saved project")
}

fn track<'a>(doc: &'a Value, id: u64) -> &'a Value {
    doc["tracks"]
        .as_array()
        .unwrap()
        .iter()
        .find(|t| t["track_id"].as_u64() == Some(id))
        .expect("track present")
}

const Q: u64 = 960_000;

/// Notes entered far apart are segmented into separate clips on save, so every
/// note lands under a clip ("no notes outside clips") with sane boundaries.
#[test]
fn segments_live_edits_into_clips() {
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let (engine, session) = start_engine("seg");
    let four_bars = 16 * Q;

    let a = session.execute(&ToolCall {
        tool: "add_notes".into(),
        args: json!({"track":0,"pitches":[60,62,64],"start":0,"step":Q,"duration":Q}),
    });
    assert!(a.ok, "cluster A failed: {a:?}");
    // add_notes waits for its batch to land, so B reads a settled version.
    let b = session.execute(&ToolCall {
        tool: "add_notes".into(),
        args: json!({"track":0,"pitches":[67,69],"start":four_bars,"step":Q,"duration":Q}),
    });
    assert!(b.ok, "cluster B failed: {b:?}");
    assert_eq!(
        b.output["first_base_version"].as_u64(),
        Some(3),
        "B should base on A's applied version (no stale-version race): {b:?}"
    );

    let save = session.execute(&ToolCall {
        tool: "save".into(),
        args: json!({"name":"segout"}),
    });
    assert!(save.ok, "save failed: {save:?}");

    let doc = read_project(&engine.proj, "segout");
    assert_eq!(doc["schema_version"].as_u64(), Some(4));
    let clips = doc["clips"].as_array().unwrap();
    assert_eq!(clips.len(), 2, "expected two segmented clips: {clips:?}");
    let pls = track(&doc, 0)["placements"].as_array().unwrap().clone();
    assert_eq!(pls.len(), 2, "expected two placements: {pls:?}");
    let mut ats: Vec<u64> = pls.iter().map(|p| p["at"].as_u64().unwrap()).collect();
    ats.sort_unstable();
    assert_eq!(ats, vec![0, four_bars], "placement anchors: {ats:?}");
    // Every entered pitch survives, distributed across the two clips.
    let mut pitches: Vec<u64> = clips
        .iter()
        .flat_map(|c| c["notes"].as_array().unwrap())
        .map(|n| n["pitch"].as_u64().unwrap())
        .collect();
    pitches.sort_unstable();
    assert_eq!(pitches, vec![60, 62, 64, 67, 69]);
}

/// A project with two placements (one carrying an additive add + a mute) survives
/// a load -> save round-trip with its structure intact — the engine re-emits the
/// authored placements rather than flattening them.
#[test]
fn roundtrip_preserves_placements() {
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let (engine, session) = start_engine("rt");
    let two_bars = 8 * Q;
    let one_bar = 4 * Q;

    let rt_in = json!({
        "schema_version": 2,
        "meta": { "name": "rt_in", "created_utc": 0, "modified_utc": 0 },
        "nanoticks_per_quarter": Q,
        "tempo_map": [ { "nanotick": 0, "bpm": 120 } ],
        "harmony_timeline": [],
        "clips": [ {
            "id": 7, "name": "Riff", "length": one_bar,
            "notes": [
                { "nanotick": 0, "duration": Q/2, "pitch": 60, "velocity": 100, "column": 0, "note_id": 101 },
                { "nanotick": 2*Q, "duration": Q/2, "pitch": 64, "velocity": 100, "column": 0, "note_id": 102 }
            ],
            "chords": []
        } ],
        "tracks": [ {
            "track_id": 0, "name": "Track 1", "harmony_quantize": 0, "lines_per_beat": 4,
            "mixer": { "gain_db": 0.0, "pan": 0.0, "mute": false, "solo": false },
            "device_chain": [], "mod_links": [],
            "placements": [
                { "clip_id": 7, "at": 0, "length": one_bar, "notes": [], "chords": [], "mutes": [] },
                { "clip_id": 7, "at": two_bars, "length": one_bar,
                  "notes": [ { "nanotick": Q, "duration": Q/2, "pitch": 67, "velocity": 90, "column": 1, "note_id": 201 } ],
                  "chords": [], "mutes": [102] }
            ]
        } ]
    });
    std::fs::write(
        engine.proj.join("rt_in.uniproj.json"),
        serde_json::to_string_pretty(&rt_in).unwrap(),
    )
    .unwrap();

    let before = session.handle().clip_version();
    let load = session.execute(&ToolCall {
        tool: "load".into(),
        args: json!({"name":"rt_in"}),
    });
    assert!(load.ok, "load failed: {load:?}");
    // Load bumps the clip version once; wait for that rather than sleeping.
    assert!(
        session.handle().wait_for_clip_version(
            before,
            before.wrapping_add(1),
            Duration::from_secs(3)
        ),
        "load was not applied"
    );

    let save = session.execute(&ToolCall {
        tool: "save".into(),
        args: json!({"name":"rt_out"}),
    });
    assert!(save.ok, "save failed: {save:?}");

    let doc = read_project(&engine.proj, "rt_out");
    let pls = track(&doc, 0)["placements"].as_array().unwrap().clone();
    assert_eq!(pls.len(), 2, "placements not preserved: {pls:?}");
    let p2 = pls
        .iter()
        .find(|p| p["at"].as_u64() == Some(two_bars))
        .expect("second placement preserved");
    // The additive override survives: the mute and the added note.
    let mutes: Vec<u64> = p2["mutes"].as_array().unwrap().iter().map(|m| m.as_u64().unwrap()).collect();
    assert_eq!(mutes, vec![102], "override mute lost: {p2:?}");
    let adds = p2["notes"].as_array().unwrap();
    assert!(
        adds.iter().any(|n| n["note_id"].as_u64() == Some(201)),
        "override add note lost: {p2:?}"
    );
    // The referenced clip is re-emitted (deduped) with both base notes.
    let clip7 = doc["clips"]
        .as_array()
        .unwrap()
        .iter()
        .find(|c| c["id"].as_u64() == Some(7))
        .expect("clip 7 re-emitted");
    assert_eq!(clip7["notes"].as_array().unwrap().len(), 2);
}

/// The M3.2 fix: editing a note *inside* a loaded placement mutates that
/// placement's clip in place (structural note entry), so a load -> edit -> save
/// keeps the arrangement's two-placement structure instead of flattening it. The
/// added note lands in the second placement's clip at the clip-relative tick; the
/// first placement's clip is untouched.
#[test]
fn edit_inside_placement_lands_in_its_clip() {
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let (engine, session) = start_engine("editin");
    let one_bar = 4 * Q;
    let two_bars = 8 * Q;

    // Two distinct clips, one placement each: clip 10 at bar 0, clip 20 at bar 2.
    let proj = json!({
        "schema_version": 2,
        "meta": { "name": "editin_in", "created_utc": 0, "modified_utc": 0 },
        "nanoticks_per_quarter": Q,
        "tempo_map": [ { "nanotick": 0, "bpm": 120 } ],
        "harmony_timeline": [],
        "clips": [
            { "id": 10, "name": "A", "length": one_bar,
              "notes": [ { "nanotick": 0, "duration": Q/2, "pitch": 60, "velocity": 100, "column": 0, "note_id": 1001 } ],
              "chords": [] },
            { "id": 20, "name": "B", "length": one_bar,
              "notes": [ { "nanotick": 0, "duration": Q/2, "pitch": 64, "velocity": 100, "column": 0, "note_id": 2001 } ],
              "chords": [] }
        ],
        "tracks": [ {
            "track_id": 0, "name": "Track 1", "harmony_quantize": 0, "lines_per_beat": 4,
            "mixer": { "gain_db": 0.0, "pan": 0.0, "mute": false, "solo": false },
            "device_chain": [], "mod_links": [],
            "placements": [
                { "clip_id": 10, "at": 0, "length": one_bar, "notes": [], "chords": [], "mutes": [] },
                { "clip_id": 20, "at": two_bars, "length": one_bar, "notes": [], "chords": [], "mutes": [] }
            ]
        } ]
    });
    std::fs::write(
        engine.proj.join("editin_in.uniproj.json"),
        serde_json::to_string_pretty(&proj).unwrap(),
    )
    .unwrap();

    let before = session.handle().clip_version();
    let load = session.execute(&ToolCall { tool: "load".into(), args: json!({"name":"editin_in"}) });
    assert!(load.ok, "load failed: {load:?}");
    assert!(
        session.handle().wait_for_clip_version(before, before.wrapping_add(1), Duration::from_secs(3)),
        "load was not applied"
    );

    // Add a note one quarter into the *second* placement (absolute two_bars + Q).
    // It must land inside clip 20 at clip-relative tick Q — not clip 10, not a new
    // clip out on its own.
    let edit = session.execute(&ToolCall {
        tool: "add_notes".into(),
        args: json!({"track":0,"pitches":[72],"start": two_bars + Q,"step":Q,"duration":Q/2}),
    });
    assert!(edit.ok, "edit failed: {edit:?}");

    let save = session.execute(&ToolCall { tool: "save".into(), args: json!({"name":"editin_out"}) });
    assert!(save.ok, "save failed: {save:?}");

    let doc = read_project(&engine.proj, "editin_out");
    let pls = track(&doc, 0)["placements"].as_array().unwrap().clone();
    assert_eq!(pls.len(), 2, "two-placement structure not preserved: {pls:?}");
    let mut ats: Vec<u64> = pls.iter().map(|p| p["at"].as_u64().unwrap()).collect();
    ats.sort_unstable();
    assert_eq!(ats, vec![0, two_bars], "placement anchors moved: {ats:?}");

    let clip_of = |at: u64| -> Value {
        let cid = pls.iter().find(|p| p["at"].as_u64() == Some(at)).unwrap()["clip_id"].as_u64().unwrap();
        doc["clips"].as_array().unwrap().iter()
            .find(|c| c["id"].as_u64() == Some(cid)).expect("placement's clip re-emitted").clone()
    };

    // First placement's clip is untouched: still exactly its one base note.
    let first = clip_of(0);
    let first_notes = first["notes"].as_array().unwrap();
    assert_eq!(first_notes.len(), 1, "first clip changed: {first:?}");
    assert_eq!(first_notes[0]["pitch"].as_u64(), Some(60));

    // Second placement's clip now has the base note plus the edit, at rel 0 and Q.
    let second = clip_of(two_bars);
    let notes = second["notes"].as_array().unwrap();
    assert_eq!(notes.len(), 2, "edit did not land in the second clip: {second:?}");
    let mut by_tick: Vec<(u64, u64)> = notes.iter()
        .map(|n| (n["nanotick"].as_u64().unwrap(), n["pitch"].as_u64().unwrap()))
        .collect();
    by_tick.sort_unstable();
    assert_eq!(by_tick, vec![(0, 64), (Q, 72)], "second clip contents wrong: {notes:?}");
}

/// Regression: writing a note PAST the end of all material must grow the song, not
/// silently vanish. patternTicks defaults to one bar, so a write at row 20 (== 5*Q)
/// is past the end — the frontend's reproduction. The note must appear in the
/// derived clip with room to sound, and persist.
#[test]
fn write_past_pattern_end_creates_material() {
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let (engine, session) = start_engine("pastend");

    let far = 5 * Q; // row 20 at 4 rows/beat — past the 1-bar default pattern
    let a = session.execute(&ToolCall {
        tool: "add_notes".into(),
        args: json!({"track":0,"pitches":[67],"start":far,"step":Q,"duration":0}),
    });
    assert!(a.ok, "write past end failed: {a:?}");

    // Visible in the derived (flat) clip the UI reads, with a positive length.
    //
    // `observe` with no window returns the song's SHAPE and no notes — the whole
    // song's notes ran to megabytes and could not be handed to a model at all.
    // A test that wants notes asks for the window they are in; this one covers
    // well past `far` (beat 5).
    let obs = session.execute(&ToolCall {
        tool: "observe".into(),
        args: json!({ "from_beat": 0, "beats": 64 }),
    });
    assert!(obs.ok, "observe failed: {obs:?}");
    let note = obs.output["tracks"][0]["notes"]
        .as_array()
        .unwrap()
        .iter()
        .find(|n| n["pitch"].as_u64() == Some(67))
        .unwrap_or_else(|| panic!("note past the end vanished from the clip: {}", obs.output));
    assert_eq!(note["nanotick"].as_u64(), Some(far), "note landed at the wrong tick: {note}");
    assert!(note["duration"].as_u64().unwrap() > 0, "note past the end has no length: {note}");

    // And it persists on disk.
    assert!(session.execute(&ToolCall { tool: "save".into(), args: json!({"name":"pastend_out"}) }).ok);
    let doc = read_project(&engine.proj, "pastend_out");
    let pitches: Vec<u64> = doc["clips"]
        .as_array()
        .unwrap()
        .iter()
        .flat_map(|c| c["notes"].as_array().unwrap())
        .map(|n| n["pitch"].as_u64().unwrap())
        .collect();
    assert!(pitches.contains(&67), "note past the end was not saved: {}", doc["clips"]);
}

/// Undo/redo of a structural edit is a whole-store swap: after two note adds, one
/// undo restores the store to a single note, and a redo brings the second back.
#[test]
fn undo_redo_structural_edit() {
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let (engine, session) = start_engine("undo");

    let a = session.execute(&ToolCall {
        tool: "add_notes".into(),
        args: json!({"track":0,"pitches":[60],"start":0,"step":Q,"duration":Q}),
    });
    assert!(a.ok, "add A failed: {a:?}");
    let b = session.execute(&ToolCall {
        tool: "add_notes".into(),
        args: json!({"track":0,"pitches":[62],"start":Q,"step":Q,"duration":Q}),
    });
    assert!(b.ok, "add B failed: {b:?}");

    let pitches_after = |name: &str| -> Vec<u64> {
        let doc = read_project(&engine.proj, name);
        let mut ps: Vec<u64> = doc["clips"].as_array().unwrap().iter()
            .flat_map(|c| c["notes"].as_array().unwrap())
            .map(|n| n["pitch"].as_u64().unwrap())
            .collect();
        ps.sort_unstable();
        ps
    };

    assert!(session.execute(&ToolCall { tool: "save".into(), args: json!({"name":"undo_two"}) }).ok);
    assert_eq!(pitches_after("undo_two"), vec![60, 62], "both notes present before undo");

    let u = session.execute(&ToolCall { tool: "undo".into(), args: json!({}) });
    assert!(u.ok && u.output["applied"].as_bool() == Some(true), "undo not applied: {u:?}");
    assert!(session.execute(&ToolCall { tool: "save".into(), args: json!({"name":"undo_one"}) }).ok);
    assert_eq!(pitches_after("undo_one"), vec![60], "undo should leave just the first note");

    let r = session.execute(&ToolCall { tool: "redo".into(), args: json!({}) });
    assert!(r.ok && r.output["applied"].as_bool() == Some(true), "redo not applied: {r:?}");
    assert!(session.execute(&ToolCall { tool: "save".into(), args: json!({"name":"undo_redo"}) }).ok);
    assert_eq!(pitches_after("undo_redo"), vec![60, 62], "redo should restore the second note");
}

/// The agent loop (perceive -> decide -> act) drives the real engine: a scripted
/// decider adds notes then saves, and the loop stops when the script runs out.
#[test]
fn agent_loop_drives_engine() {
    use daw_agent::{run_agent_loop, ScriptedDecider};
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let (engine, session) = start_engine("loop");

    let mut decider = ScriptedDecider::new(vec![
        vec![ToolCall {
            tool: "add_notes".into(),
            args: json!({"track":0,"pitches":[60,62,64],"start":0,"step":Q,"duration":Q}),
        }],
        vec![ToolCall {
            tool: "save".into(),
            args: json!({"name":"loopout"}),
        }],
    ]);
    let transcript = run_agent_loop(&session, &mut decider, 8);

    assert_eq!(transcript.len(), 2, "loop should run two scripted steps then stop");
    assert!(transcript.iter().all(|s| s.ok()), "every step should succeed: {transcript:?}");

    let doc = read_project(&engine.proj, "loopout");
    let mut pitches: Vec<u64> = doc["clips"]
        .as_array()
        .unwrap()
        .iter()
        .flat_map(|c| c["notes"].as_array().unwrap())
        .map(|n| n["pitch"].as_u64().unwrap())
        .collect();
    pitches.sort_unstable();
    assert_eq!(pitches, vec![60, 62, 64], "loop should have added the notes");
}

/// The patcher graph the engine runs is published so the UI can draw it: the
/// default graph has a euclidean node (with config) wired to a passthrough.
#[test]
fn patcher_graph_published() {
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let (_engine, session) = start_engine("patcher");
    // The region fills on the first publish; poll briefly.
    let deadline = Instant::now() + Duration::from_secs(3);
    let view = loop {
        let v = session.handle().read_patcher();
        if !v.nodes.is_empty() {
            break v;
        }
        assert!(Instant::now() < deadline, "patcher graph never published");
        std::thread::sleep(Duration::from_millis(20));
    };
    // Euclidean = node type 1, carrying config (steps/hits/...).
    let euclid = view
        .nodes
        .iter()
        .find(|n| n.node_type == 1)
        .expect("default graph should have a euclidean node");
    assert_eq!(euclid.has_config, 1, "euclidean node should carry config");
    assert_eq!(euclid.config[0], 16, "euclidean steps"); // steps
    assert_eq!(euclid.config[1], 5, "euclidean hits"); // hits
    assert!(!view.edges.is_empty(), "default graph should have an edge");
}

/// Per-device patcher execution: a project whose track carries TWO patcher devices,
/// each with its own graph, is assembled into one shared pool on load (globally
/// unique node ids), so each device's subgraph runs independently. Verified through
/// the published graph: both devices' euclidean nodes (distinct steps) appear.
#[test]
fn per_device_patchers_assemble_into_pool() {
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let (engine, session) = start_engine("perdev");

    // One patcher device's own graph: euclid(steps/hits) -> event_out, output = 1.
    let dev = |dev_id: u64, kind: &str, steps: u64, hits: u64| {
        json!({
            "device_id": dev_id, "kind": kind, "patcher_node_id": 1, "bypass": false,
            "patcher": {
                "nodes": [
                    { "id": 0, "type": "euclidean",
                      "euclidean": { "steps": steps, "hits": hits, "offset": 0, "duration_ticks": 0,
                                     "degree": 1, "octave_offset": 0, "velocity": 100, "base_octave": 4 } },
                    { "id": 1, "type": "event_out" }
                ],
                "edges": [
                    { "src_node_id": 0, "src_port_id": 1, "dst_node_id": 1, "dst_port_id": 0, "kind": "event" }
                ]
            }
        })
    };
    let proj = json!({
        "schema_version": 2,
        "meta": { "name": "perdev_in", "created_utc": 0, "modified_utc": 0 },
        "nanoticks_per_quarter": Q,
        "tempo_map": [ { "nanotick": 0, "bpm": 120 } ],
        "harmony_timeline": [], "clips": [],
        "tracks": [ {
            "track_id": 0, "name": "T", "harmony_quantize": 0, "lines_per_beat": 4,
            "mixer": { "gain_db": 0.0, "pan": 0.0, "mute": false, "solo": false },
            "device_chain": [ dev(10, "patcher_event", 7, 3), dev(20, "patcher_instrument", 9, 5) ],
            "mod_links": [], "placements": []
        } ]
    });
    std::fs::write(
        engine.proj.join("perdev_in.uniproj.json"),
        serde_json::to_string_pretty(&proj).unwrap(),
    )
    .unwrap();

    let load = session.execute(&ToolCall { tool: "load".into(), args: json!({"name":"perdev_in"}) });
    assert!(load.ok, "load failed: {load:?}");

    // Poll the published graph until both devices' euclidean nodes (steps 7 and 9)
    // appear — proof both were assembled into the pool, not just the first.
    let deadline = Instant::now() + Duration::from_secs(3);
    let view = loop {
        let v = session.handle().read_patcher();
        let steps: Vec<i32> =
            v.nodes.iter().filter(|n| n.node_type == 1).map(|n| n.config[0]).collect();
        if steps.contains(&7) && steps.contains(&9) {
            break v;
        }
        assert!(
            Instant::now() < deadline,
            "assembled per-device pool never published: {:?}",
            v.nodes.iter().map(|n| (n.node_type, n.config[0])).collect::<Vec<_>>()
        );
        std::thread::sleep(Duration::from_millis(20));
    };
    // Union pool: two euclidean + two event_out = 4 nodes, two edges, ids unique.
    assert_eq!(view.nodes.len(), 4, "expected 4 pooled nodes");
    assert_eq!(view.edges.len(), 2, "expected two edges, one per device");
    let mut ids: Vec<u32> = view.nodes.iter().map(|n| n.id).collect();
    ids.sort_unstable();
    ids.dedup();
    assert_eq!(ids.len(), 4, "node ids must be globally unique in the assembled pool");
    // Each device keeps its own event_out terminal (type 6).
    assert_eq!(
        view.nodes.iter().filter(|n| n.node_type == 6).count(),
        2,
        "each device's event_out should survive assembly"
    );

    // Round-trip: save must preserve each device's OWN graph (2 nodes each), not
    // park the assembled 4-node pool onto one device.
    let save = session.execute(&ToolCall { tool: "save".into(), args: json!({"name":"perdev_out"}) });
    assert!(save.ok, "save failed: {save:?}");
    let doc = read_project(&engine.proj, "perdev_out");
    let devices = track(&doc, 0)["device_chain"].as_array().unwrap();
    let with_patcher: Vec<&Value> = devices
        .iter()
        .filter(|d| d.get("patcher").is_some())
        .collect();
    assert_eq!(with_patcher.len(), 2, "both devices should keep their own patcher: {devices:?}");
    for d in with_patcher {
        let nodes = d["patcher"]["nodes"].as_array().unwrap();
        assert_eq!(nodes.len(), 2, "each device's own graph is 2 nodes, not the pooled union: {d:?}");
    }
}

/// B1: a device's real name + parameters are published on RequestDeviceParams
/// (v17), so the device-chain rack shows the plugin instead of "VST #7". Loads the
/// Identity plugin (one "gain" param) and reads it back.
#[test]
fn device_params_published_on_request() {
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let (engine, session) = start_engine("devparams");

    // Current Identity build (the per-config path; the flat one is a stale copy).
    let build = build_dir();
    let mut vst = build.join("identity_plugin_artefacts/RelWithDebInfo/VST3/Identity.vst3");
    if !vst.exists() {
        vst = build.join("identity_plugin_artefacts/VST3/Identity.vst3");
    }
    assert!(vst.exists(), "Identity.vst3 not built at {}", vst.display());

    let proj = json!({
        "schema_version": 4,
        "meta": { "name": "devparams", "created_utc": 0, "modified_utc": 0 },
        "nanoticks_per_quarter": Q,
        "tempo_map": [ { "nanotick": 0, "bpm": 120 } ],
        "harmony_timeline": [], "clips": [],
        "tracks": [ {
            "track_id": 0, "name": "T", "harmony_quantize": 0, "lines_per_beat": 4,
            "mixer": { "gain_db": 0.0, "pan": 0.0, "mute": false, "solo": false },
            "device_chain": [ {
                "device_id": 0, "kind": "vst_effect", "patcher_node_id": 0, "bypass": false,
                "vst_ref": { "vendor": "", "name": "Identity", "path": vst.to_str().unwrap(), "uid16": "" }
            } ],
            "mod_links": [], "placements": []
        } ]
    });
    std::fs::write(
        engine.proj.join("devparams.uniproj.json"),
        serde_json::to_string_pretty(&proj).unwrap(),
    )
    .unwrap();
    let load = session.execute(&ToolCall { tool: "load".into(), args: json!({"name":"devparams"}) });
    assert!(load.ok, "load failed: {load:?}");

    // The plugin loads asynchronously in the host; retry the request until its
    // params appear (or time out).
    let req = |device_id: u32| {
        let p = UiCommandPayload {
            command_type: UiCommandType::RequestDeviceParams as u16,
            flags: 0,
            track_id: 0,
            plugin_index: 0,
            note_pitch: 0,
            value0: device_id,
            note_nanotick_lo: 0,
            note_nanotick_hi: 0,
            note_duration_lo: 0,
            note_duration_hi: 0,
            base_version: 0,
        };
        let _ = session.handle().send_command(p);
    };
    let deadline = Instant::now() + Duration::from_secs(20);
    let view = loop {
        req(0);
        let v = session.handle().read_device_params();
        if !v.params.is_empty() {
            break v;
        }
        assert!(
            Instant::now() < deadline,
            "device params never published (name={:?}, count={})",
            v.device_name, v.params.len()
        );
        std::thread::sleep(Duration::from_millis(250));
    };

    // The read-back is the requested device, with a real name and named params.
    assert_eq!(view.track_id, 0);
    assert_eq!(view.device_id, 0);
    assert!(!view.device_name.is_empty(), "device name should be published");
    assert!(!view.params.is_empty(), "at least one parameter should be published");
    // Every published param carries a display name, a durable id, and an in-range
    // normalised value — the three things a rack + a MAP binding need.
    let named = view.params.iter().filter(|p| !p.name.is_empty()).count();
    assert!(named > 0, "params should have display names: {:?}",
            view.params.iter().take(4).map(|p| &p.name).collect::<Vec<_>>());
    assert!(
        view.params.iter().any(|p| p.uid16 != [0u8; 16]),
        "params should carry a durable uid16 to key mappings on"
    );
    assert!(
        view.params.iter().all(|p| p.value >= 0.0 && p.value <= 1.0),
        "normalised values must be in 0..1"
    );
}

/// A multi-plugin VST3 bundle (u-he Zebra2.vst3 holds Zebra2, Zebralette, ZRev,
/// Zebrify) must load the sub-plugin the project NAMES, not just the first type.
/// Before the SetChain name change, this loaded Zebra2 for any of them. Gated on the
/// bundle being installed — it is a vendor plugin, absent on CI, so we skip rather
/// than fail there.
#[test]
fn multi_bundle_selects_named_subplugin() {
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let bundle = std::path::Path::new("/Library/Audio/Plug-Ins/VST3/Zebra2.vst3");
    if !bundle.exists() {
        eprintln!("skipping: {} not installed", bundle.display());
        return;
    }
    let (engine, session) = start_engine("zlette");

    // Same bundle path as a Zebra2 project, but the project asks for Zebralette —
    // a different plugin living inside the same .vst3.
    let proj = json!({
        "schema_version": 4,
        "meta": { "name": "zlette", "created_utc": 0, "modified_utc": 0 },
        "nanoticks_per_quarter": Q,
        "tempo_map": [ { "nanotick": 0, "bpm": 120 } ],
        "harmony_timeline": [], "clips": [],
        "tracks": [ {
            "track_id": 0, "name": "T", "harmony_quantize": 0, "lines_per_beat": 4,
            "mixer": { "gain_db": 0.0, "pan": 0.0, "mute": false, "solo": false },
            "device_chain": [ {
                "device_id": 0, "kind": "vst_instrument", "patcher_node_id": 0,
                "host_slot_index": 0, "bypass": false,
                "vst_ref": { "vendor": "u-he", "name": "Zebralette",
                             "path": bundle.to_str().unwrap(), "uid16": "" }
            } ],
            "mod_links": [], "placements": []
        } ]
    });
    std::fs::write(
        engine.proj.join("zlette.uniproj.json"),
        serde_json::to_string_pretty(&proj).unwrap(),
    )
    .unwrap();
    let load = session.execute(&ToolCall { tool: "load".into(), args: json!({"name":"zlette"}) });
    assert!(load.ok, "load failed: {load:?}");

    let req = || {
        let p = UiCommandPayload {
            command_type: UiCommandType::RequestDeviceParams as u16,
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
        let _ = session.handle().send_command(p);
    };
    let deadline = Instant::now() + Duration::from_secs(20);
    let view = loop {
        req();
        let v = session.handle().read_device_params();
        if !v.device_name.is_empty() {
            break v;
        }
        assert!(Instant::now() < deadline, "device params never published");
        std::thread::sleep(Duration::from_millis(250));
    };

    // The host reports the ACTUALLY-loaded instance's name; it must be the one the
    // project asked for, not the bundle's first type (Zebra2).
    assert_eq!(
        view.device_name, "Zebralette",
        "named sub-plugin selection failed: loaded {:?} instead of Zebralette",
        view.device_name
    );
}

/// The scale registry is published (v16) so the harmony + tuning UI can draw the
/// real cents ladder. Read-only, written once at startup.
#[test]
fn scale_registry_published() {
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let (_engine, session) = start_engine("scales");
    let scales = session.handle().read_scales();
    assert!(!scales.is_empty(), "scale registry should be published");
    let major = scales.iter().find(|s| s.id == 1).expect("Major scale (id 1) present");
    assert_eq!(major.name, "Major");
    assert_eq!(major.step_cents.len(), 7, "Major has 7 degrees: {major:?}");
    assert_eq!(major.step_cents[0], 0.0);
    assert_eq!(major.step_cents[1], 200.0, "milli-cents -> cents conversion");
    assert_eq!(major.step_cents[6], 1100.0);
    assert_eq!(major.octave_cents, 1200.0);
}

/// Track names are published: a fresh track defaults to "Track N", and a loaded
/// project's name flows through so every lane-labelling surface reads one source.
#[test]
fn track_names_published() {
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let (engine, session) = start_engine("names");
    // Fresh engine: default name.
    let names = session.handle().read_track_names();
    assert_eq!(names.first().map(String::as_str), Some("Track 1"), "default name: {names:?}");

    // Load a project whose track carries a distinctive name.
    let proj = json!({
        "schema_version": 3,
        "meta": { "name": "named", "created_utc": 0, "modified_utc": 0 },
        "nanoticks_per_quarter": Q,
        "tracks": [ { "track_id": 0, "name": "Bassline" } ]
    });
    std::fs::write(engine.proj.join("named.uniproj.json"), serde_json::to_string(&proj).unwrap()).unwrap();
    let before = session.handle().clip_version();
    let load = session.execute(&ToolCall { tool: "load".into(), args: json!({"name":"named"}) });
    assert!(load.ok, "load: {load:?}");
    session.handle().wait_for_clip_version(before, before.wrapping_add(1), Duration::from_secs(3));

    let deadline = Instant::now() + Duration::from_secs(3);
    loop {
        if session.handle().read_track_names().first().map(String::as_str) == Some("Bassline") {
            break;
        }
        assert!(Instant::now() < deadline, "loaded name never published: {:?}", session.handle().read_track_names());
        std::thread::sleep(Duration::from_millis(20));
    }
}

/// Renaming a track via set_track_name updates the published name.
#[test]
fn set_track_name_updates_published_name() {
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let (_engine, session) = start_engine("rename");
    let r = session.execute(&ToolCall {
        tool: "set_track_name".into(),
        args: json!({ "track": 0, "name": "Kick" }),
    });
    assert!(r.ok, "set_track_name failed: {r:?}");
    let deadline = Instant::now() + Duration::from_secs(3);
    loop {
        if session.handle().read_track_names().first().map(String::as_str) == Some("Kick") {
            break;
        }
        assert!(Instant::now() < deadline, "rename never published: {:?}", session.handle().read_track_names());
        std::thread::sleep(Duration::from_millis(20));
    }
}

/// A loaded project's harmony timeline is published (was 0 events before — the
/// load bumped the version but never armed the snapshot write).
#[test]
fn harmony_timeline_published() {
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let (engine, session) = start_engine("harmony");
    let proj = json!({
        "schema_version": 4,
        "meta": { "name": "harm" },
        "nanoticks_per_quarter": Q,
        "harmony_timeline": [
            { "nanotick": 0, "root": 9, "scale_id": 1, "flags": 0 },       // A minor
            { "nanotick": 4*Q, "root": 5, "scale_id": 0, "flags": 0 }      // F major
        ],
        "tracks": [ { "track_id": 0, "name": "T" } ]
    });
    std::fs::write(engine.proj.join("harm.uniproj.json"), serde_json::to_string(&proj).unwrap()).unwrap();
    let before = session.handle().clip_version();
    let load = session.execute(&ToolCall { tool: "load".into(), args: json!({"name":"harm"}) });
    assert!(load.ok, "load: {load:?}");
    session.handle().wait_for_clip_version(before, before.wrapping_add(1), Duration::from_secs(3));
    let deadline = Instant::now() + Duration::from_secs(3);
    loop {
        let h = session.handle().read_harmony();
        if h.len() == 2 && h[0].root == 9 && h[1].root == 5 {
            break;
        }
        assert!(Instant::now() < deadline, "harmony never published: {:?}", session.handle().read_harmony());
        std::thread::sleep(Duration::from_millis(20));
    }
}

/// SetLoopRange is published back so the UI can draw the loop.
#[test]
fn loop_range_published() {
    use daw_bridge::layout::{UiCommandPayload, UiCommandType};
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let (_engine, session) = start_engine("looprange");
    let start = 2 * Q;
    let end = 6 * Q;
    // SetLoopRange: start in note_nanotick_lo/hi, end in note_duration_lo/hi.
    let payload = UiCommandPayload {
        command_type: UiCommandType::SetLoopRange as u16,
        flags: 0, track_id: 0, plugin_index: 0, note_pitch: 0, value0: 0,
        note_nanotick_lo: (start & 0xffff_ffff) as u32,
        note_nanotick_hi: (start >> 32) as u32,
        note_duration_lo: (end & 0xffff_ffff) as u32,
        note_duration_hi: (end >> 32) as u32,
        base_version: 0,
    };
    session.handle().send_command(payload).expect("send SetLoopRange");
    let deadline = Instant::now() + Duration::from_secs(3);
    loop {
        if session.handle().loop_range() == (start, end) {
            break;
        }
        assert!(Instant::now() < deadline, "loop range never published: {:?}", session.handle().loop_range());
        std::thread::sleep(Duration::from_millis(20));
    }
}

/// The engine publishes the current tempo (milli-BPM, at the playhead) and the map's
/// point count, and honors SetTempo: flatten (flags=1) and insert-or-replace a point
/// (flags=0). This is what makes the chrome's BPM field real and editable.
#[test]
fn tempo_read_back_and_set() {
    use daw_bridge::layout::{UiCommandPayload, UiCommandType};
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let (_engine, session) = start_engine("tempo");

    let tempo = || session.handle().snapshot().map(|s| (s.ui_tempo_milli_bpm, s.ui_tempo_point_count));
    let wait_tempo = |want: (u32, u32), what: &str| {
        let deadline = Instant::now() + Duration::from_secs(3);
        loop {
            if tempo() == Some(want) {
                return;
            }
            assert!(Instant::now() < deadline, "{what}: tempo is {:?}, wanted {:?}", tempo(), want);
            std::thread::sleep(Duration::from_millis(20));
        }
    };
    let set = |milli: u32, pos: u64, flags: u16| {
        session
            .handle()
            .send_command(UiCommandPayload {
                command_type: UiCommandType::SetTempo as u16,
                flags,
                track_id: 0,
                plugin_index: 0,
                note_pitch: 0,
                value0: milli,
                note_nanotick_lo: (pos & 0xffff_ffff) as u32,
                note_nanotick_hi: (pos >> 32) as u32,
                note_duration_lo: 0,
                note_duration_hi: 0,
                base_version: 0,
            })
            .expect("send SetTempo");
    };

    // Default: 120.000 BPM, one point.
    wait_tempo((120_000, 1), "default tempo");
    // Flatten to 90 (flags=1) — one point, ignores position.
    set(90_000, 12345, 1);
    wait_tempo((90_000, 1), "after flatten");
    // Insert a second point at a later position (flags=0). The playhead is at 0, so
    // the read-back tempo stays 90 (the point at/before 0) but the count grows to 2.
    set(140_000, 4 * Q, 0);
    wait_tempo((90_000, 2), "after insert point");
}

/// A failed LoadProject bumps the load seq with ok=0, so the UI can tell it from
/// a silent no-op; a good load reports ok=1.
#[test]
fn failed_load_surfaces_error() {
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let (engine, session) = start_engine("loaderr");
    // A good project.
    std::fs::write(
        engine.proj.join("good.uniproj.json"),
        serde_json::to_string(&json!({
            "schema_version": 4, "meta": {"name":"good"}, "nanoticks_per_quarter": Q,
            "tracks": [ { "track_id": 0, "name": "T" } ]
        })).unwrap(),
    ).unwrap();
    // A malformed project: a future schema version is rejected by deserialize.
    std::fs::write(
        engine.proj.join("bad.uniproj.json"),
        r#"{"schema_version": 9999, "tracks": []}"#,
    ).unwrap();

    let wait_seq = |from: u32, what: &str| -> (u32, u32) {
        let deadline = Instant::now() + Duration::from_secs(3);
        loop {
            let (seq, ok) = session.handle().load_status();
            if seq != from {
                return (seq, ok);
            }
            assert!(Instant::now() < deadline, "{what}: load seq never advanced from {from}");
            std::thread::sleep(Duration::from_millis(20));
        }
    };
    let (seq0, _) = session.handle().load_status();
    assert!(session.execute(&ToolCall { tool: "load".into(), args: json!({"name":"good"}) }).ok);
    let (seq1, ok1) = wait_seq(seq0, "good load");
    assert_eq!(ok1, 1, "good load should report ok=1");
    assert!(session.execute(&ToolCall { tool: "load".into(), args: json!({"name":"bad"}) }).ok);
    let (_seq2, ok2) = wait_seq(seq1, "bad load");
    assert_eq!(ok2, 0, "malformed load should report ok=0, not a silent no-op");
}

/// Per-track mixer state written via SetTrackMixer is published back verbatim, so
/// the UI can render a fader at its true position; the mixer version advances.
#[test]
fn mixer_read_back() {
    use daw_bridge::layout::{UiCommandPayload, UiCommandType, MIXER_FLAG_MUTE};
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let (_engine, session) = start_engine("mixer");
    let h = session.handle();
    let v0 = h.mixer_version();

    // -6 dB (=-600 mB), pan 25% left (-250 thousandths), muted.
    let payload = UiCommandPayload {
        command_type: UiCommandType::SetTrackMixer as u16,
        flags: MIXER_FLAG_MUTE,
        track_id: 0,
        plugin_index: (-250i32) as u32, // pan thousandths
        note_pitch: 0,
        value0: (-600i32) as u32, // gain millibels
        note_nanotick_lo: 0,
        note_nanotick_hi: 0,
        note_duration_lo: 0,
        note_duration_hi: 0,
        base_version: 0,
    };
    h.send_command(payload).expect("send SetTrackMixer");

    let deadline = Instant::now() + Duration::from_secs(3);
    loop {
        let m = h.read_mixer();
        if let Some(t0) = m.first() {
            if t0.gain_millibels == -600
                && t0.pan_thousandths == -250
                && t0.flags & MIXER_FLAG_MUTE as u8 != 0
            {
                break;
            }
        }
        assert!(
            Instant::now() < deadline,
            "mixer read-back never reflected the change: {:?}",
            h.read_mixer()
        );
        std::thread::sleep(Duration::from_millis(20));
    }
    assert_ne!(h.mixer_version(), v0, "mixer_version should advance on a change");
}

/// Seek moves the transport to a position, and Stop halts and rewinds it — the
/// two transport commands the web UI needs beyond play/pause.
#[test]
fn transport_seek_and_stop() {
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let (_engine, session) = start_engine("transport");
    let target = 2 * Q; // within the default 1-bar loop

    let seek = session.execute(&ToolCall {
        tool: "transport".into(),
        args: json!({ "action": "seek", "position": target }),
    });
    assert!(seek.ok, "seek failed: {seek:?}");
    // The published playhead reflects the seek (transport, not a clip edit, so
    // poll the snapshot rather than the clip version).
    let poll = |want: u64, what: &str| {
        let deadline = Instant::now() + Duration::from_secs(3);
        loop {
            if let Some(s) = session.handle().snapshot() {
                if s.ui_global_nanotick_playhead == want {
                    return;
                }
            }
            assert!(Instant::now() < deadline, "{what}: playhead never reached {want}");
            std::thread::sleep(Duration::from_millis(20));
        }
    };
    poll(target, "seek");

    let stop = session.execute(&ToolCall {
        tool: "transport".into(),
        args: json!({ "action": "stop" }),
    });
    assert!(stop.ok, "stop failed: {stop:?}");
    poll(0, "stop rewind");
    let s = session.handle().snapshot().expect("snapshot");
    assert_eq!(s.ui_transport_state, 0, "stop should halt playback");
}

/// An audio clip persists through a load -> save round-trip (kind + source ref
/// intact) and shows as a rail flagged as audio, even though the engine doesn't
/// schedule it yet — the Movement 4 format slot, exercised against a live engine.
#[test]
fn audio_clip_persists_and_flags_rail() {
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let (engine, session) = start_engine("audio");
    let one_bar = 4 * Q;

    let proj = json!({
        "schema_version": 3,
        "meta": { "name": "audio_in", "created_utc": 0, "modified_utc": 0 },
        "nanoticks_per_quarter": Q,
        "clips": [ {
            "id": 5, "name": "Vox", "length": one_bar, "kind": "audio",
            "audio": { "source_path": "/takes/vox.wav", "source_start_frame": 44100,
                       "gain_db": -3.0, "fade_in": 24000, "fade_out": 48000 }
        } ],
        "tracks": [ {
            "track_id": 0, "name": "Track 1",
            "placements": [ { "clip_id": 5, "at": 0, "length": one_bar } ]
        } ]
    });
    std::fs::write(
        engine.proj.join("audio_in.uniproj.json"),
        serde_json::to_string_pretty(&proj).unwrap(),
    )
    .unwrap();

    let before = session.handle().clip_version();
    let load = session.execute(&ToolCall { tool: "load".into(), args: json!({"name":"audio_in"}) });
    assert!(load.ok, "load failed: {load:?}");
    assert!(session.handle().wait_for_clip_version(before, before.wrapping_add(1), Duration::from_secs(3)));

    // The rail is published and flagged as audio. The extent region refreshes on
    // the publish that carries the new clip version, which can trail the version
    // bump by a block, so poll briefly rather than reading once.
    let deadline = Instant::now() + Duration::from_secs(3);
    let audio_rail = loop {
        let extents = session.handle().read_clip_extents();
        if let Some(e) = extents
            .into_iter()
            .find(|e| e.track_id == 0 && e.flags & daw_bridge::layout::UI_CLIP_EXTENT_AUDIO != 0)
        {
            break e;
        }
        assert!(Instant::now() < deadline, "audio rail not published/flagged");
        std::thread::sleep(Duration::from_millis(50));
    };
    assert_eq!(audio_rail.start_tick, 0);
    assert_eq!(audio_rail.end_tick, one_bar);

    // And it survives a save as an audio clip, not flattened away.
    let save = session.execute(&ToolCall { tool: "save".into(), args: json!({"name":"audio_out"}) });
    assert!(save.ok, "save failed: {save:?}");
    let doc = read_project(&engine.proj, "audio_out");
    let clip = doc["clips"]
        .as_array()
        .unwrap()
        .iter()
        .find(|c| c["id"].as_u64() == Some(5))
        .expect("audio clip 5 not re-emitted");
    assert_eq!(clip["kind"].as_str(), Some("audio"), "kind lost: {clip:?}");
    assert_eq!(clip["audio"]["source_path"].as_str(), Some("/takes/vox.wav"));
    assert_eq!(clip["audio"]["source_start_frame"].as_u64(), Some(44100));
}

// ---------------------------------------------------------------------------
// THE TOOLS ADDED FOR THE SPINE, THE RACK, MODULATION AND LANE QUANTIZE.
//
// A tool that compiles is not a tool that works. Every one of these was written by reading the
// engine's payloads, and reading them is exactly how four modulation facts came out wrong in a
// row — a remove that needs the link's devices, an add that refuses an existing id, a uid that
// cannot ride along with the add, and sixteen zero bytes that hex to a truthy string. So each
// tool is CALLED and its effect checked against what the engine published, never against the
// tool's own reply.
// ---------------------------------------------------------------------------

/// Every ToolSpec in the manifest has an arm in `execute`.
///
/// THE FORCING FUNCTION. A tool the model can see and cannot call answers "unknown tool", which
/// reads to the model as the feature being absent and to us as the manifest being complete —
/// the same shape as a console command whose api method does not exist, which this repo has
/// shipped four times.
///
/// Called with NO arguments on purpose: a dispatched tool refuses by naming what it needs, and
/// only an undispatched one says "unknown tool".
#[test]
fn every_tool_in_the_manifest_is_dispatched() {
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let (_engine, session) = start_engine("dispatch");
    let mut undispatched: Vec<String> = Vec::new();
    for spec in daw_agent::tools::tool_manifest() {
        let out = session.execute(&ToolCall { tool: spec.name.into(), args: json!({}) });
        let text = format!("{:?}{:?}", out.error, out.output);
        if text.contains("unknown tool") {
            undispatched.push(spec.name.to_string());
        }
    }
    assert!(undispatched.is_empty(), "tools in the manifest with no dispatch arm: {undispatched:?}");
}

/// THE SPINE, v29: markers are added, renamed, moved and removed, and TIME is a separate op.
///
/// The distinction is the whole point of the contract change. The four marker ops are TOTAL —
/// they move no music and can fail only on a bad id — and `insert_time` is the one that ripples.
/// A span is two adjacent markers, so a "length" is derived, and the test derives it the same
/// way the renderer does.
#[test]
fn marker_tools_drive_the_spine() {
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let (_engine, session) = start_engine("mark");

    let markers = |s: &AgentSession| -> Vec<Value> {
        let out = s.execute(&ToolCall { tool: "markers".into(), args: json!({}) });
        assert!(out.ok, "markers failed: {out:?}");
        out.output["markers"].as_array().cloned().unwrap_or_default()
    };
    let settle = || std::thread::sleep(Duration::from_millis(400));
    let bar = 4 * Q;

    assert!(markers(&session).is_empty(), "a new project has no markers");

    for (tick, name) in [(0u64, "intro"), (4 * bar, "verse"), (12 * bar, "chorus")] {
        let out = session.execute(&ToolCall {
            tool: "edit_marker".into(),
            args: json!({"op":"add","tick":tick,"name":name}),
        });
        assert!(out.ok, "add {name} failed: {out:?}");
        settle();
    }
    let list = markers(&session);
    assert_eq!(list.len(), 3, "three markers: {list:?}");
    // IN TICK ORDER, whatever order they were added in — the engine keeps the spine sorted, and
    // a strip that derived a span from an unsorted list would draw negative widths.
    let ticks: Vec<u64> = list.iter().map(|m| m["nanotick"].as_u64().unwrap()).collect();
    assert_eq!(ticks, vec![0, 4 * bar, 12 * bar], "{list:?}");
    // BARS ARE RESOLVED BY THE ENGINE. 4/4 here, so bar 1, 5 and 13 — but the point is that this
    // reads them rather than dividing ticks, because across a meter change division is wrong.
    let bars: Vec<u64> = list.iter().map(|m| m["bar"].as_u64().unwrap()).collect();
    assert_eq!(bars, vec![1, 5, 13], "{list:?}");
    // The span each marker BEGINS, derived from the next one. The last runs to the song's end.
    assert_eq!(list[0]["span_end"].as_u64(), Some(4 * bar), "{list:?}");
    assert_eq!(list[1]["span_end"].as_u64(), Some(12 * bar), "{list:?}");

    let id = list[1]["id"].as_u64().unwrap();
    let out = session.execute(&ToolCall {
        tool: "edit_marker".into(), args: json!({"op":"rename","id":id,"name":"VERSE ONE"}),
    });
    assert!(out.ok, "rename failed: {out:?}");
    settle();
    assert_eq!(markers(&session)[1]["name"].as_str(), Some("VERSE ONE"));

    /*
     * MOVING A MARKER MOVES THE MARKER, AND NOTHING ELSE.
     *
     * This is the difference from the sections it replaces, where changing a "length" rippled
     * the song. The later marker must NOT move.
     */
    let before = markers(&session);
    let out = session.execute(&ToolCall {
        tool: "edit_marker".into(), args: json!({"op":"move","id":id,"tick":6 * bar}),
    });
    assert!(out.ok, "move failed: {out:?}");
    settle();
    let after = markers(&session);
    assert_eq!(after[1]["nanotick"].as_u64(), Some(6 * bar), "the marker moved: {after:?}");
    assert_eq!(after[2]["nanotick"].as_u64(), before[2]["nanotick"].as_u64(),
               "and NOTHING else did: {after:?}");

    /*
     * INSERTING TIME MOVES EVERYTHING AT OR AFTER IT — which is the capability the drag needed
     * and the reason sections were worth replacing rather than deleting.
     */
    let before = markers(&session);
    let out = session.execute(&ToolCall {
        tool: "insert_time".into(), args: json!({"tick": 6 * bar, "bars": 2}),
    });
    assert!(out.ok, "insert_time failed: {out:?}");
    settle();
    let after = markers(&session);
    assert_eq!(after[0]["nanotick"].as_u64(), before[0]["nanotick"].as_u64(),
               "a marker BEFORE the point stays: {after:?}");
    assert_eq!(after[1]["nanotick"].as_u64(), Some(8 * bar),
               "the marker AT the point moves with the music: {after:?}");
    assert_eq!(after[2]["nanotick"].as_u64(),
               Some(before[2]["nanotick"].as_u64().unwrap() + 2 * bar),
               "and so does every later one: {after:?}");

    // ...AND IT IS UNDOABLE, which SetSectionLength never was: it moved every placement on every
    // track plus three song timelines and pushed no undo entry big enough to hold it.
    let out = session.execute(&ToolCall { tool: "undo".into(), args: json!({}) });
    assert!(out.ok, "undo failed: {out:?}");
    settle();
    let undone = markers(&session);
    assert_eq!(undone[1]["nanotick"].as_u64(), before[1]["nanotick"].as_u64(),
               "undo puts the time back: {undone:?}");

    let out = session.execute(&ToolCall {
        tool: "edit_marker".into(), args: json!({"op":"remove","id":id}),
    });
    assert!(out.ok, "remove failed: {out:?}");
    settle();
    let left = markers(&session);
    assert_eq!(left.len(), 2, "{left:?}");
    assert!(left.iter().all(|m| m["id"].as_u64() != Some(id)), "the right one went: {left:?}");
}

/// Every marker and time refusal NAMES the argument it is about.
///
/// The model reads these. An `ok: false` with an empty body teaches it nothing and it will make
/// the same call again — a loop that costs money and never converges.
#[test]
fn marker_refusals_say_what_is_missing() {
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let (_engine, session) = start_engine("markref");
    for (tool, args, want) in [
        ("edit_marker", json!({"op":"wat"}), "add, remove, rename"),
        ("edit_marker", json!({"op":"remove"}), "id"),
        ("edit_marker", json!({"op":"rename","id":1}), "name"),
        ("edit_marker", json!({"op":"move","id":1}), "tick"),
        ("edit_marker", json!({}), "op"),
        // Zero bars is not a small edit, it is no edit — and it would spend a whole-song
        // transaction and an undo entry on nothing.
        ("insert_time", json!({"tick":0,"bars":0}), "bars"),
        ("insert_time", json!({"bars":4}), "tick"),
        ("set_time_signature", json!({"signature":"7/0"}), "beats/note-value"),
        ("set_time_signature", json!({}), "signature"),
    ] {
        let out = session.execute(&ToolCall { tool: tool.into(), args: args.clone() });
        assert!(!out.ok, "{tool} {args} should be refused: {out:?}");
        let why = out.error.clone().unwrap_or_default();
        assert!(why.contains(want), "{tool} {args} refused without naming {want:?}: {why:?}");
    }
}

/// MODULATION's refusals, which are the interesting half.
///
/// Three of the four ways to make an inert link are caught here rather than sent, because the
/// engine accepts all three and then moves nothing: a same-device source (the applier requires
/// STRICTLY earlier, the validator only refuses later), a malformed uid, and a missing uid. The
/// fourth — an AUTO link id, which cannot be named in the same call — is not refused because it
/// is a legitimate thing to do; it is REPORTED, and that is asserted too.
#[test]
fn modulate_refuses_the_links_that_could_not_work() {
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let (_engine, session) = start_engine("modref");
    let uid = "0".repeat(31) + "1";

    let same = session.execute(&ToolCall {
        tool: "modulate".into(),
        args: json!({"track":0,"source_device":6,"target_device":6,"param_uid":uid}),
    });
    assert!(!same.ok, "a same-device link must be refused: {same:?}");
    assert!(same.error.clone().unwrap_or_default().contains("EARLIER"),
            "and the reason must be the forward rule: {same:?}");

    let bad = session.execute(&ToolCall {
        tool: "modulate".into(),
        args: json!({"track":0,"source_device":5,"target_device":6,"param_uid":"nope"}),
    });
    assert!(!bad.ok && bad.error.clone().unwrap_or_default().contains("32 hex"),
            "a malformed uid must be refused by shape: {bad:?}");

    let none = session.execute(&ToolCall {
        tool: "modulate".into(),
        args: json!({"track":0,"source_device":5,"target_device":6}),
    });
    assert!(!none.ok && none.error.clone().unwrap_or_default().contains("param_uid"),
            "a missing uid must be refused, since the engine ignores the index: {none:?}");

    let rm = session.execute(&ToolCall {
        tool: "unmodulate".into(), args: json!({"track":0,"link":1}),
    });
    assert!(!rm.ok, "a removal without the link's devices must be refused: {rm:?}");
    assert!(rm.error.clone().unwrap_or_default().contains("source_device"),
            "and say which: {rm:?}");
}

/// LANE QUANTIZE, where the UNITS are the whole risk.
///
/// Percent in and thousandths on the wire, and swing BIASED BY +500 so a negative value
/// survives an unsigned field. A tool that forwarded the numbers unchanged would quantize at a
/// tenth of the strength asked for, with a swing near the extreme — and both would look like
/// settings that had been applied. Read back from the SAVED PROJECT, which is the engine's own
/// account of what it holds.
#[test]
fn lane_quantize_tool_converts_its_units() {
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let (engine, session) = start_engine("quant");

    let out = session.execute(&ToolCall {
        tool: "set_lane_quantize".into(),
        args: json!({"track":0,"grid":"1/16","strength":80,"swing":-20}),
    });
    assert!(out.ok, "set_lane_quantize failed: {out:?}");
    std::thread::sleep(Duration::from_millis(500));
    let save = session.execute(&ToolCall { tool: "save".into(), args: json!({"name":"quantout"}) });
    assert!(save.ok, "save failed: {save:?}");
    let doc = read_project(&engine.proj, "quantout");
    let q = &track(&doc, 0)["quantize"];
    assert_eq!(q["grid_nanoticks"].as_u64(), Some(240_000),
               "1/16 is 240000 nanoticks, not a subdivision index: {q:?}");
    assert_eq!(q["strength_milli"].as_u64(), Some(800),
               "80 percent is 800 thousandths: {q:?}");
    assert_eq!(q["swing_milli"].as_i64(), Some(-200),
               "-20 percent is -200 thousandths, unbiased again on the way out: {q:?}");

    let bad = session.execute(&ToolCall {
        tool: "set_lane_quantize".into(), args: json!({"track":0,"grid":"sixteenth"}),
    });
    assert!(!bad.ok && bad.error.clone().unwrap_or_default().contains("unknown grid"),
            "an unknown grid must be refused by name: {bad:?}");
}

/// The agent's `patcher_node` edits land in the DEVICE'S OWN graph, not the shared pool.
///
/// This exists because the end-to-end version of the claim could not be trusted. Asking a model
/// to "add a euclidean node to the patcher" and then reading the published graph tests three
/// things at once — whether the model picked the right device, whether the command was scoped,
/// and which graph the UI happened to have open — and the published graph is the POOL unless a
/// device has been opened, so a correctly scoped edit reads as `owner: 0` exactly like a broken
/// one. That produced a confident wrong diagnosis ("the agent writes pool-scoped edits") off a
/// reading that could not have said otherwise.
///
/// The saved project can say otherwise. A device's own graph is what gets written to disk, and a
/// pool-scoped edit saves as zero nodes on every device — which is the failure this guards, and
/// the one that made per-device patcher state look like it worked for a whole session.
///
/// TWO devices, so "it went somewhere" is not the same answer as "it went to the right place".
#[test]
fn agent_patcher_edits_are_device_scoped() {
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let (engine, session) = start_engine("agpatch");

    // Both start EMPTY, so any node found afterwards was put there by the tool call below.
    let empty = |dev_id: u64, kind: &str| {
        json!({ "device_id": dev_id, "kind": kind, "patcher_node_id": 1, "bypass": false,
                "patcher": { "nodes": [], "edges": [] } })
    };
    let proj = json!({
        "schema_version": 2,
        "meta": { "name": "agpatch_in", "created_utc": 0, "modified_utc": 0 },
        "nanoticks_per_quarter": Q,
        "tempo_map": [ { "nanotick": 0, "bpm": 120 } ],
        "harmony_timeline": [], "clips": [],
        "tracks": [ {
            "track_id": 0, "name": "T", "harmony_quantize": 0, "lines_per_beat": 4,
            "mixer": { "gain_db": 0.0, "pan": 0.0, "mute": false, "solo": false },
            "device_chain": [ empty(10, "patcher_event"), empty(20, "patcher_audio") ],
            "mod_links": [], "placements": []
        } ]
    });
    std::fs::write(engine.proj.join("agpatch_in.uniproj.json"),
                   serde_json::to_string_pretty(&proj).unwrap()).unwrap();
    let load = session.execute(&ToolCall { tool: "load".into(), args: json!({"name":"agpatch_in"}) });
    assert!(load.ok, "load failed: {load:?}");

    /*
     * DEVICE 0 IS REFUSED, and this is the assertion that matters most here.
     *
     * It used to be ACCEPTED and answer `sent: true`. The wire packs the device into 15 bits
     * under a "has device" flag, and 0 with that flag set is what the engine reads as
     * pool-scoped — so the tool reported success and put the node in a graph no project saves.
     *
     * A model reached it by ordinary reasoning, in as many words: "device should be device #0
     * since it was added first in the chain". That is a POSITION. Positions start at 0, ids
     * start at 1, and every device-taking tool here wants the id. A tool that accepts a
     * plausible wrong value and reports success is worse than one that fails, because the next
     * call is made with more confidence.
     */
    let zero = session.execute(&ToolCall {
        tool: "patcher_node".into(),
        args: json!({ "track": 0, "device": 0, "action": "add", "type": "euclidean" }),
    });
    assert!(!zero.ok, "device 0 must be refused, not sent: {zero:?}");
    let why = format!("{zero:?}");
    assert!(why.contains("observe"),
            "the refusal has to say where a real id comes from — a model that is only told 'no' \
             tries the next plausible number: {why}");

    let add = session.execute(&ToolCall {
        tool: "patcher_node".into(),
        args: json!({ "track": 0, "device": 10, "action": "add", "type": "euclidean" }),
    });
    assert!(add.ok, "patcher_node failed: {add:?}");

    // The engine applies the edit asynchronously; save until the graph shows it, then assert.
    let deadline = Instant::now() + Duration::from_secs(5);
    let doc = loop {
        let save = session.execute(&ToolCall { tool: "save".into(), args: json!({"name":"agpatch_out"}) });
        assert!(save.ok, "save failed: {save:?}");
        let doc = read_project(&engine.proj, "agpatch_out");
        let n = track(&doc, 0)["device_chain"].as_array().unwrap().iter()
            .filter_map(|d| d.get("patcher"))
            .filter_map(|p| p["nodes"].as_array())
            .map(|a| a.len()).sum::<usize>();
        if n > 0 { break doc; }
        assert!(Instant::now() < deadline,
                "the node never reached either device's saved graph — a pool-scoped edit saves as \
                 zero nodes everywhere, which is exactly what this looks like");
        std::thread::sleep(Duration::from_millis(150));
    };

    let devices = track(&doc, 0)["device_chain"].as_array().unwrap().clone();
    let nodes_of = |id: u64| -> usize {
        devices.iter()
            .find(|d| d["device_id"].as_u64() == Some(id))
            .and_then(|d| d.get("patcher"))
            .and_then(|p| p["nodes"].as_array())
            .map_or(0, |a| a.len())
    };
    assert_eq!(nodes_of(10), 1,
               "the euclidean belongs to device 10, which is the one the tool was given: {devices:?}");
    assert_eq!(nodes_of(20), 0,
               "device 20 was never named and must be untouched — a tool that edits whichever \
                patcher it finds is worse than one that refuses: {devices:?}");
}

/// The agent can write row ops, and the observation gives it the note id to aim at.
///
/// Two halves of one gap. Seven ops have been published since v23/v32 and no agent tool could set
/// any of them, so everything an agent produced was a grid of plain notes — no rolls, no ghost
/// notes, no push or drag. And `NoteView` dropped `note_id`, so even with a tool there was
/// nothing to address: `delete_note` works on (track, tick), which SetRowOps does not accept.
///
/// THE MASK IS WHAT IS REALLY UNDER TEST. It is built from which arguments were SUPPLIED, not
/// from which are non-zero — a bit clear leaves that op alone, a bit set with value 0 clears it.
/// Derived from non-zero values instead, removal could not be expressed at all, and an unmentioned
/// op would be silently zeroed. So this sets one op, checks the others were not touched, and then
/// clears it by naming it with 0.
#[test]
fn agent_writes_row_ops_by_note_id() {
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let (engine, session) = start_engine("agrowops");

    let proj = json!({
        "schema_version": 2,
        "meta": { "name": "agrowops_in", "created_utc": 0, "modified_utc": 0 },
        "nanoticks_per_quarter": Q,
        "tempo_map": [ { "nanotick": 0, "bpm": 120 } ],
        "harmony_timeline": [],
        "clips": [ { "id": 1, "name": "c", "length": Q * 16, "lines_per_beat": 4,
                     "kind": "symbolic", "time_sig_numerator": 4, "time_sig_denominator": 4,
                     "notes": [ { "nanotick": 0, "duration": Q, "pitch": 60, "velocity": 100,
                                  "column": 0, "note_id": 1 } ],
                     "chords": [] } ],
        "tracks": [ {
            "track_id": 0, "name": "T", "harmony_quantize": 0, "lines_per_beat": 4,
            "mixer": { "gain_db": 0.0, "pan": 0.0, "mute": false, "solo": false },
            "device_chain": [], "mod_links": [],
            "placements": [ { "clip_id": 1, "at": 0, "length": Q * 16,
                              "notes": [], "chords": [], "mutes": [] } ]
        } ]
    });
    std::fs::write(engine.proj.join("agrowops_in.uniproj.json"),
                   serde_json::to_string_pretty(&proj).unwrap()).unwrap();
    let load = session.execute(&ToolCall { tool: "load".into(), args: json!({"name":"agrowops_in"}) });
    assert!(load.ok, "load failed: {load:?}");

    /*
     * THE ID COMES FROM THE OBSERVATION, which is the point. Asking for a window is what makes
     * notes appear at all — the shape alone is counts and ranges — and the id had to travel with
     * them for any of this to be usable.
     */
    /*
     * `observe_track`, not `observe`. The whole-song observation deliberately omits notes — it is
     * the SHAPE, and enumerating every note of every track is what once made it 2.2 MB — so
     * asking it for a note id returns nothing and would make this test hang on a design decision
     * rather than a defect.
     */
    let deadline = Instant::now() + Duration::from_secs(5);
    let note_id = loop {
        let found = observe_track(session.handle(), 0).into_iter()
            .map(|n| n.note_id).find(|id| *id != 0);
        if let Some(id) = found { break id; }
        assert!(Instant::now() < deadline,
                "no note with an id came back — without one no row op can be addressed, whatever \
                 the tool does");
        std::thread::sleep(Duration::from_millis(100));
    };

    let set = session.execute(&ToolCall {
        tool: "set_row_ops".into(),
        args: json!({ "track": 0, "note_id": note_id, "retrigger": 4, "probability": 60 }),
    });
    assert!(set.ok, "set_row_ops failed: {set:?}");

    let saved = |name: &str| -> Value {
        let s = session.execute(&ToolCall { tool: "save".into(), args: json!({"name": name}) });
        assert!(s.ok, "save failed: {s:?}");
        read_project(&engine.proj, name)
    };
    let first_note = |doc: &Value| -> Value {
        doc["clips"].as_array().unwrap().iter()
            .flat_map(|c| c["notes"].as_array().cloned().unwrap_or_default())
            .next().unwrap_or(Value::Null)
    };

    let deadline = Instant::now() + Duration::from_secs(5);
    let n = loop {
        let n = first_note(&saved("agrowops_a"));
        if n["retrigger"].as_u64() == Some(4) { break n; }
        assert!(Instant::now() < deadline, "retrigger never reached the saved note: {n:?}");
        std::thread::sleep(Duration::from_millis(150));
    };
    assert_eq!(n["probability"].as_u64(), Some(60), "probability travelled with it: {n:?}");
    /*
     * AND THE ONES NOT NAMED WERE NOT TOUCHED. project_file.cpp omits an inert op, so their
     * absence is the assertion — if the mask had been derived from values rather than presence,
     * these would have been written as explicit zeros or, worse, silently cleared.
     */
    assert!(n.get("delay").is_none(), "delay was never mentioned and must be untouched: {n:?}");
    assert!(n.get("sound").is_none(), "sound was never mentioned and must be untouched: {n:?}");

    // NAMING AN OP WITH 0 CLEARS IT — the only way to remove one, and the reason the mask is
    // built from presence.
    let clear = session.execute(&ToolCall {
        tool: "set_row_ops".into(),
        args: json!({ "track": 0, "note_id": note_id, "probability": 0 }),
    });
    assert!(clear.ok, "clearing failed: {clear:?}");
    let deadline = Instant::now() + Duration::from_secs(5);
    loop {
        let n = first_note(&saved("agrowops_b"));
        if n.get("probability").is_none() {
            assert_eq!(n["retrigger"].as_u64(), Some(4),
                       "clearing probability must leave retrigger alone: {n:?}");
            break;
        }
        assert!(Instant::now() < deadline, "probability was never cleared: {n:?}");
        std::thread::sleep(Duration::from_millis(150));
    }

    // An empty call is refused rather than sent — a SetRowOps with mask 0 would change nothing
    // and report success, which is the shape that teaches an agent it did something.
    let empty = session.execute(&ToolCall {
        tool: "set_row_ops".into(),
        args: json!({ "track": 0, "note_id": note_id }),
    });
    assert!(!empty.ok, "a call naming no ops must be refused, not sent: {empty:?}");
}

/// A track that follows the harmony timeline says so in the saved project.
#[test]
fn agent_sets_harmony_quantize() {
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let (engine, session) = start_engine("agharmq");

    let proj = json!({
        "schema_version": 2,
        "meta": { "name": "agharmq_in", "created_utc": 0, "modified_utc": 0 },
        "nanoticks_per_quarter": Q,
        "tempo_map": [ { "nanotick": 0, "bpm": 120 } ],
        "harmony_timeline": [], "clips": [],
        "tracks": [ {
            "track_id": 0, "name": "T", "harmony_quantize": 0, "lines_per_beat": 4,
            "mixer": { "gain_db": 0.0, "pan": 0.0, "mute": false, "solo": false },
            "device_chain": [], "mod_links": [], "placements": []
        } ]
    });
    std::fs::write(engine.proj.join("agharmq_in.uniproj.json"),
                   serde_json::to_string_pretty(&proj).unwrap()).unwrap();
    let load = session.execute(&ToolCall { tool: "load".into(), args: json!({"name":"agharmq_in"}) });
    assert!(load.ok, "load failed: {load:?}");

    let on = session.execute(&ToolCall {
        tool: "harmony_quantize".into(), args: json!({ "track": 0 }),
    });
    assert!(on.ok, "harmony_quantize failed: {on:?}");

    // Defaults to ON with no `on` argument — the shortest call must not be a no-op.
    let deadline = Instant::now() + Duration::from_secs(5);
    loop {
        let doc = {
            let s = session.execute(&ToolCall { tool: "save".into(), args: json!({"name":"agharmq_out"}) });
            assert!(s.ok, "save failed: {s:?}");
            read_project(&engine.proj, "agharmq_out")
        };
        let q = &track(&doc, 0)["harmony_quantize"];
        if q.as_bool() == Some(true) || q.as_u64() == Some(1) { break; }
        assert!(Instant::now() < deadline,
                "the track never came to follow the harmony timeline: {q:?}");
        std::thread::sleep(Duration::from_millis(150));
    }

    let off = session.execute(&ToolCall {
        tool: "harmony_quantize".into(), args: json!({ "track": 0, "on": false }),
    });
    assert!(off.ok, "turning it off failed: {off:?}");
}
