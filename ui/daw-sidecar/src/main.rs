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
use std::sync::atomic::{AtomicU32, AtomicU64, Ordering};
use std::collections::VecDeque;
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

use daw_bridge::control::{default_shm_name, EngineHandle};
use daw_bridge::layout::{EventEntry, UiChainCommandPayload, UiChordCommandPayload,
                        UiCommandPayload, UiCommandType,
                        UiDiffType, UiPatcherGraphCommandPayload, UiPatcherNodeConfigPayload,
                        UiPatcherPresetCommandPayload, UiSetParamPayload,
                        UiWaveformRequestPayload, K_UI_WAVEFORM_SLOTS, K_CHAIN_DEVICE_ID_AUTO,
                        K_CHAIN_TRACK_ALL, K_HOST_SLOT_DIRECT};
use daw_bridge::grid::{aggregate_rows, LaneGrid};

/// Wire format, little-endian. The frontend decodes with a DataView.
/// Bump `WIRE_VERSION` here and in `ui-web/src/wire.js` together.
const WIRE_MAGIC: u32 = 0x31_49_4e_55; // "UNI1"
const WIRE_VERSION: u16 = 12;

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
const FULL_HEADER_BYTES: usize = 124;
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

/// How many clients have EVER connected.
///
/// The live count is not enough to decide "nobody came back". A page reload opens
/// the new socket before the old one's handler has finished unwinding, so the
/// decrement can land AFTER the increment and the count reads zero while a client
/// is sitting there connected — which quit the engine on every refresh. A
/// monotonic counter cannot be fooled that way: if it moved, somebody arrived,
/// whatever the live count happens to say at the instant we look.
static CONNECTS: AtomicU64 = AtomicU64::new(0);

/// Whether the engine should outlive its last client.
///
/// Off by default, because a user thinks the window IS the application. On for
/// test runs, which open and close a browser dozens of times and would otherwise
/// take the engine down with the first one — the behaviour is correct for a
/// person and hostile to a harness.
static KEEP_ENGINE: AtomicU64 = AtomicU64::new(0);

struct Args {
    port: u16,
    cmd_port: u16,
    shm: String,
    hz: u32,
    /// Where projects live. The engine resolves names against its own
    /// DAW_PROJECT_DIR; we need the same directory to LIST them, because the
    /// browser cannot read a filesystem and the engine publishes no index.
    projects: String,
    /// The engine's plugin scan, which it writes beside its own binary. Same
    /// reasoning as `projects`: the browser cannot read it and the engine does
    /// not publish it, but it is already on disk.
    plugin_cache: String,
}

fn parse_args() -> Args {
    let mut a = Args {
        port: 8174, cmd_port: 8175, shm: default_shm_name(), hz: 120,
        projects: std::env::var("DAW_PROJECT_DIR").unwrap_or_else(|_| "presets/projects".into()),
        // Relative to the sidecar's cwd, which webstack.sh sets to <repo>/ui —
        // so the default finds the engine's build directory beside it. The flag
        // is what a stack running an engine from somewhere else uses.
        plugin_cache: std::env::var("DAW_PLUGIN_CACHE")
            .unwrap_or_else(|_| "../build/plugin_cache.json".into()),
    };
    let v: Vec<String> = std::env::args().skip(1).collect();
    let mut i = 0;
    while i < v.len() {
        match v[i].as_str() {
            "--port" if i + 1 < v.len() => { a.port = v[i + 1].parse().unwrap_or(a.port); a.cmd_port = a.port + 1; i += 2; }
            "--cmd-port" if i + 1 < v.len() => { a.cmd_port = v[i + 1].parse().unwrap_or(a.cmd_port); i += 2; }
            "--keep-engine" => { KEEP_ENGINE.store(1, Ordering::Relaxed); i += 1; }
            "--shm" if i + 1 < v.len() => { a.shm = v[i + 1].clone(); i += 2; }
            "--hz" if i + 1 < v.len() => { a.hz = v[i + 1].parse().unwrap_or(a.hz).clamp(1, 1000); i += 2; }
            "--projects" if i + 1 < v.len() => { a.projects = v[i + 1].clone(); i += 2; }
            "--plugin-cache" if i + 1 < v.len() => { a.plugin_cache = v[i + 1].clone(); i += 2; }
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
    /// The tempo AT THE PLAYHEAD, in milli-BPM (120000 = 120), and how many
    /// points the project's tempo map has (1 = constant).
    ///
    /// Milli-BPM rather than a float on purpose: the chrome guards its readout on
    /// the value having changed, and a float that jitters in its last digit
    /// defeats every such guard — it would rebuild the string sixty times a
    /// second to print the same number. An integer compares exactly.
    ///
    /// The point count is not decoration either: it is what lets the chrome say
    /// "128" when the song is 128 throughout and "128"-at-a-position when it is
    /// not, which are different claims.
    tempo_milli_bpm: u32,
    tempo_point_count: u32,
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
    out.extend_from_slice(&f.load_ok.to_le_bytes());                 // 112
    out.extend_from_slice(&f.tempo_milli_bpm.to_le_bytes());         // 116
    out.extend_from_slice(&f.tempo_point_count.to_le_bytes());       // 120, to 124
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
    // Read inside the same seqlock frame as the playhead, engine-side, so the
    // tempo we forward is the tempo AT the position we forward — not the tempo a
    // block later, which is a different number the moment a song has a change in
    // it.
    out.tempo_milli_bpm = snap.ui_tempo_milli_bpm;
    out.tempo_point_count = snap.ui_tempo_point_count;

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
/**
 * Is this text a single, complete JSON object?
 *
 * The plugin cache is passed through to the client verbatim rather than parsed
 * and rebuilt — it is already JSON, the browser already has a parser, and
 * re-encoding 52 records by hand is 52 chances to mangle a plugin name that
 * contains a quote. But a value spliced into a message must be well formed, or
 * the client loses the WHOLE message to a parse error and cannot tell why.
 *
 * The realistic failure is truncation: the engine rewrites this file after a
 * scan, and a read that lands mid-write returns half a document. Balancing
 * braces and brackets while respecting strings and escapes catches exactly that,
 * which is why it is worth twenty lines rather than a dependency.
 */
fn is_complete_json_object(t: &str) -> bool {
    let t = t.trim();
    if !t.starts_with('{') || !t.ends_with('}') { return false; }
    let (mut depth, mut in_str, mut esc) = (0i32, false, false);
    for c in t.chars() {
        if esc { esc = false; continue; }
        if in_str {
            match c { '\\' => esc = true, '"' => in_str = false, _ => {} }
            continue;
        }
        match c {
            '"' => in_str = true,
            '{' | '[' => depth += 1,
            '}' | ']' => { depth -= 1; if depth < 0 { return false; } }
            _ => {}
        }
    }
    depth == 0 && !in_str
}

/**
 * The plugin catalogue, as the engine's scanner left it.
 *
 * The browser rail had eight categories and content in one of them, so there was
 * no way to see — let alone choose — an installed plugin. The engine already
 * scans, and already writes the answer to plugin_cache.json beside its binary:
 * name, vendor, format, is_instrument, uid16, path, and an ok/error per entry.
 * Nothing here needs the engine to publish anything new; it needs somebody to
 * read the file the engine already wrote.
 *
 * Served from the sidecar for the same reason the project list is: a browser
 * cannot read a filesystem.
 *
 * The `error` entries are forwarded rather than filtered. A plugin you own and
 * cannot see is worse than one you can see and cannot use, and "why is Zebra not
 * in the list" is a question the list itself should answer.
 */
fn list_plugins(path: &str) -> String {
    let text = match std::fs::read_to_string(path) {
        Ok(t) => t,
        Err(e) => return format!(
            "{{\"error\":\"no plugin catalogue at {} - the engine writes it when it scans ({})\"}}",
            path.replace('"', "'"), e.to_string().replace('"', "'")),
    };
    if !is_complete_json_object(&text) {
        return format!(
            "{{\"error\":\"the plugin catalogue at {} is not a complete JSON object - \
              a scan may be in progress; try again\"}}",
            path.replace('"', "'"));
    }
    format!("{{\"ok\":true,\"pluginCache\":{text}}}")
}

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

/// `key`'s value when it is genuinely a JSON string, and None when it is a
/// number or absent.
///
/// `parse_str` takes the next quoted run ANYWHERE after the key, which is right
/// where every caller writes a string and wrong where a field may be either: on
/// `{"kind":3,"slot":1}` it returns `slot`. This one insists on the colon and the
/// opening quote, so a numeric value reads as "not a string" rather than as the
/// name of the field after it.
fn parse_str_value<'a>(txt: &'a str, key: &str) -> Option<&'a str> {
    let i = txt.find(key)? + key.len();
    let rest = txt[i..].trim_start().strip_prefix(':')?.trim_start();
    let rest = rest.strip_prefix('"')?;
    let end = rest.find('"')?;
    Some(&rest[..end])
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

/// The engine's port table, per node type: (port id, kind, is_output).
///
/// Kinds are PatcherPortKind: Event 0, Audio 1, Control 2. Port ids are the
/// engine's constants (event in 0, event out 1, control in 2, control out 3,
/// audio in 4, audio out 5).
///
/// This lives here rather than in the page because it is engine data, and one
/// copy of engine data as close to the engine as the wire allows beats two
/// copies drifting apart. The engine validates again regardless — this table
/// exists so the UI can offer a connection that will be accepted, not so it can
/// skip the check.
fn ports_for(node_type: u32) -> &'static [(u32, u32, bool)] {
    const EVENT: u32 = 0;
    const AUDIO: u32 = 1;
    const CONTROL: u32 = 2;
    match node_type {
        // RustKernel
        0 => &[(0, EVENT, false), (1, EVENT, true), (2, CONTROL, false), (3, CONTROL, true)],
        // Euclidean: a source of events and nothing else.
        1 => &[(1, EVENT, true)],
        // Passthrough
        2 => &[(0, EVENT, false), (1, EVENT, true)],
        // AudioPassthrough
        3 => &[(4, AUDIO, false), (5, AUDIO, true)],
        // Lfo: control out only.
        4 => &[(3, CONTROL, true)],
        // RandomDegree
        5 => &[(0, EVENT, false), (1, EVENT, true)],
        // EventOut: a sink.
        6 => &[(0, EVENT, false)],
        _ => &[],
    }
}

/// The one connection that can exist between two node types, or why not.
///
/// Almost every pair has exactly one answer, so the UI should not make anyone
/// type port numbers. Kernel-to-kernel is the exception — it could be events or
/// control — and that is refused with a message rather than guessed, unless the
/// caller says which kind.
fn resolve_link(
    src_type: u32,
    dst_type: u32,
    want_kind: Option<u32>,
) -> Result<(u32, u32, u32), &'static str> {
    let mut found: Option<(u32, u32, u32)> = None;
    let mut count = 0;
    for &(sp, sk, s_out) in ports_for(src_type) {
        if !s_out { continue; }
        for &(dp, dk, d_out) in ports_for(dst_type) {
            if d_out || dk != sk { continue; }
            if let Some(k) = want_kind { if k != sk { continue; } }
            count += 1;
            if found.is_none() { found = Some((sp, dp, sk)); }
        }
    }
    match (found, count) {
        (None, _) => Err("those two node types have no compatible ports"),
        (Some(link), 1) => Ok(link),
        (Some(_), _) => Err("more than one kind of connection fits — say which"),
    }
}

/// Add, remove or connect patcher nodes.
///
/// Ports are resolved from the node TYPES the caller sends, which it read from
/// the published graph. The alternative is asking a person to type "port 3",
/// which is engine trivia and gets it wrong.
fn build_patcher_graph(body: &str) -> Option<Result<UiPatcherGraphCommandPayload, &'static str>> {
    let add = body.contains("\"patchadd\"");
    let del = body.contains("\"patchdel\"");
    let link = body.contains("\"patchlink\"");
    if !(add || del || link) { return None; }
    let n = |k: &str| parse_num(body, k).unwrap_or(0);
    let mut p = UiPatcherGraphCommandPayload {
        command_type: UiCommandType::None as u16,
        flags: 0,
        track_id: n("\"track\"").max(0) as u32,
        base_version: n("\"base\"").max(0) as u32,
        node_id: n("\"node\"").max(0) as u32,
        node_type: n("\"nodeType\"").max(0) as u32,
        src_node_id: n("\"src\"").max(0) as u32,
        dst_node_id: n("\"dst\"").max(0) as u32,
        src_port_id: 0,
        dst_port_id: 0,
        edge_kind: 0,
    };
    if add {
        // PatcherNodeType::EventOut is the last one; the engine refuses past it
        // and so does this, so a typo is answered on the socket rather than in a
        // log the UI cannot see.
        if p.node_type > 6 { return Some(Err("no such node type")); }
        p.command_type = UiCommandType::AddPatcherNode as u16;
    } else if del {
        p.command_type = UiCommandType::RemovePatcherNode as u16;
    } else {
        if p.src_node_id == p.dst_node_id {
            return Some(Err("a node cannot connect to itself"));
        }
        let want = parse_num(body, "\"kind\"").map(|k| k.max(0) as u32);
        let (sp, dp, kind) = match resolve_link(
            n("\"srcType\"").max(0) as u32,
            n("\"dstType\"").max(0) as u32,
            want,
        ) {
            Ok(v) => v,
            Err(e) => return Some(Err(e)),
        };
        p.command_type = UiCommandType::ConnectPatcherNodes as u16;
        p.src_port_id = sp;
        p.dst_port_id = dp;
        p.edge_kind = kind;
    }
    Some(Ok(p))
}

