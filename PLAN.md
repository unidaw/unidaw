# DAW Project Plan

## Vision

A **fractal, harmony-aware musical programming environment** that combines the best of:
- **Renoise** (tracker efficiency)
- **Ableton Live** (modern workflow, NO session view)
- **Max/MSP** (visual programming, but with better design)

Core principle: **One musical reality, multiple lenses**. The same musical data can be viewed and edited through different interfaces (tracker, piano roll, patcher, arrangement), with operations that work consistently across all scales (samples → notes → phrases → clips → songs).

## Core Concepts

### 1. Fractals in Music
- The same operations work at every time scale
- Patterns repeat with variation at different levels
- UI zooms smoothly from samples to songs
- Every transformation (reverse, repeat, vary, etc.) works on any musical unit

### 2. Harmony as Gravity
- Harmony is a **global field** that affects all notes
- Always-on quantization (not a processing node)
- Per-track on/off (drums off, melodic instruments on)
- Override mechanisms for chromatic notes (! prefix, shift+click, etc.)
- Extensive scale support including microtonal

### 3. Clips as First-Class Objects
- Clips are single-track musical containers
- Can be manipulated by the patcher
- Can be generated, transformed, and arranged programmatically
- Enable both fully composed and fully generative music

## Architecture

### Process Separation
```
Engine Process (C++)
├── Scheduler (nanotick-based)
├── Harmony Context
├── Clip Management
├── Timeline State
└── IPC Coordinator

Host Process(es) (C++/JUCE)
├── VST3 Plugin Instance
├── Audio Processing
├── Parameter Management
└── Shared Memory I/O

UI Process (Rust/GPUI)
├── Tracker View
├── Piano Roll View (later)
├── Patcher View
├── Arrangement View (later)
└── Mixer View
```

### Data Model

```cpp
// GLOBAL TIMELINE (shared by all)
Timeline {
  HarmonyLane harmony;          // Global harmony progression
  TempoMap tempo;              // Global tempo changes
  TimeSignatureMap timesig;    // Global time signatures
  GrooveTemplate groove;       // Global groove (can override per track)
}

// TRACK (vertical channel)
Track {
  string id;
  string name;
  vector<Clip> clips;          // Clips placed on this track
  PluginChain effects;         // VST3 instruments and effects
  float volume, pan;
  bool harmony_quantize;       // Use global harmony or not
  GrooveAmount groove_amount;  // How much groove to apply
}

// CLIP (musical data for ONE track)
Clip {
  string id;
  uint64_t start_time;         // When placed on timeline
  uint64_t length;
  vector<Note> notes;
  vector<Automation> automation;
  optional<Patcher> patcher;   // Can contain generative patcher
}

// NOTE
Note {
  uint64_t nanotick;          // 960,000 per quarter note
  float frequency_hz;         // For microtonal support
  uint8_t midi_pitch;         // For VST3 compatibility
  uint8_t velocity;
  uint64_t duration;
  bool harmony_override;      // Bypass quantization
}

// HARMONY
HarmonyEvent {
  uint64_t nanotick;
  uint8_t root;               // 0-11 for 12-TET, or frequency for microtonal
  Scale scale;                // Can be standard or custom
  float quantize_strength;   // Usually 0 or 100%
}

Scale {
  string name;
  vector<float> intervals_cents;  // Supports any tuning
  ScaleType type;            // Major, Minor, Custom, etc.
}
```

## UX Design

### Primary Modes

#### 1. COMPOSE Mode (Tracker/Piano Roll)
```
Musical Time Display (bar:beat:tick):
═════════════════════════════════════════════════════
BAR:BT:TK│HARM │ TRK1      │ TRK2      │ TRK3
─────────┼─────┼───────────┼───────────┼──────────
1:1:000  │C:maj│ C-4 kick  │ E-3 bass  │ C-5 lead
1:1:240  │     │           │           │
1:2:000  │     │ D-4 snare │ F-3 bass  │ E-5 lead
1:2:240  │     │           │           │
1:3:000  │     │ C-4 kick  │ G-3 bass  │ G-5 lead
1:3:240  │     │           │           │
1:4:000  │     │ D-4 snare │ A-3 bass  │
2:1:000  │G:maj│ C-4 kick  │ D-3 bass  │ B-5 lead
═════════════════════════════════════════════════════
```

**Zoomable Time Resolution:**
- Zoom in: Shows more ticks (1:1:000, 1:1:120, 1:1:240...)
- Zoom out: Shows only beats or bars
- Microtiming shown as offset when zoomed out (d+120 = delayed 120 ticks)

**Harmony Display:**
- Hover shows full scale info
- Color coding shows scale degrees
- Click opens scale browser

