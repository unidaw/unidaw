//! daw-sidecar — bridges the engine's shared memory to the web frontend.
//!
//! Attaches to `/daw_engine_ui` through `daw-bridge` and pushes binary frames
//! over a localhost WebSocket. It exists because a browser cannot mmap POSIX
//! shared memory, and because the seqlock's acquire fence belongs in Rust — a
//! JS seqlock over an ArrayBuffer has no acquire semantics, which matters on ARM.
//!
//! It links `daw-bridge` rather than re-describing the layout. Three mirrors of
//! `SHM_LAYOUT.md` already exist, pinned by `static_assert` and
//! `const_assert_eq!`; a fourth unguarded one produces wrong notes rather than a
//! compile error.
//!
//! **Polling, not signalling.** There is no OS-level change notification on the
//! UI segment; the contract is a seqlock keyed on `uiVersion`. The engine bumps
//! it once per audio block — ~11.6 ms at 512/44.1k, so ~86 Hz. We poll faster
//! than that and dedup on the returned version, so at most one frame per engine
//! publish reaches the socket however fast the timer runs. Worst-case staleness
//! is one publish interval plus one poll interval. If zero-idle-CPU ever matters,
//! this is the single place a real signal would go; the wire format is unaffected.
//!
//!   cargo run -p daw-sidecar -- [--port 8174] [--shm /daw_engine_ui] [--hz 120]

use std::net::{TcpListener, TcpStream};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::{Duration, Instant};

use daw_bridge::control::{default_shm_name, EngineHandle};
use daw_bridge::layout::{UiCommandPayload, UiCommandType, UiPatcherPresetCommandPayload};
use daw_bridge::grid::{aggregate_rows, LaneGrid};

/// Wire format, little-endian. The frontend decodes with a DataView.
/// Bump `WIRE_VERSION` here and in `ui-web/src/wire.js` together.
const WIRE_MAGIC: u32 = 0x31_49_4e_55; // "UNI1"
const WIRE_VERSION: u16 = 6;

/// Frame kinds. The channel byte exists from the start so DSP scope feeds can be
/// added additively rather than as a version bump on both sides: per-track scopes
/// are likely, and multiplexing them onto this one socket is two bytes of header
/// versus a protocol change. `feed` identifies which producer within a kind —
/// a track index for scopes, unused for state.
const KIND_STATE: u8 = 0;

/// Header is fixed-size; peaks and notes follow, counted in the header.
const HEADER_BYTES: usize = 56;
#[allow(dead_code)] // documents the wire layout for ui-web/src/wire.js
const NOTE_BYTES: usize = 40;

/// The client's current viewport. It owns zoom and scroll; we own the
/// projection, because LaneGrid is the authority on tick<->row and reimplementing
/// it in JS would be a second definition of the same truth — and JS division
/// cannot express triplet grids (lines_per_beat = 3) at all.
#[derive(Clone, Copy)]
struct Viewport {
    lines_per_beat: u32,
    first_row: u64,
    row_count: u32,
}
impl Default for Viewport {
    fn default() -> Self { Self { lines_per_beat: 4, first_row: 0, row_count: 0 } }
}

fn parse_viewport(txt: &str, vp: &mut Viewport) {
    let field = |k: &str| -> Option<u64> {
        let i = txt.find(k)? + k.len();
        let rest = &txt[i..];
        let start = rest.find(|c: char| c.is_ascii_digit())?;
        let end = rest[start..].find(|c: char| !c.is_ascii_digit()).unwrap_or(rest.len() - start);
        rest[start..start + end].parse().ok()
    };
    if let Some(v) = field("\"linesPerBeat\"") { vp.lines_per_beat = (v as u32).clamp(1, 64); }
    if let Some(v) = field("\"firstRow\"") { vp.first_row = v; }
    if let Some(v) = field("\"rowCount\"") { vp.row_count = (v as u32).min(512); }
}

