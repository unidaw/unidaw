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

// The agent loop: a typed sentence to a sequence of tool calls. Its own module
// because it is the only part of this binary that talks to the network.
mod ask;

use std::net::{TcpListener, TcpStream};
use std::sync::atomic::{AtomicU32, AtomicU64, Ordering};
use std::collections::VecDeque;
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

use daw_bridge::control::{default_shm_name, EngineHandle};
use daw_bridge::layout::{UiSetRowOpsPayload, UiSamplerLoadPayload, UiSamplerSlicePayload,
                         UiSamplerFilterPayload, UiSamplerEnvelopePayload,
                         UiSamplerSetSlotPayload, UiSamplerSetDevicePayload, EventEntry, UiChainCommandPayload, UiChordCommandPayload,
                         UiMarkerCommandPayload, UiArrangeTimeCommandPayload,
                         UI_TIME_SIG_FLATTEN, UiModLinkCommandPayload,
                         UiModLinkUid16Payload, UiModSourceValuePayload,
                         UiAutomationLaneRequestPayload, UiAutomationPointPayload,
                         MOD_SOURCE_MACRO, MOD_SOURCE_LFO, MOD_SOURCE_ENVELOPE,
                         MOD_SOURCE_PATCHER_NODE_OUTPUT, MOD_TARGET_VST_PARAM,
                         MOD_TARGET_PATCHER_PARAM, MOD_TARGET_PATCHER_MACRO,
                         MOD_RATE_BLOCK, MOD_RATE_SAMPLE, MOD_LINK_ID_AUTO,
                        UiCommandPayload, UiCommandType,
                        UiDiffType, UiPatcherGraphCommandPayload, UiPatcherNodeConfigPayload,
                        UiPatcherPresetCommandPayload, UiSetParamPayload,
                        UiTrackRoutingPayload,
                        UiWaveformRequestPayload, K_UI_WAVEFORM_SLOTS, K_CHAIN_DEVICE_ID_AUTO,
                        K_CHAIN_TRACK_ALL, K_HOST_SLOT_DIRECT};
use daw_bridge::grid::{aggregate_rows, LaneGrid};

/// Wire format, little-endian. The frontend decodes with a DataView.
/// Bump `WIRE_VERSION` here and in `ui-web/src/wire.js` together.
/// How many LANES the frame carries per-track data for.
///
/// One constant, because it governs three things that must agree: the width of the
/// `lpb` block, how many tracks' notes and chords are read, and what the page can
/// draw. They did NOT agree — the note loop capped at a hard-coded 8 while `lpb`
/// was 16 — so every note and chord on track 8 and above was silently dropped from
/// the feed. The notes were in the engine and in the saved file; they simply never
/// reached the browser, so they could not be seen, edited or played from the
/// tracker. No fixture has more than six tracks, which is exactly why nothing
/// caught it: the ninth track a person adds is where it starts.
/// Sixty-four — `kUiMaxTracks`, so the wire carries every track the engine can hold.
///
/// Was 8, then 16. Each widening had the same cause and the same symptom: a lane past the end
/// had no grid on the client and silently fell back to the zoom's, which is a wrong grid that
/// looks like a choice. At 16 it was invisible because the UI capped tracks at 16 — the cap was
/// hiding the truncation, and the cap is what this change removes.
const WIRE_LANES: usize = 64;

const WIRE_MAGIC: u32 = 0x31_49_4e_55; // "UNI1"
const WIRE_VERSION: u16 = 26;

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
const FULL_HEADER_BYTES: usize = 232;
/// Bytes per note on the BROWSER wire — `NOTE_BYTES` in ui-web/src/wire.js.
///
/// NOT the engine's `UiClipNote` stride, which is 48 at kShmVersion 32 and comes from the typed
/// struct rather than a constant. This one said 40 while wire.js said 44, was `allow(dead_code)`
/// and therefore checked by nothing, and documented a layout that had moved on without it — a
/// comment that lies is worse than no comment. It is asserted against the packer below now.
const NOTE_BYTES: usize = 50;

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
    /// v32 THE SOUND ADDRESS: which sampler slot this note plays. 0 = the keymap picks it from
    /// pitch, which is every row on an ordinary kit track — so a UI draws 0 as EMPTY, not "0".
    sound: u16,
    /// v32 the 9xx seek, as a FRACTION of the slot's extent. Absolute frames break when the
    /// slot's sample is swapped or its slice re-cut; a fraction survives both.
    sound_offset: u16,
    /// v33 RETRIGGER VOLUME RAMP: signed TOTAL percent across a retrigger's strikes. Means
    /// nothing without `retrigger`, and 0 is flat — which is what every note written before this
    /// carries, so nothing changes shape.
    retrig_ramp: i8,
    /// v33 CONDITIONAL TRIG, packed A:B. 0 = unconditional. Deterministic in the transport's
    /// pass rather than random, which is what separates it from `probability`.
    trig_condition: u8,
    /// Row index under the client's current grid, computed by LaneGrid here so
    /// the frontend never re-derives the projection.
    row: u32,
    /// v26 lane quantize: how far this note's lane MOVES it, in nanoticks, signed.
    /// 0 when the lane is not quantized.
    ///
    /// The SOUNDING tick is `t_on + dev_nanoticks + delay_nanoticks` — quantize and
    /// the note's own delay COMPOSE; the scheduler quantizes the note start on its
    /// scheduling copy and expandNoteOps adds the delay afterwards. So a cell draws
    /// ONE mark, from where the note is written to where it is heard, and not two
    /// competing ones.
    ///
    /// Published by the ENGINE rather than derived here or in the browser. A second
    /// implementation of quantizeTick would rest on two integer divisions that
    /// truncate toward zero, where the natural JS spelling rounds toward negative
    /// infinity — off by one tick on precisely the notes played late, silently,
    /// with the bar still moving and still pointing the right way.
    dev_nanoticks: i32,
}

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
    /**
     * Per-track lines-per-beat, for as many tracks as the PAGE can draw.
     *
     * Sixteen, not the engine's kUiMaxTracks — which is 64 as of kShmVersion 21
     * and will grow again. The renderer caps at 16 lanes and clamps
     * `state.tracks` to min(trackCount, 16), so sending 64 would be 48 bytes a
     * frame for lanes nobody draws; sending 8, which is what this was until v21
     * widened the engine's array, silently left lanes 8-15 with no grid at all
     * and they fell back to the zoom's own — a wrong grid that looks like a
     * choice.
     *
     * TIED TO THE RENDERER'S LANE CAP. Widen the cap and this widens with it, or
     * the same silence comes back one lane further out.
     */
    /// `#[serde]`-free, but note the `#[derive(Default)]` on Frame cannot cover this: Rust
    /// implements Default for `[T; N]` only up to N = 32, and this is 64. Hence the attribute
    /// below, which defaults it by function instead.
    #[allow(dead_code)]
    lpb: [u8; WIRE_LANES],
    /// A track's CHORDS, which the engine has always published and this side has
    /// never forwarded.
    ///
    /// They are not notes: a chord is (degree, quality, inversion) resolved
    /// against the harmony timeline, which is what lets a chord track survive a
    /// key change. The sidecar could WRITE one — `build_chord` below — and never
    /// read one back, so a track of chords played and showed nothing at all.
    /// Reported twice as "sound with no notes".
    ///
    /// (tick, duration, chord_id, track, degree, quality, inversion, base_octave,
    /// flags, row)
    ///
    /// `row` is computed HERE by the same LaneGrid the notes use. The frontend
    /// never re-derives the projection — one axis on the wire — because a lane's
    /// grid is the engine's business and a client that computed its own put
    /// notes three beats out with no error anywhere.
    chords: Vec<(u64, u64, u32, u8, u8, u8, u8, u8, u32, u32)>,
    /// v24 per-insert meters, as (track ID, device id, inPeak, outPeak, inRms,
    /// outRms) in dBFS millibels. Only PRESENT entries — the region is 64x16 and
    /// almost all of it is "no device". kUiMeterSilent (i16::MIN) means silent or
    /// below the floor and is a real value, not a hole: an instrument has no
    /// audio input and honestly reports its input as silent forever.
    ///
    /// The track ID, NOT the slot the engine indexes the region by. The page keys
    /// everything on ids and the two diverge the moment a track is removed — a
    /// tombstoned slot keeps its position while its id retires. Translating here,
    /// once, beats every reader doing it: slot-versus-id is the same shape of bug
    /// as position-versus-id, which is the one this record already carries a
    /// device id to avoid.
    meters: Vec<(u32, u32, i16, i16, i16, i16)>,
    /// v26 per-lane NON-DESTRUCTIVE quantize, as (track id, grid nanoticks,
    /// strength thousandths, swing thousandths). Only lanes with a grid set — an
    /// unquantized lane is the overwhelming majority and has nothing to say.
    ///
    /// Swing is PLAIN SIGNED. The command carries it biased by +500 because that
    /// payload field is unsigned; the read-back does not. Biasing both legs is an
    /// off-by-500 that would show a groove nobody asked for.
    quantize: Vec<(u32, u64, u32, i32)>,
    /// Moves ONLY when a lane's quantize changes — never when a note does. Backend
    /// kept it off the clip version on purpose: quantize moves no authored note, so
    /// it must not invalidate anyone's in-flight edit. The page caches on it.
    quantize_version: u32,
    /// v27 arrangement SECTIONS, already resolved by the engine: (id, start_bar,
    /// The song's MARKERS: (id, bar, beat, color, nanotick, name).
    ///
    /// A SPAN IS TWO ADJACENT MARKERS. There is no stored length any more — sections were
    /// retired in v29 and a marker is a named tick — so a strip's width comes from the NEXT
    /// marker's position, or from `song_end_tick` for the last one. That derivation is
    /// subtraction and belongs wherever it is drawn; what would NOT belong there is deriving
    /// the bar number.
    ///
    /// `bar` and `beat` ARE RESOLVED BY THE ENGINE, and now it matters. Bar numbering across a
    /// meter change is a prefix sum through the map, not `tick / barLength` — so a client that
    /// computed it would be right until the first 7/8 passage and then quietly wrong, with
    /// markers sitting between ruler numbers that do not match them. One derivation, in the
    /// engine.
    markers: Vec<(u32, u32, u32, u32, u64, [u8; 24])>,
    /// The arrange region's OWN generation. Moves on a spine or meter change and NEVER on
    /// a note edit, so the page can cache the spine on it and keep the cache through
    /// typing. Forwarded so the client can do exactly that.
    arrange_version: u32,
    /// How many sections did NOT fit, and the furthest placement END.
    ///
    /// Truncation is forwarded rather than dropped because a short list that says nothing
    /// reads as a complete one — which turns "the arrangement is missing sections" into a
    /// bug report about the view. `song_end` is NOT the end of the last section: material
    /// can sit past the spine, and it plays and is unnamed.
    markers_truncated: u32,
    song_end_tick: u64,
    /// The audio device's block size in frames and its rate in Hz — the two facts the
    /// latency readout is made of. Zero until the engine has opened a device, which is
    /// distinct from "a block of zero": the chrome draws nothing rather than "0.0ms".
    block_size: u32,
    sample_rate_hz: u32,
    /// One entry per automated parameter: (track, targetPluginIndex, pointCount, flags, paramId).
    /// The LIST, so a lane is discoverable; the curve is fetched per lane on request.
    automation: Vec<(u32, u32, u32, u32, String)>,
    automation_version: u32,
    /// THE SAMPLER KIT'S VERSION, so a drawn kit can be a subscription rather than a snapshot.
    ///
    /// Bumped when a kit CHANGES, not when one is requested — the distinction is the whole
    /// value, because "did anyone ask recently" is not the client's question and a counter that
    /// ticked on request would make a polling loop re-fetch for ever while looking correct.
    ///
    /// ZERO means the engine does not publish one. The counter starts at 1, so an old engine is
    /// distinguishable from an unchanged kit without a version to key on.
    sampler_kit_version: u32,
    automation_truncated: u32,
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
    /// The SONG's time signature (kShmVersion 19). Bar NUMBERING is global — the
    /// time gutter and the arrangement ruler count the song's bars — while a clip
    /// may run an entirely different meter inside one of them. So this is not the
    /// same quantity as the per-clip grid packed into `UiClipExtent.flags`, and
    /// the two are not interchangeable: this is the one you count in.
    ///
    /// u16 apiece because a numerator of 5 does not need 32 bits and the header is
    /// read by a DataView with hand-written offsets, where four spare bytes are
    /// four more chances to mislay a field.
    song_time_sig_num: u16,
    song_time_sig_den: u16,
    /// v20: (parent_id, flags) per track. parent_id 0 = top-level; flags bit0 =
    /// collapsed. A child is an ORDINARY track in every other array — collapse is a
    /// drawing decision and never changes what exists, so a client that ignores
    /// both still renders the whole project.
    track_parent: Vec<(u32, u8)>,
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

impl Default for Frame {
  /// Hand-written because `[u8; 64]` has no `Default` — Rust stops deriving it for
  /// arrays at 32 elements, and the lane block is 64 now (`kUiMaxTracks`).
  fn default() -> Self {
    Self {
      version: Default::default(),
      seq: Default::default(),
      playhead_nanotick: Default::default(),
      visual_sample: Default::default(),
      transport: Default::default(),
      track_count: Default::default(),
      clip_version: Default::default(),
      harmony_version: Default::default(),
      window_start: Default::default(),
      window_end: Default::default(),
      peaks: Default::default(),
      notes: Default::default(),
      aggs: Default::default(),
      agg_rows: Default::default(),
      agg_tracks: Default::default(),
      lpb: [0; WIRE_LANES],
      chords: Default::default(),
      meters: Default::default(),
      quantize: Default::default(),
      quantize_version: Default::default(),
      markers: Default::default(),
      arrange_version: Default::default(),
      markers_truncated: Default::default(),
      song_end_tick: Default::default(),
      block_size: Default::default(),
      sample_rate_hz: Default::default(),
      automation: Default::default(),
      automation_version: Default::default(),
      sampler_kit_version: Default::default(),
      automation_truncated: Default::default(),
      extents: Default::default(),
      notes_grid: Default::default(),
      stale: Default::default(),
      mixer: Default::default(),
      mixer_version: Default::default(),
      loop_start: Default::default(),
      loop_end: Default::default(),
      load_seq: Default::default(),
      load_ok: Default::default(),
      tempo_milli_bpm: Default::default(),
      tempo_point_count: Default::default(),
      song_time_sig_num: Default::default(),
      song_time_sig_den: Default::default(),
      track_parent: Default::default(),
      harmony: Default::default(),
      harmony_read_at: Default::default(),
      harmony_ever_read: Default::default(),
      names: Default::default(),
      patcher_version: Default::default(),
      patcher_device: Default::default(),
      patcher_nodes: Default::default(),
      patcher_edges: Default::default(),
      ev: Default::default(),
    }
  }
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
    // lpb is 16 bytes now, not 8 (kShmVersion 21 widened the engine's array to 64
    // and the page draws 16 lanes), so every offset from here on moved by 8.
    out.extend_from_slice(&f.mixer_version.to_le_bytes());           // 76
    out.extend_from_slice(&(f.mixer.len() as u16).to_le_bytes());    // 80
    // 82 was a pad; the harmony count took it rather than being appended after
    // it, which is what a two-byte shift of everything downstream looks like
    // when you get it wrong — names decode empty and every pitch reads 0.
    out.extend_from_slice(&(f.harmony.len() as u16).to_le_bytes());  // 82
    out.extend_from_slice(&(f.names.len() as u16).to_le_bytes());    // 84
    out.extend_from_slice(&f.patcher_version.to_le_bytes());         // 86
    out.extend_from_slice(&f.patcher_device.to_le_bytes());          // 90
    out.extend_from_slice(&(f.patcher_nodes.len() as u16).to_le_bytes());  // 94
    out.extend_from_slice(&(f.patcher_edges.len() as u16).to_le_bytes());  // 96
    // 98 was a pad. It holds the CHORD COUNT now — a count is exactly what a
    // spare two bytes in a header is for, and taking it moves nothing.
    out.extend_from_slice(&(f.chords.len() as u16).to_le_bytes());   // 98
    // Checkpoints, not just a total. The trailing comments are the map and a map
    // cannot be verified — when the lpb block widened from 8 to 16 every offset
    // after it moved, and a careless renumber left the COMMENTS scrambled
    // (loop_start said 132, load_ok said 128) while the writes stayed correct.
    // Nothing caught it: the final length still came to 136 and the drift test
    // compares two totals. Two fields SWAPPED would survive both the same way, and
    // the page would read them swapped with no error anywhere.
    debug_assert_eq!(out.len(), 100, "loop range starts at 100");
    out.extend_from_slice(&f.loop_start.to_le_bytes());              // 100
    out.extend_from_slice(&f.loop_end.to_le_bytes());                // 108
    out.extend_from_slice(&f.load_seq.to_le_bytes());                // 116
    out.extend_from_slice(&f.load_ok.to_le_bytes());                 // 120
    debug_assert_eq!(out.len(), 124, "tempo starts at 124");
    out.extend_from_slice(&f.tempo_milli_bpm.to_le_bytes());         // 124
    out.extend_from_slice(&f.tempo_point_count.to_le_bytes());       // 128
    debug_assert_eq!(out.len(), 132, "the song meter starts at 132");
    out.extend_from_slice(&f.song_time_sig_num.to_le_bytes());       // 132
    out.extend_from_slice(&f.song_time_sig_den.to_le_bytes());       // 134
    /*
     * v24 PER-INSERT METERS (wire 16). A count and a pad, appended rather than
     * taking one of the header's spare bytes, because there were none left — 98
     * was the last and the chord count has it.
     *
     * Only the entries the engine says are PRESENT are sent. The region is
     * 64 tracks x 16 inserts and almost all of it is "no device"; publishing the
     * whole grid at 120 Hz would be 12 KB a frame to say nothing.
     */
    out.extend_from_slice(&(f.meters.len() as u16).to_le_bytes());   // 136
    out.extend_from_slice(&0u16.to_le_bytes());                      // 138
    /*
     * v26 LANE QUANTIZE (wire 17). Its own version, because it moves when a lane's
     * setting changes and NOT when notes do — the page caches the deviation layer
     * on it, and keying that on the clip version would rebuild it on every
     * keystroke while missing the one change it cares about.
     */
    out.extend_from_slice(&f.quantize_version.to_le_bytes());        // 140
    out.extend_from_slice(&(f.quantize.len() as u16).to_le_bytes()); // 144
    out.extend_from_slice(&0u16.to_le_bytes());                      // 146
    /*
     * v27 ARRANGEMENT SPINE (wire 19). Count, truncation, the region's own generation,
     * and the song end.
     *
     * The GENERATION is on the wire rather than being inferred, because it is what the
     * page caches the spine on: it moves when the spine or the meter changes and never
     * when a note does, so the cache survives typing and cannot go stale on a rename.
     * Deriving "did this change" by comparing the list would be work done to always
     * answer yes.
     *
     * TRUNCATION travels too. A short list that says nothing reads as a complete one,
     * which turns "the arrangement is missing sections" into a bug report about the view.
     */
    out.extend_from_slice(&(f.markers.len() as u16).to_le_bytes()); // 148
    out.extend_from_slice(&(f.markers_truncated.min(0xffff) as u16).to_le_bytes()); // 150
    out.extend_from_slice(&f.arrange_version.to_le_bytes());         // 152
    out.extend_from_slice(&f.song_end_tick.to_le_bytes());           // 156, to 164
    /*
     * THE AUDIO DEVICE'S BLOCK AND RATE, so the chrome can say what the latency IS.
     *
     * Derived rather than published: `blockSize / sampleRate` is the output latency in
     * seconds and there is no second answer to it, so sending the two facts is better than
     * sending a computed millisecond figure — a person tuning the buffer wants to see the
     * block size itself, and anything else that needs the rate has it.
     *
     * The rate as an INTEGER Hz. It is a double in the header and 44100 or 48000 in
     * practice; four bytes of float precision on a value that is always an integer would
     * cost the same and invite a readout that says 47999.9.
     */
    out.extend_from_slice(&f.block_size.to_le_bytes());               // 164
    out.extend_from_slice(&f.sample_rate_hz.to_le_bytes());           // 168
    /*
     * WHICH PARAMETERS ARE AUTOMATED. The LIST, not the curves.
     *
     * A standing list is what makes automation DISCOVERABLE — "cutoff is automated on track 3"
     * — and it is small: 32 bytes a lane against a curve's 512 points. The points are fetched
     * per lane on request, because a song can hold far more automation than a frame should
     * carry and the interface only ever draws the lanes that are open.
     *
     * Its own version, from the engine, so a client caches on automation changing rather than
     * on anything else moving.
     */
    out.extend_from_slice(&(f.automation.len() as u16).to_le_bytes());   // 172
    out.extend_from_slice(&(f.automation_truncated.min(0xffff) as u16).to_le_bytes()); // 174
    out.extend_from_slice(&f.automation_version.to_le_bytes());       // 176, to 180
    // 180..184. Same bargain as every field before it: both sides move together and
    // WIRE_VERSION goes with them, so a page reading 180 where this writes 184 rejects the
    // frame rather than decoding a harmony tick out of the middle of a counter.
    out.extend_from_slice(&f.sampler_kit_version.to_le_bytes());      // 180, to 184
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
        // NOTE_BYTES is asserted per note rather than declared and forgotten. It sat at 40 while
        // the packer wrote 44, `allow(dead_code)`, checked by nothing — the exact shape this
        // project keeps writing down: an invariant that depends on remembering breaks on the
        // sixth occasion, so write the check.
        let note_start = out.len();
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
        out.extend_from_slice(&n.delay_nanoticks.to_le_bytes());   // 32
        out.extend_from_slice(&n.row.to_le_bytes());                // 36
        // 40..44. THE STRIDE GREW, and it is load-bearing for everything after
        // the notes — extents, aggregates, the track structure, chords, meters
        // and quantize all follow. Growing it on one side only is what made the
        // extents decode as garbage once before, so both sides move together and
        // WIRE_VERSION is bumped: a page reading 40 where the sidecar writes 44
        // rejects the frame outright instead of rendering nonsense.
        out.extend_from_slice(&n.dev_nanoticks.to_le_bytes());      // 40, to 44
        // 44..48, v32. Same bargain as the 40->44 growth above: both sides move together and
        // WIRE_VERSION goes with them, so a page reading 44 where this writes 48 REJECTS the
        // frame rather than decoding every extent after the notes as garbage.
        out.extend_from_slice(&n.sound.to_le_bytes());              // 44
        out.extend_from_slice(&n.sound_offset.to_le_bytes());       // 46, to 48
        // 48..50, v33. Same bargain again: a page reading 48 where this writes 50 REJECTS the
        // frame on WIRE_VERSION rather than decoding every extent after the notes as garbage.
        out.push(n.retrig_ramp as u8);                              // 48
        out.push(n.trig_condition);                                 // 49, to 50
        debug_assert_eq!(out.len() - note_start, NOTE_BYTES,
                         "note stride drifted from NOTE_BYTES / wire.js");
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
    // v20 child-track structure.
    //
    // Appended rather than folded into the mixer's 12-byte record, because that
    // record's stride is load-bearing for everything after it and GUIDELINES 2.3
    // is a list of the two times widening one shifted the whole tail. Counted by
    // `track_count`, which the header already carries.
    for &(parent, flags) in &f.track_parent {
        out.extend_from_slice(&parent.to_le_bytes());   // 0
        out.push(flags);                                // 4
        out.push(0); out.push(0); out.push(0);          // 8 bytes each
    }

    /*
     * CHORDS, and now these are last.
     *
     * AFTER the track structure, not before it, so no existing section moves at
     * all — a section inserted ahead of another shifts it, which is the whole
     * reason the note above says what it says. This is the first thing appended
     * since, and the "nothing follows this" line moved with the position rather
     * than being left behind as a claim that had quietly stopped being true.
     *
     * 40 bytes each, counted by the u16 at header offset 98, which was a pad.
     *
     * The engine has always published a track's chords and this side never read
     * them, so a track of chords played and showed nothing at all — reported
     * twice as "sound with no notes". A chord is not a note: it is (degree,
     * quality, inversion) resolved against the harmony timeline, which is what
     * lets a chord track survive a key change.
     */
    for &(tick, dur, id, track, degree, quality, inversion, octave, flags, row)
        in &f.chords
    {
        out.extend_from_slice(&tick.to_le_bytes());     // 0
        out.extend_from_slice(&dur.to_le_bytes());      // 8
        out.extend_from_slice(&id.to_le_bytes());       // 16
        out.push(track); out.push(degree); out.push(quality); out.push(inversion); // 20
        out.push(octave); out.push(0); out.push(0); out.push(0);                   // 24
        out.extend_from_slice(&flags.to_le_bytes());    // 28
        out.extend_from_slice(&row.to_le_bytes());      // 32
        out.extend_from_slice(&0u32.to_le_bytes());     // 36, to 40
    }

    /*
     * PER-INSERT METERS, and now THESE are last.
     *
     * Appended after the chords for the reason the chords were appended after the
     * track structure: a section inserted ahead of another shifts it, and this
     * file's history is a list of the times that happened. The "nothing follows
     * this" line moves with the position rather than being left behind as a claim
     * that has quietly stopped being true.
     *
     * 16 bytes each, counted by the u16 at header offset 136.
     *
     * MATCHED ON deviceId, never on position: the engine's compacted insert order
     * skips patcher devices, so the Nth meter is not the Nth card. Backend and I
     * agreed the id travels with the meter for exactly this reason.
     */
    for &(track, device, in_peak, out_peak, in_rms, out_rms) in &f.meters {
        out.extend_from_slice(&track.to_le_bytes());      // 0, the track ID
        out.extend_from_slice(&device.to_le_bytes());     // 4
        out.extend_from_slice(&in_peak.to_le_bytes());    // 8
        out.extend_from_slice(&out_peak.to_le_bytes());   // 10
        out.extend_from_slice(&in_rms.to_le_bytes());     // 12
        out.extend_from_slice(&out_rms.to_le_bytes());    // 14, to 16
    }

