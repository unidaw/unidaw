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
        // Presets go into the test's own directory. Without this the engine writes into the
        // repo's presets/patcher/, so a test would leave files behind in the working tree —
        // and would pass by finding a preset an EARLIER run had written.
        .env("DAW_PATCHER_PRESET_DIR", proj.join("patcher"))
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
// THE PER-TRACK WAIT DOES NOT FIRE ON ANOTHER TRACK'S EDITS.
//
// add_notes takes its base from clip_version_for_track and then waited on wait_for_clip_version,
// which polls the GLOBAL counter. Crossing them means the wait is satisfied by activity anywhere
// in the song: the caller returns believing its own writes have settled when the engine may not
// have applied them, and the next write to that track carries a base that is already stale.
//
// This asserts the PRIMITIVE rather than the symptom, because the symptom needs a race and this
// does not: edit ONE track, then ask the OTHER track's wait to advance. The global has moved a
// long way; the quiet track has not. A wait that returns true here is reading the wrong counter,
// whatever it is named.
fn the_per_track_wait_ignores_other_tracks() {
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let (_engine, session) = start_engine("perwait");

    let added = session.execute(&ToolCall { tool: "add_track".into(), args: json!({}) });
    assert!(added.ok, "add_track failed: {added:?}");
    let quiet = added.output["track"].as_u64().unwrap() as u32;

    let h = session.handle();
    let quiet_base = h.clip_version_for_track(quiet);
    let global_base = h.clip_version();

    // Sixteen notes onto a DIFFERENT track, so the global counter moves a long way.
    let busy = session.execute(&ToolCall {
        tool: "add_notes".into(),
        args: json!({"track":0,"pitches":[60,60,60,60,60,60,60,60,60,60,60,60,60,60,60,60],
                     "start":0,"step":Q,"duration":Q}),
    });
    assert!(busy.ok, "the write to track 0 failed: {busy:?}");

    // The global HAS moved — without this the test cannot tell the counters apart.
    assert!(h.clip_version().wrapping_sub(global_base) > 0,
            "the global clip version did not move, so this fixture proves nothing");

    let fired = h.wait_for_track_clip_version(
        quiet, quiet_base, quiet_base.wrapping_add(1),
        std::time::Duration::from_millis(250));
    assert!(!fired,
            "the per-track wait returned true for a track nothing was written to — it is polling \
             the global counter. A caller crossing the two returns before its own writes are \
             applied, and its next write to that track is refused as stale.");

    // And the GLOBAL wait, given a global base, IS satisfied — which is what made the crossing
    // invisible: the two calls look alike and one of them is nearly always true.
    assert!(h.wait_for_clip_version(global_base, global_base.wrapping_add(1),
                                    std::time::Duration::from_millis(250)),
            "the global wait should see the track-0 edit; if not, the fixture is wrong");
}


#[test]
// TWO WRITES IN A ROW TO ONE TRACK BOTH LAND.
//
// This is the drum beat: "kick on every beat, snare on 2 and 4" is two add_notes calls, and the
// SECOND one was refused in its entirety while the tool reported ok=true. Measured in a demo
// rehearsal — the engine logged `clip.version_mismatch base=1..4 current=17 track=2` and the
// saved project held pitch 36 and nothing else, after the model had said it added both.
//
// The cause was two counters being crossed. add_notes takes its base from
// clip_version_for_track — correctly PER TRACK, with a comment right there explaining that
// reading the global is the exact failure per-track counters were introduced to end — and then
// waited on wait_for_clip_version, which polls the GLOBAL. So the wait returned as soon as the
// global moved for any reason at all, add_notes returned before the track's own writes had been
// applied, and the next call read a stale per-track version and had every note rejected.
//
// A ONE-CALL TEST CANNOT SEE THIS. The first write always succeeds; it is the second that reads
// the version the first was supposed to have settled.
fn two_writes_in_a_row_to_a_NEW_track_both_land() {
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let (engine, session) = start_engine("twowrites");

    // A FRESHLY ADDED TRACK, because that is the case that failed. The same pair of writes to a
    // track that already existed both land — I checked — so a test on track 0 passes with the
    // defect present, which is the kind of test this suite exists to not write.
    let added = session.execute(&ToolCall { tool: "add_track".into(), args: json!({}) });
    assert!(added.ok, "add_track failed: {added:?}");
    let track = added.output["track"].as_u64().expect("add_track returns the new id") as u64;

    // Kick: sixteen notes, so the version moves far enough that a stale re-read is unambiguous.
    let kick = session.execute(&ToolCall {
        tool: "add_notes".into(),
        args: json!({"track":track,"pitches":[36,36,36,36,36,36,36,36,36,36,36,36,36,36,36,36],
                     "start":0,"step":Q,"duration":Q}),
    });
    assert!(kick.ok, "the kick write failed: {kick:?}");

    // Snare, immediately after, on the SAME track. OFF the kick's ticks on purpose: one note per
    // (tick, column) is the tracker's model, so a snare written onto a kick's tick REPLACES it
    // and the total stays 16 whether or not the second write landed. The first draft of this test
    // did exactly that and could not tell the two apart.
    let snare = session.execute(&ToolCall {
        tool: "add_notes".into(),
        args: json!({"track":track,"pitches":[38,38,38,38,38,38,38,38],
                     "start":Q/2,"step":Q*2,"duration":Q/2}),
    });
    assert!(snare.ok, "the snare write reported failure: {snare:?}");

    assert!(session.execute(&ToolCall { tool: "save".into(), args: json!({"name":"twowrites"}) }).ok);
    let doc = read_project(&engine.proj, "twowrites");
    let all: Vec<(u64, u64)> = doc["clips"].as_array().unwrap().iter()
        .flat_map(|c| c["notes"].as_array().unwrap())
        .map(|n| (n["nanotick"].as_u64().unwrap_or(0), n["pitch"].as_u64().unwrap()))
        .collect();
    let kicks = all.iter().filter(|(_, p)| *p == 36).count();
    let snares = all.iter().filter(|(_, p)| *p == 38).count();

    assert_eq!(kicks, 16, "the kick write did not fully land: {all:?}");
    assert_eq!(snares, 8,
               "THE SECOND WRITE TO A NEW TRACK WAS DROPPED — {snares} of 8 snares landed. This is \
                'ask for a kick and a snare, get a kick', and the tool reported success for notes \
                the engine refused as stale. all={all:?}");
}

#[test]
// THE MODEL CAN WRITE A CHORD PROGRESSION, AND THE STRUM SURVIVES THE FILE.
//
// Chords were reachable from the tracker and from daw-cli and from nothing the agent could say,
// so "make the music by prompting" could produce melodies and never harmony. add_chords writes
// DEGREES, which is the point: the progression follows the harmony lane instead of being frozen
// into whatever key was current when it was written.
//
// SPREAD IS ASSERTED ON PURPOSE. daw-cli's chord command shipped for months sending zero for
// spread and both humanize fields, so no project written through a tool could contain a strummed
// chord — the feature existed everywhere except the surface anyone used. A test that only counted
// chords would pass with the same bug reintroduced here, so this reads the strum back out of the
// saved file.
fn add_chords_writes_a_progression_with_a_strum() {
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let (engine, session) = start_engine("chords");

    // I - V - vi - IV, one per bar, as triads, strummed.
    let r = session.execute(&ToolCall {
        tool: "add_chords".into(),
        args: json!({
            "track": 0,
            "degrees": [1, 5, 6, 4],
            "quality": 1,
            "octave": 4,
            "step": Q * 4,
            "spread": 12000,
            "humanize_velocity": 8,
        }),
    });
    assert!(r.ok, "add_chords failed: {r:?}");
    assert_eq!(r.output["sent"].as_u64(), Some(4), "should have sent four chords: {r:?}");

    assert!(session.execute(&ToolCall { tool: "save".into(), args: json!({"name":"chordprog"}) }).ok);
    let doc = read_project(&engine.proj, "chordprog");

    let mut chords: Vec<&Value> = doc["clips"].as_array().unwrap().iter()
        .flat_map(|c| c["chords"].as_array().map(|a| a.iter()).into_iter().flatten())
        .collect();
    chords.sort_by_key(|c| c["nanotick"].as_u64().unwrap_or(0));

    assert_eq!(chords.len(), 4, "expected four chords in the saved file, got {}: {doc}", chords.len());

    let degrees: Vec<u64> = chords.iter().map(|c| c["degree"].as_u64().unwrap()).collect();
    // ONE-BASED. resolveDegree indexes with `degree - 1` and coerces 0 to 1, so a 0-based
    // reading gives the tonic for the first chord by accident and the wrong chord for every
    // other one — which is why this asserts the exact degrees rather than just the count.
    assert_eq!(degrees, vec![1, 5, 6, 4],
               "the progression must be saved in the order it was asked for, as DEGREES");

    // ONE BAR APART, which is add_chords' default step and not add_notes' quarter — a progression
    // that changed chord every beat is not what a I-V-vi-IV means.
    let ticks: Vec<u64> = chords.iter().map(|c| c["nanotick"].as_u64().unwrap()).collect();
    assert_eq!(ticks, vec![0, Q * 4, Q * 8, Q * 12], "chords should be one bar apart");

    // DEGREE 0 IS REFUSED, not quietly turned into the tonic. The engine coerces it, so without
    // this the 0-based misreading writes a progression whose FIRST chord is right and whose rest
    // are one degree low — the failure that looks like success.
    let zero = session.execute(&ToolCall {
        tool: "add_chords".into(),
        args: json!({"track": 0, "degrees": [0, 4, 5, 3]}),
    });
    assert!(!zero.ok, "degree 0 must be refused, not coerced to the tonic: {zero:?}");
    assert!(format!("{zero:?}").contains("one-based"),
            "the refusal must say WHY, or the model will just try 0 again: {zero:?}");

    for c in &chords {
        assert_eq!(c["quality"].as_u64(), Some(1), "quality did not persist: {c}");
        assert_eq!(c["spread"].as_u64(), Some(12000),
                   "THE STRUM DID NOT PERSIST. This is the field daw-cli sent as zero for months, \
                    so a chord written by a tool could never be anything but a rigid block: {c}");
        assert_eq!(c["humanize_velocity"].as_u64(), Some(8),
                   "humanize_velocity did not persist: {c}");
    }
}

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