/// DeviceKind, from apps/device_chain.h, in the enum's own order.
///
/// The same five ui-web's `DEVICE_KINDS` lists, because both are that enum. This
/// copy is here rather than only there for the reason the port table above is:
/// engine vocabulary belongs as close to the engine as the wire allows, and a
/// name the engine does not have must be refused before it becomes an integer.
const DEVICE_KINDS: [&str; 5] = [
    "patcher event",
    "patcher instrument",
    "patcher audio",
    "vst instrument",
    "vst effect",
];

/// The kind a chain command names, as the engine's DeviceKind number.
///
/// Accepted by name or by number, because both callers exist: a UI that read a
/// chain snapshot has the number, a person or an agent typing a command has the
/// word. Anything else is refused rather than sent — the engine derives a
/// device's capability mask from its kind with a switch that falls through to
/// `DeviceCapabilityNone`, so an unknown kind arrives as a device that consumes
/// nothing, produces nothing, and looks like a device.
fn device_kind(body: &str) -> Result<u32, &'static str> {
    const UNKNOWN: &str = "no such device kind - it is one of patcher event, \
                           patcher instrument, patcher audio, vst instrument, vst effect";
    if let Some(name) = parse_str_value(body, "\"kind\"") {
        return DEVICE_KINDS
            .iter()
            .position(|k| k.eq_ignore_ascii_case(name))
            .map(|i| i as u32)
            .ok_or(UNKNOWN);
    }
    match parse_num(body, "\"kind\"") {
        Some(k) if (0..DEVICE_KINDS.len() as i64).contains(&k) => Ok(k as u32),
        _ => Err(UNKNOWN),
    }
}

/// Add a device to a track's chain, or remove one from it.
///
///   {"type":"adddevice","track":0,"kind":"patcher event"}
///   {"type":"adddevice","track":0,"kind":4,"slot":2}
///   {"type":"deldevice","track":0,"device":7}
///
/// `UiChainCommandPayload`, not `UiCommandPayload` — the engine matches the entry
/// SIZE first and dispatches on commandType second, so a chain edit in the
/// generic shape is dropped without a word.
///
/// The engine numbers the new device itself (`kChainDeviceIdAuto`) and appends
/// it: an id chosen here would race every other writer on the ring, and there is
/// no position to insert at until something can express one.
fn build_chain_edit(body: &str) -> Option<Result<UiChainCommandPayload, &'static str>> {
    let add = body.contains("\"adddevice\"");
    let del = body.contains("\"deldevice\"");
    if !(add || del) { return None; }
    let mut p = UiChainCommandPayload {
        command_type: UiCommandType::None as u16,
        flags: 0,
        track_id: parse_num(body, "\"track\"").unwrap_or(0).max(0) as u32,
        base_version: parse_num(body, "\"base\"").unwrap_or(0).max(0) as u32,
        device_id: K_CHAIN_DEVICE_ID_AUTO,
        device_kind: 0,
        insert_index: K_CHAIN_DEVICE_ID_AUTO,
        // NOT 0. Node ids start at 0, so a zero here silently binds the new
        // device to whichever node the global patcher graph numbered first; the
        // engine resolves an unknown id to nothing, which is what "this device
        // has no patcher node yet" should mean.
        patcher_node_id: K_CHAIN_DEVICE_ID_AUTO,
        host_slot_index: K_HOST_SLOT_DIRECT,
        bypass: 0,
        reserved: [0; 4],
    };
    if del {
        // There is no sentinel for "remove whichever": kChainDeviceIdAuto is an
        // id no chain holds, so a missing one would travel to the engine and come
        // back as chain error 2 — a refusal about a device nobody named.
        let Some(id) = parse_num(body, "\"device\"").filter(|id| *id >= 0) else {
            return Some(Err("deldevice needs the id of the device to remove"));
        };
        p.command_type = UiCommandType::RemoveDevice as u16;
        p.device_id = id as u32;
        return Some(Ok(p));
    }
    let kind = match device_kind(body) { Ok(k) => k, Err(why) => return Some(Err(why)) };
    p.command_type = UiCommandType::AddDevice as u16;
    p.device_kind = kind;
    // hostSlotIndex is an index into the engine's plugin scan, and a VST device
    // without one is a device pointing at whatever the scan happened to list
    // first. There is nowhere in 40 bytes to put a path or a VstRef, so the
    // caller has to say which slot and this refuses when it does not. A patcher
    // device is not hosted out of process at all: it keeps the direct sentinel,
    // which is what makes its card read "in-process" rather than "slot 0".
    let vst = kind == 3 || kind == 4;
    match parse_num(body, "\"slot\"") {
        Some(s) if s >= 0 => p.host_slot_index = s as u32,
        Some(_) => return Some(Err("a host slot index cannot be negative")),
        None if vst => return Some(Err(
            "adding a VST device needs a slot - the engine resolves a plugin by \
             its scan index and the command has no room for a path")),
        None => {}
    }
    Some(Ok(p))
}

/// The most parameters the engine's region carries, kUiMaxDeviceParams.
const MAX_DEVICE_PARAMS: i64 = 256;

/// Write one of a device's parameters — which this cannot do, and says so.
///
///   {"type":"setparam","track":0,"device":7,"index":42,"valueMilli":620,
///    "uid":"9a3f…32 hex"}
///
/// THERE IS NO ENGINE COMMAND FOR THIS. `UiCommandType` in apps/event_payloads.h
/// runs 0..41 and none of them writes a plugin parameter: UpdateDevice (17) sets
/// bypass, patcher node and host slot and nothing else; SetAutomationTarget (13)
/// retargets an automation clip at a plugin index; SetModSourceValue (23) moves a
/// modulation SOURCE, not a parameter; RequestDeviceParams (40) only asks a host
/// to publish what it already has. So this refuses by name rather than sending
/// the nearest thing — a plausible neighbour would be acknowledged, would change
/// nothing, and would leave the rack reporting a silent engine when in fact the
/// question was wrong. Guessing a command number is the same bet with worse
/// odds: 42 is unallocated today and belongs to whoever the engine gives it to.
///
/// The shape is still validated, and validated FIRST, so a caller who got the
/// message wrong hears about their message rather than about the engine — and so
/// that the day the command exists, the wire format is already agreed and tested.
///
/// `valueMilli`, not a 0..1 float: `parse_num` reads integers, so `"value":0.62`
/// would parse as 0 and a drag to two thirds would arrive as a drag to minimum,
/// silently. It is also the engine's own unit — `UiDeviceParam::valueMilli` is
/// what came out — so the number goes back exactly as it came.
///
/// `uid` is the durable parameter id (`hashStableId16`, published beside every
/// parameter). `index` is ORDERING ONLY; shared_memory.h says so where it defines
/// the region. A command keyed on the index alone would point at a different
/// parameter after a plugin update, which is GUIDELINES 2.1's "an identifier that
/// moves is not an identity" — so both travel, and whatever the engine grows
/// should resolve the uid and use the index only as a hint.
fn build_set_param(body: &str) -> Option<Result<UiSetParamPayload, String>> {
    if !is_type(body, "setparam") { return None; }
    // Checked by its exact key, so `"valueMilli"` does not match: a caller who
    // sent a float gets told why the number would not have survived the trip.
    if body.contains("\"value\"") {
        return Some(Err("setparam carries valueMilli (an integer 0..1000, the engine's own \
                         unit) - a 0..1 float in \"value\" would parse as its integer part \
                         and arrive as 0".into()));
    }
    let Some(track) = parse_num(body, "\"track\"").filter(|t| *t >= 0) else {
        return Some(Err("setparam needs the track the device is on".into()));
    };
    let Some(device) = parse_num(body, "\"device\"").filter(|d| *d >= 0) else {
        return Some(Err("setparam needs the id of the device to change".into()));
    };
    let Some(index) = parse_num(body, "\"index\"")
        .filter(|i| (0..MAX_DEVICE_PARAMS).contains(i)) else {
        return Some(Err(format!("setparam needs a parameter index in 0..{}", MAX_DEVICE_PARAMS - 1)));
    };
    let Some(milli) = parse_num(body, "\"valueMilli\"").filter(|v| (0..=1000).contains(v)) else {
        return Some(Err("setparam needs valueMilli in 0..1000".into()));
    };
    // REQUIRED, and required to be non-zero — which it was not while this only
    // ever refused.
    //
    // The engine resolves the parameter by uid16 and has no index fallback, so a
    // missing or all-zero uid is a write the host silently drops. Sending it
    // anyway would be the worst available outcome: acknowledged, plausible, and
    // nothing moves. If a device ever publishes zeroed uids, the honest answer is
    // that its parameters are not writable yet, and that is what this says.
    let uid_hex = match parse_str_value(body, "\"uid\"") {
        Some(u) => u,
        None => return Some(Err("setparam needs the parameter's uid - the engine resolves \
                                 the parameter by its durable id, not by its index".into())),
    };
    if uid_hex.len() != 32 || !uid_hex.bytes().all(|b| b.is_ascii_hexdigit()) {
        return Some(Err("setparam's uid is the parameter's 16-byte durable id as 32 \
                         hex characters".into()));
    }
    let mut uid16 = [0u8; 16];
    for i in 0..16 {
        uid16[i] = u8::from_str_radix(&uid_hex[i * 2..i * 2 + 2], 16).unwrap_or(0);
    }
    if uid16.iter().all(|&b| b == 0) {
        return Some(Err("this parameter has no durable id, so the engine cannot find it - \
                         the host published a zeroed uid for it".into()));
    }
    // `index` is validated but NOT sent: the engine keys on uid16, and
    // shared_memory.h defines the index as ordering only. It is checked because a
    // message carrying an impossible index was built wrong, and hearing that is
    // more use than having the uid lookup miss for an unrelated reason.
    let _ = index;
    Some(Ok(UiSetParamPayload {
        command_type: UiCommandType::SetDeviceParam as u16,
        flags: 0,
        track_id: track as u32,
        device_id: device as u32,
        value_milli: milli as u32,
        uid16,
        reserved: [0u8; 8],
    }))
}

/// Does this message's `type` field say exactly this?
///
/// The dispatch below matched on `body.contains("\"waveform\"")` and friends —
/// a substring search over the WHOLE message, including its values. That is fine
/// until a value happens to be a command name, and then it is silent: loading a
/// project called `waveform` sends {"type":"load","name":"waveform"}, which the
/// waveform handler claimed and answered with "waveform needs the id of the
/// source to read". The load never reached the engine and nothing said why.
///
/// Found by the e2e test loading the waveform fixture by name, which is exactly
/// the case hand-testing had skipped. A project called `list`, `plugins`,
/// `settempo` or `setparam` would have done the same thing.
///
/// Matching the type FIELD closes it for every verb that goes through here. No
/// whitespace tolerance is needed: every client of this socket builds its
/// messages with JSON.stringify, which emits none.
fn is_type(body: &str, verb: &str) -> bool {
    let mut needle = String::with_capacity(verb.len() + 10);
    needle.push_str("\"type\":\"");
    needle.push_str(verb);
    needle.push('"');
    body.contains(&needle)
}

/// A process-wide request counter for waveform queries.
///
/// Allocated HERE, not in the browser. Two tabs minting their own ids into one
/// four-slot region is a livelock: each rejects the other's echo, times out, and
/// re-requests for ever. One counter in the one process both tabs go through
/// costs nothing and makes that impossible.
static WAVEFORM_SEQ: AtomicU32 = AtomicU32::new(1);

/// The waveform answer's binary frame. Header, then channel-planar i16 pairs.
///
/// Binary because a full slot is 24,576 pairs — 98 KB of i16 — and rendering that
/// as JSON numbers would be roughly 400 KB of text to parse per zoom step, for
/// data the browser wants as a typed array anyway.
const WAVE_MAGIC: u32 = 0x5749_4e55; // "UNIW"
const WAVE_WIRE_VERSION: u16 = 1;
const WAVE_HEADER_BYTES: usize = 56;

