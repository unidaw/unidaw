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
use daw_bridge::layout::{UiChordCommandPayload, UiCommandPayload, UiCommandType,
                        UiPatcherNodeConfigPayload, UiPatcherPresetCommandPayload};
use daw_bridge::grid::{aggregate_rows, LaneGrid};

/// Wire format, little-endian. The frontend decodes with a DataView.
/// Bump `WIRE_VERSION` here and in `ui-web/src/wire.js` together.
const WIRE_MAGIC: u32 = 0x31_49_4e_55; // "UNI1"
const WIRE_VERSION: u16 = 11;

/// Frame kinds. The channel byte exists from the start so DSP scope feeds can be
/// added additively rather than as a version bump on both sides: per-track scopes
/// are likely, and multiplexing them onto this one socket is two bytes of header
/// versus a protocol change. `feed` identifies which producer within a kind —
/// a track index for scopes, unused for state.
const KIND_STATE: u8 = 0;

/// Header is fixed-size; peaks and notes follow, counted in the header.
const HEADER_BYTES: usize = 56;
/// The full fixed header, matching HEADER_BYTES in ui-web/src/wire.js. Asserted
/// after the last field is written — the 56-byte checkpoint below predates every
/// field added since and stopped catching drift long ago.
const FULL_HEADER_BYTES: usize = 116;
#[allow(dead_code)] // documents the wire layout for ui-web/src/wire.js
const NOTE_BYTES: usize = 40;

/// The client's current viewport. It owns zoom and scroll; we own the
/// projection, because LaneGrid is the authority on tick<->row and reimplementing
/// it in JS would be a second definition of the same truth — and JS division
/// cannot express triplet grids (lines_per_beat = 3) at all.
#[derive(Clone, Copy, Debug)]
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
    // 2048, not 512. The engine's LaneGrid is expressed in lines-per-beat and
    // cannot describe a row coarser than one beat, so a client showing bars per
    // row has to ask for the BEATS its window covers and fold them itself. At
    // 4 bars a row over 62 rows that is 992 beats. 2048 x 16 tracks x 8 bytes is
    // 256 KB in the worst case, against a measured 126 MB/s ceiling.
    if let Some(v) = field("\"rowCount\"") { vp.row_count = (v as u32).min(2048); }
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

/// Bumped whenever a publish thread re-attaches to the segment.
///
/// The command threads hold their OWN EngineHandle and cannot poll for staleness
/// — they are blocked in ws.read(). Without this they keep writing into a ring
/// the restarted engine never reads: the command succeeds, the ack says ok, and
/// nothing happens. Which is the same silent-plausible-failure this whole file
/// keeps having, so it gets a counter rather than a hope.
static SHM_GENERATION: AtomicU64 = AtomicU64::new(0);

struct Args {
    port: u16,
    cmd_port: u16,
    shm: String,
    hz: u32,
    /// Where projects live. The engine resolves names against its own
    /// DAW_PROJECT_DIR; we need the same directory to LIST them, because the
    /// browser cannot read a filesystem and the engine publishes no index.
    projects: String,
}