    /*
     * PER-LANE QUANTIZE, and now THESE are last.
     *
     * Appended after the meters for the reason the meters were appended after the
     * chords and the chords after the track structure: a section inserted ahead of
     * another shifts it, and this file's history is a list of the times that
     * happened. The "nothing follows this" line moves with the position rather
     * than being left behind as a claim that quietly stopped being true.
     *
     * 24 bytes each, counted by the u16 at header offset 144. The grid is a u64
     * because it is a tick count, not a subdivision — a lane can quantize to
     * something its display grid does not show.
     */
    for &(track, grid, strength, swing) in &f.quantize {
        out.extend_from_slice(&track.to_le_bytes());      // 0
        out.extend_from_slice(&strength.to_le_bytes());   // 4
        out.extend_from_slice(&swing.to_le_bytes());      // 8
        out.extend_from_slice(&grid.to_le_bytes());       // 12
        out.extend_from_slice(&0u32.to_le_bytes());       // 20, to 24
    }

    /*
     * THE SECTION SPINE, and now THIS is last.
     *
     * Appended after the quantize block for the reason every section in this function is
     * appended after the one before it: a section inserted ahead of another shifts it, and
     * this file's history is a list of the times that happened. The "nothing follows this"
     * line moves with the position rather than being left as a claim that stopped being
     * true.
     *
     * 56 bytes each, counted by the u16 at header offset 148. Both ticks are u64 because a
     * section's position is a tick count, and both are ALREADY RESOLVED by the engine —
     * the bar number too. Nothing here derives a position.
     */
    for &(id, bar, beat, color, nanotick, name) in &f.markers {
        out.extend_from_slice(&id.to_le_bytes());          // 0
        out.extend_from_slice(&bar.to_le_bytes());         // 4
        out.extend_from_slice(&beat.to_le_bytes());        // 8
        out.extend_from_slice(&color.to_le_bytes());       // 12
        out.extend_from_slice(&nanotick.to_le_bytes());    // 16
        out.extend_from_slice(&0u64.to_le_bytes());        // 24  reserved
        out.extend_from_slice(&name);                      // 32, to 56
    }

