repo: unidaw/unidaw
branch: main

## Last sync
date: 2026-07-25T00:00:00Z

### Updated in this project
- Interface redesigned from the plan docs, ignoring the current implementation.
- One Design Component: `Uni.dc.html` — tracker (3 layouts), arrangement + scale roll, patcher, mixer, device chain, browser, agent dock, overlays.
- New concepts introduced beyond the plan: clips visible/editable in the tracker (grab rails + clip scope), per-track modulation columns, unified browser, agent dock with pending diffs.
- Built on the Nocturne design system (`_ds/nocturne-…`); Inter for chrome, IBM Plex Mono for all musical data.

## Screen map
| Screen | Built from |
| --- | --- |
| Tracker A ledger / B field / C ribbon | PLAN.md "COMPOSE Mode"; ZOOMEDITINGPLAN.md (zoom levels, aggregation pills, selection, page ops, minimap) |
| Clip model in tracker (rails, clip scope, elastic auto-clip) | New design work — no repo counterpart |
| Harmony + tuning dock card, scale browser | PLAN.md harmony/microtonal sections; README.md nanoticks, per-note cents |
| Arrangement + scale roll (piano roll as projection, chords unrolled to voices) | PLAN.md "ARRANGE Mode" |
| Patcher graph, tool rail, node palette, wiring with type checks | DEVICE_CHAIN_PATCHER_UX.md "Patcher UX (Graph View)"; PATCHER.md port types/rates |
| Device chain strip (macros, mapping, mod links) | DEVICE_CHAIN_PATCHER_UX.md |
| Mixer | PLAN.md "MIX Mode" |
| Browser rail (plugins/presets/samples/clips/patches/tunings) | New design work |
| Agent dock (context, command log, pending diff) | New design work — AI-native surface over the engine's command/version model |
| Command palette | PLAN.md "Command Palette (Cmd+K)" |

## Keyboard grammar (as designed)
F1/F2/F3/F8 modes · `[`/`]` tracker layout · ⌘± zoom · ⏎ write · ⌫ clear ·
⇧⏎ clip scope (⌘X⌘C⌘V, ⌘D, ⌥↑↓ move, ⇧←→ end, ⌥⇧←→ start, F fit, ⌘E split) ·
⌘B browser · ⌘J agent · ⌘K palette · ⌘⇧S scales · ⌘⇧P nodes · V/W/A/L patcher tools