/// The client's viewport, handed from the command thread to the publish thread.
///
/// It arrives on the COMMAND socket, not the state socket. The state socket is
/// write-only (see the publish loop for why), so the `setViewport` messages the
/// client had been sending there were read by nobody: the sidecar projected every
/// frame at the default 4 lines/beat no matter what the zoom was, and requested
/// 256 aggregate rows regardless of the pool. Nothing errored — a 4-per-beat
/// projection is a perfectly plausible tracker, which is exactly why it survived.
///
/// Packed into one u64 so the publish loop reads it with a single relaxed load
/// and takes no lock at frame rate. Relaxed is enough: each field is republished
/// every time any of them changes, and a frame built from a viewport one poll
/// stale is indistinguishable from one built a poll earlier.
///
/// One viewport for all clients. There is one client in practice; if a second
/// ever matters, this becomes per-connection state rather than a shared cell.
#[derive(Clone)]
struct SharedViewport(Arc<AtomicU64>);

impl SharedViewport {
    fn new(vp: Viewport) -> Self {
        let s = Self(Arc::new(AtomicU64::new(0)));
        s.store(vp);
        s
    }
    /// lines_per_beat: 8 bits (clamped to 64), row_count: 16 (clamped to 512),
    /// first_row: 40 (1.1e12 rows; the timeline is 1e5).
    fn store(&self, vp: Viewport) {
        let packed = (vp.lines_per_beat as u64 & 0xff)
            | ((vp.row_count as u64 & 0xffff) << 8)
            | ((vp.first_row & 0xff_ffff_ffff) << 24);
        self.0.store(packed, Ordering::Relaxed);
    }
    fn load(&self) -> Viewport {
        let p = self.0.load(Ordering::Relaxed);
        Viewport {
            lines_per_beat: (p & 0xff) as u32,
            row_count: ((p >> 8) & 0xffff) as u32,
            first_row: (p >> 24) & 0xff_ffff_ffff,
        }
    }
}

struct Args {
    port: u16,
    cmd_port: u16,
    shm: String,
    hz: u32,
}

fn parse_args() -> Args {
    let mut a = Args { port: 8174, cmd_port: 8175, shm: default_shm_name(), hz: 120 };
    let v: Vec<String> = std::env::args().skip(1).collect();
    let mut i = 0;
    while i < v.len() {
        match v[i].as_str() {
            "--port" if i + 1 < v.len() => { a.port = v[i + 1].parse().unwrap_or(a.port); a.cmd_port = a.port + 1; i += 2; }
            "--cmd-port" if i + 1 < v.len() => { a.cmd_port = v[i + 1].parse().unwrap_or(a.cmd_port); i += 2; }
            "--shm" if i + 1 < v.len() => { a.shm = v[i + 1].clone(); i += 2; }
            "--hz" if i + 1 < v.len() => { a.hz = v[i + 1].parse().unwrap_or(a.hz).clamp(1, 1000); i += 2; }
            _ => i += 1,
        }
    }
    a
}

#[derive(Clone, Copy)]
struct WireNote {
    t_on: u64,
    t_off: u64,
    /// u64, not u32. The previous encoder truncated this and nothing noticed
    /// because note_count was always zero — but two ids 2^32 apart would have
    /// collided, and optimistic-edit reconciliation is exactly the code that
    /// would have discovered it, at the worst possible moment.
    note_id: u64,
    pitch: u8,
    velocity: u8,
    column: u8,
    track: u8,
    retrigger: u8,
    probability: u8,
    /// bit0 muted, bit1 is-add. A muted base note still ships — the feed is
    /// DISPLAY-resolved, so the editor can draw it struck out.
    placement_flags: u8,
    /// u8, not u16: it has to fit the note's 2 spare bytes without growing the
    /// 40-byte stride. Growing it silently misaligned everything after the notes
    /// — the extents decoded as garbage and nothing rendered. Widen only with a
    /// deliberate stride change on both sides.
    placement_id: u8,
    delay_nanoticks: u32,
    /// Row index under the client's current grid, computed by LaneGrid here so
    /// the frontend never re-derives the projection.
    row: u32,
}