**Key Bindings:**
- `Tab`/`Shift+Tab`: Navigate between tracks
- `a-z`: Enter notes (mapped to pitches)
- `!`: Force chromatic (bypass harmony)
- `Ctrl++/-`: Zoom time resolution
- `Cmd+K`: Command palette
- Numbers: Jump to bar:beat

**Tracker Extensions:**
- Multiple note columns per track with add/remove controls.
- Free-text cell tokens for notes, degree notes, and chord tokens:
  - `C-4` (note)
  - `24-4` (degree 24, octave 4)
  - `@3^7~80h20` (degree 3 seventh chord, spread 80, humanize 20)
- Harmony lane is global and editable from the tracker.

#### 2. PATCH Mode (Visual Programming)
```
┌─────────────────────────────────────────────────┐
│                  PATCHER VIEW                    │
├─────────────────────────────────────────────────┤
│                                                  │
│  [Note Gen]→[Transpose]→[Arpeggiator]→[Output]  │
│      ↓           ↓            ↓                 │
│  [Harmony]   [LFO:+3]    [Clock÷4]              │
│                                                  │
│  Fractal operations work at all scales:         │
│  - Process individual notes                     │
│  - Transform entire clips                       │
│  - Arrange song sections                        │
└─────────────────────────────────────────────────┘
```

**Node Categories:**
- **Generators**: Note, Chord, Pattern, Euclidean, Clip Player
- **Processors**: Transpose, Quantize, Humanize, Reverse, Vary
- **Routers**: Gate, Probability, Split, Merge, Crossfade
- **Control**: LFO, Envelope, Random, Sequencer
- **I/O**: Track Input/Output, Clip Reader/Writer, Harmony Reader

**Fractal Operations:**
Every operation works at multiple scales:
- `[Reverse]` → Reverse sample/note/phrase/clip/section
- `[Repeat]` → Echo/trill/sequence/loop
- `[Vary]` → Timbre/pitch/rhythm/arrangement variation

#### 3. MIX Mode (Mixer)
```
┌──────┬──────┬──────┬──────┬──────┐
│ TRK1 │ TRK2 │ TRK3 │ BUS1 │ MAIN │
├──────┼──────┼──────┼──────┼──────┤
│ ▇▇▇  │ ▇▇   │ ▇    │ ▇▇   │ ▇▇▇▇ │ (meters)
│ ▇▇   │ ▇    │      │ ▇    │ ▇▇▇  │
├──────┼──────┼──────┼──────┼──────┤
│[EQ  ]│[COMP]│[REV ]│[DLY ]│[LIM ]│ (effects)
│[DIST]│[CHR ]│     ]│     ]│[EQ  ]│
├──────┼──────┼──────┼──────┼──────┤
│ S M  │ S M  │ S M  │      │      │ (solo/mute)
│  ●   │  ●   │  ●   │  ●   │  ●   │ (pan)
│ ███  │ ██   │ ████ │ ██   │ ████ │ (fader)
└──────┴──────┴──────┴──────┴──────┘
```

#### 4. ARRANGE Mode (Timeline) - Later Phase
```
Timeline (bar:beat) →
═══════════════════════════════════════════════════
HARMONY  [C:maj═══════][G:maj═══][Am:min══════════]

Track 1  [Clip:intro  ][Clip:verse   ][Clip:chorus ]
Track 2      [Clip:bassline═══════][Clip:bass_ch  ]
Track 3  [───────Clip:pad_long─────────────────────]
Track 4  [Audio:vocals.wav══════════════]
         ↑ Can trim/fade audio clips
═══════════════════════════════════════════════════
```

### Universal UI Patterns

#### Command Palette (Cmd+K)
```
> transpose selection up 5
> quantize to harmony
> generate bassline
> add reverb to track 3
> set scale Dorian
> export stems
```

#### Radial Context Menus (Right-click)
```
        [Quantize]
    [Cut]     [Process]
         ●(target)
    [Copy]    [Generate]
        [Extract]
```

