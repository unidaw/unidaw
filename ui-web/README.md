# ui-web — the Uni frontend

A web frontend for the Uni engine. HTML and CSS, no framework, no build step for
the app itself. It talks to the engine through a Rust sidecar that owns the
shared-memory bridge.

    ┌──────────┐   POSIX shm    ┌──────────────┐   WebSocket   ┌─────────┐
    │  engine  │ ─────────────► │  daw-sidecar │ ────────────► │ browser │
    │  (C++)   │ ◄───────────── │    (Rust)    │ ◄──────────── │  (JS)   │
    └──────────┘  command ring  └──────────────┘   commands    └─────────┘

Two sockets. **8174** carries state, write-only from the sidecar's side. **8175**
carries commands, with a blocking reader. One duplex socket does not work: a
non-blocking socket reports `WouldBlock` under ordinary backpressure, and a read
timeout that fires mid-frame corrupts the stream.

## The manual

**[docs/MANUAL.md](../docs/MANUAL.md)** — how to use the thing, for someone who already knows
trackers and DAWs. It names what refuses as well as what works. Read it before this file if
you want to make a noise rather than change the code. This README is the frontend's
*architecture*.

## Running it

    tools/webstack.sh            # engine + sidecar + page server, or says why not
    open http://127.0.0.1:8173/index.html

Background it from a script or an agent: the processes it starts are orphaned out
of its process group, but a caller that waits on the group will still wait.

It starts exactly one of each and refuses rather than guessing. Read the comments
in it before changing how it picks binaries.

## Surfaces

Six, all consuming the same published snapshot. `Tab` cycles the graphical ones;
`?` shows the keymap for whichever is up.

| | what it projects |
|---|---|
| tracker | time on Y, columns on X |
| arrange | time on X, tracks on Y |
| piano roll | time on X, pitch on Y |
| mixer | one strip per track |
| patcher | the engine's node graph, laid out here |
| browser rail (`B`) | projects on disk |
| agent dock (`/`) | the command stream, and a console over it |

## Driving it from code

`window.__uni` is the whole surface: `probe()`, `state()`, per-surface probes, and
`run(line)` for the dock's command grammar. Console commands route through the
same functions the keyboard does, so prefer `run()`.

    window.__uni.run('load webtest')
    window.__uni.run('goto 16 2')
    window.__uni.run('note 64')
    window.__uni.run('view arrange')

## Tests

    npm test         units + goldens + alloc   (fixtures; no engine needed)
    npm run unit     pure functions only       (fast, DOM-free)
    npm run perf     frame work per surface    (opens a real window)
    npm run e2e      one suite against a live engine
    npm run e2e:all  EVERY engine-backed suite, one at a time
    npm run soak     heap leak check           (fixtures; takes minutes)

`npm run e2e` is one file; `e2e:all` runs the two dozen other engine-backed
suites. One at a time: each starts an engine, a plugin host and a browser, and
several at once starve the audio producer. It prints what it did NOT run, and why,
on every run. `--only kit,ops` runs just those. `--with-audio` adds the five
suites that measure sound through a live capture.

The signal to read is the one at the device boundary; zero from the device means
the device never asked for audio.

    Audio device callbacks: N from the DEVICE, M reaching the engine

Not the older `0 of 0 playback callbacks`, which counts callbacks **that had a
track to play**, so a healthy device with the transport stopped prints `0 of 0`
too. "The producer reported no underruns" is not independent either — zero
callbacks produce zero underruns.

All engine-free checks at once, including the Rust side `npm test` does not reach:

    ../tools/verify.sh           rust + units + goldens + allocation
    ../tools/verify.sh --all     also frame work and a 4-minute soak

`npm test` never depends on a running sidecar, which leaves the engine boundary to
`npm run e2e`.

Goldens are committed PNGs; `npm run bless` accepts new ones. Look at the image
before you bless it.

`npm run alloc` measures **bytes allocated per draw**, using the sampling heap
profiler with `includeObjectsCollectedByMinorGC` — allocation, not retained heap.
See the top of `test/alloc.mjs`; do not bracket the measured loop with
`collectGarbage`, which under-reports at 14–40 bytes/draw. At 1500×760 with a
loaded project:

| | bytes/draw |
|---|---:|
| tracker, playing | 320 |
| tracker, scrolling | 703 |
| arrangement, panning | 143 |
| piano roll, panning | 225 |
| mixer, meters moving | 210 |
| device chain | 19 |

Frame work is under 1% of the 16.6 ms budget on every surface, including a busy
project three thousand bars in.

## Before you change anything

Read `GUIDELINES.md`, sections 2 and 2.1: the domain model, and the one bug this
project keeps having — a cache key that does not name everything the cached value
depends on. Six instances so far, each rendering something plausible while wrong.