#[derive(Default)]
struct Frame {
    version: u64,
    seq: u64,
    playhead_nanotick: u64,
    visual_sample: u64,
    transport: u16,
    track_count: u16,
    clip_version: u32,
    harmony_version: u32,
    window_start: u64,
    window_end: u64,
    peaks: Vec<f32>,
    notes: Vec<WireNote>,
    /// Per (track, row) aggregate for the requested window, from aggregate_rows.
    /// count 0 means the row is empty on that track.
    aggs: Vec<(u32, u8, u8, u8)>,
    agg_rows: u32,
    agg_tracks: u16,
    /// Per-track lines_per_beat. The client needs it for BOTH halves of the
    /// projection: to render a lane on its own grid, and to compute the tick a
    /// write targets. Only the read half knew about it, which is why a note
    /// written for display row 1 landed at row 4.
    lpb: [u8; 8],
    /// Real clip placements from the engine (v11). placement_id, track,
    /// start/end tick, name. Loose session placements are excluded upstream.
    extents: Vec<(u32, u32, u64, u64, [u8; 32])>,
    /// The lines-per-beat the cached `notes` rows were projected with.
    ///
    /// The cache below is keyed on clip_version, because notes only move on an
    /// edit — but their ROW also moves when the viewport's grid changes, and zoom
    /// does not touch clip_version. Without this the rows stayed frozen at
    /// whatever grid was current when the project loaded: change zoom and every
    /// note kept its old row, landing off its own lane's grid. Same shape as
    /// every other bug on this branch — the content changed, the key did not.
    notes_grid: u32,
    /// True once the engine has not published for STALE_AFTER.
    stale: bool,
    /// Scratch, reused so the aggregation path allocates nothing per frame.
    ev: Vec<(u64, u8)>,
}

fn encode(f: &Frame, out: &mut Vec<u8>) {
    out.clear();
    out.extend_from_slice(&WIRE_MAGIC.to_le_bytes());             // 0
    out.extend_from_slice(&WIRE_VERSION.to_le_bytes());           // 4
    out.push(KIND_STATE);                                         // 6
    // 7: for KIND_STATE this is liveness — 1 means the engine has stopped
    // publishing. The engine bumps ui_version once per audio block whether or not
    // transport is running, so a stalled version means the process is gone. The
    // segment stays mapped after it dies, so without this the UI keeps rendering
    // the last frame: connected, plausible, and permanently wrong. That is how
    // "the play button does nothing" presented.
    out.push(f.stale as u8);                                      // 7  feed/status
    out.extend_from_slice(&f.seq.to_le_bytes());                  // 8
    out.extend_from_slice(&f.playhead_nanotick.to_le_bytes());    // 16
    out.extend_from_slice(&f.visual_sample.to_le_bytes());        // 24
    out.extend_from_slice(&f.clip_version.to_le_bytes());         // 32
    out.extend_from_slice(&f.harmony_version.to_le_bytes());      // 36
    out.extend_from_slice(&f.transport.to_le_bytes());            // 40
    out.extend_from_slice(&f.track_count.to_le_bytes());          // 42
    out.extend_from_slice(&(f.peaks.len() as u16).to_le_bytes()); // 44
    // 46: the grid these rows are projected in. Not decoration — the client
    // caches decoded notes and only re-reads them when something in the header
    // says they changed. Zoom changes every row while leaving clip_version and
    // note_count identical, so without this the client keeps rendering rows from
    // the previous grid: notes land at 1/12th of their real position and look
    // like an ordinary, slightly odd pattern. Sixth instance of this shape.
    out.extend_from_slice(&(f.notes_grid as u16).to_le_bytes());  // 46
    out.extend_from_slice(&(f.notes.len() as u32).to_le_bytes()); // 48
    out.extend_from_slice(&f.agg_rows.to_le_bytes());             // 52
    debug_assert_eq!(out.len(), HEADER_BYTES);
    out.extend_from_slice(&f.agg_tracks.to_le_bytes());
    out.extend_from_slice(&(f.extents.len() as u16).to_le_bytes());
    out.extend_from_slice(&f.lpb);
    for p in &f.peaks {
        out.extend_from_slice(&p.to_le_bytes());
    }
    for n in &f.notes {
        out.extend_from_slice(&n.t_on.to_le_bytes());
        out.extend_from_slice(&n.t_off.to_le_bytes());
        out.extend_from_slice(&n.note_id.to_le_bytes());
        out.push(n.pitch);
        out.push(n.velocity);
        out.push(n.column);
        out.push(n.track);
        out.push(n.retrigger);
        out.push(n.probability);
        out.push(n.placement_flags);
        out.push(n.placement_id);
        out.extend_from_slice(&n.delay_nanoticks.to_le_bytes());
        out.extend_from_slice(&n.row.to_le_bytes());
    }
    for &(pid, track, start, end, name) in &f.extents {
        out.extend_from_slice(&pid.to_le_bytes());
        out.extend_from_slice(&track.to_le_bytes());
        out.extend_from_slice(&start.to_le_bytes());
        out.extend_from_slice(&end.to_le_bytes());
        out.extend_from_slice(&name);
    }
    for &(count, rep, lo, hi) in &f.aggs {
        out.extend_from_slice(&count.to_le_bytes());
        out.push(rep); out.push(lo); out.push(hi); out.push(0);
    }
}

