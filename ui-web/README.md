# ui-web — web frontend for Uni

A DOM tracker driven by a view-model, with WebGL2 reserved for DSP scopes. Runs
alongside `ui/` (Rust/GPUI); neither replaces the other yet.

```sh
npm install
npm run tokens     # design/tokens.json -> src/tokens.css, design/out/{theme.rs,tokens.js}
npm run shots      # render, assert, screenshot, diff vs baselines
npm run bless      # accept current renders as baselines
```

**Read [GUIDELINES.md](GUIDELINES.md) before changing anything here.** It carries
the hard requirements, the performance rules with the measurement that justifies
each one, the domain invariants, and the measurement discipline — including the
mistakes that produced phantom findings. This README is the tour; that file is
the contract.

## Why it is built this way

Every rule below is a measurement taken on this machine at the redesign's real
per-row density (81 nodes/row against the design export's own 88), headless
Chrome, DPR 2, 100 keystrokes after warmup. `scratchpad/dens/` has the harness.

| Case | Nodes | Main thread / key |
|---|---:|---:|
| Edit, 64×16 | 7,433 | **1.50 ms** |
| Edit, 96×16 | 11,145 | **1.57 ms** |
| Edit, 64×16, tokenised | 7,433 | **1.41 ms** |
| Scroll by band transform, 64×16 | 7,433 | **1.54 ms** (Layout 0.000) |
| Scroll by band transform, 96×16 | 11,145 | **2.10 ms** |
| Scroll by naive rebind, 64×16 | 7,433 | **11.66 ms** |
| Scroll by naive rebind, 96×16 | 11,145 | **15.11 ms** |

**Rule 1 — scroll by transforming the band, never by rebinding visible cells.**
This is the only cliff, and it is steep: 11.7–15.1 ms against 1.5–2.1 ms, i.e.
70–90% of a 16.67 ms frame against ~10%. `Tracker.render()` rebinds a pooled row
only when its `data-row` changes, which happens for the handful of rows crossing
the band edge.

**Rule 2 — `contain: strict` on the grid, rows absolutely positioned.** This is
why PrePaint stays flat (0.047 → 0.082 ms from 3,087 to 11,145 nodes) and why
Layout reads `0.000` during a scroll. Total cost is sublinear: 3.6× the nodes
buys 1.7× the cost. The density worry was unfounded.

**Rule 3 — everything visual comes from `design/tokens.json`.** No hex, no font,
no px literal anywhere else. Tokenising measured *faster* than the design
export's inline styles (1.41 vs 1.50 ms) because it drops a per-cell
`color-mix(in srgb, oklch(...))` resolution. The token build also emits
`design/out/theme.rs`, so the pipeline stays kit-agnostic — swapping the renderer
never means re-typing colours.

Patcher, measured the same way: dragging a node with 8 attached wires among 400
costs **0.38 ms** in SVG and 0.26 ms in canvas; with 32 attached, 0.48 vs 0.33 ms.
Keep the design's SVG overlay — it is far inside budget and wires stay queryable
and hit-testable as real elements.

## Architecture

```
daw_engine (C++) → SHM → [Rust sidecar, not built yet] → ViewModel → Tracker (DOM)
                                                            ↓
                                          fixtures · goldens · agent assertions
```

`src/viewmodel.js` is the boundary. It describes only the visible window plus
overlapping clips — Uni's timeline is unbounded and zoom decides detail, so
there is no whole-document representation by construction. The renderer consumes
the view-model and nothing else; tests and `window.__uni` read the same shape.

Clip rails span rows, so they live **outside** the recycled band (`.tk-rails`),
keyed by clip id and transformed in lockstep with it.

## Agent loop

`test/shot.mjs` asserts on structure first and pixels second:

```
[deep]
  PASS  dom nodes bounded: 2539
  PASS  pool is viewport-sized, not timeline-sized: 74
  PASS  cell (99003,2,0) has geometry: {"x":564,"y":51,"w":76,"h":17}
  PASS  cell text readable via data attributes: "E-4"
```

Structural assertions catch logic bugs; goldens catch what structure cannot
express. `window.__uni.probe()` exposes `cellText(row,track,col)` and
`cellRect(row,track,col)` — the grid is queryable, which a canvas tracker would
not be.

## Not done yet

- **Fonts not vendored** — Inter and IBM Plex Mono still fall back to system
  mono, and Phosphor icons are absent. Goldens are not portable until this is
  fixed; font loading is the largest source of screenshot instability.
- No header row (tracks are unlabelled), the 148px clip lane is empty, the first
  row clips at the top, and the HUD overlaps the last row.
- No engine connection. The Rust sidecar that mmaps `/daw_engine_ui` and pushes
  binary frames over localhost is the next slice.
- Patcher, mixer, arrangement, browser, agent dock — designed, not built.
- Selection, mouse input, editing. Keyboard is arrows + `PageUp/Down` + `+`/`-`.
- Windows is entirely unmeasured, here and everywhere else in this evaluation.
