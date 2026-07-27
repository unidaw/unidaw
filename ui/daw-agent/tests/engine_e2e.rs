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

use daw_agent::{AgentSession, ToolCall};
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