fn read_frame(h: &EngineHandle, seq: u64, out: &mut Frame, prev_clip_version: u32, vp: Viewport) -> bool {
    // read_snapshot performs the load-v0 / read / acquire-fence / load-v1 retry
    // and returns the version. Never read header fields raw — that is how you tear.
    let Some(snap) = h.snapshot() else { return false };

    out.version = snap.version;
    out.seq = seq;
    out.playhead_nanotick = snap.ui_global_nanotick_playhead;
    out.visual_sample = snap.ui_visual_sample_count;
    out.transport = snap.ui_transport_state as u16;
    out.track_count = snap.ui_track_count as u16;
    out.clip_version = snap.ui_clip_version;
    out.harmony_version = snap.ui_harmony_version;

    out.peaks.clear();
    let n = (snap.ui_track_count as usize).min(snap.ui_track_peak_rms.len());
    out.peaks.extend_from_slice(&snap.ui_track_peak_rms[..n]);

    // SHM v9: an all-tracks region the engine publishes unasked, indexed by
    // track. No clip-window request, so no write access and no contention for
    // the single-producer command ring — the read path never needs to be a
    // producer at all.
    //
    // The region is rebuilt only when clipVersion moves, so notes are re-parsed
    // only on an edit. Transport, playhead and peaks still update every frame.
    // One grid PER TRACK, from the engine's own published value. The grid is a
    // property of the lane, not of the client's viewport — flattening it to a
    // single viewport-level grid collapsed track 3's triplets and track 5's
    // sextuplets onto quarter rows, where they silently overwrote each other and
    // still rendered a plausible-looking pattern. SHM v10 publishes it so the
    // client never has to guess.
    let lpb = snap.ui_lines_per_beat;
    out.lpb = lpb;

    // ...but `row` on the wire is in the VIEWPORT's grid, not the lane's, because
    // every lane is drawn against one shared row axis. Emitting lane-space rows
    // and reading them as viewport rows is invisible while all lanes agree and
    // silently misplaces every note the moment one doesn't — a lane at 4/beat put
    // its beat-3 note on row 12 while the viewport at 12/beat drew beat 3 at row
    // 36, three beats off, with no error anywhere. The lane grid stays
    // authoritative for what a row MEANS in ticks (note duration, and which rows a
    // lane has at all); the client gets lpb[] for both. One axis on the wire.
    let vp_grid = LaneGrid::new(vp.lines_per_beat);

    if out.clip_version != prev_clip_version || out.notes.is_empty()
        || out.notes_grid != vp.lines_per_beat
    {
        out.notes_grid = vp.lines_per_beat;
        out.notes.clear();
        out.window_start = 0;
        out.window_end = 0;
        for track in 0..(snap.ui_track_count.min(8)) {
            let Some(w) = h.read_track_clip(track) else { continue };
            if track == 0 {
                out.window_start = w.window_start_nanotick;
                out.window_end = w.window_end_nanotick;
            }
            let count = (w.note_count as usize).min(w.notes.len());
            for note in &w.notes[..count] {
                out.notes.push(WireNote {
                    t_on: note.t_on,
                    t_off: note.t_off,
                    note_id: note.note_id,
                    pitch: note.pitch,
                    velocity: note.velocity,
                    column: note.column,
                    track: track as u8,
                    retrigger: note.retrigger,
                    probability: note.probability,
                    placement_flags: note.placement_flags,
                    placement_id: note.placement_id as u8,
                    delay_nanoticks: note.delay_nanoticks,
                    row: vp_grid.row_of_tick(note.t_on) as u32,
                });
            }
        }
    }
    if std::env::var_os("DAW_SIDECAR_DEBUG_ROWS").is_some() {
        for n in out.notes.iter().take(6) {
            eprintln!("dbg: vp_lpb={} track={} t_on={} row={}", vp.lines_per_beat, n.track, n.t_on, n.row);
        }
    }
    out.extents.clear();
    for e in h.read_clip_extents() {
        out.extents.push((e.placement_id, e.track_id, e.start_tick, e.end_tick, e.name));
    }

    // Aggregation is the engine's policy, not the frontend's. aggregate_rows is
    // pure over its inputs, so the same window yields the same answer wherever it
    // is called — which is the point of calling it here rather than approximating
    // it in JS with a count that read as uniform noise at coarse zoom.
    out.aggs.clear();
    out.agg_rows = vp.row_count;
    out.agg_tracks = out.track_count;
    if vp.row_count > 0 {
        // Built through grid.window() rather than a struct literal: RowWindow
        // keeps its fields private precisely so the grid it belongs to cannot be
        // mismatched with the rows in it.
        for t in 0..out.track_count {
            // Aggregates are indexed by viewport row on the client, so they must
            // be BUCKETED by viewport row here. Aggregating each lane on its own
            // grid produced buckets that did not line up with the rows they were
            // drawn into — the same axis mismatch as `row` above, just harder to
            // see because a wrong aggregate still looks like a plausible one.
            let g = vp_grid;
            let window = g.window(
                g.tick_of_row(vp.first_row),
                g.tick_of_row(vp.first_row + vp.row_count as u64),
            );
            out.ev.clear();
            for n in out.notes.iter().filter(|n| n.track as u16 == t) {
                out.ev.push((n.t_on, n.pitch));
            }
            for slot in aggregate_rows(&out.ev, g, window) {
                match slot {
                    Some(a) => out.aggs.push((a.count, a.representative, a.pitch_min, a.pitch_max)),
                    None => out.aggs.push((0, 0, 0, 0)),
                }
            }
        }
    }
    true
}