#### Scale Browser
```
┌─── Scale Browser ────────────────────────┐
│ Search: [______________]                 │
│                                          │
│ ▼ Common                                 │
│   • Major (Ionian)                       │
│   • Natural Minor (Aeolian)              │
│   • Harmonic Minor                       │
│   • Blues                                │
│ ▼ Modes                                  │
│   • Dorian                               │
│   • Phrygian                             │
│   • Lydian                               │
│   • Mixolydian                           │
│ ▼ Jazz                                   │
│   • Altered                              │
│   • Bebop Major                          │
│   • Diminished                           │
│ ▼ World                                  │
│   • Arabic Maqam Hijaz                   │
│   • Japanese Hirajoshi                   │
│   • Indian Ragas...                      │
│ ▼ Microtonal                             │
│   • 19-TET                               │
│   • 31-TET                               │
│   • Bohlen-Pierce                        │
│   • Custom...                            │
│                                          │
│ — Custom Scale Editor —                  │
│ Root: [440.0] Hz                         │
│ Intervals (cents):                       │
│ [0] [200] [400] [500] [700] [900] [1100]│
│ [+] Add interval                         │
│                                          │
│ [Import Scala] [Export]                  │
└──────────────────────────────────────────┘
```

#### Smart Drag & Drop
- Note → Patcher: Creates note generator
- Clip → Timeline: Places clip
- Clip → Patcher: Creates clip player
- Scale → Track: Sets harmony quantization
- Patcher → Track: Becomes insert effect

## Technical Implementation

### Nanotick Timebase
- 960,000 nanoticks per quarter note
- Deterministic conversion to samples
- Supports all common musical divisions
- No floating point in timing calculations

### Harmony System
```cpp
class HarmonyContext {
  // Query harmony at any point in time
  Scale getScaleAt(uint64_t nanotick);
  uint8_t getRootAt(uint64_t nanotick);

  // Quantization (always on, per-track setting)
  Note quantizeToScale(Note input, float strength = 1.0);
  vector<uint8_t> getChordTones(uint64_t nanotick);

  // For patcher nodes
  uint8_t nextScaleDegree(uint8_t current, int steps);
  float getIntervalCents(uint8_t degree);
};
```

### Microtonal Support
```cpp
struct MicrotonalNote {
  uint8_t midi_note;       // Closest MIDI note for VST3
  float cents_offset;      // Deviation from MIDI note

  // VST3 output uses per-note tuning at note-on/off (cents)
  // Internal synths can still use frequency directly
};
```

### Patcher Execution
```cpp
class PatcherNode {
  // Same interface works at all scales
  virtual MusicData process(MusicData input, TimeScale scale) = 0;
};

class PatcherEngine {
  // Different rates for different domains
  void processAudio(samples);      // Sample rate
  void processEvents(events);      // Control rate
  void processStructure(clips);    // Bar rate
};
```

### Groove System
```cpp
class GrooveTemplate {
  // Timing adjustments
  map<Subdivision, float> timing_offset;

  // Velocity adjustments
  map<Subdivision, float> velocity_scale;

  // Applied to all events (notes, automation, triggers)
  Event applyGroove(Event e, float amount);
};
```

## Roadmap

### Phase 1: Harmony Foundation (2-3 weeks)
- [x] Basic engine and audio output
- [x] Global harmony lane data structure
- [x] Tracker display with harmony column
- [x] Tracker harmony lane input (global)
- [x] Tracker multi-column input with free-text tokens (notes, degrees, chords)
- [x] Column-specific chord/degree editing and diffs
- [x] UI<->engine baseVersion checks with resync on mismatch
- [x] Undo for tracker note add/remove (global stack)
- [x] Microtonal scale model (Scala-style intervals: ratio + cents) + VST3 per-note tuning
- [x] Initial scale library + registry (builtins wired to UI)
- [x] Per-track harmony quantization toggle
- [x] Scale browser UI (palette command + browser overlay)
- [ ] Chromatic override mechanisms
- [x] Tracker rapid-edit integration tests (multi-column, chords, degrees, note-offs)
- [ ] Phase 1 automated tests (engine + SHM + UI parsing)

### Tracker Scrolling + Editing (UI milestone)
- [ ] Infinite tracker viewport: cursor snaps to grid, viewport scrolls in nanoticks.
- [ ] Zoom: `Cmd+Wheel` changes nanoticks-per-row; no quantization on zoom.
- [ ] Micro-scroll: `Shift+Wheel` scrolls view without row snap.
- [ ] Cmd+G jump: numeric input jumps to bar (e.g., `24` -> `24:1:0`).
- [ ] Selection model: time range + column mask; supports keyboard and mouse paint.
- [ ] Page ops: visible rows define Page; `Shift-F3/F4/F5` cut/copy/paste Page.
- [ ] Selection ops: `Cmd+C/X/V` operate only on selection; no selection -> toast.
- [ ] Loop: `Cmd+L` loops selection or Page if no selection.
- [ ] Semantic zoom: aggregate only when rows with data would overlap.
- [ ] Minimap: density strip ends at last data point; click to jump.
- [ ] Tests: page ops, selection vs page, Cmd+G parsing, zoom aggregation.