/// The sampler tools reach the engine and the settings survive a save.
///
/// Five tools landed together — `sampler_slot`, `sampler_device`, `sampler_envelope`,
/// `sampler_slice`, `sampler_vintage` — and compiling plus satisfying the manifest ratchet proves
/// only that they EXIST. A tool that builds a payload with a field in the wrong place sends
/// happily and changes nothing, which is this wire's default failure mode and has happened here
/// twice this week (an editor id at the wrong offset, a pan written into the wrong payload field).
///
/// So each one is sent and then read back off DISK, which is the engine's own record of what it
/// believes rather than an echo of what was asked.
#[test]
fn sampler_tools_reach_the_engine() {
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let (engine, session) = start_engine("agsampler");

    let proj = json!({
        "schema_version": 2,
        "meta": { "name": "agsampler_in", "created_utc": 0, "modified_utc": 0 },
        "nanoticks_per_quarter": Q,
        "tempo_map": [ { "nanotick": 0, "bpm": 120 } ],
        "harmony_timeline": [], "clips": [],
        "tracks": [ {
            "track_id": 0, "name": "S", "harmony_quantize": 0, "lines_per_beat": 4,
            "mixer": { "gain_db": 0.0, "pan": 0.0, "mute": false, "solo": false },
            // A sampler with one slot, so the slot tools have something to address.
            "device_chain": [ { "device_id": 7, "kind": "sampler", "patcher_node_id": 0,
                                "bypass": false,
                                "sampler": { "slots": [ { "id": 1, "name": "s", "key_low": 0,
                                                          "key_high": 127, "root_key": 60,
                                                          "gate": 0 } ] } } ],
            "mod_links": [], "placements": []
        } ]
    });
    std::fs::write(engine.proj.join("agsampler_in.uniproj.json"),
                   serde_json::to_string_pretty(&proj).unwrap()).unwrap();
    let load = session.execute(&ToolCall { tool: "load".into(), args: json!({"name":"agsampler_in"}) });
    assert!(load.ok, "load failed: {load:?}");

    // A slot field, by NAME. `gate` 1 is the one a person reaches for and the one the runbook
    // tells them to set, so it is the one worth proving.
    let gate = session.execute(&ToolCall {
        tool: "sampler_slot".into(),
        // SLOT 1, NOT 0 — `slot_id` addresses a slot by its ID and the fixture's slot is id 1.
        // Passing 0 addressed a slot that does not exist: the command was accepted, reported
        // success, and changed nothing. Exactly the silent no-op this test was written to catch,
        // arriving first as a fault in the test itself.
        args: json!({ "track": 0, "device": 7, "slot": 1, "field": "gate", "value": 1 }),
    });
    assert!(gate.ok, "sampler_slot failed: {gate:?}");

    // A device field. `default-gate` seeds slots minted AFTER it, so this is not expected to
    // change the slot above — only the device's own record.
    let bank = session.execute(&ToolCall {
        tool: "sampler_device".into(),
        args: json!({ "track": 0, "device": 7, "field": "default-gate", "value": 1 }),
    });
    assert!(bank.ok, "sampler_device failed: {bank:?}");

    let env = session.execute(&ToolCall {
        tool: "sampler_envelope".into(),
        args: json!({ "track": 0, "device": 7, "attack": 200, "decay": 300,
                      "sustain": 50, "release": 400 }),
    });
    assert!(env.ok, "sampler_envelope failed: {env:?}");

    let deadline = Instant::now() + Duration::from_secs(5);
    loop {
        let s = session.execute(&ToolCall { tool: "save".into(), args: json!({"name":"agsampler_out"}) });
        assert!(s.ok, "save failed: {s:?}");
        let doc = read_project(&engine.proj, "agsampler_out");
        let dev = track(&doc, 0)["device_chain"].as_array().unwrap()
            .iter().find(|d| d["device_id"].as_u64() == Some(7)).cloned().unwrap_or(Value::Null);
        let slot = dev["sampler"]["slots"].as_array()
            .and_then(|a| a.first()).cloned().unwrap_or(Value::Null);
        let gate_on = slot["gate"].as_u64() == Some(1) || slot["gate"].as_bool() == Some(true);
        if gate_on {
            // The slot really carries it, so the field name resolved to the right selector.
            assert!(gate_on, "gate did not reach the slot: {slot:?}");
            break;
        }
        assert!(Instant::now() < deadline,
                "the slot's gate never came back set — a field name that resolves to the wrong \
                 selector writes SOMETHING and reports success: {slot:?}");
        std::thread::sleep(Duration::from_millis(150));
    }

    // A mistyped field is refused WITH THE LIST, not sent. The table is long enough that guessing
    // is the normal case, and a silent no-op teaches the wrong lesson.
    let bad = session.execute(&ToolCall {
        tool: "sampler_slot".into(),
        args: json!({ "track": 0, "device": 7, "slot": 1, "field": "gaet", "value": 1 }),
    });
    assert!(!bad.ok, "a misspelt slot field must be refused: {bad:?}");
    assert!(format!("{bad:?}").contains("gate"),
            "the refusal should list the real field names so the caller can correct it: {bad:?}");

    // Device 0 is refused everywhere, with the message that says where a real id comes from.
    for tool in ["sampler_slot", "sampler_device", "sampler_envelope", "sampler_slice",
                 "sampler_vintage", "sampler_emit_rows"] {
        let r = session.execute(&ToolCall {
            tool: tool.into(),
            args: json!({ "track": 0, "device": 0, "slot": 0, "field": "gate", "value": 1,
                          "bits": 12 }),
        });
        assert!(!r.ok, "{tool} accepted device 0: {r:?}");
    }

    // And the two that refuse an EMPTY call rather than sending one that changes nothing.
    let novintage = session.execute(&ToolCall {
        tool: "sampler_vintage".into(), args: json!({ "track": 0, "device": 7 }),
    });
    assert!(!novintage.ok, "vintage with neither bits nor rate must be refused: {novintage:?}");
}