/// The command path: a second WebSocket, with the writable handle owned by its
/// reader thread.
///
/// Not a second direction on the state socket — two attempts at duplex broke it
/// (non-blocking makes send() fail under ordinary backpressure; a read timeout
/// corrupts the stream mid-frame). And not HTTP either, for two reasons: a TCP
/// handshake per keystroke is real cost when a drag emits a stream of edits, and
/// EngineHandle holds raw pointers so it is not Send — it cannot be shared across
/// connection threads however the front door is shaped. One connection, one
/// owning thread, blocking reads.
///
/// Commands do NOT need to be synchronised with a frame. Every edit carries the
/// clip version it was composed against, and the engine arbitrates by version
/// rather than arrival order — that is what base_version is for. Optimistic
/// editing is: render locally, send with base_version N, reconcile when a frame
/// with N+1 arrives.
///
/// The ring is SPSC, so exactly one producer may write. That is this thread.
///
///   {"type":"play"}
///   {"type":"note","track":0,"pitch":60,"tick":0,"dur":960000,"vel":100,"base":7}
fn parse_num(body: &str, key: &str) -> Option<i64> {
    let i = body.find(key)? + key.len();
    let rest = &body[i..];
    let start = rest.find(|c: char| c.is_ascii_digit() || c == '-')?;
    let end = rest[start..].find(|c: char| !c.is_ascii_digit() && c != '-').unwrap_or(rest.len() - start);
    rest[start..start + end].parse().ok()
}

