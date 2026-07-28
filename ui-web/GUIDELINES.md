# Uni web frontend — requirements and working rules

Everything here was either stated as a requirement or learned by measuring on
this machine. Each rule carries the number that justifies it, so a future change
can argue with the evidence rather than with the rule.

Measurements were taken in real Chrome with a real window on a 59.997 Hz display,
against Jaakko's own profile with extensions live, at 3008×1580 DPR 2.

---

## 1. Hard requirements

Stated by the owner. These are not negotiable and not up for re-derivation.

1. **Cross-platform** — macOS *and* Windows.
2. **Very performant. The UI must not lag.**
3. **Must accommodate DSP scopes later** — oscilloscopes, spectrum analysers, a
   scrolling spectrogram.
4. **AI-legible and AI-operable** — an agent must be able to see the UI, drive
   it, and assert on it.
5. **Avoid GC like the plague.**
6. **WASM is on the table** if it comes to that.

**Windows is entirely unmeasured**, here and in every earlier evaluation. Treat
any performance claim as macOS-only until someone runs it there.

---

## 2. Domain model

Getting these wrong produces bugs that look like performance problems.

- **There are no patterns. There is one continuous, unbounded timeline with
  clips on it.** Virtualisation is not an optimisation, it is mandatory; there is
  no size at which "just render it all" becomes acceptable.
- **Zoom decides how much detail a row carries.** Rows are a *projection* of the
  timeline, not a storage format.
- **Anything durable is expressed in ticks, never in rows.** A row index is a
  function of the current zoom, so storing one bakes today's zoom into tomorrow's
  data. This applies to everything crossing the engine boundary.
- **Clips contain notes.** Wherever a note exists there is a clip; no note exists
  outside one; the space between clips is genuinely empty. Coverage is decided
  *before* content, or notes float in gaps with no rail around them.
- **`lines_per_beat` belongs to the lane, not the viewport.** A lane can be at 3
  (triplets) or 6 (sextuplets) while its neighbour is at 4. Any code that assumes
  one grid for the whole screen is wrong, and will look right until a project
  mixes them.
- **There is exactly one row axis on the wire, and it is the viewport's.** Lanes
  are drawn against a shared axis, so a lane at 4/beat occupies every third row of
  a 12/beat axis and has *no row* in between. The lane grid decides what a row
  means in ticks — a note's duration, and which rows a lane has at all — and
  nothing else. Emitting rows in lane space and reading them as viewport rows is
  invisible while every lane agrees and misplaces every note the moment one does
  not.

### 2.1 The one bug this project keeps having

