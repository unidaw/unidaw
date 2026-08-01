# ui-web — the Uni frontend

A web frontend for the Uni engine. HTML and CSS, no framework, no build step for
the app itself. It talks to the engine through a small Rust sidecar that owns the
shared-memory bridge.

    ┌──────────┐   POSIX shm    ┌──────────────┐   WebSocket   ┌─────────┐
    │  engine  │ ─────────────► │  daw-sidecar │ ────────────► │ browser │
    │  (C++)   │ ◄───────────── │    (Rust)    │ ◄──────────── │  (JS)   │
    └──────────┘  command ring  └──────────────┘   commands    └─────────┘

Two sockets, deliberately: **8174** carries state and is write-only from the
sidecar's side, **8175** carries commands and has a blocking reader. Trying to
make one socket duplex broke it twice — a non-blocking socket reports `WouldBlock`
under ordinary backpressure, and a read timeout that fires mid-frame corrupts the
stream.

## The manual

**[docs/MANUAL.md](../docs/MANUAL.md)** — how to use the thing, for someone who already knows
trackers and DAWs. Written against the source rather than against intent, so it names what
refuses as well as what works. Read it before this file if you want to make a noise rather than
change the code.

This README is the frontend's *architecture*: what talks to what, and why it is shaped this way.

## Running it

    tools/webstack.sh            # engine + sidecar + page server, or says why not
    open http://127.0.0.1:8173/index.html

From a script or an agent, background it: the processes it starts are orphaned
out of its process group, but a caller that waits on the group will still wait.

The script starts exactly one of each and refuses rather than guessing. Read the
comments in it before changing how it picks binaries — every line there is the
result of something that went wrong.

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

`window.__uni` is the whole surface: `probe()`, `state()`, per-surface probes,
and `run(line)` for the dock's command grammar. Every console command routes
through the same functions the keyboard does, so the two cannot drift — which is
why `run()` is the entry point to prefer.

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

`npm run e2e` is one file. There are two dozen more suites that stand up a real
engine, and until `e2e:all` existed they ran only when someone remembered them by
name — which is how several went weeks without running, and how a suite can rot
into permanent red with nobody noticing it went red. The runner goes one at a
time on purpose: each suite starts an engine, a plugin host and a browser, and
running several at once starves the audio producer and turns real checks into
load-average failures that say nothing about the code.

It prints what it did NOT run, and why, on every run. That is the point of it —
"all suites passed" has to mean the set it listed, not the set that happened to
be cheap that day. `--with-audio` adds the three needing a working output device;
`--only kit,ops` runs just those.

Or all of the engine-free ones at once, including the Rust side that `npm test`
does not reach:

    ../tools/verify.sh           rust + units + goldens + allocation
    ../tools/verify.sh --all     also frame work and a 4-minute soak

The split is deliberate. `npm test` must not depend on whether a sidecar happens
to be running — a test that changes with the weather is not a test. But that
leaves the engine boundary unexercised, and nearly every serious bug in this
codebase has lived exactly there, so `npm run e2e` covers the other half.

Goldens are committed PNGs; `npm run bless` accepts new ones. Look at the image
before you bless it. That is not a formality — the track headers spent months
labelled with the *next* track's name, and the last one blank, purely because the
baseline was blessed with it in place. Sixteen plausible labels photograph
exactly like sixteen correct ones.

`npm run alloc` measures **bytes allocated per draw**, using the sampling heap
profiler with `includeObjectsCollectedByMinorGC` — allocation, not retained heap.
The distinction is the whole point and it is explained at the top of
`test/alloc.mjs`: the version that bracketed the loop with `collectGarbage`
reported 14–40 bytes/draw while the renderers were throwing away hundreds of
strings a frame, because it collected the evidence before measuring it.

Where the draw path stands today, at 1500×760 with a loaded project:

| | bytes/draw |
|---|---:|
| tracker, playing | 320 |
| tracker, scrolling | 703 |
| arrangement, panning | 143 |
| piano roll, panning | 225 |
| mixer, meters moving | 210 |
| device chain | 19 |

Frame work is under 1% of the 16.6 ms budget on every surface, including a
busy project three thousand bars in.

## Before you change anything

Read `GUIDELINES.md`. Sections 2 and 2.1 in particular: the domain model, and the
one bug this project keeps having (a cache key that does not name everything the
cached value depends on — six instances so far, every one of them rendering
something plausible while being wrong).