/// The master bus and a marker's colour — two things the agent could not reach.
///
/// `set_mixer` took a track INDEX, and the master's id is 4294901760: a number no model produces
/// and one the observation never prints in a form anyone would type. So the one fader every track
/// passes through was unreachable from this surface. daw-cli hit the identical wall and solved it
/// with `--track master`; this spells it the same way rather than inventing a second convention.
///
/// `edit_marker` accepted a `color` on `add` and nowhere else, so a marker's colour could be
/// chosen once and never changed — while the console and daw-cli have both had that since the
/// field existed.
///
/// Both read back off DISK, because "sent" is not "landed" on this wire.
#[test]
fn the_agent_reaches_the_master_bus_and_recolours_a_marker() {
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let (engine, session) = start_engine("agmaster");

    let proj = json!({
        "schema_version": 2,
        "meta": { "name": "agmaster_in", "created_utc": 0, "modified_utc": 0 },
        "nanoticks_per_quarter": Q,
        "tempo_map": [ { "nanotick": 0, "bpm": 120 } ],
        "harmony_timeline": [], "clips": [],
        "tracks": [ {
            "track_id": 0, "name": "T", "harmony_quantize": 0, "lines_per_beat": 4,
            "mixer": { "gain_db": 0.0, "pan": 0.0, "mute": false, "solo": false },
            "device_chain": [], "mod_links": [], "placements": []
        } ]
    });
    std::fs::write(engine.proj.join("agmaster_in.uniproj.json"),
                   serde_json::to_string_pretty(&proj).unwrap()).unwrap();
    assert!(session.execute(&ToolCall { tool: "load".into(), args: json!({"name":"agmaster_in"}) }).ok);

    let m = session.execute(&ToolCall {
        tool: "set_mixer".into(),
        args: json!({ "track": "master", "gain_db": -6.0, "mute": true }),
    });
    assert!(m.ok, "set_mixer on the master failed: {m:?}");

    // A marker to recolour, then the recolour.
    let add = session.execute(&ToolCall {
        tool: "edit_marker".into(),
        args: json!({ "op": "add", "tick": 0, "name": "A", "color": 1 }),
    });
    assert!(add.ok, "adding a marker failed: {add:?}");
    std::thread::sleep(Duration::from_millis(300));
    let recolour = session.execute(&ToolCall {
        tool: "edit_marker".into(),
        args: json!({ "op": "color", "id": 1, "color": 5 }),
    });
    assert!(recolour.ok, "recolouring failed: {recolour:?}");

    let deadline = Instant::now() + Duration::from_secs(5);
    loop {
        let s = session.execute(&ToolCall { tool: "save".into(), args: json!({"name":"agmaster_out"}) });
        assert!(s.ok, "save failed: {s:?}");
        let doc = read_project(&engine.proj, "agmaster_out");

        // The master is a real track slot carrying UI_TRACK_FLAG_MASTER; in the file it is the
        // track whose id is the master sentinel.
        let master = doc["tracks"].as_array().unwrap().iter()
            .find(|t| t["track_id"].as_u64() == Some(0xFFFF_0000)).cloned();
        let gain = master.as_ref().map(|t| t["mixer"]["gain_db"].as_f64().unwrap_or(0.0));
        let markers = doc["markers"].as_array().cloned().unwrap_or_default();
        // `color_rgb` on disk, not `color` — the tool's argument name and the file's field name
        // are different words for one thing, and looking for the argument's name found nothing
        // while the recolour had landed perfectly.
        let coloured = markers.iter().any(|mk| mk["color_rgb"].as_u64() == Some(5));

        // TOLERANCE, because dB goes to the engine as MILLIBELS and comes back through the
        // integer: -6.0 returns as -6.000000491. Exact equality here would fail forever on a
        // conversion that is working exactly as designed.
        if gain.map_or(false, |g| (g + 6.0).abs() < 0.001) && coloured {
            assert_eq!(master.as_ref().unwrap()["mixer"]["mute"].as_bool(), Some(true),
                       "the mute went with the gain");
            break;
        }
        assert!(Instant::now() < deadline,
                "master gain {gain:?} (want -6.0), marker recoloured {coloured} — a command that \
                 reports sent and does not land is this wire's default failure, which is why this \
                 reads the file rather than the reply. markers: {markers:?}");
        std::thread::sleep(Duration::from_millis(200));
    }

    // An unknown op is refused rather than silently doing nothing.
    let bad = session.execute(&ToolCall {
        tool: "edit_marker".into(), args: json!({ "op": "recolor", "id": 1, "color": 2 }),
    });
    assert!(!bad.ok, "an unknown marker op must be refused: {bad:?}");
}

/// The agent can READ a sampler back, and gets the kit it asked for.
///
/// The write tools landed without an observation half: an agent could load a sample, map it, chop
/// it, shape its envelope and crush it, and could not see any of it — the same shape as the device
/// ids, where five tools took an id nothing reported.
///
/// TWO SAMPLERS, DELIBERATELY. The answer lands in `request_seq % UI_SAMPLER_KIT_SLOTS` and
/// daw-cli once defaulted that sequence to a CONSTANT, so every request matched the slot's
/// existing contents and each read returned the PREVIOUS question's answer. That survived because
/// every fixture had ONE sampler, where the previous answer and the current one are the same kit.
/// With two, a swapped answer is wrong in its CONTENT and not merely in its label.
#[test]
fn the_agent_reads_a_sampler_kit_back() {
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let (engine, session) = start_engine("agkit");

    let sampler = |dev: u64, slot_id: u64, name: &str, lo: u64, hi: u64| json!({
        "device_id": dev, "kind": "sampler", "patcher_node_id": 0, "bypass": false,
        "sampler": { "slots": [ { "id": slot_id, "name": name, "key_low": lo, "key_high": hi,
                                  "root_key": 60, "gate": 0 } ] }
    });
    let proj = json!({
        "schema_version": 2,
        "meta": { "name": "agkit_in", "created_utc": 0, "modified_utc": 0 },
        "nanoticks_per_quarter": Q,
        "tempo_map": [ { "nanotick": 0, "bpm": 120 } ],
        "harmony_timeline": [], "clips": [],
        "tracks": [
            { "track_id": 0, "name": "A", "harmony_quantize": 0, "lines_per_beat": 4,
              "mixer": { "gain_db": 0.0, "pan": 0.0, "mute": false, "solo": false },
              "device_chain": [ sampler(3, 1, "alpha", 0, 127) ], "mod_links": [], "placements": [] },
            { "track_id": 1, "name": "B", "harmony_quantize": 0, "lines_per_beat": 4,
              "mixer": { "gain_db": 0.0, "pan": 0.0, "mute": false, "solo": false },
              "device_chain": [ sampler(4, 2, "beta", 36, 36) ], "mod_links": [], "placements": [] },
        ]
    });
    std::fs::write(engine.proj.join("agkit_in.uniproj.json"),
                   serde_json::to_string_pretty(&proj).unwrap()).unwrap();
    assert!(session.execute(&ToolCall { tool: "load".into(), args: json!({"name":"agkit_in"}) }).ok);

    let read = |track: u64| -> Value {
        let deadline = Instant::now() + Duration::from_secs(5);
        loop {
            let r = session.execute(&ToolCall {
                tool: "sampler_kit".into(), args: json!({ "track": track }),
            });
            if r.ok {
                let v: Value = serde_json::from_str(&serde_json::to_string(&r).unwrap()).unwrap();
                if v.to_string().contains("slots") { return v; }
            }
            assert!(Instant::now() < deadline, "sampler_kit never answered for track {track}: {r:?}");
            std::thread::sleep(Duration::from_millis(150));
        }
    };

    // ASKED IN TURN, and each answer must carry ITS OWN track's slot — not the previous one's.
    let a = read(0).to_string();
    let b = read(1).to_string();
    assert!(a.contains("alpha"), "track 0's kit should hold `alpha`: {a}");
    assert!(b.contains("beta"),
            "track 1's kit came back without `beta` — if it holds `alpha`, the answer slot was \
             read before this request landed and the previous question's answer was returned: {b}");
    assert!(!b.contains("alpha"), "track 1's answer still carries track 0's slot: {b}");

    // The key range distinguishes them in CONTENT as well as in name: 0-127 against 36-36.
    assert!(a.contains("\"key_high\":127"), "track 0's slot spans the keyboard: {a}");
    assert!(b.contains("\"key_high\":36"), "track 1's slot is pinned to one key: {b}");

    // A track with no sampler is refused with a message that says what to do, not a silent empty.
    let none = session.execute(&ToolCall {
        tool: "sampler_kit".into(), args: json!({ "track": 5 }),
    });
    assert!(!none.ok, "a track with no sampler must be refused rather than answered empty: {none:?}");
}

/// Wait until a LOADED project's clips are visible, and hand back the runtime clip id.
///
/// TWO SEPARATE MISTAKES THIS EXISTS TO PREVENT, both of which I made:
///
/// 1. `load` returns as soon as the command is ACCEPTED, not when the document has been swapped.
///    Writing immediately afterwards races the load: the edit is applied to the outgoing document
///    and vanishes when the new one lands. The tool reports `applied: true` and the chords are
///    simply not there — the same silent-no-op shape as a wrong payload, from a different cause.
///
/// 2. **THE FIXTURE'S CLIP ID IS NOT THE RUNTIME'S.** A hand-written project can say `id: 7` and
///    the engine will hold that clip under a different id, so a tool addressed at 7 edits nothing
///    and succeeds. Round trips ARE stable (save/load/save keeps an id), which is exactly what
///    makes this easy to assume wrongly — it is only the first load of an authored fixture that
///    re-numbers.
///
/// So: poll the published extents, which is what an agent would actually do, and take the id from
/// there rather than from the file we wrote.
fn wait_for_clip_on(session: &AgentSession, track: u64, what: &str) -> u64 {
    let deadline = Instant::now() + Duration::from_secs(8);
    loop {
        let r = session.execute(&ToolCall { tool: "clips".into(), args: json!({}) });
        if let Some(list) = r.output["clips"].as_array() {
            if let Some(c) = list.iter().find(|c| c["track"].as_u64() == Some(track)) {
                if let Some(id) = c["clip"].as_u64() { return id; }
            }
        }
        assert!(Instant::now() < deadline,
                "{what}: no clip on track {track} after 8s — `load` returns when the command is \
                 ACCEPTED, not when the document has been swapped, and an edit sent before the \
                 swap is applied to the outgoing document and lost");
        std::thread::sleep(Duration::from_millis(150));
    }
}