fn encode_waveform(v: &daw_bridge::control::WaveformSlotView, out: &mut Vec<u8>) {
    out.clear();
    out.extend_from_slice(&WAVE_MAGIC.to_le_bytes());              // 0
    out.extend_from_slice(&WAVE_WIRE_VERSION.to_le_bytes());       // 4
    out.push(1u8);                                                 // 6 kind: answer
    out.push(v.status as u8);                                      // 7
    out.extend_from_slice(&v.request_seq.to_le_bytes());           // 8
    out.extend_from_slice(&v.source_id.to_le_bytes());             // 12
    out.extend_from_slice(&((v.content_key & 0xffff_ffff) as u32).to_le_bytes()); // 16
    out.extend_from_slice(&((v.content_key >> 32) as u32).to_le_bytes());         // 20
    out.extend_from_slice(&v.decimation.to_le_bytes());            // 24
    out.extend_from_slice(&v.columns.to_le_bytes());               // 28
    out.extend_from_slice(&v.channels.to_le_bytes());              // 32
    out.extend_from_slice(&(v.first_frame as u32).to_le_bytes());  // 36
    out.extend_from_slice(&((v.first_frame >> 32) as u32).to_le_bytes()); // 40
    out.extend_from_slice(&(v.frame_count as u32).to_le_bytes());  // 44
    out.extend_from_slice(&((v.frame_count >> 32) as u32).to_le_bytes()); // 48
    out.extend_from_slice(&v.flags.to_le_bytes());                 // 52, to 56
    debug_assert_eq!(out.len(), WAVE_HEADER_BYTES, "waveform header drifted");
    for p in &v.pairs {
        out.extend_from_slice(&p.to_le_bytes());
    }
}

/**
 * A windowed waveform query.
 *
 *   {"type":"waveform","source":1,"decim":64,"frame":0,"cols":1396,"mask":3}
 *
 * `decim` 1 means raw samples: a bucket of one frame has min == max == the
 * sample, so the fine regime and the peak regime are the same request and the
 * same reply, and there is no crossover to get wrong.
 *
 * Validated here as well as in the engine, because the engine answers a bad
 * request with status 3 on a slot the caller then has to go and read to discover
 * it asked wrongly. Saying so on the socket the caller is already listening to is
 * the difference between a mistake and a mystery.
 */