    /*
     * ...AND THE AUTOMATION LANES, after the sections, counted by the u16 at 172.
     *
     * 32 bytes each: track u32, target u32, points u32, flags u32, paramId 16. LAST for the
     * reason the sections are second-to-last — a variable-length block cannot move a fixed
     * field, and a reader that stops early gets a short list rather than garbage.
     *
     * `paramId` is the STRING the engine keys an automation clip on, not a uid16 — it is what
     * WriteAutomationPoint addresses and what a person sees. Nul-padded to 16 like every other
     * name on this wire.
     */
    for (track, target, points, flags, param) in &f.automation {
        out.extend_from_slice(&track.to_le_bytes());        // 0
        out.extend_from_slice(&target.to_le_bytes());       // 4
        out.extend_from_slice(&points.to_le_bytes());       // 8
        out.extend_from_slice(&flags.to_le_bytes());        // 12
        let b = param.as_bytes();
        let take = b.len().min(16);
        out.extend_from_slice(&b[..take]);
        out.extend_from_slice(&vec![0u8; 16 - take]);       // 16, to 32
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
    /*
     * LAST FRAME'S QUANTIZE VERSION, captured before anything overwrites it.
     *
     * The notes region is re-read only when the clip version moves — and
     * SetLaneQuantize deliberately does NOT move it, because quantize invalidates
     * nobody's edit. So a lane's per-note deviations sat at their old values until
     * some unrelated note edit happened to force a rebuild: the bars would have
     * been right by accident and stale the rest of the time, which is worse than
     * wrong because wrong gets reported.
     *
     * This is the same bug backend found and fixed one layer down, in the engine's
     * own gate for the same region, for the same reason. `uiQuantizeVersion` exists
     * precisely to be the second half of this condition.
     */
    let prev_quantize_version = out.quantize_version;
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

    /*
     * v24 PER-INSERT METERS, every frame, like the track peaks beside them.
     *
     * Every published SLOT, master included: the master occupies a real slot with
     * kUiTrackFlagMaster, so metering it needs no second path — which was the
     * first thing I asked backend to guarantee, because a master with no
     * per-insert meters is the one chain where gain staging matters most.
     *
     * `read_device_meters` skips kUiMeterNoDevice itself, so what arrives is only
     * what is really there. It is bounded by 16 inserts a track and in practice a
     * handful; the whole 64x16 grid would be 12 KB a frame to say nothing.
     */
    /*
     * PER-LANE QUANTIZE. Read every frame like the meters, and cheap for the same
     * reason: only lanes that HAVE a grid are forwarded, and almost none do.
     */
    out.quantize.clear();
    {
        let (lanes, version) = h.read_track_quantize();
        out.quantize_version = version;
        let (ids, _flags) = h.read_track_ids_and_flags();
        for (slot, &(grid, strength, swing)) in lanes.iter().enumerate() {
            if grid == 0 || slot >= ids.len() { continue; }
            out.quantize.push((ids[slot], grid, strength, swing));
        }
    }

    /*
     * THE ARRANGEMENT SPINE, gated on the region's OWN generation.
     *
     * `read_arrange_summary` does the version-body-version read; a generation of 0 means a
     * write is in flight and the bridge returns nothing, so a torn spine — some sections
     * from before an edit and some from after — cannot reach the page. Backend's first
     * guard here did not work, because the version only changed AFTER the body was
     * written: a reader that sampled, read, and sampled again before the stamp saw the two
     * agree and accepted garbage. Worth remembering as the shape rather than the instance.
     *
     * Re-read only when the generation MOVES. It moves on a spine or meter change and never
     * on a note edit, so this costs one integer compare per frame while somebody is typing.
     */
    if let Some(sum) = h.read_arrange_summary() {
        // `version == 0` means A WRITE IS IN FLIGHT. Retry — never "empty": the counter only
        // moves after the body is written, so version-body-version equality is not torn-safe on
        // its own, and holding the previous list for a frame is exactly right.
        if sum.version != 0 && sum.version != out.arrange_version {
            out.arrange_version = sum.version;
            out.markers_truncated = sum.markers_truncated;
            out.song_end_tick = sum.song_end_tick;
            out.markers.clear();
            let n = (sum.marker_count as usize).min(sum.markers.len());
            for m in &sum.markers[..n] {
                out.markers.push((m.id, m.bar, m.beat, m.color_rgb, m.nanotick, m.name));
            }
        }
    }

    out.meters.clear();
    {
        // v22's stable per-slot ids, which is what turns a slot into something the
        // page can match on. Read once for the whole loop rather than per slot.
        let (ids, _flags) = h.read_track_ids_and_flags();
        for slot in 0..ids.len().min(daw_bridge::layout::K_UI_MAX_TRACKS) {
            let metered = h.read_device_meters(slot);
            if metered.is_empty() { continue; }
            for (device, in_peak, out_peak, in_rms, out_rms) in metered {
                out.meters.push((ids[slot], device, in_peak, out_peak, in_rms, out_rms));
            }
        }
    }

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
    // The first 16 of the engine's 64, which is what the page can draw.
    out.lpb.copy_from_slice(&snap.ui_lines_per_beat[..WIRE_LANES]);

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

    /*
     * The automation LANES, read every frame and gated on the engine's own version.
     *
     * `version == 0` means A WRITE IS IN FLIGHT — backend's note, and it is the arrange
     * summary's trap over again: the counter only moves after the body is written, so
     * version-body-version equality is not torn-safe by itself. Zero is a retry, never "empty",
     * and holding the previous list for a frame is exactly right — automation does not change
     * between two frames unless somebody edited it.
     */
    /*
     * The kit's version, read every frame and forwarded raw.
     *
     * No caching and no comparison here: it is one word, the client is the one that decides
     * whether it means anything, and a sidecar that decided for it would be a second opinion
     * about staleness — which is the shape that lets two sides disagree about whether a drawn
     * thing is still true.
     */
    out.sampler_kit_version = h.sampler_kit_version();

    {
        let lanes = h.read_automation_lanes();
        if lanes.version != 0 && lanes.version != out.automation_version {
            out.automation_version = lanes.version;
            out.automation_truncated = lanes.truncated;
            out.automation.clear();
            for l in &lanes.lanes {
                // `discrete` is a bool on this side and a FLAG BIT on the wire, packed back
                // into bit 0 — the client draws a stepped curve for a discrete clip and a
                // ramped one otherwise, and a lane that draws the wrong shape for half its
                // curves is worse than one that draws none.
                out.automation.push((l.track_id, l.target_plugin_index, l.point_count,
                                     if l.discrete { 1 } else { 0 }, l.param_id.clone()));
            }
        }
    }

    let (bs, sr) = h.device_block();
    out.block_size = bs;
    out.sample_rate_hz = sr;

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
    // Clamped into the u16 the frame carries, and defaulted rather than zeroed: a
    // denominator of 0 divides by zero in every bar computation downstream, and an
    // engine that has not published a meter yet is the normal state during the
    // first frames after a load, not an error worth propagating as one.
    out.song_time_sig_num = if snap.ui_song_time_sig_num == 0 { 4 }
                            else { snap.ui_song_time_sig_num.min(u16::MAX as u32) as u16 };
    out.song_time_sig_den = if snap.ui_song_time_sig_den == 0 { 4 }
                            else { snap.ui_song_time_sig_den.min(u16::MAX as u32) as u16 };
    out.track_parent.clear();
    for t in 0..(snap.ui_track_count as usize).min(snap.ui_track_parent_id.len()) {
        out.track_parent.push((snap.ui_track_parent_id[t], snap.ui_track_flags[t]));
    }

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
        || out.quantize_version != prev_quantize_version
    {
        out.notes_grid = vp.lines_per_beat;
        out.notes.clear();
        // Gathered on the same trigger as the notes, from the same snapshot. A
        // chord and a note both move when the clip version does, so a separate
        // guard would only be a second thing to get wrong.
        out.chords.clear();
        out.window_start = 0;
        out.window_end = 0;
        for track in 0..(snap.ui_track_count.min(WIRE_LANES as u32)) {
            let Some(w) = h.read_track_clip(track) else { continue };
            if track == 0 {
                out.window_start = w.window_start_nanotick;
                out.window_end = w.window_end_nanotick;
            }
            let chord_count = (w.chord_count as usize).min(w.chords.len());
            for c in &w.chords[..chord_count] {
                out.chords.push((c.nanotick, c.duration_nanoticks, c.chord_id,
                                 track as u8, c.degree, c.quality, c.inversion,
                                 c.base_octave, c.flags,
                                 vp_grid.row_of_tick(c.nanotick) as u32));
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
                    dev_nanoticks: note.dev_nanoticks,
                    sound: note.sound,
                    sound_offset: note.sound_offset,
                    retrig_ramp: note.retrig_ramp,
                    trig_condition: note.trig_condition,
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
/// Where a KEY's value begins, or None.
///
/// `"depth"` APPEARS AS A VALUE TOO. `{"op":"depth","depth":0.25}` contains the needle `"depth"`
/// twice, and the first hit is inside `"op":"depth"` — so a scan that took the first one read
/// the number after it, which is whatever field comes next. Every depth arrived as 0, the
/// modulation multiplied by nothing, and the capture was silence: a parser bug that presented
/// as an audio bug.
///
/// So a key is only a key when a COLON follows it. Whitespace between is allowed, because JSON
/// permits it and `JSON.stringify` not producing any is a property of one caller rather than of
/// the format.
fn value_at(body: &str, key: &str) -> Option<usize> {
    let mut from = 0;
    while let Some(rel) = body[from..].find(key) {
        let after = from + rel + key.len();
        let rest = &body[after..];
        let trimmed = rest.trim_start();
        if trimmed.starts_with(':') {
            // Past the colon, so the value scan cannot mistake the colon for punctuation.
            let skipped = rest.len() - trimmed.len();
            return Some(after + skipped + 1);
        }
        from = after;
    }
    None
}

fn parse_num(body: &str, key: &str) -> Option<i64> {
    let i = value_at(body, key)?;
    let rest = &body[i..];
    let start = rest.find(|c: char| c.is_ascii_digit() || c == '-')?;
    let end = rest[start..].find(|c: char| !c.is_ascii_digit() && c != '-').unwrap_or(rest.len() - start);
    rest[start..start + end].parse().ok()
}

/// The same, for a FRACTION. `parse_num` stops at the decimal point and returns the whole
/// part, so a depth of 0.5 read as 0 — a modulation link that does nothing, sent by a
/// caller who asked for half range, with no error anywhere.
///
/// The scan accepts one leading sign, digits, at most one point, and an exponent, then
/// hands the slice to Rust's own parser rather than doing the arithmetic here. Refused
/// (None) rather than clamped when the text is not a number: a depth of NaN would reach
/// the audio thread.
fn parse_f32(body: &str, key: &str) -> Option<f32> {
    let i = value_at(body, key)?;
    let rest = &body[i..];
    let start = rest.find(|c: char| c.is_ascii_digit() || c == '-' || c == '.')?;
    let tail = &rest[start..];
    let mut end = 0;
    let mut seen_point = false;
    for (n, c) in tail.char_indices() {
        let ok = c.is_ascii_digit()
            || (c == '-' && n == 0)
            || (c == '.' && !seen_point)
            // An exponent, and the sign that may follow it. Rare in hand-written JSON and
            // ordinary in anything a program generated.
            || (c == 'e' || c == 'E')
            || ((c == '-' || c == '+') && matches!(tail.as_bytes().get(n - 1), Some(b'e') | Some(b'E')));
        if !ok { break; }
        if c == '.' { seen_point = true; }
        end = n + c.len_utf8();
    }
    let v: f32 = tail[..end].parse().ok()?;
    // A non-finite value is refused, not passed on. This number ends up multiplying a
    // parameter on the audio thread.
    v.is_finite().then_some(v)
}

/// Ask the engine for one lane's points and wait for the answer in our own slot.
///
/// BLOCKING, briefly, and on the connection's own thread — which is where every other command
/// on this socket is already handled, so it cannot stall a frame: the state frames go out on a
/// different socket and a different thread. The wait is bounded and a timeout is REPORTED
/// rather than returned as an empty lane, because "no answer yet" and "no automation" are
/// different facts and only one of them is worth drawing.
fn request_automation(handle: &EngineHandle, track: u32, target: u32, param: &str) -> String {
    if param.is_empty() {
        return "{\"error\":\"automation needs a param\"}".to_string();
    }
    let mut param_id = [0u8; 16];
    let b = param.as_bytes();
    let take = b.len().min(param_id.len());
    param_id[..take].copy_from_slice(&b[..take]);
    /*
     * A SEQ NOBODY ELSE WILL USE THIS SECOND. Monotonic per process, so two connections asking
     * at once land in different slots — the answer is matched on the echo regardless, but
     * colliding on a slot means one of them waits for a reply that has already been overwritten.
     */
    static NEXT_SEQ: AtomicU64 = AtomicU64::new(1);
    let seq = NEXT_SEQ.fetch_add(1, Ordering::AcqRel) as u32;
    let payload = UiAutomationLaneRequestPayload {
        command_type: UiCommandType::RequestAutomationLane as u16,
        flags: 0,
        request_seq: seq,
        track_id: track,
        target_plugin_index: target,
        param_id,
        reserved0: 0,
        reserved1: 0,
    };
    if let Err(e) = handle.send_automation_lane_request(payload) {
        return format!("{{\"error\":\"{}\"}}", e.replace('"', "'"));
    }
    let slot = (seq as usize) % daw_bridge::layout::K_UI_AUTOMATION_SLOTS;
    let deadline = Instant::now() + Duration::from_millis(1500);
    loop {
        if let Some(a) = handle.read_automation_slot(slot) {
            // EVERY echoed field, not just the seq. A slot is reused mod four and the param is
            // what makes two questions about the same track different.
            if a.request_seq == seq && a.track_id == track && a.param_id == param {
                let pts: Vec<String> = a.points.iter()
                    .map(|(t, v)| format!("[{},{}]", t, if v.is_finite() { *v } else { 0.0 }))
                    .collect();
                return format!(
                    "{{\"automation\":{{\"track\":{},\"param\":\"{}\",\"found\":{},\
                     \"discrete\":{},\"truncated\":{},\"points\":[{}]}}}}",
                    track, escape_json(param), a.found, a.discrete, a.points_truncated,
                    pts.join(","));
            }
        }
        if Instant::now() >= deadline {
            // Said out loud. An empty list here would read as "nothing automates that", which is
            // a different answer and one the engine is perfectly capable of giving.
            return format!(
                "{{\"automation\":{{\"track\":{},\"param\":\"{}\",\"timeout\":true}}}}",
                track, escape_json(param));
        }
        std::thread::sleep(Duration::from_millis(10));
    }
}

/// Ask the engine what is in a sampler, and wait briefly for the answer.
///
/// Same shape as `request_automation` and for the same reasons: the request carries a
/// `request_seq` the caller chooses, the answer lands in `seq % UI_SAMPLER_KIT_SLOTS`, and every
/// echoed field is checked rather than just the sequence — two slots are reused, and a kit
/// question about track 3 must not be answered by a stale reply about track 1.
///
/// PUBLISHED FROM THE SNAPSHOT THE PRODUCER READS, not from the document — backend's own note on
/// the region. That is what gives this teeth: reading the model back would say what was
/// configured while the audio thread plays something else, and catching exactly that divergence
/// is the point of a read-back.
///
/// `found: false` is an ANSWER — "there is no sampler on that device" — and is forwarded as one.
fn request_sampler_kit(handle: &EngineHandle, track: u32, device: u32) -> String {
    // Its own counter, like `request_automation`'s. They index DIFFERENT slot arrays, so sharing
    // one would only couple two unrelated questions — and a counter per read-back is what keeps
    // `seq % SLOTS` meaning "the slot this answer lands in" rather than "whatever is free".
    static NEXT_KIT_SEQ: AtomicU64 = AtomicU64::new(1);
    let seq = NEXT_KIT_SEQ.fetch_add(1, Ordering::AcqRel) as u32;
    let payload = daw_bridge::layout::UiSamplerKitRequestPayload {
        command_type: daw_bridge::layout::UiCommandType::RequestSamplerKit as u16,
        flags: 0,
        track_id: track,
        device_id: device,
        request_seq: seq,
        reserved: [0u8; 24],
    };
    if let Err(e) = handle.send_sampler_kit_request(payload) {
        return format!("{{\"error\":\"{}\"}}", e.replace('"', "'"));
    }
    let slot = (seq as usize) % daw_bridge::layout::UI_SAMPLER_KIT_SLOTS;
    let deadline = Instant::now() + Duration::from_millis(1500);
    loop {
        if let Some(k) = handle.read_sampler_kit_slot(slot) {
            // DEVICE 0 IS A WILDCARD ON THE WAY OUT, and the answer comes back with the id it
            // RESOLVED TO. Requiring the echo to equal the request rejected every answer to a
            // wildcard — the read simply timed out and the UI held null for ever.
            //
            // It passed for months by luck: ids used to start at 0, so "the first sampler" and
            // "device 0" were the same number and the comparison happened to hold. The day the
            // engine stopped handing out 0 (it is the no-device sentinel, so a device that owned
            // it was never sent a note) every wildcard read broke — a fix on one side surfacing
            // a latent bug on the other.
            //
            // The seq is what makes an answer THIS answer; the track must still match, because
            // the seq only indexes a slot and a slot is reused.
            let device_ok = device == 0 || k.device_id == device;
            if k.request_seq == seq && k.track_id == track && device_ok {
                let slots: Vec<String> = k.slots.iter().map(|e| format!(
                    "{{\"slot\":{},\"source\":{},\"keyLow\":{},\"keyHigh\":{},\"root\":{},\
                      \"velLow\":{},\"velHigh\":{},\"group\":{},\"nna\":{},\"flags\":{},\
                      \"gainMb\":{},\"panTh\":{},\"modSet\":{},\"stem\":{},\"quality\":{},\
                      \"frames\":{},\"slice\":{},\"modMask\":{},\"filterType\":{}}}",
                    e.slotId, e.sourceLocalId, e.keyLow, e.keyHigh, e.rootKey,
                    e.velLow, e.velHigh, e.voiceGroup, e.nna, e.flags,
                    e.gainMillibels, e.panThousandths, e.modSetId, e.outputStem,
                    e.quality, e.lengthFrames, e.sliceId,
                    // A bit means "WOULD move something", not "is stored" — an envelope with no
                    // points and an LFO with zero swing both save and both do nothing. And the
                    // filter type comes with it because the two are only useful together: a
                    // cutoff envelope on a filter that is OFF is silent, so a UI drawing one
                    // without the other shows a live control over a dead one.
                    e.modMask, e.filterType)).collect();
                return format!(
                    "{{\"samplerKit\":{{\"track\":{},\"device\":{},\"resolvedDevice\":{},\
                     \"found\":{},\
                     \"voiceCap\":{},\"activeVoices\":{},\"steals\":{},\"unmapped\":{},\
                     \"truncated\":{},\"slots\":[{}]}}}}",
                    /*
                     * THREE IDS, AND THEY ANSWER DIFFERENT QUESTIONS.
                     *
                     * `track` is the ANSWER's own field, so a reply about the wrong track cannot
                     * be filed as one about the right one. `device` is what was ASKED, because
                     * the caller's cache is keyed on the question and an answer filed under
                     * another key is an answer nobody can find. `resolvedDevice` is what the
                     * engine picked — for the deviceId-0 wildcard ("the first sampler on this
                     * track") that is the only place the real id is visible at all.
                     *
                     * All three echoed the REQUEST before, so a check comparing the reply to what
                     * it asked for compared a value with itself and could not fail. That is what
                     * the guard written this morning against being handed another track's kit
                     * was actually doing.
                     */
                    k.track_id, device, k.device_id, k.found, k.voice_cap, k.active_voices, k.steals,
                    k.unmapped, k.slots_truncated, slots.join(","));
            }
        }
        if Instant::now() >= deadline {
            // Said out loud. An empty slot list here would read as "that sampler is empty",
            // which is a different answer and one the engine can give perfectly well.
            return format!(
                "{{\"samplerKit\":{{\"track\":{},\"device\":{},\"timeout\":true}}}}",
                track, device);
        }
        std::thread::sleep(Duration::from_millis(10));
    }
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

/**
 * Write the smallest valid project: one track, 4/4, 120 BPM, nothing on it.
 *
 * Schema 4 with an explicit empty `clips` and one `placements`-less track — a
 * document the engine's reader accepts and that says nothing it does not mean.
 * No devices, no notes, no harmony: a new song should not arrive with opinions.
 *
 * `safe_name` is the same gate `load` and `save` use, so `new` cannot write
 * outside the project directory by a name the other two would have refused.
 */
fn new_project(dir: &str, name: &str) -> Result<(), &'static str> {
    if !safe_name(name) { return Err("bad project name"); }
    let path = std::path::Path::new(dir).join(format!("{name}.uniproj.json"));
    // Refuses rather than clobbers. Overwriting a song is not something to do as
    // a side effect of the shortcut for "start something".
    if path.exists() { return Err("a project by that name already exists"); }
    let doc = format!(
        "{{\"schema_version\":4,\
          \"meta\":{{\"name\":\"{name}\",\"created_utc\":0,\"modified_utc\":0}},\
          \"timebase\":{{\"nanoticks_per_quarter\":960000,\
                          \"time_sig_numerator\":4,\"time_sig_denominator\":4}},\
          \"nanoticks_per_quarter\":960000,\
          \"tempo_map\":[{{\"nanotick\":0,\"bpm\":120.0}}],\
          \"harmony_timeline\":[],\"clips\":[],\
          \"tracks\":[{{\"track_id\":0,\"name\":\"Track 1\",\"harmony_quantize\":false,\
                        \"lines_per_beat\":4,\
                        \"mixer\":{{\"gain_db\":0.0,\"pan\":0.0,\"mute\":false,\"solo\":false}},\
                        \"device_chain\":[],\"mod_links\":[],\"placements\":[]}}]}}");
    std::fs::create_dir_all(dir).map_err(|_| "cannot create the project directory")?;
    std::fs::write(&path, doc).map_err(|_| "cannot write the project")?;
    Ok(())
}

/// HAS THIS PROJECT ACTUALLY BEEN WRITTEN, and how long ago?
///
/// The FILE, not an ack. `SaveProject`'s outcome goes to `DAW_EVENT("project.save")` and
/// nowhere a browser can read — there is no `uiSaveSeq`/`uiSaveOk` pair mirroring the one the
/// loader got in v15 — so an ack means "the command was queued" and nothing more. A chip
/// reading "saved 40s ago" on the strength of that would be confident for exactly the failure
/// it exists to catch: the write that did not happen.
///
/// So this stats the artefact. `age_seconds` is how old the file is, `bytes` is its size, and
/// `exists: false` is the answer when nothing was written — which is a fact the interface can
/// act on rather than an absence it has to guess about.
///
/// The name is sanitised the way a save is: a caller cannot use this to stat `../../etc`.
fn saved_state(dir: &str, name: &str) -> String {
    if name.is_empty() || name.contains('/') || name.contains('\\') || name.contains("..") {
        return "{\"error\":\"that is not a project name\"}".to_string();
    }
    let path = std::path::Path::new(dir).join(format!("{name}.uniproj.json"));
    let Ok(md) = std::fs::metadata(&path) else {
        return format!("{{\"saved\":{{\"exists\":false,\"name\":\"{}\"}}}}", escape_json(name));
    };
    let age = md.modified().ok()
        .and_then(|t| t.elapsed().ok())
        .map(|d| d.as_secs())
        .unwrap_or(0);
    format!("{{\"saved\":{{\"exists\":true,\"name\":\"{}\",\"age_seconds\":{},\"bytes\":{}}}}}",
            escape_json(name), age, md.len())
}

/// A name as a JSON string body. Filesystem names reach the client, and a quote or a backslash
/// in one would produce a reply the page silently fails to parse — losing the WHOLE message
/// rather than one field.
fn escape_json(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    for c in s.chars() {
        match c {
            '"' | '\\' => { out.push('\\'); out.push(c); }
            c if c < ' ' => {}
            c => out.push(c),
        }
    }
    out
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
    // `value_at`, not `find`: a key that also occurs as a VALUE elsewhere in the message would
    // otherwise match the wrong one. See its doc — that bug reached the audio path.
    let i = value_at(txt, key)?;
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
    // is_type on all three. These were the last `contains` dispatches left, so a
    // project named "save" was claimed by SaveProject and a track renamed to
    // "load" by LoadProject — silent in both directions.
    let ty = if is_type(body, "load") {
        UiCommandType::LoadProject
    } else if is_type(body, "save") {
        UiCommandType::SaveProject
    } else if is_type(body, "rename") {
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

/*
 * PLACEMENT EDITS: where a clip sits, rather than what is in it.
 *
 * One verb with an `op` rather than four verbs, because the four are one gesture
 * seen from different edges — drag the middle and it is a move, drag an edge and
 * it is a resize — and a page that has to pick a message type per edge ends up
 * with the edge logic in two places.
 *
 *   {"type":"placement","op":"move",  "track":0,"id":7,"at":1920000,"toTrack":1}
 *   {"type":"placement","op":"resize","track":0,"id":7,"at":960000,"len":1920000}
 *   {"type":"placement","op":"remove","track":0,"id":7}
 *   {"type":"placement","op":"add",   "track":0,"clip":2,"at":0,"len":3840000}
 *
 * `at` and `len` are OPTIONAL on resize and mean "leave it" when missing — which
 * is what makes a right-edge drag one command instead of a move plus a resize.
 */
/*
 * A TRACK'S ROUTING: where its audio goes.
 *
 *   {"type":"routing","track":2,"audioOutKind":2,"audioOutTrack":0, ...}
 *
 * kind 1 = the master, 2 = another track. A track whose output feeds another
 * track IS a group; there is no separate object to create.
 *
 * The WHOLE struct travels, not the one field being changed. The engine reads a
 * SetTrackRouting as the track's routing entire, so a payload with the other
 * fields zeroed does not leave them alone — it sets them to none, and cuts the
 * track's MIDI on the way past. The page carries the current values through.
 */
fn build_routing(body: &str) -> Option<Result<UiTrackRoutingPayload, &'static str>> {
    if !is_type(body, "routing") { return None; }
    let n = |k: &str| parse_num(body, k).unwrap_or(0).max(0) as u32;
    let kind = n("\"audioOutKind\"") as u8;
    if kind > daw_bridge::layout::ROUTE_KIND_EXTERNAL {
        return Some(Err("unknown route kind"));
    }
    Some(Ok(UiTrackRoutingPayload {
        command_type: UiCommandType::SetTrackRouting as u16,
        // bit0 is pre-fader send, per the payload's own comment.
        flags: if body.contains("\"preFaderSend\":true") { 1 } else { 0 },
        track_id: n("\"track\""),
        base_version: 0,
        midi_in_kind: 0,
        midi_out_kind: n("\"midiOutKind\"") as u8,
        audio_in_kind: n("\"audioInKind\"") as u8,
        audio_out_kind: kind,
        midi_in_track_id: 0,
        midi_out_track_id: n("\"midiOutTrack\""),
        audio_in_track_id: n("\"audioInTrack\""),
        audio_out_track_id: n("\"audioOutTrack\""),
        midi_in_input_id: 0,
        audio_in_input_id: 0,
    }))
}

/// Set a lane's NON-DESTRUCTIVE quantize (UiCommandType::SetLaneQuantize).
///
///   {"type":"quantize","track":0,"grid":240000,"strength":600,"swing":-100}
///
/// Nothing here rewrites a note. The engine applies this to a separate scheduling
/// copy of the flat clip: the authored tick is what is stored, saved and drawn, and
/// quantize changes only where the note SOUNDS. That is the whole point of the item
/// — a performance keeps its exact timing and can be tightened afterwards without
/// ever losing what was played.
///
/// SWING IS BIASED BY +500 HERE AND NOWHERE ELSE. The payload carries it in an
/// unsigned field, so the command adds the bias (0 = -500, 500 = straight, 1000 =
/// +500); the READ-BACK is plain signed. Applying it on both legs is an off-by-500
/// that would show as a groove nobody asked for, and applying it on neither sends
/// a swing of -500 for "straight". The constant is the bridge's, not a copy.
///
/// NOT VERSIONED. `base_version` stays 0 deliberately: quantize moves no authored
/// note, so gating it on a clip version would let an unrelated edit refuse a
/// setting that cannot conflict with anything.
/// SetTrackSoundAddressed (87). A track where PITCH NEVER SELECTS A SLOT.
///
/// Opt-in per track. With it off — the default — a blank `sound` means "let the keymap pick from
/// the pitch", which is right for a drum kit laid across the keys. With it on, the keymap is out
/// of the way and a 64-slot kit stays fully chromatic: every key plays the SAME slot at a
/// different speed, and a row says which slot with `s`.
///
/// A blank sound under the flag resolves to the track's LOWEST SLOT ID rather than to silence.
/// That was worth arguing about and the argument is that it stays a pure function of published
/// data: this side can answer "what will this note play" from the kit read-back alone, with no
/// hidden per-track selection living in the engine and no bounce that depends on where a cursor
/// was.
///
/// NOT VERSIONED, for the same reason quantize is not: it moves no authored note, so gating it
/// on a clip version would let an unrelated edit refuse a setting that cannot conflict.
fn build_sound_addressed(body: &str) -> Option<Result<UiCommandPayload, &'static str>> {
    if !is_type(body, "soundaddressed") { return None; }
    Some(Ok(UiCommandPayload {
        command_type: UiCommandType::SetTrackSoundAddressed as u16,
        flags: 0,
        track_id: parse_num(body, "\"track\"").unwrap_or(0).max(0) as u32,
        plugin_index: 0,
        note_pitch: 0,
        value0: if parse_num(body, "\"on\"").unwrap_or(1) != 0 { 1 } else { 0 },
        note_nanotick_lo: 0,
        note_nanotick_hi: 0,
        note_duration_lo: 0,
        note_duration_hi: 0,
        base_version: 0,
    }))
}

fn build_quantize(body: &str) -> Option<Result<UiCommandPayload, &'static str>> {
    if !is_type(body, "quantize") { return None; }
    let Some(grid) = parse_num(body, "\"grid\"") else {
        return Some(Err("quantize needs a grid in nanoticks (0 turns it off)"));
    };
    if grid < 0 { return Some(Err("a quantize grid cannot be negative")); }
    let strength = parse_num(body, "\"strength\"").unwrap_or(1000).clamp(0, 1000) as u32;
    let swing = parse_num(body, "\"swing\"").unwrap_or(0);
    if !(-500..=500).contains(&swing) {
        // Refused rather than clamped: past +/-500 an odd slot lands on or beyond
        // the next even one, so the slots cross and the pattern reorders itself.
        // Silently clamping would accept a number that means something else.
        return Some(Err("quantize swing must be -500..500 (thousandths of a step)"));
    }
    Some(Ok(UiCommandPayload {
        command_type: UiCommandType::SetLaneQuantize as u16,
        flags: 0,
        track_id: parse_num(body, "\"track\"").unwrap_or(0).max(0) as u32,
        plugin_index: 0,
        note_pitch: (swing + daw_bridge::layout::LANE_QUANTIZE_SWING_BIAS as i64) as u32,
        value0: strength,
        note_nanotick_lo: (grid as u64 & 0xffff_ffff) as u32,
        note_nanotick_hi: ((grid as u64) >> 32) as u32,
        note_duration_lo: 0,
        note_duration_hi: 0,
        base_version: 0,
    }))
}

fn build_placement(body: &str) -> Option<Result<UiCommandPayload, &'static str>> {
    if !is_type(body, "placement") { return None; }
    let op = match parse_str(body, "\"op\"") {
        Some(o) => o,
        None => return Some(Err("placement needs an op: move, resize, remove or add")),
    };
    let mut p = UiCommandPayload {
        command_type: UiCommandType::None as u16,
        flags: 0, track_id: 0, plugin_index: 0, note_pitch: 0, value0: 0,
        note_nanotick_lo: 0, note_nanotick_hi: 0,
        note_duration_lo: 0, note_duration_hi: 0,
        base_version: parse_num(body, "\"base\"").unwrap_or(0).max(0) as u32,
    };
    p.track_id = parse_num(body, "\"track\"").unwrap_or(0).max(0) as u32;
    // Ticks are read as i64 and refused when negative rather than wrapped. A
    // clip dragged left past zero is the ordinary way to produce one, and
    // `as u64` on -1 is 2^64-1 — which is the UNCHANGED sentinel, so a drag off
    // the left edge would have silently meant "leave the start alone".
    let tick = |k: &str| -> Option<Result<u64, &'static str>> {
        parse_num(body, k).map(|v| if v < 0 { Err("negative tick") } else { Ok(v as u64) })
    };
    let split = |p: &mut UiCommandPayload, v: u64, dur: bool| {
        if dur { p.note_duration_lo = v as u32; p.note_duration_hi = (v >> 32) as u32; }
        else   { p.note_nanotick_lo = v as u32; p.note_nanotick_hi = (v >> 32) as u32; }
    };
    match op {
        "move" => {
            p.command_type = UiCommandType::MovePlacement as u16;
            p.value0 = parse_num(body, "\"id\"").unwrap_or(-1).max(-1) as u32;
            match tick("\"at\"") {
                Some(Ok(v)) => split(&mut p, v, false),
                Some(Err(e)) => return Some(Err(e)),
                None => return Some(Err("move needs an `at`")),
            }
            // Absent means "same lane", and the sentinel says so explicitly
            // rather than by repeating the source track — which would look
            // identical to a deliberate move onto the track it is already on.
            p.note_pitch = match parse_num(body, "\"toTrack\"") {
                Some(t) if t >= 0 => t as u32,
                _ => daw_bridge::layout::PLACEMENT_SAME_TRACK,
            };
        }
        "resize" => {
            p.command_type = UiCommandType::ResizePlacement as u16;
            p.value0 = parse_num(body, "\"id\"").unwrap_or(-1).max(-1) as u32;
            let un = daw_bridge::layout::PLACEMENT_UNCHANGED;
            match tick("\"at\"") {
                Some(Ok(v)) => split(&mut p, v, false),
                Some(Err(e)) => return Some(Err(e)),
                None => split(&mut p, un, false),
            }
            match tick("\"len\"") {
                Some(Ok(0)) => return Some(Err("a clip cannot have zero length")),
                Some(Ok(v)) => split(&mut p, v, true),
                Some(Err(e)) => return Some(Err(e)),
                None => split(&mut p, un, true),
            }
            // Both absent is a command that travels the ring to do nothing. It
            // is always a caller bug — an edge drag that computed neither edge —
            // and it is much easier to see here than as a clip that ignores you.
            if p.note_nanotick_lo == un as u32 && p.note_duration_lo == un as u32 {
                return Some(Err("resize needs an `at`, a `len`, or both"));
            }
        }
        "remove" => {
            p.command_type = UiCommandType::RemovePlacement as u16;
            p.value0 = parse_num(body, "\"id\"").unwrap_or(-1).max(-1) as u32;
        }
        "add" => {
            p.command_type = UiCommandType::AddPlacement as u16;
            // The CLIP id here, not a placement id: this is the one op that
            // creates a placement rather than addressing one.
            p.value0 = match parse_num(body, "\"clip\"") {
                Some(c) if c >= 0 => c as u32,
                _ => return Some(Err("add needs a `clip`")),
            };
            match tick("\"at\"") {
                Some(Ok(v)) => split(&mut p, v, false),
                Some(Err(e)) => return Some(Err(e)),
                None => return Some(Err("add needs an `at`")),
            }
            match tick("\"len\"") {
                Some(Ok(0)) | None => return Some(Err("add needs a non-zero `len`")),
                Some(Ok(v)) => split(&mut p, v, true),
                Some(Err(e)) => return Some(Err(e)),
            }
        }
        _ => return Some(Err("unknown placement op")),
    }
    // A missing id reads as u32::MAX after the casts above, which is a placement
    // that cannot exist. Refused here rather than sent, so "nothing happened"
    // is never the answer to a malformed command.
    if !matches!(op, "add") && p.value0 == u32::MAX {
        return Some(Err("placement needs an `id`"));
    }
    Some(Ok(p))
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
    // is_type, not `contains` — the last raw substring dispatch in this file, and it
    // was claiming any message with the word "chord" anywhere in it. Renaming a
    // track to "chord" sends {"type":"rename","track":2,"name":"chord"}, which this
    // builder answered by WRITING A CHORD to track 2 and reporting ok. The essay in
    // `a_project_may_be_named_after_a_command` is about exactly this hole; every
    // other verb was converted and this one was missed, and the loop that would have
    // caught it lists "chord" among its verbs without ever calling this function.
    /*
     * ONE BUILDER, TWO COMMANDS, because they are one payload. `delchord` removes a
     * chord — by its ID when the caller has one, and by (track, tick, column) when it
     * does not, which is what the engine's own handler branches on.
     *
     * A chord could be WRITTEN and not removed: `del` at the cursor checks for a NOTE
     * and refuses with "no note here" when the cursor is on a chord, so a chord you
     * typed by mistake stayed for the life of the song. Creating something you cannot
     * delete is worse than not being able to create it.
     */
    let del = is_type(body, "delchord");
    if !(is_type(body, "chord") || del) { return None; }
    let n = |k: &str, d: i64| parse_num(body, k).unwrap_or(d);
    let tick = n("\"tick\"", 0).max(0) as u64;
    let dur = n("\"dur\"", 0).max(0) as u64;
    Some(UiChordCommandPayload {
        command_type: if del { UiCommandType::DeleteChord } else { UiCommandType::WriteChord }
            as u16,
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
        /*
         * TWO MEANINGS, ONE FIELD, and that is the engine's doing rather than mine:
         * on a WRITE this is the chord's spread in nanoticks; on a DELETE the same
         * bytes carry the chord ID, and 0 means "whatever is at this tick and column".
         *
         * Named as `id` on a delete so a caller does not have to know that, and so a
         * `spread` accidentally left in a delete message cannot be read as an id.
         */
        spread_nanoticks: if del { n("\"id\"", 0).clamp(0, u32::MAX as i64) as u32 }
                          else { n("\"spread\"", 0).clamp(0, u32::MAX as i64) as u32 },
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
///   SliceSelect(7)  base u16@0, count u16@2
fn build_patcher_config(body: &str) -> Option<Result<UiPatcherNodeConfigPayload, &'static str>> {
    // is_type: a project named `patchcfg` was claimed by this and refused with "no
    // config layout for that node type". Third of three raw-substring dispatches the
    // extended naming loop found in one run.
    if !is_type(body, "patchcfg") { return None; }
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
        // SliceSelect(7): base u16@0, count u16@2 — `PatcherSliceSelectConfig` in patcher_abi.h.
        //
        // Two SLOT ADDRESSES, so they are packed as they are and not scaled. A count of 0 would
        // be an empty range for a node whose whole job is to pick from one, so the low bound is
        // 1; a base of 0 is legal and means the sampler's own "let the keymap pick from the
        // pitch" sentinel, which is a setting rather than an unset value.
        7 => {
            cfg[0..2].copy_from_slice(&(c[0].clamp(0, 65535) as u16).to_le_bytes());
            cfg[2..4].copy_from_slice(&(c[1].clamp(1, 65535) as u16).to_le_bytes());
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
    // is_type on all three, for the reason the essay in
    // `a_project_may_be_named_after_a_command` gives: a project named `patchadd`
    // sends {"type":"load","name":"patchadd"} and this claimed it, so the load never
    // reached the engine and the reply talked about node types. Found by extending
    // that test's loop to call every builder in the dispatch chain rather than two
    // of them — the same extension that caught build_chord.
    let add = is_type(body, "patchadd");
    let del = is_type(body, "patchdel");
    let link = is_type(body, "patchlink");
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
/// The same six ui-web's `DEVICE_KINDS` lists, because both are that enum. This
/// copy is here rather than only there for the reason the port table above is:
/// engine vocabulary belongs as close to the engine as the wire allows, and a
/// name the engine does not have must be refused before it becomes an integer.
const DEVICE_KINDS: [&str; 6] = [
    "patcher event",
    "patcher instrument",
    "patcher audio",
    "vst instrument",
    "vst effect",
    // The built-in sampler. This list was FIVE long while DeviceKind::Sampler was 5, so
    // `{"kind":"sampler"}` came back "no such device kind" and the UI could not make one at all
    // — the same one-entry-short drift the page's own table had, in a second place. Both are
    // that enum and both now say so.
    "sampler",
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
                           patcher instrument, patcher audio, vst instrument, vst effect, \
                           sampler";
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
    // is_type, not `contains`: a project named "deldevice" would otherwise be
    // claimed by this builder — the same substring bug we fixed for the other
    // verbs, which is silent when it fires.
    let add = is_type(body, "adddevice");
    let del = is_type(body, "deldevice");
    // Open a plugin's own editor window. The engine has accepted this since
    // before the web UI existed and nothing ever sent it: "how do I open the
    // plugin UI" had the answer "you can't", for a command that was already there.
    let open = is_type(body, "openeditor");
    // Bypass an insert. The chain snapshot has carried each device's bypass state
    // since v20 and the rack has drawn it as a dimmed card the whole time — with
    // no way to set it. A state you can see and cannot change is worse than one
    // you cannot see: it reads as a control that has stopped working.
    let byp = is_type(body, "bypass");
    /*
     * Reorder a device. ORDER IS WHAT A CHAIN IS — a compressor before a distortion
     * is a different sound from the same two the other way round — and until now the
     * rack could add a device and remove one and not change where it sat.
     */
    let mv = is_type(body, "movedevice");
    if !(add || del || open || byp || mv) { return None; }
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
    if open {
        let Some(id) = parse_num(body, "\"device\"").filter(|id| *id >= 0) else {
            return Some(Err("openeditor needs the device id"));
        };
        p.command_type = UiCommandType::OpenPluginEditor as u16;
        p.device_id = id as u32;
        return Some(Ok(p));
    }
    if mv {
        let Some(id) = parse_num(body, "\"device\"").filter(|id| *id >= 0) else {
            return Some(Err("movedevice needs the id of the device to move"));
        };
        let Some(pos) = parse_num(body, "\"pos\"") else {
            return Some(Err("movedevice needs the position to move it to"));
        };
        if pos < 0 { return Some(Err("a chain position cannot be negative")); }
        /*
         * `pos` IS THE DEVICE'S FINAL INDEX, and that is worth stating because the
         * engine's implementation could be read either way: moveDeviceById ERASES the
         * device and then inserts at the index, so the index is into the list
         * WITHOUT it. Which happens to make the index the final resting position —
         * move A to 2 in [A,B,C] gives [B,C,A] — but only because the two
         * cancel. Read as "an index into the original list" it is off by one for
         * every rightward move, silently, producing a chain one place from the one
         * you asked for.
         *
         * The engine clamps past-the-end to the end, so "last" is expressible.
         */
        p.command_type = UiCommandType::MoveDevice as u16;
        p.device_id = id as u32;
        p.insert_index = pos as u32;
        p.patcher_node_id = 0;
        p.host_slot_index = 0;
        return Some(Ok(p));
    }
    if byp {
        let Some(id) = parse_num(body, "\"device\"").filter(|id| *id >= 0) else {
            return Some(Err("bypass needs the id of the device to bypass"));
        };
        // UpdateDevice applies ONLY the fields its flags name — bit0 bypass, bit1
        // patcher node, bit2 host slot. So the auto sentinels this builder defaults
        // the other fields to are never read, and are zeroed anyway rather than
        // relying on that: a command whose payload is only correct because the
        // receiver ignores most of it is one flag bit away from repointing a
        // device's patcher node as a side effect of dimming its card.
        p.command_type = UiCommandType::UpdateDevice as u16;
        p.flags = 0x1;
        p.device_id = id as u32;
        p.insert_index = 0;
        p.patcher_node_id = 0;
        p.host_slot_index = 0;
        // Absent means ON. A toggle sends the state it wants rather than asking
        // for "the other one": two clicks racing on the ring would otherwise
        // cancel out, and the UI already knows what it is looking at.
        p.bypass = match parse_num(body, "\"on\"") { Some(0) => 0, _ => 1 };
        return Some(Ok(p));
    }
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

/// M2.17 GLOBAL-SCOPE COMMANDS, which keep arbitrating on the global counter.
///
/// Undo, Redo, Load and Save can touch any track, so a per-track base would be
/// meaningless for them; harmony has its own counter entirely and never reaches
/// this. Everything else that names a track is arbitrated against THAT track's
/// counter, so this list is the exception and the default is per-track.
fn is_global_scope(command_type: u16) -> bool {
    command_type == UiCommandType::Undo as u16
        || command_type == UiCommandType::Redo as u16
        || command_type == UiCommandType::LoadProject as u16
        || command_type == UiCommandType::SaveProject as u16
}

/// The base version a track-scoped edit must present, when the caller did not say.
///
/// M2.17 MOVED THE GOALPOSTS AND THE SYMPTOM WAS SILENCE. Acceptance used to be
/// global; it is per-track now, and the two diverge on the first edit — measured
/// on `maximal`, three notes on track 0 left it at global 5, track 0 at 5 and
/// every other track at 1. An edit on track 1 quoting 5 is not refused with a
/// message, it is DROPPED, so note entry, chords and transpose all stopped
/// working on every track except the one edited last, with nothing on screen.
///
/// The page cannot stamp this itself: the per-track counters are not on the wire,
/// and putting them there would mean every client re-deriving what this side can
/// read in one call. It is the same argument the BATCH path already makes for
/// re-basing here rather than in the browser — this is the side that can ask.
///
/// An EXPLICIT base is honoured. `daw-cli do note --base V` models an author who
/// read a version, thought, and wrote; overriding that would turn optimistic
/// concurrency into no concurrency at all.
fn resolve_base(handle: &EngineHandle, track_id: u32, sent: u32, command_type: u16) -> u32 {
    if sent != 0 { return sent; }
    // A global-scope command still needs a base — it is arbitrated, just against
    // the other counter. Returning 0 here dropped every Undo silently, which is
    // the same failure this function exists to fix, one counter over.
    if is_global_scope(command_type) { return handle.clip_version(); }
    handle.clip_version_for_track(track_id)
}

/// A MODULATION link command, or None if this message is not one.
///
///   {"type":"mod","op":"add","track":0,"srcDevice":3,"srcId":0,"dstDevice":3,
///    "dstParam":7,"depth":0.5,"bias":0,"source":"macro","rate":"block"}
///   {"type":"mod","op":"remove","track":0,"link":4}
///   {"type":"mod","op":"depth","track":0,"link":4,"depth":0.25}
///
/// MODULATION FLOWS FORWARD: the engine refuses a source LATER in the chain than its
/// target, because a value computed after the parameter was read is a value from the
/// previous block. Same device is legal and is the common case with per-device patchers.
/// Not checked here — the chain order is the engine's and this side does not hold it — so
/// that refusal must reach the person, and it arrives as ModError.
///
/// `depth` and `bias` are the plugin's NORMALISED units (0..1 across the parameter's whole
/// range), which is what the engine's mod path works in. A depth of 1 means the source
/// sweeps the parameter end to end.
///
/// `op: "depth"` is an ADD with the same link id, which is how the engine expresses an
/// update: `AddModLink` with an existing id replaces that link rather than making a
/// second one. Named separately here because "change how much" is a different intention
/// from "make a link", and a caller should not have to know they are the same command.
fn build_mod(body: &str) -> Option<Result<UiModLinkCommandPayload, &'static str>> {
    if !is_type(body, "mod") { return None; }
    let Some(op) = parse_str(body, "\"op\"") else {
        return Some(Err("mod needs an op: add, remove or depth"));
    };
    let removing = op == "remove";
    let command_type = match op {
        "add" => UiCommandType::AddModLink,
        "remove" => UiCommandType::RemoveModLink,
        /*
         * ITS OWN OPCODE NOW (v28). This used to be an AddModLink with the same id, which the
         * engine REFUSES — so a depth change was a remove and an add: the link got a new id,
         * came back UNNAMED (and therefore inert until re-named), and was not atomic. Which
         * made a depth SLIDER impossible, because a continuous gesture would have torn the
         * link down and rebuilt it every frame.
         *
         * `SetModLinkDepth` updates in place by linkId and ignores the device and kind fields.
         * They are still sent, because a payload with holes in it is a payload somebody fills
         * in wrongly later.
         */
        "depth" => UiCommandType::SetModLinkDepth,
        _ => return Some(Err("mod op must be add, remove or depth")),
    };
    let link = parse_num(body, "\"link\"").unwrap_or(-1);
    if (removing || op == "depth") && link < 0 {
        return Some(Err("that mod op needs the id of an existing link"));
    }
    // The four small enums, by NAME. A caller typing `"source":"lfo"` should not have to
    // know it is 1, and a number that means something else in the next version of the
    // engine is exactly the kind of literal that outlives its meaning.
    let source_kind = match parse_str(body, "\"source\"").unwrap_or("macro") {
        "macro" => MOD_SOURCE_MACRO,
        "lfo" => MOD_SOURCE_LFO,
        "env" | "envelope" => MOD_SOURCE_ENVELOPE,
        "node" | "patcher" => MOD_SOURCE_PATCHER_NODE_OUTPUT,
        _ => return Some(Err("mod source must be macro, lfo, env or node")),
    };
    let target_kind = match parse_str(body, "\"target\"").unwrap_or("param") {
        "param" | "vst" => MOD_TARGET_VST_PARAM,
        "patcher" => MOD_TARGET_PATCHER_PARAM,
        "macro" => MOD_TARGET_PATCHER_MACRO,
        _ => return Some(Err("mod target must be param, patcher or macro")),
    };
    let rate = match parse_str(body, "\"rate\"").unwrap_or("block") {
        "block" => MOD_RATE_BLOCK,
        "sample" => MOD_RATE_SAMPLE,
        _ => return Some(Err("mod rate must be block or sample")),
    };
    // Bits 0-3 source kind, 4-7 target kind, 8-9 rate, bit 10 enabled. Packed ONCE, here,
    // so the bit layout lives on this side of the wire and no caller learns it.
    let mut flags = (source_kind & 0x0f) | ((target_kind & 0x0f) << 4) | ((rate & 0x03) << 8);
    // Enabled unless explicitly disabled: a link nobody asked to switch off is on.
    if parse_num(body, "\"enabled\"").unwrap_or(1) != 0 { flags |= 1 << 10; }
    Some(Ok(UiModLinkCommandPayload {
        command_type: command_type as u16,
        flags,
        track_id: parse_num(body, "\"track\"").unwrap_or(0).max(0) as u32,
        // The engine arbitrates mod edits on its own version, and 0 means "whatever you
        // hold" — the same convention the chain edits use for an unstated base.
        base_version: parse_num(body, "\"base\"").unwrap_or(0).max(0) as u32,
        link_id: if link < 0 { MOD_LINK_ID_AUTO } else { link as u32 },
        source_device_id: parse_num(body, "\"srcDevice\"").unwrap_or(0).max(0) as u32,
        source_id: parse_num(body, "\"srcId\"").unwrap_or(0).max(0) as u32,
        target_device_id: parse_num(body, "\"dstDevice\"").unwrap_or(0).max(0) as u32,
        target_id: parse_num(body, "\"dstParam\"").unwrap_or(0).max(0) as u32,
        depth: parse_f32(body, "\"depth\"").unwrap_or(1.0),
        bias: parse_f32(body, "\"bias\"").unwrap_or(0.0),
    }))
}

/// SetModLinkUid16 (22): name the plugin parameter a link targets.
///
///   {"type":"moduid","track":0,"link":4,"uid16":"0a1b…"}
///
/// THIS IS NOT OPTIONAL DECORATION. The engine's block-rate modulation addresses a VST
/// parameter by `uid16` and never reads `targetId` — see the ParamPayload it builds — so a
/// link without one moves NOTHING, while still being accepted, published and drawable. It
/// is the single sharpest edge in this feature: a lit badge over a link that does nothing.
///
/// The uid is 32 hex characters (16 bytes). Anything else is refused rather than padded:
/// a half-parsed uid names a different parameter, and "the wrong knob moved" is harder to
/// diagnose than "that was not a uid".
fn build_mod_uid(body: &str) -> Option<Result<UiModLinkUid16Payload, &'static str>> {
    if !is_type(body, "moduid") { return None; }
    let Some(hex) = parse_str(body, "\"uid16\"") else {
        return Some(Err("moduid needs the parameter's uid16"));
    };
    if hex.len() != 32 || !hex.bytes().all(|c| c.is_ascii_hexdigit()) {
        return Some(Err("a uid16 is 32 hex characters"));
    }
    let mut uid16 = [0u8; 16];
    for i in 0..16 {
        uid16[i] = u8::from_str_radix(&hex[i * 2..i * 2 + 2], 16).unwrap_or(0);
    }
    let link = parse_num(body, "\"link\"").unwrap_or(-1);
    if link < 0 { return Some(Err("moduid needs the id of an existing link")); }
    Some(Ok(UiModLinkUid16Payload {
        command_type: UiCommandType::SetModLinkUid16 as u16,
        flags: 0,
        track_id: parse_num(body, "\"track\"").unwrap_or(0).max(0) as u32,
        base_version: 0,
        link_id: link as u32,
        uid16,
        reserved: [0u8; 8],
    }))
}

/// SetModSourceValue (23): turn a macro knob.
///
///   {"type":"modsource","track":0,"device":3,"source":0,"value":0.5}
///
/// ALSO NOT OPTIONAL. The engine resolves a link's source by looking it up in the track's
/// source STATES, and a macro that has never been given a value is not in that table — so
/// the link is skipped and, again, moves nothing. A macro is a knob with nothing behind it
/// until somebody turns it, and "nobody has turned it yet" and "this does not work" look
/// identical from outside.
fn build_mod_source(body: &str) -> Option<Result<UiModSourceValuePayload, &'static str>> {
    if !is_type(body, "modsource") { return None; }
    let Some(v) = parse_f32(body, "\"value\"") else {
        return Some(Err("modsource needs a value from 0 to 1"));
    };
    Some(Ok(UiModSourceValuePayload {
        command_type: UiCommandType::SetModSourceValue as u16,
        // The source KIND, in the same bits AddModLink packs it into. Macro unless told
        // otherwise, because a macro is the only source that exists without something
        // else being made first.
        flags: match parse_str(body, "\"kind\"").unwrap_or("macro") {
            "macro" => MOD_SOURCE_MACRO,
            "lfo" => MOD_SOURCE_LFO,
            "env" | "envelope" => MOD_SOURCE_ENVELOPE,
            "node" | "patcher" => MOD_SOURCE_PATCHER_NODE_OUTPUT,
            _ => return Some(Err("mod source must be macro, lfo, env or node")),
        },
        track_id: parse_num(body, "\"track\"").unwrap_or(0).max(0) as u32,
        base_version: 0,
        source_device_id: parse_num(body, "\"device\"").unwrap_or(0).max(0) as u32,
        source_id: parse_num(body, "\"source\"").unwrap_or(0).max(0) as u32,
        value: v.clamp(0.0, 1.0),
        reserved: [0u8; 16],
    }))
}

/// WRITE one automation point, or None if this message is not one.
///
///   {"type":"autopoint","track":0,"param":"cutoff","tick":0,"value":0.25,"discrete":0}
///
/// A point is addressed by (track, paramId, tick) — writing the same tick again REPLACES that
/// point rather than stacking a second one on it, which is what makes a drag over a curve
/// possible at all.
///
/// `discrete` belongs to the CLIP and not to the point: it is applied when the clip is created
/// and ignored afterwards, because a switch that changed meaning halfway through a curve would
/// make the curve unreadable. Sent anyway on every point, so the first one carries it.
///
/// `value` is the plugin's NORMALISED 0..1. Out of range is CLAMPED rather than refused: a
/// gesture that runs past the end of a fader is a normal thing for a pointer to do, and refusing
/// it would make the last pixel of a drag do nothing.
fn build_automation_point(body: &str) -> Option<Result<UiAutomationPointPayload, &'static str>> {
    if !is_type(body, "autopoint") { return None; }
    let Some(param) = parse_str(body, "\"param\"").filter(|p| !p.is_empty()) else {
        return Some(Err("autopoint needs a param"));
    };
    let Some(value) = parse_f32(body, "\"value\"") else {
        return Some(Err("autopoint needs a value from 0 to 1"));
    };
    let tick = parse_num(body, "\"tick\"").unwrap_or(0).max(0) as u64;
    let mut param_id = [0u8; 16];
    let b = param.as_bytes();
    let take = b.len().min(param_id.len());
    param_id[..take].copy_from_slice(&b[..take]);
    Some(Ok(UiAutomationPointPayload {
        command_type: UiCommandType::WriteAutomationPoint as u16,
        flags: if parse_num(body, "\"discrete\"").unwrap_or(0) != 0 { 1 } else { 0 },
        track_id: parse_num(body, "\"track\"").unwrap_or(0).max(0) as u32,
        // PARAM_TARGET_ALL by default: every plugin on the track that publishes this parameter.
        // A caller that means one plugin says which.
        target_plugin_index: parse_num(body, "\"target\"").unwrap_or(u32::MAX as i64) as u32,
        nanotick_lo: (tick & 0xffff_ffff) as u32,
        nanotick_hi: (tick >> 32) as u32,
        value: value.clamp(0.0, 1.0),
        param_id,
    }))
}

/// One of the four MARKER commands, or None if this message is not one.
///
///   {"type":"marker","op":"add","tick":0,"name":"chorus","color":0}
///   {"type":"marker","op":"remove","id":3}
///   {"type":"marker","op":"rename","id":3,"name":"chorus 2"}
///   {"type":"marker","op":"move","id":3,"tick":15360000}
///
/// A MARKER IS A NAMED TICK AND NOTHING ELSE. It stores no length, so these four ops are TOTAL:
/// they move no material and can be refused only for a bad id. That is the whole difference from
/// the sections they replace, where changing a "length" rippled the song — and it is why the
/// ripple is now its own command with its own name (`build_arrange_time`, below) rather than a
/// side effect of editing a label.
fn build_marker(body: &str) -> Option<Result<UiMarkerCommandPayload, &'static str>> {
    if !is_type(body, "marker") { return None; }
    let Some(op) = parse_str(body, "\"op\"") else {
        return Some(Err("marker needs an op: add, remove, rename or move"));
    };
    let (command_type, addressed) = match op {
        // The bool is "this op names an EXISTING marker", which everything but add does.
        "add" => (UiCommandType::AddMarker, false),
        "remove" => (UiCommandType::RemoveMarker, true),
        "rename" => (UiCommandType::RenameMarker, true),
        "move" => (UiCommandType::MoveMarker, true),
        _ => return Some(Err("marker op must be add, remove, rename or move")),
    };
    let id = parse_num(body, "\"id\"").unwrap_or(0).max(0) as u32;
    // 0 is the "let the engine assign" sentinel for an add, so a missing id would silently
    // become a NEW marker on a remove or a rename.
    if id == 0 && addressed {
        return Some(Err("that marker op needs the id of an existing marker"));
    }
    let tick = parse_num(body, "\"tick\"").unwrap_or(0).max(0) as u64;
    if command_type as u16 == UiCommandType::MoveMarker as u16
        && parse_num(body, "\"tick\"").is_none() {
        // A move with no destination would move it to tick 0 — the start of the song, which is
        // the one place nobody means.
        return Some(Err("a marker move needs the tick to move it to"));
    }
    let mut name = [0u8; 20];
    if let Some(n) = parse_str(body, "\"name\"") {
        let b = n.as_bytes();
        let take = b.len().min(name.len());
        name[..take].copy_from_slice(&b[..take]);
    } else if command_type as u16 == UiCommandType::RenameMarker as u16 {
        return Some(Err("a rename needs a name"));
    }
    Some(Ok(UiMarkerCommandPayload {
        command_type: command_type as u16,
        flags: 0,
        marker_id: id,
        nanotick_lo: (tick & 0xffff_ffff) as u32,
        nanotick_hi: (tick >> 32) as u32,
        color_rgb: parse_num(body, "\"color\"").unwrap_or(0).clamp(0, 0xffffff) as u32,
        name,
    }))
}

/// A TIMELINE command: insert or remove arrangement time, or set a meter point.
///
///   {"type":"time","op":"insert","tick":15360000,"bars":2}
///   {"type":"time","op":"remove","tick":15360000,"bars":2}
///   {"type":"timesig","sig":"7/8","tick":0,"flatten":1}
///
/// INSERT/REMOVE IS THE RIPPLE, and it is what the boundary drag on the spine strip sends. Two
/// bars inserted at a marker's tick moves that marker and everything at or after it — every
/// placement on every track, the tempo points, the key changes, the automation points, the meter
/// points and the other markers — in ONE transaction that can be refused whole and undone whole.
/// SetSectionLength did the same moving and pushed no undo entry, because the undo entry held at
/// most two tracks.
///
/// BARS, NOT TICKS. A bar's length depends on the meter in force where the edit happens, and the
/// engine holds the authoritative map — a client converting would have to re-implement the prefix
/// sum, and would be wrong on the first mid-song meter change. `UI_TIME_EDIT_DELTA_IS_TICKS` is
/// there for a caller that genuinely means ticks; nothing here does.
fn build_arrange_time(body: &str) -> Option<Result<UiArrangeTimeCommandPayload, &'static str>> {
    let is_time = is_type(body, "time");
    let is_sig = is_type(body, "timesig");
    if !is_time && !is_sig { return None; }
    let tick = parse_num(body, "\"tick\"").unwrap_or(0).max(0) as u64;
    if is_sig {
        // "7/8" as one token, because that is how a person writes a time signature and splitting
        // it into two arguments invites the pair to be sent half-updated.
        let Some(sig) = parse_str(body, "\"sig\"") else {
            return Some(Err("timesig needs a signature like 7/8"));
        };
        let mut parts = sig.split('/');
        let num: u32 = parts.next().and_then(|x| x.trim().parse().ok()).unwrap_or(0);
        let den: u32 = parts.next().and_then(|x| x.trim().parse().ok()).unwrap_or(0);
        if num == 0 || den == 0 || !den.is_power_of_two() {
            // A zero denominator divides by zero in every bar computation downstream, and a
            // denominator that is not a power of two is not a time signature music uses.
            return Some(Err("a time signature is beats/note-value, like 4/4 or 7/8"));
        }
        return Some(Ok(UiArrangeTimeCommandPayload {
            command_type: UiCommandType::SetTimeSignature as u16,
            flags: if parse_num(body, "\"flatten\"").unwrap_or(0) != 0 { UI_TIME_SIG_FLATTEN } else { 0 },
            nanotick_lo: (tick & 0xffff_ffff) as u32,
            nanotick_hi: (tick >> 32) as u32,
            delta: 0,
            numerator: num,
            denominator: den,
            reserved0: 0, reserved1: 0, reserved2: 0, reserved3: 0,
        }));
    }
    let Some(op) = parse_str(body, "\"op\"") else {
        return Some(Err("time needs an op: insert or remove"));
    };
    let Some(bars) = parse_num(body, "\"bars\"").filter(|b| *b > 0) else {
        return Some(Err("time insert/remove needs a positive number of bars"));
    };
    let delta = match op {
        "insert" => bars,
        // Removal is a NEGATIVE delta on the same command, so one refusal path covers both and
        // the undo entry is the same shape either way.
        "remove" => -bars,
        _ => return Some(Err("time op must be insert or remove")),
    };
    Some(Ok(UiArrangeTimeCommandPayload {
        command_type: UiCommandType::InsertRemoveTime as u16,
        flags: 0,
        nanotick_lo: (tick & 0xffff_ffff) as u32,
        nanotick_hi: (tick >> 32) as u32,
        delta: delta.clamp(i32::MIN as i64, i32::MAX as i64) as i32,
        numerator: 0, denominator: 0,
        reserved0: 0, reserved1: 0, reserved2: 0, reserved3: 0,
    }))
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

/// The note column a command names, as the engine wants it: the low byte of
/// `flags`. Absent means column 0, which is what a single-column tracker sends.
fn note_column(body: &str) -> u16 {
    (parse_num(body, "\"column\"").unwrap_or(0).clamp(0, 255) as u16) & 0xff
}

fn build_command(body: &str) -> Result<UiCommandPayload, &'static str> {
    if let Some(r) = build_named(body) { return r; }
    if let Some(r) = build_placement(body) { return r; }
    if let Some(r) = build_quantize(body) { return r; }
    if let Some(r) = build_sound_addressed(body) { return r; }

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
    } else if body.contains("\"panic\"") {
        /*
         * PANIC. All notes off, everywhere, now.
         *
         * Checked BEFORE "stop" — the engine halts the transport as part of a
         * panic, so the two are not alternatives and the more specific one has
         * to win. Ordered the other way, a panic would arrive as a plain stop
         * and the room would stay full of sound.
         */
        p.command_type = UiCommandType::Panic as u16;
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
    } else if is_type(body, "preview") {
        /*
         * Sound a pitch without writing it (kUiCommandType 45).
         *
         * Held: on=1 at keydown, on=0 at keyup, so a sustained key sustains and a
         * chord is several on=1. Out of band — the engine injects it straight into
         * the track's instrument and it never reaches the clip store, so it is not
         * recorded, not undoable and does not dirty the project. It is a sound,
         * not an edit.
         *
         * Checked BEFORE the "note" arm, and with is_type rather than a substring:
         * `{"type":"preview","pitch":60}` contains no "note" today, but the arm
         * below matches on a substring and the two would collide the moment either
         * message grew a field named for the other.
         */
        p.command_type = UiCommandType::PreviewNote as u16;
        p.note_pitch = parse_num(body, "\"pitch\"").unwrap_or(60).clamp(0, 127) as u32;
        p.value0 = parse_num(body, "\"vel\"").unwrap_or(100).clamp(0, 127) as u32;
        // Anything but an explicit 0 is a note-on: a keyup that lost its field
        // should not silently become a stuck voice.
        p.flags = if parse_num(body, "\"on\"").unwrap_or(1) != 0 { 1 } else { 0 };
    } else if body.contains("\"note\"") {
        let dur = parse_num(body, "\"dur\"").unwrap_or(960_000).max(1) as u64;
        p.command_type = UiCommandType::WriteNote as u16;
        p.note_pitch = parse_num(body, "\"pitch\"").unwrap_or(60).clamp(0, 127) as u32;
        p.value0 = parse_num(body, "\"vel\"").unwrap_or(100).clamp(0, 127) as u32;
        p.note_duration_lo = dur as u32;
        p.note_duration_hi = (dur >> 32) as u32;
        // WHICH NOTE COLUMN. The engine has read this off the low byte of flags
        // since before the web UI existed — `applyAddNote` opens with
        // `const uint8_t column = flags & 0xff` — and this builder dropped it, so
        // every note the UI ever wrote landed in column 0 and a second note on the
        // same row REPLACED the first instead of sitting beside it. build_chord
        // three hundred lines up has always sent it; notes never did.
        p.flags = note_column(body);
    } else if body.contains("\"delete\"") {
        p.command_type = UiCommandType::DeleteNote as u16;
        // Same byte, same reason: with more than one note column, deleting by
        // (track, tick) alone removes whichever the engine matches first.
        p.flags = note_column(body);
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
    } else if is_type(body, "harmony") {
        // A key change on the harmony timeline.
        //
        // The engine has taken WriteHarmony since long before the web UI, and
        // daw-cli builds it — but this sidecar had no verb for it, so harmony
        // could be READ from every surface in the app and WRITTEN from none. The
        // right-hand pane shows the key, the scale and its degrees; the tracker
        // has a harmony column; nothing could change any of it.
        //
        // Root and scale ride in note_pitch and value0, which is where the engine
        // reads them from — not a shape I chose, but the one that already exists.
        p.command_type = UiCommandType::WriteHarmony as u16;
        p.note_pitch = (parse_num(body, "\"root\"").unwrap_or(0).rem_euclid(12)) as u32;
        p.value0 = parse_num(body, "\"scale\"").unwrap_or(0).max(0) as u32;
    } else if is_type(body, "delharmony") {
        /*
         * Remove a key change. The other half of the pair above, and missing for the
         * same reason: harmony could be read everywhere and written nowhere, and once
         * writing landed a key change could be ADDED to the timeline and never taken
         * off. A timeline you can only add to is one you stop using.
         *
         * Addressed by TICK, which is what the engine matches on — `removeHarmony`
         * takes a nanotick and reports "event not found" otherwise. The tick comes
         * from the event the caller is looking at, so the round trip is exact rather
         * than a search for the nearest.
         */
        p.command_type = UiCommandType::DeleteHarmony as u16;
    } else if is_type(body, "addtrack") {
        // v1 APPENDS at the extent — no insert-after, no fields read. The engine
        // refuses at kUiMaxTracks rather than growing past what the UI region can
        // publish, so a full song reports "no" instead of losing a track quietly.
        p.command_type = UiCommandType::AddTrack as u16;
    } else if is_type(body, "removetrack") {
        // track_id is the STABLE id, already parsed above from "track". The engine
        // tombstones the slot instead of compacting, so the tracks after it keep
        // their ids and every cursor, selection and per-track cache keyed on an
        // index stays pointing where it was. A child id is rejected engine-side:
        // aux stems are views into their parent's buses and go when it goes.
        p.command_type = UiCommandType::RemoveTrack as u16;
    } else if is_type(body, "scratch") {
        /*
         * M2.57 SCRATCH CLIPS: fork, swap, keep.
         *
         * `fork` copies what a placement plays, points the placement at the copy, and keeps the
         * original as its ALTERNATE. `swap` exchanges the two — that IS the A/B, and what plays
         * is always the placement's clip, so there is no auditioning mode to fall out of step
         * with what you hear. `keep` drops the other once you have decided, and keeping is doing
         * nothing.
         *
         * `value0` is the PLACEMENT, not the clip: forking is about one appearance, and naming
         * the clip would be naming the thing every appearance shares — the opposite of the point.
         */
        let op = parse_str(body, "\"op\"").unwrap_or("");
        p.command_type = match op {
            "fork" => UiCommandType::ForkPlacementClip as u16,
            "swap" => UiCommandType::SwapPlacementClip as u16,
            "keep" => UiCommandType::ClearPlacementAlternate as u16,
            // An unknown op leaves command_type at None, which the engine ignores — so it is
            // refused HERE instead, by name, rather than sent to be dropped in silence.
            _ => return Err("scratch op must be fork, swap or keep"),
        };
        p.value0 = parse_num(body, "\"placement\"").unwrap_or(-1).max(0) as u32;
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

/// SET ROW OPS on one existing note (opcode 81).
///
///   {"type":"setrowops","track":0,"clip":1,"note":7,"mask":3,"ret":3,"prob":60,
///    "sound":0,"offset":0,"delay":160000}
///
/// THE MASK IS THE POINT and it is carried through verbatim from the client. A bit CLEAR leaves
/// that op alone; a bit SET with a zero value CLEARS it. Deciding the mask HERE — say, from
/// which fields the JSON happens to carry — would make deleting an op impossible to express,
/// because "probability: 0" and "no probability given" would look identical on the wire.
///
/// `note` is the authored EventId. The payload field is u32 while the id is u64, so the caller
/// is responsible for refusing an id that does not fit; see the guard in the page. Truncating
/// here would drop the AUTHOR field and land the edit on a different note.
fn build_set_row_ops(body: &str) -> Option<Result<UiSetRowOpsPayload, &'static str>> {
    if !is_type(body, "setrowops") { return None; }
    let Some(mask) = parse_num(body, "\"mask\"") else {
        return Some(Err("setrowops needs a mask — which ops it means"));
    };
    if mask <= 0 { return Some(Err("setrowops with an empty mask would change nothing")); }
    let note = parse_num(body, "\"note\"").unwrap_or(0).max(0) as u64;
    if note == 0 { return Some(Err("setrowops needs the note it edits")); }
    Some(Ok(UiSetRowOpsPayload {
        command_type: UiCommandType::SetRowOps as u16,
        mask: mask as u16,
        track_id: parse_num(body, "\"track\"").unwrap_or(0).max(0) as u32,
        clip_id: parse_num(body, "\"clip\"").unwrap_or(0).max(0) as u32,
        // SPLIT, not truncated. EventId packs the AUTHOR into bits 48+ and each author counts
        // independently, so a 32-bit id drops the author and agent note (1, 5) collides with
        // human note (0, 5) — an edit that lands on a different note and says nothing. Backend
        // widened the field after this was reported; the halves are what that looks like.
        note_id_lo: (note & 0xFFFF_FFFF) as u32,
        note_id_hi: (note >> 32) as u32,
        delay_nanoticks: parse_num(body, "\"delay\"").unwrap_or(0).max(0) as u32,
        sound: parse_num(body, "\"sound\"").unwrap_or(0).clamp(0, 65535) as u16,
        sound_offset: parse_num(body, "\"offset\"").unwrap_or(0).clamp(0, 65535) as u16,
        retrigger: parse_num(body, "\"ret\"").unwrap_or(0).clamp(0, 255) as u8,
        probability: parse_num(body, "\"prob\"").unwrap_or(0).clamp(0, 255) as u8,
        /*
         * v33: pad0 became these two, so the payload is the same 40 bytes and nothing moved.
         * They are read from the message like every other op — the MASK is what says whether
         * this command is speaking about them, so a caller that never mentions them leaves them
         * alone rather than clearing them.
         */
        retrig_ramp: parse_num(body, "\"ramp\"").unwrap_or(0).clamp(-100, 100) as i8,
        trig_condition: parse_num(body, "\"cond\"").unwrap_or(0).clamp(0, 255) as u8,
        reserved: [0u8; 8],
    }))
}

/// LOAD A SAMPLE into a sampler device, minting a source and a slot (opcode 73).
///
///   {"type":"samplerload","track":0,"device":9,"name":"kick.wav","root":36,"fixed":1}
///
/// `name` is a project-relative FILE NAME and not a path, because the whole payload is 40 bytes
/// and the name gets 24 of them. The engine resolves it against its own audio directory, which
/// is also what stops a client handing the engine a path of its choosing.
///
/// `fixed` sets SAMPLER_LOAD_FIXED_PITCH: keyLow == keyHigh == rootKey, which is how a drum
/// stays a drum across the keyboard. Clear it for a playable zone. There is no mapping MODE
/// stored anywhere — the flag chooses which keys get written at load time and nothing reads it
/// afterwards.
fn build_sampler_load(body: &str) -> Option<Result<UiSamplerLoadPayload, &'static str>> {
    if !is_type(body, "samplerload") { return None; }
    let Some(name) = parse_str(body, "\"name\"").filter(|n| !n.is_empty()) else {
        return Some(Err("samplerload needs a file name"));
    };
    if name.len() > 24 {
        // Refused rather than truncated: a truncated name resolves to nothing or, worse, to a
        // DIFFERENT file, and "load succeeded, wrong sample" is not a failure anyone would
        // think to look for.
        return Some(Err("that file name is longer than the 24 bytes the command carries"));
    }
    let mut buf = [0u8; 24];
    buf[..name.len()].copy_from_slice(name.as_bytes());
    let fixed = parse_num(body, "\"fixed\"").unwrap_or(1) != 0;
    Some(Ok(UiSamplerLoadPayload {
        command_type: UiCommandType::SamplerLoad as u16,
        flags: if fixed { daw_bridge::layout::SAMPLER_LOAD_FIXED_PITCH } else { 0 },
        track_id: parse_num(body, "\"track\"").unwrap_or(0).max(0) as u32,
        device_id: parse_num(body, "\"device\"").unwrap_or(0).max(0) as u32,
        root_key: parse_num(body, "\"root\"").unwrap_or(36).clamp(0, 127) as u8,
        reserved: [0u8; 3],
        name: buf,
    }))
}

