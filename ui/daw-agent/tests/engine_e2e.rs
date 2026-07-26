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
    let _serial = SERIAL.lock().unwrap();
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
    assert_eq!(doc["schema_version"].as_u64(), Some(2));
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
    let _serial = SERIAL.lock().unwrap();
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