/// The agent can take back a chord and a key change it wrote.
///
/// `add_chords` and `set_harmony` were both write-only, so an agent that put a wrong chord or a
/// wrong key on the timeline could overwrite it and never remove it — and a key change nobody
/// wanted is not cosmetic, because every harmony-quantized track is read against that timeline.
///
/// The delete asserts the chord COUNT drops, not that a command was accepted: `DeleteChord`
/// addresses by tick and column, and a mis-addressed removal is accepted and removes nothing.
#[test]
fn the_agent_can_delete_a_chord_and_a_key_change() {
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let (engine, session) = start_engine("agdel");

    let proj = json!({
        "schema_version": 2,
        "meta": { "name": "agdel_in", "created_utc": 0, "modified_utc": 0 },
        "nanoticks_per_quarter": Q,
        "tempo_map": [ { "nanotick": 0, "bpm": 120 } ],
        "harmony_timeline": [], "clips": [ { "id": 1, "name": "c", "length": Q * 16,
            "lines_per_beat": 4, "kind": "symbolic", "time_sig_numerator": 4,
            "time_sig_denominator": 4, "notes": [], "chords": [] } ],
        "tracks": [ {
            "track_id": 0, "name": "T", "harmony_quantize": 0, "lines_per_beat": 4,
            "mixer": { "gain_db": 0.0, "pan": 0.0, "mute": false, "solo": false },
            "device_chain": [], "mod_links": [],
            "placements": [ { "clip_id": 1, "at": 0, "length": Q * 16,
                              "notes": [], "chords": [], "mutes": [] } ]
        } ]
    });
    std::fs::write(engine.proj.join("agdel_in.uniproj.json"),
                   serde_json::to_string_pretty(&proj).unwrap()).unwrap();
    assert!(session.execute(&ToolCall { tool: "load".into(), args: json!({"name":"agdel_in"}) }).ok);
    wait_for_clip_on(&session, 0, "agdel");

    // Two chords, so removing one is visible as a COUNT and not only as an absence.
    let wrote = session.execute(&ToolCall {
        tool: "add_chords".into(),
        args: json!({ "track": 0, "degrees": [1, 5], "start": 0, "step": Q * 4 }),
    });
    assert!(wrote.ok, "add_chords failed: {wrote:?}");

    let chords_in = |name: &str| -> usize {
        let s = session.execute(&ToolCall { tool: "save".into(), args: json!({"name": name}) });
        assert!(s.ok, "save failed: {s:?}");
        read_project(&engine.proj, name)["clips"].as_array().unwrap().iter()
            .filter_map(|c| c["chords"].as_array()).map(|a| a.len()).sum()
    };

    let deadline = Instant::now() + Duration::from_secs(5);
    loop {
        if chords_in("agdel_a") == 2 { break; }
        assert!(Instant::now() < deadline, "the two chords never reached the clip");
        std::thread::sleep(Duration::from_millis(150));
    }

    let gone = session.execute(&ToolCall {
        tool: "delete_chord".into(), args: json!({ "track": 0, "tick": 0 }),
    });
    assert!(gone.ok, "delete_chord failed: {gone:?}");

    let deadline = Instant::now() + Duration::from_secs(5);
    loop {
        let n = chords_in("agdel_b");
        if n == 1 { break; }
        assert!(Instant::now() < deadline,
                "the chord count is {n}, want 1 — DeleteChord addresses by tick and column, and a \
                 mis-addressed removal is ACCEPTED and removes nothing");
        std::thread::sleep(Duration::from_millis(150));
    }

    // A key change, then take it back.
    assert!(session.execute(&ToolCall {
        tool: "set_harmony".into(), args: json!({ "tick": 0, "root": 2, "scale": 1 }),
    }).ok);
    let keys_in = |name: &str| -> usize {
        let s = session.execute(&ToolCall { tool: "save".into(), args: json!({"name": name}) });
        assert!(s.ok, "save failed: {s:?}");
        read_project(&engine.proj, name)["harmony_timeline"].as_array().map_or(0, |a| a.len())
    };
    let deadline = Instant::now() + Duration::from_secs(5);
    loop {
        if keys_in("agdel_c") > 0 { break; }
        assert!(Instant::now() < deadline, "the key change never reached the timeline");
        std::thread::sleep(Duration::from_millis(150));
    }

    let unkey = session.execute(&ToolCall {
        tool: "delete_harmony".into(), args: json!({ "tick": 0 }),
    });
    assert!(unkey.ok, "delete_harmony failed: {unkey:?}");
    let deadline = Instant::now() + Duration::from_secs(5);
    loop {
        let n = keys_in("agdel_d");
        if n == 0 { break; }
        assert!(Instant::now() < deadline,
                "the harmony timeline still holds {n} point(s) — a key nobody wanted is not \
                 cosmetic, every quantized track is read against this");
        std::thread::sleep(Duration::from_millis(150));
    }
}

/// The agent can name a clip — the first tool here that does not use the ring.
///
/// A name does not fit the 40-byte ring payload, so `set_clip_text` rides the bulk carrier. That
/// is the only reason the agent never had it, and it is why this test exists at all: every other
/// tool is a fixed-size struct whose wire is exercised a hundred times over, and this one has a
/// header-plus-bytes layout with exactly one caller.
///
/// Asserts the NAME ON DISK, not that the command was accepted. A bulk send with a stale
/// base_version or a wrong field id is accepted by the carrier and dropped by the engine.
#[test]
fn the_agent_can_name_a_clip() {
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let (engine, session) = start_engine("agname");

    let proj = json!({
        "schema_version": 2,
        "meta": { "name": "agname_in", "created_utc": 0, "modified_utc": 0 },
        "nanoticks_per_quarter": Q,
        "tempo_map": [ { "nanotick": 0, "bpm": 120 } ],
        "harmony_timeline": [], "clips": [ { "id": 7, "name": "before", "length": Q * 16,
            "lines_per_beat": 4, "kind": "symbolic", "time_sig_numerator": 4,
            "time_sig_denominator": 4, "notes": [], "chords": [] } ],
        "tracks": [ {
            "track_id": 0, "name": "T", "harmony_quantize": 0, "lines_per_beat": 4,
            "mixer": { "gain_db": 0.0, "pan": 0.0, "mute": false, "solo": false },
            "device_chain": [], "mod_links": [],
            "placements": [ { "clip_id": 7, "at": 0, "length": Q * 16,
                              "notes": [], "chords": [], "mutes": [] } ]
        } ]
    });
    std::fs::write(engine.proj.join("agname_in.uniproj.json"),
                   serde_json::to_string_pretty(&proj).unwrap()).unwrap();
    assert!(session.execute(&ToolCall { tool: "load".into(), args: json!({"name":"agname_in"}) }).ok);
    // The id the ENGINE holds this clip under, not the 7 the fixture asked for — see
    // wait_for_clip_on. Addressing the file's id renamed nothing and reported success.
    let clip_id = wait_for_clip_on(&session, 0, "agname");

    // Addressing first: a rename that cannot say WHICH clip is the failure this tool refuses to
    // have, so the refusal is asserted rather than assumed.
    let no_clip = session.execute(&ToolCall {
        tool: "set_clip_text".into(), args: json!({ "field": "name", "text": "x" }),
    });
    assert!(!no_clip.ok, "a missing clip id must be refused, not defaulted to clip 0: {no_clip:?}");

    let sent = session.execute(&ToolCall {
        tool: "set_clip_text".into(),
        args: json!({ "track": 0, "clip": clip_id, "field": "name", "text": "Verse Bass" }),
    });
    assert!(sent.ok, "set_clip_text failed: {sent:?}");

    let deadline = Instant::now() + Duration::from_secs(5);
    let mut saw = String::new();
    loop {
        let s = session.execute(&ToolCall {
            tool: "save".into(), args: json!({"name": "agname_out"}) });
        assert!(s.ok, "save failed: {s:?}");
        let doc = read_project(&engine.proj, "agname_out");
        saw = doc["clips"].as_array().unwrap().iter()
            .find(|c| c["id"].as_u64() == Some(clip_id))
            .and_then(|c| c["name"].as_str())
            .unwrap_or("<missing>").to_string();
        if saw == "Verse Bass" { break; }
        assert!(Instant::now() < deadline,
                "the clip is still named {saw:?} — a bulk send with a stale base_version or a \
                 wrong field id is ACCEPTED by the carrier and dropped by the engine, so \
                 `sent: true` proves nothing here");
        std::thread::sleep(Duration::from_millis(150));
    }
    assert_eq!(saw, "Verse Bass");
}