fn build_waveform_request(body: &str) -> Option<Result<UiWaveformRequestPayload, String>> {
    if !is_type(body, "waveform") { return None; }
    let Some(source) = parse_num(body, "\"source\"").filter(|v| *v >= 0) else {
        return Some(Err("waveform needs the id of the source to read".into()));
    };
    let decim = parse_num(body, "\"decim\"").unwrap_or(1);
    if decim < 1 || (decim & (decim - 1)) != 0 {
        return Some(Err("waveform decim must be a power of two (1 = raw samples)".into()));
    }
    let frame = parse_num(body, "\"frame\"").unwrap_or(0).max(0);
    if frame % decim != 0 {
        return Some(Err("waveform frame must be a multiple of decim - buckets are \
                         anchored to source frame 0 so a window is an index, not a scan".into()));
    }
    let Some(cols) = parse_num(body, "\"cols\"").filter(|v| *v > 0) else {
        return Some(Err("waveform needs cols > 0".into()));
    };
    // 1 = channel 0, 3 = both. Defaulting to both and letting the engine clamp to
    // what the source has is friendlier than making every caller ask first.
    let mask = parse_num(body, "\"mask\"").unwrap_or(3).clamp(1, 3);
    let seq = WAVEFORM_SEQ.fetch_add(1, Ordering::Relaxed);
    Some(Ok(UiWaveformRequestPayload {
        command_type: UiCommandType::RequestWaveform as u16,
        flags: if body.contains("\"force\":true") { 1 } else { 0 },
        request_seq: seq,
        source_id: source as u32,
        decimation: decim as u32,
        first_frame_lo: frame as u32,
        first_frame_hi: ((frame as u64) >> 32) as u32,
        columns: cols as u32,
        channel_mask: mask as u32,
        reserved0: 0,
        reserved1: 0,
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
    } else if body.contains("\"reqparams\"") {
        // Ask the engine to query a device's HOST for its parameters. The answer
        // lands in a region, not on the wire — the engine bumps its version and
        // the publish loop below notices.
        p.command_type = UiCommandType::RequestDeviceParams as u16;
        p.value0 = parse_num(body, "\"device\"").unwrap_or(0).max(0) as u32;
    } else if is_type(body, "settempo") {
        // {"type":"settempo","milliBpm":128000}                  — the whole song
        // {"type":"settempo","milliBpm":128000,"tick":7680000}   — from here on
        //
        // milliBpm, not bpm: `parse_num` reads integers, so `"bpm":128.5` would
        // parse as 128 and the half would vanish without a word. It is also the
        // engine's own unit, so the number that comes back in the next frame is
        // the number that went out — which is what lets the UI tell "the engine
        // took it" from "the engine rounded it".
        //
        // Absence of "tick" is what means "the whole song", and it maps to the
        // engine's flags=1 (flatten the map). That is deliberately not the same
        // as `"tick":0`, which replaces the point AT zero and leaves any later
        // tempo changes standing. Someone typing a BPM into the transport bar
        // means the first thing; a tempo lane means the second.
        let milli = parse_num(body, "\"milliBpm\"").unwrap_or(0);
        // The engine ignores a non-positive tempo silently, on the far side of an
        // IPC boundary. Refuse here, where the UI is listening.
        if milli <= 0 { return Err("a tempo must be greater than zero"); }
        // 10..=1000 BPM. Not arbitrary: below 10 a bar is longer than most
        // sessions and above 1000 a sixteenth is shorter than an audio block, so
        // either end is a typo rather than a tempo. The engine will take any
        // positive number, which is why the check belongs here.
        if !(10_000..=1_000_000).contains(&milli) {
            return Err("tempo must be between 10 and 1000 BPM");
        }
        p.command_type = UiCommandType::SetTempo as u16;
        p.value0 = milli as u32;
        match parse_num(body, "\"tick\"") {
            Some(t) if t >= 0 => {
                p.flags = 0;
                p.note_nanotick_lo = t as u32;
                p.note_nanotick_hi = ((t as u64) >> 32) as u32;
            }
            _ => p.flags = 1,
        }
    } else if body.contains("\"loop\"") {
        // Start and end, not tick and dur: the engine reads the end as an
        // absolute nanotick out of the duration field, and calling the second
        // one "dur" in the wire message would invite someone to send a length.
        let start = parse_num(body, "\"start\"").unwrap_or(0).max(0) as u64;
        let end = parse_num(body, "\"end\"").unwrap_or(0).max(0) as u64;
        // The engine refuses end <= start to stderr, where nobody sees it.
        // Refuse here so the UI gets an answer on the socket it is listening to.
        if end <= start { return Err("a loop must end after it starts"); }
        p.command_type = UiCommandType::SetLoopRange as u16;
        p.note_nanotick_lo = start as u32;
        p.note_nanotick_hi = (start >> 32) as u32;
        p.note_duration_lo = end as u32;
        p.note_duration_hi = (end >> 32) as u32;
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
    } else if body.contains("\"reqchain\"") {
        // Ask the engine to re-emit a chain. The UI needs this because chains
        // arrive only as diffs: a page opened after the last device edit has
        // nothing to read back and would show an empty chain for ever.
        p.command_type = UiCommandType::RequestChainSnapshot as u16;
        // A missing or negative track means ALL of them. Not the `track_id`
        // parsed above, which floors a negative to 0 — that would silently turn
        // "every track" into "track 0" and answer a whole-project request with
        // one chain.
        p.track_id = match parse_num(body, "\"track\"") {
            Some(t) if t >= 0 => t as u32,
            _ => K_CHAIN_TRACK_ALL,
        };
    } else if body.contains("\"undo\"") {
        p.command_type = UiCommandType::Undo as u16;
    } else if body.contains("\"redo\"") {
        p.command_type = UiCommandType::Redo as u16;
    } else {
        return Err("unknown command");
    }
    Ok(p)
}

/// How long the engine waits, with nobody watching, before it quits.
///
/// A page RELOAD is a disconnect followed a moment later by a connect, so quitting
/// on the first empty moment would end the session every time somebody refreshed.
/// Long enough to cover a reload and a slow page load; short enough that a closed
/// window does not leave a synth running.
const NO_CLIENT_GRACE: Duration = Duration::from_secs(12);

/// The last UI disconnected.
///
/// Silence goes out IMMEDIATELY — closing the window should stop the sound, and a
/// user who has shut the UI is not waiting to hear whether we meant it. The quit
/// waits out the grace period and is cancelled if anyone reconnects, which a
/// reload does.
///
/// The engine is the application's process, not a service: a user thinks the
/// window IS the app, so leaving a headless engine playing after it closes is a
/// surprise nobody asked for.
fn last_client_gone(shm: &str, clients: Arc<AtomicU64>) {
    let shm = shm.to_string();
    let seen = CONNECTS.load(Ordering::Relaxed);
    thread::spawn(move || {
        // Stop the transport at once. Best effort: if the engine is already gone
        // there is nothing to silence and nothing to report.
        if let Ok(h) = EngineHandle::attach(&shm, true) {
            let mut p = UiCommandPayload {
                command_type: UiCommandType::Stop as u16,
                flags: 0, track_id: 0, plugin_index: 0, note_pitch: 0, value0: 0,
                note_nanotick_lo: 0, note_nanotick_hi: 0,
                note_duration_lo: 0, note_duration_hi: 0, base_version: 0,
            };
            let _ = h.send_command(p);
            thread::sleep(NO_CLIENT_GRACE);
            // Someone came back — a reload, or a second window. Either signal is
            // enough: a live count above zero, or ANY new connection since the
            // grace period began. The second is the one that matters, because a
            // reload's decrement can land after its own reconnect and leave the
            // count reading zero with a client on the other end.
            if clients.load(Ordering::Relaxed) > 0
                || CONNECTS.load(Ordering::Relaxed) != seen {
                eprintln!("sidecar: a client returned within the grace period — engine stays up");
                return;
            }
            p.command_type = UiCommandType::Quit as u16;
            match h.send_command(p) {
                Ok(()) => eprintln!("sidecar: no client for {}s — asked the engine to quit",
                                    NO_CLIENT_GRACE.as_secs()),
                Err(e) => eprintln!("sidecar: could not ask the engine to quit: {e}"),
            }
        }
    });
}

fn serve_commands(listener: TcpListener, shm: String, viewport: SharedViewport, projects: String,
                  plugin_cache: String) {
    for stream in listener.incoming().flatten() {
        let shm = shm.clone();
        let viewport = viewport.clone();
        let projects = projects.clone();
        let plugin_cache = plugin_cache.clone();
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
                        if is_type(&t, "list") {
                            let reply = list_projects(&projects);
                            if ws.send(tungstenite::Message::Text(reply)).is_err() { break; }
                            continue;
                        }
                        // Same shape and the same reason: a directory the
                        // browser cannot read, answered here.
                        if is_type(&t, "plugins") {
                            let reply = list_plugins(&plugin_cache);
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

                        // Graph edits: add, remove, connect. Own payload again.
                        if let Some(r) = build_patcher_graph(&t) {
                            let reply = match r {
                                Err(why) => format!("{{\"error\":\"{why}\"}}"),
                                Ok(p) => match handle.send_patcher_graph(p) {
                                    Ok(()) => format!(
                                        "{{\"ok\":true,\"type\":{},\"node\":{},\"srcPort\":{},\"dstPort\":{},\"kind\":{}}}",
                                        p.command_type, p.node_id, p.src_port_id, p.dst_port_id, p.edge_kind),
                                    Err(e) => format!("{{\"error\":\"{}\"}}", e.replace('"', "'")),
                                },
                            };
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

                        // Chain edits: add or remove a device. Own payload again.
                        //
                        // The reply names what was sent rather than what
                        // happened — the engine assigns the id, and whether the
                        // chain took the device arrives later as a ChainSnapshot
                        // or a ChainError on the outbound ring.
                        if let Some(r) = build_chain_edit(&t) {
                            let reply = match r {
                                Err(why) => format!("{{\"error\":\"{why}\"}}"),
                                Ok(p) => match handle.send_chain_command(p) {
                                    Ok(()) => format!(
                                        "{{\"ok\":true,\"type\":{},\"track\":{},\"device\":{},\"kind\":{},\"slot\":{}}}",
                                        p.command_type, p.track_id, p.device_id,
                                        p.device_kind, p.host_slot_index),
                                    Err(e) => format!("{{\"error\":\"{}\"}}", e.replace('"', "'")),
                                },
                            };
                            if ws.send(tungstenite::Message::Text(reply)).is_err() { break; }
                            continue;
                        }

                        // A windowed waveform read. Answered as a BINARY frame:
                        // a full slot is 24,576 pairs and JSON numbers would be
                        // ~400 KB of text per zoom step for something the browser
                        // wants as a typed array anyway.
                        if let Some(r) = build_waveform_request(&t) {
                            match r {
                                Err(why) => {
                                    let reply = format!("{{\"error\":\"{}\"}}", why.replace('"', "'"));
                                    if ws.send(tungstenite::Message::Text(reply)).is_err() { break; }
                                }
                                Ok(req) => {
                                    let seq = req.request_seq;
                                    let slot = (seq as usize) % K_UI_WAVEFORM_SLOTS;
                                    if let Err(e) = handle.send_waveform_request(req) {
                                        let reply = format!("{{\"error\":\"{}\"}}", e.replace('"', "'"));
                                        if ws.send(tungstenite::Message::Text(reply)).is_err() { break; }
                                    } else {
                                        // Poll the slot until the engine echoes OUR seq.
                                        //
                                        // The echo is the only proof the answer is
                                        // ours: slots are reused mod 4, so a slot
                                        // holding an answer is not a slot holding
                                        // the answer to this question. Reading it
                                        // without checking is how you draw one
                                        // clip's waveform inside another.
                                        //
                                        // ~250 ms at 1 ms: the engine answers a
                                        // sliced level in microseconds and a
                                        // scanned one in about a millisecond, so
                                        // this is two orders of margin, and a
                                        // timeout says so rather than hanging the
                                        // command thread.
                                        let mut got = None;
                                        for _ in 0..250 {
                                            if let Some(v) = handle.read_waveform_slot(slot) {
                                                if v.request_seq == seq { got = Some(v); break; }
                                            }
                                            thread::sleep(Duration::from_millis(1));
                                        }
                                        match got {
                                            Some(v) => {
                                                let mut out = Vec::with_capacity(
                                                    WAVE_HEADER_BYTES + v.pairs.len() * 2);
                                                encode_waveform(&v, &mut out);
                                                if ws.send(tungstenite::Message::Binary(out)).is_err() { break; }
                                            }
                                            None => {
                                                let reply = format!(
                                                    "{{\"error\":\"the engine did not answer waveform request {} within 250ms\"}}",
                                                    seq);
                                                if ws.send(tungstenite::Message::Text(reply)).is_err() { break; }
                                            }
                                        }
                                    }
                                }
                            }
                            continue;
                        }

                        // A parameter write. It carries a uid16, which does not
                        // fit UiCommandPayload's fields, so it has its own
                        // payload and its own send — the engine dispatches on
                        // size. Fire and forget by design: the value you see
                        // reflected comes back through the next device-params
                        // read-back, not through this socket.
                        if let Some(r) = build_set_param(&t) {
                            let reply = match r {
                                Ok(p) => match handle.send_set_param(p) {
                                    Ok(()) => format!(
                                        "{{\"ok\":true,\"type\":{},\"track\":{},\"device\":{},\"valueMilli\":{}}}",
                                        p.command_type, p.track_id, p.device_id, p.value_milli),
                                    Err(e) => format!("{{\"error\":\"{}\"}}", e.replace('"', "'")),
                                },
                                Err(why) => format!("{{\"error\":\"{}\"}}", why.replace('"', "'")),
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

fn serve(stream: TcpStream, shm: String, hz: u32, clients: Arc<AtomicU64>,
         viewport: SharedViewport, events: Arc<EngineEvents>, chains: Arc<ChainStore>) {
    let peer = stream.peer_addr().map(|a| a.to_string()).unwrap_or_default();
    let mut ws = match tungstenite::accept(stream) {
        Ok(w) => w,
        Err(e) => { eprintln!("sidecar: handshake failed from {peer}: {e}"); return; }
    };
    CONNECTS.fetch_add(1, Ordering::Relaxed);
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

    // Start at the newest, so a client that connects late is not handed a
    // backlog of errors from before it existed and told they are its own.
    let mut event_cursor = events.since(0).1;

    /// The scale registry, sent ONCE per client.
    ///
    /// The engine writes it at startup and never again, so it is state a client
    /// needs on arrival rather than news it could miss — no cursor, no version,
    /// no polling. Sent after the first successful read rather than before, so a
    /// client that connects while the engine is absent still gets it when the
    /// engine appears.
    let mut scales_sent = false;
    /**
     * The last device-params version this client was sent.
     *
     * The engine fills ONE region per query — ask for another device and the
     * previous answer is gone. So this is not accumulated state like the chains;
     * it is "the answer to the most recent question", and the version is what
     * says a new answer has arrived.
     */
    let mut params_version = 0u32;
    // The audio source table's version, so the descriptors are sent when they
    // change and not on every frame. They move on a project load and never
    // otherwise, and they carry u64 frame counts — pushing them at the 86 Hz
    // frame rate would be ~256 BigInt temporaries per frame in the client's
    // ingest path for data that is constant between loads.
    let mut audio_sources_version = 0u32;
    // Chains start at zero, deliberately unlike the events above: a chain is
    // STATE, not news. A tab that connects after the last device edit still has
    // to be told what the chains are, and the store is the only place that
    // knows — the engine publishes chains as diffs, so there is nothing to read
    // back. (Nothing has been missed either: the client can ask for a fresh
    // snapshot with `reqchain`.)
    let mut chain_cursor = 0u64;

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

        if !scales_sent {
            let scales = handle.read_scales();
            if !scales.is_empty() {
                let body: Vec<String> = scales.iter().map(|s| format!(
                    "{{\"id\":{},\"name\":\"{}\",\"octaveCents\":{},\"stepCents\":[{}]}}",
                    s.id,
                    s.name.replace('"', "'"),
                    s.octave_cents,
                    s.step_cents.iter().map(|c| c.to_string()).collect::<Vec<_>>().join(","))).collect();
                let msg = format!("{{\"scales\":[{}]}}", body.join(","));
                if ws.send(tungstenite::Message::Text(msg)).is_err() { break; }
                scales_sent = true;
            }
        }

        {
            // Audio sources + clips, same shape and the same rule as device
            // params: one shared read, a version gate, a JSON line when it moves.
            let av = handle.read_audio_sources();
            if av.version != 0 && av.version != audio_sources_version {
                audio_sources_version = av.version;
                let srcs: Vec<String> = av.sources.iter().map(|s| format!(
                    "{{\"id\":{},\"frames\":{},\"rate\":{},\"channels\":{},\"waveChannels\":{},\"status\":{},\"absPeak\":{:.6},\"levelMask\":{},\"keyLo\":{},\"keyHi\":{},\"path\":\"{}\"}}",
                    s.source_id, s.source_frames, s.source_rate_hz, s.source_channels,
                    s.wave_channels, s.status, s.abs_peak, s.level_mask,
                    (s.content_key & 0xffff_ffff) as u32, (s.content_key >> 32) as u32,
                    s.path.replace('\\', "").replace('"', "'"))).collect();
                let clips: Vec<String> = av.clips.iter().map(|c| format!(
                    "{{\"clipId\":{},\"sourceId\":{},\"startFrame\":{},\"lengthTicks\":{},\"fadeInTicks\":{},\"fadeOutTicks\":{},\"gainDb\":{:.3}}}",
                    c.clip_id, c.source_id, c.source_start_frame, c.clip_length_ticks,
                    c.fade_in_ticks, c.fade_out_ticks, c.gain_db)).collect();
                let msg = format!(
                    "{{\"audioSources\":{{\"version\":{},\"bpmMilli\":{},\"sources\":[{}],\"clips\":[{}]}}}}",
                    av.version, av.audio_map_bpm_milli, srcs.join(","), clips.join(","));
                if ws.send(tungstenite::Message::Text(msg)).is_err() { break; }
            }

            let dp = handle.read_device_params();
            if dp.version != 0 && dp.version != params_version {
                params_version = dp.version;
                let ps: Vec<String> = dp.params.iter().map(|q| format!(
                    "{{\"index\":{},\"value\":{},\"name\":\"{}\",\"display\":\"{}\",\"uid\":\"{}\"}}",
                    q.index, q.value,
                    q.name.replace('\\', "").replace('"', "'"),
                    q.display.replace('\\', "").replace('"', "'"),
                    q.uid16.iter().map(|b| format!("{b:02x}")).collect::<String>())).collect();
                let msg = format!(
                    "{{\"deviceParams\":{{\"version\":{},\"track\":{},\"device\":{},\"name\":\"{}\",\"params\":[{}]}}}}",
                    dp.version, dp.track_id, dp.device_id,
                    dp.device_name.replace('\\', "").replace('"', "'"),
                    ps.join(","));
                if ws.send(tungstenite::Message::Text(msg)).is_err() { break; }
            }
        }

        // Engine-originated messages, on the same socket as the frames but as
        // TEXT — the client already dispatches on the frame type, and these are
        // rare enough that a JSON line costs nothing. Each client has its own
        // cursor into the shared buffer, so one tab reading them does not take
        // them from another.
        let (msgs, cursor, missed) = events.since(event_cursor);
        if cursor != event_cursor {
            event_cursor = cursor;
            if !msgs.is_empty() || missed > 0 {
                let body = format!("{{\"engine\":[{}],\"missed\":{missed}}}", msgs.join(","));
                if ws.send(tungstenite::Message::Text(body)).is_err() { break; }
            }
        }

        // Device chains, same channel and the same rule: one shared store, a
        // cursor per client, and a message only when the store actually moved.
        // Whole-state, not a delta — the accumulation the client would otherwise
        // have to redo is the part that is easy to get wrong (see ChainStore).
        if let Some((body, rev)) = chains.changed_since(chain_cursor) {
            chain_cursor = rev;
            if ws.send(tungstenite::Message::Text(body)).is_err() { break; }
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
    if n == 0 && KEEP_ENGINE.load(Ordering::Relaxed) == 0 {
        last_client_gone(&shm, clients.clone());
    }
}

/// Decoded engine-to-UI messages, newest last, with a monotonic sequence.
///
/// The engine's out ring is SINGLE CONSUMER: whoever drains it takes the
/// messages away from everyone else. One thread drains, and every connected
/// client reads from this shared buffer with its own cursor — a per-client
/// drain would give each browser tab a random subset of the engine's errors,
/// which is worse than not reading the ring at all.
#[derive(Default)]
struct EngineEvents {
    /// (seq, json). Bounded: an engine spraying errors must not grow this.
    items: Mutex<VecDeque<(u64, String)>>,
    next_seq: AtomicU64,
}

/// How many messages are kept for clients that connect or fall behind.
const ENGINE_EVENT_CAP: usize = 64;

impl EngineEvents {
    fn push(&self, json: String) {
        let seq = self.next_seq.fetch_add(1, Ordering::Relaxed);
        let mut q = self.items.lock().unwrap();
        q.push_back((seq, json));
        while q.len() > ENGINE_EVENT_CAP { q.pop_front(); }
    }

    /// Everything newer than `after`, and the new cursor. A client that has
    /// fallen further behind than the cap silently skips — it is told, because
    /// "you missed some" is itself information the UI should show.
    fn since(&self, after: u64) -> (Vec<String>, u64, u64) {
        let q = self.items.lock().unwrap();
        let newest = self.next_seq.load(Ordering::Relaxed);
        if newest == after { return (Vec::new(), after, 0); }
        let oldest = q.front().map(|(s, _)| *s).unwrap_or(newest);
        let missed = oldest.saturating_sub(after);
        let out: Vec<String> = q.iter().filter(|(s, _)| *s >= after).map(|(_, j)| j.clone()).collect();
        (out, newest, missed)
    }
}

/// One device in a track's chain, as the engine published it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct ChainDevice {
    id: u32,
    kind: u32,
    pos: u32,
    node: u32,
    slot: u32,
    caps: u32,
    bypass: u32,
}

/// One ChainSnapshot entry off the ring: which track and which version it
/// belongs to, plus the device it describes — `None` for the empty-chain
/// sentinel, which is a statement about the chain rather than a device in it.
#[derive(Clone, Copy, Debug)]
struct ChainEntry {
    track: u32,
    version: u32,
    device: Option<ChainDevice>,
}

/// A track's chain as accumulated so far, plus the version it was published at.
#[derive(Default)]
struct TrackChain {
    track: u32,
    version: u32,
    /// Reused across snapshots — a replacement clears this rather than dropping
    /// it, so a chain that is re-published on every device edit allocates once.
    devices: Vec<ChainDevice>,
    /// The engine's first version is 1, so 0 would do as "never seen" — but that
    /// is the engine's counter's business, not ours, and a store that silently
    /// depends on it breaks the day the counter starts somewhere else.
    seen: bool,
}

/// Per-track device chains, accumulated from the engine's ChainSnapshot diffs.
///
/// Shared for the same reason `EngineEvents` is: the out ring is SINGLE CONSUMER,
/// so one thread drains it and every client reads what that thread accumulated.
/// A per-client drain would hand each browser tab a different subset of the SAME
/// snapshot's entries, and every tab would render a chain that is short by a
/// device or two — plausible, silent, and different in each window.
#[derive(Default)]
struct ChainStore {
    tracks: Mutex<Vec<TrackChain>>,
    /// Bumped on every accepted entry so a client can tell "unchanged" from
    /// "changed" with one relaxed load, and take the lock only when it must.
    revision: AtomicU64,
}

impl ChainStore {
    /// Fold one snapshot entry into the store.
    ///
    /// A snapshot is a REPLACEMENT keyed on (track, chain_version), not an
    /// append. The engine emits one entry per device and stamps them all with one
    /// version, so the first entry of a new version has to throw away what the
    /// previous version left behind. Appending instead shows every device twice
    /// after the first edit — the content changed while the key the consumer
    /// watches (the track, its version) stood still, which is GUIDELINES 2.1
    /// exactly, and it renders a perfectly believable chain while doing it.
    fn apply(&self, entry: &ChainEntry) {
        let mut tracks = self.tracks.lock().unwrap();
        let slot = match tracks.iter().position(|t| t.track == entry.track) {
            Some(i) => &mut tracks[i],
            None => {
                tracks.push(TrackChain { track: entry.track, ..Default::default() });
                tracks.last_mut().unwrap()
            }
        };
        if !slot.seen || entry.version > slot.version {
            slot.seen = true;
            slot.version = entry.version;
            slot.devices.clear();
        } else if entry.version < slot.version {
            // A late entry from a snapshot the engine has already superseded.
            // Ignored rather than appended: splicing two versions' devices into
            // one list produces a chain that never existed, and its version says
            // it is the newer one.
            return;
        }
        match entry.device {
            Some(d) => slot.devices.push(d),
            // The empty-chain sentinel. The clear above IS the whole update: a
            // track whose chain was emptied must show zero devices, not the ones
            // it had before the engine emptied it.
            None => {}
        }
        self.revision.fetch_add(1, Ordering::Release);
    }

    /// Forget everything. Called when the drainer re-attaches, because a fresh
    /// engine maps a new segment AND restarts its chain-version counter — so
    /// carrying the old versions over would make every snapshot the new engine
    /// sends look stale, and the store would ignore all of them for ever.
    ///
    /// Refilling it is the CLIENT's job, via `reqchain`. The drainer holds a
    /// writable handle but must not use it: the command ring is single-producer
    /// and the command thread is that producer, so a request sent from here
    /// would corrupt the ring it was trying to ask a question on.
    fn reset(&self) {
        let mut tracks = self.tracks.lock().unwrap();
        if tracks.is_empty() { return; }
        tracks.clear();
        self.revision.fetch_add(1, Ordering::Release);
    }

    /// The whole store as JSON if it has moved since `cursor`, with the new
    /// cursor. `None` when nothing changed — that is the common case, running on
    /// every client thread at frame rate, and it costs one relaxed load and no
    /// lock and no allocation.
    ///
    /// The message carries `rev` as well as each chain's `version` because a
    /// snapshot can straddle two drain ticks: the devices for version V can still
    /// be arriving while V stands still, so a client that cache-keys on the
    /// version alone would pin the half of the chain it saw first. `rev` names
    /// the other input.
    fn changed_since(&self, cursor: u64) -> Option<(String, u64)> {
        if self.revision.load(Ordering::Acquire) == cursor { return None; }
        let tracks = self.tracks.lock().unwrap();
        // Re-read under the lock: the value published with this body has to be
        // the one the body was built from, not one an entry applied in between
        // has already moved past.
        let rev = self.revision.load(Ordering::Acquire);
        let mut out = String::from("{\"chains\":[");
        for (i, t) in tracks.iter().enumerate() {
            if i > 0 { out.push(','); }
            out.push_str(&format!(
                "{{\"track\":{},\"version\":{},\"devices\":[", t.track, t.version));
            for (j, d) in t.devices.iter().enumerate() {
                if j > 0 { out.push(','); }
                out.push_str(&format!(
                    "{{\"id\":{},\"kind\":{},\"pos\":{},\"node\":{},\"slot\":{},\
                     \"caps\":{},\"bypass\":{}}}",
                    d.id, d.kind, d.pos, d.node, d.slot, d.caps, d.bypass));
            }
            out.push_str("]}");
        }
        out.push_str(&format!("],\"rev\":{rev}}}"));
        Some((out, rev))
    }

    #[cfg(test)]
    fn devices_of(&self, track: u32) -> Vec<ChainDevice> {
        let tracks = self.tracks.lock().unwrap();
        tracks.iter().find(|t| t.track == track).map(|t| t.devices.clone()).unwrap_or_default()
    }
}

/// Read a ChainSnapshot diff off the ring, or None if this entry is not one.
///
/// Offsets are the authority here, not a struct: `UiChainDiffPayload` is
/// diff_type u16@0, flags u16@2, track_id u32@4, chain_version u32@8,
/// device_id u32@12, device_kind u32@16, position u32@20, patcher_node_id u32@24,
/// host_slot_index u32@28, capability_mask u32@32, bypass u32@36 — 40 bytes, the
/// whole payload. A short entry is refused rather than read past.
fn decode_chain_snapshot(e: &EventEntry) -> Option<ChainEntry> {
    if (e.size as usize) < 40 { return None; }
    let p = &e.payload;
    let u16at = |i: usize| u16::from_le_bytes([p[i], p[i + 1]]);
    let u32at = |i: usize| u32::from_le_bytes([p[i], p[i + 1], p[i + 2], p[i + 3]]);
    if u16at(0) != UiDiffType::ChainSnapshot as u16 { return None; }
    let device_id = u32at(12);
    Some(ChainEntry {
        track: u32at(4),
        version: u32at(8),
        // kDeviceIdAuto in a SNAPSHOT is not a device id — it is how the engine
        // says "this chain is empty", emitted as one entry so the version still
        // travels. Taken as a device it would leave the track showing a phantom
        // device with id 4294967295 and, worse, never clear the real ones.
        device: (device_id != K_CHAIN_DEVICE_ID_AUTO).then(|| ChainDevice {
            id: device_id,
            kind: u32at(16),
            pos: u32at(20),
            node: u32at(24),
            slot: u32at(28),
            caps: u32at(32),
            bypass: u32at(36),
        }),
    })
}

/// Turn one EventEntry from the engine's out ring into JSON, or None to drop it.
///
/// Only the messages the UI must act on are forwarded. The note diffs (1-3) fire
/// on every edit and are already covered by clipVersion; forwarding them would be
/// a firehose that buries the one line somebody needs to read.
///
/// ResyncNeeded (4) is NOT an error — backend was explicit about this. An
/// undo/redo swaps a track's whole store, which the engine will not try to
/// describe note-by-note, so it republishes and asks for a refetch. It rides a
/// UiDiffPayload, whose offset 2 is `flags`, not `error_code`, and whose offset 8
/// is the clip version. Decoding it with the error prefix would report a code
/// that is really a flag word.
fn decode_engine_event(e: &EventEntry) -> Option<String> {
    if e.size < 8 { return None; }
    let p = &e.payload;
    let u16at = |i: usize| u16::from_le_bytes([p[i], p[i + 1]]);
    let u32at = |i: usize| u32::from_le_bytes([p[i], p[i + 1], p[i + 2], p[i + 3]]);
    let diff = u16at(0);
    let track = u32at(4);
    if diff == 4 {
        let clip_version = if e.size >= 12 { u32at(8) } else { 0 };
        return Some(format!(
            "{{\"kind\":\"resync\",\"track\":{track},\"clipVersion\":{clip_version}}}"));
    }
    // The error payloads DO share a prefix: diff_type:u16, error_code:u16,
    // track_id:u32.
    let code = u16at(2);
    let kind = match diff {
        6 => "chain-error",
        8 => "routing-error",
        10 => "mod-error",
        13 => "patcher-error",
        _ => return None,
    };
    // The patcher error names the nodes and ports involved, which is the
    // difference between "that connection was refused" and a usable message.
    if diff == 13 && e.size >= 32 {
        return Some(format!(
            "{{\"kind\":\"{kind}\",\"code\":{code},\"track\":{track},\"node\":{},\
             \"src\":{},\"dst\":{},\"srcPort\":{},\"dstPort\":{},\"edgeKind\":{}}}",
            u32at(8), u32at(12), u32at(16), u32at(20), u32at(24), u32at(28)));
    }
    Some(format!("{{\"kind\":\"{kind}\",\"code\":{code},\"track\":{track}}}"))
}

/// Drain the engine's out ring forever, decoding into `events`.
///
/// Its own thread with its own writable handle, at a slower cadence than the
/// publish loop: these are rare, and an error the user reads 50 ms late is not
/// a worse error.
/// UiDiffType by number, for the log. Names, because "type 5 = 12" is not a
/// report anyone can act on.
fn diff_type_name(t: u16) -> &'static str {
    match t {
        1 => "add-note", 2 => "remove-note", 3 => "update-note", 4 => "resync",
        5 => "chain-snapshot", 6 => "chain-error", 7 => "routing-snapshot",
        8 => "routing-error", 9 => "mod-snapshot", 10 => "mod-error",
        11 => "mod-link-uid", 12 => "patcher-delta", 13 => "patcher-error",
        _ => "unknown",
    }
}

fn drain_engine_events(shm: String, events: Arc<EngineEvents>, chains: Arc<ChainStore>) {
    const EVERY: Duration = Duration::from_millis(50);
    const MAX_PER_TICK: usize = 32;
    let mut handle = EngineHandle::attach(&shm, true).ok();
    let mut entries: Vec<EventEntry> = Vec::with_capacity(MAX_PER_TICK);
    let mut last_attach = Instant::now();
    let mut generation = SHM_GENERATION.load(Ordering::Acquire);
    let mut dropped: u64 = 0;
    let mut kinds = [0u64; 16];
    let mut last_report = Instant::now();
    loop {
        thread::sleep(EVERY);
        // The publish loop bumps the generation when it re-attaches to a
        // restarted engine. Follow it, because this thread's handle points into
        // the segment the OLD engine mapped: it would keep draining a ring
        // nobody writes any more, quietly, for ever — the chain would simply
        // stop updating with no error anywhere.
        let g = SHM_GENERATION.load(Ordering::Acquire);
        if g != generation {
            generation = g;
            last_attach = Instant::now();
            handle = EngineHandle::attach(&shm, true).ok();
            chains.reset();
        }
        let Some(h) = handle.as_ref() else {
            // The engine may not be up yet, or may have restarted under us.
            if last_attach.elapsed() > Duration::from_secs(2) {
                last_attach = Instant::now();
                handle = EngineHandle::attach(&shm, true).ok();
                if handle.is_some() { chains.reset(); }
            }
            continue;
        };
        entries.clear();
        let n = h.drain_ui_out(&mut entries, MAX_PER_TICK);
        if n == 0 { continue; }
        for e in &entries {
            if e.size >= 2 {
                let t = u16::from_le_bytes([e.payload[0], e.payload[1]]) as usize;
                if t < kinds.len() { kinds[t] += 1; }
            }
            // A chain snapshot is STATE, not news. It accumulates into the
            // shared store instead of joining the event queue, which would
            // replay a device list as a stream of one-line notifications and
            // bury the errors the queue exists for.
            if let Some(chain) = decode_chain_snapshot(e) {
                chains.apply(&chain);
                continue;
            }
            match decode_engine_event(e) {
                Some(json) => events.push(json),
                // Counted, not ignored. A silent drop here would make the ring
                // look quiet when it is busy with things we chose not to name.
                None => dropped += 1,
            }
        }
        // A histogram rather than a bare count. "1400 diffs not forwarded" says
        // nothing about whether the ring carries something worth reading; a
        // breakdown by type says exactly which capability is publishing and
        // which is silent.
        if last_report.elapsed() > Duration::from_secs(10) {
            last_report = Instant::now();
            let seen: Vec<String> = kinds.iter().enumerate()
                .filter(|(_, n)| **n > 0)
                .map(|(t, n)| format!("{}={}", diff_type_name(t as u16), n))
                .collect();
            if !seen.is_empty() {
                // `dropped` next to the histogram, not instead of it: the
                // breakdown says what the ring carries, this says how much of it
                // reaches a client. The two diverging is the interesting case.
                eprintln!("sidecar: engine diffs since start: {} (not-forwarded={dropped})",
                          seen.join(" "));
            }
        }
    }
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
            let plugin_cache = args.plugin_cache.clone();
            thread::spawn(move || serve_commands(l, shm, vp, projects, plugin_cache));
        }
        Err(e) => eprintln!("sidecar: no command port {} ({e}) — read-only", args.cmd_port),
    }

    // One drainer for the whole process, because the ring has one consumer. It
    // owns both shared stores for the same reason: what it takes off the ring is
    // taken from everyone else, so it has to put it somewhere every client can
    // read.
    let events = Arc::new(EngineEvents::default());
    let chains = Arc::new(ChainStore::default());
    {
        let (shm, ev, ch) = (args.shm.clone(), events.clone(), chains.clone());
        thread::spawn(move || drain_engine_events(shm, ev, ch));
    }

    let clients = Arc::new(AtomicU64::new(0));
    for stream in listener.incoming() {
        match stream {
            Ok(s) => {
                let (shm, hz, c) = (args.shm.clone(), args.hz, clients.clone());
                let vp = viewport.clone();
                let ev = events.clone();
                let ch = chains.clone();
                thread::spawn(move || serve(s, shm, hz, c, vp, ev, ch));
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
    fn entry(payload: &[u8]) -> EventEntry {
        let mut e = EventEntry { sample_time: 0, block_id: 0, event_type: 0,
                                 size: payload.len() as u16, flags: 0, payload: [0u8; 40] };
        e.payload[..payload.len()].copy_from_slice(payload);
        e
    }

    #[test]
    fn engine_events_name_the_nodes_and_drop_the_firehose() {
        // A patcher error: diff 13, code 6 (invalid port), track 0, then node,
        // src, dst, srcPort, dstPort, edgeKind as u32s.
        let mut p = Vec::new();
        p.extend_from_slice(&13u16.to_le_bytes());
        p.extend_from_slice(&6u16.to_le_bytes());
        p.extend_from_slice(&0u32.to_le_bytes());
        for v in [3u32, 3, 1, 5, 0, 2] { p.extend_from_slice(&v.to_le_bytes()); }
        let json = decode_engine_event(&entry(&p)).expect("patcher error");
        assert!(json.contains("\"kind\":\"patcher-error\""), "{json}");
        assert!(json.contains("\"code\":6"), "{json}");
        assert!(json.contains("\"srcPort\":5"), "{json}");
        assert!(json.contains("\"edgeKind\":2"), "{json}");

        // A note diff is NOT forwarded: it fires on every edit and clipVersion
        // already covers it. Dropping it is a decision, not an oversight.
        let mut n = Vec::new();
        n.extend_from_slice(&1u16.to_le_bytes());
        n.extend_from_slice(&0u16.to_le_bytes());
        n.extend_from_slice(&0u32.to_le_bytes());
        n.resize(32, 0);
        assert!(decode_engine_event(&entry(&n)).is_none());

        // A short entry cannot be trusted to have the common prefix.
        assert!(decode_engine_event(&entry(&[13, 0])).is_none());

        // ResyncNeeded rides a UiDiffPayload: offset 2 is FLAGS, offset 8 is the
        // clip version. Decoded with the error prefix it would report a flag word
        // as an error code and call an undo a failure.
        let mut r = Vec::new();
        r.extend_from_slice(&4u16.to_le_bytes());
        r.extend_from_slice(&7u16.to_le_bytes());     // flags, NOT an error code
        r.extend_from_slice(&2u32.to_le_bytes());     // track
        r.extend_from_slice(&912u32.to_le_bytes());   // clipVersion
        r.resize(40, 0);
        let json = decode_engine_event(&entry(&r)).expect("resync");
        assert_eq!(json, "{\"kind\":\"resync\",\"track\":2,\"clipVersion\":912}");
        assert!(!json.contains("code"), "a resync has no error code: {json}");

        // The other error kinds decode from the shared prefix.
        let mut c = Vec::new();
        c.extend_from_slice(&6u16.to_le_bytes());
        c.extend_from_slice(&2u16.to_le_bytes());
        c.extend_from_slice(&4u32.to_le_bytes());
        c.resize(32, 0);
        let json = decode_engine_event(&entry(&c)).expect("chain error");
        assert_eq!(json, "{\"kind\":\"chain-error\",\"code\":2,\"track\":4}");
    }

    #[test]
    fn every_client_reads_every_event_from_its_own_cursor() {
        let ev = EngineEvents::default();
        ev.push("{\"a\":1}".into());
        ev.push("{\"a\":2}".into());
        // Two clients, independent cursors: the second must not be robbed by
        // the first. This is the whole reason one thread drains the ring and
        // the clients read a buffer.
        let (one, c1, _) = ev.since(0);
        let (two, c2, _) = ev.since(0);
        assert_eq!(one.len(), 2);
        assert_eq!(two.len(), 2);
        assert_eq!(c1, c2);
        // Caught up: nothing new, cursor unchanged.
        let (none, c3, _) = ev.since(c1);
        assert!(none.is_empty());
        assert_eq!(c3, c1);
        // A client further behind than the cap is TOLD it missed some rather
        // than being handed a partial history that looks complete.
        for i in 0..(ENGINE_EVENT_CAP as u64 + 5) { ev.push(format!("{{\"n\":{i}}}")); }
        let (_, _, missed) = ev.since(1);
        assert!(missed > 0, "a client that fell behind is told");
    }

    /// One ChainSnapshot entry. Every field carries a distinct value, because
    /// the failure this file has had twice is a field read from the wrong
    /// offset, and a payload of zeros and ones cannot tell you about it.
    fn chain_entry(track: u32, version: u32, device_id: u32, pos: u32) -> EventEntry {
        let mut p = Vec::new();
        p.extend_from_slice(&(UiDiffType::ChainSnapshot as u16).to_le_bytes()); // 0
        p.extend_from_slice(&0u16.to_le_bytes());        // 2  flags
        p.extend_from_slice(&track.to_le_bytes());       // 4
        p.extend_from_slice(&version.to_le_bytes());     // 8
        p.extend_from_slice(&device_id.to_le_bytes());   // 12
        p.extend_from_slice(&7u32.to_le_bytes());        // 16 kind
        p.extend_from_slice(&pos.to_le_bytes());         // 20
        p.extend_from_slice(&11u32.to_le_bytes());       // 24 patcher node
        p.extend_from_slice(&2u32.to_le_bytes());        // 28 host slot
        p.extend_from_slice(&3u32.to_le_bytes());        // 32 capability mask
        p.extend_from_slice(&1u32.to_le_bytes());        // 36 bypass
        entry(&p)
    }

    fn apply_chain(store: &ChainStore, e: &EventEntry) {
        store.apply(&decode_chain_snapshot(e).expect("a chain snapshot"));
    }

    /// The engine emits one entry per device and stamps them all with the same
    /// version, so a new version REPLACES a track's devices. Appending shows
    /// every device twice after the first edit, with a version that says it is
    /// the current chain — the bug class in GUIDELINES 2.1.
    #[test]
    fn a_new_chain_version_replaces_the_devices_rather_than_appending_to_them() {
        let store = ChainStore::default();
        apply_chain(&store, &chain_entry(0, 87, 100, 0));
        apply_chain(&store, &chain_entry(0, 87, 101, 1));
        assert_eq!(store.devices_of(0).len(), 2);

        // The same chain re-published after an edit, one device longer.
        apply_chain(&store, &chain_entry(0, 88, 100, 0));
        apply_chain(&store, &chain_entry(0, 88, 101, 1));
        apply_chain(&store, &chain_entry(0, 88, 102, 2));
        let devices = store.devices_of(0);
        assert_eq!(devices.len(), 3, "three devices, not five: {devices:?}");
        assert_eq!(devices[0].id, 100);
        assert_eq!(devices[2].id, 102);
        // Every field, from its own offset. This is the decode half of the same
        // test: a payload read two bytes off still yields a plausible chain.
        assert_eq!(devices[2], ChainDevice {
            id: 102, kind: 7, pos: 2, node: 11, slot: 2, caps: 3, bypass: 1,
        });

        // Another track is its own chain and its own version; one track's
        // snapshot must not disturb it.
        apply_chain(&store, &chain_entry(3, 89, 200, 0));
        assert_eq!(store.devices_of(0).len(), 3);
        assert_eq!(store.devices_of(3).len(), 1);

        let (json, _) = store.changed_since(0).expect("something changed");
        assert!(json.contains("\"track\":0,\"version\":88"), "{json}");
        assert!(json.contains("\"id\":102,\"kind\":7,\"pos\":2,\"node\":11,\"slot\":2,\"caps\":3,\"bypass\":1"),
                "{json}");
    }

    /// Entries can still arrive for a version the engine has already superseded.
    /// Applied, they would splice two chains into one list and label the result
    /// with the newer version.
    #[test]
    fn an_entry_from_a_superseded_chain_version_is_ignored() {
        let store = ChainStore::default();
        apply_chain(&store, &chain_entry(1, 90, 100, 0));
        apply_chain(&store, &chain_entry(1, 91, 200, 0));
        let before = store.changed_since(0).expect("changed").1;

        apply_chain(&store, &chain_entry(1, 90, 101, 1));   // late, older version
        let devices = store.devices_of(1);
        assert_eq!(devices.len(), 1, "the stale entry was not appended: {devices:?}");
        assert_eq!(devices[0].id, 200);
        // Ignored means ignored: a client must not be woken for a no-op.
        assert!(store.changed_since(before).is_none(), "revision did not move");
    }

    /// An empty chain is a single entry whose device id is the auto sentinel.
    /// Read as a device it would leave a phantom in the list; not read at all it
    /// would leave the track showing the devices it no longer has.
    #[test]
    fn the_empty_chain_sentinel_leaves_the_track_with_no_devices() {
        let store = ChainStore::default();
        apply_chain(&store, &chain_entry(2, 5, 100, 0));
        apply_chain(&store, &chain_entry(2, 5, 101, 1));
        assert_eq!(store.devices_of(2).len(), 2);

        apply_chain(&store, &chain_entry(2, 6, K_CHAIN_DEVICE_ID_AUTO, 0));
        assert!(store.devices_of(2).is_empty(), "the chain was emptied");
        let (json, _) = store.changed_since(0).expect("changed");
        assert!(json.contains("\"track\":2,\"version\":6,\"devices\":[]}"), "{json}");
    }

    /// Same contract as the engine events: one shared store, one cursor per
    /// client, and no message when nothing moved. A drain per client would give
    /// each tab a different subset of one snapshot.
    #[test]
    fn every_client_reads_the_chains_from_its_own_cursor() {
        let store = ChainStore::default();
        assert!(store.changed_since(0).is_none(), "nothing published yet");
        apply_chain(&store, &chain_entry(0, 1, 100, 0));

        let (one, c1) = store.changed_since(0).expect("client one");
        let (two, c2) = store.changed_since(0).expect("client two, not robbed");
        assert_eq!(one, two);
        assert_eq!(c1, c2);
        assert!(store.changed_since(c1).is_none(), "caught up");

        apply_chain(&store, &chain_entry(0, 2, 100, 0));
        assert!(store.changed_since(c1).is_some(), "a new snapshot is news again");
    }

    /// A restarted engine maps a new segment and restarts its version counter,
    /// so the store has to forget: kept, the old versions make every snapshot
    /// the new engine sends look stale and the chain never updates again.
    #[test]
    fn a_reset_forgets_the_previous_engines_versions() {
        let store = ChainStore::default();
        apply_chain(&store, &chain_entry(0, 900, 100, 0));
        store.reset();
        assert!(store.devices_of(0).is_empty());
        apply_chain(&store, &chain_entry(0, 1, 200, 0));
        let devices = store.devices_of(0);
        assert_eq!(devices.len(), 1, "version 1 from a fresh engine is accepted");
        assert_eq!(devices[0].id, 200);
    }

    /// Not every diff on the ring is a chain snapshot, and a truncated entry
    /// cannot be trusted to carry the fields we would read out of it.
    #[test]
    fn only_a_full_chain_snapshot_entry_decodes() {
        let mut short = chain_entry(0, 1, 100, 0);
        short.size = 24;
        assert!(decode_chain_snapshot(&short).is_none(), "a short entry is refused");
        // A note diff shares the first four bytes' shape and nothing else.
        let mut note = Vec::new();
        note.extend_from_slice(&1u16.to_le_bytes());
        note.resize(40, 0);
        assert!(decode_chain_snapshot(&entry(&note)).is_none());
        // ...and a chain snapshot does not leak into the error/event queue.
        assert!(decode_engine_event(&chain_entry(0, 1, 100, 0)).is_none());
    }

    /// "All tracks" is 0xFFFFFFFF, not 0. The generic parse above floors a
    /// negative track to 0, which would answer a whole-project request with one
    /// chain and look like the UI simply forgot the other tracks.
    #[test]
    fn reqchain_asks_for_every_track_unless_one_is_named() {
        let p = build_command(r#"{"type":"reqchain"}"#).expect("should build");
        assert_eq!(p.command_type, UiCommandType::RequestChainSnapshot as u16);
        assert_eq!(p.command_type, 37, "the engine's own number for it");
        assert_eq!(p.track_id, 0xFFFF_FFFF);
        assert_eq!(build_command(r#"{"type":"reqchain","track":-1}"#).unwrap().track_id,
                   0xFFFF_FFFF, "a negative track means all of them");
        assert_eq!(build_command(r#"{"type":"reqchain","track":3}"#).unwrap().track_id, 3);
    }

    #[test]
    fn a_link_resolves_ports_from_the_two_node_types() {
        // euclidean -> passthru: event out 1 to event in 0, kind Event.
        assert_eq!(resolve_link(1, 2, None), Ok((1, 0, 0)));
        // lfo -> kernel: control out 3 to control in 2, kind Control.
        assert_eq!(resolve_link(4, 0, None), Ok((3, 2, 2)));
        // audio -> audio: ports 5 and 4, kind Audio.
        assert_eq!(resolve_link(3, 3, None), Ok((5, 4, 1)));
        // An LFO's control output has nowhere to go on a euclidean.
        assert!(resolve_link(4, 1, None).is_err());
        // Euclidean is a source: nothing connects INTO it.
        assert!(resolve_link(2, 1, None).is_err());
        // Kernel to kernel could be events or control, so it is refused rather
        // than guessed — until the caller says which.
        assert_eq!(resolve_link(0, 0, None).err(),
                   Some("more than one kind of connection fits — say which"));
        assert_eq!(resolve_link(0, 0, Some(0)), Ok((1, 0, 0)));
        assert_eq!(resolve_link(0, 0, Some(2)), Ok((3, 2, 2)));
    }

    #[test]
    fn graph_edits_carry_the_right_command_and_refuse_the_impossible() {
        let p = build_patcher_graph(r#"{"type":"patchadd","nodeType":4}"#).unwrap().unwrap();
        assert_eq!(p.command_type, UiCommandType::AddPatcherNode as u16);
        assert_eq!(p.node_type, 4);

        let p = build_patcher_graph(r#"{"type":"patchdel","node":7}"#).unwrap().unwrap();
        assert_eq!(p.command_type, UiCommandType::RemovePatcherNode as u16);
        assert_eq!(p.node_id, 7);

        let p = build_patcher_graph(
            r#"{"type":"patchlink","src":0,"srcType":1,"dst":3,"dstType":2}"#)
            .unwrap().unwrap();
        assert_eq!(p.command_type, UiCommandType::ConnectPatcherNodes as u16);
        assert_eq!((p.src_node_id, p.dst_node_id), (0, 3));
        assert_eq!((p.src_port_id, p.dst_port_id, p.edge_kind), (1, 0, 0));

        // A node type the engine does not have is refused here, where the answer
        // reaches the UI, rather than on the engine's error ring.
        assert_eq!(build_patcher_graph(r#"{"type":"patchadd","nodeType":9}"#).unwrap().err(),
                   Some("no such node type"));
        assert_eq!(build_patcher_graph(
            r#"{"type":"patchlink","src":2,"srcType":1,"dst":2,"dstType":2}"#).unwrap().err(),
                   Some("a node cannot connect to itself"));
        assert!(build_patcher_graph(r#"{"type":"note","pitch":60}"#).is_none());
    }

    #[test]
    fn set_tempo_distinguishes_the_whole_song_from_a_point() {
        // No "tick" => flatten the map. This is the transport-bar BPM edit.
        let p = build_command(r#"{"type":"settempo","milliBpm":128000}"#).expect("settempo");
        assert_eq!(p.command_type, UiCommandType::SetTempo as u16);
        assert_eq!(p.value0, 128_000);
        assert_eq!(p.flags, 1, "no position means the whole song");

        // A "tick" => insert-or-replace a point there, leaving later changes be.
        let p = build_command(r#"{"type":"settempo","milliBpm":90000,"tick":4294967296}"#)
            .expect("settempo at a point");
        assert_eq!(p.flags, 0, "a position means one point");
        assert_eq!(p.value0, 90_000);
        assert_eq!(p.note_nanotick_lo, 0);
        assert_eq!(p.note_nanotick_hi, 1);

        // `"tick":0` is NOT the same as no tick: it replaces the point at zero
        // and leaves any later tempo change standing. Conflating the two would
        // make a tempo-lane edit at bar 1 silently wipe the rest of the map.
        let p = build_command(r#"{"type":"settempo","milliBpm":90000,"tick":0}"#).expect("at 0");
        assert_eq!(p.flags, 0);
        assert_eq!(p.note_nanotick_lo, 0);
    }

    #[test]
    fn set_tempo_refuses_a_tempo_no_one_could_have_meant() {
        // The engine ignores a non-positive tempo on the far side of an IPC
        // boundary, where nothing on the socket ever hears about it.
        assert_eq!(build_command(r#"{"type":"settempo","milliBpm":0}"#).err(),
                   Some("a tempo must be greater than zero"));
        assert_eq!(build_command(r#"{"type":"settempo","milliBpm":-120000}"#).err(),
                   Some("a tempo must be greater than zero"));
        // Below 10 BPM a bar outlasts most sessions; above 1000 a sixteenth is
        // shorter than an audio block. Either end is a typo.
        assert_eq!(build_command(r#"{"type":"settempo","milliBpm":9999}"#).err(),
                   Some("tempo must be between 10 and 1000 BPM"));
        assert_eq!(build_command(r#"{"type":"settempo","milliBpm":1000001}"#).err(),
                   Some("tempo must be between 10 and 1000 BPM"));
        // ...and the edges themselves are allowed.
        assert!(build_command(r#"{"type":"settempo","milliBpm":10000}"#).is_ok());
        assert!(build_command(r#"{"type":"settempo","milliBpm":1000000}"#).is_ok());
    }

    #[test]
    fn a_loop_carries_two_absolute_nanoticks_and_refuses_an_empty_span() {
        let p = build_command(r#"{"type":"loop","start":4294967296,"end":4294967296000}"#)
            .expect("loop");
        assert_eq!(p.command_type, UiCommandType::SetLoopRange as u16);
        // Start in the nanotick pair, end in the DURATION pair — the engine reads
        // it as an absolute position, not a length.
        assert_eq!(p.note_nanotick_lo, 0);
        assert_eq!(p.note_nanotick_hi, 1);
        assert_eq!(
            ((p.note_duration_hi as u64) << 32) | p.note_duration_lo as u64,
            4_294_967_296_000
        );
        // A zero-length or inverted span is refused here rather than on the
        // engine's stderr.
        assert_eq!(build_command(r#"{"type":"loop","start":960000,"end":960000}"#).err(),
                   Some("a loop must end after it starts"));
        assert_eq!(build_command(r#"{"type":"loop","start":960000,"end":0}"#).err(),
                   Some("a loop must end after it starts"));
    }

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
    fn a_device_kind_is_accepted_by_name_or_by_the_number_the_chain_publishes() {
        assert_eq!(device_kind(r#"{"kind":"patcher event"}"#), Ok(0));
        assert_eq!(device_kind(r#"{"kind":"VST Effect"}"#), Ok(4), "case does not matter");
        assert_eq!(device_kind(r#"{"kind":3}"#), Ok(3));
        // The number after the key, not the name of the field that follows it —
        // the loose string parser returns "slot" for this and would have sent
        // whatever kind that resolved to.
        assert_eq!(device_kind(r#"{"kind":2,"slot":1}"#), Ok(2));
    }

    #[test]
    fn an_unknown_device_kind_is_refused_rather_than_sent_as_a_number() {
        // DeviceKind stops at 4. The engine's capability switch falls through to
        // "no capabilities" for anything past it, so a 9 would be added as a real
        // device that can neither consume nor produce anything.
        assert!(device_kind(r#"{"kind":9}"#).is_err());
        assert!(device_kind(r#"{"kind":-1}"#).is_err());
        assert!(device_kind(r#"{"kind":"reverb"}"#).is_err());
        assert!(device_kind(r#"{"track":0}"#).is_err(), "a missing kind is not a default");
        assert_eq!(
            build_chain_edit(r#"{"type":"adddevice","track":0,"kind":9}"#).unwrap().err(),
            device_kind(r#"{"kind":9}"#).err());
    }

    #[test]
    fn adding_a_device_lets_the_engine_number_it_and_appends_it() {
        let p = build_chain_edit(r#"{"type":"adddevice","track":2,"kind":"patcher audio"}"#)
            .expect("recognised").expect("built");
        assert_eq!(p.command_type, UiCommandType::AddDevice as u16);
        assert_eq!(p.command_type, 14, "the engine's own number for it");
        assert_eq!(p.track_id, 2);
        assert_eq!(p.device_kind, 2);
        assert_eq!(p.device_id, K_CHAIN_DEVICE_ID_AUTO, "the engine assigns the id");
        assert_eq!(p.insert_index, K_CHAIN_DEVICE_ID_AUTO, "appended");
        // Node ids start at 0, so 0 would bind the new device to the graph's
        // first node instead of to none.
        assert_eq!(p.patcher_node_id, K_CHAIN_DEVICE_ID_AUTO, "no patcher node yet");
        assert_eq!(p.bypass, 0);
    }

    #[test]
    fn a_patcher_device_is_added_in_process_rather_than_in_slot_zero() {
        let p = build_chain_edit(r#"{"type":"adddevice","kind":0}"#).unwrap().unwrap();
        assert_eq!(p.host_slot_index, K_HOST_SLOT_DIRECT);
        assert_ne!(p.host_slot_index, 0, "slot 0 is a real plugin in the scan");
    }

    #[test]
    fn a_vst_device_without_a_slot_is_refused_rather_than_pointed_at_the_first_plugin() {
        // The 40-byte payload carries an index into the engine's plugin scan and
        // nothing else — no path, no VstRef — so there is no honest default.
        assert!(build_chain_edit(r#"{"type":"adddevice","kind":"vst effect"}"#)
                .unwrap().is_err());
        assert!(build_chain_edit(r#"{"type":"adddevice","kind":3}"#).unwrap().is_err());
        let p = build_chain_edit(r#"{"type":"adddevice","kind":"vst effect","slot":2}"#)
            .unwrap().unwrap();
        assert_eq!(p.device_kind, 4);
        assert_eq!(p.host_slot_index, 2);
    }

    #[test]
    fn removing_a_device_needs_the_id_and_carries_nothing_else() {
        let p = build_chain_edit(r#"{"type":"deldevice","track":1,"device":7}"#)
            .expect("recognised").expect("built");
        assert_eq!(p.command_type, UiCommandType::RemoveDevice as u16);
        assert_eq!(p.command_type, 15, "the engine's own number for it");
        assert_eq!(p.track_id, 1);
        assert_eq!(p.device_id, 7);
        // "deldevice" must not be read as the device id by a parser looking for
        // the key "device" inside it.
        assert_eq!(build_chain_edit(r#"{"type":"deldevice","device":0}"#).unwrap().unwrap()
                   .device_id, 0, "device 0 is a device, not a missing id");
        assert!(build_chain_edit(r#"{"type":"deldevice","track":1}"#).unwrap().is_err());
        assert!(build_chain_edit(r#"{"type":"deldevice","device":-1}"#).unwrap().is_err());
    }

    /// A well-formed parameter write becomes a payload, keyed on the uid.
    ///
    /// This test used to assert a REFUSAL: nothing in UiCommandType wrote a
    /// plugin parameter, and the sidecar was not going to pick a neighbouring
    /// number and hope. The comment on it said "when the engine grows the
    /// command, this test is the one that has to change, and changing it is the
    /// moment somebody re-reads the enum." The engine grew it — SetDeviceParam,
    /// 43 — so here is that moment, and the numbers below are read off the enum.
    #[test]
    fn setparam_becomes_a_payload_keyed_on_the_durable_id() {
        let p = build_set_param(concat!(
            r#"{"type":"setparam","track":2,"device":7,"index":42,"valueMilli":620,"#,
            r#""uid":"0123456789abcdef0123456789abcdef"}"#))
            .expect("recognised").expect("built");
        assert_eq!(p.command_type, UiCommandType::SetDeviceParam as u16);
        assert_eq!(p.command_type, 43, "the engine's own number for it");
        assert_eq!(p.track_id, 2);
        assert_eq!(p.device_id, 7);
        assert_eq!(p.value_milli, 620, "the engine's unit, unconverted");
        // The hex string is bytes, not characters: 0x01 0x23 ... not '0' '1' ...
        assert_eq!(p.uid16[0], 0x01);
        assert_eq!(p.uid16[1], 0x23);
        assert_eq!(p.uid16[15], 0xef);
        // 40 bytes, which is what the engine dispatches on — a payload of the
        // wrong size is read as some other command entirely.
        assert_eq!(std::mem::size_of::<UiSetParamPayload>(), 40);
        // And nothing else in the sidecar claims it, which is the actual danger:
        // every other builder matches on a substring.
        assert!(build_chain_edit(
            r#"{"type":"setparam","track":0,"device":7,"index":1,"valueMilli":1}"#).is_none());
        assert!(build_chord(
            r#"{"type":"setparam","track":0,"device":7,"index":1,"valueMilli":1}"#).is_none());
        assert!(build_patcher_graph(
            r#"{"type":"setparam","track":0,"device":7,"index":1,"valueMilli":1}"#).is_none());
        assert!(build_command(
            r#"{"type":"setparam","track":0,"device":7,"index":1,"valueMilli":1}"#).is_err(),
            "and the generic builder must not send 40 bytes the engine reads as something else");
    }

    #[test]
    fn setparam_refuses_what_the_host_would_silently_drop() {
        let miss = |b: &str| build_set_param(b).expect("recognised").expect_err("refused");
        // A 0..1 float is the shape everybody reaches for first, and parse_num
        // reads integers — so 0.62 would arrive as 0, silently, which is the one
        // failure mode this whole path exists to avoid.
        assert!(miss(r#"{"type":"setparam","track":0,"device":7,"index":1,"value":0.62}"#)
                .contains("valueMilli"));
        assert!(miss(r#"{"type":"setparam","device":7,"index":1,"valueMilli":1}"#)
                .contains("track"));
        assert!(miss(r#"{"type":"setparam","track":0,"index":1,"valueMilli":1}"#)
                .contains("device"));
        assert!(miss(r#"{"type":"setparam","track":0,"device":7,"valueMilli":1}"#)
                .contains("parameter index"));
        assert!(miss(r#"{"type":"setparam","track":0,"device":7,"index":256,"valueMilli":1}"#)
                .contains("parameter index"), "256 parameters means 0..255");
        assert!(miss(r#"{"type":"setparam","track":0,"device":7,"index":1}"#)
                .contains("valueMilli in 0..1000"));
        assert!(miss(r#"{"type":"setparam","track":0,"device":7,"index":1,"valueMilli":1001}"#)
                .contains("valueMilli in 0..1000"));
        // The uid is REQUIRED now, and was optional while this only ever refused.
        // The engine resolves the parameter by uid16 and has no index fallback,
        // so a missing or zeroed one is a write the host drops without a word —
        // acknowledged, plausible, and nothing moves. Refusing is the honest answer.
        assert!(miss(r#"{"type":"setparam","track":0,"device":7,"index":1,"valueMilli":1}"#)
                .contains("uid"), "absent");
        assert!(miss(r#"{"type":"setparam","track":0,"device":7,"index":1,"valueMilli":1,"uid":"zz"}"#)
                .contains("32 hex"), "malformed");
        assert!(miss(concat!(r#"{"type":"setparam","track":0,"device":7,"index":1,"#,
                             r#""valueMilli":1,"uid":"00000000000000000000000000000000"}"#))
                .contains("no durable id"), "all zeroes resolves to nothing");
        // Not a setparam at all.
        assert!(build_set_param(r#"{"type":"note","pitch":60}"#).is_none());
        assert!(build_set_param(r#"{"type":"reqparams","track":0,"device":7}"#).is_none());
    }

    #[test]
    fn a_chain_edit_does_not_answer_for_any_other_command() {
        assert!(build_chain_edit(r#"{"type":"note","pitch":60}"#).is_none());
        assert!(build_chain_edit(r#"{"type":"patchadd","nodeType":4}"#).is_none());
        // The generic builder must not claim one either — it would send 40 bytes
        // the engine reads as some other command entirely.
        assert!(build_command(r#"{"type":"adddevice","kind":0}"#).is_err());
        assert!(build_command(r#"{"type":"deldevice","device":7}"#).is_err(),
                "deldevice is not the note command delete");
    }

    #[test]
    fn a_project_may_be_named_after_a_command() {
        // A message is dispatched on its `type` FIELD, not on any substring of it.
        //
        // This was not true: the handlers matched `body.contains("\"waveform\"")`,
        // so loading a project called `waveform` — which sends
        // {"type":"load","name":"waveform"} — was claimed by the waveform handler
        // and answered "waveform needs the id of the source to read". The load
        // never reached the engine, and the reply named a concept the caller had
        // not mentioned. Every verb dispatched here had the same hole.
        for verb in ["waveform", "setparam", "settempo", "list", "plugins",
                     "note", "chord", "loop", "seek", "adddevice"] {
            let body = format!("{{\"type\":\"load\",\"name\":\"{verb}\"}}");
            assert!(build_waveform_request(&body).is_none(),
                    "the waveform handler claimed a load of a project called {verb}");
            assert!(build_set_param(&body).is_none(),
                    "the setparam handler claimed a load of a project called {verb}");
            assert!(!is_type(&body, verb),
                    "is_type matched {verb} in a message whose type is load");
            // ...and the load itself still builds, which is the point.
            let p = build_command(&body).expect("a load of a project named after a verb");
            assert_eq!(p.command_type, UiCommandType::LoadProject as u16,
                       "loading a project called {verb} must still be a load");
        }
        // The positive direction, so the check is not vacuously satisfied by
        // is_type never matching anything.
        assert!(is_type(r#"{"type":"waveform","source":1}"#, "waveform"));
        assert!(build_waveform_request(r#"{"type":"waveform","source":1,"decim":1,"cols":4}"#)
                .is_some(), "a real waveform request is still recognised");
    }

    #[test]
    fn a_waveform_request_is_validated_before_it_reaches_the_engine() {
        let ok = |b: &str| build_waveform_request(b).expect("recognised").expect("built");
        let bad = |b: &str| build_waveform_request(b).expect("recognised").expect_err("refused");

        let p = ok(r#"{"type":"waveform","source":2,"decim":64,"frame":4294967296,"cols":1396}"#);
        assert_eq!(p.command_type, UiCommandType::RequestWaveform as u16);
        assert_eq!(p.command_type, 44, "the engine's own number for it");
        assert_eq!(p.source_id, 2);
        assert_eq!(p.decimation, 64);
        assert_eq!(p.columns, 1396);
        // 64-bit frame split lo/hi, like every other position on this wire.
        assert_eq!(p.first_frame_lo, 0);
        assert_eq!(p.first_frame_hi, 1);
        assert_eq!(p.channel_mask, 3, "both channels unless asked otherwise");
        assert_eq!(std::mem::size_of::<UiWaveformRequestPayload>(), 40);

        // decim 1 is RAW SAMPLES and must be accepted: a bucket of one frame has
        // min == max == the sample, which is what makes the fine regime and the
        // peak regime one mechanism instead of two with a seam between them.
        assert_eq!(ok(r#"{"type":"waveform","source":1,"decim":1,"frame":0,"cols":8}"#).decimation, 1);

        // Every sequence number is distinct, because the slot is seq % 4 and the
        // echo is the only proof an answer is yours.
        let a = ok(r#"{"type":"waveform","source":1,"decim":1,"frame":0,"cols":1}"#).request_seq;
        let b = ok(r#"{"type":"waveform","source":1,"decim":1,"frame":0,"cols":1}"#).request_seq;
        assert_ne!(a, b, "two requests must not share a slot identity");

        // Refusals, on the socket the caller is listening to rather than as a
        // status code on a slot they would have to go and read.
        assert!(bad(r#"{"type":"waveform","decim":1,"frame":0,"cols":8}"#).contains("source"));
        assert!(bad(r#"{"type":"waveform","source":1,"decim":48,"frame":0,"cols":8}"#)
                .contains("power of two"), "48 is not");
        assert!(bad(r#"{"type":"waveform","source":1,"decim":0,"frame":0,"cols":8}"#)
                .contains("power of two"));
        // Buckets are anchored to source frame 0, so a misaligned window is not a
        // window at all — it would silently become a scan at a different phase and
        // the outline would crawl as you panned.
        assert!(bad(r#"{"type":"waveform","source":1,"decim":64,"frame":32,"cols":8}"#)
                .contains("multiple of decim"));
        assert!(bad(r#"{"type":"waveform","source":1,"decim":1,"frame":0,"cols":0}"#)
                .contains("cols"));
        // Not a waveform request at all.
        assert!(build_waveform_request(r#"{"type":"note","pitch":60}"#).is_none());
    }

    #[test]
    fn the_waveform_frame_says_what_it_is_before_it_says_anything_else() {
        let v = daw_bridge::control::WaveformSlotView {
            request_seq: 7, source_id: 2, content_key: 0x1122_3344_5566_7788,
            decimation: 64, columns: 3, channels: 2,
            first_frame: 0x1_0000_0000, frame_count: 192, status: 1, flags: 1,
            pairs: vec![-32767, 32767, 0, 0, 5, -5, 1, 2, 3, 4, 5, 6],
        };
        let mut out = Vec::new();
        encode_waveform(&v, &mut out);
        assert_eq!(out.len(), WAVE_HEADER_BYTES + v.pairs.len() * 2);
        assert_eq!(&out[0..4], &WAVE_MAGIC.to_le_bytes(), "magic first");
        assert_eq!(out[6], 1, "kind");
        assert_eq!(out[7], 1, "status rides in the header, not the payload");
        // The 64-bit fields are split lo/hi rather than written as u64, because
        // the reader is JavaScript and getBigUint64 allocates a BigInt per call.
        assert_eq!(&out[36..40], &0u32.to_le_bytes(), "firstFrame lo");
        assert_eq!(&out[40..44], &1u32.to_le_bytes(), "firstFrame hi");
        // Pairs follow the header verbatim, little-endian, so the client can view
        // them as an Int16Array without copying.
        let first = i16::from_le_bytes([out[WAVE_HEADER_BYTES], out[WAVE_HEADER_BYTES + 1]]);
        assert_eq!(first, -32767);
    }

    #[test]
    fn a_truncated_plugin_catalogue_is_refused_rather_than_forwarded() {
        // The realistic failure: the engine rewrites this file after a scan and a
        // read lands mid-write. Forwarding half a document costs the client the
        // WHOLE message to a parse error, with nothing saying why.
        assert!(is_complete_json_object(r#"{"plugins":[]}"#));
        assert!(is_complete_json_object(r#"  {"a":{"b":[1,2]}}  "#));
        assert!(!is_complete_json_object(r#"{"plugins":[{"name":"Zeb"#), "truncated");
        assert!(!is_complete_json_object(r#"{"plugins":[]}}"#), "one too many");
        assert!(!is_complete_json_object(r#"[1,2]"#), "an array is not the document");
        assert!(!is_complete_json_object(""), "empty");
        // Braces and brackets INSIDE strings must not count, or a plugin called
        // "Bass{" makes a complete file look truncated. Nobody would find that.
        assert!(is_complete_json_object(r#"{"name":"Bass{ [ }"}"#));
        // ...nor must an escaped quote end the string early.
        assert!(is_complete_json_object(r#"{"name":"say \" {"}"#));

        // A missing file explains itself rather than returning an empty list,
        // which would read as "you have no plugins".
        let miss = list_plugins("/nonexistent/plugin_cache.json");
        assert!(miss.contains("\"error\""), "{miss}");
        assert!(miss.contains("the engine writes it when it scans"), "{miss}");

        // A good one is passed through verbatim, under a key the client reads.
        let dir = std::env::temp_dir().join("uni-sidecar-test-plugincache");
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let good = dir.join("plugin_cache.json");
        std::fs::write(&good, r#"{"schema_version":1,"plugins":[{"name":"A \"quoted\" one"}]}"#).unwrap();
        let out = list_plugins(good.to_str().unwrap());
        assert!(out.starts_with("{\"ok\":true,\"pluginCache\":{"), "{out}");
        // Verbatim means the awkward name survives: re-encoding it by hand is
        // exactly the step this avoids.
        assert!(out.contains(r#"A \"quoted\" one"#), "{out}");
        let _ = std::fs::remove_dir_all(&dir);
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