Twelve times now, in twelve different places: **content changed while the key the
consumer watches stayed the same.** (Thirteen, if you count 2.1.1 below, where
the thing that stood still was a test's own coverage.) Every instance rendered something plausible,
none errored, and no timing instrument could see any of them.

| Where | Content that moved | Key that didn't |
|---|---|---|
| `contentAt` | cell text (keyed on row index) | zoom changed ticks, not indices |
| `bindRow` | engine notes | row identity |
| aggregates | viewport moved | `notesRevision` |
| wire stride | note grew to 42 bytes | `NOTE_BYTES` still said 40 |
| sidecar note cache | rows reprojected on zoom | `clip_version` |
| client note cache | rows reprojected on zoom | `clipVersion`, `noteCount` |
| track names (sidecar) | a rename | `clip_version` |
| track names (client) | a rename | `clipVersion` |
| piano selection | ids reassigned on rewrite | the note id |
| arrange selection | placement_id is an index that shifts | the placement id |
| aggregate zooms | 1 bar and 4 bars are different | both asked for 1 line/beat |
| harmony (both sides) | a key change | the version, compared against itself |

The rule: **a cache key must name everything the cached value is computed from.**
If a value depends on the grid, the grid is in the key — which is why the frame
header carries `notes_grid` and the store carries `rowGrid`. When adding an input
to a derivation, the same commit adds it to the key, or the next person debugs a
screen that looks fine.

The last three are worth singling out. The two name rows were written by me in
one commit, *after* this table existed — I keyed names on the clip version out of
convenience, and a rename changes a name and nothing else, so the engine accepted
the command, the ack said ok, and the name never moved. The third is the same
shape from the other end: a selection keyed on note id emptied itself when the
engine reassigned ids on rewrite. **If there is no version for a thing, compare
the thing.** Names are 8x24 bytes; comparing them costs less than being wrong.

Corollary for tests: a fixture where every lane shares a grid cannot distinguish a
correct projection from a plausible one. `__uni.useMixedGrid()` exists for exactly
this and any grid work must be checked against it.

### 2.1.1 A fixture only tests where you point it

`useMixedGrid` was built to catch exactly the bug described above, and it did not,
because the golden that uses it is shot at **zoom 0 — the one zoom level where the
projection is correct.** Measured across the table:

| zoom | axis | 4/beat lane | 3/beat lane | 6/beat lane |
|---|---|---|---|---|
| 0 | 12/beat | 24/37 off-grid | 27/37 | 18/37 |
| 1 | 4/beat | **0/37** | **0/37** | **0/37** |
| 2 | 2/beat | **0/37** | **0/37** | **0/37** |
| 3 | 1/beat | **0/37** | **0/37** | **0/37** |

Zoom 0 is right: a lane occupies every (axis / itsLpb)-th row. At every other zoom
nothing is marked off-grid at all, so the tracker offers a writable cell on every
row of a triplet lane whose rows sit at 1/3-beat positions a 4/beat axis cannot
express. The cause is one rounding — `Math.round(4 / 3)` is `1`, and `r % 1` is
always `0`, so "incommensurable" becomes "every row is fine".

The lesson is not about grids. It is 2.1 applied to the TEST: **the fixture's
coverage was itself a key that stood still while the thing it tested varied.** A
fixture that exercises one point of a parameter it was built to protect is a
fixture that will pass while the feature is broken everywhere else.

So: when a fixture exists to protect a property that varies along an axis — zoom,
meter, channel count, sample rate — the test must sweep that axis, or say in one
line why one point is sufficient. `lcmGrid()` in `viewmodel.js` does the correct
arithmetic for this, is exported, and is called from nowhere: the machinery for the
right answer was written and never wired, which is its own kind of evidence.

The last three add two more shapes. An identifier that MOVES is not an identity:
`placement_id` is currently an extent's index, so a selection keyed on it jumps
to a different clip when the list changes — both the arrange and piano-roll
selections are keyed on (track, position) instead. And a version compared against
ITSELF is not a check at all: both harmony readers refreshed the version from the
snapshot earlier in the same function and then compared it to itself, so the
timeline would have been read exactly once. Hold the version the data you have was
READ at, separately from the version currently published.

### 2.1.2 A golden that is not reproducible is not a golden

Two scenes disagreed with their own baselines while every structural assertion
about them passed. Both were the harness, not the product, and both had already
been dismissed once as "pre-existing" — which is what a flaky golden buys you: it
trains everyone to explain the failure away, and then it hides the real one. The
556px regression underneath these was a genuine bug in my own draw path.

Three causes, all of them the same mistake — photographing a system that had not
finished moving:

1. **A CSS transition.** `.ch-reject` fades in over 120ms; the harness waited two
   animation frames, ~32ms. Same text, different opacity, a different image every
   run. Blessing froze one arbitrary point on the ramp and the next run picked
   another. Fixed by finishing every running animation
   (`document.getAnimations()` → `finish()`) rather than by deleting the fade: the
   ease-in is real product behaviour, and a harness that switches off the thing it
   is photographing tests a UI nobody uses. What a golden should capture is where
   a transition ENDS, which is the one part of it that is deterministic.

2. **Lazily-loaded fonts.** `document.fonts.ready` was awaited at boot, but it
   resolves for the faces wanted *at that moment*. A face that a later scene is
   the first to use is requested when that scene first paints a glyph in it. Await
   it per scene.

3. **Partial rasterisation.** Chrome re-rasterises only the tiles it believes are
   dirty and reuses the rest, so identical DOM can come out with different
   antialiasing depending on what was invalidated before it. This UI mutates its
   surfaces in place between scenes *by design*, which is exactly the case that
   varies. Pin it at launch: `--disable-partial-raster`,
   `--disable-gpu-rasterization`, `--force-color-profile=srgb`,
   `--disable-lcd-text`.

The tell for all three was the SHAPE of the diff, and it is worth recognising.
23 pixels, bistable rather than drifting, confined to the rounded corners of one
pill and two diagonal cable pixels. Straight edges land on the same pixel under a
sub-pixel shift; curves and diagonals are the only witness. A diff that small and
that shaped is never a feature change — it is the renderer, and chasing it through
the product code finds nothing, because there is nothing there.

So: before believing any golden diff, check the scene against ITSELF across two
runs. `magick a.png b.png -compose difference …` on two consecutive outputs
answers "is this a regression or a flake?" in one command, and it is the first
question, not the last.

### 2.3 Strides and offsets are load-bearing

Twice a field added to the wire has shifted everything after it. The first grew a
note from 40 to 42 bytes and decoded clip extents as garbage — rendering nothing,
erroring nowhere. The second put a count after a pad instead of in place of it,
and every note pitch read 0.

The encoder asserts its full header length against the constant the client
expects, in BOTH builds: `debug_assert` fails at the point of the mistake during
development, and a startup check exits in release, since release is what ships
and the one where a silent reinterpretation actually costs something. Add a field,
update both numbers, and the assertion tells you if you got it wrong.

---

### 2.15 A test that calls past the UI does not test the UI

The loop-drag handlers were written, wired, and verified through
`window.__uni.setLoop(...)` — which passed, while dragging the ruler with a real
mouse did nothing at all. `.ar-ruler` still carried `pointer-events: none` from
when it was decorative. A listener nobody can reach throws no error, logs
nothing, and satisfies every test that reaches the function directly.

So: **when the feature IS the interaction, drive the interaction.**
`page.mouse.down()` on the element, not the callback it eventually calls. The
agent-facing API and the pointer path have to be tested separately, because the
whole point of the agent-facing API is that it bypasses the pointer path.

The same shape applies to anything that makes a decorative element interactive:
`pointer-events`, `user-select`, and `z-index` were all chosen when nothing had
to be clickable, and none of them announce that they are now wrong.

### 2.19 A result the engine keeps publishing is not news

Two false alarms on the reject line, both the same shape: the engine publishes a
LAST-RESULT, and the client read every value it had not seen as an event that had
just happened.

- A fresh engine on its default project publishes `{loadSeq: 0, loadOk: 0}`,
  which means NO LOAD HAS BEEN ATTEMPTED. Compared against a `-1` sentinel it
  read as a failed load, so every startup said "the engine refused that project"
  about something nobody asked for.
- The engine keeps publishing the last load result indefinitely. So a page opened
  after somebody typed a bad project name greeted every later visitor with that
  refusal — a result belonging to a session that had ended.

The rule: **a client reports only results for actions it initiated.** The first
frame adopts the engine's counters silently; from then on a change is news.

### 2.18 The engine's own refusals were on the floor

The engine writes diffs and errors to `ringUiOut` and nothing read it, so a
refusal only the engine could make — a graph cycle, a chain error, a resync —
looked exactly like success: command sent, ack ok, read-back simply unchanged.
The sidecar drains it now and forwards the error kinds as JSON text on the state
socket.

Two things this ring demands:

- **It is SINGLE CONSUMER.** Whoever drains it takes the messages from everyone
  else. One thread in the sidecar drains; every client reads a shared buffer with
  its own cursor. A per-client drain would give each browser tab a random subset
  of the engine's errors, which is worse than not reading it at all. The C++
  device-chain tests also read this ring — on their own segments. If anything
  ever drains `/daw_web_ui` alongside the sidecar, both get partial history.
- **The note diffs are not forwarded**, and that is a decision, not an oversight:
  they fire on every edit and `clipVersion` already covers them. They are counted
  and the count is logged, so the ring never looks quiet while it is busy.

Related engine-state fact worth knowing: **the patcher graph is engine-lifetime
state, not project state.** Loading a project leaves it untouched; only a fresh
engine restores the pristine one. A test that edits the graph must delete exactly
what it added, by id, and must not assume it starts from a known shape.

### 2.16 A synthesised key is not the key the platform sends

Every `alt+` shortcut in the tracker and piano roll matched on `e.key`, and every
one of them was dead on macOS — where Option is a COMPOSE modifier, so Option+Q
delivers `key: 'œ'`, Option+C delivers `'ç'`, Option+S delivers `'ß'`. The tests
passed throughout, because Playwright synthesises `key: 'q'` for `Alt+Q`, which
is what Windows sends and macOS does not.

- **Match modifier combos on `e.code`.** It is the physical key and no modifier
  rewrites it. The cost is that on a non-US physical layout the shortcut follows
  the key's position rather than its printed letter — the usual trade, and much
  better than not firing.
- **When a test synthesises input, know what the platform actually sends.** The
  e2e now dispatches the real macOS values (`'œ'`/`KeyQ`) as well as the Windows
  ones. A green test that only ever produced input the platform does not produce
  is not evidence.

### 2.17 A test that measures the page must not also be a client of the engine

The soak, the goldens, the allocation and frame-work runs all booted the page,
which connects to `ws://127.0.0.1:8174` at startup. So a four-minute soak ran
attached to a live engine: it measured the page AND the decode of live frames,
and put an extra client on a stack another agent might be using.

`?engine=off` boots standalone with a null connection object of the same shape
(`send` returns false, exactly as a real connection with no socket does), so the
callers' "no engine" path is the one under test. Only the e2e connects, because
a live engine is the thing it is testing.

### 2.2 Two rules from index.html specifically

Five temporal-dead-zone bugs in one file. Four failed loudly — the page did not
boot — and the goldens catch those, because they record `pageerror`. The fifth
was silent and cost the most.

- **Anything that RUNS at module scope goes at the bottom**, after every
  declaration it touches. Declarations hoist; `const` and `let` do not
  initialise.
- **A catch that tolerates bad data must not span anything else.** The silent one
  was `try { s = JSON.parse(localStorage.getItem(SESSION_KEY)) } catch { return
  null }` — written to tolerate malformed JSON, it swallowed the ReferenceError
  from `SESSION_KEY` and the feature quietly did nothing while looking wired.
  Read outside the try, parse inside it.

---

## 3. Performance rules

Ranked by how much they cost to get wrong.

### 3.0 This is enforced, not just written down

`npm test` runs the goldens **and** `test/alloc.mjs`, which measures bytes
allocated per draw. The unfixed renderer allocated ~11,000. A rule in a document
decays; a failing test does not.

If it fails you have added one of these to a per-frame path — all of them
allocate, and all of them were present at some point in this file's history:

| Pattern | Instead |
|---|---|
| `` `${x}px` `` or `'a' + x` | cache the **number**, build the string only when it changes |
| `String(x)`, `x.toFixed()`, `padStart` | compare a cached number |
| `el.textContent = s` | own a Text node, write `.nodeValue` |
| `el.dataset.foo !== String(x)` | `el._foo !== x`, then write dataset |
| an unguarded `style.*` write | compare the cached number first |
| `for (const x of arr)` | an index loop — for-of builds an iterator per call |
| `{a, b}` passed to a per-frame callee | a module-level scratch object, mutated in place |
| `.map`/`.filter`/`.find`/`.forEach` | an index loop; the closure is per frame too |
| a value from a small closed domain | a table built once at load (128 pitches, 256 velocities) |
| anything O(tree) for a diagnostic | put it on a timer |

The residual is two strings for the one row whose identity genuinely changed —
its `top` and its `data-row`. That is the floor, not waste.

### 3.0.1 Measure allocation, not retained heap

The first version of `test/alloc.mjs` bracketed the loop with
`HeapProfiler.collectGarbage` + `Runtime.getHeapUsage`. **That measures what
SURVIVED**, which is close to the opposite of what this rule is about: every
allocation a draw makes is dead by the end of the frame, so the collectGarbage
took it all away before the measurement. It read 14–40 bytes/draw and passed
while three independent audits were finding hundreds of transient strings per
frame in the same code. It was not measuring a clean draw path; it was measuring
the absence of a **leak**, which is a weaker and different property.

The test now uses `HeapProfiler.startSampling` with
`includeObjectsCollectedByMinorGC`, which counts allocations whether or not they
survive. It keeps the retained check as a separate assertion, because a leak and
a churn are both worth failing on and neither implies the other.

The corollary is a general one and it has now caught this project twice: **a
green test proves nothing until you have checked that it can go red.** Before
trusting a performance number, break the thing on purpose and confirm the number
moves.

### 3.0.2 Cover the path the program is actually on

Every scenario in `test/alloc.mjs` loaded the page with `?engine=off`, so for the
whole life of that file the branch it never measured was the one that runs when a
project is open: engine notes spelled into cells, harmony fields named, clip
rails from real placements. `__uni.useBusyEngine()` / `__uni.tickBusy()` exist so
the measured case is a loaded project, playing.

The same mistake in a different costume: a scenario that only moves a cursor does
not exercise the list under it. "Patcher moving the field cursor" passed at 14
bytes/draw while `buildPatcherModel` was allocating 433 bytes per call, because
the fixture had no selected node with a config.

### 3.1 Zero allocation in the draw path
The view-model is pooled and double-buffered; rows, cells, clips and rails are
allocated once per shape change and mutated in place. **0 GC events** across 80
sustained keypresses.

`npm run soak` covers the leak case that `npm test` cannot: 753 passes over every
surface in 3.5 minutes drifted **17 KB/min** on a 3.2 MB heap, and the heap fell
on the last sample — which is what a non-leak looks like. Pools settle ~560 KB
above cold start over the first minute; that is one-off, and measuring drift from
before it reported 238 KB/min for a heap moving at 17.

Double-buffering is not optional: the renderer diffs the previous view-model
against the current one, so mutating a single buffer silently turns every draw
into a full rebind.

### 3.2 Own your Text nodes
`element.textContent = x` **destroys and recreates a Text node on every write**.
Append a Text node at construction and write `.nodeValue`, which mutates in place.

> DOM node mutations over a 5-minute soak: **621,239 → 12**

### 3.3 Scroll by transforming the band, never by rebinding visible cells

| | 64×16 | 96×16 |
|---|---:|---:|
| transform | 1.54 ms | 2.10 ms |
| naive rebind | **11.66 ms** | **15.11 ms** |

This is a rule for **every** timeline surface, not just the tracker. The
arrangement and the piano roll each spent their first year positioning every
clip, note, gridline and ruler tick at `(tick - startTick) / ticksPerPixel`,
which means a pan rewrites every element's position string — 3.9 KB and 3.6 KB
per frame respectively, measured, against 0.02 KB at rest. Both now place
elements at their absolute `tick / ticksPerPixel` inside a wrapper and translate
the wrapper: **3,981 → 144** and **3,664 → 228** bytes/draw.

The one thing this makes easy to get wrong: a zoom changes `ticksPerPixel` and
therefore every element's absolute position, so the rebind guard must name the
zoom. A guard that names only the scroll position leaves everything where it was
and looks like a zoom that did nothing.

### 3.4 Recycle the pool as a ring, not a list
Slot is `row mod poolSize`. Indexing by position shifts every element's identity
on every scroll, so a one-row move costs a full rebind. The tell was that a 1-row
and a 32-row scroll cost *exactly the same* 7.8 ms.

> cursor step 3.97 → **0.24 ms**, scroll 1 row 7.75 → **0.36 ms**

Absolute `top` is what makes this legal — an element is placed by the row it
holds, so which slot it occupies is irrelevant.

### 3.5 `contain: strict` on the grid, rows absolutely positioned
This is why PrePaint stays flat — **0.047 → 0.082 ms from 3,087 to 11,145
nodes** — and why Layout reads `0.000` during a scroll. Total cost is sublinear:
3.6× the nodes buys 1.7× the time.

Per-row `contain: layout style paint` was tried and made it **worse** (13.77 →
15.39 ms): scoping 66 rows costs more bookkeeping than the sibling invalidation
it prevents.

### 3.6 Guard every write
Assigning a style or a text value dirties the node even when the value is
identical. Compare first; reading is far cheaper than invalidating.

### 3.7 Pool DOM elements; hide, never remove
Clip rails were created and destroyed as clips scrolled in and out — 219 added,
213 removed over 700 keypresses, each an allocation plus a layout. Surplus
elements are now hidden, so the pool high-water-marks and stops mutating the DOM.

### 3.8 Coalesce input into one draw per animation frame
Key repeat is ~30/s. Without coalescing, several synchronous draws queue inside
one frame.

### 3.9 Split work that does not fit one frame
A zoom re-contents every row: 14.4 ms of a 16.6 ms budget, 86% utilisation with
nothing spare. Visible rows bind in the input frame, overscan in the next.

> worst single frame **14.39 → 9.29 ms**, total work unchanged

### 3.10 Never let a diagnostic into the hot path
`host.querySelectorAll('*').length` in the HUD was an O(tree) walk per frame —
more expensive than the render it reported on. Sample diagnostics on a timer.

### 3.11 Measure geometry from CSS; do not duplicate the box model
A hand-computed scroll extent forgot the 2px per-track border — 32px across 16
tracks, leaving the last 30px permanently unreachable. `cellLeft()` and
`maxScrollX()` measure the real row once per shape change. Forces layout, so it
runs on resize only.

### 3.12 Never read a box in the draw path

`clientWidth`, `clientHeight` and `getBoundingClientRect()` force the browser to
flush pending style and layout so it can answer. `draw()` writes styles and then
read a box, five times over — five synchronous layouts inside one animation
frame, which is textbook thrashing.

Host boxes come from a `ResizeObserver` (`hostBox` / `boxOf()` in `index.html`),
which is handed the measurement rather than asking for it. The renderer takes the
viewport height as an argument instead of re-reading the element it was already
measured from.

The cached value is `0` when a host is hidden, and `boxOf` falls back to a live
read in that case — the frame after a view switch unhides a host, the observer
has not fired yet, and drawing a surface at width 0 for one frame is a flash.

### 3.13 Positions outgrow small integers about nine minutes in

Nanoticks run at 960,000 to the quarter. V8's small-integer range with pointer
compression is ±2³⁰, which a song passes at roughly bar 280 — about nine minutes
at 120 BPM. From there **every tick expression in the draw path produces a boxed
double on the heap**: window bounds, the cursor's position, the minimap's span.
It costs about a kilobyte a frame and it cannot be guarded away without giving up
absolute-tick arithmetic.

Two consequences, both of which have bitten:

1. **Do not compute a tick you are not going to use.** `buildViewModel` was
   computing each row's absolute tick unconditionally when only the fixture and
   the label rebuild need it — 62 boxed doubles a frame, invisible in any short
   test.
2. **A benchmark must say where in the song it stands.** Two scenarios in
   `test/alloc.mjs` were implicitly running at bar 3,000 because an earlier
   scenario had scrolled there, and read four times what the same code measures
   from the top. `test/alloc.mjs` now has a deliberate "hour into the song"
   scenario with its own limit, and every other scenario resets zoom, scroll and
   playhead in its setup.

---

## 4. Measurement discipline

Two of three "findings" in one session were errors in the instrument, not the
app. This section exists because of that.

- **The frame budget is 16.6 ms of *work*, not a 33 ms interval.** Measuring rAF
  intervals against 33 ms says "no dropped frames" while the app sits at 95%
  utilisation with no margin.
- **Measure keydown→draw with ONE `rAF`, not two.** The app's `schedule()` queues
  its callback first, so the draw has already run by the first one. Waiting for a
  second measured a frame the app never spent, and produced a phantom "p95 30.7 ms
  GC tail" that did not exist.
- **Headless has no vsync.** Frame intervals come back at 8.2 ms p50, which is
  impossible on a 60 Hz panel. Use `headless: false` for anything timing-related.
- **`channel: 'chrome'` is not the same as headed.** It selects real Chrome; it
  does not give you a window.
- **Run long.** 3 seconds found nothing; 28 seconds found 219 nodes of DOM churn;
  5 minutes confirmed no heap trend. Bucket results into thirds to catch drift.
- **Cost that does not scale with the input is not doing the work you think.** A
  1-row and a 32-row scroll costing the same was the whole diagnosis.
- **Read the trace before optimising.** Three consecutive guesses at why scroll
  was slow were all real problems and all changed nothing; a CDP trace showing
  `Layout` at 4.76 ms during a transform-only scroll found the actual cause.
- **A press that changes nothing is indistinguishable from a press that was
  slow.** Verify that a visual change *occurred* — an image diff of adjacent
  states catches "0 pixels changed" instantly.
- **Do not assume the obvious optimisation helps.** Per-row containment made
  things worse; a reported "rayon is 3–6× slower" did not reproduce at all.
- **Measure against the real environment.** Connect over CDP to a clone of the
  user's own profile with extensions live, not a sanitised launch.
- **The user's perception outranks the instrument.** Every disagreement so far
  resolved in the user's favour.

---

## 4.5 Surfaces

Six of them now, all consuming the SAME engine store. Nothing below re-reads the
engine differently; they differ only in what they project onto the screen.

| Surface | Projection | Needs from the engine |
|---|---|---|
| tracker | time on Y, columns on X | notes, aggregates, per-lane grids |
| arrange | time on X, tracks on Y | clip extents (incl. the audio flag) |
| piano roll | time on X, pitch on Y | notes |
| mixer | one strip per track | peaks, gain, pan, mute/solo |
| browser rail | projects and plugins on disk | nothing; the sidecar reads both from disk |
| patcher | the node graph, laid out | patcher nodes, edges and per-node config |
| agent dock | the command stream | nothing |

Two rules that came out of building them:

- **A surface must say what it does not know.** The mixer carried a line on screen
  saying its faders were a local guess for as long as the engine published no
  mixer state, and exposed `authoritative: false` so a test could assert it was
  still being honest; when the read-back landed, both went away. The patcher says
  the graph it shows is the engine's one global graph rather than implying a
  per-device view it does not have. The alternative — a confident control over a
  guess — is the same silent-plausible-wrongness section 2.1 is about.
- **Anything unimplemented refuses out loud.** Clip edits and the effect column
  name what is missing rather than doing nothing. A control that silently
  ignores you is indistinguishable from one that worked. What is left: arrange
  clip edits, waiting on a stable placement id from the engine (`placement_id`
  is an index today), and reading the engine's own error ring, whose format is
  not published — a refusal only the engine can detect is still silent.
- **A refusal the other side wrote is a refusal this side must show.** The
  sidecar answers every command with `{"ok":true}` or `{"error":"..."}`, and
  those error strings are written to be read — "those two node types have no
  compatible ports" is the answer to the question just asked. They were logged to
  a dock that is usually closed and dropped otherwise, so careful refusals on one
  side arrived as silence on the other. Every error ack now lands on the chrome's
  reject line.
- **A read-back the UI does not fully write is a config the engine will zero.**
  The engine rebuilds a patcher node's whole config from the payload it receives,
  so the eight published values all go back, edited field aside. `duration_ticks`
  was not in the first field table and would have been silently zeroed by every
  edit; it is named now. The general rule: when a write replaces a record rather
  than patching a field, the UI has to carry every field of that record, whether
  or not it offers a control for it.

The agent-facing contract is the dock's command grammar, not the keymap: every
command routes through the same functions the keys do, so the two cannot drift.
`window.__uni.run(line)` is the entry point. `src/help.js` documents the keymap
as data, but the handler stays authoritative — if they disagree, help.js is a
stale document.

---

## 5. Architecture

```
daw_engine (C++) → SHM → Rust sidecar → binary frames → ViewModel → DOM
                                                            ↓
                                          fixtures · goldens · agent assertions
```

- **The view-model is the boundary.** Plain data describing only the visible
  window plus overlapping clips. The renderer consumes it and nothing else;
  tests and `window.__uni` read the same shape. Swapping the renderer means
  reimplementing one consumer, not re-deriving the projection.
- **One token source.** `design/tokens.json` → CSS custom properties, a Rust
  `const Theme`, and a JS object. No hex, no font name, no px literal anywhere
  else. Tokenising measured *faster* than inline styles (1.41 vs 1.50 ms) by
  dropping a per-cell `color-mix(oklch())`.
- **Reuse `daw-bridge`; never hand-mirror the SHM structs.** Three guarded
  mirrors exist already, pinned by `static_assert` / `const_assert_eq!`. A fourth
  unguarded one produces wrong notes, not a compile error.
- **The acquire fence belongs in Rust**, not in JS over an ArrayBuffer.
- **WebGL2 for DSP scopes, in a worker via `OffscreenCanvas`.** Measured
  headroom: 137,408 line vertices plus a live 512×1024 spectrogram at 3200×2000
  in 1.55 ms. Workers cannot help the tracker — style, layout and paint are
  main-thread by definition — but they keep scope rendering off it entirely.

---

## 6. Agent loop

- **Assert on structure first, pixels second.** `data-row` / `data-track` /
  `data-col` on every cell; `window.__uni.probe()` exposes `cellText(row, track,
  col)` and `cellRect(row, track, col)`. Reading back 1,024 cells with zero
  misses is what makes a DOM tracker legible where a canvas is opaque pixels.
- **Prefer an invariant to an eyeball.** "Every cell with content falls inside a
  visible rail's span" caught the clip-containment bug in one number.
- **A diff must report a bounding box, not a percentage.** A percentage does not
  say where.
- **Goldens are not portable until the fonts are vendored** — they currently
  encode this machine's mono fallback. Bless them in the same change that vendors
  Inter and IBM Plex Mono.
- `npm run shots` renders, asserts, screenshots and diffs. `npm run bless`
  accepts.

---

## 6.5 The M3 clip model — design for this, not for flat notes

Announced by backend before it lands, so the tracker is designed once. Three
layers, and the tracker is a view over the resolved result of all three.

- **Clip** is a reusable *definition*, project-level, with **clip-relative**
  events and its own length. Not a span on a track.
- **Placement** drops a clip on a track: `{clipId, at, length, adds[], mutes[]}`.
  `at` is the absolute tick where the clip's tick 0 lands. A shorter clip **loops**
  to fill the placement — it is not copied N times. `at` may be null, meaning a
  loose session cell.
- **Overrides are additive-only and one level deep.** `adds[]` are notes local to
  one placement; `mutes[]` are EventIds silenced in one placement. Resolved =
  base − mutes + adds. This is what lets chorus 3 gain a hihat without copying the
  clip. There is no SetField and no deeper nesting, by ruling.

Consequences the renderer has to carry:

1. **A note's row is `at` + its clip-relative tick**, so several placements of one
   clip produce several rows from the same base note.
2. **Overlapping placements can co-locate ticks.** Merge by `note_id`; never let a
   position key silently overwrite. This is the `contentAt` failure class and
   backend called it out by name. Until provenance arrives, a collision renders as
   `**` rather than quietly dropping a note.
3. **Rails are the same clip list in two views** — arrange draws blocks, the
   tracker shades `[at, at+length)` with start and end markers.
4. **Edit target is a mode**: BASE edits the clip and every placement of it,
   OVERRIDE touches one placement. The UI must show which mode it is in and which
   placement the cursor is over. A mute is a struck-out base note; an add shows
   provenance; each placement gets a count badge and one-click revert.
5. **Note entry creates or stretches a clip** — stretch within about a bar of an
   edge, otherwise a new clip. Entry still feels free; clips form underneath.

### Answered by backend, and binding at M3.4

**The merge key is `(placementId, iteration, noteId)` — three parts.** Identity
lives in the clip definition: one EventId per clip note, and repeats do **not**
mint new ids. A 1-bar clip in a 4-bar placement is the same note sounding four
times with the same id, so a two-part key would drop three of them. Tick is
`at + iteration * clipLength + noteTick`. The `**` collision marker is the
placeholder until those fields arrive.

**Editing any repeat targets the clip note.** Change one, all four repeats and
every other placement of that clip change — that is the point of a shared clip.
To vary a single bar you do not loop; you place the clip explicitly per bar and
override the one you want. Override is per-**placement**, affecting all its
iterations, never per-iteration. Looping means deliberately uniform repeats.

**Muted notes ship with a flag; the feed is DISPLAY-resolved, not
playback-resolved.** It carries base notes each with a played/muted bit, plus
adds. Playback omits muted notes; the feed keeps them so they can be drawn struck
out, and the base clip is never needed separately. So the guarantee is not "what
plays is what is shown" but the stronger and more useful *"the feed shows what
plays plus what is deliberately silenced, labelled"* — which is what an editor
needs and a player does not.

**Loose placements never arrive.** A null `at` has no timeline position, and the
all-tracks region excludes those placements entirely rather than leaving them for
the client to filter. Session cells get their own feed when the session view lands
past M3.5. Nothing to pin, nothing to skip.

So a rendered note row is `(placementId, iteration, noteId, muted?, isAdd?)` at an
absolute tick, and rails are the non-null-`at` placements only.

Arriving at M3.4, announced before the contract moves: `placementId` and a
base/add flag folded into `UiClipNote`'s spare bytes, plus a clip-extents feed
`{placementId, clipId, trackId, startTick, endTick, name}` — the shape the rails
need. The note feed itself stays the v9/v10 all-tracks region and stays
engine-**resolved**, so what plays is what is shown and the decoder survives.

## 7. Open and unmeasured

- **Windows** — nothing has ever been run there.
- **Real engine data** — the fixture is `Math.sin`; real notes change the
  allocation profile and the clip shapes.
- **Modulation columns** — the redesign adds them on top of the three columns
  built here, and cell count is the one thing measured to scale cost.
- **Cross-track selection over lanes in different meters.** The grids themselves
  are settled (see below), but ARCHITECTURE_REVIEW Movement 1 item 11's harder
  half is not: a selection spanning a 4/4 lane and a 3/4 one has no obvious "the
  row I am on", and paste into it has no obvious meaning. Nothing is broken today
  because the selection is keyed on rows and rows are shared; the question is what
  it should DO, and that is interaction design rather than a defect.
- **`lcmGrid()` is exported and called from nowhere.** It was the machinery for the
  LCM-axis answer, which was not the one taken. Either wire it or delete it — an
  exported function with no caller reads as a feature somebody forgot to finish.

Recorded as done rather than deleted, so the list stays honest about what it has
and has not been tracking:

- Clip edges have visible end grips (`.tk-rail::before/::after`, plus solid caps in
  the rail's own gradient).
- Aggregate cells that read `[30x]`–`[45x]` as uniform noise carry the pitch ribbon
  built from `RowAggregate`'s `pitch_min`/`pitch_max`.
- **Per-lane grids are correct at every zoom.** The rounding in 2.1.1's table is
  fixed — the test is in ticks, multiplied through by lines-per-beat rather than
  dividing by it, so it is exact for every value the engine can publish. The grid
  belongs to the CLIP (v19), anchored at the clip's start, so a lane's grid changes
  down the timeline; a unit test sweeps all four non-aggregate zooms with
  expectations derived from the meter rather than transcribed.
- **A lane numbers itself.** Where a track's bars are not the song's — a different
  meter, or an origin off a song bar — the lane carries its own bar:beat readout.
  Only there: a lane that agrees with the gutter says nothing, which is what keeps
  the column affordable. This made track widths non-uniform, so the renderer's
  `trackStride` is a per-track array.
- **Deviation is visible.** A note that does not land on its row draws a hairline at
  its true position inside the cell (item 13's visible half). It was a bare "D"
  before, which said a deviation existed without saying which way or how far.