/// Pull a JSON string field. Same deliberately-small parser as `parse_num`:
/// commands are a handful of flat objects we generate ourselves, so a real JSON
/// dependency would be the largest thing in the binary for no gain.
fn parse_str<'a>(txt: &'a str, key: &str) -> Option<&'a str> {
    let i = txt.find(key)? + key.len();
    let rest = &txt[i..];
    let open = rest.find('"')?;
    let after = &rest[open + 1..];
    let close = after.find('"')?;
    Some(&after[..close])
}

/// A project name has to survive being joined to a directory on the engine side,
/// where it becomes `<dir>/<name>.uniproj.json`. Anything that could climb out of
/// that directory is refused here, at the process boundary that faces the socket,
/// rather than trusted to be handled two hops away.
fn safe_name(name: &str) -> bool {
    !name.is_empty()
        && name.len() <= 28
        && !name.contains(['/', '\\', '\0'])
        && name != ".."
        && name != "."
}

/// Commands that carry a name ride in the same 40-byte slot as the rest; see
/// `UiPatcherPresetCommandPayload::as_command`.
fn build_named(body: &str) -> Option<Result<UiCommandPayload, &'static str>> {
    let ty = if body.contains("\"load\"") {
        UiCommandType::LoadProject
    } else if body.contains("\"save\"") {
        UiCommandType::SaveProject
    } else {
        return None;                       // not a named command at all
    };
    let name = parse_str(body, "\"name\"").unwrap_or("default");
    if !safe_name(name) {
        // Distinct from "unknown command": the command WAS understood and was
        // refused. Collapsing the two would report a rejected name as a typo.
        return Some(Err("bad project name"));
    }
    Some(Ok(UiPatcherPresetCommandPayload::named(ty, name).as_command()))
}

fn build_command(body: &str) -> Result<UiCommandPayload, &'static str> {
    if let Some(r) = build_named(body) { return r; }

    let mut p = UiCommandPayload {
        command_type: UiCommandType::None as u16,
        flags: 0, track_id: 0, plugin_index: 0, note_pitch: 0, value0: 0,
        note_nanotick_lo: 0, note_nanotick_hi: 0,
        note_duration_lo: 0, note_duration_hi: 0,
        base_version: parse_num(body, "\"base\"").unwrap_or(0).max(0) as u32,
    };
    let tick = parse_num(body, "\"tick\"").unwrap_or(0).max(0) as u64;
    p.note_nanotick_lo = tick as u32;
    p.note_nanotick_hi = (tick >> 32) as u32;
    p.track_id = parse_num(body, "\"track\"").unwrap_or(0).max(0) as u32;

    if body.contains("\"play\"") {
        p.command_type = UiCommandType::TogglePlay as u16;
    } else if body.contains("\"note\"") {
        let dur = parse_num(body, "\"dur\"").unwrap_or(960_000).max(1) as u64;
        p.command_type = UiCommandType::WriteNote as u16;
        p.note_pitch = parse_num(body, "\"pitch\"").unwrap_or(60).clamp(0, 127) as u32;
        p.value0 = parse_num(body, "\"vel\"").unwrap_or(100).clamp(0, 127) as u32;
        p.note_duration_lo = dur as u32;
        p.note_duration_hi = (dur >> 32) as u32;
    } else if body.contains("\"delete\"") {
        p.command_type = UiCommandType::DeleteNote as u16;
    } else if body.contains("\"undo\"") {
        p.command_type = UiCommandType::Undo as u16;
    } else if body.contains("\"redo\"") {
        p.command_type = UiCommandType::Redo as u16;
    } else {
        return Err("unknown command");
    }
    Ok(p)
}