/// Three grid-and-automation writers the agent never had, asserted ON DISK.
///
/// `set_track_grid` (rows-per-beat and the note-overlap rule), `set_clip_grid` (the CLIP's own
/// subdivision, which is drawn BEFORE the track's) and `delete_automation_point` (the counterpart
/// to a writer that could only add).
///
/// Every assertion here reads the SAVED PROJECT rather than the tool's reply, because all three
/// of these are shapes where an accepted command changes nothing: `set_clip_grid` with flags of 0
/// travels and does nothing, and `delete_automation_point` with the wrong param id or the wrong
/// tick is a well-formed removal of a point that is not there.
#[test]
fn the_agent_can_set_grids_and_remove_an_automation_point() {
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let (engine, session) = start_engine("aggrid");

    let proj = json!({
        "schema_version": 2,
        "meta": { "name": "aggrid_in", "created_utc": 0, "modified_utc": 0 },
        "nanoticks_per_quarter": Q,
        "tempo_map": [ { "nanotick": 0, "bpm": 120 } ],
        "harmony_timeline": [], "clips": [ { "id": 3, "name": "c", "length": Q * 16,
            "lines_per_beat": 4, "kind": "symbolic", "time_sig_numerator": 4,
            "time_sig_denominator": 4, "notes": [], "chords": [] } ],
        "tracks": [ {
            "track_id": 0, "name": "T", "harmony_quantize": 0, "lines_per_beat": 4,
            "allow_note_overlap": false,
            "mixer": { "gain_db": 0.0, "pan": 0.0, "mute": false, "solo": false },
            "device_chain": [], "mod_links": [],
            "placements": [ { "clip_id": 3, "at": 0, "length": Q * 16,
                              "notes": [], "chords": [], "mutes": [] } ]
        } ]
    });
    std::fs::write(engine.proj.join("aggrid_in.uniproj.json"),
                   serde_json::to_string_pretty(&proj).unwrap()).unwrap();
    assert!(session.execute(&ToolCall { tool: "load".into(), args: json!({"name":"aggrid_in"}) }).ok);
    let clip3 = wait_for_clip_on(&session, 0, "aggrid");

    // Naming NO field is refused rather than sent: flags of 0 is a command that travels, is
    // accepted, and does nothing — which would pass any "was it accepted" assertion.
    let empty = session.execute(&ToolCall {
        tool: "set_clip_grid".into(), args: json!({ "track": 0, "clip": clip3 }) });
    assert!(!empty.ok, "set_clip_grid naming no field must be refused: {empty:?}");
    let neither = session.execute(&ToolCall {
        tool: "set_track_grid".into(), args: json!({ "track": 0 }) });
    assert!(!neither.ok, "set_track_grid naming no field must be refused: {neither:?}");

    // A point to remove, so the removal has something to prove.
    assert!(session.execute(&ToolCall {
        tool: "write_automation_point".into(),
        args: json!({ "track": 0, "param": "index:0", "tick": Q * 2, "value": 0.75 }),
    }).ok);
    assert!(session.execute(&ToolCall {
        tool: "set_track_grid".into(),
        args: json!({ "track": 0, "lines": 3, "note_overlap": true }),
    }).ok);
    assert!(session.execute(&ToolCall {
        tool: "set_clip_grid".into(),
        args: json!({ "track": 0, "clip": clip3, "lines": 6, "numerator": 7, "denominator": 8 }),
    }).ok);

    let points_at = |doc: &Value| -> usize {
        doc["tracks"].as_array().unwrap().iter()
            .filter_map(|t| t["automation"].as_array())
            .flatten()
            .filter_map(|c| c["points"].as_array())
            .map(|p| p.len()).sum()
    };

    let deadline = Instant::now() + Duration::from_secs(6);
    let mut doc;
    loop {
        assert!(session.execute(&ToolCall {
            tool: "save".into(), args: json!({"name": "aggrid_a"}) }).ok);
        doc = read_project(&engine.proj, "aggrid_a");
        let t = &doc["tracks"][0];
        let c = doc["clips"].as_array().unwrap().iter()
            .find(|c| c["id"].as_u64() == Some(clip3)).unwrap().clone();
        if t["lines_per_beat"] == 3 && t["allow_note_overlap"] == true
            && c["lines_per_beat"] == 6 && c["time_sig_numerator"] == 7
            && c["time_sig_denominator"] == 8 && points_at(&doc) == 1 { break; }
        assert!(Instant::now() < deadline,
                "track lpb={} overlap={} clip lpb={} sig={}/{} points={} — want 3/true, 6, 7/8, 1",
                t["lines_per_beat"], t["allow_note_overlap"], c["lines_per_beat"],
                c["time_sig_numerator"], c["time_sig_denominator"], points_at(&doc));
        std::thread::sleep(Duration::from_millis(150));
    }

    let gone = session.execute(&ToolCall {
        tool: "delete_automation_point".into(),
        args: json!({ "track": 0, "param": "index:0", "tick": Q * 2 }),
    });
    assert!(gone.ok, "delete_automation_point failed: {gone:?}");

    let deadline = Instant::now() + Duration::from_secs(6);
    loop {
        assert!(session.execute(&ToolCall {
            tool: "save".into(), args: json!({"name": "aggrid_b"}) }).ok);
        let n = points_at(&read_project(&engine.proj, "aggrid_b"));
        if n == 0 { break; }
        assert!(Instant::now() < deadline,
                "{n} automation point(s) remain — a removal with the wrong param id or the wrong \
                 tick is well-formed, accepted, and removes nothing");
        std::thread::sleep(Duration::from_millis(150));
    }
}

/// The filter, the pad's name and an audio region's fields — three more the agent could not write.
///
/// `sampler_filter` matters more than its size suggests: `filterType` was read at the kit publish
/// site and written by nothing for a while, so a cutoff envelope built by any surface modulated a
/// filter that was off. The tool REQUIRES a type where daw-cli defaults it, because the command
/// writes the type on every send with no set-flag — so a caller adjusting only the cutoff through
/// the CLI's default turns the filter off on the way past, and gets a successful call for it.
///
/// That difference is asserted here: the filter is set to lp24, then the cutoff is moved WITHOUT
/// naming a type, and the call must be REFUSED rather than silently disabling the filter.
#[test]
fn the_agent_can_set_a_filter_a_pad_name_and_an_audio_region() {
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let (engine, session) = start_engine("agfilt");

    let wav = std::fs::canonicalize("ui-web/test/audio/waveform_probe.wav")
        .or_else(|_| std::fs::canonicalize("../daw/ui-web/test/audio/waveform_probe.wav"))
        .map(|p| p.display().to_string())
        .unwrap_or_else(|_| "waveform_probe.wav".into());
    let proj = json!({
        "schema_version": 2,
        "meta": { "name": "agfilt_in", "created_utc": 0, "modified_utc": 0 },
        "nanoticks_per_quarter": Q,
        "tempo_map": [ { "nanotick": 0, "bpm": 120 } ],
        "harmony_timeline": [],
        // An AUDIO clip, because set_audio_clip's fields exist only on one — the engine refuses
        // with not_an_audio_clip otherwise, which would look exactly like the tool not working.
        "clips": [ { "id": 5, "name": "a", "length": Q * 8, "lines_per_beat": 4, "kind": "audio",
                     "time_sig_numerator": 4, "time_sig_denominator": 4,
                     "audio": { "source_path": wav, "source_start_frame": 0, "gain_db": 0.0,
                                "fade_in": 0, "fade_out": 0 } } ],
        "tracks": [ {
            "track_id": 0, "name": "S", "harmony_quantize": 0, "lines_per_beat": 4,
            "mixer": { "gain_db": 0.0, "pan": 0.0, "mute": false, "solo": false },
            "device_chain": [ { "device_id": 7, "kind": "sampler", "patcher_node_id": 0,
                                "bypass": false,
                                "sampler": { "slots": [ { "id": 1, "name": "s", "key_low": 0,
                                                          "key_high": 127, "root_key": 60,
                                                          "gate": 0 } ] } } ],
            "mod_links": [],
            "placements": [ { "clip_id": 5, "at": 0, "length": Q * 8,
                              "notes": [], "chords": [], "mutes": [] } ]
        } ]
    });
    std::fs::write(engine.proj.join("agfilt_in.uniproj.json"),
                   serde_json::to_string_pretty(&proj).unwrap()).unwrap();
    assert!(session.execute(&ToolCall { tool: "load".into(), args: json!({"name":"agfilt_in"}) }).ok);
    let clip5 = wait_for_clip_on(&session, 0, "agfilt");

    // The refusal that separates this tool from the CLI verb it mirrors.
    let typeless = session.execute(&ToolCall {
        tool: "sampler_filter".into(),
        args: json!({ "track": 0, "device": 7, "cutoff": 400 }),
    });
    assert!(!typeless.ok,
            "a filter change naming no type must be refused — the command writes the type on \
             every send, so this would turn the filter off while adjusting its cutoff: {typeless:?}");

    assert!(session.execute(&ToolCall {
        tool: "sampler_filter".into(),
        args: json!({ "track": 0, "device": 7, "type": "lp24", "cutoff": 400, "resonance": 250 }),
    }).ok);
    assert!(session.execute(&ToolCall {
        tool: "sampler_slot_name".into(),
        args: json!({ "track": 0, "device": 7, "slot": 1, "name": "Snare" }),
    }).ok);
    /*
     * 3000 MILLIBELS, WHICH THE ENGINE CLAMPS TO 2400 — AND THAT IS THE POINT.
     *
     * `engine_clip_commands.cpp` bounds clip gain to [-9600, 2400] millibels. Asking for 3000 and
     * asserting 24.0 dB on disk proves two things at once: the value travelled, and the ENGINE's
     * domain guard is reachable through this tool. daw-cli's own comments record why that matters
     * — a guard whose only caller validates first is a guard nothing exercises, and it rots. The
     * tool deliberately does not duplicate the range for the same reason.
     *
     * It is also still far enough from 0 that a defaulted value could not have produced it.
     */
    assert!(session.execute(&ToolCall {
        tool: "set_audio_clip".into(),
        args: json!({ "track": 0, "clip": clip5, "field": "gain", "value": 3000 }),
    }).ok);
    assert!(session.execute(&ToolCall {
        tool: "set_audio_clip".into(),
        args: json!({ "track": 0, "clip": clip5, "field": "fade_in", "value": Q }),
    }).ok);

    let deadline = Instant::now() + Duration::from_secs(6);
    loop {
        assert!(session.execute(&ToolCall {
            tool: "save".into(), args: json!({"name": "agfilt_out"}) }).ok);
        let doc = read_project(&engine.proj, "agfilt_out");
        let dev = &doc["tracks"][0]["device_chain"][0];
        let ftype = dev["sampler"]["mod_sets"].as_array()
            .and_then(|a| a.first()).map(|m| m["filter_type"].clone()).unwrap_or(Value::Null);
        let pad = dev["sampler"]["slots"].as_array()
            .and_then(|a| a.iter().find(|s| s["id"] == 1))
            .and_then(|s| s["name"].as_str()).unwrap_or("").to_string();
        let clip = doc["clips"].as_array().unwrap().iter()
            .find(|c| c["id"].as_u64() == Some(clip5)).cloned().unwrap_or(Value::Null);
        let fade = clip["audio"]["fade_in"].as_u64().unwrap_or(0);
        // Millibels on the wire, dB on disk, and CLAMPED to +24 by the engine on the way.
        let gain = clip["audio"]["gain_db"].as_f64().unwrap_or(0.0);
        if ftype == 2 && pad == "Snare" && fade == Q as u64 && (gain - 24.0).abs() < 0.05 { break; }
        assert!(Instant::now() < deadline,
                "filter_type={ftype} pad={pad:?} fade_in={fade} gain_db={gain} — \
                 want 2 (lp24), \"Snare\", {Q}, 24.0 (3000 mB clamped to the engine's 2400)");
        std::thread::sleep(Duration::from_millis(200));
    }
}

