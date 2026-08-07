//! A clipboard that survives a process exiting.
//!
//! The web UI keeps its clipboard in a page variable, which is fine — the page is still there when
//! you paste. daw-cli exits between `copy` and `paste`, so it needs somewhere to put it, and the
//! agent's session may or may not outlive the call. That is the whole reason `copy`/`cut`/`paste`
//! sat unbuilt on the parity lists while every other row got closed.
//!
//! A FILE BESIDE THE PROJECTS, deliberately. Not a temp path keyed on a pid, which would make
//! `daw-cli do copy` and `daw-cli do paste` two different clipboards; and not inside a project,
//! because copying from one song and pasting into another is the interesting case.
//!
//! ONE FILE FOR BOTH SURFACES, so the CLI and the agent share it: copy with one and paste with the
//! other. That falls out of putting it here rather than in either binary, and it is the same
//! reason `new_project` and `plan_transpose` live in this crate.
//!
//! NOTES ARE STORED RELATIVE TO THE COPY'S START, exactly as the page's clipboard does — `dt` from
//! the first tick, `d_track` from the first track. An absolute clipboard can only be pasted back
//! where it came from, which is not what anybody means by copy.

use std::path::{Path, PathBuf};

/// One note on the clipboard, positioned relative to the copy's origin.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ClipboardNote {
    /// Ticks after the first note in the copy.
    pub dt: u64,
    pub duration: u64,
    pub pitch: u8,
    pub velocity: u8,
    /// Which note column, carried so a two-column phrase survives the round trip. Dropping this
    /// is what made the page's paste collapse both columns onto column 0.
    pub column: u8,
    /// Tracks below the first track in the copy.
    pub d_track: u32,
}

/// Where the clipboard lives, given the project directory.
pub fn clipboard_path(project_dir: &str) -> PathBuf {
    Path::new(project_dir).join(".clipboard.json")
}

/// Serialise. Hand-written rather than pulled through serde because this crate has no serde
/// dependency and the shape is six integers — a dependency for that is not a trade worth making.
pub fn encode(notes: &[ClipboardNote]) -> String {
    let mut out = String::from("{\"schema\":1,\"notes\":[");
    for (i, n) in notes.iter().enumerate() {
        if i > 0 {
            out.push(',');
        }
        out.push_str(&format!(
            "{{\"dt\":{},\"duration\":{},\"pitch\":{},\"velocity\":{},\"column\":{},\"d_track\":{}}}",
            n.dt, n.duration, n.pitch, n.velocity, n.column, n.d_track));
    }
    out.push_str("]}");
    out
}

/// Parse what `encode` wrote. Field-by-field on each object, so a file from a newer version with
/// extra keys still reads rather than failing — and a MISSING key is an error rather than a zero,
/// because a silent 0 for `pitch` is a note nobody copied.
pub fn decode(text: &str) -> Result<Vec<ClipboardNote>, String> {
    let mut out = Vec::new();
    let body = match text.find("\"notes\":[") {
        Some(i) => &text[i + 9..],
        None => return Err("clipboard file has no notes array".into()),
    };
    for chunk in body.split('{').skip(1) {
        let obj = match chunk.find('}') {
            Some(i) => &chunk[..i],
            None => continue,
        };
        let field = |k: &str| -> Result<u64, String> {
            let needle = format!("\"{k}\":");
            let at = obj.find(&needle).ok_or_else(|| format!("clipboard note has no {k}"))?;
            let rest = &obj[at + needle.len()..];
            let end = rest.find(|c: char| !c.is_ascii_digit() && c != '-').unwrap_or(rest.len());
            rest[..end].trim().parse::<u64>().map_err(|_| format!("clipboard {k} is not a number"))
        };
        out.push(ClipboardNote {
            dt: field("dt")?,
            duration: field("duration")?,
            pitch: field("pitch")?.min(127) as u8,
            velocity: field("velocity")?.min(127) as u8,
            column: field("column")?.min(255) as u8,
            d_track: field("d_track")? as u32,
        });
    }
    Ok(out)
}

/// Write the clipboard. Overwrites: a copy replaces what was there, which is what every clipboard
/// in the world does.
pub fn store(project_dir: &str, notes: &[ClipboardNote]) -> Result<PathBuf, String> {
    let path = clipboard_path(project_dir);
    std::fs::create_dir_all(project_dir)
        .map_err(|e| format!("cannot create {project_dir}: {e}"))?;
    std::fs::write(&path, encode(notes)).map_err(|e| format!("cannot write clipboard: {e}"))?;
    Ok(path)
}

/// Read it back. An ABSENT file is "nothing has been copied" — a distinct answer from a corrupt
/// one, and the caller should be able to say which.
pub fn load(project_dir: &str) -> Result<Vec<ClipboardNote>, String> {
    let path = clipboard_path(project_dir);
    match std::fs::read_to_string(&path) {
        Ok(text) => decode(&text),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(Vec::new()),
        Err(e) => Err(format!("cannot read clipboard: {e}")),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn n(dt: u64, pitch: u8, column: u8, d_track: u32) -> ClipboardNote {
        ClipboardNote { dt, duration: 480, pitch, velocity: 100, column, d_track }
    }

    #[test]
    fn a_phrase_survives_the_round_trip_including_its_columns() {
        // The column is the field the page's clipboard dropped, which collapsed a two-column
        // phrase onto column 0 where the second note replaced the first.
        let notes = [n(0, 60, 0, 0), n(960, 67, 1, 0), n(1920, 72, 0, 2)];
        let back = decode(&encode(&notes)).expect("decodes");
        assert_eq!(back, notes.to_vec());
    }

    #[test]
    fn an_empty_clipboard_encodes_and_decodes() {
        assert_eq!(decode(&encode(&[])).unwrap(), Vec::new());
    }

    #[test]
    fn a_missing_field_is_an_error_rather_than_a_zero() {
        // A silent 0 for pitch is a note nobody copied, sounding at C-1.
        let bad = "{\"schema\":1,\"notes\":[{\"dt\":0,\"duration\":480,\"velocity\":100,\"column\":0,\"d_track\":0}]}";
        assert!(decode(bad).is_err(), "a note with no pitch must not decode");
    }

    #[test]
    fn a_file_that_is_not_a_clipboard_is_refused() {
        assert!(decode("{\"hello\":true}").is_err());
    }

    #[test]
    fn store_and_load_round_trip_through_a_directory() {
        let dir = std::env::temp_dir().join(format!("clipfile_{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let d = dir.to_string_lossy().to_string();
        // Nothing copied yet is an EMPTY list, not an error — "you have not copied anything" and
        // "your clipboard is corrupt" are different things to tell somebody.
        assert_eq!(load(&d).unwrap(), Vec::new());
        store(&d, &[n(0, 60, 1, 0)]).expect("stores");
        assert_eq!(load(&d).unwrap(), vec![n(0, 60, 1, 0)]);
        let _ = std::fs::remove_dir_all(&dir);
    }
}