fn parse_args() -> Args {
    let mut a = Args {
        port: 8174, cmd_port: 8175, shm: default_shm_name(), hz: 120,
        projects: std::env::var("DAW_PROJECT_DIR").unwrap_or_else(|_| "presets/projects".into()),
    };
    let v: Vec<String> = std::env::args().skip(1).collect();
    let mut i = 0;
    while i < v.len() {
        match v[i].as_str() {
            "--port" if i + 1 < v.len() => { a.port = v[i + 1].parse().unwrap_or(a.port); a.cmd_port = a.port + 1; i += 2; }
            "--cmd-port" if i + 1 < v.len() => { a.cmd_port = v[i + 1].parse().unwrap_or(a.cmd_port); i += 2; }
            "--shm" if i + 1 < v.len() => { a.shm = v[i + 1].clone(); i += 2; }
            "--hz" if i + 1 < v.len() => { a.hz = v[i + 1].parse().unwrap_or(a.hz).clamp(1, 1000); i += 2; }
            "--projects" if i + 1 < v.len() => { a.projects = v[i + 1].clone(); i += 2; }
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
    /// Real clip placements from the engine. placement_id, clip_id, track,
    /// flags, start/end tick, name. Loose session placements are excluded
    /// upstream. `flags` bit0 is UI_CLIP_EXTENT_AUDIO — an audio region, which
    /// carries no note events and is drawn as a waveform rather than a lane.
    extents: Vec<(u32, u32, u32, u32, u64, u64, [u8; 32])>,
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
    /// Per-track mixer read-back (SHM v12). Until this existed the mixer drew
    /// what the client last sent, which is wrong after a load, an undo, or any
    /// other surface moving a fader.
    mixer: Vec<(i32, i32, u8)>,
    mixer_version: u32,
    /// The loop region (v15). You could always SET one and never draw it.
    loop_start: u64,
    loop_end: u64,
    /// A load result (v15): the sequence bumps once per LoadProject attempt and
    /// `ok` says whether it took. Before this a malformed project was accepted
    /// with {"ok":true} and silently changed nothing.
    load_seq: u32,
    load_ok: u32,
    /// The harmony timeline: (nanotick, root, scale_id) per key change. The
    /// chrome showed a hardcoded "C major" before this, which was wrong for
    /// three quarters of the maximal project.
    harmony: Vec<(u64, u32, u32)>,
    /// The version the harmony we HOLD was read at. Not `harmony_version`, which
    /// is refreshed from the snapshot earlier in the same function — comparing
    /// against that makes the check always false and the timeline read exactly
    /// once. Same cache-key mistake as the names, caught this time before it
    /// shipped, by writing the comparison and then reading the order.
    harmony_read_at: u32,
    /// -1 is impossible for a u32 version, so this starts "never read".
    harmony_ever_read: bool,
    /// Track names (SHM v13). Four surfaces were labelling lanes "T01".
    names: Vec<String>,
    /// The patcher graph (SHM v14). One global graph today; the region gains
    /// per-device graphs later and this shape does not change.
    patcher_version: u32,
    patcher_device: u32,
    patcher_nodes: Vec<(u32, u8, u8, [i32; 8])>,
    patcher_edges: Vec<(u32, u32, u32, u32, u8)>,
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
    out.extend_from_slice(&f.mixer_version.to_le_bytes());
    out.extend_from_slice(&(f.mixer.len() as u16).to_le_bytes());  // 72
    // 74 was a pad; the harmony count took it rather than being appended after
    // it, which is what a two-byte shift of everything downstream looks like
    // when you get it wrong — names decode empty and every pitch reads 0.
    out.extend_from_slice(&(f.harmony.len() as u16).to_le_bytes());  // 74
    out.extend_from_slice(&(f.names.len() as u16).to_le_bytes());    // 76
    out.extend_from_slice(&f.patcher_version.to_le_bytes());         // 78
    out.extend_from_slice(&f.patcher_device.to_le_bytes());          // 82
    out.extend_from_slice(&(f.patcher_nodes.len() as u16).to_le_bytes());  // 86
    out.extend_from_slice(&(f.patcher_edges.len() as u16).to_le_bytes());  // 88
    out.extend_from_slice(&0u16.to_le_bytes());                      // 90, pad
    out.extend_from_slice(&f.loop_start.to_le_bytes());              // 92
    out.extend_from_slice(&f.loop_end.to_le_bytes());                // 100
    out.extend_from_slice(&f.load_seq.to_le_bytes());                // 108
    out.extend_from_slice(&f.load_ok.to_le_bytes());                 // 112, to 116
    // The WHOLE header, not just the first 56 bytes. The old assertion stopped
    // before every field added since, so a mislaid u16 shifted the entire
    // variable section and nothing here noticed.
    debug_assert_eq!(out.len(), FULL_HEADER_BYTES, "header layout drifted");
    for &(tick, root, scale) in &f.harmony {
        out.extend_from_slice(&tick.to_le_bytes());
        out.extend_from_slice(&root.to_le_bytes());
        out.extend_from_slice(&scale.to_le_bytes());               // 16 bytes each
    }
    for n in &f.names {
        let b = n.as_bytes();
        let take = b.len().min(24);
        out.extend_from_slice(&b[..take]);
        out.extend(std::iter::repeat(0u8).take(24 - take));
    }
    for &(id, ty, has_cfg, cfg) in &f.patcher_nodes {
        out.extend_from_slice(&id.to_le_bytes());
        out.push(ty); out.push(has_cfg); out.push(0); out.push(0);
        for v in cfg { out.extend_from_slice(&v.to_le_bytes()); }   // 40 bytes each
    }
    for &(sn, sp, dn, dp, kind) in &f.patcher_edges {
        out.extend_from_slice(&sn.to_le_bytes());
        out.extend_from_slice(&sp.to_le_bytes());
        out.extend_from_slice(&dn.to_le_bytes());
        out.extend_from_slice(&dp.to_le_bytes());
        out.push(kind); out.push(0); out.push(0); out.push(0);      // 20 bytes each
    }
    for &(gain, pan, flags) in &f.mixer {
        out.extend_from_slice(&gain.to_le_bytes());
        out.extend_from_slice(&pan.to_le_bytes());
        out.push(flags);
        out.push(0); out.push(0); out.push(0);                    // 12 bytes each
    }
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
    // 64 bytes each, matching UiClipExtent. The stride is load-bearing for the
    // aggregates that follow it; widening it without the client is what made
    // extents decode as garbage once before.
    for &(pid, clip_id, track, flags, start, end, name) in &f.extents {
        out.extend_from_slice(&pid.to_le_bytes());        // 0
        out.extend_from_slice(&clip_id.to_le_bytes());    // 4
        out.extend_from_slice(&track.to_le_bytes());      // 8
        out.extend_from_slice(&flags.to_le_bytes());      // 12
        out.extend_from_slice(&start.to_le_bytes());      // 16
        out.extend_from_slice(&end.to_le_bytes());        // 24
        out.extend_from_slice(&name);                     // 32..64
    }
    for &(count, rep, lo, hi) in &f.aggs {
        out.extend_from_slice(&count.to_le_bytes());
        out.push(rep); out.push(lo); out.push(hi); out.push(0);
    }
}

/// Whether the harmony we hold is out of date. Its own version, not the clip
/// one: a key change touches neither notes nor placements, and keying it on
/// clip_version is the mistake that made renames invisible.
fn f_harmony_stale(out: &Frame) -> bool {
    // `harmony_read_at` alone, not `is_empty()` as well: the engine currently
    // publishes an EMPTY timeline (the region is there, event_count is 0), and
    // re-reading an empty one every frame is a poll for something that only
    // changes when the version does.
    out.harmony_read_at != out.harmony_version
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

    // Cheap: read_mixer is a seqlock read of a fixed-size row. Guarded on the
    // engine's own version so an unchanged mixer costs one atomic load.
    // Read EVERY frame, not keyed on clip_version.
    //
    // "Names change only on a project load, so they ride the clip version" was
    // wrong the moment SetTrackName existed: a rename changes a name and touches
    // nothing else, so the engine accepted the command, the ack said ok, and the
    // name never moved. Seventh instance of this project's one bug — content
    // changing while the key the consumer watches stays still — and the first I
    // have written myself since documenting it.
    //
    // There is no name version to key on, so this reads 8x24 bytes per frame and
    // compares. That is cheap here; the browser is the allocation-sensitive side,
    // not this one.
    out.names = h.read_track_names();

    let (ls, le) = h.loop_range();
    out.loop_start = ls;
    out.loop_end = le;
    let (lseq, lok) = h.load_status();
    out.load_seq = lseq;
    out.load_ok = lok;

    // Keyed on the engine's own harmony version, which moves only on a change.
    if !out.harmony_ever_read || f_harmony_stale(out) {
        out.harmony_ever_read = true;
        out.harmony_read_at = out.harmony_version;
        out.harmony.clear();
        for e in h.read_harmony() {
            out.harmony.push((e.nanotick, e.root, e.scale_id));
        }
    }

    let pv = h.patcher_version();
    if pv != out.patcher_version || out.patcher_nodes.is_empty() {
        let view = h.read_patcher();
        out.patcher_version = view.version;
        out.patcher_device = view.device_id;
        out.patcher_nodes.clear();
        for n in &view.nodes {
            out.patcher_nodes.push((n.id, n.node_type, n.has_config, n.config));
        }
        out.patcher_edges.clear();
        for e in &view.edges {
            out.patcher_edges.push((e.src_node, e.src_port, e.dst_node, e.dst_port, e.kind));
        }
    }

    let mv = h.mixer_version();
    if mv != out.mixer_version || out.mixer.is_empty() {
        out.mixer_version = mv;
        out.mixer.clear();
        for m in h.read_mixer() {
            out.mixer.push((m.gain_millibels, m.pan_thousandths, m.flags));
        }
    }

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
        out.extents.push((e.placement_id, e.clip_id, e.track_id, e.flags,
                          e.start_tick, e.end_tick, e.name));
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

/// The projects on disk, newest first, as a JSON array of names.
///
/// Names only — the engine resolves them against its own project directory, and
/// handing the client paths would invite it to send one back.
fn list_projects(dir: &str) -> String {
    let mut items: Vec<(std::time::SystemTime, String)> = Vec::new();
    if let Ok(rd) = std::fs::read_dir(dir) {
        for entry in rd.flatten() {
            let name = entry.file_name().to_string_lossy().to_string();
            let Some(stem) = name.strip_suffix(".uniproj.json") else { continue };
            let when = entry.metadata().and_then(|m| m.modified()).unwrap_or(std::time::UNIX_EPOCH);
            items.push((when, stem.to_string()));
        }
    }
    items.sort_by(|a, b| b.0.cmp(&a.0));
    let mut out = String::from("{\"ok\":true,\"projects\":[");
    for (i, (_, name)) in items.iter().enumerate() {
        if i > 0 { out.push(','); }
        out.push('"');
        // Names come from the filesystem; a quote or backslash in one would
        // produce a malformed reply the client silently fails to parse.
        for c in name.chars() {
            match c { '"' | '\\' => { out.push('\\'); out.push(c); } _ => out.push(c) }
        }
        out.push('"');
    }
    out.push_str("]}");
    out
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
    } else if body.contains("\"rename\"") {
        UiCommandType::SetTrackName
    } else {
        return None;                       // not a named command at all
    };
    let name = parse_str(body, "\"name\"").unwrap_or("default");
    // A track name is not a filename, so the path rules do not apply to it —
    // only the length the payload can carry.
    if matches!(ty, UiCommandType::SetTrackName) {
        if name.is_empty() || name.len() > 28 { return Some(Err("bad track name")); }
        let mut p = UiPatcherPresetCommandPayload::named(ty, name);
        p.track_id = parse_num(body, "\"track\"").unwrap_or(0).clamp(0, 63) as u32;
        return Some(Ok(p.as_command()));
    }
    if !safe_name(name) {
        // Distinct from "unknown command": the command WAS understood and was
        // refused. Collapsing the two would report a rejected name as a typo.
        return Some(Err("bad project name"));
    }
    Some(Ok(UiPatcherPresetCommandPayload::named(ty, name).as_command()))
}

/// Chords ride in their OWN payload, not UiCommandPayload — the engine dispatches
/// on entry size, so a chord sent in the wrong shape is silently ignored rather
/// than rejected.
///
/// A chord here is (scale degree, quality, inversion) resolved against the
/// harmony timeline, NOT absolute semitones: `degree 3, quality seventh` means
/// the seventh built on the third degree of whatever key is in force, which is
/// what makes a chord track survive a key change.
fn build_chord(body: &str) -> Option<UiChordCommandPayload> {
    if !body.contains("\"chord\"") { return None; }
    let n = |k: &str, d: i64| parse_num(body, k).unwrap_or(d);
    let tick = n("\"tick\"", 0).max(0) as u64;
    let dur = n("\"dur\"", 0).max(0) as u64;
    Some(UiChordCommandPayload {
        command_type: UiCommandType::WriteChord as u16,
        // Low byte of flags is the column.
        flags: (n("\"column\"", 0).clamp(0, 255) as u16) & 0xff,
        track_id: n("\"track\"", 0).max(0) as u32,
        base_version: n("\"base\"", 0).max(0) as u32,
        nanotick_lo: tick as u32,
        nanotick_hi: (tick >> 32) as u32,
        // 0 means "until the next event in this column", which is the tracker's
        // own convention for a held chord.
        duration_lo: dur as u32,
        duration_hi: (dur >> 32) as u32,
        degree: n("\"degree\"", 0).clamp(0, 65535) as u16,
        quality: n("\"quality\"", 1).clamp(0, 255) as u8,
        inversion: n("\"inv\"", 0).clamp(0, 255) as u8,
        base_octave: n("\"oct\"", 4).clamp(0, 9) as u8,
        humanize_timing: n("\"ht\"", 0).clamp(0, 255) as u8,
        humanize_velocity: n("\"hv\"", 0).clamp(0, 255) as u8,
        reserved: 0,
        spread_nanoticks: n("\"spread\"", 0).clamp(0, u32::MAX as i64) as u32,
    })
}

/// A patcher node's config, packed into the 16 bytes the engine expects.
///
/// The read side gives eight i32 per node; the write side is an explicit
/// little-endian layout that DIFFERS per node type, and is not a C++ struct
/// memcpy — backend replaced that precisely because it coupled the wire to
/// padding and truncated Euclidean. So the client sends the same eight values it
/// read and the packing happens here, once, next to the layout it implements.
///
///   Euclidean(1)    steps u16@0, hits u16@2, offset u16@4, degree u8@6,
///                   octaveOffset i8@7, velocity u8@8, baseOctave u8@9,
///                   pad u16@10, durationTicks u32@12
///   Lfo(4)          freqMilliHz i32@0, depthMilli i32@4, biasMilli i32@8,
///                   phaseMilli i32@12
///   RandomDegree(5) degree u8@0, velocity u8@1, pad u16@2, durationTicks u32@4
fn build_patcher_config(body: &str) -> Option<Result<UiPatcherNodeConfigPayload, &'static str>> {
    if !body.contains("\"patchcfg\"") { return None; }
    let n = |k: &str| parse_num(body, k).unwrap_or(0);
    let node_type = n("\"nodeType\"") as u32;
    let mut cfg = [0u8; 16];
    // The eight values as read from UiPatcherNode.config, in that order.
    let c: Vec<i64> = (0..8).map(|i| parse_num(body, &format!("\"c{i}\"")).unwrap_or(0)).collect();
    match node_type {
        1 => {
            cfg[0..2].copy_from_slice(&(c[0].clamp(0, 65535) as u16).to_le_bytes());
            cfg[2..4].copy_from_slice(&(c[1].clamp(0, 65535) as u16).to_le_bytes());
            cfg[4..6].copy_from_slice(&(c[2].clamp(0, 65535) as u16).to_le_bytes());
            cfg[6] = c[3].clamp(0, 255) as u8;
            cfg[7] = (c[4].clamp(-128, 127) as i8) as u8;
            cfg[8] = c[5].clamp(0, 255) as u8;
            cfg[9] = c[6].clamp(0, 255) as u8;
            cfg[12..16].copy_from_slice(&(c[7].clamp(0, u32::MAX as i64) as u32).to_le_bytes());
        }
        4 => {
            for i in 0..4 {
                let v = c[i].clamp(i32::MIN as i64, i32::MAX as i64) as i32;
                cfg[i * 4..i * 4 + 4].copy_from_slice(&v.to_le_bytes());
            }
        }
        5 => {
            cfg[0] = c[0].clamp(0, 255) as u8;
            cfg[1] = c[1].clamp(0, 255) as u8;
            cfg[4..8].copy_from_slice(&(c[2].clamp(0, u32::MAX as i64) as u32).to_le_bytes());
        }
        // A type with no layout is refused rather than sent as zeros, which the
        // engine would apply.
        _ => return Some(Err("no config layout for that node type")),
    }
    Some(Ok(UiPatcherNodeConfigPayload {
        command_type: UiCommandType::SetPatcherNodeConfig as u16,
        flags: 0,
        track_id: n("\"track\"").max(0) as u32,
        base_version: n("\"base\"").max(0) as u32,
        node_id: n("\"node\"").max(0) as u32,
        config_type: node_type,
        config: cfg,
        reserved: [0; 4],
    }))
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
    } else if body.contains("\"stop\"") {
        // A real Stop now: halt AND rewind. TogglePlay is pause-in-place.
        p.command_type = UiCommandType::Stop as u16;
    } else if body.contains("\"seek\"") {
        // Target nanotick rides in note_nanotick_lo/hi, already set above from
        // "tick". Seeking while playing is audible about an ahead-buffer later
        // (~100ms) but the published playhead moves on the next block, so the
        // UI is honest immediately.
        p.command_type = UiCommandType::SetPosition as u16;
    } else if body.contains("\"note\"") {
        let dur = parse_num(body, "\"dur\"").unwrap_or(960_000).max(1) as u64;
        p.command_type = UiCommandType::WriteNote as u16;
        p.note_pitch = parse_num(body, "\"pitch\"").unwrap_or(60).clamp(0, 127) as u32;
        p.value0 = parse_num(body, "\"vel\"").unwrap_or(100).clamp(0, 127) as u32;
        p.note_duration_lo = dur as u32;
        p.note_duration_hi = (dur >> 32) as u32;
    } else if body.contains("\"delete\"") {
        p.command_type = UiCommandType::DeleteNote as u16;
    } else if body.contains("\"mixer\"") {
        // Gain is signed millibels and pan signed thousandths, but the payload
        // fields are u32 — bit-cast rather than clamp, since the engine reads
        // them back as i32 and a saturating cast would silently mean full left.
        p.command_type = UiCommandType::SetTrackMixer as u16;
        p.value0 = (parse_num(body, "\"gain\"").unwrap_or(0).clamp(-9600, 1200) as i32) as u32;
        p.plugin_index = (parse_num(body, "\"pan\"").unwrap_or(0).clamp(-1000, 1000) as i32) as u32;
        p.flags = parse_num(body, "\"flags\"").unwrap_or(0).clamp(0, 3) as u16;
    } else if body.contains("\"undo\"") {
        p.command_type = UiCommandType::Undo as u16;
    } else if body.contains("\"redo\"") {
        p.command_type = UiCommandType::Redo as u16;
    } else {
        return Err("unknown command");
    }
    Ok(p)
}