/// All three column-carrying tools refuse an out-of-range column, rather than two of them casting.
///
/// `add_notes`, `add_chords` and `delete_chord` put the column in the payload's `flags`, and each
/// had grown its own cast: two `as u16`, one `min(u16::MAX)`. None was the READER's bound, which
/// is `kUiEditColumnMask = 0x00FF`.
///
/// AND THE OVERFLOW IS NOT MERELY A WRONG COLUMN. The same 16 bits carry `kUiEditScopeLocal`
/// (1 << 15), so column 32768 does not land in column 0 — it turns a document edit into a
/// placement-local override. That is why this is asserted at all three sites in one test: the
/// rule has three call sites, so a fix at one of them is a fix that comes back.
#[test]
fn every_column_carrying_tool_refuses_a_column_the_engine_cannot_read() {
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let (_engine, session) = start_engine("agcol");

    // 300 truncates to 44; 32768 sets the local-scope bit and leaves column 0.
    for bad in [300u64, 32768] {
        let notes = session.execute(&ToolCall {
            tool: "add_notes".into(),
            args: json!({ "track": 0, "pitches": [60], "start": 0, "step": Q, "column": bad }),
        });
        assert!(!notes.ok, "add_notes accepted column {bad}: {notes:?}");
        let chords = session.execute(&ToolCall {
            tool: "add_chords".into(),
            args: json!({ "track": 0, "degrees": [1], "start": 0, "step": Q, "column": bad }),
        });
        assert!(!chords.ok, "add_chords accepted column {bad}: {chords:?}");
        let del = session.execute(&ToolCall {
            tool: "delete_chord".into(),
            args: json!({ "track": 0, "tick": 0, "column": bad }),
        });
        assert!(!del.ok, "delete_chord accepted column {bad}: {del:?}");
    }

    // The boundary is INCLUSIVE at 255 and the tools still work without a column at all, so the
    // guard cannot be satisfied by refusing everything.
    let ok_edge = session.execute(&ToolCall {
        tool: "delete_chord".into(), args: json!({ "track": 0, "tick": 0, "column": 255 }) });
    assert!(ok_edge.ok, "column 255 is the last legal one and must be accepted: {ok_edge:?}");
}

/// The envelope PENCIL — a drawn curve, not four ADSR numbers.
///
/// `sampler_envelope` sets attack/decay/sustain/release, so every shape the agent could make was
/// the same shape with different numbers. This draws points, which is the only way to express a
/// double attack, a hold, or a rise-then-fall.
///
/// The shape asserted here is deliberately NOT ADSR-describable: it rises, falls, rises again.
/// A test using an ADSR-shaped curve would pass just as well against the old tool doing nothing
/// new, which is the trap this whole session keeps finding.
#[test]
fn the_agent_can_draw_an_envelope_an_adsr_cannot_describe() {
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let (engine, session) = start_engine("agenv");

    let proj = json!({
        "schema_version": 2,
        "meta": { "name": "agenv_in", "created_utc": 0, "modified_utc": 0 },
        "nanoticks_per_quarter": Q,
        "tempo_map": [ { "nanotick": 0, "bpm": 120 } ],
        "harmony_timeline": [], "clips": [],
        "tracks": [ {
            "track_id": 0, "name": "S", "harmony_quantize": 0, "lines_per_beat": 4,
            "mixer": { "gain_db": 0.0, "pan": 0.0, "mute": false, "solo": false },
            "device_chain": [ { "device_id": 7, "kind": "sampler", "patcher_node_id": 0,
                                "bypass": false,
                                "sampler": { "slots": [ { "id": 1, "name": "s", "key_low": 0,
                                                          "key_high": 127, "root_key": 60,
                                                          "gate": 0 } ] } } ],
            "mod_links": [], "placements": []
        } ]
    });
    std::fs::write(engine.proj.join("agenv_in.uniproj.json"),
                   serde_json::to_string_pretty(&proj).unwrap()).unwrap();
    assert!(session.execute(&ToolCall { tool: "load".into(), args: json!({"name":"agenv_in"}) }).ok);

    // Two refusals first, both of which would otherwise be silent wrong shapes.
    let one = session.execute(&ToolCall {
        tool: "sampler_envelope_points".into(),
        args: json!({ "track": 0, "device": 7, "points": [{"time": 0, "value": 0}] }),
    });
    assert!(!one.ok, "a single point is not a shape and must be refused: {one:?}");
    let unsorted = session.execute(&ToolCall {
        tool: "sampler_envelope_points".into(),
        args: json!({ "track": 0, "device": 7, "points": [
            {"time": 500, "value": 1000}, {"time": 100, "value": 0}] }),
    });
    assert!(!unsorted.ok,
            "points out of order must be refused, not sorted — sorting hides a caller that \
             computed the wrong times: {unsorted:?}");

    // RISE, FALL, RISE. No ADSR can do this, which is the point of the tool and of this fixture.
    let drawn = session.execute(&ToolCall {
        tool: "sampler_envelope_points".into(),
        args: json!({ "track": 0, "device": 7, "target": "amp", "points": [
            { "time": 0,      "value": 0 },
            { "time": 50000,  "value": 1000 },
            { "time": 120000, "value": 200, "tension": 40 },
            { "time": 200000, "value": 900 },
            { "time": 400000, "value": 0, "step": true },
        ] }),
    });
    assert!(drawn.ok, "sampler_envelope_points failed: {drawn:?}");

    let deadline = Instant::now() + Duration::from_secs(6);
    loop {
        assert!(session.execute(&ToolCall {
            tool: "save".into(), args: json!({"name": "agenv_out"}) }).ok);
        let doc = read_project(&engine.proj, "agenv_out");
        // Any modulator whose envelope carries our five points — addressed BY TARGET on the wire,
        // so the modulator id is the engine's to choose and must not be assumed here.
        let found = doc["tracks"][0]["device_chain"][0]["sampler"]["mod_sets"].as_array()
            .into_iter().flatten()
            .filter_map(|m| m["modulators"].as_array())
            .flatten()
            .filter_map(|m| m["points"].as_array())
            .find(|pts| pts.len() == 5)
            .cloned();
        if let Some(pts) = found {
            let times: Vec<i64> = pts.iter().filter_map(|p| p["t"].as_i64()).collect();
            let vals: Vec<i64> = pts.iter().filter_map(|p| p["v"].as_i64()).collect();
            assert_eq!(times, vec![0, 50000, 120000, 200000, 400000], "point times");
            assert_eq!(vals, vec![0, 1000, 200, 900, 0], "point values");
            // The non-monotonic middle is what proves this is not an ADSR: value FALLS to 200 and
            // RISES again to 900. An attack/decay/sustain/release curve cannot produce that.
            assert!(vals[1] > vals[2] && vals[3] > vals[2],
                    "the drawn shape must rise, fall and rise — got {vals:?}");
            assert_eq!(pts[2]["tension"].as_i64(), Some(40), "per-point tension survived");
            assert_eq!(pts[4]["flags"].as_i64(), Some(1), "the step flag survived");
            break;
        }
        assert!(Instant::now() < deadline,
                "no envelope with five points reached the saved project — a bulk send with a \
                 wrong header field is accepted by the carrier and dropped by the engine");
        std::thread::sleep(Duration::from_millis(200));
    }
}

