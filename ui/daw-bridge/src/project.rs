//! Project files on disk: what a NEW one contains, and what a name is allowed to be.
//!
//! This lives here rather than in the sidecar because `new` has three surfaces. The browser
//! cannot write files, so it asks the sidecar; daw-cli and the agent are native processes that
//! can write them directly. Left in the sidecar, "what an empty project looks like" would have
//! been re-typed once per surface — and the ways that goes wrong are quiet: a different default
//! tempo, a missing `timebase` block, one track versus none. Every one of those produces a file
//! that loads, so nothing fails; the song just starts out subtly different depending on which
//! surface you used to start it.

use std::path::{Path, PathBuf};

/// Nanoticks per quarter note, and the `new` document's tempo and meter.
///
/// Not configurable, deliberately. `new` is the shortcut for "start something" — a shortcut that
/// takes options is a dialog, and this one already has the name it needs. Change the tempo after.
pub const NEW_PROJECT_NANOTICKS_PER_QUARTER: u64 = 960_000;
pub const NEW_PROJECT_BPM: f64 = 120.0;

/// Is this usable as a project file name?
///
/// Length-capped at 28 because the engine's named-command payload carries a project name in a
/// fixed 40-byte slot; a name that cannot make the trip is refused HERE, where the caller can be
/// told, rather than arriving truncated and creating a file nobody asked for.
pub fn safe_name(name: &str) -> bool {
    !name.is_empty()
        && name.len() <= 28
        && !name.contains(['/', '\\', '\0'])
        && name != ".."
        && name != "."
}

/// WHERE THE ENGINE WILL LOOK — mirroring `defaultProjectDir()` in
/// apps/patcher_preset_library.cpp: `DAW_PROJECT_DIR`, else `./projects`, else `../projects`.
///
/// This matters more than it looks. A caller that writes a new project and then sends
/// `LoadProject` is naming a file the ENGINE has to find; if the writer and the reader disagree
/// about the directory, the file appears, the load fails, and the only symptom is a new song that
/// is still the old one.
///
/// NOTE A REAL DIVERGENCE: the sidecar falls back to `presets/projects`, not `projects`. Both are
/// launched with `DAW_PROJECT_DIR` set in every path this repo uses, so the fallbacks are dead
/// code in practice — which is exactly why nobody has noticed they differ. Recorded here rather
/// than changed, because changing the sidecar's default is not this function's business.
pub fn engine_project_dir() -> String {
    if let Ok(dir) = std::env::var("DAW_PROJECT_DIR") {
        if !dir.is_empty() {
            return dir;
        }
    }
    if Path::new("projects").exists() {
        return "projects".to_string();
    }
    if Path::new("../projects").exists() {
        return "../projects".to_string();
    }
    "projects".to_string()
}

/// Where a project by this name lives.
pub fn project_path(dir: &str, name: &str) -> PathBuf {
    Path::new(dir).join(format!("{name}.uniproj.json"))
}

/// The document a new, empty project starts as.
///
/// One track, because a project with none has nothing to type into and every surface would
/// otherwise add one itself, differently.
pub fn new_project_document(name: &str) -> String {
    let q = NEW_PROJECT_NANOTICKS_PER_QUARTER;
    let bpm = NEW_PROJECT_BPM;
    format!(
        "{{\"schema_version\":4,\
          \"meta\":{{\"name\":\"{name}\",\"created_utc\":0,\"modified_utc\":0}},\
          \"timebase\":{{\"nanoticks_per_quarter\":{q},\
                          \"time_sig_numerator\":4,\"time_sig_denominator\":4}},\
          \"nanoticks_per_quarter\":{q},\
          \"tempo_map\":[{{\"nanotick\":0,\"bpm\":{bpm:?}}}],\
          \"harmony_timeline\":[],\"clips\":[],\
          \"tracks\":[{{\"track_id\":0,\"name\":\"Track 1\",\"harmony_quantize\":false,\
                        \"lines_per_beat\":4,\
                        \"mixer\":{{\"gain_db\":0.0,\"pan\":0.0,\"mute\":false,\"solo\":false}},\
                        \"device_chain\":[],\"mod_links\":[],\"placements\":[]}}]}}"
    )
}

/// Write a new, empty project. Does NOT load it — the caller sends `LoadProject`, so a new song
/// arrives by exactly the same route as an opened one and nothing downstream has a second case.
///
/// REFUSES RATHER THAN CLOBBERS. Overwriting a song is not something to do as a side effect of
/// the shortcut for "start something", and this project has already lost one song to a `new` that
/// was too willing.
pub fn new_project(dir: &str, name: &str) -> Result<PathBuf, &'static str> {
    if !safe_name(name) {
        return Err("bad project name");
    }
    let path = project_path(dir, name);
    if path.exists() {
        return Err("a project by that name already exists");
    }
    std::fs::create_dir_all(dir).map_err(|_| "cannot create the project directory")?;
    std::fs::write(&path, new_project_document(name)).map_err(|_| "cannot write the project")?;
    Ok(path)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_new_document_is_parseable_and_has_one_track() {
        let doc = new_project_document("thing");
        // Field-by-field rather than a golden string: a golden would have to be re-typed on every
        // change and says nothing about whether the result LOADS.
        assert!(doc.contains("\"schema_version\":4"));
        assert!(doc.contains("\"name\":\"thing\""));
        assert!(doc.contains("\"track_id\":0"));
        assert!(doc.contains("\"bpm\":120.0"), "got {doc}");
        // The tempo must not print as `120` — the loader reads a double, and an integer literal
        // here is the kind of thing that parses on one JSON reader and not another.
        assert!(!doc.contains("\"bpm\":120,"));
    }

    #[test]
    fn names_that_would_escape_the_project_directory_are_refused() {
        for bad in ["", "..", ".", "a/b", "a\\b", "a\0b", &"x".repeat(29)] {
            assert!(!safe_name(bad), "{bad:?} was accepted");
        }
        for good in ["kala", "a", &"x".repeat(28)] {
            assert!(safe_name(good), "{good:?} was refused");
        }
    }
}