fn serve_commands(listener: TcpListener, shm: String, viewport: SharedViewport, projects: String) {
    for stream in listener.incoming().flatten() {
        let shm = shm.clone();
        let viewport = viewport.clone();
        let projects = projects.clone();
        thread::spawn(move || {
            let mut ws = match tungstenite::accept(stream) { Ok(w) => w, Err(_) => return };
            // Attached HERE, on the thread that will use it: EngineHandle is not
            // Send, so it cannot be created elsewhere and moved in.
            let mut generation = SHM_GENERATION.load(Ordering::Acquire);
            let mut handle = match EngineHandle::attach(&shm, true) {
                Ok(h) => h,
                Err(e) => { let _ = ws.send(tungstenite::Message::Text(
                    format!("{{\"error\":\"attach failed: {e}\"}}"))); return; }
            };
            eprintln!("sidecar: command client connected");
            loop {
                match ws.read() {
                    Ok(tungstenite::Message::Text(t)) => {
                        // The engine may have restarted while we were blocked in
                        // read(); re-attach before acting on anything.
                        let g = SHM_GENERATION.load(Ordering::Acquire);
                        if g != generation {
                            match EngineHandle::attach(&shm, true) {
                                Ok(h) => { handle = h; generation = g;
                                           eprintln!("sidecar: command channel re-attached"); }
                                Err(e) => {
                                    let _ = ws.send(tungstenite::Message::Text(
                                        format!("{{\"error\":\"re-attach failed: {}\"}}",
                                                e.replace('"', "'"))));
                                    continue;
                                }
                            }
                        }
                        // Viewport updates are not engine commands — they change
                        // what we project, not what the song contains, so they
                        // never touch the command ring.
                        // Not an engine command: a directory listing. Answered
                        // here because the browser cannot read a filesystem and
                        // the engine publishes no project index.
                        if t.contains("\"list\"") {
                            let reply = list_projects(&projects);
                            if ws.send(tungstenite::Message::Text(reply)).is_err() { break; }
                            continue;
                        }
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
                        // A batch: one frame, several edits, applied in order.
                        //
                        // Each edit must be composed against the version the
                        // PREVIOUS one produced. The engine arbitrates by
                        // base_version, so a client that stamps every op in a
                        // transpose with the same base gets the first applied and
                        // the rest rejected — which looks exactly like a partly
                        // working transpose, and is how this was found.
                        if let Some(rest) = t.strip_prefix("BATCH\n") {
                            let (mut ok, mut failed) = (0u32, 0u32);
                            for line in rest.lines().filter(|l| !l.trim().is_empty()) {
                                let base = handle.clip_version();
                                let sent = if let Some(c) = build_chord(line) {
                                    let mut c = c;
                                    c.base_version = base;
                                    handle.send_chord_command(c).is_ok()
                                } else {
                                    match build_command(line) {
                                        Ok(mut p) => { p.base_version = base;
                                                       handle.send_command(p).is_ok() }
                                        Err(_) => false,
                                    }
                                };
                                if !sent { failed += 1; continue; }
                                // Wait for the engine to actually apply it. Without
                                // this the next op re-reads the same version and we
                                // are back to the race we are fixing.
                                if handle.wait_for_clip_version(
                                    base, base.wrapping_add(1),
                                    Duration::from_millis(250)) { ok += 1; } else { failed += 1; }
                            }
                            let reply = format!("{{\"ok\":true,\"applied\":{ok},\"failed\":{failed}}}");
                            if ws.send(tungstenite::Message::Text(reply)).is_err() { break; }
                            continue;
                        }

                        // Patcher config has its own payload too.
                        if let Some(r) = build_patcher_config(&t) {
                            let reply = match r {
                                Err(why) => format!("{{\"error\":\"{why}\"}}"),
                                Ok(p) => match handle.send_patcher_config(p) {
                                    Ok(()) => format!("{{\"ok\":true,\"type\":{},\"node\":{}}}",
                                                      p.command_type, p.node_id),
                                    Err(e) => format!("{{\"error\":\"{}\"}}", e.replace('"', "'")),
                                },
                            };
                            if ws.send(tungstenite::Message::Text(reply)).is_err() { break; }
                            continue;
                        }

                        // Chords take a different payload, so they cannot go
                        // through build_command's return type.
                        if let Some(c) = build_chord(&t) {
                            let reply = match handle.send_chord_command(c) {
                                Ok(()) => format!("{{\"ok\":true,\"type\":{},\"degree\":{}}}",
                                                  c.command_type, c.degree),
                                Err(e) => format!("{{\"error\":\"{}\"}}", e.replace('"', "'")),
                            };
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

    let mut handle = match EngineHandle::attach(&shm, false) {
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
    const REATTACH_EVERY: Duration = Duration::from_secs(2);
    let mut last_reattach = Instant::now();
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

        // A restarted engine maps a NEW segment, so the handle we are holding
        // points at memory nobody writes any more: the UI stays "engine gone"
        // for ever and the only cure is reloading the page. Re-attach while
        // stale, at a gentle interval — attaching is a mmap, not free, and a
        // genuinely absent engine would otherwise be a busy loop.
        if stale && now.duration_since(last_reattach) > REATTACH_EVERY {
            last_reattach = now;
            if let Ok(h) = EngineHandle::attach(&shm, false) {
                handle = h;
                // Force the next read to look new: the fresh engine starts its
                // own version counter, which can be BELOW the one we remember.
                last_seen_version = u64::MAX;
                last_version = u64::MAX;
                frame.clip_version = u32::MAX;
                SHM_GENERATION.fetch_add(1, Ordering::Release);
                eprintln!("sidecar: re-attached to {shm}");
            }
        }

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
    // Encode an empty frame and check the header came out the length the client
    // expects. debug_assert catches this in development and is compiled out of
    // the build we actually ship, which is the one where a two-byte drift
    // silently reinterprets every field after it. Refuse to start instead.
    {
        let mut probe = Vec::new();
        encode(&Frame::default(), &mut probe);
        if probe.len() != FULL_HEADER_BYTES {
            eprintln!("sidecar: header is {} bytes, wire.js expects {FULL_HEADER_BYTES} — \
                       the two have drifted and every field after the mismatch would be \
                       misread. Fix encode() and HEADER_BYTES together.", probe.len());
            std::process::exit(2);
        }
    }

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
            let projects = args.projects.clone();
            thread::spawn(move || serve_commands(l, shm, vp, projects));
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

#[cfg(test)]
mod tests {
    use super::*;

    /// The viewport is packed into one u64 so the publish loop reads it with a
    /// single relaxed load. Bit-twiddling with no test is bit-twiddling that is
    /// wrong, and a wrong viewport here is a wrong row projection everywhere.
    #[test]
    fn viewport_round_trips_through_the_packing() {
        for vp in [
            Viewport { lines_per_beat: 1, first_row: 0, row_count: 0 },
            Viewport { lines_per_beat: 4, first_row: 62, row_count: 62 },
            Viewport { lines_per_beat: 12, first_row: 99_999, row_count: 512 },
            // The widest each field is allowed to be: lpb clamps to 64,
            // row_count to 512, and first_row gets 40 bits.
            Viewport { lines_per_beat: 64, first_row: (1 << 40) - 1, row_count: 512 },
        ] {
            let s = SharedViewport::new(vp);
            let got = s.load();
            assert_eq!(got.lines_per_beat, vp.lines_per_beat, "lpb for {vp:?}");
            assert_eq!(got.first_row, vp.first_row, "first_row for {vp:?}");
            assert_eq!(got.row_count, vp.row_count, "row_count for {vp:?}");
        }
    }

    /// Fields must not bleed into each other. A first_row that overflowed into
    /// lines_per_beat would reproject every note on screen.
    #[test]
    fn viewport_fields_do_not_bleed() {
        let s = SharedViewport::new(Viewport { lines_per_beat: 12, first_row: 0, row_count: 0 });
        s.store(Viewport { lines_per_beat: 12, first_row: 0xff_ffff_ffff, row_count: 511 });
        let got = s.load();
        assert_eq!(got.lines_per_beat, 12);
        assert_eq!(got.row_count, 511);
    }

    #[test]
    fn parse_viewport_reads_the_clients_json() {
        let mut vp = Viewport::default();
        parse_viewport(r#"{"linesPerBeat":12,"firstRow":480,"rowCount":62}"#, &mut vp);
        assert_eq!((vp.lines_per_beat, vp.first_row, vp.row_count), (12, 480, 62));
    }

    #[test]
    fn parse_viewport_clamps_rather_than_trusting() {
        let mut vp = Viewport::default();
        parse_viewport(r#"{"linesPerBeat":9999,"firstRow":1,"rowCount":99999}"#, &mut vp);
        assert_eq!(vp.lines_per_beat, 64, "lpb clamped");
        // 2048, not 512: a client showing bars per row asks for the BEATS its
        // window covers, and 62 rows at 4 bars each is 992 of them.
        assert_eq!(vp.row_count, 2048, "row_count clamped");
    }

    /// A project name becomes `<dir>/<name>.uniproj.json` on the engine side.
    /// This is the boundary that stops it being anything else.
    #[test]
    fn safe_name_refuses_anything_that_could_leave_the_directory() {
        for good in ["maximal", "webtest", "take-2", "a.b_c", "x"] {
            assert!(safe_name(good), "{good} should be allowed");
        }
        for bad in ["", "..", ".", "../evil", "a/b", "a\\b", "with\0nul",
                    "0123456789012345678901234567890"] {
            assert!(!safe_name(bad), "{bad:?} should be refused");
        }
    }

    #[test]
    fn parse_str_reads_a_json_string_field() {
        assert_eq!(parse_str(r#"{"type":"load","name":"maximal"}"#, "\"name\""), Some("maximal"));
        assert_eq!(parse_str(r#"{"name":""}"#, "\"name\""), Some(""));
        assert_eq!(parse_str(r#"{"type":"load"}"#, "\"name\""), None);
    }

    #[test]
    fn build_command_maps_the_verbs() {
        let ty = |body: &str| build_command(body).ok().map(|p| p.command_type);
        assert_eq!(ty(r#"{"type":"play"}"#), Some(UiCommandType::TogglePlay as u16));
        assert_eq!(ty(r#"{"type":"stop"}"#), Some(UiCommandType::Stop as u16));
        assert_eq!(ty(r#"{"type":"seek","tick":960000}"#), Some(UiCommandType::SetPosition as u16));
        assert_eq!(ty(r#"{"type":"delete","track":2,"tick":0}"#), Some(UiCommandType::DeleteNote as u16));
        assert_eq!(ty(r#"{"type":"undo"}"#), Some(UiCommandType::Undo as u16));
        assert_eq!(ty(r#"{"type":"nonsense"}"#), None);
    }

    /// A rejected name has to be distinguishable from a verb we do not know.
    /// Collapsing the two reported a path-escape attempt as a typo.
    #[test]
    fn a_refused_name_is_not_an_unknown_command() {
        // UiCommandPayload has no PartialEq (it is another crate's POD), so
        // compare the error rather than the whole Result.
        assert_eq!(build_command(r#"{"type":"load","name":"../etc"}"#).err(),
                   Some("bad project name"));
        assert_eq!(build_command(r#"{"type":"rename","track":0,"name":""}"#).err(),
                   Some("bad track name"));
        assert_eq!(build_command(r#"{"type":"nope"}"#).err(), Some("unknown command"));
    }

    #[test]
    fn note_carries_its_tick_duration_and_velocity() {
        let p = build_command(r#"{"type":"note","track":3,"pitch":64,"tick":4294967296,"dur":480000,"vel":90}"#)
            .expect("should build");
        assert_eq!(p.command_type, UiCommandType::WriteNote as u16);
        assert_eq!(p.track_id, 3);
        assert_eq!(p.note_pitch, 64);
        assert_eq!(p.value0, 90);
        // 2^32 exactly: the split across lo/hi is the part that can silently
        // truncate, so it is the part worth asserting.
        assert_eq!(p.note_nanotick_lo, 0);
        assert_eq!(p.note_nanotick_hi, 1);
        assert_eq!(p.note_duration_lo, 480_000);
    }

    #[test]
    fn mixer_gain_and_pan_survive_being_negative() {
        let p = build_command(r#"{"type":"mixer","track":1,"gain":-600,"pan":-500,"flags":1}"#)
            .expect("should build");
        assert_eq!(p.command_type, UiCommandType::SetTrackMixer as u16);
        // Bit-cast, not saturated: the engine reads these back as i32, and a
        // saturating cast would silently mean full left at minimum gain.
        assert_eq!(p.value0 as i32, -600);
        assert_eq!(p.plugin_index as i32, -500);
        assert_eq!(p.flags, 1);
    }

    #[test]
    fn chord_is_its_own_payload_with_a_zero_based_degree() {
        let c = build_chord(r#"{"type":"chord","track":2,"tick":960000,"degree":2,"quality":2,"spread":80,"ht":20,"hv":20}"#)
            .expect("should build");
        assert_eq!(c.command_type, UiCommandType::WriteChord as u16);
        assert_eq!(c.track_id, 2);
        assert_eq!(c.degree, 2);
        assert_eq!(c.quality, 2);
        assert_eq!(c.spread_nanoticks, 80);
        assert_eq!(c.humanize_timing, 20);
        assert!(build_chord(r#"{"type":"note","pitch":60}"#).is_none(), "not a chord");
    }

    /// Byte-level layout, which is where this project has been bitten twice.
    /// The values are the ones the READ side reports, so a round trip through
    /// the UI has to come back the same.
    #[test]
    fn euclidean_config_packs_to_the_engine_layout() {
        let p = build_patcher_config(
            r#"{"type":"patchcfg","node":0,"nodeType":1,"c0":16,"c1":5,"c2":3,"c3":2,"c4":-1,"c5":100,"c6":4,"c7":480000}"#)
            .expect("recognised").expect("packed");
        assert_eq!(p.config_type, 1);
        assert_eq!(u16::from_le_bytes([p.config[0], p.config[1]]), 16, "steps");
        assert_eq!(u16::from_le_bytes([p.config[2], p.config[3]]), 5, "hits");
        assert_eq!(u16::from_le_bytes([p.config[4], p.config[5]]), 3, "offset");
        assert_eq!(p.config[6], 2, "degree");
        assert_eq!(p.config[7] as i8, -1, "octaveOffset is SIGNED");
        assert_eq!(p.config[8], 100, "velocity");
        assert_eq!(p.config[9], 4, "baseOctave");
        assert_eq!(u32::from_le_bytes([p.config[12], p.config[13], p.config[14], p.config[15]]),
                   480_000, "durationTicks at 12, after the pad");
    }

    #[test]
    fn lfo_config_is_four_i32_and_keeps_its_sign() {
        let p = build_patcher_config(
            r#"{"type":"patchcfg","node":2,"nodeType":4,"c0":2500,"c1":750,"c2":-250,"c3":0}"#)
            .expect("recognised").expect("packed");
        let at = |i: usize| i32::from_le_bytes([p.config[i], p.config[i+1], p.config[i+2], p.config[i+3]]);
        assert_eq!(at(0), 2500, "freq milliHz");
        assert_eq!(at(4), 750, "depth milli");
        assert_eq!(at(8), -250, "bias milli, negative");
        assert_eq!(at(12), 0, "phase milli");
    }

    #[test]
    fn random_degree_puts_duration_at_four_not_two() {
        let p = build_patcher_config(
            r#"{"type":"patchcfg","node":1,"nodeType":5,"c0":8,"c1":100,"c2":960000}"#)
            .expect("recognised").expect("packed");
        assert_eq!(p.config[0], 8);
        assert_eq!(p.config[1], 100);
        // Bytes 2-3 are a pad; duration starts at 4. Writing it at 2 would be a
        // plausible guess and silently wrong.
        assert_eq!(u16::from_le_bytes([p.config[2], p.config[3]]), 0, "pad stays zero");
        assert_eq!(u32::from_le_bytes([p.config[4], p.config[5], p.config[6], p.config[7]]),
                   960_000);
    }

    #[test]
    fn an_unknown_node_type_is_refused_rather_than_zeroed() {
        // Sending zeros for a type we do not know how to pack would be applied
        // by the engine as a real configuration.
        assert_eq!(build_patcher_config(r#"{"type":"patchcfg","nodeType":99}"#).unwrap().err(),
                   Some("no config layout for that node type"));
        assert!(build_patcher_config(r#"{"type":"note","pitch":60}"#).is_none());
    }

    #[test]
    fn list_projects_returns_names_not_paths() {
        let dir = std::env::temp_dir().join("uni-sidecar-test-projects");
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(dir.join("alpha.uniproj.json"), "{}").unwrap();
        std::fs::write(dir.join("beta.uniproj.json"), "{}").unwrap();
        std::fs::write(dir.join("notaproject.txt"), "x").unwrap();
        let out = list_projects(dir.to_str().unwrap());
        assert!(out.contains("\"alpha\""), "{out}");
        assert!(out.contains("\"beta\""), "{out}");
        assert!(!out.contains("notaproject"), "non-projects excluded: {out}");
        assert!(!out.contains(".uniproj"), "names, not filenames: {out}");
        assert!(!out.contains(dir.to_str().unwrap()), "never a path: {out}");
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn list_projects_survives_a_missing_directory() {
        assert_eq!(list_projects("/nope/definitely/not/here"), "{\"ok\":true,\"projects\":[]}");
    }
}