/// CHOP A SOURCE into slices, and optionally a playable slot per slice (opcode 74).
///
///   {"type":"samplerslice","track":0,"device":9,"source":1,"mode":"equal","count":16,"slots":1}
///
/// `slots` is the gesture that turns a chop into something you can play in ONE command rather
/// than N: a slot per slice from `base` upward, so the break lands under the fingers in order
/// and a pattern addresses a hit by pitch alone. Without it the slice set exists and nothing
/// plays it.
///
/// `snap` is the row grid in nanoticks, and 0 means no snap. Snapping makes a chop
/// tempo-adaptive from the moment it is made rather than tied to the rate the file was recorded
/// at — which is the difference between a break that follows the song and one the song has to
/// follow.
/// SamplerSetDevice (88). ONE device-level field of a sampler, by its wire id.
///
/// Three fields that the engine has always read and saved and nothing could write: the default
/// gate a new slot is minted with, the voice cap, and the remembered view. Field-addressed like
/// SamplerSetSlot so all three arrived on one opcode.
///
/// The field id is not validated against a list here — the engine owns which ids exist and
/// refuses an unknown one on the SamplerRejected channel, where a caller can see it. A second
/// copy of that list in the sidecar would be a third place for it to drift.
fn build_sampler_device(body: &str) -> Option<Result<UiSamplerSetDevicePayload, &'static str>> {
    if !is_type(body, "samplerdevice") { return None; }
    let Some(field) = parse_num(body, "\"field\"") else {
        return Some(Err("samplerdevice needs a field id"));
    };
    Some(Ok(UiSamplerSetDevicePayload {
        command_type: UiCommandType::SamplerSetDevice as u16,
        field: field.clamp(0, 65535) as u16,
        track_id: parse_num(body, "\"track\"").unwrap_or(0).max(0) as u32,
        device_id: parse_num(body, "\"device\"").unwrap_or(0).max(0) as u32,
        value: parse_num(body, "\"value\"").unwrap_or(0).clamp(i32::MIN as i64, i32::MAX as i64) as i32,
        reserved: [0u8; 24],
    }))
}