/// A patcher graph the agent built can be saved and recalled, rather than rebuilt node by node.
///
/// Asserts THE FILE ON DISK, in the test's own preset directory. The engine reports the outcome as
/// a UI diff on a channel this tool does not read, so `sent: true` carries no information at all
/// here — an empty name, a full disk and a successful save are the same reply.
#[test]
fn the_agent_can_save_a_patcher_preset() {
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let (engine, session) = start_engine("agpreset");
    let preset_dir = engine.proj.join("patcher");

    // Two refusals the tool makes itself, because the engine's version of each answers on a
    // channel an agent cannot hear.
    let empty = session.execute(&ToolCall {
        tool: "save_patcher_preset".into(), args: json!({ "name": "  " }) });
    assert!(!empty.ok, "an empty preset name must be refused here: {empty:?}");
    let long = session.execute(&ToolCall {
        tool: "save_patcher_preset".into(),
        args: json!({ "name": "a_preset_name_far_longer_than_the_wire_field_allows" }) });
    assert!(!long.ok,
            "an oversize name must be refused rather than truncated — it would save under a name \
             the caller cannot predict: {long:?}");

    // A patcher DEVICE to build on, because that is the only graph an agent can edit:
    // patcher_node requires a device id, since a pool edit is not what any project renders.
    let proj = json!({
        "schema_version": 2,
        "meta": { "name": "agpreset_in", "created_utc": 0, "modified_utc": 0 },
        "nanoticks_per_quarter": Q,
        "tempo_map": [ { "nanotick": 0, "bpm": 120 } ],
        "harmony_timeline": [], "clips": [],
        "tracks": [ {
            "track_id": 0, "name": "P", "harmony_quantize": 0, "lines_per_beat": 4,
            "mixer": { "gain_db": 0.0, "pan": 0.0, "mute": false, "solo": false },
            "device_chain": [ { "device_id": 4, "kind": "patcher_event",
                                "patcher_node_id": 0, "bypass": false } ],
            "mod_links": [], "placements": []
        } ]
    });
    std::fs::write(engine.proj.join("agpreset_in.uniproj.json"),
                   serde_json::to_string_pretty(&proj).unwrap()).unwrap();
    assert!(session.execute(&ToolCall {
        tool: "load".into(), args: json!({"name":"agpreset_in"}) }).ok);
    std::thread::sleep(Duration::from_millis(1500));
    let node = session.execute(&ToolCall {
        tool: "patcher_node".into(),
        args: json!({ "track": 0, "device": 4, "type": "euclidean" }),
    });
    assert!(node.ok, "patcher_node failed: {node:?}");
    std::thread::sleep(Duration::from_millis(800));

    let saved = session.execute(&ToolCall {
        tool: "save_patcher_preset".into(), args: json!({ "name": "agentmade" }) });
    assert!(saved.ok, "save_patcher_preset failed: {saved:?}");

    let path = preset_dir.join("agentmade.json");
    let deadline = Instant::now() + Duration::from_secs(6);
    loop {
        if path.exists() {
            let text = std::fs::read_to_string(&path).unwrap_or_default();
            /*
             * THE NODE, not the word "nodes". My first version asserted `text.contains("nodes")`,
             * which an empty `"nodes": []` satisfies — the exact weak assertion this session keeps
             * finding elsewhere, written here by me.
             *
             * This matters more than tidiness: SavePatcherPreset serialises `patcherGraphState`,
             * the SHARED POOL, and the engine's own comment records that "since patcher-is-a-device
             * the pool is not what a project renders". If the pool did not carry the euclidean this
             * test just built on DEVICE 4, the tool would be saving something the agent never made,
             * and a `contains("nodes")` check would have called that a pass.
             */
            assert!(text.contains("euclidean"),
                    "the preset does not contain the euclidean built on device 4 — \
                     SavePatcherPreset serialises the shared pool, so if this is empty the tool \
                     saves a graph the agent never built: {text}");
            break;
        }
        assert!(Instant::now() < deadline,
                "no preset at {} — the engine answers a refused save on a UI channel this tool \
                 does not read, so `sent: true` says nothing about whether a file exists",
                path.display());
        std::thread::sleep(Duration::from_millis(200));
    }
}

/// FOUR KEY CHANGES IN A ROW ALL LAND — the case a live session found broken.
///
/// Reported from real use: "create a chord progression using the harmony lane" over four bars
/// produced several "refused an edit composed against version" and landed two of the four points.
///
/// `requireMatchingHarmonyVersion` demands `baseVersion == current` EXACTLY. `set_harmony` read
/// the counter, sent, and returned immediately — so the second call read a version the engine had
/// not reached yet, and its edit was refused into a resync diff that no tool reads. Every call
/// looked successful from the caller's side; the timeline simply had holes in it.
///
/// Asserts FOUR POINTS ON DISK, because the failure mode is partial success: two of four is what
/// was actually reported, and any check that stops at "the first one worked" passes on it.
#[test]
fn consecutive_harmony_writes_all_land() {
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let (engine, session) = start_engine("agharm");

    // C minor, G minor, A minor, B minor — one per bar, the shape that was asked for.
    let want = [(0u64, 0u64, 2u64), (Q * 4, 7, 2), (Q * 8, 9, 2), (Q * 12, 11, 2)];
    for (i, (tick, root, scale)) in want.iter().enumerate() {
        let r = session.execute(&ToolCall {
            tool: "set_harmony".into(),
            args: json!({ "tick": tick, "root": root, "scale": scale }),
        });
        assert!(r.ok, "set_harmony {i} failed: {r:?}");
        // The tool now reports whether the engine actually took it, rather than assuming.
        assert_eq!(r.output["applied"].as_bool(), Some(true),
                   "set_harmony {i} was sent but never applied — this is the exact shape of the \
                    live report: the call succeeds and the point is not there. {r:?}");
    }

    let deadline = Instant::now() + Duration::from_secs(6);
    loop {
        assert!(session.execute(&ToolCall {
            tool: "save".into(), args: json!({"name": "agharm_out"}) }).ok);
        let doc = read_project(&engine.proj, "agharm_out");
        let pts = doc["harmony_timeline"].as_array().cloned().unwrap_or_default();
        if pts.len() == want.len() {
            let mut got: Vec<(u64, u64)> = pts.iter()
                .map(|p| (p["nanotick"].as_u64().unwrap_or(0), p["root"].as_u64().unwrap_or(99)))
                .collect();
            got.sort();
            let expect: Vec<(u64, u64)> = want.iter().map(|(t, r, _)| (*t, *r)).collect();
            assert_eq!(got, expect, "the four key changes are not the four that were asked for");
            break;
        }
        assert!(Instant::now() < deadline,
                "{} of {} key changes reached the timeline — partial success is the reported \
                 failure, and a check that only looks at the first one passes on it",
                pts.len(), want.len());
        std::thread::sleep(Duration::from_millis(200));
    }
}