### Phase 2: Patcher Core (4-6 weeks)
- [ ] Node graph UI with pan/zoom
- [ ] Basic node types (generators, processors, routers)
- [ ] Patcher execution engine
- [ ] Harmony-aware nodes
- [ ] Clip manipulation nodes
- [ ] Real-time preview
- [ ] Fractal operations

### Phase 3: Mixer & Routing (3-4 weeks)
- [ ] Mixer view UI
- [ ] Plugin chains per track
- [ ] Sends/returns
- [ ] Bus routing
- [ ] Sidechain support
- [ ] Basic effects (EQ, Comp, Reverb)

### Phase 4: Sample Layer (3-4 weeks)
- [ ] Audio recording
- [ ] Sample playback
- [ ] Time-stretching
- [ ] Sample editing (trim, fade)
- [ ] Drum samples in tracker
- [ ] Audio clips in arrangement

### Phase 5: Groove System (2-3 weeks)
- [ ] Groove templates
- [ ] Timing and velocity maps
- [ ] Extract groove from audio/MIDI
- [ ] Per-track groove amount
- [ ] Apply to all event types

### Phase 6: Piano Roll (2-3 weeks)
- [ ] Piano roll view
- [ ] Same data as tracker
- [ ] Harmony highlighting
- [ ] Note editing tools
- [ ] Velocity/length editing

### Phase 7: Arrangement (3-4 weeks)
- [ ] Timeline view
- [ ] Clip placement
- [ ] Audio clips with trim/fade
- [ ] Automation lanes
- [ ] Zoom and navigation

### Phase 8: Advanced Patcher (4-6 weeks)
- [ ] DSP nodes (oscillators, filters, effects)
- [ ] Advanced clip manipulation
- [ ] Markov chains and probability
- [ ] AI/ML integration nodes
- [ ] Preset system

### Phase 9: Browser & Library (2-3 weeks)
- [ ] Unified browser for all assets
- [ ] Tagging system
- [ ] Search and filters
- [ ] Preset management
- [ ] Clip library

### Phase 10: Polish & Performance (4-6 weeks)
- [ ] Multi-threaded audio
- [ ] GPU-accelerated UI
- [ ] Modulation matrix
- [ ] MPE support
- [ ] Analysis tools (spectrum, meters)
- [ ] Export/render (audio, stems, MIDI)

## Key Decisions Made

1. **No Session/Clip Launcher View** - Focus on arrangement
2. **Harmony is Global** - Not per-clip, accessible everywhere
3. **Clips are Single-Track** - One clip = one track's data
4. **Musical Time Only** - No row numbers, use bar:beat:tick
5. **Quantization Always On** - Per-track toggle, not a node
6. **Fractal Operations** - Every operation works at every scale
7. **Zoomable Tracker** - Time resolution changes with zoom
8. **Tracker Column Semantics** - One entry per cell; columns are independent.

## Open Questions

1. Collaboration/version control system design
2. Cloud sync approach
3. Plugin sandboxing strategy (per-plugin vs shared)
4. Performance mode design (if any)
5. Mobile/tablet companion app
6. Hardware controller integration

## Success Metrics

- Can make a complete track using only tracker + patcher
- Sub-10ms latency with 10+ tracks and effects
- Harmony quantization feels musical, not mechanical
- Patcher enables both simple and complex operations
- UI remains responsive with large projects
- Fractal operations feel intuitive and consistent

## Development Principles

1. **Always Shippable** - Each phase produces a usable DAW
2. **Test with Real Music** - Make actual tracks at each phase
3. **Performance First** - Profile early and often
4. **Fractal from Start** - Build operations to work at all scales
5. **Harmony Native** - It's not a feature, it's fundamental

## File Organization

```
daw/
├── engine/           # C++ engine process
│   ├── scheduler/    # Nanotick-based scheduling
│   ├── harmony/      # Harmony context and quantization
│   └── clips/        # Clip management
├── host/            # C++ plugin host process
│   ├── vst3/        # VST3 support
│   └── effects/     # Built-in effects
├── ui/              # Rust/GPUI UI process
│   ├── tracker/     # Tracker view
│   ├── patcher/     # Node graph view
│   └── mixer/       # Mixer view
├── common/          # Shared definitions
│   ├── protocol/    # IPC protocols
│   └── types/       # Common data types
└── tests/           # Test suites
```

## Notes

This DAW is designed primarily for the creator's personal use but will be open-sourced. The focus is on power and versatility over mass appeal. The combination of tracker efficiency, visual programming, and fractal operations creates a unique tool that doesn't exist in the current market.

The key innovation is that **composition becomes programming** and **programming becomes composition**, with harmony as a native concept and time as a malleable dimension.