/// SamplerSetSlot (opcode 84's neighbour). ONE field of one slot, by its wire id.
///
/// `value` is SIGNED and stays signed all the way down: gain, pan, tune and pitch-track are all
/// legitimately negative, and backend's euclidean octave_offset bug was exactly a signed value
/// pushed through an unsigned path.
///
/// The field id is not validated against a list here. The engine owns which ids exist and
/// answers an unknown one; a second copy of that list in the sidecar would be a third place for
/// it to drift, after the header and the UI's names.
fn build_sampler_slot(body: &str) -> Option<Result<UiSamplerSetSlotPayload, &'static str>> {
    if !is_type(body, "samplerslot") { return None; }
    let Some(field) = parse_num(body, "\"field\"") else {
        return Some(Err("samplerslot needs a field id"));
    };
    if !(0..=26).contains(&field) {
        return Some(Err("slot field id is 0..26 — see UiSamplerSlotField"));
    }
    Some(Ok(UiSamplerSetSlotPayload {
        command_type: UiCommandType::SamplerSetSlot as u16,
        field: field as u16,
        track_id: parse_num(body, "\"track\"").unwrap_or(0).max(0) as u32,
        device_id: parse_num(body, "\"device\"").unwrap_or(0).max(0) as u32,
        // A REAL SLOT ID. 0 is NOT a wildcard here, unlike deviceId and modSetId on the
        // neighbouring commands: the engine answers `sampler.set_slot_rejected ... no_such_slot`.
        // Left as the default anyway so the refusal comes from the engine that owns the rule
        // rather than from a second copy of it here.
        slot_id: parse_num(body, "\"slot\"").unwrap_or(0).max(0) as u32,
        value: parse_num(body, "\"value\"").unwrap_or(0).clamp(i32::MIN as i64, i32::MAX as i64) as i32,
        reserved: [0u8; 20],
    }))
}

/// SamplerSetEnvelope (82). One modulator's shape on one target.
///
/// TIMES CARRY THEIR UNIT. `timeBase` names it in the same payload — 0 microseconds, 1 nanoticks
/// — because a payload of bare durations means different things depending on state the sender
/// never saw: "a 300 ms attack" silently becomes a 300-nanotick one against a mod set someone
/// else had switched to tempo-sync. Defaulted to microseconds here, which is what a drum kit
/// wants and what a caller who does not say is asking for.
///
/// `sustain` and `depth` are milli-units (1000 = full), and depth is SIGNED: an inverted
/// envelope is a normal thing to want on pitch or panning, and refusing negatives would make
/// this the one place that cannot express it.
fn build_sampler_envelope(body: &str) -> Option<Result<UiSamplerEnvelopePayload, &'static str>> {
    if !is_type(body, "samplerenv") { return None; }
    let target = parse_num(body, "\"target\"").unwrap_or(0);
    if !(0..=4).contains(&target) {
        return Some(Err("envelope target is 0 volume, 1 pan, 2 pitch, 3 cutoff, 4 resonance"));
    }
    let time_base = parse_num(body, "\"timeBase\"").unwrap_or(0);
    if !(0..=1).contains(&time_base) {
        return Some(Err("timeBase is 0 microseconds or 1 nanoticks"));
    }
    /*
     * ADDRESS BY TARGET, NOT BY MODULATOR ID.
     *
     * `kSamplerEnvByTarget` (bit 0) selects HOW the modulator is found: by its target rather
     * than by its id. It was called `kSamplerEnvAmp` when this was written, after the target it
     * usually resolves to, and that name cost two rounds here — backend renamed it. Without it the payload addresses
     * `modulatorId`, and on a freshly loaded kit there is no modulator 0 to address: the command
     * is accepted, applies to nothing, and the slot stays silent. That is exactly how this
     * verb's first version reported success and changed nothing.
     *
     * So it is set unless the caller NAMES a modulator, which is the only case where an id is
     * the thing they meant.
     */
    let by_id = parse_num(body, "\"modulator\"").is_some();
    Some(Ok(UiSamplerEnvelopePayload {
        command_type: UiCommandType::SamplerSetEnvelope as u16,
        flags: if by_id { 0 } else { daw_bridge::layout::SAMPLER_ENV_BY_TARGET },
        track_id: parse_num(body, "\"track\"").unwrap_or(0).max(0) as u32,
        device_id: parse_num(body, "\"device\"").unwrap_or(0).max(0) as u32,
        // 0 = every mod set on that sampler, the engine's own sentinel and the sane default for
        // a kit meant to share one envelope.
        mod_set_id: parse_num(body, "\"modSet\"").unwrap_or(0).max(0) as u32,
        modulator_id: parse_num(body, "\"modulator\"").unwrap_or(0).max(0) as u16,
        time_base: time_base as u8,
        reserved1: 0,
        attack: parse_num(body, "\"attack\"").unwrap_or(0).max(0) as u32,
        decay: parse_num(body, "\"decay\"").unwrap_or(0).max(0) as u32,
        release: parse_num(body, "\"release\"").unwrap_or(0).max(0) as u32,
        sustain_milli: parse_num(body, "\"sustain\"").unwrap_or(1000).clamp(-32768, 32767) as i16,
        rate_milli: parse_num(body, "\"rate\"").unwrap_or(1000).clamp(0, 65535) as u16,
        target: target as u8,
        reserved2: 0,
        depth_milli: parse_num(body, "\"depth\"").unwrap_or(1000).clamp(-32768, 32767) as i16,
    }))
}

/// SamplerSetFilter (86). The mod set's filter: type, and optionally the base cutoff and
/// resonance the modulators move AROUND.
///
/// The two flags are the whole subtlety. The common edit is changing the TYPE on a mod set whose
/// cutoff someone already dialled in, and zero is a LEGAL cutoff rather than a missing one — so
/// "leave it alone" cannot be encoded as a zero value and has to be the absence of a flag. This
/// therefore sets a flag only when the caller actually named the field.
fn build_sampler_filter(body: &str) -> Option<Result<UiSamplerFilterPayload, &'static str>> {
    if !is_type(body, "samplerfilter") { return None; }
    let ftype = parse_num(body, "\"filterType\"").unwrap_or(0);
    if !(0..=4).contains(&ftype) {
        return Some(Err("filter type is 0 off, 1 LP12, 2 LP24, 3 HP, 4 BP"));
    }
    let cutoff = parse_num(body, "\"cutoff\"");
    let resonance = parse_num(body, "\"resonance\"");
    let mut flags = 0u16;
    if cutoff.is_some() { flags |= daw_bridge::layout::SAMPLER_FILTER_SET_CUTOFF; }
    if resonance.is_some() { flags |= daw_bridge::layout::SAMPLER_FILTER_SET_RESONANCE; }
    Some(Ok(UiSamplerFilterPayload {
        command_type: UiCommandType::SamplerSetFilter as u16,
        flags,
        track_id: parse_num(body, "\"track\"").unwrap_or(0).max(0) as u32,
        device_id: parse_num(body, "\"device\"").unwrap_or(0).max(0) as u32,
        // 0 = every mod set on that sampler, which is the engine's own sentinel and the sane
        // default for a kit where one filter setting is meant for the whole thing.
        mod_set_id: parse_num(body, "\"modSet\"").unwrap_or(0).max(0) as u32,
        filter_type: ftype as u8,
        reserved0: 0,
        cutoff_milli: cutoff.unwrap_or(0).clamp(0, 1000) as u16,
        resonance_milli: resonance.unwrap_or(0).clamp(0, 1000) as u16,
        reserved1: 0,
        reserved2: [0u32; 4],
    }))
}

fn build_sampler_slice(body: &str) -> Option<Result<UiSamplerSlicePayload, &'static str>> {
    if !is_type(body, "samplerslice") { return None; }
    let mode = match parse_str(body, "\"mode\"").unwrap_or("equal") {
        "transient" => daw_bridge::layout::SAMPLER_SLICE_TRANSIENT,
        "equal" => daw_bridge::layout::SAMPLER_SLICE_EQUAL,
        "clear" => daw_bridge::layout::SAMPLER_SLICE_CLEAR,
        _ => return Some(Err("slice mode is transient, equal or clear")),
    };
    Some(Ok(UiSamplerSlicePayload {
        command_type: UiCommandType::SamplerSlice as u16,
        mode,
        track_id: parse_num(body, "\"track\"").unwrap_or(0).max(0) as u32,
        device_id: parse_num(body, "\"device\"").unwrap_or(0).max(0) as u32,
        source_local_id: parse_num(body, "\"source\"").unwrap_or(1).max(0) as u32,
        sensitivity: parse_num(body, "\"sensitivity\"").unwrap_or(500).clamp(0, 1000) as u32,
        count: parse_num(body, "\"count\"").unwrap_or(16).clamp(0, 4096) as u32,
        max_slices: parse_num(body, "\"max\"").unwrap_or(0).max(0) as u32,
        snap_nanoticks: parse_num(body, "\"snap\"").unwrap_or(0).max(0) as u32,
        make_slots: if parse_num(body, "\"slots\"").unwrap_or(1) != 0 { 1 } else { 0 },
        slot_base_key: parse_num(body, "\"base\"").unwrap_or(36).clamp(0, 127) as u8,
        reserved: [0u8; 6],
    }))
}

/// One line of agent progress, as JSON the page can render.
///
/// Hand-built rather than serde-derived: this is four fields and the escaping is
/// the only part that matters — a model's prose contains quotes and newlines,
/// and a broken frame here would take down the socket the UI depends on.
fn json_line(kind: &str, text: &str, detail: Option<&str>, ok: bool) -> String {
    let esc = |s: &str| {
        let mut out = String::with_capacity(s.len() + 8);
        for c in s.chars() {
            match c {
                '"' => out.push_str("\\\""),
                '\\' => out.push_str("\\\\"),
                '\n' => out.push_str("\\n"),
                '\r' => out.push_str("\\r"),
                '\t' => out.push_str("\\t"),
                c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
                c => out.push(c),
            }
        }
        out
    };
    match detail {
        Some(d) => format!(
            "{{\"agent\":\"{}\",\"text\":\"{}\",\"detail\":\"{}\",\"ok\":{}}}",
            kind, esc(text), esc(d), ok),
        None => format!("{{\"agent\":\"{}\",\"text\":\"{}\",\"ok\":{}}}",
                        kind, esc(text), ok),
    }
}