/// The agent's patcher LINK actually makes an edge — it never did.
///
/// From a live session: "the AI wasn't able to do it either: it did add the nodes but not the
/// connections". The nodes appeared because `action: "add"` was correct. The absence of cables
/// read as the model not trying.
///
/// It was trying. `patcher_node` with `action: "link"` set the two node ids and nothing else, so
/// `src_port_id` and `dst_port_id` kept the struct's zeros — and port 0 is the event INPUT. Every
/// link it ever sent asked the engine to join an input to an input, was refused with
/// `invalid_port` on a channel no tool reads, and reported `sent: true`.
///
/// The parity registry pointed at this action the whole time, and the ratchet was satisfied
/// because the named tool EXISTED. Existing is not the same as working, which is the lesson this
/// whole file keeps re-teaching.
#[test]
fn the_agents_patcher_link_actually_connects() {
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let (engine, session) = start_engine("aglink");

    let proj = json!({
        "schema_version": 2,
        "meta": { "name": "aglink_in", "created_utc": 0, "modified_utc": 0 },
        "nanoticks_per_quarter": Q,
        "tempo_map": [ { "nanotick": 0, "bpm": 120 } ],
        "harmony_timeline": [], "clips": [],
        "tracks": [ {
            "track_id": 0, "name": "P", "harmony_quantize": 0, "lines_per_beat": 4,
            "mixer": { "gain_db": 0.0, "pan": 0.0, "mute": false, "solo": false },
            "device_chain": [ { "device_id": 4, "kind": "patcher_event",
                                "patcher_node_id": 0, "bypass": false } ],
            "mod_links": [], "placements": []
        } ]
    });
    std::fs::write(engine.proj.join("aglink_in.uniproj.json"),
                   serde_json::to_string_pretty(&proj).unwrap()).unwrap();
    assert!(session.execute(&ToolCall { tool: "load".into(), args: json!({"name":"aglink_in"}) }).ok);
    std::thread::sleep(Duration::from_millis(1500));

    // euclidean -> out. NOT two euclideans: a euclidean emits gates and has no event input, so
    // that pair is refused for a REAL reason and would hide a port bug behind a legitimate one.
    for ty in ["euclidean", "out"] {
        let r = session.execute(&ToolCall {
            tool: "patcher_node".into(),
            args: json!({ "track": 0, "device": 4, "action": "add", "type": ty }),
        });
        assert!(r.ok, "adding {ty} failed: {r:?}");
        std::thread::sleep(Duration::from_millis(600));
    }

    let graph_of = |name: &str| -> (usize, usize) {
        assert!(session.execute(&ToolCall {
            tool: "save".into(), args: json!({"name": name}) }).ok);
        let doc = read_project(&engine.proj, name);
        let dev = &doc["tracks"][0]["device_chain"][0];
        let n = dev["patcher"]["nodes"].as_array().map_or(0, |a| a.len());
        let e = dev["patcher"]["edges"].as_array().map_or(0, |a| a.len());
        (n, e)
    };

    let deadline = Instant::now() + Duration::from_secs(6);
    let ids: Vec<u64> = loop {
        let doc = {
            assert!(session.execute(&ToolCall {
                tool: "save".into(), args: json!({"name": "aglink_a"}) }).ok);
            read_project(&engine.proj, "aglink_a")
        };
        let nodes = doc["tracks"][0]["device_chain"][0]["patcher"]["nodes"]
            .as_array().cloned().unwrap_or_default();
        if nodes.len() == 2 {
            break nodes.iter().filter_map(|n| n["id"].as_u64()).collect();
        }
        assert!(Instant::now() < deadline, "the two nodes never reached the device graph");
        std::thread::sleep(Duration::from_millis(200));
    };
    assert_eq!(graph_of("aglink_b").1, 0, "no edges before the link — else this proves nothing");

    let linked = session.execute(&ToolCall {
        tool: "patcher_node".into(),
        args: json!({ "track": 0, "device": 4, "action": "link",
                      "src": ids[0], "dst": ids[1] }),
    });
    assert!(linked.ok, "link failed: {linked:?}");

    let deadline = Instant::now() + Duration::from_secs(6);
    loop {
        let (n, e) = graph_of("aglink_c");
        if e >= 1 { break; }
        assert!(Instant::now() < deadline,
                "{n} node(s) and {e} edge(s) in the saved device graph — the link reported \
                 success and made nothing. Port 0 is the event INPUT: a link that leaves both \
                 port ids at zero asks to join an input to an input, and the engine refuses it \
                 into a diff no tool reads");
        std::thread::sleep(Duration::from_millis(200));
    }
}

/// The DEVICE RACK, through the agent: add, move, bypass, remove — none of them ever driven.
///
/// Four tools the parity registry counted as covered that no test had called. `move_device` and
/// `remove_device` are exercised through daw-cli by cli-verbs.mjs, which says nothing about the
/// AGENT's versions: they are different code sending different payloads.
///
/// Asserts the ORDER OF IDS in the saved chain, not a count. A move that dropped the device and
/// re-added it keeps the count and loses the identity; a move that did nothing keeps both. Only
/// the sequence tells the three apart. Bypass is read back as its persisted flag.
#[test]
fn the_agent_can_work_the_device_rack() {
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let (engine, session) = start_engine("agrack");

    let chain = |name: &str| -> Vec<(u64, bool)> {
        assert!(session.execute(&ToolCall {
            tool: "save".into(), args: json!({"name": name}) }).ok);
        read_project(&engine.proj, name)["tracks"][0]["device_chain"].as_array()
            .cloned().unwrap_or_default().iter()
            .map(|d| (d["device_id"].as_u64().unwrap_or(0),
                      d["bypass"].as_bool().unwrap_or(false)))
            .collect()
    };

    assert!(chain("agrack_a").is_empty(), "the track starts with no devices");

    for kind in ["sampler", "patcher_event"] {
        let r = session.execute(&ToolCall {
            tool: "add_device".into(), args: json!({ "track": 0, "kind": kind }) });
        assert!(r.ok, "add_device {kind} failed: {r:?}");
        std::thread::sleep(Duration::from_millis(700));
    }

    let deadline = Instant::now() + Duration::from_secs(6);
    let ids: Vec<u64> = loop {
        let c = chain("agrack_b");
        if c.len() == 2 { break c.iter().map(|(id, _)| *id).collect(); }
        assert!(Instant::now() < deadline, "two add_device calls left {} device(s)", c.len());
        std::thread::sleep(Duration::from_millis(200));
    };

    // MOVE the second to the front. Ids, not positions: position 0 and id 0 are different things
    // and confusing them is a documented trap on this wire.
    assert!(session.execute(&ToolCall {
        tool: "move_device".into(),
        args: json!({ "track": 0, "device": ids[1], "position": 0 }),
    }).ok);
    let deadline = Instant::now() + Duration::from_secs(6);
    loop {
        let got: Vec<u64> = chain("agrack_c").iter().map(|(id, _)| *id).collect();
        if got == vec![ids[1], ids[0]] { break; }
        assert!(Instant::now() < deadline,
                "chain is {got:?}, want {:?} — a move that dropped and re-added keeps the COUNT \
                 and loses the identity, so only the order can tell them apart",
                vec![ids[1], ids[0]]);
        std::thread::sleep(Duration::from_millis(200));
    }

    assert!(session.execute(&ToolCall {
        tool: "set_bypass".into(),
        args: json!({ "track": 0, "device": ids[0], "on": true }),
    }).ok);
    let deadline = Instant::now() + Duration::from_secs(6);
    loop {
        let c = chain("agrack_d");
        if c.iter().any(|(id, byp)| *id == ids[0] && *byp) { break; }
        assert!(Instant::now() < deadline, "bypass never reached the saved chain: {c:?}");
        std::thread::sleep(Duration::from_millis(200));
    }

    assert!(session.execute(&ToolCall {
        tool: "remove_device".into(), args: json!({ "track": 0, "device": ids[1] }) }).ok);
    let deadline = Instant::now() + Duration::from_secs(6);
    loop {
        let got: Vec<u64> = chain("agrack_e").iter().map(|(id, _)| *id).collect();
        // The SURVIVOR, not the count: removing the wrong device leaves one either way.
        if got == vec![ids[0]] { break; }
        assert!(Instant::now() < deadline, "chain is {got:?}, want [{}]", ids[0]);
        std::thread::sleep(Duration::from_millis(200));
    }
}

/// Tempo, time signature and the loop range — three more the registry claimed and nothing drove.
#[test]
fn the_agent_can_set_tempo_meter_and_loop() {
    let _serial = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    let (engine, session) = start_engine("agtime");

    assert!(session.execute(&ToolCall {
        tool: "set_tempo".into(), args: json!({ "bpm": 96 }) }).ok);
    assert!(session.execute(&ToolCall {
        tool: "set_time_signature".into(), args: json!({ "signature": "3/4" }) }).ok);
    // A loop is transport state, not document state — asserted through the observation rather
    // than the file, because the file is not where it lives.
    let loop_set = session.execute(&ToolCall {
        tool: "set_loop".into(), args: json!({ "start": 0, "end": Q * 8 }) });
    assert!(loop_set.ok, "set_loop failed: {loop_set:?}");

    let deadline = Instant::now() + Duration::from_secs(6);
    loop {
        assert!(session.execute(&ToolCall {
            tool: "save".into(), args: json!({"name": "agtime_out"}) }).ok);
        let doc = read_project(&engine.proj, "agtime_out");
        let map = doc["tempo_map"].as_array().cloned().unwrap_or_default();
        let bpm = map.first().and_then(|p| p["bpm"].as_f64()).unwrap_or(0.0);
        // No position given means FLATTEN, so one point at 96 — an append would leave 120 first
        // and still read as "the tempo changed".
        let flat = map.len() == 1 && (bpm - 96.0).abs() < 0.01;
        // `timebase.time_sig_numerator` — the SONG's meter. Not the clip's: a clip carries its
        // own, and reading that would have this pass or fail on whether a clip happened to exist.
        let num = doc["timebase"]["time_sig_numerator"].as_u64();
        if flat && num == Some(3) { break; }
        assert!(Instant::now() < deadline,
                "tempo_map={map:?} numerator={num:?} — want one point at 96 and a 3/4 meter");
        std::thread::sleep(Duration::from_millis(200));
    }
}