fn serve_commands(listener: TcpListener, shm: String, viewport: SharedViewport) {
    for stream in listener.incoming().flatten() {
        let shm = shm.clone();
        let viewport = viewport.clone();
        thread::spawn(move || {
            let mut ws = match tungstenite::accept(stream) { Ok(w) => w, Err(_) => return };
            // Attached HERE, on the thread that will use it: EngineHandle is not
            // Send, so it cannot be created elsewhere and moved in.
            let handle = match EngineHandle::attach(&shm, true) {
                Ok(h) => h,
                Err(e) => { let _ = ws.send(tungstenite::Message::Text(
                    format!("{{\"error\":\"attach failed: {e}\"}}"))); return; }
            };
            eprintln!("sidecar: command client connected");
            loop {
                match ws.read() {
                    Ok(tungstenite::Message::Text(t)) => {
                        // Viewport updates are not engine commands — they change
                        // what we project, not what the song contains, so they
                        // never touch the command ring.
                        if t.contains("\"linesPerBeat\"") {
                            let mut vp = viewport.load();
                            parse_viewport(&t, &mut vp);
                            viewport.store(vp);
                            let reply = format!(
                                "{{\"ok\":true,\"viewport\":{{\"linesPerBeat\":{},\"firstRow\":{},\"rowCount\":{}}}}}",
                                vp.lines_per_beat, vp.first_row, vp.row_count);
                            if ws.send(tungstenite::Message::Text(reply)).is_err() { break; }
                            continue;
                        }
                        let reply = match build_command(&t) {
                            Err(why) => format!("{{\"error\":\"{why}\"}}"),
                            Ok(p) => match handle.send_command(p) {
                                Ok(()) => format!("{{\"ok\":true,\"type\":{}}}", p.command_type),
                                Err(e) => format!("{{\"error\":\"{}\"}}", e.replace('"', "'")),
                            },
                        };
                        if ws.send(tungstenite::Message::Text(reply)).is_err() { break; }
                    }
                    Ok(tungstenite::Message::Close(_)) | Err(_) => break,
                    Ok(_) => {}
                }
            }
            eprintln!("sidecar: command client gone");
        });
    }
}

fn serve(stream: TcpStream, shm: String, hz: u32, clients: Arc<AtomicU64>, viewport: SharedViewport) {
    let peer = stream.peer_addr().map(|a| a.to_string()).unwrap_or_default();
    let mut ws = match tungstenite::accept(stream) {
        Ok(w) => w,
        Err(e) => { eprintln!("sidecar: handshake failed from {peer}: {e}"); return; }
    };
    let n = clients.fetch_add(1, Ordering::Relaxed) + 1;
    eprintln!("sidecar: client connected ({peer}), {n} total");

    let handle = match EngineHandle::attach(&shm, false) {
        Ok(h) => h,
        Err(e) => {
            let _ = ws.send(tungstenite::Message::Text(
                format!("{{\"error\":\"attach failed: {e}\"}}"),
            ));
            clients.fetch_sub(1, Ordering::Relaxed);
            return;
        }
    };

    let period = Duration::from_micros(1_000_000 / hz as u64);
    // Reused across ticks — the steady state allocates nothing, same rule the
    // renderer follows.
    let mut buf: Vec<u8> = Vec::with_capacity(64 * 1024);
    let mut frame = Frame::default();
    let mut last_version = u64::MAX;
    // Fixed generous window; the client slices what it can see. 256 rows across
    // 8 tracks is 16 KB, well under the measured 126 MB/s ceiling.
    // WRITE-ONLY, deliberately. Two attempts at a client->server channel on this
    // one socket both broke it: a non-blocking socket makes send() fail with
    // WouldBlock under ordinary backpressure, and a read timeout corrupts the
    // stream when it fires mid-frame — one frame through, then close 1006.
    // Reading safely needs a second thread — and that thread already exists, on
    // the command port, so the viewport arrives there and reaches us through
    // `SharedViewport`. It used to be a hardcoded constant here while the client
    // dutifully sent updates into this socket's void.
    // The engine publishes every audio block (~11.6 ms at 512/44.1k). A whole
    // second of silence is ~86 missed publishes: it is not late, it is gone.
    const STALE_AFTER: Duration = Duration::from_secs(1);
    // Tracked separately from `last_version`, which is the SEND dedup key. Using
    // one variable for both means forcing a frame out (to report the stall) also
    // counts as the engine having published, and the flag clears itself the
    // instant it is raised.
    let mut last_seen_version = u64::MAX;
    let mut last_change = Instant::now();
    let (mut seq, mut sent, mut polls) = (0u64, 0u64, 0u64);
    let started = Instant::now();
    let mut reported = started;

    loop {
        let tick = Instant::now();
        polls += 1;

        let prev_cv = frame.clip_version;
        // Re-read every poll: zoom and scroll change between frames.
        let read = read_frame(&handle, seq, &mut frame, prev_cv, viewport.load());
        // A stall has to be reported, which means sending a frame when nothing
        // changed — the one case where the dedup below must not win.
        let now = Instant::now();
        if read && frame.version != last_seen_version {
            last_seen_version = frame.version;
            last_change = now;
        }
        let stale = now.duration_since(last_change) > STALE_AFTER;
        if stale != frame.stale {
            frame.stale = stale;
            eprintln!("sidecar: engine {}", if stale { "stopped publishing" } else { "publishing again" });
            last_version = u64::MAX;                 // force one frame out
        }
        if read && frame.version != last_version {
            // Dedup on the engine's own version: the engine publishes at ~86 Hz,
            // we poll faster so we never miss one, and the surplus polls cost a
            // single atomic load each.
            last_version = frame.version;
            seq += 1;
            frame.seq = seq;
            encode(&frame, &mut buf);
            // The socket is non-blocking so viewport reads never stall the poll
            // loop — which means send() reports WouldBlock under ordinary
            // backpressure. Treating that as fatal made the sidecar hang up on
            // itself: the client connected, sent its viewport, and was dropped a
            // moment later showing 'disconnected'. Only a genuine error closes.
            if ws.send(tungstenite::Message::Binary(buf.clone())).is_err() { break; }
            sent += 1;
        }

        if reported.elapsed() >= Duration::from_secs(10) {
            let secs = started.elapsed().as_secs_f64();
            eprintln!(
                "sidecar: {sent} frames / {polls} polls in {secs:.0}s — {:.1} out/s, {:.1} polls/s, {:.0}% deduped",
                sent as f64 / secs,
                polls as f64 / secs,
                100.0 * (1.0 - sent as f64 / polls.max(1) as f64),
            );
            reported = Instant::now();
        }

        if let Some(rest) = period.checked_sub(tick.elapsed()) {
            thread::sleep(rest);
        }
    }

    let n = clients.fetch_sub(1, Ordering::Relaxed) - 1;
    eprintln!("sidecar: client gone ({peer}), {n} remain");
}