fn serve_commands(listener: TcpListener, shm: String, viewport: SharedViewport, projects: String,
                  plugin_cache: String) {
    for stream in listener.incoming().flatten() {
        let shm = shm.clone();
        let viewport = viewport.clone();
        let projects = projects.clone();
        let plugin_cache = plugin_cache.clone();
        thread::spawn(move || {
            // A READ TIMEOUT, so this loop can do two things.
            //
            // An `ask` runs a model on its own thread and reports progress as it
            // goes; the websocket can only be written from here, so the loop has
            // to come up for air rather than sitting in a blocking read. A
            // timeout turns `ws.read()` into "a message, or a moment passed",
            // and both are useful.
            let _ = stream.set_read_timeout(Some(std::time::Duration::from_millis(120)));
            let mut ws = match tungstenite::accept(stream) { Ok(w) => w, Err(_) => return };
            // Progress from an in-flight ask. Bounded only by how fast a model
            // can talk, which is not fast.
            let (ask_tx, ask_rx) = std::sync::mpsc::channel::<ask::Progress>();
            let asking = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false));
            // The conversation, for as long as this tab is open. Per connection
            // rather than per process: two tabs are two conversations, and a
            // reload is a fresh one. See `ask::History`.
            let history = std::sync::Arc::new(std::sync::Mutex::new(ask::History::new()));
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
                // Anything the ask thread has said since last time.
                while let Ok(p) = ask_rx.try_recv() {
                    let msg = match p {
                        ask::Progress::Say(t) => json_line("say", &t, None, true),
                        ask::Progress::Did { tool, args, result, ok } =>
                            json_line("did", &tool, Some(&format!("{args} -> {result}")), ok),
                        ask::Progress::Done(t) => {
                            asking.store(false, Ordering::Release);
                            json_line("done", &t, None, true)
                        }
                        ask::Progress::Failed(t) => {
                            asking.store(false, Ordering::Release);
                            json_line("failed", &t, None, false)
                        }
                    };
                    if ws.send(tungstenite::Message::Text(msg)).is_err() { return; }
                }
                match ws.read() {
                    // A read timeout is not a disconnect: it is this loop's
                    // heartbeat, and the only reason progress reaches the page
                    // while a model is still thinking.
                    Err(tungstenite::Error::Io(ref e))
                        if e.kind() == std::io::ErrorKind::WouldBlock
                            || e.kind() == std::io::ErrorKind::TimedOut => continue,
                    Ok(tungstenite::Message::Text(t)) => {
                        // The engine may have restarted while we were blocked in
                        // read(); re-attach before acting on anything.
                        let g = SHM_GENERATION.load(Ordering::Acquire);
                        if g != generation {
                            match EngineHandle::attach(&shm, true) {
                                Ok(h) => { handle = h; generation = g;
                                           // A new engine is a new document. See
                                           // the note at the verb check below.
                                           history.lock()
                                               .unwrap_or_else(|e| e.into_inner()).clear();
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
                        /*
                         * A DIFFERENT SONG ENDS THE CONVERSATION.
                         *
                         * Ordinary edits do not: if the model raised the bass and
                         * the person then muted a track by hand, "put it back"
                         * still means something, and the fresh shape in the next
                         * prompt says what is true now.
                         *
                         * These four are not ordinary edits. Load and new replace
                         * the document outright; undo and redo move it to a state
                         * nobody narrated. After any of them the transcript
                         * describes a song that does not exist — every track id
                         * it names may now be a different instrument. A model
                         * reading "track 2 is the lead, I gave it a fifth" and
                         * acting on it would be editing at random.
                         *
                         * Checked BEFORE the verbs are dispatched, in one place,
                         * because the four arrive by three different routes:
                         * `new` is handled here, `load` builds a LoadProject, and
                         * undo/redo fall through to the generic command path.
                         */
                        if is_type(&t, "load") || is_type(&t, "new")
                            || is_type(&t, "undo") || is_type(&t, "redo")
                        {
                            let dropped = history.lock()
                                .unwrap_or_else(|e| e.into_inner()).clear();
                            // Said out loud. A chat panel that silently forgets
                            // looks like a chat panel that is ignoring you, and
                            // the next "do that again" would fail for a reason
                            // nothing on screen explains.
                            if dropped {
                                let _ = ws.send(tungstenite::Message::Text(json_line(
                                    "note", "— the song changed; starting a new conversation",
                                    None, true)));
                            }
                            // Falls through: this only clears history, the verb
                            // still has to be done.
                        }
                        // And on purpose. The four above are the cases where
                        // carrying on would be wrong; this is the case where it
                        // is merely unwanted — a new line of thought, or a model
                        // that has talked itself into a corner and needs to stop
                        // being reminded of it.
                        if is_type(&t, "forget") {
                            let dropped = history.lock()
                                .unwrap_or_else(|e| e.into_inner()).clear();
                            let reply = json_line(
                                "note",
                                if dropped { "— conversation cleared" }
                                else { "— nothing to clear" },
                                None, true);
                            if ws.send(tungstenite::Message::Text(reply)).is_err() { break; }
                            continue;
                        }
                        if is_type(&t, "list") {
                            let reply = list_projects(&projects);
                            if ws.send(tungstenite::Message::Text(reply)).is_err() { break; }
                            continue;
                        }
                        /*
                         * "Was this project actually written?" — answered from the FILE.
                         *
                         * A save's outcome reaches the engine's log and nothing else, so an
                         * ack says the command was queued and no more. The chrome asks this
                         * after a save and prints the file's own age, which is a fact about
                         * the artefact rather than a claim about a message.
                         */
                        /*
                         * ONE AUTOMATION LANE'S POINTS, on request.
                         *
                         *   {"type":"automation","track":0,"param":"cutoff"}
                         *
                         * Request/answer rather than a standing region, because a song can hold
                         * far more automation than a frame should carry and the interface only
                         * draws the lanes that are open — the shape backend and I agreed on.
                         *
                         * THE SEQ IS OURS. Slots are reused mod four, so an answer to somebody
                         * else's question is indistinguishable from ours on anything but the
                         * echo: the reply is only accepted when `request_seq`, `track_id` and
                         * `param_id` all match what went out.
                         *
                         * `found: false` is an ANSWER — "nothing automates that parameter" —
                         * and is forwarded as one. A client that treated it as silence would
                         * spin forever on a question that has been answered.
                         */
                        if is_type(&t, "automation") {
                            let track = parse_num(&t, "\"track\"").unwrap_or(0).max(0) as u32;
                            let param = parse_str(&t, "\"param\"").unwrap_or("").to_string();
                            let target = parse_num(&t, "\"target\"")
                                .unwrap_or(u32::MAX as i64) as u32;
                            let reply = request_automation(&handle, track, target, &param);
                            if ws.send(tungstenite::Message::Text(reply)).is_err() { break; }
                            continue;
                        }
                        if is_type(&t, "samplerkit") {
                            let track = parse_num(&t, "\"track\"").unwrap_or(0).max(0) as u32;
                            let device = parse_num(&t, "\"device\"").unwrap_or(0).max(0) as u32;
                            let reply = request_sampler_kit(&handle, track, device);
                            if ws.send(tungstenite::Message::Text(reply)).is_err() { break; }
                            continue;
                        }
                        if is_type(&t, "savedstate") {
                            let name = parse_str(&t, "\"name\"").unwrap_or("").to_string();
                            let reply = saved_state(&projects, &name);
                            if ws.send(tungstenite::Message::Text(reply)).is_err() { break; }
                            continue;
                        }
                        /*
                         * ASK. A sentence instead of a command.
                         *
                         * The model gets daw-agent's tool manifest and operates
                         * the song through it — the same named operations the
                         * console and the CLI use, so it can do what a person can
                         * do and nothing else. Nothing here builds a payload.
                         *
                         * On its OWN THREAD with its own EngineHandle, because a
                         * round trip to the API is hundreds of milliseconds and
                         * this loop is where a person's next edit arrives.
                         * EngineHandle is not Send, so the thread attaches its
                         * own rather than borrowing this one.
                         *
                         * One at a time: two models editing the same song from
                         * the same text box is not a feature anybody asked for,
                         * and the second one would be planning against a document
                         * the first is halfway through changing.
                         */
                        if is_type(&t, "ask") {
                            if asking.swap(true, Ordering::AcqRel) {
                                let _ = ws.send(tungstenite::Message::Text(json_line(
                                    "failed", "still working on the last one", None, false)));
                                continue;
                            }
                            let prompt = parse_str(&t, "\"text\"").unwrap_or("").to_string();
                            if prompt.trim().is_empty() {
                                asking.store(false, Ordering::Release);
                                let _ = ws.send(tungstenite::Message::Text(json_line(
                                    "failed", "ask what?", None, false)));
                                continue;
                            }
                            let _ = ws.send(tungstenite::Message::Text(json_line(
                                "thinking", &prompt, None, true)));
                            let (tx, shm2) = (ask_tx.clone(), shm.clone());
                            let flag = asking.clone();
                            let hist = history.clone();
                            thread::spawn(move || {
                                match daw_agent::AgentSession::attach(&shm2) {
                                    Ok(session) => ask::run(&session, &prompt, &tx, &hist),
                                    Err(e) => {
                                        let _ = tx.send(ask::Progress::Failed(
                                            format!("could not attach to the engine: {e}")));
                                    }
                                }
                                // The Done/Failed arm clears this too; belt and
                                // braces, because a panic in the loop would
                                // otherwise wedge the box forever.
                                flag.store(false, Ordering::Release);
                            });
                            continue;
                        }
                        /*
                         * A NEW SONG.
                         *
                         * Answered here rather than by the engine, and with no new
                         * command type, because the engine already has the only
                         * hard part: LoadProject. An empty document is a file the
                         * sidecar can write — it owns the project directory
                         * already, for `list` — so `new` is "compose the smallest
                         * valid document, write it, load it".
                         *
                         * Before this there was no way to start a song at all. The
                         * only route was to open a preset and overwrite it, so
                         * every project began as somebody else's and inherited its
                         * tempo, meter, track names and devices. It is the first
                         * thing anyone does with a DAW and it was the one thing
                         * the app could not do.
                         *
                         * REFUSES TO CLOBBER. A name that already exists comes back
                         * as an error rather than silently replacing a song — the
                         * one operation you cannot undo is the one that must ask.
                         */
                        if is_type(&t, "new") {
                            let name = parse_str(&t, "\"name\"").unwrap_or("untitled");
                            let reply = match new_project(&projects, name) {
                                Ok(()) => {
                                    // Load it through the ordinary path, so a new
                                    // song arrives by exactly the same route as an
                                    // opened one and nothing downstream has a
                                    // second case to handle.
                                    let p = UiPatcherPresetCommandPayload::named(
                                        UiCommandType::LoadProject, name);
                                    let _ = handle.send_command(p.as_command());
                                    format!("{{\"ok\":true,\"new\":\"{name}\"}}")
                                }
                                Err(e) => format!("{{\"ok\":false,\"error\":\"{e}\"}}"),
                            };
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
                                // The GLOBAL counter, kept only to wait on below: it
                                // moves on any accepted edit, so it is still the
                                // "did that land" signal even though it is no longer
                                // the base anything is arbitrated against.
                                let global_before = handle.clip_version();
                                // Read per OP, from the op's OWN track: a batch can
                                // span tracks (a transpose across a selection does),
                                // and the counters diverge, so one base for the whole
                                // frame is right for at most one of them.
                                let sent = if let Some(c) = build_chord(line) {
                                    let mut c = c;
                                    c.base_version = handle.clip_version_for_track(c.track_id);
                                    handle.send_chord_command(c).is_ok()
                                } else {
                                    match build_command(line) {
                                        Ok(mut p) => {
                                            p.base_version = resolve_base(
                                                &handle, p.track_id, 0, p.command_type);
                                            handle.send_command(p).is_ok()
                                        }
                                        Err(_) => false,
                                    }
                                };
                                if !sent { failed += 1; continue; }
                                // Wait for the engine to actually apply it. Without
                                // this the next op re-reads the same version and we
                                // are back to the race we are fixing.
                                if handle.wait_for_clip_version(
                                    global_before, global_before.wrapping_add(1),
                                    Duration::from_millis(250)) { ok += 1; } else { failed += 1; }
                            }
                            let reply = format!("{{\"ok\":true,\"applied\":{ok},\"failed\":{failed}}}");
                            if ws.send(tungstenite::Message::Text(reply)).is_err() { break; }
                            continue;
                        }

                        // A track's routing. Own 40-byte payload, matched on
                        // size by the engine before commandType is read.
                        if let Some(r) = build_routing(&t) {
                            let reply = match r {
                                Err(why) => format!("{{\"error\":\"{why}\"}}"),
                                Ok(p) => match handle.send_routing_command(p) {
                                    Ok(()) => format!(
                                        "{{\"ok\":true,\"routing\":{},\"kind\":{},\"to\":{}}}",
                                        p.track_id, p.audio_out_kind, p.audio_out_track_id),
                                    Err(e) => format!("{{\"error\":\"{}\"}}", e.replace('"', "'")),
                                },
                            };
                            if ws.send(tungstenite::Message::Text(reply)).is_err() { break; }
                            continue;
                        }

                        // The parameter a link NAMES, and the knob that drives it. Two
                        // more 40-byte payloads, and both are load-bearing: without them
                        // a link is accepted, published, drawn and inert.
                        if let Some(r) = build_mod_uid(&t) {
                            let reply = match r {
                                Err(why) => format!("{{\"error\":\"{why}\"}}"),
                                Ok(p) => match handle.send_mod_link_uid16(p) {
                                    Ok(()) => format!("{{\"ok\":true,\"moduid\":{}}}", p.link_id),
                                    Err(e) => format!("{{\"error\":\"{}\"}}", e.replace('"', "'")),
                                },
                            };
                            if ws.send(tungstenite::Message::Text(reply)).is_err() { break; }
                            continue;
                        }
                        if let Some(r) = build_mod_source(&t) {
                            let reply = match r {
                                Err(why) => format!("{{\"error\":\"{why}\"}}"),
                                Ok(p) => match handle.send_mod_source_value(p) {
                                    Ok(()) => format!(
                                        "{{\"ok\":true,\"modsource\":{},\"value\":{}}}",
                                        p.source_id, p.value),
                                    Err(e) => format!("{{\"error\":\"{}\"}}", e.replace('"', "'")),
                                },
                            };
                            if ws.send(tungstenite::Message::Text(reply)).is_err() { break; }
                            continue;
                        }

                        // ONE AUTOMATION POINT. Own 40-byte payload again.
                        if let Some(r) = build_automation_point(&t) {
                            let reply = match r {
                                Err(why) => format!("{{\"error\":\"{why}\"}}"),
                                Ok(p) => match handle.send_automation_point(p) {
                                    Ok(()) => format!(
                                        "{{\"ok\":true,\"autopoint\":{},\"value\":{}}}",
                                        p.track_id, p.value),
                                    Err(e) => format!("{{\"error\":\"{}\"}}", e.replace('"', "'")),
                                },
                            };
                            if ws.send(tungstenite::Message::Text(reply)).is_err() { break; }
                            continue;
                        }

                        // ONE DEVICE-LEVEL FIELD. Own 40-byte payload.
                        if let Some(r) = build_sampler_device(&t) {
                            let reply = match r {
                                Err(why) => format!("{{\"error\":\"{why}\"}}"),
                                Ok(p) => match handle.send_sampler_set_device(p) {
                                    Ok(()) => format!(
                                        "{{\"ok\":true,\"samplerdevice\":{},\"field\":{},\"value\":{}}}",
                                        p.device_id, p.field, p.value),
                                    Err(e) => format!("{{\"error\":\"{}\"}}", e.replace('"', "'")),
                                },
                            };
                            if ws.send(tungstenite::Message::Text(reply)).is_err() { break; }
                            continue;
                        }

                        // ONE FIELD OF ONE SLOT. Own 40-byte payload.
                        if let Some(r) = build_sampler_slot(&t) {
                            let reply = match r {
                                Err(why) => format!("{{\"error\":\"{why}\"}}"),
                                Ok(p) => match handle.send_sampler_set_slot(p) {
                                    Ok(()) => format!(
                                        "{{\"ok\":true,\"samplerslot\":{},\"field\":{},\"value\":{}}}",
                                        p.slot_id, p.field, p.value),
                                    Err(e) => format!("{{\"error\":\"{}\"}}", e.replace('"', "'")),
                                },
                            };
                            if ws.send(tungstenite::Message::Text(reply)).is_err() { break; }
                            continue;
                        }

                        // AN ENVELOPE ON A MOD SET. Own 40-byte payload.
                        if let Some(r) = build_sampler_envelope(&t) {
                            let reply = match r {
                                Err(why) => format!("{{\"error\":\"{why}\"}}"),
                                Ok(p) => match handle.send_sampler_envelope(p) {
                                    Ok(()) => format!(
                                        "{{\"ok\":true,\"samplerenv\":{},\"target\":{}}}",
                                        p.device_id, p.target),
                                    Err(e) => format!("{{\"error\":\"{}\"}}", e.replace('"', "'")),
                                },
                            };
                            if ws.send(tungstenite::Message::Text(reply)).is_err() { break; }
                            continue;
                        }

                        // THE FILTER ON A MOD SET. Own 40-byte payload.
                        if let Some(r) = build_sampler_filter(&t) {
                            let reply = match r {
                                Err(why) => format!("{{\"error\":\"{why}\"}}"),
                                Ok(p) => match handle.send_sampler_filter(p) {
                                    Ok(()) => format!(
                                        "{{\"ok\":true,\"samplerfilter\":{},\"type\":{}}}",
                                        p.device_id, p.filter_type),
                                    Err(e) => format!("{{\"error\":\"{}\"}}", e.replace('"', "'")),
                                },
                            };
                            if ws.send(tungstenite::Message::Text(reply)).is_err() { break; }
                            continue;
                        }

                        // CHOP A SOURCE. Own 40-byte payload again.
                        if let Some(r) = build_sampler_slice(&t) {
                            let reply = match r {
                                Err(why) => format!("{{\"error\":\"{why}\"}}"),
                                Ok(p) => match handle.send_sampler_slice(p) {
                                    Ok(()) => format!(
                                        "{{\"ok\":true,\"samplerslice\":{},\"count\":{}}}",
                                        p.device_id, p.count),
                                    Err(e) => format!("{{\"error\":\"{}\"}}", e.replace('"', "'")),
                                },
                            };
                            if ws.send(tungstenite::Message::Text(reply)).is_err() { break; }
                            continue;
                        }

                        // LOAD A SAMPLE. Own 40-byte payload again.
                        if let Some(r) = build_sampler_load(&t) {
                            let reply = match r {
                                Err(why) => format!("{{\"error\":\"{why}\"}}"),
                                Ok(p) => match handle.send_sampler_load(p) {
                                    Ok(()) => format!(
                                        "{{\"ok\":true,\"samplerload\":{}}}", p.device_id),
                                    Err(e) => format!("{{\"error\":\"{}\"}}", e.replace('"', "'")),
                                },
                            };
                            if ws.send(tungstenite::Message::Text(reply)).is_err() { break; }
                            continue;
                        }

                        // ROW OPS on one note. Own 40-byte payload again.
                        if let Some(r) = build_set_row_ops(&t) {
                            let reply = match r {
                                Err(why) => format!("{{\"error\":\"{why}\"}}"),
                                Ok(p) => match handle.send_set_row_ops(p) {
                                    Ok(()) => format!(
                                        "{{\"ok\":true,\"setrowops\":{},\"mask\":{}}}",
                                        p.note_id_lo, p.mask),
                                    Err(e) => format!("{{\"error\":\"{}\"}}", e.replace('"', "'")),
                                },
                            };
                            if ws.send(tungstenite::Message::Text(reply)).is_err() { break; }
                            continue;
                        }

                        // WHAT MODULATES WHAT. Own 40-byte payload again.
                        if let Some(r) = build_mod(&t) {
                            let reply = match r {
                                Err(why) => format!("{{\"error\":\"{why}\"}}"),
                                Ok(p) => match handle.send_mod_link_command(p) {
                                    Ok(()) => format!(
                                        "{{\"ok\":true,\"mod\":{},\"type\":{},\"depth\":{}}}",
                                        p.link_id, p.command_type, p.depth),
                                    Err(e) => format!("{{\"error\":\"{}\"}}", e.replace('"', "'")),
                                },
                            };
                            if ws.send(tungstenite::Message::Text(reply)).is_err() { break; }
                            continue;
                        }

                        // A MARKER: a named tick. Own 40-byte payload again.
                        if let Some(r) = build_marker(&t) {
                            let reply = match r {
                                Err(why) => format!("{{\"error\":\"{why}\"}}"),
                                Ok(p) => match handle.send_marker_command(p) {
                                    Ok(()) => format!(
                                        "{{\"ok\":true,\"marker\":{},\"type\":{}}}",
                                        p.marker_id, p.command_type),
                                    Err(e) => format!("{{\"error\":\"{}\"}}", e.replace('"', "'")),
                                },
                            };
                            if ws.send(tungstenite::Message::Text(reply)).is_err() { break; }
                            continue;
                        }
                        /*
                         * TIME ITSELF — the ripple, and the meter map.
                         *
                         * This is what the spine strip's boundary drag sends. It is checked after
                         * `build_marker` and before everything else because `is_type(&t, "time")`
                         * would otherwise be claimed by nothing at all: no earlier branch matches
                         * it, and a message that falls through every branch is dropped in silence.
                         */
                        if let Some(r) = build_arrange_time(&t) {
                            let reply = match r {
                                Err(why) => format!("{{\"error\":\"{why}\"}}"),
                                Ok(p) => match handle.send_arrange_time_command(p) {
                                    Ok(()) => format!(
                                        "{{\"ok\":true,\"time\":{},\"delta\":{}}}",
                                        p.command_type, p.delta),
                                    Err(e) => format!("{{\"error\":\"{}\"}}", e.replace('"', "'")),
                                },
                            };
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
                                Ok(mut p) => {
                                /*
                                 * NAME THE DEVICE THAT OWNS THE NODE, or the edit goes to the pool.
                                 *
                                 * Opcode 28 edits `patcherGraphState` unless it is told otherwise,
                                 * and since patcher-is-a-device the graph a project SAVES lives on
                                 * the device. Sent pool-scoped, a nudge is heard (the pool is what
                                 * the producer reads), drawn, and lost on the next load — with the
                                 * command reporting success throughout.
                                 *
                                 * The owner is resolved HERE rather than asked of the caller: the
                                 * region publishes the assembled POOL, so "which device is this
                                 * region" has no answer and only "which device owns this node"
                                 * does. That fact is published per node (`ownerDeviceId`), so the
                                 * UI names a node — which is what it has — and this names the
                                 * device.
                                 *
                                 * Owner 0 means a pool node with no owning device, which is the
                                 * legacy single-graph case: leave the flag clear and the edit goes
                                 * where it always went.
                                 */
                                let owner = handle.read_patcher().nodes.iter()
                                    .find(|n| n.id == p.node_id)
                                    .map(|n| n.owner_device_id)
                                    .unwrap_or(0);
                                if owner != 0 {
                                    p.flags = daw_bridge::layout::UI_PATCHER_FLAG_HAS_DEVICE_ID
                                            | (owner & 0x7FFF);
                                }
                                match handle.send_patcher_config(p) {
                                    Ok(()) => format!(
                                        "{{\"ok\":true,\"type\":{},\"node\":{},\"device\":{}}}",
                                        p.command_type, p.node_id, owner),
                                    Err(e) => format!("{{\"error\":\"{}\"}}", e.replace('"', "'")),
                                }
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
                            let mut c = c;
                            c.base_version = resolve_base(&handle, c.track_id,
                                                          c.base_version, c.command_type);
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
                            Ok(mut p) => {
                                p.base_version = resolve_base(&handle, p.track_id,
                                                              p.base_version, p.command_type);
                                match handle.send_command(p) {
                                Ok(()) => format!("{{\"ok\":true,\"type\":{}}}", p.command_type),
                                Err(e) => format!("{{\"error\":\"{}\"}}", e.replace('"', "'")),
                            }},
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
                /*
                 * WHAT THE PARAMETER IS, not just where it is (v30).
                 *
                 * Without these a rack can read "Cutoff is 0.62, displays 440 Hz" and cannot know
                 * what 0.0 and 1.0 mean, whether it is a switch, or what to reset it to — so a
                 * person setting a value in real units is doing a binary search against the
                 * display string, which is a guessing loop rather than an interface.
                 *
                 * `minText`/`maxText` are the endpoints AS THE PLUGIN RENDERS THEM, and they are
                 * the load-bearing pair: for a VST3 through JUCE the normalisable range is 0..1,
                 * so `min`/`max` say nothing at all and the real range exists ONLY as that text.
                 *
                 * `automatable: false` means the plugin will IGNORE an automation lane pointed at
                 * it — so drawing one would be a lie, and the rack needs to know before it offers.
                 */
                let esc = |t: &str| t.replace('\\', "").replace('"', "'");
                let ps: Vec<String> = dp.params.iter().map(|q| format!(
                    "{{\"index\":{},\"value\":{},\"name\":\"{}\",\"display\":\"{}\",\
                     \"uid\":\"{}\",\"unit\":\"{}\",\"minText\":\"{}\",\"maxText\":\"{}\",\
                     \"default\":{},\"steps\":{},\"discrete\":{},\"automatable\":{}}}",
                    q.index, q.value,
                    esc(&q.name), esc(&q.display),
                    q.uid16.iter().map(|b| format!("{b:02x}")).collect::<String>(),
                    esc(&q.unit), esc(&q.min_text), esc(&q.max_text),
                    // Finite or 0: a NaN default from a misbehaving plugin would produce JSON the
                    // page cannot parse, and the failure would be the whole rack going blank
                    // rather than one number looking wrong.
                    if q.default_value.is_finite() { q.default_value } else { 0.0 },
                    q.step_count, q.discrete, q.automatable)).collect();
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
// Not Copy any more: a device owns its bus list. The store REUSES devices across
// snapshots rather than reallocating, so the Vec is cleared, not dropped.
#[derive(Clone, Debug, Default, PartialEq)]
struct ChainDevice {
    id: u32,
    kind: u32,
    pos: u32,
    node: u32,
    slot: u32,
    caps: u32,
    bypass: u32,
    /// This device's own patcher graph emits events. See UI_CHAIN_DIFF_GENERATES.
    generates: bool,
    /// How many DeviceBus diffs the engine says are coming for this device, and
    /// whether it had more than the cap. Read from the ChainSnapshot's `flags`, not
    /// its payload, which is full.
    ///
    /// This is the field that lets the rack draw ONCE. Without it, three buses
    /// received out of four is indistinguishable from a device that has three, so
    /// the rack draws three and rearranges when the fourth lands — which is the
    /// stereo-then-rearrange this whole design exists to avoid.
    bus_count: u8,
    bus_truncated: bool,
    /// Reused across snapshots, cleared rather than dropped, like `devices`.
    buses: Vec<DeviceBus>,
}

/// One audio bus of a hosted plugin (v20).
#[derive(Clone, Debug, Default, PartialEq)]
struct DeviceBus {
    is_input: bool,
    is_main: bool,
    enabled: bool,
    index: u8,
    channel_count: u8,
    layout_id: u16,
    channel_offset: u16,
    name: String,
}

/// One ChainSnapshot entry off the ring: which track and which version it
/// belongs to, plus the device it describes — `None` for the empty-chain
/// sentinel, which is a statement about the chain rather than a device in it.
#[derive(Clone, Debug)]
struct ChainEntry {
    track: u32,
    version: u32,
    device: Option<ChainDevice>,
}

/// One DeviceBus diff off the ring, still keyed by the device it belongs to.
#[derive(Clone, Debug)]
struct BusEntry {
    track: u32,
    device: u32,
    bus: DeviceBus,
}

/// Where a track's audio and events go.
///
/// Kinds mirror daw::TrackRouteKind — 0 none, 1 master, 2 another track,
/// 3 an external input. `audio_out` is the one a person changes: "this track
/// goes to Main" versus "this track goes into that group".
#[derive(Clone, Copy, Debug, Default, PartialEq)]
struct TrackRouting {
    audio_out_kind: u8,
    audio_out_track: u32,
    audio_in_kind: u8,
    audio_in_track: u32,
    midi_out_kind: u8,
    midi_out_track: u32,
    pre_fader_send: bool,
}

/// A track's chain as accumulated so far, plus the version it was published at.
#[derive(Default)]
struct TrackChain {
    track: u32,
    version: u32,
    /// The engine publishes routing as its own diff on the same ring, and it is
    /// per-track state exactly as the chain is. Kept HERE rather than in a
    /// parallel store: one revision, one message, and the page gets "what this
    /// track's chain is" and "where its output goes" in the same breath —
    /// which is how anyone reading a mixer thinks about them.
    ///
    /// `None` until the engine has said; a track whose routing has not arrived
    /// is not a track routed to nothing.
    routing: Option<TrackRouting>,
    /// Reused across snapshots — a replacement clears this rather than dropping
    /// it, so a chain that is re-published on every device edit allocates once.
    devices: Vec<ChainDevice>,
    /// The engine's first version is 1, so 0 would do as "never seen" — but that
    /// is the engine's counter's business, not ours, and a store that silently
    /// depends on it breaks the day the counter starts somewhere else.
    seen: bool,
    /// WHAT MODULATES WHAT on this track. Here rather than in a parallel store for the
    /// reason `routing` gives: it is per-track state arriving on the same ring, and a
    /// modulation link is a fact about this track's RACK — the page gets what the chain
    /// is, where it goes and what moves what in one message.
    mod_links: Vec<ModLink>,
    /// The engine's mod version this set was published at. A snapshot is a REPLACEMENT
    /// keyed on it, exactly as the device list is keyed on `version`.
    mod_version: u32,
}

/// Per-track device chains, accumulated from the engine's ChainSnapshot diffs.
///
/// Shared for the same reason `EngineEvents` is: the out ring is SINGLE CONSUMER,
/// so one thread drains it and every client reads what that thread accumulated.
/// A per-client drain would hand each browser tab a different subset of the SAME
/// snapshot's entries, and every tab would render a chain that is short by a
/// device or two — plausible, silent, and different in each window.
/// ONE MODULATION LINK, as the engine published it.
///
/// A link is "this source moves that parameter": a macro knob, an LFO, an envelope or a
/// patcher node's output, driving a VST parameter or a patcher one. `flags` packs the four
/// small enums the payload had no room for — bits 0-3 source kind, 4-7 target kind, 8-9
/// rate, bit 10 enabled — and is forwarded UNPACKED so the page never learns the bit
/// layout.
///
/// `uid16` arrives as its OWN diff, right after the link, and only for a VST target. It is
/// the 16-byte plugin parameter id, which is what the rack's parameter rows are keyed on —
/// without it a link knows it targets "parameter 7 of device 3" and the rack cannot tell
/// which row that is, because the row order is the plugin's and the target id is not an
/// index into it.
#[derive(Clone, Debug, Default)]
struct ModLink {
    link_id: u32,
    source_device: u32,
    source_id: u32,
    target_device: u32,
    target_id: u32,
    depth: f32,
    bias: f32,
    source_kind: u16,
    target_kind: u16,
    rate: u16,
    enabled: bool,
    /// Empty until the ModLinkUid16 diff for this link arrives; non-VST targets have none.
    uid16: String,
}

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
    /// Fold one MOD SNAPSHOT entry in. A replacement keyed on (track, mod_version).
    ///
    /// The engine emits one entry per link and stamps them all with one version, so the
    /// first entry of a NEW version has to throw the previous version's set away —
    /// appending would show every link twice after the first edit, which is the exact
    /// mistake `apply` above documents for devices and renders just as plausibly.
    ///
    /// ONE CASE THIS CANNOT SEE, and it is the engine's to fix: a track whose registry is
    /// now EMPTY publishes nothing at all. `emitModSnapshot` iterates the links, so zero
    /// links means zero entries — the version moves and nothing carries it here. Removing
    /// one link of several is therefore correct (the rest republish under a new version),
    /// and removing the LAST one is invisible. Asked for the same sentinel entry chains
    /// already use for an empty chain; until then the page drops the last link locally
    /// and says why it is allowed to.
    fn apply_mod(&self, track: u32, version: u32, link: ModLink) {
        let mut tracks = self.tracks.lock().unwrap();
        let at = match tracks.iter().position(|t| t.track == track) {
            Some(i) => i,
            None => { tracks.push(TrackChain { track, ..Default::default() }); tracks.len() - 1 }
        };
        let t = &mut tracks[at];
        if t.mod_version != version {
            t.mod_version = version;
            t.mod_links.clear();
        }
        /*
         * MOD_LINK_ID_AUTO IN A SNAPSHOT IS NOT A LINK. It is the engine saying "this track has
         * no links" — one entry so the VERSION still travels, exactly as `kDeviceIdAuto` does
         * for an empty chain.
         *
         * Without it an emptied registry published nothing at all, so removing a track's LAST
         * link was invisible and the page had to drop it locally to avoid a lit badge over a
         * link that no longer existed. Asked for, added, and that workaround is gone.
         *
         * Taken as a link it would leave every unmodulated track showing a phantom with id
         * 4294967295 — the chain decoder's comment says the same thing about the same mistake.
         */
        if link.link_id == MOD_LINK_ID_AUTO {
            self.revision.fetch_add(1, Ordering::AcqRel);
            return;
        }
        // Keyed on link_id within the version, because a snapshot run can be interleaved
        // with the uid16 diffs that belong to it and a re-published link must replace
        // rather than duplicate.
        match t.mod_links.iter().position(|l| l.link_id == link.link_id) {
            Some(i) => t.mod_links[i] = link,
            None => t.mod_links.push(link),
        }
        self.revision.fetch_add(1, Ordering::AcqRel);
    }

    /// Name the VST parameter a link targets. Its own diff, so its own fold.
    ///
    /// Ignored when the link is unknown, and COUNTED as not-forwarded by the caller: the
    /// ring is ordered and the link comes first, so a uid16 for a link we never saw can
    /// only mean the link entry was lost — worth seeing in the histogram rather than
    /// quietly attaching to nothing.
    fn apply_mod_uid16(&self, track: u32, version: u32, link_id: u32, uid16: String) -> bool {
        let mut tracks = self.tracks.lock().unwrap();
        let Some(at) = tracks.iter().position(|t| t.track == track) else { return false; };
        let t = &mut tracks[at];
        if t.mod_version != version { return false; }
        let Some(i) = t.mod_links.iter().position(|l| l.link_id == link_id) else { return false; };
        if t.mod_links[i].uid16 == uid16 { return true; }
        t.mod_links[i].uid16 = uid16;
        self.revision.fetch_add(1, Ordering::AcqRel);
        true
    }

    /// Fold one routing snapshot in, and say whether it changed anything.
    ///
    /// Guarded on equality so a track that republishes identical routing does
    /// not bump the revision — the engine re-emits on load for every track, and
    /// an unconditional bump would push the whole chain set to every client on
    /// every load for no change at all.
    fn apply_routing(&self, track: u32, routing: TrackRouting) -> bool {
        let mut tracks = self.tracks.lock().unwrap();
        let at = match tracks.iter().position(|t| t.track == track) {
            Some(i) => i,
            None => {
                tracks.push(TrackChain { track, ..Default::default() });
                tracks.len() - 1
            }
        };
        if tracks[at].routing == Some(routing) { return false; }
        tracks[at].routing = Some(routing);
        self.revision.fetch_add(1, Ordering::AcqRel);
        true
    }

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
        match entry.device.clone() {
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
                "{{\"track\":{},\"version\":{},", t.track, t.version));
            // Absent when the engine has not said. A page that defaulted this to
            // "master" would show every track routed to Main before the first
            // snapshot arrives, and a track someone had sent to a group would
            // read as sent to Main until it happened to republish.
            match t.routing {
                Some(r) => out.push_str(&format!(
                    "\"routing\":{{\"audioOutKind\":{},\"audioOutTrack\":{},                     \"audioInKind\":{},\"audioInTrack\":{},                     \"midiOutKind\":{},\"midiOutTrack\":{},\"preFaderSend\":{}}},",
                    r.audio_out_kind, r.audio_out_track, r.audio_in_kind, r.audio_in_track,
                    r.midi_out_kind, r.midi_out_track, r.pre_fader_send)),
                None => out.push_str("\"routing\":null,"),
            }
            out.push_str("\"devices\":[");
            for (j, d) in t.devices.iter().enumerate() {
                if j > 0 { out.push(','); }
                out.push_str(&format!(
                    "{{\"id\":{},\"kind\":{},\"pos\":{},\"node\":{},\"slot\":{},\
                     \"caps\":{},\"bypass\":{},\"generates\":{},\"busCount\":{},\"busTruncated\":{},\"buses\":[",
                    d.id, d.kind, d.pos, d.node, d.slot, d.caps, d.bypass, d.generates,
                    d.bus_count, d.bus_truncated));
                for (k, b) in d.buses.iter().enumerate() {
                    if k > 0 { out.push(','); }
                    // The name is the ONLY free text on this wire, and it comes from
                    // a plugin. Escaped rather than trusted: a bus called
                    // `Out "A"` would otherwise produce JSON the page cannot parse,
                    // and the failure would be the whole rack going blank rather
                    // than one label looking odd.
                    let name: String = b.name.chars()
                        .filter(|c| *c >= ' ' && *c != '"' && *c != '\\')
                        .collect();
                    out.push_str(&format!(
                        "{{\"input\":{},\"main\":{},\"enabled\":{},\"index\":{},\
                         \"channels\":{},\"layoutId\":{},\"offset\":{},\"name\":\"{}\"}}",
                        b.is_input, b.is_main, b.enabled, b.index,
                        b.channel_count, b.layout_id, b.channel_offset, name));
                }
                out.push_str("]}");
            }
            out.push_str("],\"modVersion\":");
            out.push_str(&t.mod_version.to_string());
            out.push_str(",\"modLinks\":[");
            for (j, l) in t.mod_links.iter().enumerate() {
                if j > 0 { out.push(','); }
                /*
                 * Unpacked. The page is told `sourceKind`, `targetKind`, `rate` and
                 * `enabled` as named fields rather than a flags word, so the bit layout
                 * stays on this side of the wire — it is the engine's packing decision,
                 * made because the payload was full, and nothing in a renderer should
                 * have to know that the rate lives in bits 8 and 9.
                 *
                 * `uid16` is hex and may be empty: a non-VST target has none, and a VST
                 * one has none YET until its own diff arrives. Empty and absent are the
                 * same thing here and both mean "do not try to match a parameter row".
                 */
                out.push_str(&format!(
                    "{{\"id\":{},\"sourceDevice\":{},\"sourceId\":{},\
                     \"targetDevice\":{},\"targetId\":{},\"depth\":{},\"bias\":{},\
                     \"sourceKind\":{},\"targetKind\":{},\"rate\":{},\"enabled\":{},\
                     \"uid16\":\"{}\"}}",
                    l.link_id, l.source_device, l.source_id, l.target_device, l.target_id,
                    // Finite or 0: a NaN depth from a corrupt payload would produce JSON
                    // the page cannot parse, and the failure would be the whole rack
                    // going blank rather than one number looking wrong.
                    if l.depth.is_finite() { l.depth } else { 0.0 },
                    if l.bias.is_finite() { l.bias } else { 0.0 },
                    l.source_kind, l.target_kind, l.rate, l.enabled, l.uid16));
            }
            out.push_str("]}");
        }
        out.push_str(&format!("],\"rev\":{rev}}}"));
        Some((out, rev))
    }

    /// Attach one bus to the device it belongs to.
    ///
    /// A device's bus set is REPLACED by its ChainSnapshot, never merged into —
    /// backend wrote that rule into shared_memory.h at my asking, and it matters
    /// because device ids are REUSED. Without it a new plugin inherits the previous
    /// occupant's buses and draws a rack that is entirely plausible and wrong.
    /// `apply` already clears `devices` on a new version, so the buses go with them
    /// and this only ever appends to a set the current snapshot started.
    ///
    /// A bus for a device we have not seen is DROPPED, not buffered. The engine
    /// emits the snapshot first and the ring is ordered, so out-of-order here means
    /// the snapshot was lost — and holding buses for a device that may never arrive
    /// is how a store grows without bound while looking healthy.
    fn apply_bus(&self, entry: &BusEntry) -> bool {
        let mut tracks = self.tracks.lock().unwrap();
        let Some(t) = tracks.iter_mut().find(|t| t.track == entry.track) else { return false };
        let Some(d) = t.devices.iter_mut().find(|d| d.id == entry.device) else { return false };
        // Idempotent on (direction, index): the engine re-emits a device's buses on
        // every republish, and a ring that redelivers must not double the list.
        if let Some(slot) = d.buses.iter_mut()
            .find(|b| b.is_input == entry.bus.is_input && b.index == entry.bus.index) {
            *slot = entry.bus.clone();
        } else {
            d.buses.push(entry.bus.clone());
        }
        drop(tracks);
        self.revision.fetch_add(1, Ordering::AcqRel);
        true
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
            // From the PAYLOAD's `flags` at offset 2 — UiChainDiffPayload's own
            // field — not from EventEntry.flags. I read the wrong struct first and
            // the test helper encoded the same misreading, so the two agreed with
            // each other and both were wrong; what caught it was a real plugin
            // reporting two buses under a busCount of zero. The mask constants are
            // u16 (kUiChainDiffBusCountMask), which is the tell: EventEntry.flags
            // is u32.
            bus_count: (u16at(2) & 0x00ff) as u8,
            bus_truncated: (u16at(2) & 0x0100) != 0,
            // Bit 9 of the same field: this device's patcher graph EMITS events
            // it was not given. Per device, which is the whole point — the page
            // used to attribute "generates" to device 0 of whichever track,
            // because the published patcher region names no device and never
            // did. So a generator on slot 3 was reported on slot 0, and on a
            // track with no generator at all it was reported anyway.
            generates: (u16at(2) & daw_bridge::layout::UI_CHAIN_DIFF_GENERATES) != 0,
            buses: Vec::new(),
        }),
    })
}

/// Decode one DeviceBus diff (v20), or None if this entry is not one.
///
/// `name` is nul-PADDED and an exactly-22-character name carries NO terminator, so
/// the scan is bounded by the field width and stops at the first nul. Scanning for
/// a terminator instead walks off the end of the payload into the next field, which
/// on a full-width name is guaranteed rather than unlucky.
fn decode_device_bus(e: &EventEntry) -> Option<BusEntry> {
    if (e.size as usize) < 40 { return None; }
    let p = &e.payload;
    let u16at = |i: usize| u16::from_le_bytes([p[i], p[i + 1]]);
    let u32at = |i: usize| u32::from_le_bytes([p[i], p[i + 1], p[i + 2], p[i + 3]]);
    if u16at(0) != UiDiffType::DeviceBus as u16 { return None; }
    let flags = u16at(2);
    let mut name = String::new();
    for k in 0..22 {
        let c = p[18 + k];
        if c == 0 { break; }
        name.push(c as char);
    }
    Some(BusEntry {
        track: u32at(4),
        device: u32at(8),
        bus: DeviceBus {
            is_input: (flags & 1) != 0,
            is_main: (flags & 2) != 0,
            enabled: (flags & 4) != 0,
            index: p[12],
            channel_count: p[13],
            layout_id: u16at(14),
            channel_offset: u16at(16),
            name,
        },
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
    /*
     * A REFUSED EDIT SAYS SO (UiDiffType::ClipRejected = 15).
     *
     * Until backend shipped this, a clip edit with a stale base was dropped in
     * silence — no event, no code, nothing on the ring. Every symptom was "the
     * app does nothing", which is how M2.17's per-track versioning broke note
     * entry on every track but one and cost four suite failures that named
     * nothing between them.
     *
     * `current` is the value to RETRY WITH, which is why it is forwarded rather
     * than reduced to a message: it is the difference between a report and a fix.
     * The two reasons are separate because what to do about them differs — a
     * stale base means re-read and retry, an unknown track means the caller is
     * addressing something that is not there and retrying will never help.
     */
    if diff == 15 {
        let reason = u16at(2);
        return Some(format!(
            "{{\"kind\":\"clip-rejected\",\"reason\":{reason},\"track\":{track},\
              \"sentBase\":{},\"currentBase\":{},\"command\":{}}}",
            u32at(8), u32at(12), u16at(16)));
    }
    /*
     * A REFUSED SAMPLER COMMAND SAYS SO (UiDiffType::SamplerRejected = 17).
     *
     * Twenty sites across seven verbs refused into the engine's LOG and nowhere else — envelope
     * 4, emit 4, slice 3, filter 3, set_slot 2, load 2, lfo 2. daw-cli can read the engine's
     * stderr and a browser cannot, so from here every one of them was a command that reported
     * success and did nothing. I found one of the twenty by accident, because the sound kept
     * playing: `slot 0` is not a wildcard, the engine answered `no_such_slot`, and the note ran
     * the full eight seconds while the console said it had worked.
     *
     * `commandType` is what was sent, so a caller can match a refusal to the command that caused
     * it. `targetId` is the ONE id that could not be found, chosen by `reason` — a slot, a mod
     * set, a modulator, a source or a slice set. One field rather than five because exactly one
     * of them is ever the answer, and five parallel ids would be four chances to disagree.
     *
     * The reason travels as a NUMBER rather than as a sentence: what to do differs per reason —
     * `no_such_slot` means stop and re-read the kit, `bad_value` means the caller clamped wrong
     * and retrying identically will never help — and the surface that shows it is better placed
     * to word that than this layer is.
     */
    if diff == 17 {
        let reason = u16at(2);
        return Some(format!(
            "{{\"kind\":\"sampler-rejected\",\"reason\":{reason},\"command\":{},              \"target\":{},\"track\":{},\"device\":{}}}",
            u16at(4), u16at(6), u32at(8), u32at(12)));
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
/// Read a ModSnapshot diff, or None if this entry is not one.
///
/// Offsets, not a struct, like every other reader here: `UiModLinkDiffPayload` is
/// diff_type u16@0, flags u16@2, track_id u32@4, mod_version u32@8, link_id u32@12,
/// source_device u32@16, source_id u32@20, target_device u32@24, target_id u32@28,
/// depth f32@32, bias f32@36 — 40 bytes, the whole payload. A short entry is refused
/// rather than read past.
fn decode_mod_snapshot(e: &EventEntry) -> Option<(u32, u32, ModLink)> {
    if (e.size as usize) < 40 { return None; }
    let p = &e.payload;
    let u16at = |i: usize| u16::from_le_bytes([p[i], p[i + 1]]);
    let u32at = |i: usize| u32::from_le_bytes([p[i], p[i + 1], p[i + 2], p[i + 3]]);
    let f32at = |i: usize| f32::from_le_bytes([p[i], p[i + 1], p[i + 2], p[i + 3]]);
    if u16at(0) != UiDiffType::ModSnapshot as u16 { return None; }
    // The engine's own packing, unpacked ONCE, here. See UiModLinkCommandPayload's doc:
    // bits 0-3 source kind, 4-7 target kind, 8-9 rate, bit 10 enabled.
    let flags = u16at(2);
    Some((u32at(4), u32at(8), ModLink {
        link_id: u32at(12),
        source_device: u32at(16),
        source_id: u32at(20),
        target_device: u32at(24),
        target_id: u32at(28),
        depth: f32at(32),
        bias: f32at(36),
        source_kind: flags & 0x0f,
        target_kind: (flags >> 4) & 0x0f,
        rate: (flags >> 8) & 0x03,
        enabled: (flags & (1 << 10)) != 0,
        uid16: String::new(),
    }))
}

/// Read a ModLinkUid16 diff: the plugin parameter id a link targets.
///
/// `UiModLinkUid16DiffPayload` is diff_type u16@0, _pad u16@2, track_id u32@4,
/// mod_version u32@8, link_id u32@12, uid16 [u8;16]@16.
///
/// Hex, not the raw bytes: this is an OPAQUE plugin identifier, not text — some hosts put
/// a printable name in it and some put a hash, so decoding it as UTF-8 would work on one
/// plugin and produce replacement characters on the next. Hex is what the param mirror
/// already keys rows on, so the two match without either side guessing.
fn decode_mod_uid16(e: &EventEntry) -> Option<(u32, u32, u32, String)> {
    if (e.size as usize) < 32 { return None; }
    let p = &e.payload;
    let u16at = |i: usize| u16::from_le_bytes([p[i], p[i + 1]]);
    let u32at = |i: usize| u32::from_le_bytes([p[i], p[i + 1], p[i + 2], p[i + 3]]);
    if u16at(0) != UiDiffType::ModLinkUid16 as u16 { return None; }
    /*
     * SIXTEEN ZERO BYTES MEANS UNSET, and it is forwarded as the EMPTY STRING.
     *
     * The engine's ModTargetRef initialises uid16 to zeros, so a link that has never been
     * named carries them — and hexed naively that is 32 characters of "0", which is a
     * perfectly truthy string. Every consumer that asks "does this link name a parameter?"
     * would answer yes, and the answer decides whether the link WORKS: the block-rate
     * applier addresses a VST parameter by uid16 alone, so an unnamed link moves nothing.
     *
     * Collapsed here, at the one place that knows the sentinel, rather than in every reader
     * that would otherwise have to know that zeros mean absent.
     */
    if p[16..32].iter().all(|b| *b == 0) {
        return Some((u32at(4), u32at(8), u32at(12), String::new()));
    }
    let mut hex = String::with_capacity(32);
    for b in &p[16..32] { hex.push_str(&format!("{b:02x}")); }
    Some((u32at(4), u32at(8), u32at(12), hex))
}

/// UiDiffType by number, for the log. Names, because "type 5 = 12" is not a
/// report anyone can act on.
/// One RoutingSnapshot diff, if this entry is one.
///
/// Offsets from UiTrackRoutingPayload in apps/event_payloads.h. Read by name and
/// asserted against the struct's own size, because a payload read two bytes off
/// still yields a plausible routing — a track that claims to feed track 0 when
/// it feeds Main is not an error anyone will see, it is a mix that is quietly
/// wrong.
fn decode_routing_snapshot(e: &EventEntry) -> Option<(u32, TrackRouting)> {
    if e.size < 40 { return None; }
    let p = &e.payload;
    let u16at = |o: usize| u16::from_le_bytes([p[o], p[o + 1]]);
    let u32at = |o: usize| u32::from_le_bytes([p[o], p[o + 1], p[o + 2], p[o + 3]]);
    if u16at(0) != daw_bridge::layout::UiDiffType::RoutingSnapshot as u16 { return None; }
    Some((u32at(4), TrackRouting {
        // flags bit0 is preFaderSend, per the payload's own comment.
        pre_fader_send: (u16at(2) & 1) != 0,
        midi_out_kind: p[13],
        audio_in_kind: p[14],
        audio_out_kind: p[15],
        midi_out_track: u32at(20),
        audio_in_track: u32at(24),
        audio_out_track: u32at(28),
    }))
}

fn diff_type_name(t: u16) -> &'static str {
    match t {
        1 => "add-note", 2 => "remove-note", 3 => "update-note", 4 => "resync",
        5 => "chain-snapshot", 6 => "chain-error", 7 => "routing-snapshot",
        8 => "routing-error", 9 => "mod-snapshot", 10 => "mod-error",
        11 => "mod-link-uid", 12 => "patcher-delta", 13 => "patcher-error",
        // v20 (Movement 4): one per audio bus, after that device's chain snapshot.
        // Named before it has a consumer on purpose — the histogram is how anyone
        // finds out a capability is publishing, and "unknown=16" is exactly the
        // reading that makes a live feature look like noise.
        14 => "device-bus",
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
        // NO early return on an empty tick. The histogram below used to sit under
        // `if n == 0 { continue; }`, so it could only ever print on a tick that
        // happened to drain something — and diffs arrive in a BURST at load and
        // then stop. The one shape the ring actually has was the one shape the
        // diagnostic could not report, which is how "device-bus=16" looked exactly
        // like a silent ring for a whole afternoon.
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
            // Buses are STATE too, and they belong to the device the snapshot just
            // described — same reasoning as the chain itself. A bus whose device we
            // never saw is counted as not-forwarded rather than dropped in silence:
            // the ring is ordered and the snapshot comes first, so that can only
            // mean the snapshot was lost, which is worth seeing in the histogram.
            if let Some(bus) = decode_device_bus(e) {
                if chains.apply_bus(&bus) { continue; }
                dropped += 1;
                continue;
            }
            // Routing is STATE about a track, on the same ring and by the same
            // reasoning as the chain: it belongs in the store, not in the event
            // queue, where it would replay "track 2 goes to master" as a
            // notification every time anything republished.
            if let Some((track, routing)) = decode_routing_snapshot(e) {
                chains.apply_routing(track, routing);
                continue;
            }
            // Modulation is STATE about a track's RACK, and it goes in the same store as
            // the chain and the routing for the same reason: replaying "the LFO moves
            // cutoff" as a notification on every republish would bury the errors the
            // event queue exists for.
            if let Some((track, version, link)) = decode_mod_snapshot(e) {
                chains.apply_mod(track, version, link);
                continue;
            }
            // ...and the parameter it names. Counted as not-forwarded when the link is
            // unknown: the ring is ordered and the link comes first, so that can only
            // mean the link entry was lost, which is worth seeing rather than attaching
            // silently to nothing.
            if let Some((track, version, link_id, uid16)) = decode_mod_uid16(e) {
                if chains.apply_mod_uid16(track, version, link_id, uid16) { continue; }
                dropped += 1;
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
            // Says what it actually compared. The previous wording claimed to know
            // what wire.js expects, which it does not — this is encode() against
            // this file's own constant. `wire_js_agrees_about_the_header_and_the_
            // version` is the one that reads the page's literals, and it runs long
            // before anything gets this far.
            eprintln!("sidecar: encode() wrote {} header bytes, FULL_HEADER_BYTES says \
                       {FULL_HEADER_BYTES} — they have drifted, and every field after \
                       the mismatch would be misread by the page. Fix encode() and the \
                       constant together, then wire.js.", probe.len());
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

    /// The 64-bit fields, reassembled the way the engine will read them.
    fn tick64(p: &UiCommandPayload) -> u64 {
        (p.note_nanotick_hi as u64) << 32 | p.note_nanotick_lo as u64
    }
    fn len64(p: &UiCommandPayload) -> u64 {
        (p.note_duration_hi as u64) << 32 | p.note_duration_lo as u64
    }

    /*
     * PLACEMENT OPS, against the wire the engine agreed to.
     *
     * Written BEFORE the engine answers 48-51, which is the reason they are
     * worth having: the wire is locked, so every one of these can be wrong today
     * in a way that is free to fix and expensive to find later. The field
     * assignments are the whole contract — a start written into the duration
     * pair is a clip that resizes when you drag it.
     */
    #[test]
    fn placement_move_carries_id_start_and_lane() {
        let p = build_command(
            r#"{"type":"placement","op":"move","track":0,"id":7,"at":1920000,"toTrack":3}"#).unwrap();
        assert_eq!(p.command_type, UiCommandType::MovePlacement as u16);
        assert_eq!(p.track_id, 0, "the SOURCE track");
        assert_eq!(p.value0, 7, "the placement id keys the op");
        assert_eq!(tick64(&p), 1_920_000);
        assert_eq!(p.note_pitch, 3, "the destination lane");
    }

    /// Absent `toTrack` must be the sentinel, not the source track. Repeating the
    /// source would be indistinguishable from a deliberate move onto the lane it
    /// is already on — which the engine may treat as a reorder.
    #[test]
    fn placement_move_without_a_lane_says_so_explicitly() {
        let p = build_command(
            r#"{"type":"placement","op":"move","track":2,"id":1,"at":0}"#).unwrap();
        assert_eq!(p.note_pitch, daw_bridge::layout::PLACEMENT_SAME_TRACK);
        assert_ne!(p.note_pitch, p.track_id);
    }

    /// A right-edge drag is a length with NO start, and that has to reach the
    /// engine as one command. Sending Move+Resize instead makes the clip jump
    /// through an intermediate position on screen.
    #[test]
    fn placement_resize_leaves_the_missing_edge_alone() {
        let un = daw_bridge::layout::PLACEMENT_UNCHANGED;

        let right = build_command(
            r#"{"type":"placement","op":"resize","track":0,"id":4,"len":3840000}"#).unwrap();
        assert_eq!(right.command_type, UiCommandType::ResizePlacement as u16);
        assert_eq!(len64(&right), 3_840_000);
        assert_eq!(tick64(&right), un, "no start given: leave it");

        // A left-edge trim moves both, together.
        let left = build_command(
            r#"{"type":"placement","op":"resize","track":0,"id":4,"at":960000,"len":2880000}"#)
            .unwrap();
        assert_eq!(tick64(&left), 960_000);
        assert_eq!(len64(&left), 2_880_000);
    }

    /// THE ONE THAT MATTERS MOST. A clip dragged off the left edge is the
    /// ordinary way to produce a negative tick, and `-1 as u64` is all-ones —
    /// which is the UNCHANGED sentinel. Unchecked, dragging past zero would
    /// silently mean "leave the start where it was".
    #[test]
    fn placement_refuses_a_negative_tick_rather_than_wrapping_onto_the_sentinel() {
        assert_eq!(build_command(
            r#"{"type":"placement","op":"move","track":0,"id":1,"at":-1}"#).err(),
            Some("negative tick"));
        assert_eq!(build_command(
            r#"{"type":"placement","op":"resize","track":0,"id":1,"at":-960000}"#).err(),
            Some("negative tick"));
    }

    /// Malformed commands are refused here rather than travelling the ring to do
    /// nothing. "Nothing happened" is the hardest failure to diagnose from a UI.
    #[test]
    fn placement_refuses_what_it_cannot_mean() {
        let err = |b: &str| build_command(b).err();
        assert_eq!(err(r#"{"type":"placement","track":0}"#),
                   Some("placement needs an op: move, resize, remove or add"));
        assert_eq!(err(r#"{"type":"placement","op":"wiggle","track":0,"id":1}"#),
                   Some("unknown placement op"));
        // No id: every op but `add` addresses an existing placement.
        assert_eq!(err(r#"{"type":"placement","op":"remove","track":0}"#),
                   Some("placement needs an `id`"));
        assert_eq!(err(r#"{"type":"placement","op":"move","track":0,"at":0}"#),
                   Some("placement needs an `id`"));
        // A resize that changes neither edge is always a caller bug.
        assert_eq!(err(r#"{"type":"placement","op":"resize","track":0,"id":1}"#),
                   Some("resize needs an `at`, a `len`, or both"));
        // A zero-length clip is not a clip.
        assert_eq!(err(r#"{"type":"placement","op":"resize","track":0,"id":1,"len":0}"#),
                   Some("a clip cannot have zero length"));
        assert_eq!(err(r#"{"type":"placement","op":"add","track":0,"clip":1,"at":0,"len":0}"#),
                   Some("add needs a non-zero `len`"));
        // `add` names a CLIP, not a placement — it is the op that creates one.
        assert_eq!(err(r#"{"type":"placement","op":"add","track":0,"at":0,"len":10}"#),
                   Some("add needs a `clip`"));
        assert_eq!(err(r#"{"type":"placement","op":"add","track":0,"clip":1,"len":10}"#),
                   Some("add needs an `at`"));
    }

    #[test]
    fn placement_remove_and_add_map_their_fields() {
        let r = build_command(
            r#"{"type":"placement","op":"remove","track":5,"id":9}"#).unwrap();
        assert_eq!(r.command_type, UiCommandType::RemovePlacement as u16);
        assert_eq!((r.track_id, r.value0), (5, 9));

        let a = build_command(
            r#"{"type":"placement","op":"add","track":1,"clip":2,"at":3840000,"len":7680000}"#)
            .unwrap();
        assert_eq!(a.command_type, UiCommandType::AddPlacement as u16);
        assert_eq!((a.track_id, a.value0), (1, 2));
        assert_eq!((tick64(&a), len64(&a)), (3_840_000, 7_680_000));
    }

    /// The four opcodes are the numbers the engine announced. A renumber on
    /// either side is silent — the payload is the right size, so a wrong type is
    /// ignored rather than rejected — so it is pinned by value, not by name.
    #[test]
    fn placement_opcodes_are_the_announced_numbers() {
        assert_eq!(UiCommandType::MovePlacement as u16, 48);
        assert_eq!(UiCommandType::RemovePlacement as u16, 49);
        assert_eq!(UiCommandType::ResizePlacement as u16, 50);
        assert_eq!(UiCommandType::AddPlacement as u16, 51);
    }

    #[test]
    fn build_command_maps_the_verbs() {
        let ty = |body: &str| build_command(body).ok().map(|p| p.command_type);
        assert_eq!(ty(r#"{"type":"play"}"#), Some(UiCommandType::TogglePlay as u16));
        assert_eq!(ty(r#"{"type":"stop"}"#), Some(UiCommandType::Stop as u16));
        assert_eq!(ty(r#"{"type":"seek","tick":960000}"#), Some(UiCommandType::SetPosition as u16));
        assert_eq!(ty(r#"{"type":"delete","track":2,"tick":0}"#), Some(UiCommandType::DeleteNote as u16));
        assert_eq!(ty(r#"{"type":"undo"}"#), Some(UiCommandType::Undo as u16));
        assert_eq!(ty(r#"{"type":"harmony","root":2,"scale":3,"tick":0}"#),
                   Some(UiCommandType::WriteHarmony as u16));
        assert_eq!(ty(r#"{"type":"addtrack"}"#), Some(UiCommandType::AddTrack as u16));
        assert_eq!(ty(r#"{"type":"removetrack","track":2}"#),
                   Some(UiCommandType::RemoveTrack as u16));
        assert_eq!(ty(r#"{"type":"nonsense"}"#), None);
    }

    /// The engine reads the note column off the low byte of `flags`
    /// (`applyAddNote`: `const uint8_t column = flags & 0xff`). This builder used
    /// to drop it, so every note the UI wrote landed in column 0 and a second
    /// note on one row replaced the first instead of sitting beside it.
    #[test]
    fn a_note_carries_its_column() {
        let p = build_command(r#"{"type":"note","track":0,"pitch":67,"tick":0,"column":2}"#)
            .expect("builds");
        assert_eq!(p.command_type, UiCommandType::WriteNote as u16);
        assert_eq!(p.flags & 0xff, 2);
        // ...and so does the delete, or removing in one column takes another's note.
        let d = build_command(r#"{"type":"delete","track":0,"tick":0,"column":2}"#)
            .expect("builds");
        assert_eq!(d.command_type, UiCommandType::DeleteNote as u16);
        assert_eq!(d.flags & 0xff, 2);
        // Absent means column 0 — what a single-column tracker sends.
        let z = build_command(r#"{"type":"note","track":0,"pitch":60,"tick":0}"#).expect("builds");
        assert_eq!(z.flags & 0xff, 0);
    }

    #[test]
    fn remove_track_carries_the_track_id() {
        // The whole point of the v22 contract is that this id is STABLE — the
        // engine tombstones that slot and leaves later ids alone. Sending the
        // wrong number here removes somebody else's track, and unlike a wrong
        // note it cannot be undone in v1.
        let p = build_command(r#"{"type":"removetrack","track":5}"#).expect("builds");
        assert_eq!(p.command_type, UiCommandType::RemoveTrack as u16);
        assert_eq!(p.track_id, 5);
    }

    /// `is_type`, not `contains` — a project or track named "addtrack" must not
    /// be able to add a track. Same class of bug as the save/load and deldevice
    /// dispatches, which were silent in both directions.
    #[test]
    fn a_project_named_addtrack_does_not_add_a_track() {
        let ty = |body: &str| build_command(body).ok().map(|p| p.command_type);
        assert_eq!(ty(r#"{"type":"load","name":"addtrack"}"#),
                   Some(UiCommandType::LoadProject as u16));
        assert_eq!(ty(r#"{"type":"rename","track":0,"name":"removetrack"}"#),
                   Some(UiCommandType::SetTrackName as u16));
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
        // `ready` is M2.18's publication flag for the multi-producer ring. This
        // builds an entry the DECODERS read rather than one the ring publishes,
        // so its value is immaterial here — but it is set to 1 anyway, because a
        // fixture that does not look like a real entry is a fixture that stops
        // catching the day someone checks the flag.
        let mut e = EventEntry { sample_time: 0, block_id: 0, event_type: 0,
                                 size: payload.len() as u16, flags: 0,
                                 payload: [0u8; 40], ready: 1 };
        e.payload[..payload.len()].copy_from_slice(payload);
        e
    }

    /// A ChainSnapshot with a bus count on the ENTRY's flags, not its payload.
    fn chain_entry_with_buses(track: u32, version: u32, device_id: u32,
                              bus_count: u8, truncated: bool) -> EventEntry {
        // Written into the PAYLOAD at offset 2, where UiChainDiffPayload.flags is.
        // The first version of this helper set EventEntry.flags instead, matching a
        // decoder that read the same wrong field — so the test passed against a
        // build that could never work against the engine. A helper that encodes the
        // implementation's assumption rather than the contract's proves nothing.
        let mut e = chain_entry(track, version, device_id, 0);
        let f: u16 = bus_count as u16 | if truncated { 0x0100 } else { 0 };
        e.payload[2..4].copy_from_slice(&f.to_le_bytes());
        e
    }

    fn bus_entry(track: u32, device: u32, index: u8, is_input: bool,
                 name: &str) -> EventEntry {
        let mut p = Vec::new();
        p.extend_from_slice(&(UiDiffType::DeviceBus as u16).to_le_bytes());   // 0
        let flags: u16 = if is_input { 1 } else { 0 } | 2 | 4;                // main+enabled
        p.extend_from_slice(&flags.to_le_bytes());                            // 2
        p.extend_from_slice(&track.to_le_bytes());                            // 4
        p.extend_from_slice(&device.to_le_bytes());                           // 8
        p.push(index);                                                        // 12
        p.push(2);                                                            // 13 channels
        p.extend_from_slice(&2u16.to_le_bytes());                             // 14 layoutId stereo
        p.extend_from_slice(&(index as u16 * 2).to_le_bytes());               // 16 offset
        let mut nm = [0u8; 22];
        for (i, c) in name.bytes().take(22).enumerate() { nm[i] = c; }
        p.extend_from_slice(&nm);                                             // 18..40
        entry(&p)
    }

    #[test]
    fn a_bus_decodes_every_field_from_its_own_offset() {
        let e = bus_entry(3, 77, 1, false, "Individual Out 3/4");
        let b = decode_device_bus(&e).expect("decodes");
        assert_eq!(b.track, 3);
        assert_eq!(b.device, 77);
        assert_eq!(b.bus.index, 1);
        assert_eq!(b.bus.is_input, false);
        assert_eq!(b.bus.is_main, true);
        assert_eq!(b.bus.enabled, true);
        assert_eq!(b.bus.channel_count, 2);
        assert_eq!(b.bus.layout_id, 2);
        assert_eq!(b.bus.channel_offset, 2);
        assert_eq!(b.bus.name, "Individual Out 3/4");
        // A chain snapshot is not a bus and vice versa; each decoder must refuse
        // the other or a mis-typed entry decodes as a plausible record of the
        // wrong kind.
        assert!(decode_chain_snapshot(&e).is_none());
        assert!(decode_device_bus(&chain_entry(3, 1, 77, 0)).is_none());
    }

    #[test]
    fn a_full_width_bus_name_has_no_terminator() {
        // 22 characters exactly: nul-PADDED means there is no nul to find, so a
        // decoder that scanned for one would walk off the end of the payload. The
        // scan is bounded by the field width instead.
        let name = "ABCDEFGHIJKLMNOPQRSTUV";
        assert_eq!(name.len(), 22);
        let b = decode_device_bus(&bus_entry(0, 1, 0, false, name)).expect("decodes");
        assert_eq!(b.bus.name, name);
        // And a longer one is truncated at the field, not read past it.
        let b2 = decode_device_bus(&bus_entry(0, 1, 0, false, "ABCDEFGHIJKLMNOPQRSTUVWXYZ"))
            .expect("decodes");
        assert_eq!(b2.bus.name, name);
    }

    #[test]
    fn bus_count_rides_the_entry_flags_so_a_reader_can_draw_once() {
        let store = ChainStore::default();
        apply_chain(&store, &chain_entry_with_buses(1, 1, 50, 4, false));
        let d = store.devices_of(1);
        assert_eq!(d[0].bus_count, 4, "four are coming");
        assert_eq!(d[0].buses.len(), 0, "none have arrived yet");
        assert!(!d[0].bus_truncated);
        // Without this the rack cannot tell three-of-four from a device with three,
        // so it draws three and rearranges — the failure the whole design avoids.
        apply_chain(&store, &chain_entry_with_buses(1, 2, 50, 33, true));
        let d2 = store.devices_of(1);
        assert_eq!(d2[0].bus_count, 33);
        assert!(d2[0].bus_truncated, "more buses than the cap, said out loud");
    }

    #[test]
    fn a_chain_snapshot_replaces_a_devices_buses_rather_than_merging() {
        // THE rule, and it is in shared_memory.h because device ids are REUSED.
        // Merge instead and a new plugin inherits the previous occupant's buses:
        // a rack that is entirely plausible and completely wrong.
        let store = ChainStore::default();
        apply_chain(&store, &chain_entry_with_buses(1, 1, 50, 2, false));
        for i in 0..2u8 {
            let e = bus_entry(1, 50, i, false, "Out");
            let b = decode_device_bus(&e).unwrap();
            assert!(store.apply_bus(&b), "the device exists, so the bus lands");
        }
        assert_eq!(store.devices_of(1)[0].buses.len(), 2);

        // A new snapshot for the same track: the old device, and its buses, go.
        apply_chain(&store, &chain_entry_with_buses(1, 2, 50, 1, false));
        assert_eq!(store.devices_of(1)[0].buses.len(), 0,
                   "the previous version's buses did not survive the replacement");
    }

    #[test]
    fn a_bus_for_an_unknown_device_is_refused_rather_than_buffered() {
        // The ring is ordered and the snapshot comes first, so this can only mean
        // the snapshot was lost. Holding the bus for a device that may never arrive
        // is how a store grows without bound while looking healthy.
        let store = ChainStore::default();
        let b = decode_device_bus(&bus_entry(9, 999, 0, false, "Out")).unwrap();
        assert!(!store.apply_bus(&b), "refused, and the caller counts it");
    }

    #[test]
    fn re_emitting_a_bus_updates_it_instead_of_doubling_the_list() {
        // The engine re-emits a device's buses on every republish, and a ring that
        // redelivers must not grow the list. Keyed on (direction, index), which is
        // the identity backend gave these.
        let store = ChainStore::default();
        apply_chain(&store, &chain_entry_with_buses(1, 1, 50, 1, false));
        for _ in 0..3 {
            let b = decode_device_bus(&bus_entry(1, 50, 0, false, "Main")).unwrap();
            assert!(store.apply_bus(&b));
        }
        assert_eq!(store.devices_of(1)[0].buses.len(), 1);
        // ...but an input bus 0 and an output bus 0 are DIFFERENT buses.
        let bi = decode_device_bus(&bus_entry(1, 50, 0, true, "Sidechain")).unwrap();
        assert!(store.apply_bus(&bi));
        assert_eq!(store.devices_of(1)[0].buses.len(), 2);
    }

    #[test]
    fn a_bus_name_cannot_break_the_json() {
        // The only free text on this wire, and it comes from a plugin. A bus called
        // `Out "A"` would otherwise emit JSON the page cannot parse, and the failure
        // is the whole rack going blank rather than one label looking odd.
        let store = ChainStore::default();
        apply_chain(&store, &chain_entry_with_buses(1, 1, 50, 1, false));
        let b = decode_device_bus(&bus_entry(1, 50, 0, false, "Out \"A\" \\ x")).unwrap();
        assert!(store.apply_bus(&b));
        let (json, _) = store.changed_since(0).expect("a body");
        assert!(!json.contains("\"A\""), "the quotes did not survive into the JSON");
        assert!(json.contains("\"buses\":["), "and the array is still there");
        // Balanced quotes is the property that actually matters.
        assert_eq!(json.matches('"').count() % 2, 0, "quotes are balanced");
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
            // No buses on this fixture's entries: `bus_count` rides the EventEntry's
            // flags, which `chain_entry` leaves zero. A device with no buses and a
            // device whose buses have not arrived look the same here, which is
            // exactly what `bus_count` exists to separate — see the bus tests below.
            bus_count: 0, bus_truncated: false, buses: Vec::new(),
            // Same story as bus_count: the generates bit rides the payload's
            // `flags`, which this fixture leaves zero.
            generates: false,
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
        // The two facts, checked separately rather than as one substring: the
        // track's version moved and its device list is empty. Matching the exact
        // byte sequence made this fail when a `routing` field was added between
        // them, which is a change to a NEIGHBOURING field and nothing this test
        // is about.
        assert!(json.contains("\"track\":2,\"version\":6,"), "{json}");
        /*
         * `"devices":[]` WITHOUT the closing brace.
         *
         * The comment above says matching an exact byte sequence broke this once when a
         * neighbouring field was added, and then the fix pinned `devices` as the LAST field
         * — which broke it again the moment `modVersion` and `modLinks` were appended after
         * it. A test that asserts a field's POSITION is asserting the serialiser's field
         * order, which is not what it is about.
         */
        assert!(json.contains("\"devices\":[]"), "{json}");
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

    #[test]
    fn quantize_biases_the_swing_exactly_once() {
        let p = build_quantize(
            r#"{"type":"quantize","track":2,"grid":240000,"strength":600,"swing":-100}"#)
            .expect("recognised").expect("built");
        assert_eq!(p.command_type, UiCommandType::SetLaneQuantize as u16);
        assert_eq!(p.command_type, 53, "the engine's own number for it");
        assert_eq!(p.track_id, 2);
        assert_eq!(p.value0, 600, "strength in thousandths");
        let grid = ((p.note_nanotick_hi as u64) << 32) | p.note_nanotick_lo as u64;
        assert_eq!(grid, 240_000, "a 16th at 960000 per quarter");
        // THE WHOLE POINT: the payload field is unsigned, so the command adds the
        // bias and the READ-BACK does not. -100 travels as 400. Applying it on both
        // legs, or neither, is an off-by-500 that shows as a groove nobody asked
        // for — and it would look like a working feature.
        assert_eq!(p.note_pitch, 400, "swing -100 travels as 400, biased by +500");
        assert_eq!(build_quantize(r#"{"type":"quantize","grid":0}"#).unwrap().unwrap()
                   .note_pitch, 500, "straight is 500, not 0");
        // Unversioned on purpose: quantize moves no authored note, so gating it on
        // a clip version would let an unrelated edit refuse a setting that cannot
        // conflict with anything.
        assert_eq!(p.base_version, 0);

        // Grid 0 is "off", and a real value, so it must not be read as absent.
        let off = build_quantize(r#"{"type":"quantize","track":1,"grid":0}"#)
            .unwrap().unwrap();
        assert_eq!(((off.note_nanotick_hi as u64) << 32) | off.note_nanotick_lo as u64, 0);
        assert_eq!(off.value0, 1000, "strength defaults to full");

        // Past +/-500 the odd slots cross the even ones and the pattern reorders.
        // Refused rather than clamped: clamping accepts a number meaning something
        // else, which is the same silence this whole file argues against.
        assert!(build_quantize(r#"{"type":"quantize","grid":240000,"swing":900}"#)
                .unwrap().is_err());
        assert!(build_quantize(r#"{"type":"quantize","grid":240000,"swing":-900}"#)
                .unwrap().is_err());
        assert!(build_quantize(r#"{"type":"quantize","track":0}"#).unwrap().is_err(),
                "no grid is a refusal, not a silent off");
        assert!(build_quantize(r#"{"type":"quantize","grid":-1}"#).unwrap().is_err());
        // Strength out of range clamps rather than refusing: unlike swing, every
        // value past the end still MEANS the end.
        assert_eq!(build_quantize(r#"{"type":"quantize","grid":1,"strength":9000}"#)
                   .unwrap().unwrap().value0, 1000);
        // And a project named "quantize" is not a quantize command.
        assert!(build_quantize(r#"{"type":"load","name":"quantize"}"#).is_none());
    }

    #[test]
    fn a_chord_can_be_deleted_by_id_or_by_position() {
        let by_id = build_chord(r#"{"type":"delchord","track":2,"tick":960000,"id":41}"#)
            .expect("recognised");
        assert_eq!(by_id.command_type, UiCommandType::DeleteChord as u16);
        assert_eq!(by_id.command_type, 9, "the engine's own number for it");
        assert_eq!(by_id.track_id, 2);
        assert_eq!(u64::from(by_id.nanotick_lo), 960_000);
        /*
         * THE ID RIDES IN `spread_nanoticks`, which on a WRITE means the chord's spread.
         * The engine reuses the field, so the same bytes mean two different things
         * depending on the command — which is exactly the sort of overload that gets
         * read wrong once and then believed. Pinned in both directions below.
         */
        assert_eq!(by_id.spread_nanoticks, 41, "the id, not a spread");

        // No id: the engine removes whatever is at that tick and column.
        let by_pos = build_chord(r#"{"type":"delchord","track":1,"tick":480000,"column":1}"#)
            .expect("recognised");
        assert_eq!(by_pos.spread_nanoticks, 0, "0 means 'whatever is here'");
        assert_eq!(by_pos.flags & 0xff, 1, "and the column still selects which");

        // A `spread` left in a delete message must NOT become an id — the field is named
        // for what it means on THIS command.
        assert_eq!(build_chord(r#"{"type":"delchord","track":0,"tick":0,"spread":9999}"#)
                   .expect("recognised").spread_nanoticks, 0);
        // ...and a WRITE still reads spread as a spread.
        let w = build_chord(r#"{"type":"chord","track":0,"degree":3,"spread":1234}"#)
            .expect("recognised");
        assert_eq!(w.command_type, UiCommandType::WriteChord as u16);
        assert_eq!(w.spread_nanoticks, 1234);
        // And a project named `delchord` is neither.
        assert!(build_chord(r#"{"type":"load","name":"delchord"}"#).is_none());
    }

    #[test]
    fn movedevice_carries_the_final_index() {
        let p = build_chain_edit(r#"{"type":"movedevice","track":2,"device":7,"pos":0}"#)
            .expect("recognised").expect("built");
        assert_eq!(p.command_type, UiCommandType::MoveDevice as u16);
        assert_eq!(p.command_type, 16, "the engine's own number for it");
        assert_eq!(p.track_id, 2);
        assert_eq!(p.device_id, 7);
        assert_eq!(p.insert_index, 0);
        // Untouched, because MoveDevice reads neither — and the builder's defaults for
        // an ADD are the auto sentinel, which would repoint a patcher node if the
        // engine ever grew a reason to look.
        assert_eq!(p.patcher_node_id, 0);
        assert_eq!(p.host_slot_index, 0);

        // Position 0 is a real position, so it must not be read as absent.
        assert_eq!(build_chain_edit(r#"{"type":"movedevice","device":0,"pos":0}"#)
                   .unwrap().unwrap().device_id, 0, "device 0 is a device");
        assert!(build_chain_edit(r#"{"type":"movedevice","device":3}"#).unwrap().is_err(),
                "no position is a refusal, not position 0");
        assert!(build_chain_edit(r#"{"type":"movedevice","pos":1}"#).unwrap().is_err(),
                "and no device id likewise");
        assert!(build_chain_edit(r#"{"type":"movedevice","device":3,"pos":-1}"#)
                .unwrap().is_err());
        // A project named `movedevice` is not a move.
        assert!(build_chain_edit(r#"{"type":"load","name":"movedevice"}"#).is_none());
    }

    #[test]
    fn bypass_names_the_state_it_wants_and_touches_only_that() {
        let p = build_chain_edit(r#"{"type":"bypass","track":2,"device":7,"on":1}"#)
            .expect("recognised").expect("built");
        assert_eq!(p.command_type, UiCommandType::UpdateDevice as u16);
        assert_eq!(p.command_type, 17, "the engine's own number for it");
        assert_eq!(p.flags, 0x1, "bit0 alone — bit1 would repoint the patcher node");
        assert_eq!(p.track_id, 2);
        assert_eq!(p.device_id, 7);
        assert_eq!(p.bypass, 1);
        // The fields this builder defaults to the auto sentinel for adds. They are
        // unread under flags 0x1, and zeroed rather than trusted to stay unread.
        assert_eq!(p.patcher_node_id, 0);
        assert_eq!(p.host_slot_index, 0);
        assert_eq!(p.insert_index, 0);

        // A state, not a toggle: "off" has to be expressible, or two clicks racing
        // on the ring cancel each other out.
        assert_eq!(build_chain_edit(r#"{"type":"bypass","device":7,"on":0}"#)
                   .unwrap().unwrap().bypass, 0);
        // Device 0 is a device. The same trap deldevice has: a parser looking for
        // "device" inside the word "bypass" would find nothing and default to 0,
        // which is a real id.
        assert_eq!(build_chain_edit(r#"{"type":"bypass","device":0,"on":1}"#)
                   .unwrap().unwrap().device_id, 0);
        assert!(build_chain_edit(r#"{"type":"bypass","track":1}"#).unwrap().is_err(),
                "no device id is a refusal, not device 0");
        assert!(build_chain_edit(r#"{"type":"bypass","device":-1}"#).unwrap().is_err());
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
                     "note", "chord", "loop", "seek", "adddevice",
                     // Added with the builders below them: a verb absent from this
                     // list is a builder nothing protects.
                     "deldevice", "openeditor", "bypass", "movedevice", "quantize", "routing",
                     "delchord",
                     "patchadd", "patchremove", "patchlink", "patchcfg",
                     "placement", "preview", "panic", "stop", "undo", "redo"] {
            let body = format!("{{\"type\":\"load\",\"name\":\"{verb}\"}}");
            assert!(build_waveform_request(&body).is_none(),
                    "the waveform handler claimed a load of a project called {verb}");
            assert!(build_set_param(&body).is_none(),
                    "the setparam handler claimed a load of a project called {verb}");
            /*
             * EVERY BUILDER IN THE DISPATCH CHAIN, not two of them.
             *
             * This loop listed "chord" among its verbs and never called build_chord,
             * so the one builder still using a raw `contains` was named by the test
             * that exists to find it and checked by no assertion in it. A ratchet
             * that records a case without testing it is a ratchet that reports
             * safety it does not provide.
             */
            assert!(build_chord(&body).is_none(),
                    "the chord handler claimed a load of a project called {verb}");
            assert!(build_chain_edit(&body).is_none(),
                    "a chain builder claimed a load of a project called {verb}");
            assert!(build_patcher_graph(&body).is_none(),
                    "the graph handler claimed a load of a project called {verb}");
            assert!(build_patcher_config(&body).is_none(),
                    "the node-config handler claimed a load of a project called {verb}");
            assert!(build_routing(&body).is_none(),
                    "the routing handler claimed a load of a project called {verb}");
            assert!(build_quantize(&body).is_none(),
                    "the quantize handler claimed a load of a project called {verb}");
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

    /// A KEY THAT IS ALSO A VALUE MUST NOT MATCH ITSELF.
    ///
    /// `{"op":"depth","depth":0.25}` contains `"depth"` twice, and a scan that took the first
    /// hit read the number after `"op":"depth"` — the NEXT field's value. Every modulation
    /// depth arrived as 0, the link multiplied by nothing, and the capture was silence: a
    /// parser bug that presented as an audio bug, and was found by an RMS measurement rather
    /// than by anything that looked at a message.
    ///
    /// The real message is used here rather than a minimal one, because the ORDER is what
    /// makes the bug: `op` comes before `depth` in what the page sends.
    #[test]
    fn a_key_that_is_also_a_value_does_not_match_itself() {
        let body = r#"{"type":"mod","op":"depth","track":0,"link":1,"depth":0.25,"bias":0}"#;
        assert_eq!(parse_f32(body, "\"depth\""), Some(0.25), "read the FIELD, not the op's value");
        assert_eq!(parse_str(body, "\"op\""), Some("depth"));
        assert_eq!(parse_num(body, "\"link\""), Some(1));
        // ...and a key that is genuinely absent is still absent, rather than matching a value
        // somewhere else that happens to spell it.
        assert_eq!(parse_f32(r#"{"op":"depth"}"#, "\"depth\""), None);
        // Whitespace after the colon is legal JSON, and a hand-written message has it.
        assert_eq!(parse_f32(r#"{"depth" : 0.5}"#, "\"depth\""), Some(0.5));
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

    /// The encoder writes exactly the header it claims to.
    ///
    /// `encode` asserts this itself, but only through `debug_assert`, which is
    /// compiled out of the release build — the one where a two-byte drift silently
    /// reinterprets every field after it. `main` re-checks at startup and refuses
    /// to run, which is the right behaviour but arrives after someone has already
    /// pushed. This is the same check at the time it is cheap to act on.
    /// Every header field is at the offset the page reads it from.
    ///
    /// The length assertions and the wire.js drift test both compare TOTALS, and a
    /// total is blind to the two mistakes this layout actually makes. When the
    /// per-track lines-per-beat block widened from 8 to 16 every offset after it
    /// moved, and a careless renumber left the trailing comments scrambled —
    /// loop_start said 132, load_ok said 128 — while the writes stayed correct.
    /// Nothing caught it, because 136 is still 136. And SWAPPING two equal-width
    /// fields survives every length check there is: the page then reads load_seq
    /// out of load_ok and neither side errors.
    ///
    /// So this pins each field to its offset BY VALUE. Every field gets a distinct
    /// sentinel and is read back from the exact byte range wire.js reads it from —
    /// the offsets here are copied from the DECODER, not from the encoder, so the
    /// two are being compared rather than agreeing with themselves.
    #[test]
    fn every_header_field_is_where_the_page_reads_it() {
        let mut f = Frame::default();
        f.seq = 0x1122_3344_5566_7788;
        f.playhead_nanotick = 0x0102_0304_0506_0708;
        f.visual_sample = 0x1111_2222_3333_4444;
        f.clip_version = 0xAABB_CCDD;
        f.harmony_version = 0x1234_5678;
        f.transport = 0x0BAD;
        f.track_count = 0x0C0D;
        f.notes_grid = 0x0E0F;
        f.agg_rows = 0x2233_4455;
        f.mixer_version = 0x3344_5566;
        f.patcher_version = 0x4455_6677;
        f.patcher_device = 0x5566_7788;
        f.loop_start = 0x0A0B_0C0D_0E0F_1011;
        f.loop_end = 0x1112_1314_1516_1718;
        f.load_seq = 0x6677_8899;
        f.load_ok = 0x7788_99AA;
        f.tempo_milli_bpm = 0x8899_AABB;
        f.tempo_point_count = 0x99AA_BBCC;
        f.song_time_sig_num = 7;
        f.song_time_sig_den = 8;
        f.lpb[0] = 3;
        f.lpb[15] = 12;

        let mut out = Vec::new();
        encode(&f, &mut out);

        let u16at = |i: usize| u16::from_le_bytes([out[i], out[i + 1]]);
        let u32at = |i: usize| u32::from_le_bytes([out[i], out[i + 1], out[i + 2], out[i + 3]]);
        let u64at = |i: usize| u64::from_le_bytes([
            out[i], out[i + 1], out[i + 2], out[i + 3],
            out[i + 4], out[i + 5], out[i + 6], out[i + 7]]);

        assert_eq!(u32at(0), WIRE_MAGIC, "magic @0");
        assert_eq!(u16at(4), WIRE_VERSION, "version @4");
        assert_eq!(u64at(8), f.seq, "seq @8");
        assert_eq!(u64at(16), f.playhead_nanotick, "playhead @16");
        assert_eq!(u64at(24), f.visual_sample, "visual sample @24");
        assert_eq!(u32at(32), f.clip_version, "clip version @32");
        assert_eq!(u32at(36), f.harmony_version, "harmony version @36");
        assert_eq!(u16at(40), f.transport, "transport @40");
        assert_eq!(u16at(42), f.track_count, "track count @42");
        assert_eq!(u16at(46) as u32, f.notes_grid, "notes grid @46");
        assert_eq!(u32at(52), f.agg_rows, "agg rows @52");
        // The lines-per-beat block: 16 wide since v21, and its FIRST and LAST bytes
        // are checked, because a width mistake moves only the far end.
        assert_eq!(out[60], 3, "lpb[0] @60");
        assert_eq!(out[75], 12, "lpb[15] @75 — the block is 16 wide, not 8");
        assert_eq!(u32at(76), f.mixer_version, "mixer version @76");
        assert_eq!(u32at(86), f.patcher_version, "patcher version @86");
        assert_eq!(u32at(90), f.patcher_device, "patcher device @90");
        assert_eq!(u64at(100), f.loop_start, "loop start @100");
        assert_eq!(u64at(108), f.loop_end, "loop end @108");
        // These two are the swap this test exists for: same width, adjacent, and
        // indistinguishable to every length check in the file.
        assert_eq!(u32at(116), f.load_seq, "load seq @116");
        assert_eq!(u32at(120), f.load_ok, "load ok @120");
        assert_eq!(u32at(124), f.tempo_milli_bpm, "tempo @124");
        assert_eq!(u32at(128), f.tempo_point_count, "tempo points @128");
        assert_eq!(u16at(132), f.song_time_sig_num, "song meter numerator @132");
        assert_eq!(u16at(134), f.song_time_sig_den, "song meter denominator @134");
    }

    #[test]
    fn the_header_is_the_length_it_says_it_is() {
        let mut probe = Vec::new();
        encode(&Frame::default(), &mut probe);
        assert_eq!(probe.len(), FULL_HEADER_BYTES,
                   "encode() and FULL_HEADER_BYTES disagree");
    }

    /// ...and the page agrees about both numbers.
    ///
    /// Every version of this file has carried a comment saying to change the two
    /// sides together, and the startup check's message already claims to know what
    /// "wire.js expects" while in fact comparing the Rust constant against itself.
    /// A comment is not a check. This reads the actual literals out of wire.js, so
    /// a header field added on one side only fails here rather than in a browser,
    /// where the symptom is every subsequent field decoding as garbage — that has
    /// happened twice and cost an afternoon each time.
    ///
    /// Parsed with a plain string search rather than a dependency: two constants
    /// declared one way in one file do not justify a JS parser, and the test fails
    /// loudly if the declaration it looks for is no longer there.
    #[test]
    fn wire_js_agrees_about_the_header_and_the_version() {
        let path = concat!(env!("CARGO_MANIFEST_DIR"), "/../../ui-web/src/wire.js");
        let src = std::fs::read_to_string(path)
            .unwrap_or_else(|e| panic!("cannot read {path}: {e}"));

        fn literal(src: &str, decl: &str) -> usize {
            let at = src.find(decl)
                .unwrap_or_else(|| panic!("wire.js no longer declares `{decl}` — \
                                           this test cannot see the value it guards"));
            let rest = &src[at + decl.len()..];
            let digits: String = rest.chars().take_while(|c| c.is_ascii_digit()).collect();
            digits.parse().unwrap_or_else(|_| panic!("`{decl}` is not followed by a number"))
        }

        assert_eq!(literal(&src, "const HEADER_BYTES = "), FULL_HEADER_BYTES,
                   "wire.js HEADER_BYTES and the sidecar's FULL_HEADER_BYTES have drifted");
        assert_eq!(literal(&src, "export const WIRE_VERSION = "), WIRE_VERSION as usize,
                   "wire.js WIRE_VERSION and the sidecar's have drifted — the page will \
                    reject every frame");
    }
}
