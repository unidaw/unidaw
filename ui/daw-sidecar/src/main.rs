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
use daw_bridge::grid::{aggregate_rows, LaneGrid};

/// Wire format, little-endian. The frontend decodes with a DataView.
/// Bump `WIRE_VERSION` here and in `ui-web/src/wire.js` together.
const WIRE_MAGIC: u32 = 0x31_49_4e_55; // "UNI1"
const WIRE_VERSION: u16 = 3;

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

/// Minimal field scrape — the control channel carries four integers and does not
/// justify a JSON dependency.
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

struct Args {
    port: u16,
    shm: String,
    hz: u32,
}

fn parse_args() -> Args {
    let mut a = Args { port: 8174, shm: default_shm_name(), hz: 120 };
    let v: Vec<String> = std::env::args().skip(1).collect();
    let mut i = 0;
    while i < v.len() {
        match v[i].as_str() {
            "--port" if i + 1 < v.len() => { a.port = v[i + 1].parse().unwrap_or(a.port); i += 2; }
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
    /// Scratch, reused so the aggregation path allocates nothing per frame.
    ev: Vec<(u64, u8)>,
}

fn encode(f: &Frame, out: &mut Vec<u8>) {
    out.clear();
    out.extend_from_slice(&WIRE_MAGIC.to_le_bytes());             // 0
    out.extend_from_slice(&WIRE_VERSION.to_le_bytes());           // 4
    out.push(KIND_STATE);                                         // 6
    out.push(0);                                                  // 7  feed
    out.extend_from_slice(&f.seq.to_le_bytes());                  // 8
    out.extend_from_slice(&f.playhead_nanotick.to_le_bytes());    // 16
    out.extend_from_slice(&f.visual_sample.to_le_bytes());        // 24
    out.extend_from_slice(&f.clip_version.to_le_bytes());         // 32
    out.extend_from_slice(&f.harmony_version.to_le_bytes());      // 36
    out.extend_from_slice(&f.transport.to_le_bytes());            // 40
    out.extend_from_slice(&f.track_count.to_le_bytes());          // 42
    out.extend_from_slice(&(f.peaks.len() as u16).to_le_bytes()); // 44
    out.extend_from_slice(&0u16.to_le_bytes());                   // 46 pad
    out.extend_from_slice(&(f.notes.len() as u32).to_le_bytes()); // 48
    out.extend_from_slice(&f.agg_rows.to_le_bytes());             // 52
    debug_assert_eq!(out.len(), HEADER_BYTES);
    out.extend_from_slice(&f.agg_tracks.to_le_bytes());
    out.extend_from_slice(&0u16.to_le_bytes());
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
        out.extend_from_slice(&0u16.to_le_bytes());
        out.extend_from_slice(&n.delay_nanoticks.to_le_bytes());
        out.extend_from_slice(&n.row.to_le_bytes());
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
    let grid_for = |t: usize| LaneGrid::new(if lpb[t] == 0 { vp.lines_per_beat } else { lpb[t] as u32 });

    if out.clip_version != prev_clip_version || out.notes.is_empty() {
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
                    delay_nanoticks: note.delay_nanoticks,
                    row: grid_for(track as usize).row_of_tick(note.t_on) as u32,
                });
            }
        }
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
            // Each lane aggregates on its own grid, so a triplet lane's rows are
            // thirds of a beat and a sextuplet lane's are sixths.
            let g = grid_for(t as usize);
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

fn serve(stream: TcpStream, shm: String, hz: u32, clients: Arc<AtomicU64>) {
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
    let mut viewport = Viewport::default();
    ws.get_mut().set_nonblocking(true).ok();
    let (mut seq, mut sent, mut polls) = (0u64, 0u64, 0u64);
    let started = Instant::now();
    let mut reported = started;

    loop {
        let tick = Instant::now();
        polls += 1;

        // Drain any viewport updates the client sent. Non-blocking, so a quiet
        // client costs one failed read per tick.
        loop {
            match ws.read() {
                Ok(tungstenite::Message::Text(t)) => { parse_viewport(&t, &mut viewport); last_version = u64::MAX; }
                Ok(tungstenite::Message::Close(_)) => return,
                Ok(_) => {}
                Err(_) => break,
            }
        }

        let prev_cv = frame.clip_version;
        if read_frame(&handle, seq, &mut frame, prev_cv, viewport) && frame.version != last_version {
            // Dedup on the engine's own version: the engine publishes at ~86 Hz,
            // we poll faster so we never miss one, and the surplus polls cost a
            // single atomic load each.
            last_version = frame.version;
            seq += 1;
            frame.seq = seq;
            encode(&frame, &mut buf);
            if ws.send(tungstenite::Message::Binary(buf.clone())).is_err() {
                break;
            }
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

    let clients = Arc::new(AtomicU64::new(0));
    for stream in listener.incoming() {
        match stream {
            Ok(s) => {
                let (shm, hz, c) = (args.shm.clone(), args.hz, clients.clone());
                thread::spawn(move || serve(s, shm, hz, c));
            }
            Err(e) => eprintln!("sidecar: accept failed: {e}"),
        }
    }
}