fn main() {
    let args = parse_args();
    let listener = match TcpListener::bind(("127.0.0.1", args.port)) {
        Ok(l) => l,
        Err(e) => {
            eprintln!("sidecar: cannot bind 127.0.0.1:{}: {e}", args.port);
            std::process::exit(1);
        }
    };

    // Attach once at startup so a missing engine is obvious immediately rather
    // than surfacing later as an inexplicably empty UI.
    match EngineHandle::attach(&args.shm, false) {
        Ok(_) => eprintln!("sidecar: attached to {}", args.shm),
        Err(e) => eprintln!("sidecar: WARNING cannot attach to {} ({e}) — retrying per client", args.shm),
    }
    eprintln!("sidecar: ws://127.0.0.1:{} polling at {} Hz", args.port, args.hz);

    // Default until the client says otherwise: a plain 4-per-beat grid and a
    // generous window, so a client that never sends a viewport still gets frames.
    let viewport = SharedViewport::new(Viewport { lines_per_beat: 4, first_row: 0, row_count: 256 });

    match TcpListener::bind(("127.0.0.1", args.cmd_port)) {
        Ok(l) => {
            eprintln!("sidecar: ws://127.0.0.1:{} for commands", args.cmd_port);
            let shm = args.shm.clone();
            let vp = viewport.clone();
            thread::spawn(move || serve_commands(l, shm, vp));
        }
        Err(e) => eprintln!("sidecar: no command port {} ({e}) — read-only", args.cmd_port),
    }

    let clients = Arc::new(AtomicU64::new(0));
    for stream in listener.incoming() {
        match stream {
            Ok(s) => {
                let (shm, hz, c) = (args.shm.clone(), args.hz, clients.clone());
                let vp = viewport.clone();
                thread::spawn(move || serve(s, shm, hz, c, vp));
            }
            Err(e) => eprintln!("sidecar: accept failed: {e}"),
        }
    }
}
