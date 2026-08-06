#!/usr/bin/env node
// Unit tests for the pure functions the view-models are built out of.
//
//   node --test test/unit.mjs
//
// The goldens and the e2e cover these indirectly, which means a bug in one shows
// up as a wrong screenshot or a failed engine assertion — true, but a long way
// from the cause. These are the edge cases: the triplet grid, the token that
// should be refused, the fader taper, the identity a selection is keyed on.
//
// Everything here is deliberately DOM-free, so it runs in Node with no browser.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, readdirSync, existsSync, writeFileSync,
         mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

import { lcmGrid, ZOOM_LEVELS, buildViewModel, createBuffer } from '../src/viewmodel.js';
import { ROW_OPS, OP_MASK, opGlyph, opsRun, opsText, opsPresent, opTokenAt,
         makeTrigCondition, parseOps } from '../src/rowops.js';
import { DEVICE_KINDS, SLOT_FIELDS, modSummary } from '../src/chainmodel.js';
import {
  parseToken, parseChord, pitchOf, pitchToToken, hexValue, shiftDigit, NOTE_KEYS,
} from '../src/entry.js';
import {
  gainLabel, panLabel, faderPosition, gainAtPosition, GAIN_MIN, GAIN_MAX,
} from '../src/mixermodel.js';
import { isBlackKey, pitchLabel, fitLowPitch } from '../src/pianomodel.js';
import { describeConfig, configFields, nudgeConfig, dragSteps,
         NODE_TYPES } from '../src/patchermodel.js';
import { snapLoop, TICKS_PER_BAR, buildArrangeModel, dragPlacement, clipZoneAt,
         CLIP_HANDLE_PX, createArrangeBuffer } from '../src/arrangemodel.js';
import { buildHarmonyModel, createHarmonyBuffer } from '../src/harmonymodel.js';
import { velocityText } from '../src/viewmodel.js';
import { trackName } from '../src/arrangemodel.js';
import { createField, begin as fBegin, feed as fFeed, cancel as fCancel } from '../src/textfield.js';
import { fillRows, setProjectRow, setPluginRow, makeRow,
         KIND_PROJECT, KIND_PLUGIN } from '../src/browser.js';
import { ticksPerBar, ticksPerBeat, positionOf, createPosition, sameMeter,
         meterText } from '../src/meter.js';
import { buildChainModel, createChainBuffer, paramKey, createParamEdits, findParamEdit,
         setParamEdit, dropParamEdit, reapParamEdits, MAX_PARAMS,
         EDIT_HOLD_MS, meterScale, meterDb, METER_SILENT } from '../src/chainmodel.js';
import { createCommands, checkArgs, runCommand, parseHelpArgs } from '../src/dock.js';
import { unpackClipGrid } from '../src/wire.js';

test('lcmGrid finds a grid every lane lands on', () => {
  assert.equal(lcmGrid([4, 4, 4]), 4);
  assert.equal(lcmGrid([4, 3]), 12);
  assert.equal(lcmGrid([4, 3, 6]), 12, 'the maximal project');
  assert.equal(lcmGrid([3, 6]), 6);
  // Capped: past 24 per beat the rows are too dense to read and aggregation
  // takes over, so an absurd lane must not produce an absurd grid.
  assert.equal(lcmGrid([5, 7, 11]), 24);
  assert.equal(lcmGrid([]), 1);
  assert.equal(lcmGrid([0, 4]), 4, 'a zero lane is unset, not a divisor');
});

test('zoom 0 can represent every lane grid the domain allows', () => {
  const z = ZOOM_LEVELS[0];
  for (const lpb of [1, 2, 3, 4, 6, 12]) {
    assert.equal(z.linesPerBeat % lpb, 0, `${lpb}/beat lands on the ${z.label} axis`);
  }
});

test('the piano keymap is two octaves and has no note on `a`', () => {
  // The spec, verbatim: "z=C-3 while q=C-4, b=G-3, u=B-4, i=C-5, m=B-3",
  // all at octave 4. The lower row is an octave below the upper one.
  assert.equal(pitchToToken(pitchOf('z', 4)), 'C-3');
  assert.equal(pitchToToken(pitchOf('q', 4)), 'C-4');
  assert.equal(pitchToToken(pitchOf('b', 4)), 'G-3');
  assert.equal(pitchToToken(pitchOf('u', 4)), 'B-4');
  assert.equal(pitchToToken(pitchOf('i', 4)), 'C-5');
  assert.equal(pitchToToken(pitchOf('m', 4)), 'B-3');
  // `a` is note-off, the one edit in that column that is not a pitch.
  assert.equal(NOTE_KEYS.a, undefined);
  assert.equal(pitchOf('a', 4), -1);
});

test('a pitch outside MIDI range is refused rather than clamped', () => {
  assert.equal(pitchOf('z', 0), 0);
  assert.equal(pitchOf('z', -1), -1, 'below 0');
  assert.equal(pitchOf('i', 9), -1, 'above 127');
});

test('parseToken reads notes, degrees and off; refuses the rest', () => {
  assert.deepEqual(parseToken('C-4', 0), { kind: 'note', pitch: 60 });
  assert.deepEqual(parseToken('c#3', 0), { kind: 'note', pitch: 49 });
  assert.equal(parseToken('---', 0).kind, 'off');
  assert.equal(parseToken('off', 0).kind, 'off');
  assert.equal(parseToken('', 0).kind, 'empty');
  assert.deepEqual(parseToken('24-4', 0), { kind: 'degree', degree: 24, octave: 4 });
  // Never a silent drop: a cell that ignores what you typed is
  // indistinguishable from one that took it.
  assert.equal(parseToken('zzz', 0).kind, 'invalid');
  assert.equal(parseToken('H-4', 0).kind, 'invalid');
});

test('value columns take hex, note columns do not', () => {
  assert.deepEqual(parseToken('64', 1), { kind: 'value', value: 64 });
  assert.equal(parseToken('ff', 1).value, 255);
  assert.equal(parseToken('64', 0).kind, 'invalid', 'a bare number is not a note');
});

test('parseChord reads the AGENTS.md grammar', () => {
  const c = parseChord('@3^7~80h20');
  assert.equal(c.kind, 'chord');
  assert.equal(c.degree, 2, '1-based in the token, 0-based on the wire');
  assert.equal(c.quality, 2, '^7 is a seventh');
  assert.equal(c.spread, 80);
  assert.equal(c.ht, 20);
  assert.equal(c.hv, 20);
  assert.equal(parseChord('@1').quality, 1, 'a triad by default');
  assert.equal(parseChord('@1^1').quality, 0, 'a single note');
  assert.deepEqual(
    [parseChord('@5i1o5').inv, parseChord('@5i1o5').oct], [1, 5]);
  assert.equal(parseChord('@4/960000').dur, 960000);
  assert.equal(parseChord('C-4'), null, 'not a chord token at all');
  assert.equal(parseChord('@').kind, 'invalid');
  assert.equal(parseChord('@99').kind, 'invalid', 'degree out of range');
});

test('hex digits and the shifting value field', () => {
  assert.equal(hexValue('0'), 0);
  assert.equal(hexValue('f'), 15);
  assert.equal(hexValue('F'), 15);
  assert.equal(hexValue('g'), -1);
  // 6 then 4 reads 06 then 64, as a tracker's field does.
  assert.equal(shiftDigit(0, 6), 6);
  assert.equal(shiftDigit(6, 4), 100);
  // 100 is 0x64; shifting a 1 in gives 0x41. The field keeps the last two
  // digits typed, so the leading 6 falls off rather than the value clamping.
  assert.equal(shiftDigit(100, 1), 0x41);
});

test('gainLabel never prints a rounding artefact as a cut', () => {
  assert.equal(gainLabel(0), '0.0');
  assert.equal(gainLabel(-1), '0.0', 'not "-0.0"');
  assert.equal(gainLabel(1), '0.0');
  assert.equal(gainLabel(630), '+6.3');
  assert.equal(gainLabel(-490), '-4.9');
  assert.equal(gainLabel(GAIN_MIN), '-inf');
});

test('pan reads as musicians write it', () => {
  assert.equal(panLabel(0), 'C');
  assert.equal(panLabel(-400), 'L40');
  assert.equal(panLabel(600), 'R60');
});

test('the fader taper is monotonic and round-trips', () => {
  let prev = -1;
  for (let mb = GAIN_MIN; mb <= GAIN_MAX; mb += 100) {
    const p = faderPosition(mb);
    assert.ok(p >= prev, `monotonic at ${mb}`);
    prev = p;
  }
  assert.equal(faderPosition(GAIN_MIN), 0);
  assert.equal(faderPosition(GAIN_MAX), 1);
  // Unity sits high on the travel, which is the point of a taper.
  assert.ok(faderPosition(0) > 0.6 && faderPosition(0) < 0.8);
  for (const pos of [0, 0.25, 0.5, 0.702, 0.9, 1]) {
    assert.ok(Math.abs(faderPosition(gainAtPosition(pos)) - pos) < 0.01,
              `round trip at ${pos}`);
  }
});

test('the keyboard knows its black keys', () => {
  assert.equal(isBlackKey(60), false, 'C');
  assert.equal(isBlackKey(61), true, 'C#');
  assert.equal(isBlackKey(59), false, 'B is a white key');
  assert.equal(isBlackKey(58), true, 'A# is black');
  assert.equal(pitchLabel(60), 'C4');
  assert.equal(pitchLabel(61), 'C#4');
  assert.equal(pitchLabel(0), 'C-1');
});

test('fitLowPitch centres on the material, or falls back', () => {
  const engine = {
    noteCount: 3,
    notes: [{ pitch: 60, track: 0 }, { pitch: 64, track: 0 }, { pitch: 90, track: 1 }],
  };
  const low = fitLowPitch(engine, 24, 0, false, 48);
  assert.ok(low <= 60 && low + 24 > 64, `track 0 fits in the window: ${low}`);
  const all = fitLowPitch(engine, 48, 0, true, 48);
  assert.ok(all <= 60 && all + 48 > 90, `all tracks fit: ${all}`);
  // An empty track is not a reason to jump somewhere arbitrary.
  assert.equal(fitLowPitch(engine, 24, 7, false, 48), 48);
  assert.equal(fitLowPitch(null, 24, 0, false, 36), 36);
  // Never a window that runs off the end of MIDI.
  const high = fitLowPitch({ noteCount: 1, notes: [{ pitch: 127, track: 0 }] }, 24, 0, false, 48);
  assert.ok(high + 24 <= 128, `stays inside MIDI: ${high}`);
});

test('patcher config is named per type, or shown as nothing', () => {
  const euclid = describeConfig(NODE_TYPES.indexOf('euclidean'),
                                [16, 5, 0, 1, 0, 100, 4, 0]);
  assert.match(euclid, /steps 16/);
  // duration_ticks 0 is "use the default", not a zero-length note — the Rust
  // processor substitutes half a step. Rendering it as 0.00b was a confident
  // wrong number about something audible.
  assert.match(euclid, /dur auto/);
  assert.match(describeConfig(NODE_TYPES.indexOf('euclidean'),
                              [16, 5, 0, 1, 0, 100, 4, 960000]), /dur 1\.00b/);
  assert.match(euclid, /hits 5/);
  // Milli-units divided rather than shown raw.
  const lfo = describeConfig(NODE_TYPES.indexOf('lfo'), [2500, 750, 0, 0, 0, 0, 0, 0]);
  assert.match(lfo, /freq 2\.50Hz/);
  assert.match(lfo, /depth 0\.75/);
  // A type with no table shows NOTHING, not eight anonymous numbers.
  assert.equal(describeConfig(NODE_TYPES.indexOf('kernel'), [1, 2, 3, 4, 5, 6, 7, 8]), '');
});

test('a velocity is two hex digits, over the whole range', () => {
  // It was the last two DECIMAL digits, so 100 read as "00" and 127 as "27" —
  // wrong, and wrong in the way that does not announce itself, because both are
  // valid-looking velocities.
  assert.equal(velocityText(0), '00');
  assert.equal(velocityText(12), '0c');
  assert.equal(velocityText(100), '64');
  assert.equal(velocityText(127), '7f');
  // Clamped rather than allowed to produce three characters, which would widen
  // the column the grid is measured against.
  assert.equal(velocityText(999), 'ff');
  assert.equal(velocityText(-5), '00');
});

test('a dragged loop snaps, normalises and is never empty', () => {
  const BAR = TICKS_PER_BAR;
  // Forwards, snapped to the nearest bar at each end.
  assert.deepEqual(snapLoop(BAR * 0.6, BAR * 3.4, false), { start: BAR, end: BAR * 3 });
  // Backwards: a drag right-to-left is the same span, not an empty one.
  assert.deepEqual(snapLoop(BAR * 3.4, BAR * 0.6, false), { start: BAR, end: BAR * 3 });
  // A click is one bar, not zero. The engine refuses end <= start, and a ruler
  // that silently ignored a click would look broken.
  assert.deepEqual(snapLoop(BAR * 2.1, BAR * 2.1, false), { start: BAR * 2, end: BAR * 3 });
  // Fine mode is beats.
  assert.deepEqual(snapLoop(0, BAR / 4 * 1.4, true), { start: 0, end: BAR / 4 });
  // Never negative, however far left the drag goes.
  assert.equal(snapLoop(-BAR * 5, BAR, false).start, 0);
});

test('a dragged clip moves without resizing, and stops at the wall', () => {
  const BAR = TICKS_PER_BAR;
  const clip = { startTick: BAR * 4, endTick: BAR * 6, track: 1 };   // two bars long
  const opts = { laneCount: 4 };

  // An ordinary move: snapped, same length, same lane.
  const r = dragPlacement(clip, 'move', BAR * 2.4, 0, opts);
  assert.deepEqual([r.startTick, r.endTick, r.track], [BAR * 6, BAR * 8, 1]);
  assert.equal(r.changed, true);

  // THE ONE THAT MATTERS. Dragged off the left edge, the clip stops at zero and
  // KEEPS ITS LENGTH. Clamping only the start would leave the end where the
  // pointer put it — a move that silently shortens the clip, which the user
  // would not discover until it played.
  const wall = dragPlacement(clip, 'move', -BAR * 99, 0, opts);
  assert.deepEqual([wall.startTick, wall.endTick], [0, BAR * 2]);

  // Lanes clamp to what exists, in both directions.
  assert.equal(dragPlacement(clip, 'move', 0, +99, opts).track, 3);
  assert.equal(dragPlacement(clip, 'move', 0, -99, opts).track, 0);

  // A gesture that ends where it began sends NOTHING. This is a click, which is
  // the commonest gesture there is; turning it into a command would dirty the
  // project and cost an undo step for touching a clip.
  assert.equal(dragPlacement(clip, 'move', 0, 0, opts).changed, false);
  // And so does a wobble smaller than the snap unit.
  assert.equal(dragPlacement(clip, 'move', BAR * 0.1, 0, opts).changed, false);
});

test('a trimmed clip cannot be pulled through its own other edge', () => {
  const BAR = TICKS_PER_BAR;
  const clip = { startTick: BAR * 4, endTick: BAR * 6, track: 0 };
  const opts = { laneCount: 4 };

  // Left handle: the start moves, the end does not.
  const l = dragPlacement(clip, 'trim-l', -BAR * 2, 0, opts);
  assert.deepEqual([l.startTick, l.endTick], [BAR * 2, BAR * 6]);
  // Right handle: the end moves, the start does not.
  const r = dragPlacement(clip, 'trim-r', BAR * 3, 0, opts);
  assert.deepEqual([r.startTick, r.endTick], [BAR * 4, BAR * 9]);

  // Dragged past the far edge, each stops one unit short of it. A negative
  // length is the dangerous case: it reaches the engine as an enormous UNSIGNED
  // one, so the clip does not vanish — it swallows the song.
  const crossed = dragPlacement(clip, 'trim-l', BAR * 99, 0, opts);
  assert.deepEqual([crossed.startTick, crossed.endTick], [BAR * 5, BAR * 6]);
  assert.ok(crossed.endTick > crossed.startTick);
  const crossedR = dragPlacement(clip, 'trim-r', -BAR * 99, 0, opts);
  assert.deepEqual([crossedR.startTick, crossedR.endTick], [BAR * 4, BAR * 5]);

  // Fine mode trims by the beat, so the minimum is a beat rather than a bar.
  const fine = dragPlacement(clip, 'trim-l', BAR * 99, 0, { ...opts, fine: true });
  assert.equal(fine.startTick, BAR * 6 - BAR / 4);
  // A trim never changes lane, however far the pointer wandered vertically.
  assert.equal(dragPlacement(clip, 'trim-r', BAR, 3, opts).track, 0);
  // Nor does it ever go negative at the left wall.
  assert.equal(dragPlacement({ startTick: 0, endTick: BAR, track: 0 },
                             'trim-l', -BAR * 9, 0, opts).startTick, 0);
});

test('a clip narrow enough to be all handle is all body instead', () => {
  // Handles are PIXELS, because they are a target for a pointer and a pointer
  // does not get more precise when the clip is longer.
  assert.equal(clipZoneAt(2, 200), 'trim-l');
  assert.equal(clipZoneAt(100, 200), 'move');
  assert.equal(clipZoneAt(197, 200), 'trim-r');
  // Exactly on the boundary is the body: a handle is what you must reach INTO.
  assert.equal(clipZoneAt(6, 200), 'move');
  assert.equal(clipZoneAt(194, 200), 'move');
  // The model draws clips down to 2px. Under three handles' worth there is no
  // room for a body, and a clip that can only be trimmed and never moved is
  // worse than one that cannot be trimmed.
  assert.equal(clipZoneAt(0, 10), 'move');
  assert.equal(clipZoneAt(9, 10), 'move');
});

test('the trim handle the cursor shows is the one the drag begins in', async () => {
  // The stylesheet cannot import the constant, so the two are checked against
  // each other here. A cursor that appears somewhere other than where the drag
  // starts is the worst version of this bug: it looks right, and does the wrong
  // thing on the few pixels either side of the seam — which is exactly where
  // people aim, because that is where the cursor changed.
  const { readFileSync } = await import('node:fs');
  const css = readFileSync(new URL('../src/arrange.css', import.meta.url), 'utf8');
  const rule = css.match(/\.ar-clip::before,\s*\.ar-clip::after\s*\{[^}]*\}/);
  assert.ok(rule, 'the handle rule is gone — did the selector change?');
  const width = rule[0].match(/width:\s*(\d+)px/);
  assert.ok(width, 'the handle rule has no width');
  assert.equal(Number(width[1]), CLIP_HANDLE_PX,
    'arrange.css and clipZoneAt disagree about how wide a trim handle is');
  assert.ok(/cursor:\s*col-resize/.test(rule[0]), 'a handle with no cursor is invisible');
});

test('dragging a node parameter accumulates rather than rounding to nothing', () => {
  // The bug this shape exists to avoid: differencing consecutive pointer events
  // and truncating each one. A slow drag arrives as many small moves, every one
  // of which truncates to zero steps, so the parameter never moves at all and
  // the control reads as broken.
  let applied = 0;
  for (const dy of [-2, -4, -6, -8, -10]) {          // five 2px moves upward
    const r = dragSteps(dy, applied);
    applied = r.total;
  }
  assert.equal(applied, 1, 'ten pixels of travel is one step, not five zeros');

  // Up is positive. Screen y grows downward, and a parameter that falls when you
  // drag up is the fastest way to make a control feel wrong.
  assert.ok(dragSteps(-60, 0).total > 0, 'dragging up raises the value');
  assert.ok(dragSteps(60, 0).total < 0, 'dragging down lowers it');

  // The delta is what to apply NOW, so re-reporting the same position asks for
  // nothing further.
  const a = dragSteps(-60, 0);
  assert.equal(dragSteps(-60, a.total).delta, 0, 'a pointer that stopped asks for no more');

  // Fine is slower, not inverted or scaled the wrong way.
  assert.ok(Math.abs(dragSteps(-60, 0, true).total) < Math.abs(dragSteps(-60, 0).total),
            'shift makes the same travel do less');
});

test('the chain says what is flowing between its devices', async () => {
  const { resolveFlow } = await import('../src/chainmodel.js');
  const shape = (devs) => resolveFlow(devs)
    .map((g) => (g.midi ? 'M' : '-') + (g.audio ? 'A' : '-')).join(' ');

  // The clip feeds the head, so MIDI is present before anything.
  assert.equal(shape([]), 'M-');

  // An instrument CONVERTS: MIDI in, audio out. This is the one the picture
  // exists to show, and the one a hand-written "audio after the first device"
  // rule would get right by accident and wrong everywhere else.
  assert.equal(shape([{ caps: 5 }]), 'M- -A');

  // An event patcher passes MIDI along and makes no audio.
  assert.equal(shape([{ caps: 3 }, { caps: 5 }]), 'M- M- -A');

  // An audio effect after an instrument keeps audio; MIDI has already stopped.
  assert.equal(shape([{ caps: 5 }, { caps: 4 }]), 'M- -A -A');

  // A BYPASSED device is a wire, not a device that does nothing: its conversion
  // does not happen either, so a bypassed instrument leaves MIDI as MIDI. Drawing
  // audio after it would be the exact confident lie this indicator must not tell.
  assert.equal(shape([{ caps: 5, bypass: true }]), 'M- M-');
  assert.equal(shape([{ caps: 5, bypass: true }, { caps: 5 }]), 'M- M- -A');

  /*
   * DEAD: a device that swallows MIDI and emits nothing leaves everything after
   * it with nothing to work on. Reported rather than drawn as a blank, because
   * "nothing downstream of here can ever do anything" is the most useful thing
   * this can say and the hardest to work out by ear.
   *
   * My first attempt at this case was `[instrument, midi-sink]` and it was
   * wrong: audio keeps flowing past a device that only consumes MIDI, which is
   * correct and is exactly the sort of thing a carry-forward model gets right
   * and a rule of thumb gets wrong.
   */
  const dead = resolveFlow([{ caps: 1 }]);
  assert.equal(dead[0].dead, false, 'the clip feeds the head');
  assert.equal(dead[1].dead, true, 'and nothing comes out the other side');
  // Audio survives a MIDI-only device, so this chain has no dead gap at all.
  assert.deepEqual(resolveFlow([{ caps: 5 }, { caps: 1 }]).map((g) => g.dead),
                   [false, false, false]);

  // Reuses the caller's array: the rack redraws at frame rate and this must not
  // allocate a new object per gap per frame.
  const buf = [];
  const a = resolveFlow([{ caps: 5 }], buf);
  const b = resolveFlow([{ caps: 5 }], buf);
  assert.equal(a, b, 'the same array comes back');
  assert.equal(a[0], b[0], 'and the same gap objects inside it');
  // And it shrinks when the chain does, rather than leaving a stale tail.
  assert.equal(resolveFlow([], buf).length, 1);
});

test('one device\'s patcher graph can be picked out of the pooled one', async () => {
  const { subgraphFrom } = await import('../src/patchermodel.js');
  /*
   * A BITMASK, not a Set. `Set.prototype.clear()` allocates a fresh backing
   * table in V8 and this runs whenever the selected device changes — every frame
   * you hold an arrow key on the rack — which measured 844 B/draw on the chain
   * scene alone. `ids` reads a mask back as the list these assertions are about.
   */
  const ids = (mask) => [...mask].map((v, i) => (v ? i : -1)).filter((i) => i >= 0);
  /*
   * The engine publishes every device's nodes in ONE array with disjoint id
   * blocks, naming only each device's own output node. Two generator tracks look
   * like this — and rendering the pool unfiltered is what showed track 1's
   * euclidean while you stood on track 2, let you edit it thinking it was track
   * 2's, and reported a generator on a track with no devices at all.
   */
  const nodes = [{ id: 0 }, { id: 1 }, { id: 2 }, { id: 3 }, { id: 4 }];
  const edges = [{ src: 0, dst: 1 }, { src: 1, dst: 2 },      // device A -> out 2
                 { src: 3, dst: 4 }];                          // device B -> out 4

  assert.deepEqual(ids(subgraphFrom(nodes, edges, 2, null)), [0, 1, 2]);
  assert.deepEqual(ids(subgraphFrom(nodes, edges, 4, null)), [3, 4]);

  /*
   * A ROOT THAT NAMES NOTHING YIELDS NOTHING. "I do not know which nodes are
   * yours" must never render as "all of them" — that is the bug, and a
   * convenient fallback to the whole pool would reintroduce it exactly.
   */
  assert.deepEqual(ids(subgraphFrom(nodes, edges, 99, null)), []);
  assert.deepEqual(ids(subgraphFrom(nodes, edges, -1, null)), []);
  assert.deepEqual(ids(subgraphFrom(nodes, edges, undefined, null)), []);

  // A branch feeding the output is part of the device: two sources into one node.
  assert.deepEqual(ids(subgraphFrom(nodes,
    [{ src: 0, dst: 2 }, { src: 1, dst: 2 }], 2, null)), [0, 1, 2]);

  // A CYCLE must terminate. The engine refuses to build one, but this side reads
  // whatever arrives and a graph walk that hangs takes the whole page with it.
  assert.deepEqual(ids(subgraphFrom(nodes,
    [{ src: 0, dst: 1 }, { src: 1, dst: 0 }, { src: 1, dst: 2 }], 2, null)), [0, 1, 2]);

  // REUSES the caller's mask, because this runs per frame — and clears it, so a
  // second call cannot leave the first call's nodes on screen.
  const reuse = subgraphFrom(nodes, edges, 2, null);
  assert.equal(subgraphFrom(nodes, edges, 4, reuse), reuse, 'the same array comes back');
  assert.deepEqual(ids(reuse), [3, 4], 'holding only the second walk');
  // Grown rather than overflowed when the pool's ids outrun it.
  const wide = subgraphFrom([{ id: 300 }], [], 300, new Uint8Array(4));
  assert.ok(wide.length > 300 && wide[300] === 1, 'a mask too small is replaced');
});

test('a chord is named by its degree, not by a pitch set it does not contain', async () => {
  const { nameChord } = await import('../src/harmonymodel.js');
  // Quality is the engine's own vocabulary: 0 the degree alone, 1 a triad, 2 a
  // seventh (apps/chord_resolver.cpp).
  assert.equal(nameChord(0, 1, 0), 'I');
  assert.equal(nameChord(3, 1, 0), 'IV');
  assert.equal(nameChord(4, 2, 0), 'V7');
  // A single note carries no chord marker — it is not a chord.
  assert.equal(nameChord(1, 0, 0), 'II');
  // Inversions show, root position does not: "/0" on nearly every chord is noise.
  assert.equal(nameChord(0, 2, 1), 'I7/1');
  assert.equal(nameChord(0, 1, 2), 'I/2');

  /*
   * A degree past the seventh is SHOWN, not clamped. The scale may not have it
   * — that is a real thing to see, and drawing it as the seventh would hide a
   * chord pointing somewhere the key cannot reach.
   */
  assert.equal(nameChord(7, 1, 0), 'd8');

  /*
   * THE DEMO'S OWN EXAMPLE, end to end through the encodings.
   *
   * Typing `@3^7` writes degree 2 quality 2: degrees are 1-based as musicians write them and
   * stored 0-based, and quality is an enum where 2 is a seventh. The numeral table is indexed by
   * the STORED degree, so the two conventions cancel and `@3` reads III — not II.
   *
   * Worth pinning as the token rather than as bare numbers, because that cancellation is exactly
   * the kind of thing an "obvious" off-by-one fix would break in the direction of looking right.
   */
  assert.equal(nameChord(2, 2, 0), 'III7', '`@3^7` is a seventh on the THIRD degree');

  // Interned, because a tracker row redraws at frame rate.
  assert.equal(nameChord(0, 1, 0), nameChord(0, 1, 0));
});

test('a nudge clamps, and leaves every other value exactly as it was read', () => {
  const EU = NODE_TYPES.indexOf('euclidean');
  assert.deepEqual(configFields(EU).map((f) => f.name),
                   ['steps', 'hits', 'offset', 'degree', 'oct', 'vel', 'base', 'dur']);
  // Every published value goes back, edited or not: the engine rebuilds the whole
  // config from what it receives, so a field this UI does not name is a field it
  // would otherwise zero.
  assert.deepEqual(nudgeConfig(EU, [16, 5, 0, 1, 0, 100, 4, 960000], 1, 1),
                   [16, 6, 0, 1, 0, 100, 4, 960000]);
  // Clamped at the bottom rather than wrapping to something absurd. A Euclidean
  // pattern of zero steps is a division by zero in the engine's generator.
  assert.deepEqual(nudgeConfig(EU, [1, 0, 0, 0, 0, 1, 0, 0], 0, -1)[0], 1);
  // Signed fields keep going below zero.
  assert.equal(nudgeConfig(EU, [16, 5, 0, 1, 0, 100, 4, 0], 4, -1)[4], -1);
  // A type with no layout refuses rather than sending a config of zeros — the
  // same refusal the sidecar makes, so the UI never asks for it.
  assert.equal(nudgeConfig(NODE_TYPES.indexOf('passthru'), [0, 0, 0, 0, 0, 0, 0, 0], 0, 1), null);
  // And a field index past the end of the type's table.
  assert.equal(nudgeConfig(NODE_TYPES.indexOf('random'), [0, 0, 0, 0, 0, 0, 0, 0], 5, 1), null);
});

test('trackName respects the engine, and falls back visibly', () => {
  assert.equal(trackName({ names: ['Bass', 'Pad'] }, 0), 'Bass');
  assert.equal(trackName({ names: ['Bass', 'Pad'] }, 5), 'T06');
  assert.equal(trackName({ names: [] }, 0), 'T01');
  assert.equal(trackName(null, 2), 'T03');
  // An empty string means the engine has not spoken, not that it is unnamed.
  assert.equal(trackName({ names: ['', 'Pad'] }, 0), 'T01');
});

test('a seeded text field is replaced by the first keystroke', () => {
  // The bug this module exists to stop: seeding "Bass" and typing "Big Bass"
  // gave "BassBig Bass", and seeding "foo" and saving gave "foofoo". Twice, in
  // two fields written separately.
  const f = createField({ max: 16 });
  fBegin(f, 'Bass');
  assert.equal(f.text, 'Bass', 'the seed shows');
  fFeed(f, 'L');
  assert.equal(f.text, 'L', 'and the first character replaces it');
  fFeed(f, 'o'); fFeed(f, 'w');
  assert.equal(f.text, 'Low');
});

test('an unseeded field appends from the start', () => {
  const f = createField();
  fBegin(f, '');
  for (const c of 'abc') fFeed(f, c);
  assert.equal(f.text, 'abc');
});

test('backspace edits the seed rather than replacing it', () => {
  const f = createField();
  fBegin(f, 'webtest');
  fFeed(f, 'Backspace');
  assert.equal(f.text, 'webtes', 'backspace is an edit, so the seed stays');
  fFeed(f, 'X');
  assert.equal(f.text, 'webtesX', 'and it is no longer fresh');
});

test('a field refuses to commit empty unless it is allowed to', () => {
  const name = createField();
  fBegin(name, '');
  assert.equal(fFeed(name, 'Enter'), 'consumed', 'a name must not be empty');
  const cell = createField({ commitEmpty: true });
  fBegin(cell, '');
  assert.equal(fFeed(cell, 'Enter'), 'commit', 'an empty cell commit CLEARS the cell');
});

test('cancelWhenEmpty gives the keyboard back', () => {
  const f = createField({ cancelWhenEmpty: true });
  fBegin(f, '');
  fFeed(f, '@');
  assert.equal(fFeed(f, 'Backspace'), 'cancel', 'backspacing past the opener closes it');
  assert.equal(f.active, false);
  const g = createField();
  fBegin(g, '');
  fFeed(g, 'a');
  assert.equal(fFeed(g, 'Backspace'), 'consumed', 'others just empty out');
  assert.equal(g.active, true);
});

test('the charset is enforced and out-of-set keys do not fall through', () => {
  const f = createField({ charset: /[A-Za-z0-9._-]/, max: 4 });
  fBegin(f, '');
  for (const c of 'a/b!c') fFeed(f, c);
  assert.equal(f.text, 'abc', 'slash and bang refused');
  // Refused, not ignored: a key that reached a field must not also reach the
  // shortcut behind it.
  assert.equal(fFeed(f, '/'), 'consumed');
  fFeed(f, 'd'); fFeed(f, 'e');
  assert.equal(f.text, 'abcd', 'capped at max');
});

// --- the song meter ------------------------------------------------------
// 4/4 cannot distinguish a correct implementation from the four hardcoded 3840000s
// it replaces, so every check here is in a meter that is not 4/4.
test('a beat is the denominator unit, not always a quarter', () => {
  assert.equal(ticksPerBeat({ numerator: 4, denominator: 4 }), 960000, 'a quarter');
  assert.equal(ticksPerBeat({ numerator: 6, denominator: 8 }), 480000, 'an eighth');
  assert.equal(ticksPerBeat({ numerator: 5, denominator: 2 }), 1920000, 'a half');
  // The mistake this catches: treating the denominator as decoration and calling a
  // beat a quarter always, which draws 6/8 as 6/4 — twice as long, and plausible.
  assert.notEqual(ticksPerBeat({ numerator: 6, denominator: 8 }),
                  ticksPerBeat({ numerator: 6, denominator: 4 }));
});

test('a bar is numerator beats, and 4/4 still lands on the old constant', () => {
  assert.equal(ticksPerBar({ numerator: 4, denominator: 4 }), 3840000,
               'the literal this replaced, so no golden moves');
  assert.equal(ticksPerBar({ numerator: 7, denominator: 8 }), 3360000);
  assert.equal(ticksPerBar({ numerator: 3, denominator: 4 }), 2880000);
});

test('a tick knows which bar and beat it is in, in any meter', () => {
  const p = createPosition();
  const m78 = { numerator: 7, denominator: 8 };
  const bar = ticksPerBar(m78);          // 3360000
  const beat = ticksPerBeat(m78);        // 480000

  positionOf(0, m78, beat, p);
  assert.deepEqual([p.bar, p.beat, p.sub, p.onBar, p.onBeat], [1, 1, 0, true, true]);

  // Last beat of bar 1: seven beats to a bar, so beat 7 exists and beat 8 does not.
  positionOf(6 * beat, m78, beat, p);
  assert.deepEqual([p.bar, p.beat, p.onBar], [1, 7, false]);

  // ...and the next beat is bar 2 beat 1. Under the old TICKS_PER_BAR / 4 rule this
  // was bar 1 beat 8, which is a bar that does not exist in 7/8.
  positionOf(7 * beat, m78, beat, p);
  assert.deepEqual([p.bar, p.beat, p.onBar, p.onBeat], [2, 1, true, true]);
  assert.equal(7 * beat, bar, 'seven beats IS the bar');

  // sub is a row offset inside the beat and depends on the display grid, not the
  // meter — the same tick is a different sub at a different zoom.
  positionOf(beat + beat / 4, m78, beat / 4, p);
  assert.equal(p.sub, 1);
  positionOf(beat + beat / 4, m78, beat / 8, p);
  assert.equal(p.sub, 2);
});

test('meters compare by value, and their text is interned', () => {
  // The engine will republish a meter every frame. A guard on object identity
  // would rebuild every bar label sixty times a second while looking correct.
  assert.ok(sameMeter({ numerator: 5, denominator: 8 }, { numerator: 5, denominator: 8 }));
  assert.ok(!sameMeter({ numerator: 5, denominator: 8 }, { numerator: 5, denominator: 4 }));
  assert.ok(!sameMeter(null, { numerator: 4, denominator: 4 }));
  assert.equal(meterText({ numerator: 7, denominator: 8 }), '7/8');
  assert.equal(meterText({ numerator: 7, denominator: 8 }),
               meterText({ numerator: 7, denominator: 8 }),
               'same string object, so a nodeValue compare is a pointer compare');
});

// --- the device rack -------------------------------------------------------
// The rack draws a card per device and a row per parameter, and Zebra2 has 256
// of them. Two things are worth testing without a browser: that the model
// carries all of them without building a string per row per frame, and that an
// edit in flight settles or expires rather than being adopted on faith.

test('the model carries every parameter the engine published, not the first five', () => {
  const params = [];
  for (let k = 0; k < 300; k++) {
    params.push({ index: k, value: k / 1000, name: 'p' + k, display: k + ' Hz', uid: 'ab' });
  }
  const chains = { 0: { track: 0, version: 4, devices: [
    { id: 7, kind: 4, pos: 0, node: 0xffffffff, slot: 2, caps: 4, bypass: 0 }] } };
  const buf = createChainBuffer();
  const vm = buildChainModel(
    { track: 0, chains, selected: 0, trackName: 'Bass', params: { 7: { name: 'Zebra2', params } } },
    buf);
  assert.equal(vm.cards[0].paramCount, MAX_PARAMS, 'capped by the engine region, not by the card');
  assert.equal(vm.cards[0].params[255].name, 'p255', 'and the last one is really there');
  // The engine's region holds 256, so 300 means 44 were dropped before we ever
  // saw them. Saying so is the difference between complete and looking it.
  assert.equal(vm.cards[0].more, '+44 more');
  assert.match(vm.cards[0].sub, /256 params · \+44 more/);
});

test('rebuilding the same rack allocates no new parameter slots or strings', () => {
  const params = [{ index: 0, value: 0.5, name: 'cutoff', display: '620 Hz', uid: 'ab' },
                  { index: 1, value: 0.25, name: 'reso', display: '0.25', uid: 'cd' }];
  const chains = { 0: { track: 0, version: 4, devices: [
    { id: 7, kind: 4, pos: 0, node: 0xffffffff, slot: 2, caps: 5, bypass: 0 }] } };
  const opts = { track: 0, chains, selected: 0, trackName: 'Bass',
                 params: { 7: { name: 'Zebra2', params } } };
  const buf = createChainBuffer();
  const a = buildChainModel(opts, buf);
  const slot0 = a.cards[0].params[0], title = a.cards[0].title;
  const sub = a.cards[0].sub, caps = a.cards[0].caps;
  const b = buildChainModel(opts, buf);
  // Identity, not equality: a new object or a new string here is an allocation
  // on every frame, which at eight cards of 256 is the whole GUIDELINES 3 budget.
  assert.equal(b.cards[0].params[0], slot0, 'the parameter slot is reused');
  assert.equal(b.cards[0].title === title, true, 'the title is not rebuilt');
  assert.equal(b.cards[0].sub === sub, true, 'nor the footer');
  assert.equal(b.cards[0].caps === caps, true, 'nor the capability phrase');
});

test('an edit settles when the engine agrees and expires when it never does', () => {
  const edits = createParamEdits();
  setParamEdit(edits, 7, 3, 750, 1000);
  assert.equal(edits.count, 1);
  assert.deepEqual([findParamEdit(edits, 7, 3).milli, findParamEdit(edits, 7, 4)], [750, null]);
  // Still inside the hold, engine still says something else: held, not dropped.
  assert.equal(reapParamEdits(edits, () => 500, 1500, null), 0);
  assert.equal(edits.count, 1, 'an engine that has not answered yet is not a refusal');
  // The engine answers with the value that was asked for.
  assert.equal(reapParamEdits(edits, () => 750, 1600, null), 0);
  assert.equal(edits.count, 0, 'an edit that arrived is dropped silently');

  // And one that is never answered.
  const s = setParamEdit(edits, 7, 3, 750, 1000);
  assert.equal(reapParamEdits(edits, () => 500, 1000 + EDIT_HOLD_MS + 1, s), 0);
  assert.equal(edits.count, 1, 'the one under the pointer never expires');
  assert.equal(reapParamEdits(edits, () => 500, 1000 + EDIT_HOLD_MS + 1, null), 1);
  assert.equal(edits.count, 0, 'and is reported so the strip can say so');

  // A parameter that is not on screen at all reads as -1, which is not agreement.
  setParamEdit(edits, 9, 1, 100, 0);
  assert.equal(reapParamEdits(edits, () => -1, 10, null), 0);
  assert.equal(edits.count, 1);
});

test('edits are pooled, and the pool is a pool rather than a leak', () => {
  const edits = createParamEdits(2);
  const a = setParamEdit(edits, 1, 0, 10, 0);
  const b = setParamEdit(edits, 1, 1, 20, 0);
  assert.equal(setParamEdit(edits, 1, 2, 30, 0), null, 'a full store refuses rather than grows');
  // Moving an existing one is not a new one.
  assert.equal(setParamEdit(edits, 1, 0, 40, 5), a);
  assert.deepEqual([a.milli, a.at, edits.count], [40, 5, 2]);
  dropParamEdit(edits, a);
  assert.equal(edits.count, 1);
  assert.equal(findParamEdit(edits, 1, 1), b, 'dropping one does not lose the other');
  assert.equal(edits.slots.length, 2, 'and the slots are still there to be reused');
});

// --- the browser rail's row model ------------------------------------------
// The rail is heterogeneous now: a row is a project or a plugin, and they differ
// in badge, meta line, search text and what opening one does. The interesting
// part is DOM-free — every string a row shows is built when the feed arrives,
// which is the property alloc.mjs measures from the other end and the one thing
// a renderer cannot be trusted to keep on its own.

test('a plugin row carries the catalogue’s own words, and says which kind it is', () => {
  const inst = setPluginRow(makeRow(), {
    name: 'Zebra2', vendor: 'u-he', format: 'VST3', is_instrument: true, ok: true,
    error: '', path: '/Library/Audio/Plug-Ins/VST3/u-he/Zebra2.vst3', uid: 'VST3-Zebra2-aa-bb' });
  assert.equal(inst.kind, KIND_PLUGIN);
  assert.equal(inst.badge, 'PLUG');
  assert.equal(inst.name, 'Zebra2');
  assert.equal(inst.meta, 'u-he · VST3', 'vendor and format, in the rail’s separator');
  assert.equal(inst.mark, 'INST', 'the type survives the meta line being truncated');
  // The durable identity stays reachable: it is what an insert will need, and
  // the row is the only thing the click handler is handed.
  assert.equal(inst.plugin.uid, 'VST3-Zebra2-aa-bb');

  const fx = setPluginRow(makeRow(), {
    name: 'Analog Heat', vendor: 'Elektron Music Machines', format: 'VST3',
    is_instrument: false, ok: true, error: '' });
  assert.equal(fx.mark, 'FX');
  assert.equal(fx.meta, 'Elektron Music Machines · VST3');
});

test('a failed scan keeps its row and wears the reason', () => {
  // "why is Zebra not in the list" has to be answerable by the list.
  const bad = setPluginRow(makeRow(), {
    name: 'Sylenth1', vendor: 'LennarDigital', format: 'VST3', is_instrument: true,
    ok: false, error: 'the plugin crashed while being scanned' });
  assert.equal(bad.ok, false);
  assert.equal(bad.meta, 'the plugin crashed while being scanned',
               'the scanner’s reason, not a vendor line for something unusable');
  assert.equal(bad.mark, '!');
  // And one whose scanner said nothing at all still says something.
  const mute = setPluginRow(makeRow(), { name: 'X', ok: false, error: '' });
  assert.match(mute.meta, /scan failed/);
});

test('search text names the vendor, the format and the kind', () => {
  const row = setPluginRow(makeRow(), {
    name: 'Diva', vendor: 'u-he', format: 'VST3', is_instrument: true, ok: true, error: '' });
  for (const q of ['diva', 'u-he', 'vst3', 'instrument']) {
    assert.ok(row.lower.indexOf(q) >= 0, `"${q}" narrows to it`);
  }
  assert.equal(row.lower.indexOf('effect'), -1, 'and does not match the other kind');
});

test('a project row leaves its meta line to the renderer', () => {
  const rows = [makeRow(), makeRow()];
  fillRows(rows, ['newest', 'older'], setProjectRow);
  assert.equal(rows[0].kind, KIND_PROJECT);
  assert.equal(rows[0].badge, 'PROJ');
  // Which project is LOADED changes without the list changing, so the line is
  // chosen from interned constants at render time rather than built here. The
  // one fact this side does know is the sidecar's newest-first ordering.
  assert.deepEqual([rows[0].recent, rows[1].recent], [true, false]);
  assert.equal(rows[0].meta, '');
  assert.equal(rows[0].lower, 'newest');
});

// --- the command grammar ---------------------------------------------------
// Every command states its arguments twice: as prose in `help`, which is what a
// person and a model read, and as `args`, which is what the gate reads. The
// first test below is the one that matters — it is what stops those two from
// becoming two descriptions of the same thing with nothing forcing them to
// agree, which is the bug this project keeps having (GUIDELINES 2.1). It
// already found one: `select <row0> <row1>` documented a required second row
// and accepted a missing one.

/** Every api method the grammar reaches, recording what it was handed. */
/**
 * Every method name the host promises the console.
 *
 * Kept as a list so the test below can be exact about which one is missing —
 * `add-track`, `remove-track` and `columns` all shipped calling functions that
 * existed on __uni and NOT on the object the dock is handed, so each threw "is
 * not a function" the moment anyone typed it. The op-registry test did not
 * notice, because it checks that a command is DECLARED, not that it can run.
 */
const API_METHODS = ['automationEdit', 'automationEditing', 'samplerKit', 'samplerKitCached',
                     'rowOps', 'opsAtCursor', 'opAtCursor', 'opsTextAtCursor', 'noteIdAtCursor',
                     'loadSample', 'addDevice', 'sliceSample', 'samplerFilter', 'samplerEnvelope', 'samplerSlot', 'samplerDevice', 'samplerEmit', 'soundAddressed',
                     'setView', 'load', 'save', 'listProjects', 'transport', 'tempo',
                     'note', 'del', 'goto', 'zoom', 'octave', 'gain', 'strip', 'state',
                     'seek',
                     'engine', 'close', 'follow', 'rename', 'select', 'transpose', 'setLoop',
                     'nodes', 'addNode', 'delNode', 'linkNodes', 'patch', 'copy', 'paste',
                     'cut', 'addTrack', 'removeTrack', 'noteColumns', 'delDevice', 'bypass',
                     'quantize', 'moveDevice', 'chord', 'delChord', 'deleteHarmony',
                     'addDevice', 'openEditor', 'newSong', 'fold', 'opsColumn', 'opsShown',
                     'harmonyQuantize', 'harmonyQuantized', 'savePatch', 'linesPerBeat',
                     'clipGrid', 'hasMaster', 'audioClip', 'velocityEdit', 'delAutomationPoint',
                     'allowOverlap', 'overlapping',
                     'samplerSlotName',
                     'edit', 'harmony', 'ask', 'forget',
                     'clips', 'moveClip', 'trimClip', 'delClip', 'addClip',
                     // The cell inspector: what it is showing, and pointing at a cell.
                     'inspect', 'hover',
                     'selectedClip', 'ticksPerBar', 'master',
                     // The spine. Six, because a section has six things you can do to it
                     // and every one is reachable from both surfaces — the strip's click,
                     // drag and double-click all come through these same methods.
                     'shared', 'fork', 'swapClip', 'keepClip',
                     'markers', 'addMarker', 'delMarker', 'nameMarker', 'moveMarker',
                     'colorMarker', 'clipText', 'samplerEnvelopeShape',
                     // Which graph a patcher edit lands in — pool, or a device's own.
                     'patchTarget',
                     'insertTime', 'setTimeSig',
                     // Modulation. `mapParam` takes a parameter INDEX and resolves the
                     // uid16 itself — the console should not have to type a 32-character
                     // hex string to map a knob.
                     'mods', 'mapParam', 'unmapParam', 'modDepth', 'macro',
                     'automation', 'automationPoints', 'writeAutomation'];

function stubApi() {
  const calls = [];
  const api = { calls };
  for (const k of API_METHODS) {
    api[k] = (...args) => { calls.push(k + '(' + args.join(',') + ')'); return 0; };
  }
  return api;
}

/** A plausible value for one argument of a command's schema. */
function sampleArg(a) {
  if (a.type === 'enum') return a.values[0];
  if (a.type === 'text') return 'x';
  const lo = a.min !== undefined ? a.min : 1;
  const hi = a.max !== undefined ? a.max : lo + 1;
  return String(Math.min(hi, Math.max(lo, 1)));
}

test('every console command can actually run', () => {
  // Calls each one with arguments its own schema accepts, against an api that
  // has every method the host promises. A command that reaches for something
  // else throws TypeError here instead of the first time a person types it.
  //
  // This is the check that `add-track`, `remove-track` and `columns` needed and
  // did not have: all three were declared, listed in the op registry, covered by
  // the CLI-parity ratchet — and dead.
  const api = stubApi();
  const cmds = createCommands(api);
  const broken = [];
  for (const [name, cmd] of Object.entries(cmds)) {
    const args = (cmd.args || []).filter((a) => !a.optional).map(sampleArg);
    try {
      // The HOST object, which is not the api: `clear` calls x.clear() on it.
      cmd.run(args, { print: () => {}, clear: () => {}, api });
    } catch (e) {
      // A command may legitimately refuse these arguments; only a missing method
      // is a defect here.
      if (/is not a function|undefined is not|Cannot read propert/.test(String(e.message))) {
        broken.push(`${name}: ${e.message}`);
      }
    }
  }
  assert.deepEqual(broken, [], `console commands calling nothing:\n  ${broken.join('\n  ')}`);
});

test('the ? overlay records no limitations, because it is not reviewed', async () => {
  /*
   * THE OVERLAY IS A KEY REFERENCE, NOT A PLACE TO RECORD WHAT IS MISSING.
   *
   * Three of its lines were stale at once, and all three were stale in the same way —
   * each recorded an absence that had since been built:
   *
   *   "clip edits — not implemented, needs engine commands"  (they all work)
   *   "one global graph; the engine does not run per-device graphs yet"  (it does)
   *   "** = notes a cell cannot show apart"  (the cell draws "4x C-4" now)
   *
   * The manual had all three written down as known-stale, which is the tell: a claim
   * nobody re-checks is indistinguishable from a real limitation, and the overlay is
   * hand-maintained against a handler that moves under it.
   *
   * So limitations live in docs/MANUAL.md, which gets read end to end, and this table
   * says only what a key does. A line that needs a caveat is a line whose key needs
   * fixing.
   */
  const { GLOBAL_KEYS, SURFACE_KEYS } = await import('../src/help.js');
  const CLAIMS = /not implemented|does not .* yet|\byet\b|cannot|can't|no command|needs engine|unimplemented|coming soon|todo/i;
  const bad = [];
  const scan = (where, rows) => {
    for (const [key, text] of rows) if (CLAIMS.test(text)) bad.push(`${where} "${key}": ${text}`);
  };
  scan('global', GLOBAL_KEYS);
  for (const [view, def] of Object.entries(SURFACE_KEYS)) scan(view, def.keys);
  assert.deepEqual(bad, [],
    'the ? overlay states a limitation — put it in docs/MANUAL.md, which is reviewed');
});

test('the manual states the real number of commands', async () => {
  /*
   * A COUNT IN PROSE IS A FACT THAT ROTS SILENTLY. The manual said "Ninety-one
   * commands" while there were ninety-seven — stale by five before anything was added
   * to it, and nothing could have noticed, because a wrong number reads exactly like a
   * right one. The same defect as a recorded limitation nobody re-checks.
   *
   * So the number is checked against the command table rather than trusted. If this
   * fails, the manual is wrong: update the sentence, don't relax the check.
   */
  const { readFileSync } = await import('node:fs');
  const md = readFileSync(new URL('../../docs/MANUAL.md', import.meta.url), 'utf8');
  const m = md.match(/^(\d+) commands\./m);
  assert.ok(m, 'the command reference states a count as "<n> commands."');
  const actual = Object.keys(createCommands(stubApi())).length;
  assert.equal(Number(m[1]), actual,
               `the manual says ${m[1]} commands and there are ${actual}`);
});

test('every command’s prose and its schema describe the same arguments', () => {
  const cmds = createCommands(stubApi());
  const names = Object.keys(cmds);
  assert.ok(names.length > 30, `the whole grammar is covered: ${names.length} commands`);
  for (const name of names) {
    const cmd = cmds[name];
    // No schema means the gate has nothing to check and everything passes, so a
    // command that forgets one fails here rather than in front of a user.
    assert.ok(Array.isArray(cmd.args), `${name} declares an argument schema`);
    const prose = parseHelpArgs(cmd.help);
    assert.equal(prose.length, cmd.args.length,
                 `${name} takes ${cmd.args.length} arguments but says "${cmd.help}"`);
    for (let i = 0; i < prose.length; i++) {
      const a = cmd.args[i];
      assert.equal(prose[i].name, a.name,
                   `${name} argument ${i}: "${cmd.help}" vs schema <${a.name}>`);
      assert.equal(prose[i].optional, !!a.optional,
                   `${name} <${a.name}>: [optional] in the prose and in the schema disagree`);
      assert.ok(['int', 'num', 'text', 'enum'].includes(a.type), `${name} <${a.name}> is typed`);
      // An enum's values ARE its name in the prose, so a value added to one and
      // not the other shows up as a name that no longer matches.
      if (a.type === 'enum') assert.equal(a.values.join('|'), a.name, `${name} enum values`);
      if (a.min !== undefined && a.max !== undefined) {
        assert.ok(a.min <= a.max, `${name} <${a.name}> has a range that can be satisfied`);
      }
      // Only the last argument can swallow the rest of the line; anything
      // earlier would eat the arguments after it and never report a thing.
      if (a.rest) assert.equal(i, cmd.args.length - 1, `${name} <${a.name}> is last`);
    }
    assert.equal(cmd.sig, prose.length
      ? name + ' ' + prose.map((p) => (p.optional ? `[${p.name}]` : `<${p.name}>`)).join(' ')
      : name, `${name}: the signature refusals quote is the prose signature`);
  }
});

test('the help parser reads the signature and stops at the em dash', () => {
  // The commentary after the dash names <tick> a second time. A parser that
  // read the whole line would report three arguments for a command with two,
  // and the drift test above would fail on a file that is perfectly correct.
  assert.deepEqual(parseHelpArgs('tempo <bpm> [tick] — whole song, or one point from <tick>'),
                   [{ name: 'bpm', optional: false }, { name: 'tick', optional: true }]);
  assert.deepEqual(parseHelpArgs('list commands'), [], 'prose with no placeholders takes none');
  assert.deepEqual(parseHelpArgs('follow [on|off] — keep the playhead in view'),
                   [{ name: 'on|off', optional: true }]);
});

test('a bad argument is refused by name, not coerced', () => {
  const cmds = createCommands(stubApi());
  // Each of these used to be accepted: num(a[0], d) turns anything into a
  // number or a NaN and the command carried on with it.
  assert.equal(checkArgs('gain', cmds.gain, ['0', '400']),
               'gain: <dB> must be between -96 and 12, got 400');
  // 64 is one past `kUiMaxTracks`, which is what the bound means now — it was 16 while the
  // mixer had sixteen strips and the track-count intake clamped there.
  assert.equal(checkArgs('gain', cmds.gain, ['64', '0']),
               'gain: <track> must be between 0 and 63, got 64',
               'sixteen strips exist; the seventeenth threw a TypeError about undefined');
  assert.equal(checkArgs('note', cmds.note, ['60.5']),
               'note: <pitch> must be a whole number, got "60.5"');
  assert.equal(checkArgs('seek', cmds.seek, ['-1']), 'seek: <tick> must be at least 0, got -1');
  assert.equal(checkArgs('zoom', cmds.zoom, ['99']), 'zoom: <index> must be between 0 and 5, got 99');
  assert.equal(checkArgs('patch', cmds.patch, ['0', 'hits', 'lots']),
               'patch: <steps> must be a whole number, got "lots"',
               'this one sent nothing at all and reported the unchanged config');
  assert.equal(checkArgs('view', cmds.view, ['trackr']),
               'view: "trackr" is not one of tracker, arrange, piano, mixer, patcher');
  assert.equal(checkArgs('follow', cmds.follow, ['of']),
               'follow: "of" is not one of on, off', 'anything but "off" used to mean on');
  // And the ones that were always fine still are.
  assert.equal(checkArgs('gain', cmds.gain, ['63', '-96']), null);
  assert.equal(checkArgs('note', cmds.note, ['127', '480000', '100']), null);
  assert.equal(checkArgs('tempo', cmds.tempo, ['128.5']), null, 'a decimal bpm');
  assert.equal(checkArgs('transpose', cmds.transpose, ['-12']), null, 'a signed field');
});

test('arity is checked against the prose, and names what is missing', () => {
  const cmds = createCommands(stubApi());
  assert.equal(checkArgs('gain', cmds.gain, ['3']), 'gain: missing <dB> — gain <track> <dB>');
  assert.equal(checkArgs('gain', cmds.gain, []), 'gain: missing <track> — gain <track> <dB>');
  assert.equal(checkArgs('gain', cmds.gain, ['3', '0', '0']),
               'gain: too many arguments — gain <track> <dB>');
  assert.equal(checkArgs('play', cmds.play, ['now']), 'play: takes no arguments');
  // One argument, and the message already named it: quoting the signature after
  // that says the same thing twice.
  assert.equal(checkArgs('zoom', cmds.zoom, []), 'zoom: missing <index>');
  assert.equal(checkArgs('view', cmds.view, []),
               'view: missing <tracker|arrange|piano|mixer|patcher>');
  // Optional means optional, in both directions.
  assert.equal(checkArgs('select', cmds.select, ['8']), null, 'one row is a one-row range');
  assert.equal(checkArgs('note', cmds.note, ['60']), null);
  // A trailing name is the rest of the line, so the words after it are not
  // "too many arguments" — they are the name.
  assert.equal(checkArgs('rename', cmds.rename, ['0', 'Big', 'Bass']), null);
  assert.equal(checkArgs('rename', cmds.rename, ['0']),
               'rename: missing <name> — rename <track> <name>');
  // A command with no schema at all refuses rather than accepting anything.
  assert.match(checkArgs('bogus', { help: 'x' }, []), /no argument schema/);
});

test('the gate runs before the command, and the command still says what it said', () => {
  const api = stubApi();
  const cmds = createCommands(api);
  assert.throws(() => runCommand('gain', cmds.gain, ['0', '400'], null),
                /must be between -96 and 12/);
  assert.equal(api.calls.length, 0, 'the mixer never heard about it');
  // The log lines are part of the contract — an agent reads them back.
  assert.equal(runCommand('gain', cmds.gain, ['3', '-6'], null), 'gain t3 -6dB');
  assert.equal(runCommand('zoom', cmds.zoom, ['2'], null), 'zoom 2');
  assert.equal(runCommand('goto', cmds.goto, ['8', '1'], null), 'cursor 8 t1');
  assert.equal(runCommand('note', cmds.note, ['64'], null), 'note 64');
  assert.equal(runCommand('rename', cmds.rename, ['0', 'Big', 'Bass'], null),
               'renamed t0 to Big Bass');
  assert.equal(runCommand('tempo', cmds.tempo, ['128'], null), 'tempo 128 (whole song)');
  assert.equal(runCommand('tempo', cmds.tempo, ['128', '3840000'], null), 'tempo 128 from 3840000');
  assert.deepEqual(api.calls, ['gain(3,-6)', 'zoom(2)', 'goto(8,1)', 'note(64,,)',
                               'rename(0,Big Bass)', 'tempo(128,)', 'tempo(128,3840000)']);
  // A check the gate cannot make stays where it is: zero is inside every range
  // and still means nothing.
  assert.throws(() => runCommand('transpose', cmds.transpose, ['0'], null),
                /transpose by how much\?/);
});

test('the row pool grows, is reused, and never mixes the two feeds', () => {
  const pool = [];
  const cat = [{ name: 'A', vendor: 'v', format: 'VST3', ok: true, error: '' },
               { name: 'B', vendor: 'v', format: 'VST3', ok: true, error: '' }];
  assert.equal(fillRows(pool, cat, setPluginRow), 2);
  const first = pool[0], second = pool[1];
  // A re-list of ONE plugin must not drop the second slot: the pool is a pool.
  assert.equal(fillRows(pool, [cat[0]], setPluginRow), 1);
  assert.equal(pool.length, 2, 'the surplus slot is kept, not discarded');
  assert.equal(pool[0], first, 'and the live one is the same object as before');
  assert.equal(fillRows(pool, cat, setPluginRow), 2);
  assert.equal(pool[1], second, 'so a regrowth allocates nothing');
  // Projects and plugins never share a pool, which is why a project re-list —
  // it happens every time the rail opens — cannot rebuild 52 plugin strings.
  const projects = [];
  fillRows(projects, ['p'], setProjectRow);
  assert.equal(projects[0].kind, KIND_PROJECT);
  assert.equal(pool[0].kind, KIND_PLUGIN);
});

/**
 * The per-clip grid, unpacked from the flags word.
 *
 * `unpackClipGrid` mirrors `unpack_clip_grid` in ui/daw-bridge/src/layout.rs, and a
 * decoder that mirrors another decoder is exactly the thing that drifts quietly:
 * every wrong answer here is a plausible meter, so nothing downstream looks broken
 * — a clip in 7/8 simply draws its bar lines in the wrong places.
 *
 * Packed here from the shift/mask constants rather than from magic numbers, so the
 * test states the layout it believes in instead of hiding it in a literal.
 */
const packGrid = (lpb, num, denExp) => (lpb << 1) | (num << 6) | (denExp << 11);

test('unpackClipGrid reads the grid a clip publishes', () => {
  // The three the meter fixture carries, which is what the e2e asserts against.
  assert.deepEqual(unpackClipGrid(packGrid(4, 7, 3)),
                   { linesPerBeat: 4, numerator: 7, denominator: 8 }, '7/8 lpb4');
  assert.deepEqual(unpackClipGrid(packGrid(6, 5, 2)),
                   { linesPerBeat: 6, numerator: 5, denominator: 4 }, '5/4 lpb6');
  assert.deepEqual(unpackClipGrid(packGrid(4, 4, 2)),
                   { linesPerBeat: 4, numerator: 4, denominator: 4 }, '4/4 lpb4');
});

test('a clip that publishes no grid is not a clip in 4/4', () => {
  // linesPerBeat 0 is the sentinel, and the distinction is load-bearing: "no grid"
  // means COUNT ME IN THE SONG'S METER, which for a song in 7/8 is not 4/4. A
  // decoder that returned a default record here would silently put every such clip
  // in common time and look entirely reasonable doing it.
  assert.equal(unpackClipGrid(0), null);
  assert.equal(unpackClipGrid(1), null, 'the audio bit alone still means no grid');
  assert.equal(unpackClipGrid(packGrid(0, 7, 3)), null,
               'and a numerator without a lines-per-beat is still no grid');
});

test('unpackClipGrid ignores the bits that are not its own', () => {
  // bit 0 is UI_CLIP_EXTENT_AUDIO and the high bits are unallocated. An audio clip
  // has a grid like any other, so masking must not be sensitive to either.
  const g = packGrid(4, 7, 3);
  assert.deepEqual(unpackClipGrid(g | 1), unpackClipGrid(g), 'the audio bit');
  assert.deepEqual(unpackClipGrid(g | 0xffffc000), unpackClipGrid(g), 'the spare bits');
});

test('unpackClipGrid writes into a caller-owned record', () => {
  // It runs per rail per frame in the arrangement, so it must not allocate there
  // (GUIDELINES 3.1). The record is the caller's and is rewritten in place.
  const out = { linesPerBeat: 0, numerator: 0, denominator: 0 };
  const got = unpackClipGrid(packGrid(6, 5, 2), out);
  assert.equal(got, out, 'the same object comes back, not a copy');
  assert.deepEqual(out, { linesPerBeat: 6, numerator: 5, denominator: 4 });
  // Reused for a second clip: every field must be rewritten, or a clip that
  // published a smaller grid inherits the previous one's numerator.
  unpackClipGrid(packGrid(4, 4, 2), out);
  assert.deepEqual(out, { linesPerBeat: 4, numerator: 4, denominator: 4 });
});

// ---------------------------------------------------------------------------
// The song meter, where it actually reaches the screen.
//
// meter.js is tested above as arithmetic. These are the three places that
// arithmetic becomes a bar line, a bar number or a snap — the places that were
// counting every project in 4/4 because the constant was baked in at module
// level, and where being wrong looks exactly like being right.

const SEVEN_EIGHT = { numerator: 7, denominator: 8 };
const EIGHTH = 480000;                    // 960000 nanoticks per quarter / 2
const BAR_7_8 = EIGHTH * 7;               // 3360000

test('snapLoop snaps to the bar the project is actually in', () => {
  // 4/4 is unchanged, so the default still means what every caller assumed.
  assert.deepEqual(snapLoop(TICKS_PER_BAR * 0.6, TICKS_PER_BAR * 3.4, false),
                   { start: TICKS_PER_BAR, end: TICKS_PER_BAR * 3 });
  // In 7/8 the bar is 3,360,000 ticks, so a drag that used to land on 3,840,000
  // — a position no bar of this project begins on — lands on a real bar line.
  assert.deepEqual(snapLoop(BAR_7_8 * 0.6, BAR_7_8 * 3.4, false, SEVEN_EIGHT),
                   { start: BAR_7_8, end: BAR_7_8 * 3 });
});

test('a fine loop drag snaps to a BEAT, not to a quarter of a bar', () => {
  // These are the same number in 4/4 and in nothing else, which is how the bug
  // stayed invisible: `fine` computed TICKS_PER_BAR / 4. In 7/8 that is 1.75
  // eighths — a position nothing in the project can be on.
  const got = snapLoop(EIGHTH * 2.6, EIGHTH * 5.4, true, SEVEN_EIGHT);
  assert.deepEqual(got, { start: EIGHTH * 3, end: EIGHTH * 5 });
  assert.equal(got.start % EIGHTH, 0, 'and it lands on an eighth, the meter\'s beat');
});

test('the arrangement rules and numbers bars in the song meter', () => {
  const buf = createArrangeBuffer(2, 128);
  const opts = { startTick: 0, width: 800, zoomIndex: 0, tracks: 2,
                 meter: SEVEN_EIGHT };
  const m = buildArrangeModel(opts, buf);
  const tpp = m.view.ticksPerPixel;

  // Bar 1 is at tick 0 and bar 2 at 3,360,000 — NOT at 3,840,000, which is where
  // a 4/4 ruler puts it and which is a different moment in the music.
  assert.ok(m.rulerCount >= 2, `at least two bar numbers: ${m.rulerCount}`);
  assert.equal(buf.rulerBar[0], 1, 'bars are 1-based to the user');
  assert.equal(buf.ruler[0], 0);
  assert.equal(buf.ruler[1], BAR_7_8 / tpp, 'bar 2 is one 7/8 bar in');

  // And the gridline flagged as a bar is the same tick. gridIsBar drives a
  // heavier rule, so a grid that agreed with the ruler by accident at one zoom
  // and not another would draw the emphasis in the wrong place.
  const barLines = [];
  for (let i = 0; i < m.gridCount; i++) if (buf.gridIsBar[i]) barLines.push(buf.grid[i] * tpp);
  assert.equal(barLines[0], 0);
  assert.equal(barLines[1], BAR_7_8, 'the emphasised line is on the 7/8 bar too');
});

test('the harmony card counts the playhead in the song meter', () => {
  const buf = createHarmonyBuffer(4);
  // One 7/8 bar plus two eighths = tick 4,320,000, which is bar 2 beat 3 of a
  // song in 7/8.
  const tick = BAR_7_8 + EIGHTH * 2;
  buildHarmonyModel({ harmony: [], playheadTick: tick, meter: SEVEN_EIGHT }, buf);
  assert.equal(buf.at, 'bar 2.3');
  // The SAME tick counted in 4/4 is bar 2 beat 1 — the right bar by coincidence
  // and the wrong beat, which is the more dangerous of the two failures because
  // the bar number looks plausible. Asserted so that this test cannot pass against
  // a build that ignores the meter: the two answers have to differ.
  buildHarmonyModel({ harmony: [], playheadTick: tick }, buf);
  assert.equal(buf.at, 'bar 2.1');
});

// ---------------------------------------------------------------------------
// Per-lane grids, swept across the zoom axis.
//
// GUIDELINES 2.1.1 is about this exact test not existing. `useMixedGrid` was
// built to catch the projection bug and did not, because the golden that uses it
// is shot at zoom 0 — the ONE zoom where the projection was already correct. At
// every other zoom nothing was marked off-grid at all, so the tracker offered a
// writable cell on every row of a triplet lane whose rows sit at 1/3-beat
// positions a 4/beat axis cannot express.
//
// The cause was one rounding: the test was a stride in ROWS,
// `Math.round(zoom.linesPerBeat / lpb)`, and `Math.round(4 / 3)` is 1, so `r % 1`
// — which is 0 for every row. "Incommensurable" became "every row is fine".
//
// So this sweeps the axis instead of sampling it. The numbers are derived from
// the meter, not copied from a run: a lane at `lpb` lines per quarter has a row
// every 960000/lpb ticks, and a display row at `rowTicks` lands on it only when
// rowTicks * r is a multiple of that.

/** A minimal engine store: per-track lines-per-beat, no clips, no notes. */
function gridEngine(lpb) {
  return {
    ok: true, seq: 1, playheadTick: 0, visualSample: 0, clipVersion: 1,
    harmonyVersion: 0, transport: 0, trackCount: lpb.length,
    peaks: new Float32Array(4), peakCount: 0,
    notes: [], noteCount: 0,
    aggCount: new Uint32Array(0), aggRep: new Uint8Array(0),
    aggLo: new Uint8Array(0), aggHi: new Uint8Array(0), aggRows: 0, aggTracks: 0,
    extents: [], extentCount: 0, extentsRevision: 1,
    lpb: Uint8Array.from(lpb), notesRevision: 1, aggRevision: 1,
  };
}

test('every icon the app names exists in the bundled set', () => {
  // A misnamed icon class is not an error anywhere: the <i> renders, the button
  // has a real bounding box and reports a real rect to a probe, and nothing is
  // drawn inside it. That is exactly how the device-chain Open button shipped
  // invisible — I probed it, got a rect back, and reported it was there.
  // I then reached for `ph-rows-minus`, which does not exist either.
  //
  // The set is a vendored CSS file, so this is a cheap, total check.
  const css = readFileSync(new URL('../src/icons/phosphor.css', import.meta.url), 'utf8');
  const have = new Set();
  for (const m of css.matchAll(/^\.ph\.(ph-[a-z0-9-]+):before/gm)) have.add(m[1]);
  assert.ok(have.size > 100, `parsed the icon set: ${have.size} icons`);

  const files = ['../src/chrome.js', '../src/chain.js', '../src/dock.js',
                 '../src/arrange.js', '../src/mixer.js', '../index.html'];
  const missing = [];
  for (const f of files) {
    let src;
    try { src = readFileSync(new URL(f, import.meta.url), 'utf8'); } catch { continue; }
    for (const m of src.matchAll(/['"`]ph ph-([a-z0-9-]+)/g)) {
      const name = 'ph-' + m[1];
      if (!have.has(name)) missing.push(`${f.replace('../', '')}: ${name}`);
    }
  }
  assert.deepEqual(missing, [], `icons named but not in the set:\n  ${missing.join('\n  ')}`);
});

/*
 * WHERE A NOTE SOUNDS IS THE SUM OF THREE TERMS, and the mark has to be the sum.
 *
 * A note is drawn on the row its authored tick DIVIDES into, so it generally sits
 * somewhere inside that row rather than on its edge. On top of that its own delay
 * pushes it late, and its lane's quantize pulls it toward a grid. The engine adds
 * all three — quantize on the scheduling copy, delay at strike expansion — so the
 * mark is (tOn mod rowTicks) + delay + dev.
 *
 * Every one of those three has been wrong here at some point, and none of the
 * failures looked like a failure:
 *   - The in-row term was MISSING, so a note quantize pulls exactly onto a grid
 *     line was drawn as sounding before its own row.
 *   - The golden fixture put the same offset in tOn AND delayTicks, which the
 *     renderer's missing term made invisible: a wrong fixture and a wrong renderer
 *     agreeing, with a blessed screenshot on top.
 *   - `dev` arrived stale, because three separate version gates all omitted the
 *     quantize counter.
 *
 * So the composition is pinned here, in arithmetic, where no zoom, cache or
 * screenshot can agree with it by accident.
 */
/*
 * A COLLIDED CELL'S RIBBON SHOWS THE PITCHES IN IT.
 *
 * The contour ribbon has two sources. A cell holding ONE note draws it from `pitch`;
 * a cell summarising a SPAN draws it from `aggLo`/`aggHi`. The renderer chooses on
 * `aggCount`.
 *
 * The collide branch — several notes on one row — sets `aggCount` and then writes
 * its spread into `pitch` and `_hiPitch`, which that choice makes the renderer stop
 * reading. So the ribbon for a collided cell came from `aggLo`/`aggHi`, which
 * nothing had written: zero, i.e. the bottom of the pitch scale, for exactly the
 * cells the collide pill exists to explain.
 *
 * Asserted on the MODEL's fields rather than through a screenshot, because a golden
 * would have to be blessed against whatever it currently draws — and it was blessed,
 * with the ribbon at the floor.
 */
/*
 * A COLLIDED CELL'S VELOCITY IS "mix" ONLY WHEN THE VELOCITIES DISAGREE.
 *
 * It printed a number whenever the PITCHES matched — so two C-4s at 20 and 127 showed
 * one of them, whichever arrived last, as though it were the velocity of the cell. An
 * arbitrary value presented as a fact, and it changes when the wire reorders.
 *
 * The condition asked about pitch for a quantity that is velocity. Both directions are
 * pinned here, because dropping the condition entirely would be the other error: "4x
 * C-4" at velocity 100 each should print 100, which is useful and true.
 */
/*
 * CHORDS AT AN AGGREGATING ZOOM.
 *
 * The note loop is gated on `!zoom.aggregate` — at 1 bar per row a cell holds a COUNT,
 * and placing an individual note in it would put a name where the cell is summarising
 * something else. The chord loop has no such gate. This asks what it actually does,
 * because "it is inconsistent" is not the same as "it is wrong" and the answer decides
 * the fix.
 */
test('what a chord does at an aggregating zoom', () => {
  const engine = gridEngine([4]);
  engine.chords = [{ tick: 16 * 960000, duration: 960000, id: 1, track: 0, degree: 0,
                     quality: 1, inversion: 0, octave: 4, flags: 0, row: 4 }];
  engine.chordCount = 1;
  // zoomIndex 4 is "1 bar" — aggregate. The sidecar computed `row` for the grid the
  // viewport asked for, so the row it carries is the authority.
  const vm = buildViewModel({ startRow: 0, rowCount: 8, tracks: 1, columns: 3,
                              zoomIndex: 4, engine }, createBuffer(8, 1, 3));
  let placed = -1;
  for (let r = 0; r < 8; r++) {
    for (const c of vm.rows[r].cells) {
      if (c.kind === 'chord' || (c.text && /^[iIvV]/.test(c.text))) { placed = r; break; }
    }
    if (placed >= 0) break;
  }
  /*
   * It lands on the row the WIRE said, which is the row the sidecar computed for this
   * viewport's grid — so it is where it belongs, not 4x or 16x down the timeline. The
   * inconsistency with the note loop is real and benign: a chord is one name per span,
   * which is exactly what an aggregate row wants, where a note is one of possibly
   * hundreds and cannot be.
   */
  assert.equal(placed, 4, 'a chord is placed on the row the wire gave it');
});

test('a collided cell prints a velocity only when they agree', () => {
  const ROW = ZOOM_LEVELS[1].rowNanoticks;
  const mk = (pitch, vel, id) => ({
    tOn: 0, tOff: 100, id, pitch, velocity: vel, column: 0, track: 0,
    retrigger: 0, probability: 0, delayTicks: 0, devTicks: 0, row: 0,
    muted: false, isAdd: false, placementId: 1,
  });
  const velOf = (notes) => {
    const engine = gridEngine([4]);
    engine.notes = notes; engine.noteCount = notes.length;
    const vm = buildViewModel({ startRow: 0, rowCount: 2, tracks: 1, columns: 3,
                                zoomIndex: 1, engine }, createBuffer(2, 1, 3));
    return { note: vm.rows[0].cells[0].text, vel: vm.rows[0].cells[1].text };
  };

  // SAME pitch, DIFFERENT velocities — the case that printed an arbitrary one.
  const differ = velOf([mk(60, 20, 1), mk(60, 127, 2)]);
  assert.match(differ.note, /C-4/, 'the pill still names the pitch');
  assert.equal(differ.vel, 'mix', 'and the velocity refuses to pick one of them');

  // SAME pitch, SAME velocity — a doubled note, and 100 is the honest answer.
  const agree = velOf([mk(60, 100, 3), mk(60, 100, 4)]);
  assert.notEqual(agree.vel, 'mix', 'two notes at one velocity print it');

  // DIFFERENT pitches, different velocities: still a mix.
  assert.equal(velOf([mk(60, 20, 5), mk(67, 127, 6)]).vel, 'mix',
               'a chord in one column with differing velocities is a mix too');
});

test('a cell holding several notes reports the spread of them', () => {
  const engine = gridEngine([4]);
  const mk = (row, pitch, id) => ({
    tOn: row * ZOOM_LEVELS[1].rowNanoticks, tOff: row * ZOOM_LEVELS[1].rowNanoticks + 100,
    id, pitch, velocity: 100, column: 0, track: 0, retrigger: 0, probability: 0,
    delayTicks: 0, devTicks: 0, row, muted: false, isAdd: false, placementId: 1,
  });
  // Three notes on ONE row and column: a chord typed into one column, which is the
  // ambiguous data the pill reports rather than a chord across columns.
  engine.notes = [mk(0, 48, 1), mk(0, 60, 2), mk(0, 55, 3)];
  engine.noteCount = 3;
  const buf = createBuffer(4, 1, 3);
  const vm = buildViewModel({ startRow: 0, rowCount: 4, tracks: 1, columns: 3,
                              zoomIndex: 1, engine }, buf);
  const cell = vm.rows[0].cells[0];
  assert.equal(cell.kind, 'collide', 'the cell reports a collision');
  assert.equal(cell.aggCount, 3, 'and how many');
  /*
   * THE SPREAD, in the fields the renderer actually reads for a cell with an
   * aggCount. 48 to 60, whatever order they arrived in.
   */
  assert.equal(cell.aggLo, 48, 'the lowest pitch in the cell');
  assert.equal(cell.aggHi, 60, 'and the highest');
});

test('the deviation mark is the sum of in-row, delay and quantize', () => {
  const ROW = ZOOM_LEVELS[1].rowNanoticks;      // 240000, a 16th
  const engine = gridEngine([4]);
  // One note per row so nothing collides — a collided cell deliberately draws no
  // mark, and that would make this pass for the wrong reason.
  const at = (row, inRow, delay, dev) => ({
    tOn: row * ROW + inRow, tOff: row * ROW + inRow + 1000, id: 100 + row,
    pitch: 60, velocity: 100, column: 0, track: 0, retrigger: 0, probability: 0,
    delayTicks: delay, devTicks: dev, row, muted: false, isAdd: false, placementId: 1,
  });
  engine.notes = [
    at(0, 0, 0, 0),                     // dead on the line: no mark at all
    at(1, ROW / 4, 0, 0),               // written a quarter in
    at(2, 0, ROW / 2, 0),               // on the line, delayed half a row
    at(3, ROW / 4, 0, ROW / 4),         // written a quarter in, pushed a quarter later
    at(4, ROW / 2, 0, -ROW / 4),        // written halfway, pulled a quarter earlier
    at(5, ROW / 4, 0, -ROW / 2),        // pulled to BEFORE its own row
  ];
  engine.noteCount = engine.notes.length;
  const buf = createBuffer(8, 1, 3);
  const vm = buildViewModel({ startRow: 0, rowCount: 8, tracks: 1, columns: 3,
                              zoomIndex: 1, engine }, buf);
  const dev = (row) => vm.rows[row].cells[0].dev;
  const out = (row) => vm.rows[row].cells[0].devOut;

  assert.equal(dev(0), -1, 'a note on its row line has nothing to say');
  assert.equal(dev(1), 25, 'a note written a quarter into its row is marked there');
  assert.equal(dev(2), 50, 'a delay alone still reads from the row start');
  assert.equal(dev(3), 50, 'in-row and quantize ADD rather than replace');
  assert.equal(dev(4), 25, 'and a negative deviation subtracts');
  /*
   * SPILL. Written a quarter in, pulled half a row earlier: it sounds BEFORE this
   * row. Pinned to the edge, and flagged — because a mark at 0% that means
   * "somewhere earlier" is a mark claiming the note is on time, which is the one
   * thing a deviation mark must never say.
   */
  assert.equal(dev(5), 0, 'a note pulled before its row pins to the edge');
  assert.equal(out(5), -1, 'and says so, rather than reading as on time');
  assert.equal(out(3), 0, 'while one that stays inside does not');
});

test('a removed track takes no width and its neighbours keep their ids', () => {
  // kShmVersion 22: RemoveTrack tombstones the slot rather than compacting the
  // array, so `uiTrackCount` is the EXTENT and slot 1 here is a hole. The lane
  // must vanish; tracks 0 and 2 must stay at ids 0 and 2. Compaction would move
  // track 2 to slot 1 and silently repoint every cursor, selection and per-track
  // cache keyed on the index — the failure this contract exists to avoid.
  const ABSENT = 1 << 2;
  const tracks = 3, rowCount = 8;
  const engine = gridEngine([4, 4, 4]);
  engine.trackParent = Uint32Array.from([0, 0, 0]);
  engine.trackFlags = Uint8Array.from([0, ABSENT, 0]);
  const buf = createBuffer(rowCount, tracks, 3);
  const vm = buildViewModel({ startRow: 0, rowCount, tracks, columns: 3,
                              zoomIndex: 1, engine }, buf);
  assert.equal(vm.laneHidden[1], 1, 'the tombstoned lane is hidden');
  assert.equal(vm.laneHidden[0], 0, 'the track before it is not');
  assert.equal(vm.laneHidden[2], 0, 'and neither is the one after');

  // The cells still carry their ORIGINAL track ids. The hole is a drawing
  // decision, never a renumbering.
  const ids = new Set();
  for (const cell of vm.rows[0].cells) ids.add(cell.track);
  assert.ok(ids.has(0) && ids.has(2), `tracks 0 and 2 survive: ${[...ids]}`);
});

test('a tombstone with a stale parent id does not walk into the ancestor check', () => {
  // A removed slot's parent_id is whatever it was before removal. Reading it
  // could hide an unrelated lane, or spin if it points at another tombstone, so
  // ABSENT is checked before the walk and not inside it.
  const ABSENT = 1 << 2, HAS_PARENT = 1 << 1;
  const tracks = 3, rowCount = 4;
  const engine = gridEngine([4, 4, 4]);
  engine.trackParent = Uint32Array.from([2, 0, 1]);          // deliberately cyclic
  engine.trackFlags = Uint8Array.from([0, ABSENT | HAS_PARENT, 0]);
  const buf = createBuffer(rowCount, tracks, 3);
  const vm = buildViewModel({ startRow: 0, rowCount, tracks, columns: 3,
                              zoomIndex: 1, engine }, buf);
  assert.equal(vm.laneHidden[1], 1, 'the tombstone is hidden');
  assert.equal(vm.laneHidden[0], 0, 'and it did not drag a live lane down with it');
  assert.equal(vm.laneHidden[2], 0);
});

/** How many of `rowCount` rows are marked off-grid on each track. */
function offGridPerTrack(lpb, zoomIndex, rowCount) {
  const tracks = lpb.length;
  const buf = createBuffer(rowCount, tracks, 3);
  const vm = buildViewModel({ startRow: 0, rowCount, tracks, columns: 3,
                              zoomIndex, engine: gridEngine(lpb) }, buf);
  const out = new Array(tracks).fill(0);
  for (let ri = 0; ri < rowCount; ri++) {
    for (const cell of vm.rows[ri].cells) {
      if (cell.col === 0 && cell.kind === 'offgrid') out[cell.track]++;
    }
  }
  return out;
}

test('a lane is off-grid exactly where its own grid does not land', () => {
  const LPB = [4, 3, 6];
  const ROWS = 37;                       // the row count GUIDELINES 2.1.1 measured

  // Derived from the meter rather than transcribed, so the expectation cannot
  // inherit a bug from the implementation it is checking.
  const expected = (zoomIndex) => LPB.map((lpb) => {
    const laneRowTicks = 960000 / lpb;
    let off = 0;
    for (let r = 0; r < ROWS; r++) {
      if ((r * ZOOM_LEVELS[zoomIndex].rowNanoticks) % laneRowTicks !== 0) off++;
    }
    return off;
  });

  for (let z = 0; z <= 3; z++) {         // every non-aggregate zoom, not just one
    assert.deepEqual(offGridPerTrack(LPB, z, ROWS), expected(z),
                     `zoom ${z} (${ZOOM_LEVELS[z].label})`);
  }
});

test('the zoom sweep is what catches it — zoom 0 alone does not', () => {
  const LPB = [4, 3, 6], ROWS = 37;
  // Zoom 0 was ALWAYS right, which is why a golden shot there passed throughout.
  assert.deepEqual(offGridPerTrack(LPB, 0, ROWS), [24, 27, 18],
                   'the table in GUIDELINES 2.1.1, unchanged');
  // These three are the regression. The old code reported 0 off-grid on every
  // lane at every one of these zooms — a triplet lane offering a writable cell on
  // every 1/16 row. If this ever reads [0,0,0] again the rounding is back.
  assert.deepEqual(offGridPerTrack(LPB, 1, ROWS), [0, 27, 18], 'zoom 1, 1/16');
  assert.deepEqual(offGridPerTrack(LPB, 2, ROWS), [0, 18, 0], 'zoom 2, 1/8');
  // A quarter note is a whole number of rows in every one of these lanes, so at
  // 1/4 they genuinely all land. Asserted so the sweep has a negative case: a
  // build that marked everything off-grid would pass the three above.
  assert.deepEqual(offGridPerTrack(LPB, 3, ROWS), [0, 0, 0], 'zoom 3, 1/4');
});

test('a clip carries its grid, and its own start is the anchor', () => {
  const ROWS = 16;
  const engine = gridEngine([4]);
  // One clip, triplets, starting one 1/16 into the song — deliberately NOT on the
  // song's own grid. Its rows are every 320000 ticks FROM ITS START, so the rows
  // it lands on are not the ones it would land on anchored at zero. That is the
  // difference between a clip's grid being a property of the clip and being a
  // property of where the clip happens to sit.
  const START = 240000;
  engine.extents = [{ placementId: 1, clipId: 1, track: 0, flags: 0,
                      startTick: START, endTick: START + 960000 * 8, name: 'trip',
                      audio: false, grid: { linesPerBeat: 3, numerator: 4, denominator: 4 } }];
  engine.extentCount = 1;

  const buf = createBuffer(ROWS, 1, 3);
  const vm = buildViewModel({ startRow: 0, rowCount: ROWS, tracks: 1, columns: 3,
                              zoomIndex: 0, engine }, buf);   // zoom 0 = 80000/row
  const onGrid = [];
  for (let ri = 0; ri < ROWS; ri++) {
    const c = vm.rows[ri].cells.find((x) => x.col === 0);
    if (c.kind !== 'offgrid') onGrid.push(ri);
  }
  // Rows before the clip fall back to the track's own lpb of 4 (240000 ticks), so
  // rows 0..2 are governed by that; from row 3 (tick 240000) the clip takes over
  // and its rows are every 4th from there.
  assert.ok(onGrid.includes(3), 'the clip\'s first row is on its grid');
  assert.ok(onGrid.includes(7), 'and every 4th row after it (320000 ticks apart)');
  assert.ok(onGrid.includes(11), 'and the next');
  assert.ok(!onGrid.includes(5), 'a row between them is not');
  assert.ok(!onGrid.includes(9), 'nor this one');
  // The anchor is what this test is really about: anchored at ZERO instead, a
  // triplet grid lands on rows 0,4,8,12 — so row 4 would be on-grid and row 3
  // would not, the exact inverse of the above.
  assert.ok(!onGrid.includes(4), 'row 4 is on-grid only if the anchor were the song origin');
});

// ---------------------------------------------------------------------------
// Two caches that were serving the wrong answer.

test('a row that hits the label cache still gets its OWN accents', () => {
  // `_pos` is one shared record. positionOf() used to fill it only when the label
  // was a cache MISS, while the bar and beat flags were read from it every time —
  // so a row whose label came from the intern table published the accents of
  // whatever tick last missed. The labels stayed right, which is what made it hard
  // to see: the stripes moved, the numbers did not.
  const ROWS = 8;
  const buf = createBuffer(ROWS, 1, 3);
  const opts = { startRow: 0, rowCount: ROWS, tracks: 1, columns: 3, zoomIndex: 3 };

  // Fill the cache for rows 0..7 ...
  buildViewModel({ ...opts, startRow: 0 }, buf);
  // ... then for a window that ends on a row which is NOT a bar, so the shared
  // record is left holding onBar=false.
  buildViewModel({ ...opts, startRow: 100 }, buf);
  assert.equal(buf.rows[ROWS - 1].bar, false, 'row 107 is mid-bar, as the setup needs');

  // Now every row 0..7 is a cache HIT. Row 0 is tick 0, which is a bar in any
  // meter; if it reports otherwise it is wearing row 107's accents.
  buildViewModel({ ...opts, startRow: 0 }, buf);
  assert.equal(buf.rows[0].bar, true, 'tick 0 is a bar');
  assert.equal(buf.rows[0].label, '1:1', 'and its label agrees, which it always did');
  assert.equal(buf.rows[4].bar, true, 'so is the next one, four quarters on');
  assert.equal(buf.rows[1].bar, false, 'and row 1 is not');
});

test('moving a clip bumps the content revision even with no note edit', () => {
  // A clip's own lines-per-beat and meter are packed into UiClipExtent.flags, and
  // they decide which rows are off-grid. `extentsRevision` is the only thing that
  // moves when they change: notesRevision, aggRevision and rowGrid all stand still.
  // Without it in the signature the view-model computes the new marking correctly
  // and the renderer never rebinds the rows whose index did not move, so the old
  // grid stays on screen — GUIDELINES 2.1, from the inside.
  const engine = gridEngine([4]);
  engine.extents = [{ placementId: 1, clipId: 1, track: 0, flags: 0,
                      startTick: 0, endTick: 960000 * 8, name: 'a', audio: false,
                      grid: { linesPerBeat: 4, numerator: 4, denominator: 4 } }];
  engine.extentCount = 1;
  engine.extentsRevision = 1;

  const buf = createBuffer(8, 1, 3);
  const opts = { startRow: 0, rowCount: 8, tracks: 1, columns: 3, zoomIndex: 0, engine };
  buildViewModel(opts, buf);
  const before = buf.contentRevision;

  // The clip becomes triplets. Nothing else about the world changes — no note is
  // added, moved or deleted, and the aggregates are untouched.
  engine.extents[0].grid.linesPerBeat = 3;
  engine.extentsRevision = 2;
  buildViewModel(opts, buf);

  assert.notEqual(buf.contentRevision, before,
                  'the renderer is told the rows changed');
});

// ---------------------------------------------------------------------------
// The per-lane clip-local readout.

test('a lane reads its position in ITS clip, from the clip\'s own start', () => {
  // 3/4, four lines per quarter, starting one 1/16 into the song — deliberately
  // not on a song bar, because "clip bar 1 is not song bar 1" is the entire point
  // and every placement in presets/ is at tick 0.
  //
  // At zoom 1 a row is 240000 ticks. A 3/4 bar is 2,880,000 = 12 rows; a beat is
  // 960000 = 4 rows.
  const engine = gridEngine([4]);
  const START = 240000;
  engine.extents = [{ placementId: 1, clipId: 1, track: 0, flags: 0,
                      startTick: START, endTick: START + 2880000 * 3, name: 'waltz',
                      audio: false, grid: { linesPerBeat: 4, numerator: 3, denominator: 4 } }];
  engine.extentCount = 1;
  engine.extentsRevision = 1;

  const buf = createBuffer(20, 1, 3);
  const vm = buildViewModel({ startRow: 0, rowCount: 20, tracks: 1, columns: 3,
                              zoomIndex: 1, engine }, buf);
  const at = (r) => ({ text: vm.rows[r].laneBar[0], acc: vm.rows[r].laneAcc[0] });

  assert.deepEqual(at(1), { text: '1:1', acc: 3 }, 'the clip starts at ITS bar 1 beat 1');
  assert.deepEqual(at(2), { text: '1:1', acc: 0 }, 'still beat 1, no accent');
  assert.deepEqual(at(5), { text: '1:2', acc: 2 }, 'four rows on is beat 2');
  assert.deepEqual(at(9), { text: '1:3', acc: 2 }, 'and beat 3');
  assert.deepEqual(at(13), { text: '2:1', acc: 3 }, 'twelve rows on is bar 2 — 3/4, not 4/4');
  // The song gutter is unmoved and still counts song bars: row 13 is tick
  // 3,120,000, which is inside song bar 1 of a 4/4 song. Both readings on screen
  // at once is the point — the lane says where you are in the clip, the gutter
  // says where the clip is.
  assert.equal(vm.rows[13].label, '1:4:01', 'the song gutter still counts the song');
  assert.equal(vm.rows[0].laneBar[0], '', 'before the clip, the lane says nothing');
});

test('a lane with no clip, and an audio region, both read blank', () => {
  const engine = gridEngine([4, 4]);
  engine.extents = [
    // Audio: the engine packs a grid for these too, without checking the clip
    // kind, so what arrives is the 4/4 default rather than an authored meter.
    { placementId: 1, clipId: 1, track: 0, flags: 1, startTick: 0, endTick: 960000 * 8,
      name: 'a', audio: true, grid: { linesPerBeat: 4, numerator: 4, denominator: 4 } },
  ];
  engine.extentCount = 1;
  engine.extentsRevision = 1;

  const buf = createBuffer(4, 2, 3);
  const vm = buildViewModel({ startRow: 0, rowCount: 4, tracks: 2, columns: 3,
                              zoomIndex: 1, engine }, buf);
  assert.equal(vm.rows[0].laneBar[0], '', 'no bar numbers off a default meter');
  assert.equal(vm.rows[0].laneAcc[0], 0, 'and no accents either');
  assert.equal(vm.rows[0].laneBar[1], '', 'a lane with no clip at all says nothing');
});

test('a clip with no published grid is counted in the song meter', () => {
  // linesPerBeat 0 is the sentinel for "no grid", which means COUNT ME IN THE
  // SONG'S METER — not "I am in 4/4". For a song in 7/8 those differ.
  const engine = gridEngine([4]);
  engine.extents = [{ placementId: 1, clipId: 1, track: 0, flags: 0,
                      startTick: 0, endTick: 3360000 * 4, name: 'plain',
                      audio: false, grid: null }];
  engine.extentCount = 1;
  engine.extentsRevision = 1;

  const buf = createBuffer(20, 1, 3);
  // Song in 7/8: a bar is 7 eighths = 3,360,000 ticks = 14 rows at zoom 1.
  const vm = buildViewModel({ startRow: 0, rowCount: 20, tracks: 1, columns: 3,
                              zoomIndex: 1, engine,
                              meter: { numerator: 7, denominator: 8 } }, buf);
  assert.equal(vm.rows[0].laneBar[0], '1:1');
  assert.equal(vm.rows[14].laneBar[0], '2:1', 'fourteen rows on, so it counted 7/8');
  assert.notEqual(vm.rows[16].laneBar[0], '2:1', 'and not 4/4, which would put bar 2 at row 16');
});

test('a clip changing meter refreshes the readout without scrolling', () => {
  // The readout is guarded so a frame that only advances the playhead does not
  // recompute it — four Float64Array reads and two divisions per lane per row is
  // 745 B/draw of heap numbers otherwise. The guard's KEY is the whole question:
  // it watches the window, the zoom, the song meter AND extentsRevision. Drop the
  // last and this is the case that breaks — same scroll position, different clip.
  const engine = gridEngine([4]);
  engine.extents = [{ placementId: 1, clipId: 1, track: 0, flags: 0,
                      startTick: 0, endTick: 960000 * 16, name: 'c', audio: false,
                      grid: { linesPerBeat: 4, numerator: 4, denominator: 4 } }];
  engine.extentCount = 1;
  engine.extentsRevision = 1;

  const buf = createBuffer(20, 1, 3);
  const opts = { startRow: 0, rowCount: 20, tracks: 1, columns: 3, zoomIndex: 1, engine };
  buildViewModel(opts, buf);
  assert.equal(buf.rows[16].laneBar[0], '2:1', '4/4: bar 2 is 16 rows in');

  // Same window, same zoom, same song meter. Only the clip changed.
  engine.extents[0].grid.numerator = 3;
  engine.extentsRevision = 2;
  buildViewModel(opts, buf);
  assert.equal(buf.rows[12].laneBar[0], '2:1', '3/4: bar 2 is 12 rows in');
  assert.notEqual(buf.rows[16].laneBar[0], '2:1', 'and row 16 is no longer bar 2');
});

// ---------------------------------------------------------------------------
// Device buses (kShmVersion 20), and the one rule that makes them drawable.

/** A chain entry with `n` of `want` buses delivered. */
function chainWithBuses(n, want, opts = {}) {
  const buses = [];
  for (let i = 0; i < n; i++) {
    buses.push({ input: !!opts.input && i === 0, main: i === 0, enabled: true,
                 index: i, channels: 2, layoutId: 2, offset: i * 2, name: 'Out ' + i });
  }
  return { 0: { track: 0, version: 1, devices: [{
    id: 7, kind: 1, pos: 0, node: -1, slot: 0, caps: 0, bypass: 0,
    busCount: want, busTruncated: !!opts.truncated, buses }] } };
}

test('a device summarises its buses only once they have all arrived', () => {
  const buf = createChainBuffer(4);
  // Eight coming, two here. The card must NOT say "2 out" — that is a number a
  // user might act on, and it is wrong. This is the whole reason busCount is on
  // the wire, and the reason I held the version bump for it.
  let vm = buildChainModel({ track: 0, chains: chainWithBuses(2, 8), selected: -1,
                             trackName: 'T1' }, buf);
  assert.equal(vm.cards[0].busPartial, true);
  assert.equal(vm.cards[0].busText, 'buses 2/8');

  // All eight: now it describes the device.
  vm = buildChainModel({ track: 0, chains: chainWithBuses(8, 8), selected: -1,
                         trackName: 'T1' }, buf);
  assert.equal(vm.cards[0].busPartial, false);
  assert.equal(vm.cards[0].busText, '8 out');
});

test('inputs and outputs are counted apart', () => {
  const buf = createChainBuffer(4);
  // A sidechain input is not a stem, and a rack that added them together would
  // report a 2-out plugin with a sidechain as having three outputs.
  const vm = buildChainModel({ track: 0, chains: chainWithBuses(3, 3, { input: true }),
                               selected: -1, trackName: 'T1' }, buf);
  assert.equal(vm.cards[0].busText, '2 out · 1 in');
});

test('a device with no buses says nothing rather than zero', () => {
  const buf = createChainBuffer(4);
  const vm = buildChainModel({ track: 0, chains: chainWithBuses(0, 0), selected: -1,
                               trackName: 'T1' }, buf);
  assert.equal(vm.cards[0].busText, '', 'no line at all');
  assert.equal(vm.cards[0].busPartial, false, 'and it is not "still arriving"');
});

test('truncation is a different state from still-arriving', () => {
  const buf = createChainBuffer(4);
  // 32 is the wire's cap. "There were more" never resolves, where "3 of 8" does —
  // so they must not render as the same thing.
  const vm = buildChainModel({ track: 0, chains: chainWithBuses(32, 33, { truncated: true }),
                               selected: -1, trackName: 'T1' }, buf);
  assert.equal(vm.cards[0].busTruncated, true);
  assert.equal(vm.cards[0].busPartial, true, '32 of 33 is also incomplete');
});

test('the bus summary is guarded and interned', () => {
  const buf = createChainBuffer(4);
  const chains = chainWithBuses(8, 8);
  const vm1 = buildChainModel({ track: 0, chains, selected: -1, trackName: 'T1' }, buf);
  const first = vm1.cards[0].busText;
  const vm2 = buildChainModel({ track: 0, chains, selected: -1, trackName: 'T1' }, buf);
  // The SAME string object, not an equal one: this runs per card per draw and the
  // rack is redrawn on every frame the view is open.
  assert.ok(first === vm2.cards[0].busText, 'no string is built on an unchanged draw');
});

// ---------------------------------------------------------------------------
// The op registry: everything the keyboard can do, something else can do too.
//
// ARCHITECTURE_REVIEW Movement 2 item 21. Hard requirement 4 is that the UI is
// AI-OPERABLE — an agent must be able to drive it — and an action reachable only
// by a keystroke is an action an agent cannot reach. The dock and the palette
// share one command table (`createCommands`), so a command covers both; the gap
// this closes is a keystroke with no command behind it.
//
// The table below is read against index.html's ACTUAL source rather than against
// a second list, for the reason the wire-drift tests are: a duplicate of a thing
// is not a check on it. A new key added to the handler fails here until it is
// either given a command or explicitly recorded as navigation.

/**
 * Every key the app's handler matches on, and how an agent reaches the same
 * thing. `cmd` names the command; `nav` means it only moves the cursor or the
 * viewport, which `goto`, `select` and `zoom` already cover.
 */
const KEY_OPS = {
  // Transport and time.
  ' ': { cmd: 'play' }, '`': { cmd: 'stop' },
  // Navigation. An agent addresses rows and tracks directly rather than walking
  // to them, so these need no command of their own — but they are listed, not
  // omitted, so that "no command" stays a decision rather than an oversight.
  ArrowUp: { nav: true }, ArrowDown: { nav: true },
  ArrowLeft: { nav: true }, ArrowRight: { nav: true },
  PageUp: { nav: true }, PageDown: { nav: true },
  Home: { nav: true }, Tab: { nav: true },
  // Editing.
  Delete: { cmd: 'del' }, Backspace: { cmd: 'del' },
  c: { cmd: 'copy' }, s: { cmd: 'save' }, S: { cmd: 'save' },
  // Bypass the selected device, in the rack.
  b: { cmd: 'bypass' }, B: { cmd: 'bypass' },
  // Commits an open token buffer; with none open it plays from the cursor row,
  // which is `seek` + `play` and reachable as both.
  Enter: { nav: true },
  Escape: { nav: true },       // cancels; nothing to commit through a command
  // Views and panels.
  p: { cmd: 'view' }, r: { cmd: 'view' }, t: { cmd: 'view' },
  f: { cmd: 'follow' }, F: { cmd: 'follow' },
  '?': { nav: true },          // help overlay: a view of the keymap, not an edit
  // Zoom and octave.
  '[': { cmd: 'zoom' }, ']': { cmd: 'zoom' },
  '-': { cmd: 'oct' }, '_': { cmd: 'oct' }, '+': { cmd: 'oct' }, '=': { cmd: 'oct' },
  '/': { cmd: 'oct' }, '.': { cmd: 'oct' }, ',': { cmd: 'oct' }, ';': { cmd: 'oct' },
  a: { nav: true },            // note-off in the pitch column; `note` covers writes
  // NOTE COLUMNS. A second column is what makes a track polyphonic, and it is how a slide is
  // written — two columns give a monophonic instrument two overlapping note-ons on one channel,
  // which inside a single column cannot be said at all. `columns N` is the other route.
  '{': { cmd: 'columns' }, '}': { cmd: 'columns' },
};

/** The Alt combos, matched on e.code because Option is a compose key on macOS. */
const ALT_OPS = {
  KeyA: { cmd: 'select' }, KeyC: { cmd: 'copy' }, KeyQ: { cmd: 'transpose' },
  KeyS: { cmd: 'solo' }, KeyV: { cmd: 'paste' }, KeyW: { cmd: 'mute' },
  KeyX: { cmd: 'cut' },
};

/*
 * THE METER SCALE, which decides where a level lands on a 64px bar.
 *
 * Pure arithmetic and therefore testable without an engine, which matters
 * because the failure it guards is not a crash: a scale that is wrong by a
 * factor still draws a plausible bar that moves with the music, and the only
 * way to notice is to compare it against a number nobody has on screen.
 */
/*
 * AND THE dB NUMBER ON THE CARD IS THE dB NUMBER.
 *
 * The e2e "checked" this by comparing the model's string against the same string in
 * the DOM — true by construction. Mutating the divisor from 100 to 10 kept the whole
 * suite green, because both sides of that comparison came from the same expression.
 *
 * This is the arithmetic on its own, against numbers written out by hand.
 */
test('a millibel reading prints as dB', () => {
  assert.equal(meterDb(0), '0.0', 'full scale');
  assert.equal(meterDb(-3450), '-34.5', 'a hundredth of a dB per millibel');
  assert.equal(meterDb(-600), '-6.0', 'and a round one stays round');
  assert.equal(meterDb(-12000), '-120.0', 'the bottom of the engine\'s range');
  // Not "-327.7": there is no dB value for nothing, and the sentinel is not a level.
  assert.equal(meterDb(METER_SILENT), '−∞', 'silence is a symbol, not a number');
});

test('a millibel reading lands where the scale says', () => {
  // 0 mB is FULL SCALE on this contract, not silence. Getting this backwards
  // would draw a stopped transport as a pegged meter, which is what the engine
  // did for one build and why the sentinel below exists at all.
  assert.equal(meterScale(0), 1, '0 mB is full scale');
  assert.equal(meterScale(-3000), 0.5, 'half the scale is -30 dB');
  assert.equal(meterScale(-6000), 0, 'and the floor is -60 dB');
  assert.equal(meterScale(-9000), 0, 'below the floor clamps rather than going negative');
  assert.equal(meterScale(600), 1, 'and above full scale clamps rather than overflowing');
  /*
   * SILENT IS NOT A LOW VALUE. It is "there is nothing here" — an instrument has
   * no audio input and says so forever — and it happens to land in the same
   * place a very quiet signal does. They must not be computed the same way: the
   * arithmetic on i16::MIN would give a large negative fraction, and a bar drawn
   * from it is a bar whose width depends on a sentinel.
   */
  assert.equal(meterScale(METER_SILENT), 0, 'silence is the bottom, not a computation');
  assert.ok(meterScale(-100) > 0.9 && meterScale(-100) < 1, 'just under full scale is near the top');
});

/*
 * NO KEY IN `__uni` IS DECLARED TWICE.
 *
 * A duplicate key in an object literal is legal JavaScript and silent: the last
 * one wins, no warning, no error. `__uni` is 150 entries long and is the test
 * surface, the agent's surface and the console's surface at once — so a name
 * collision does not break the new entry, it deletes an OLD one, and the failure
 * lands somewhere unrelated.
 *
 * Which is exactly what happened: a `meters` added for per-insert levels replaced
 * the `meters` that reported the song's time signature, and the e2e died four
 * sections earlier reading `.song` off the wrong shape. The parse is crude —
 * top-level `name:` at a fixed indent inside the literal — and crude is fine,
 * because the guard below catches it going blind.
 */
/*
 * EVERY `api.X` A COMMAND CALLS EXISTS ON THE REAL dockApi.
 *
 * The registry test above checks that a command is DECLARED. It cannot check that
 * the method behind it is there, because it runs against a stub that answers to
 * everything — so a command wired to a function that only exists on `window.__uni`
 * passes every test in this file and throws the moment a person types it.
 *
 * That is not hypothetical. dockApi's own comment records `add-track`,
 * `remove-track` and `columns` doing exactly this. Writing that comment did not
 * stop it: `bypass` and `quantize` both landed on `__uni` first, and `bypass`
 * shipped that way — with a passing e2e, because the suite exercised the button
 * and the key and never the console.
 *
 * Source-read on both sides, because the two objects are in different files and
 * nothing else compares them.
 */
/*
 * THE PAGE'S MODULE SCRIPT PARSES.
 *
 * The cheapest possible check, and it exists because a syntax error in index.html
 * presents as a THIRTY-SECOND PLAYWRIGHT TIMEOUT waiting for `window.__uni`, with
 * no mention of syntax anywhere. I duplicated a function declaration and spent the
 * next few minutes reading a timeout stack.
 *
 * `--check` is not available in-process, so the parse is attempted by constructing
 * an AsyncFunction over the source: same parser, same errors, no execution. A
 * duplicate `function` or `const` at top level fails here in milliseconds and names
 * the identifier.
 */
test('the page script parses', async () => {
  const { readFileSync } = await import('node:fs');
  const html = readFileSync(new URL('../index.html', import.meta.url), 'utf8');
  const m = html.match(/<script type="module">([\s\S]*)<\/script>/);
  assert.ok(m, 'the module script was found');
  const src = m[1];
  assert.ok(src.length > 100000, `and it is the whole thing: ${src.length} bytes`);
  // Wrapped so `import` statements do not have to resolve — the question is
  // whether the SOURCE is syntactically valid, not whether its deps are present.
  const body = src.replace(/^\s*import\s[^;]*;/gm, '');
  const AsyncFunction = Object.getPrototypeOf(async function () {}).constructor;
  try {
    new AsyncFunction(body);
  } catch (e) {
    assert.fail(`index.html does not parse: ${e.message}`);
  }
});

test('every api method a console command calls actually exists', async () => {
  const { readFileSync } = await import('node:fs');
  const html = readFileSync(new URL('../index.html', import.meta.url), 'utf8');
  const dockSrc = readFileSync(new URL('../src/dock.js', import.meta.url), 'utf8');

  const at = html.indexOf('const dockApi = {');
  assert.ok(at > 0, 'dockApi was found');
  const body = html.slice(at, html.indexOf('\n};', at));
  const have = new Set();
  // `[:(,]` and end-of-line, because a SHORTHAND property (`delDevice,`) is a
  // method too — a pattern that only matched `name:` would report every shorthand
  // as missing, and a check that cries wolf is a check that gets deleted.
  for (const m of body.matchAll(/^  ([A-Za-z_$][\w$]*)\s*(?:[:(,]|$)/gm)) have.add(m[1]);
  assert.ok(have.size > 30, `dockApi was actually parsed: ${have.size} methods`);

  const called = new Set();
  for (const m of dockSrc.matchAll(/\bapi\.([A-Za-z_$][\w$]*)/g)) called.add(m[1]);
  assert.ok(called.size > 30, `dock.js was actually parsed: ${called.size} calls`);

  const missing = [...called].filter((n) => !have.has(n)).sort();
  assert.deepEqual(missing, [],
    'a console command calls an api method that dockApi does not have — it will throw');
});

test('the __uni surface declares no name twice', async () => {
  const { readFileSync } = await import('node:fs');
  const src = readFileSync(new URL('../index.html', import.meta.url), 'utf8');
  const at = src.indexOf('window.__uni = {');
  assert.ok(at > 0, 'the surface was found at all');
  const body = src.slice(at);
  const end = body.indexOf('\n};');
  assert.ok(end > 0, 'and its end');
  const seen = new Map();
  const dupes = [];
  for (const m of body.slice(0, end).matchAll(/^  ([A-Za-z_$][\w$]*):/gm)) {
    if (seen.has(m[1])) dupes.push(m[1]);
    seen.set(m[1], true);
  }
  // If this stops matching, the whole test quietly becomes a no-op — the failure
  // mode every source-reading check has to guard against.
  assert.ok(seen.size > 60, `the surface was actually parsed: ${seen.size} entries`);
  assert.deepEqual(dupes, [], 'a name declared twice — the earlier one is silently gone');
});

/**
 * EVERY dockApi METHOD IS ALSO ON `__uni`.
 *
 * WHY THIS IS A TEST AND NOT A CONVENTION. Six times in one session a test or a probe reached
 * for `__uni.X` and got "is not a function", because X had been added to `dockApi` alone:
 * `bypass`, `quantize`, `rowTicks`, `deleteHarmony`, `ticksPerBar`, `setView`. Each cost a full
 * suite run to find, and each was one line to fix. The existing ratchet checks the other
 * direction — that every `api.X` the console calls exists on dockApi — and nothing checked this
 * one, so the sixth was as expensive as the first.
 *
 * The two surfaces are not the same thing and should not be: `dockApi` is what the console
 * calls and `__uni` is what a test or an agent drives. But `__uni` being a SUBSET is never
 * deliberate — it is always an omission, because anything worth doing from the console is worth
 * driving from a test. So the invariant is one-directional: dockApi ⊆ __uni, and a genuine
 * console-only entry is listed below with its reason.
 */
const CONSOLE_ONLY = new Set([
  // The dock's own machinery. `print` and `clear` act on the LOG, which a test reads by other
  // means and an agent has no business writing to.
  'print', 'clear', 'close',
]);

test('every dockApi method is also reachable on __uni', async () => {
  const { readFileSync } = await import('node:fs');
  const html = readFileSync(new URL('../index.html', import.meta.url), 'utf8');

  const keysOf = (marker) => {
    const at = html.indexOf(marker);
    assert.ok(at > 0, `${marker} was found`);
    const body = html.slice(at, html.indexOf('\n};', at));
    const out = new Set();
    // Same pattern as the ratchet above: `[:(,]` or end of line, so a shorthand property is a
    // method too.
    for (const m of body.matchAll(/^  ([A-Za-z_$][\w$]*)\s*(?:[:(,]|$)/gm)) out.add(m[1]);
    return out;
  };
  const dock = keysOf('const dockApi = {');
  const uni = keysOf('window.__uni = {');
  // Both parsed, or the whole test is a no-op — the failure mode every source-reading check has
  // to guard against, and the reason each of them says so out loud.
  assert.ok(dock.size > 30, `dockApi was parsed: ${dock.size}`);
  assert.ok(uni.size > 60, `__uni was parsed: ${uni.size}`);

  const missing = [...dock].filter((n) => !uni.has(n) && !CONSOLE_ONLY.has(n)).sort();
  assert.deepEqual(missing, [],
    'on dockApi and not on __uni — a test or an agent reaching for it gets '
    + '"is not a function". Delegate it (`X: (a) => dockApi.X(a)`) or list it in CONSOLE_ONLY '
    + 'with a reason.');
});

test('every key the app handles is reachable another way', async () => {
  const { readFileSync } = await import('node:fs');
  const src = readFileSync(new URL('../index.html', import.meta.url), 'utf8');

  const keys = new Set();
  for (const m of src.matchAll(/\bk === '((?:[^'\\]|\\.)+)'/g)) keys.add(m[1]);
  const alts = new Set();
  for (const m of src.matchAll(/\balt === '([^']+)'/g)) alts.add(m[1]);

  // If this ever reads zero the regex has stopped matching the source and the
  // whole test has quietly become a no-op — the failure mode a source-reading
  // check has to guard against.
  assert.ok(keys.size > 20, `the handler was actually parsed: ${keys.size} keys`);
  assert.ok(alts.size > 3, `and its alt combos: ${alts.size}`);

  const unlisted = [...keys].filter((k) => !(k in KEY_OPS));
  assert.deepEqual(unlisted, [],
    'a key with no command and no recorded reason — give it one of the two');
  const unlistedAlt = [...alts].filter((k) => !(k in ALT_OPS));
  assert.deepEqual(unlistedAlt, [], 'same for an alt combo');
});

test('every command a key points at actually exists', () => {
  // The other direction: the table above is only worth having if the names in it
  // are real. A typo would otherwise record an action as covered by a command
  // that does not exist, which is worse than recording it as uncovered.
  const cmds = createCommands(stubApi());
  for (const [key, op] of Object.entries(KEY_OPS)) {
    if (op.cmd) assert.ok(cmds[op.cmd], `${JSON.stringify(key)} -> ${op.cmd} exists`);
  }
  for (const [key, op] of Object.entries(ALT_OPS)) {
    if (op.cmd) assert.ok(cmds[op.cmd], `Alt+${key} -> ${op.cmd} exists`);
  }
});

// ---------------------------------------------------------------------------
// ONE NAMESPACE, THREE SURFACES.
//
// ARCHITECTURE_REVIEW section 4 item 2: every operation reachable from a
// keystroke, the palette, `daw-cli` and the agent — "one namespace, one op
// registry, and a build-time assertion that every registered op has a palette
// entry and a CLI path... it is simultaneously the AI's entire API."
//
// There is no single registry today; there are three lists that disagree, and
// nothing noticed:
//
//   dock + palette   36 commands   ui-web/src/dock.js
//   daw-cli do       11 verbs      ui/daw-cli/src/main.rs
//   agent manifest    8 tools      ui/daw-agent/src/tools.rs
//
// This table IS the namespace, until something generates the three from one
// source. It is deliberately explicit: every dock command must appear here with
// either the name it goes by on each surface or a recorded reason it has none.
// Adding a command without deciding fails the first assertion below.
//
// The two coverage assertions are RATCHETS rather than a wall. The gap is real
// and pre-existing; a test that simply fails is one nobody can run. These say the
// gap may not GROW, and may not silently shrink either — closing one without
// updating the list fails too, so the list cannot rot into a lie.

/**
 * `cli` and `agent` are the name that operation goes by on that surface, or null
 * with a `why`:
 *   'view'    — view state. An agent driving a headless engine has no viewport,
 *               so there is nothing for it to address. Not a gap.
 *   'ask'     — introspection the other surfaces answer differently (the agent
 *               has `observe`, the CLI has `get`). Not a gap.
 *   'gap'     — a DOCUMENT operation with no programmatic path. A real hole:
 *               an agent that can write a note but cannot undo it is half-built.
 */
const OP_REGISTRY = {
  // Covered on all three.
  load:      { cli: 'load',        agent: 'load' },
  // New this session, all three still owed a programmatic path.
  // `new` was a browser feature until 2026-08-06: built as a sidecar websocket message because
  // the BROWSER cannot write files, which the two surfaces that CAN write files then inherited
  // for no reason. daw_bridge::project owns the document; all three call it.
  new:       { cli: 'new', agent: 'new_project' },
  deldevice: { cli: 'remove-device', agent: 'remove_device', why: 'gap' },
  // Bypass reached the ENGINE from daw-cli first (`do set-bypass`, backend's
  // verb) and this app second, so the CLI path is real and the agent's manifest
  // is what still owes it a tool.
  bypass:    { cli: 'set-bypass', agent: 'set_bypass' },
  // Reordering reached the engine from this app FIRST — daw-cli has no verb for it —
  // which is the opposite of the usual direction and worth recording as such.
  movedevice: { cli: 'move-device', agent: 'move_device', why: 'gap' },
  // Chords reached the CLI first (`do chord`) and this app's console never had them at
  // all — writing one meant typing a token into a cell, and removing one was impossible.
  // The agent grew `add_chords` — a degree/quality/inversion write with a strum, not a
  // pile of simultaneous notes. Both surfaces now, so no `why` and no gap entry.
  chord:     { cli: 'chord', agent: 'add_chords' },
  delchord:  { cli: 'chord',    agent: 'delete_chord' },
  // The engine has taken DeleteHarmony since before this UI existed and nothing sent it,
  // so a key change could be added to the timeline and never taken off.
  delharmony: { cli: 'harmony',   agent: 'delete_harmony' },
  // M1.13. daw-cli shipped `do quantize` with the engine, so the CLI path is real
  // from day one; the agent's manifest still owes it a tool.
  quantize:  { cli: 'quantize', agent: 'set_lane_quantize' },
  /*
   * THE SPINE. The CLI verb is one `do section <sub>` for the five edits and
   * `get arrangement` for the read, so all six point at a real path there. The agent
   * still owes a tool — recorded as a gap with the rest of that list, not waved through.
   */
  /*
   * MODULATION. daw-cli has `do mod` for the links; the READ has no CLI verb, which is
   * recorded rather than papered over — `mods` is this app's only way to see what moves
   * what, and that is a real gap in the CLI's coverage rather than an omission here.
   */
  /*
   * READING what modulates what has no CLI verb and no agent tool, and the reason is
   * structural rather than an omission: modulation is published as DIFFS on the engine's
   * outbound ring, which is SINGLE CONSUMER. The sidecar's thread drains it and accumulates
   * the links, so the console and the UI can read them — and an agent tool executing inside
   * daw-agent cannot, because taking entries off that ring would steal them from every
   * browser tab. It needs either a published region or the sidecar's store plumbed through
   * `session.execute`. Recorded rather than papered over: an agent CAN modulate and cannot
   * yet check its own work.
   */
  autopoint:   { cli: 'automation', agent: 'write_automation_point', why: 'gap' },
  automation:  { cli: 'automation', agent: 'automation' },
  curve:       { cli: 'automation-points', agent: 'automation_points', why: 'gap' },
  mods:        { cli: null, agent: null, why: 'gap' },
  // `depth` is `mod-link` again on the CLI. The agent reaches it through `modulate`, which
  // takes a depth — there is no separate opcode to give it a tool of its own.
  map:         { cli: 'mod-link', agent: 'modulate' },
  unmap:       { cli: 'unmod-link', agent: 'unmodulate' },
  // `depth` is `mod-link` again on the CLI. The agent reaches it through `modulate`, which
  // takes a depth — there is no separate opcode to give it a tool of its own.
  depth:       { cli: 'mod-link', agent: 'modulate' },
  /*
   * NO CLI VERB FOR THE MACRO KNOB. `mod-target` names a parameter and `mod-link` makes a
   * link, and nothing there turns a source — which means a link made from daw-cli cannot be
   * heard from daw-cli, because a macro nobody has turned is skipped by the applier.
   * Recorded as a gap on their side rather than left looking covered.
   */
  macro:       { cli: 'macro', agent: 'set_macro', why: 'gap' },
  // SCRATCH CLIPS. The read has no CLI verb — daw-cli can fork and swap but cannot say what is
  // shared, which is the half a person needs first. Recorded rather than claimed.
  shared:      { cli: 'shared', agent: 'shared_clips' },
  fork:        { cli: 'scratch', agent: 'fork_placement' },
  swapclip:    { cli: 'scratch', agent: 'swap_placement_clip' },
  keepclip:    { cli: 'scratch', agent: 'keep_placement_clip' },
  markers:     { cli: 'arrangement', agent: 'markers' },
  /*
   * All four marker edits are one agent tool with an `op`, because their arguments differ and a
   * model calls one named op more reliably than it picks between four near synonyms. The CLI
   * made the same choice — `do marker <sub>`.
   */
  marker:      { cli: 'marker', agent: 'edit_marker' },
  delmarker:   { cli: 'marker', agent: 'edit_marker' },
  namemarker:  { cli: 'marker', agent: 'edit_marker' },
  // A marker's colour was write-once everywhere, so no surface owed a verb until now.
  colormarker: { cli: 'marker', agent: 'edit_marker', why: 'gap' },
  // The clip's name and source path. Reached the engine and this console together;
  // neither the CLI nor the agent has a verb for either yet.
  cliptext:    { cli: 'clip-name', agent: 'set_clip_text' },
  // The envelope READ-BACK. The write half has a CLI verb; reading one back is new
  // everywhere, so both the CLI and the agent owe it one.
  envshape:    { cli: 'sampler-env-draw', agent: 'sampler_envelope_points' },
  movemarker:  { cli: 'marker', agent: 'edit_marker' },
  // The two that change TIME rather than a label, and they are deliberately not marker ops.
  time:        { cli: 'time', agent: 'insert_time' },
  timesig:     { cli: 'time-sig', agent: 'set_time_signature' },
  editor:    { cli: 'open-editor', agent: null, why: 'gap' },
  // v22 (AddTrack=46/RemoveTrack=47). daw-cli shipped its verbs in the same
  // commit the engine did, so these are covered on the CLI from day one; the
  // agent manifest still owes them.
  // The tracker's note-column count. No CLI or agent path yet: it is a VIEW
  // setting today, not a document edit, so an agent scripting a song does not
  // need it — but the moment column assignment becomes part of writing a chord,
  // it does. Recorded rather than waved through.
  columns:        { cli: null,           agent: null, why: 'gap' },
  // Harmony had no programmatic path at all until now — readable from every
  // surface, writable from none. daw-cli has had `do harmony` the whole time.
  harmony:        { cli: 'harmony',       agent: 'set_harmony' },
  'add-track':    { cli: 'add-track',    agent: 'add_track' },
  'remove-track': { cli: 'remove-track', agent: 'remove_track' },
  save:      { cli: 'save',        agent: 'save' },
  note:      { cli: 'note',        agent: 'add_notes' },
  play:      { cli: 'play',        agent: 'transport' },
  // Covered on two.
  del:       { cli: 'delete-note', agent: null, why: 'gap' },
  tempo:     { cli: 'set-tempo',   agent: 'set_tempo' },
  // set_mixer takes gain_db, pan, mute and solo — these three were recorded as gaps while
  // the tool that does them was already in the manifest. A stale gap entry is read as a
  // finding by whoever picks it up, which is work spent on a job already done.
  gain:      { cli: 'mixer',       agent: 'set_mixer' },
  mute:      { cli: 'mixer',       agent: 'set_mixer' },
  solo:      { cli: 'mixer',       agent: 'set_mixer' },
  undo:      { cli: 'undo',          agent: 'undo',           why: 'gap' },
  redo:      { cli: 'redo',          agent: 'redo',           why: 'gap' },
  rename:    { cli: 'rename',          agent: 'set_track_name', why: 'gap' },
  stop:      { cli: 'stop',          agent: 'transport',      why: 'gap' },
  // Document operations with NO programmatic path at all. The real hole.
  clear:     { cli: null, agent: null, why: 'gap' },
  copy:      { cli: null, agent: null, why: 'gap' },
  cut:       { cli: null, agent: null, why: 'gap' },
  paste:     { cli: null, agent: null, why: 'gap' },
  transpose: { cli: null, agent: null, why: 'gap' },
  loop:      { cli: 'loop', agent: 'set_loop', why: 'gap' },
  // `transport` with action=seek IS this op; it was recorded as having no tool at all.
  seek:      { cli: 'position', agent: 'transport', why: 'gap' },
  // One tool, three actions — add, link and remove all address ONE patcher device,
  // which is the thing that makes them land in the graph a project saves.
  addnode:   { cli: 'patcher-node', agent: 'patcher_node', why: 'gap' },
  delnode:   { cli: 'patcher-unnode', agent: 'patcher_node', why: 'gap' },
  link:      { cli: 'patcher-connect', agent: 'patcher_node', why: 'gap' },
  // The agent could ADD a euclidean node and then not tell it how many steps to take, which is
  // the same shape as the link bug: the model does the right thing, the capability is absent, and
  // it reads as the model giving up halfway. `patcher_config` takes named fields and builds on
  // the node's CURRENT config, because the wire carries all eight values every time.
  patch:     { cli: 'patcher-config', agent: 'patcher_config' },
  // View state. An agent has no viewport to address.
  // Edit mode is a property of the KEYBOARD, and an agent has no keyboard — it
  // calls add_notes, which writes regardless. So this is view state for the same
  // reason zoom is, even though it is a command a human wants in the palette.
  /*
   * `draw` is the automation curve taking the pointer. VIEW state, and genuinely so rather than
   * conveniently: it changes what a click does to a surface, and an agent driving a headless
   * engine has no click. The capability underneath it — writing a point — is not view state and
   * does have a programmatic path, which is `write_automation_point`.
   */
  draw:      { cli: null, agent: null, why: 'view' },
  // The sampler read-back. Reaches the engine from this app; daw-cli has no verb for it and the
  // agent manifest still owes it a tool, so both are recorded rather than waved through.
  kit:       { cli: 'sampler-kit', agent: 'sampler_kit', why: 'gap' },
  // The sampler, reachable at last. daw-cli had both from the day the opcodes landed, so the
  // CLI paths are real and only the agent manifest owes them tools.
  // The agent's device-kind list was three of the engine's six and the sampler was not
  // among them, so it could not make the one instrument this app implements itself.
  sampler:   { cli: 'add-device', agent: 'add_device' },
  'load-sample': { cli: 'sampler-load', agent: 'load_sample' },
  slice:     { cli: 'sampler-slice', agent: 'sampler_slice' },
  // Row ops. daw-cli reached the engine FIRST here — `do set-row-ops` shipped with the opcode —
  // so the CLI path is real from day one and only the agent manifest owes it a tool.
  ops:       { cli: 'set-row-ops', agent: 'set_row_ops' },
  /*
   * `op` sets ONE row op and leaves the other four alone. It shares the CLI's set-row-ops path —
   * the difference is the mask, not the command — so it is not a gap in coverage even though the
   * CLI has no separate verb for it. Named here rather than folded into `ops` because the two
   * mean different things to the engine: one is a replacement of the row, the other is a single
   * bit, and two clients editing different ops on one row only survive the second.
   */
  // `op` sets ONE op and `ops` sets a line of them; both are one SetRowOps with a different
  // mask, so both map to the same verb and the same tool. Many-to-one, recorded as such.
  op:        { cli: 'set-row-ops', agent: 'set_row_ops' },
  /*
   * The filter (opcode 86). No CLI verb yet — it landed engine-side this morning and daw-cli has
   * not caught up — so it is recorded as a gap rather than claimed as covered. Recording it as
   * covered would be worse than recording it as missing.
   */
  filter:    { cli: 'sampler-filter', agent: 'sampler_filter' },
  /*
   * The envelope (opcode 82). daw-cli has `sampler-env` — it is what proved a loaded slot is
   * silent without one — so the CLI path is real; the agent has no sampler tooling at all.
   */
  env:       { cli: 'sampler-env', agent: 'sampler_envelope' },
  /*
   * One slot field (SamplerSetSlot). daw-cli has `sampler-slot`, so the CLI path is real; the
   * agent has no sampler tooling at all.
   */
  slot:      { cli: 'sampler-slot', agent: 'sampler_slot' },
  // Naming a pad. The CLI shipped with opcode 90; the agent has no sampler tooling at all, which
  // is the same recorded gap every other sampler verb carries.
  'slot-name': { cli: 'sampler-slot-name', agent: 'sampler_slot_name' },
  // Vintage (opcode 91). With the other sampler verbs: the agent has no sampler tooling.
  vintage:   { cli: 'sampler-vintage', agent: 'sampler_vintage' },
  /*
   * Chromatic mode (SetTrackSoundAddressed, 87). Landed engine-side this morning; daw-cli has
   * not caught up, and the agent has no sampler tooling at all.
   */
  soundaddr: { cli: 'sound-addressed', agent: 'set_row_ops', why: 'gap' },
  /*
   * The kit's own settings (SamplerSetDevice, 88). daw-cli has `sampler-device`, so the CLI path
   * is real; the agent has no sampler tooling at all.
   */
  bank:      { cli: 'sampler-device', agent: 'sampler_device' },
  /*
   * Lay a chop out as notes (SamplerEmitRows). daw-cli has `sampler-emit-rows`, so the CLI path
   * is real; the agent has no sampler tooling at all.
   */
  emit:      { cli: 'sampler-emit-rows', agent: 'sampler_emit_rows' },
  edit:      { cli: null, agent: null, why: 'view' },
  fold:      { cli: null, agent: null, why: 'view' },
  // The engine has taken this since before the web UI existed; the CLI verb is `harmony-quantize`.
  'harmony-quantize': { cli: 'harmony-quantize', agent: 'harmony_quantize' },
  // Saving a patcher graph. With the other patcher verbs: no agent tool for the graph at all.
  'save-patch': { cli: 'patcher-save', agent: 'save_patcher_preset' },
  // A lane's subdivision (opcode 92). Landed engine-side today; no CLI verb yet.
  lpb: { cli: 'lines-per-beat', agent: 'set_track_grid' },
  // The master bus fader and mute (SetTrackMixer addressed to kMasterTrackId). The engine has
  // honoured both on the summed output since the master track landed; nothing could move them.
  'main-gain': { cli: 'mixer', agent: 'set_mixer', why: 'gap' },
  'main-mute': { cli: 'mixer', agent: 'set_mixer', why: 'gap' },
  // A CLIP's own subdivision and meter (opcode 94) — the level the renderer honours FIRST.
  // Backend shipped `daw-cli do clip-grid` with it, so this one has a CLI path from the start.
  'clip-grid': { cli: 'clip-grid', agent: 'set_clip_grid' },
  // An audio clip's gain, fades and in-point (opcode 95). Backend shipped `daw-cli do
  // audio-clip` with it, so this has a CLI path from the start.
  'audio-clip': { cli: 'audio-clip', agent: 'set_audio_clip' },
  // Whether a drag in the piano roll sets velocity. A VIEW decision like `draw`, which it
  // mirrors — the mode changes what a gesture means here and nothing about the document.
  'vel-edit': { cli: null, agent: null, why: 'view' },
  // Remove one automation point (opcode 96). The lane was create-and-adjust-only until it.
  'del-point': { cli: 'delete-automation', agent: 'delete_automation_point' },
  // Whether note entry cuts the sounding note (opcode 93). Landed engine-side today.
  'note-overlap': { cli: 'note-overlap', agent: 'set_track_grid' },
  // Which columns this window draws. Genuinely a view decision — the engine already remembers
  // the only half that outlives the tab, which is whether the ops are THERE.
  'ops-column': { cli: null, agent: null, why: 'view' },
  follow:    { cli: null, agent: null, why: 'view' },
  goto:      { cli: null, agent: null, why: 'view' },
  oct:       { cli: null, agent: null, why: 'view' },
  select:    { cli: null, agent: null, why: 'view' },
  view:      { cli: null, agent: null, why: 'view' },
  zoom:      { cli: null, agent: null, why: 'view' },
  /*
   * PLACEMENTS. Where a clip sits, as opposed to what is in it — the first
   * DOCUMENT operations to ship on all three surfaces on the day they landed.
   *
   * They are here because "mouse-only means unnameable means unscriptable" is
   * the rule, and an arrangement you can only edit by dragging is an arrangement
   * no test and no agent can build. The drag and these call the same functions.
   */
  clips:        { cli: 'extents',          agent: 'clips',           why: 'gap' },
  'move-clip':  { cli: 'move-placement',          agent: 'move_clip',       why: 'gap' },
  'trim-clip':  { cli: 'resize-placement',          agent: 'trim_clip',       why: 'gap' },
  'del-clip':   { cli: 'remove-placement',          agent: 'remove_clip',     why: 'gap' },
  'add-clip':   { cli: 'add-placement',          agent: 'add_clip',        why: 'gap' },
  // Which chain the rack shows. View state — it changes what you are looking at
  // and nothing about the song — but unlike zoom it is the ONLY route to the
  // master's chain, so it is listed rather than waved through.
  master:    { cli: null, agent: null, why: 'view' },
  // The agent's conversation, not the song. There is nothing for the CLI to do
  // with it (each invocation is already a fresh conversation) and giving the
  // agent a tool to clear its own memory is asking it to decide when to forget
  // what it was told, which is the person's call.
  forget:    { cli: null, agent: null, why: 'view' },
  // Introspection, answered differently on each surface.
  engine:    { cli: null, agent: null, why: 'ask' },
  help:      { cli: null, agent: null, why: 'ask' },
  nodes:     { cli: null, agent: null, why: 'ask' },
  projects:  { cli: null, agent: null, why: 'ask' },
  state:     { cli: null, agent: null, why: 'ask' },
};

/**
 * Ops with no CLI path today. This list may SHRINK, never grow.
 *
 * IT WAS 44 AND MOST OF THAT WAS A NAMING MISMATCH. The registry pairs a console command with a
 * CLI verb BY NAME, and wherever daw-cli chose a different word the row was recorded as "no CLI
 * path" — so the gap counted vocabulary, not capability. Twenty-seven of them were already
 * implemented:
 *
 *     addnode -> patcher-node      link      -> patcher-connect   patch     -> patcher-config
 *     delnode -> patcher-unnode    editor    -> open-editor       deldevice -> remove-device
 *     add-clip -> add-placement    del-clip  -> remove-placement  move-clip -> move-placement
 *     trim-clip -> resize-placement                               movedevice -> move-device
 *     lpb     -> lines-per-beat    cliptext  -> clip-name         kit       -> sampler-kit
 *     filter  -> sampler-filter    soundaddr -> sound-addressed   envshape  -> sampler-env-draw
 *     save-patch -> patcher-save   del-point -> delete-automation curve     -> automation-points
 *     stop, undo, redo, rename, loop, macro, note-overlap — same name, simply never mapped
 *
 * This is not re-labelling a gap as exempt: the capability EXISTS and the record was wrong. The
 * check below reads daw-cli's own source, so a claimed verb that is not there fails — which is
 * what makes correcting the map safe rather than a way of moving numbers.
 *
 * What is left divides into three kinds:
 *   - CURSOR AND SELECTION: clear, columns, copy, cut, paste, transpose, new. These act on the
 *     cursor or the selection, which a CLI invocation does not have.
 *   - GENUINELY MISSING: delchord, delharmony, main-gain, main-mute, colormarker, mods, autopoint,
 *     shared, seek, clips. Real verbs the CLI owes.
 * `seek` and `clips` were on that "genuinely missing" list until I READ the two candidates rather
 * than assuming: `do position` sends SetPosition, which is what seek is, and `get extents` is the
 * placement list. `get transport` is a READ and is NOT seek, which is the kind of near-miss that
 * makes a guessed mapping worse than an admitted gap.
 */
/*
 * AUDITED 2026-08-06, and two of these do not hold up as "exempt". Kept on the list with the
 * doubt written down rather than quietly reclassified — the point of the list is that a gap
 * stays visible until somebody decides.
 *
 * FAIRLY EXEMPT: `clear`, `copy`, `cut`, `paste`, `transpose` operate on the CURSOR and the
 * SELECTION, which is view state a CLI does not have and should not simulate. `columns` is a
 * display preference — how many note columns are drawn — and the owner's rule exempts view-only
 * ops explicitly. `mods` is explained on its own row: the READ has no shape a command can return.
 *
 * NOT OBVIOUSLY EXEMPT, and both are worth a decision:
 *
 *   `new` — creating a song is a DOCUMENT operation, not a view one, and it is missing from the
 *   CLI *and* the agent. An agent asked to start a new song cannot, so it works in whatever is
 *   already open. That is not hypothetical: `new NAME` failing to set the current project cost
 *   the owner a shipped preset tonight, and an agent has no way to say "start clean" at all. The
 *   sidecar implements it (`{"type":"new"}`) because a browser cannot write a file; daw-cli and
 *   the agent talk to the engine, which has no such command — so this needs an owner ruling on
 *   WHERE it should live before anyone builds it.
 *
 *   `shared` — the agent HAS `shared_clips`; only the CLI lacks it. That is a plain missing verb
 *   with a known shape, not a design question, and it is the cheapest thing on this list.
 *
 * ── RE-AUDITED 2026-08-07, after both of those turned out to be buildable ────────────────────
 *
 * `shared` and `new` are BUILT and DRIVEN, and the second one is the lesson: I had written that
 * `new` "needs an owner ruling on WHERE it should live". It did not. The sidecar owns it because
 * a BROWSER cannot write a file — and daw-cli and the agent are native processes that can. The
 * ruling I was waiting for was a limitation of one surface mistaken for a property of the op.
 *
 * So the remaining rows were re-derived rather than re-read, and they now divide into two kinds
 * which are NOT the same and should not sit in one list:
 *
 *   VIEW-ONLY, which is the owner's own exemption:
 *     `clear`   — clears the console LOG. There is no log on the other surfaces.
 *     `columns` — how many note columns the tracker DRAWS. It sends nothing to the engine, and
 *                 the count is DERIVED from the highest column any note uses, floored by what
 *                 was asked for. So a project saved with notes in column 1 comes back showing
 *                 two columns on its own; there is no state to carry and nothing to lose. I went
 *                 looking for an invisible-notes bug here and the derivation had already closed
 *                 it. The CAPABILITY behind it is not exempt and is not missing: `add_notes`
 *                 takes a `column`, which is how an agent writes the overlapping pair a mono
 *                 synth reads as legato.
 *     `copy`, `cut`, `paste`, `transpose`, `del` — these act on the SELECTION or the CURSOR.
 *                 Worth revisiting as range ops (`transpose --from --to`) rather than treated as
 *                 settled: `transposeSelection` is a BATCH of ordinary note writes, so nothing
 *                 about it needs a viewport. Recorded as a candidate, not as exempt-for-ever.
 *     `editor`  — opens a plugin's own GUI window. daw-cli has it; a headless agent asking for a
 *                 window on somebody's screen is a different thing from the other rows here.
 *
 *   BLOCKED ON THE ENGINE, which is not an exemption at all:
 *     `mods`    — the READ. Both surfaces can already CREATE modulation (daw-cli `mod-link`,
 *                 `mod-depth`, `mod-target`; the agent `modulate`/`unmodulate`) and neither can
 *                 say what modulation exists — which is the question you ask BEFORE changing
 *                 anything, exactly as `shared` was for clips.
 *
 *                 It is not a missing verb. Modulation links reach the browser only as CHAIN
 *                 DIFFS on the single-consumer ring, and there is no published region for them:
 *                 `control.rs` has read_mixer, read_patcher, read_device_params, read_automation
 *                 and seventeen more, and no read_mod_links. daw-cli cannot drain that ring
 *                 without stealing the diffs from the sidecar and breaking the UI.
 *
 *                 So this needs a published region or a request/response mirror like
 *                 RequestDeviceParams — an engine change, and backend's call. Raised with them.
 *                 Recorded here so it stays visible as a HOLE rather than passing as exempt.
 */
const CLI_GAP = ['clear', 'columns', 'copy', 'cut', 'paste', 'transpose',
                 'mods'];
/** Ops with no agent tool today. Same rule. */
// `bypass` joins the list rather than being smuggled past it: the engine takes
// the command and daw-cli sends it, but the agent's manifest has no tool for it,
// so an agent asked to A/B an insert still cannot. Worth closing — comparing with
// and without a device is exactly the kind of judgement an agent should be able
// to make on its own — and worth recording honestly until it is.
/*
 * Ops with no agent tool. SHORTER THAN IT WAS: sections, modulation, the device rack and lane
 * quantize all have tools now, which is what took eleven names off this list.
 *
 * What is left divides into three kinds, and only the first is a real gap:
 *
 *  - REACHABLE AND UNGIVEN: `chord`, `delchord`, `delharmony`, `addnode`, `delnode`, `link`,
 *    `patch`, `editor`. Every one is a command the engine takes and the agent has no way to
 *    send. These are the next batch.
 *  - COVERED UNDER ANOTHER NAME: `gain`, `mute`, `solo` are `set_mixer`; `loop` is `set_loop`;
 *    `tempo` is `set_tempo`; `seek` is `transport`. Left here because the registry maps
 *    CONSOLE commands to tools one-to-one and these are many-to-one, which is worth seeing as
 *    an untidy row rather than hiding behind a name that implies a tool of its own.
 *  - NOT THE AGENT'S BUSINESS: `clear`, `columns`, `copy`, `cut`, `paste`, `del`, `transpose`,
 *    `new`. These operate on the CURSOR and the SELECTION — view state the agent does not
 *    have and should not simulate. An agent edits by naming notes, which `add_notes` and
 *    `delete_note` already do.
 *
 * `mods` is its own case and is explained on its registry row: the read is structurally
 * unavailable to a tool, not merely unwritten.
 */
/*
 * TEN NAMES CAME OFF THIS LIST BY BUILDING THE TOOLS, and every one of them is covered by an
 * engine test that asserts the SAVED PROJECT rather than the tool's own reply — because on this
 * wire the default failure is a well-formed no-op, and three of these ten failed exactly that way
 * the first time they ran:
 *
 *   delchord, delharmony -> delete_chord, delete_harmony      cliptext     -> set_clip_text
 *   lpb, note-overlap    -> set_track_grid                    clip-grid    -> set_clip_grid
 *   audio-clip           -> set_audio_clip                    del-point    -> delete_automation_point
 *   filter               -> sampler_filter                    slot-name    -> sampler_slot_name
 *
 * What is LEFT divides into three kinds, and only the last is a real gap:
 *
 *  - NOT THE AGENT'S BUSINESS: `clear`, `columns`, `copy`, `cut`, `paste`, `del`, `transpose`,
 *    `new`. These operate on the CURSOR and the SELECTION — view state an agent does not have and
 *    should not simulate. An agent edits by naming notes, which `add_notes` and `delete_note` do.
 *  - STRUCTURALLY UNAVAILABLE: `mods`, explained on its registry row — the READ has no shape a
 *    tool can return.
 *  - REACHABLE AND UNGIVEN, the honest remainder: `patch` (patcher-config), `save-patch`,
 *    `editor` (open-editor), `envshape` (the envelope draw).
 *
 * `patch` is deliberately not built rather than merely unbuilt: it is a per-node-type byte layout
 * that already has three hand-rolled copies, and a fourth belongs behind a shared packer, not in
 * a tool. `editor` opens a window the engine owns, so the only assertion available is the absence
 * of a refusal line in the engine log — `cli-verbs.mjs` makes exactly that assertion for the CLI
 * verb, and an agent tool would add a caller without adding evidence.
 */
const AGENT_GAP = ['clear', 'columns', 'copy', 'cut',
                   'del', 'editor',
                   'paste',
                   'transpose', 'mods'];

test('every dock command is in the op registry', () => {
  // The forcing function: a new command cannot be added without deciding whether
  // it needs a CLI path and an agent tool. That decision is the whole point —
  // "mouse-only means unnameable means unscriptable", and the same is true of
  // palette-only.
  const cmds = Object.keys(createCommands(stubApi())).sort();
  const missing = cmds.filter((c) => !(c in OP_REGISTRY));
  assert.deepEqual(missing, [],
    'a command with no registry entry — give it a surface or a recorded reason');
  const stale = Object.keys(OP_REGISTRY).filter((c) => !cmds.includes(c));
  assert.deepEqual(stale, [], 'a registry entry for a command that no longer exists');
});

test('the CLI really implements every path the registry claims', async () => {
  // Read daw-cli's source, so the registry is checked against the CODE rather
  // than against itself. A name here that the CLI does not implement would record
  // an operation as covered when it is not — worse than recording it as a gap.
  const { readFileSync } = await import('node:fs');
  const src = readFileSync(new URL('../../ui/daw-cli/src/main.rs', import.meta.url), 'utf8');
  const verbs = new Set([...src.matchAll(/Some\(&"([a-z-]+)"\)/g)].map((m) => m[1]));
  assert.ok(verbs.size > 5, `daw-cli's dispatch was parsed: ${verbs.size} verbs`);
  for (const [op, e] of Object.entries(OP_REGISTRY)) {
    if (e.cli) assert.ok(verbs.has(e.cli), `${op} claims CLI verb "${e.cli}", which does not exist`);
  }
});

/**
 * Registry CLI verbs that NOTHING in the repository invokes.
 *
 * The check above proves a dispatch arm exists. It cannot prove the arm works, and the gap
 * between those two is where this wire fails: a verb that parses its flags, builds a payload with
 * a field at the wrong offset and sends it looks identical in the source to one that works. The
 * engine ignores a malformed command, daw-cli exits 0, and the parity list stays green. That is
 * not hypothetical — `open-editor` built a `UiChainCommandPayload` where the engine reads a
 * `UiCommandPayload`; both are 40 bytes, so it passed the size check and the engine read
 * `deviceKind` as the device id. It answered "device 0 not found" for every device on every
 * track, for as long as the verb had existed, with the registry counting it as covered.
 *
 * The audit that produced this list found SEVEN such verbs. `cli-verbs.mjs` now drives them
 * against a live engine, so the list is empty — and an empty list is the point: a NEW verb added
 * to the registry and never exercised lands here and fails.
 *
 * NOT A CLAIM THAT THE VERBS ARE CORRECT. "Something runs it" is weaker than "it does the right
 * thing", and the suites are where the second lives. This is the floor beneath them.
 */
const CLI_NEVER_DRIVEN = [];

test('every CLI verb the registry claims is actually driven by something', async () => {
  const { readFileSync, readdirSync, statSync } = await import('node:fs');
  const { join } = await import('node:path');

  const here = new URL('.', import.meta.url).pathname;
  const root = join(here, '../..');
  const files = [];
  const walk = (d) => {
    for (const name of readdirSync(d)) {
      if (['node_modules', 'target', '.git'].includes(name)) continue;
      const p = join(d, name);
      if (statSync(p).isDirectory()) walk(p);
      else if (/\.(mjs|sh|py)$/.test(name)) files.push(p);
    }
  };
  walk(join(root, 'ui-web/test'));
  walk(join(root, 'tools'));
  const corpus = files.map((p) => readFileSync(p, 'utf8')).join('\n');

  const claimed = [...new Set(Object.values(OP_REGISTRY).map((e) => e.cli).filter(Boolean))];
  // `do <verb>` or `get <verb>`, as a shell word or an argv element — the two shapes every
  // caller in this repo uses. Deliberately NOT a bare name match: half these verbs are ordinary
  // English words that appear in prose ("macro", "extents"), and matching those is how a list
  // like this decays into agreeing with itself.
  const driven = (v) => new RegExp(`(do|get)[\\s"',]+${v}\\b`).test(corpus);
  const never = claimed.filter((v) => !driven(v)).sort();

  assert.deepEqual(never, [...CLI_NEVER_DRIVEN].sort(),
    'a CLI verb the registry counts as covered that nothing in the repo runs — exercise it in '
    + 'cli-verbs.mjs, or say here why it cannot be. A dispatch arm nobody calls is a green row '
    + 'over untested code.');
});

/**
 * Suites the runbook cites that the sweep does NOT run, and which therefore have to be run by
 * hand before a demo. Each must be named in DEMO.md's own pre-flight section, so the obligation
 * lives where the person reading the runbook will see it — not only in a test.
 */
const MANUAL_BEFORE_DEMO = ['ai-demo.mjs', 'demo-stack-smoke.mjs'];

test('every suite the runbook cites exists, and the unswept ones are flagged as manual', async () => {
  /*
   * DEMO.md earns its authority by naming the check behind each claim — "`ops-ui.mjs` types all
   * seven with real keystrokes", "`live-plugin-add.mjs` guards it". That is the right way to write
   * a runbook and it creates two ways to lie that nothing was watching for.
   *
   * A RENAMED OR DELETED SUITE leaves the runbook citing a ghost. The claim reads exactly as
   * strong as before and nothing runs.
   *
   * AN EXCLUDED SUITE is worse, because it is invisible from both ends. `ai-demo.mjs` costs money
   * per run and `demo-stack-smoke.mjs` needs a live `tools/webstack.sh`, so all.mjs skips both —
   * correctly. But the runbook cites them the same way it cites the fourteen that DO run every
   * sweep, and "all suites passed" never mentioned them. Two of the six suites DEMO.md leans on
   * were only ever going to run if I remembered.
   *
   * So: cited-and-swept is fine, cited-and-unswept must be on MANUAL_BEFORE_DEMO *and* named in
   * the runbook's pre-flight section, and cited-and-absent fails.
   */
  const { readFileSync, existsSync } = await import('node:fs');
  const doc = readFileSync(new URL('../../docs/DEMO.md', import.meta.url), 'utf8');
  const all = readFileSync(new URL('./all.mjs', import.meta.url), 'utf8');

  const cited = [...new Set([...doc.matchAll(/([a-z0-9-]+\.mjs)/g)].map((m) => m[1]))].sort();
  assert.ok(cited.length >= 4, `the runbook cites suites: ${cited.length}`);

  const missing = cited.filter((f) => !existsSync(new URL(`./${f}`, import.meta.url)));
  assert.deepEqual(missing, [],
    'the runbook cites a suite that does not exist — the claim reads as strong as ever and '
    + 'nothing runs it');

  /*
   * EXCLUDED is a literal object in all.mjs; a suite named there is skipped by every sweep. The
   * slice must stop at that object's own closing brace — my first version ran to `const skipped`,
   * which swallowed NOT_A_SUITE (harnesses and libraries, declared in between) and reported
   * `all.mjs` as an unswept suite the moment the runbook mentioned it in prose.
   */
  const exStart = all.indexOf('const EXCLUDED');
  const excludedBlock = all.slice(exStart, all.indexOf('\n};', exStart));
  const notASuite = all.slice(all.indexOf('const NOT_A_SUITE'),
                              all.indexOf('\n]);', all.indexOf('const NOT_A_SUITE')));
  // A harness or library named in prose is not a claim about coverage, so it is not held to one.
  const unswept = cited.filter((f) => !notASuite.includes(`'${f}'`)
                                       && excludedBlock.includes(`'${f}'`));
  assert.deepEqual(unswept.sort(), [...MANUAL_BEFORE_DEMO].sort(),
    'the set of runbook-cited suites that no sweep runs changed. Every one of them is a claim '
    + 'that is only true if a person remembers to run it, so it belongs on MANUAL_BEFORE_DEMO '
    + 'and in the runbook\'s pre-flight section.');

  // And the runbook has to SAY so, where the person running the demo will read it.
  const preflight = doc.slice(0, doc.indexOf('## 1. The tracker'));
  const unmentioned = MANUAL_BEFORE_DEMO.filter((f) => !preflight.includes(f));
  assert.deepEqual(unmentioned, [],
    'a suite that no sweep runs is not named in the runbook\'s pre-flight section — the person '
    + 'reading DEMO.md would never learn they have to run it');
});

/**
 * Agent tools the registry claims that NO engine test has ever called.
 *
 * WHAT THIS MEASURES, EXACTLY: that a test EXISTS which drives the tool. Not that it passes —
 * this reads the corpus, and a test file cannot report its own result. So this is one notch
 * weaker than it looks, and the notch is the same one it was built to catch: existence is not
 * working. The result is the SWEEP's job, and a tool listed as driven by a test that fails is a
 * lie this check cannot see.
 *
 * That is acceptable only because the two run together. If they ever stop, this becomes a list
 * that agrees with itself.
 *
 * The twin of CLI_NEVER_DRIVEN, and it exists because the weaker check was not enough.
 *
 * `the agent really implements every tool the registry claims` verifies that the NAMED TOOL
 * EXISTS in tools.rs. That is satisfiable by a tool whose relevant branch does nothing.
 * `patcher_node` has an `action: "link"` which set the two node ids and left `src_port_id` and
 * `dst_port_id` at the struct's zeros — and port 0 is the event INPUT, so every link it sent
 * asked the engine to join an input to an input. Refused with `invalid_port` into a diff no tool
 * reads; the tool returned `sent: true`; the registry row was green throughout. It took a person
 * trying to build a patch to find it.
 *
 * Existing is not working. The only thing that tells them apart is a test that drives the tool
 * against a live engine and reads the SAVED PROJECT back, which is what ui/daw-agent/tests does.
 *
 * WHAT THIS DOES NOT CLAIM. Appearing in the corpus is not proof a tool is correct — `patcher_node`
 * appears there and was broken in a branch. It is a floor: a tool nothing has ever called is a
 * capability nobody has ever seen work, and that is worth knowing separately from a bug.
 */
const AGENT_NEVER_EXERCISED = [
  /*
   * THREE, and each is a HARNESS limit with its coverage named — not an unwritten test.
   *
   * All three are driven by tests in engine_e2e.rs that are `#[ignore]`d: `load_sample` resolves
   * a bare name against the loaded project's directory, and the Rust harness makes a fresh temp
   * dir whose engine output goes to /dev/null, so the refusal cannot be read. The capability is
   * covered where the tool was built to work — ai-demo.mjs loads a sample the MODEL chose, and
   * kit/sampler-render/sampler-state all load and play them in the Playwright harness, which
   * copies presets/ into its project directory.
   *
   * Listed here anyway. A tool proven only in another harness is worth knowing about separately
   * from one proven in this one, and quietly counting it as done is how a list starts agreeing
   * with itself.
   */
  'load_sample',
  'sampler_slice',
  'sampler_emit_rows',
];

test('every agent tool the registry claims has been driven against a real engine', async () => {
  const { readFileSync, readdirSync } = await import('node:fs');
  const { join } = await import('node:path');
  const here = new URL('.', import.meta.url).pathname;
  const testsDir = join(here, '../../ui/daw-agent/tests');

  /*
   * IGNORED TESTS ARE NOT COVERAGE, and counting them was this check's own version of the bug it
   * exists to catch. `#[ignore]` means the test does not run — so a tool driven only from inside
   * one is a tool nothing has ever executed against an engine, which is exactly the state this
   * list is for. Reading the raw file counted them, and the list and the check disagreed until
   * one of them was made to lie.
   *
   * Each test is `#[test]` preceded by its attributes, so the text between one test and the next
   * is where an `#[ignore]` for the following test lives.
   */
  const raw = readdirSync(testsDir).filter((f) => f.endsWith('.rs'))
    .map((f) => readFileSync(join(testsDir, f), 'utf8')).join('\n');
  const chunks = raw.split('#[test]');
  const corpus = chunks.map((body, i) => {
    if (i === 0) return '';                       // before the first test: no test to drive
    const attrs = chunks[i - 1].slice(-400);      // the attributes just above this #[test]
    return /#\[ignore\]/.test(attrs) ? '' : body;
  }).join('\n');
  assert.ok(raw.length > 1000, 'the agent test corpus was read');
  assert.ok(corpus.length > 1000, 'and it still holds tests that actually run');

  const claimed = [...new Set(Object.values(OP_REGISTRY).map((e) => e.agent).filter(Boolean))];
  // `tool: "name"` is how every ToolCall in that corpus is written. Deliberately not a bare name
  // match: these names appear in prose and in comments, and matching those is how a list like
  // this decays into agreeing with itself.
  const never = claimed.filter((t) => !corpus.includes(`tool: "${t}"`)).sort();

  assert.deepEqual(never, [...AGENT_NEVER_EXERCISED].sort(),
    'the set of registry-claimed agent tools that no engine test drives changed. A tool nothing '
    + 'has ever called is a capability nobody has seen work — add a test in '
    + 'ui/daw-agent/tests/engine_e2e.rs that asserts the SAVED PROJECT, or say here why not.');
});

test('every AI prompt in the runbook is one a test actually asks', async () => {
  /*
   * THE RUNBOOK PROMISED TWO THINGS NO RUN HAD EVER MADE.
   *
   * docs/DEMO.md lists the AI prompts under the word "Verified", and ai-demo.mjs asks a list of
   * prompts and asserts the song changed. Nothing tied the two lists together, and two of them
   * had drifted: the runbook said "on track 0" where the test says "on track 1" (the Bass track
   * the previous prompt creates), and "add a four on the floor kick pattern" where the test says
   * "...on a new track".
   *
   * A model is not a function. A prompt one word different is a different prompt, and the whole
   * value of that table is that someone typed each line and watched it work. Drift here is worse
   * than an ordinary stale doc, because the failure lands live in front of an audience.
   *
   * VERBATIM, not fuzzy: matching loosely would let exactly the drift this exists to catch
   * through. The test reads both files rather than sharing a constant, because the runbook has
   * to be readable as prose by someone who has never seen this suite.
   */
  const { readFileSync } = await import('node:fs');
  const doc = readFileSync(new URL('../../docs/DEMO.md', import.meta.url), 'utf8');
  const suite = readFileSync(new URL('./ai-demo.mjs', import.meta.url), 'utf8');

  const ai = doc.slice(doc.indexOf('## 7. The AI'));
  const table = ai.slice(0, ai.indexOf('\n\n', ai.indexOf('| say |')));
  const prompts = [...table.matchAll(/^\| `([^`]+)` \|/gm)].map((m) => m[1]);
  assert.ok(prompts.length >= 6, `the runbook's prompt table was parsed: ${prompts.length} rows`);

  const missing = prompts.filter((p) => !suite.includes(p));
  assert.deepEqual(missing, [],
    'the runbook lists prompts ai-demo.mjs never asks — either fix the wording or test it, but '
    + 'do not print "verified" over a string no run has made');
});

test('the agent really implements every tool the registry claims', async () => {
  const { readFileSync } = await import('node:fs');
  const src = readFileSync(new URL('../../ui/daw-agent/src/tools.rs', import.meta.url), 'utf8');
  const tools = new Set([...src.matchAll(/name:\s*"([a-z_]+)"/g)].map((m) => m[1]));
  assert.ok(tools.size > 3, `the agent manifest was parsed: ${tools.size} tools`);
  for (const [op, e] of Object.entries(OP_REGISTRY)) {
    if (e.agent) assert.ok(tools.has(e.agent), `${op} claims agent tool "${e.agent}", which does not exist`);
  }
});

test('the programmatic gap does not grow, and cannot rot', () => {
  // A ratchet, not a wall. The gap is real and pre-existing; a test that just
  // fails is a test nobody runs. This one fails when the gap GROWS — a new
  // document operation with no CLI path — and equally when it SHRINKS without
  // the list being updated, so the list cannot decay into a comforting lie.
  const doc = (e) => e.why === 'gap' || !e.why;
  const cliGap = Object.entries(OP_REGISTRY)
    .filter(([, e]) => doc(e) && !e.cli).map(([k]) => k).sort();
  const agentGap = Object.entries(OP_REGISTRY)
    .filter(([, e]) => doc(e) && !e.agent).map(([k]) => k).sort();
  assert.deepEqual(cliGap, [...CLI_GAP].sort(),
    'the set of ops with no CLI path changed — update CLI_GAP and say why');
  assert.deepEqual(agentGap, [...AGENT_GAP].sort(),
    'the set of ops with no agent tool changed — update AGENT_GAP and say why');
});

test('two tracks with the same device id do not share one plugin', () => {
  // DEVICE IDS ARE PER-TRACK. In presets/projects/maximal.uniproj.json all six
  // tracks have a device with `device_id: 0`, so a parameter map keyed on the id
  // ALONE has one slot for every track's first device: the last answer to arrive
  // wins and every track's rack shows that plugin. Reported from the app as
  // "I added Zebralette but the chain shows Analog Heat on all channels" — and
  // the plugin you just added never appears, because the slot is already full.
  //
  // The REQUEST side always keyed on the pair, so the right question was asked
  // and the answer was filed under the wrong name.
  const chains = {
    0: { track: 0, version: 1, devices: [{ id: 0, kind: 1, pos: 0, node: -1, slot: 0, caps: 0, bypass: 0 }] },
    1: { track: 1, version: 1, devices: [{ id: 0, kind: 1, pos: 0, node: -1, slot: 0, caps: 0, bypass: 0 }] },
  };
  const params = {
    [paramKey(0, 0)]: { track: 0, device: 0, name: 'Analog Heat', params: [] },
    [paramKey(1, 0)]: { track: 1, device: 0, name: 'Zebralette', params: [] },
  };
  const buf = createChainBuffer(4);
  const a = buildChainModel({ track: 0, chains, params, selected: -1, trackName: 'T1' }, buf);
  assert.equal(a.cards[0].title, 'Analog Heat', 'track 0 shows its own plugin');
  const b = buildChainModel({ track: 1, chains, params, selected: -1, trackName: 'T2' }, buf);
  assert.equal(b.cards[0].title, 'Zebralette', 'and track 1 shows ITS own, not track 0\'s');
});

test('the parameter key names the track as well as the device', () => {
  // The whole bug in one line: keyed on the device alone these collide.
  assert.notEqual(paramKey(0, 0), paramKey(1, 0));
  assert.notEqual(paramKey(1, 0), paramKey(0, 1));
  assert.equal(paramKey(0, 0), 0);
});

// ---------------------------------------------------------------------------
// THE FOURTH LIST: what the ENGINE can do.
//
// The op-registry test above compares the dock, the CLI and the agent — three
// lists that all describe the UI. None of them says what the APPLICATION is
// capable of. That is the engine's `UiCommandType`, and comparing against it
// found eleven commands the engine accepts and the frontend never sends:
// OpenPluginEditor, the two mod-link commands, MoveDevice,
// SavePatcherPreset, SetTrackHarmonyQuantize, the harmony pair, DeleteChord and
// SetDeviceEuclideanConfig.
//
// Every one of those is capability that exists, is tested engine-side, and
// cannot be reached. "How do I open the plugin UI" has the answer "you can't,
// and the command has been sitting there unused" — which is exactly the kind of
// thing a registry that only audits the UI can never tell you.
//
// So this reads the engine's own header and holds the frontend to it.

/** Engine commands with no frontend caller, and why. This list may SHRINK. */
const ENGINE_UNUSED = {
  /*
   * A STALE REASON, corrected rather than left. Euclidean nodes CAN be configured —
   * that is SetPatcherNodeConfig (41), which the patcher's knobs send and which is
   * proven end to end with audio in patchcfg.mjs. This is the older per-DEVICE path,
   * addressed by track+device rather than by node in the pool, and nothing sends it.
   *
   * The old wording said the feature was missing, which was true when it was written
   * and became a lie the day the node path landed. A recorded gap is read as a
   * finding by whoever picks it up, so a rotted one is worse than none.
   */
  SetDeviceEuclideanConfig: 'gap — superseded by SetPatcherNodeConfig; nothing sends the per-device form',
  /*
   * WAS: 'no way to know whether the file was written, so the button would lie'. True when it
   * was written, and it stopped being true when backend added UiPresetSavedPayload (diff type
   * 16, carrying `ok` and the echoed name) — which is exactly what I had asked them for and
   * then never re-checked. Now wired: `save-patch <name>`, and the result is worded in the
   * console rather than assumed. Off this list.
   */
  /*
   * NOT unreachable — UNREADABLE, which is why it is still a gap after a day of closing
   * these. The command works and I could send it in five minutes. What is missing is the
   * READ-BACK: `track.harmonyQuantize` lives in the runtime and in the project file and
   * in no published region — not a mixer flag, not a uiTrack* array.
   *
   * So the only thing shippable today is a WRITE-ONLY TOGGLE: press it, something
   * changes, and the interface can never say which way it is set. After a project load it
   * would have to guess or show nothing. That is the exact bug shape this session has been
   * spent removing, so it waits for the read-back rather than shipping as a keystroke with
   * a rumour attached. Asked backend for a bit in ui_track_mix_flags.
   */
  // WAS: 'the flag is writable but NOT published, so no control can show its state'. Wrong, and
  // it kept a working feature out of the UI for as long as it stood — the flag is
  // `uiTrackMixFlags` bit 2 and always has been. Backend caught it by checking live rather than
  // by reading the code. Now wired, so it is off this list entirely.
  RequestClipWindow: 'the sidecar owns the viewport and asks on the client\'s behalf',
  // These three are referenced by NAME elsewhere but never sent as a command from
  // a frontend path, which is the distinction this test draws: a struct that
  // mentions an enum is not a caller.
  //
  // UpdateDevice was here — "a device's config cannot be edited after it is
  // added" — until the bypass switch started sending it, and this list is
  // asserted in BOTH directions, so it failed the moment that became untrue. A
  // recorded gap that outlives the gap is a lie with a comment on it.
  LoadPluginOnTrack: 'gap — the rack inserts with AddDevice; this older path is unused',
  /*
   * `SetAutomationTarget` (13) sets a MODE: which parameter the point writes that follow it
   * will address. This app does not send it and does not want to — `WriteAutomationPoint`
   * carries its own paramId, so every write says what it is writing and a mode nobody set
   * cannot be the wrong one.
   *
   * That is the same argument the placement ops make against a global edit-scope mode, and
   * the same failure it avoids: forgetting to set a mode fails QUIETLY, in the wrong lane,
   * where a self-addressing command cannot.
   */
  SetAutomationTarget: 'gap — WriteAutomationPoint carries its own paramId; no mode needed',
  /*
   * A PLACEMENT's own edit scope (61): note edits landing in it are recorded as overrides
   * without the caller passing kUiEditScopeLocal each time. This app passes the scope per edit,
   * which is the explicit half of the same choice — so the per-placement mode is a convenience
   * it does not need yet, rather than a capability it is missing.
   */
  SetPlacementEditScope: 'gap — this app sets the scope per edit instead',
  /*
   * ARRANGEMENT SECTIONS (54-58), landed engine-side and not yet a surface here.
   *
   * Recorded rather than half-wired, and the reason is the same one that held lane
   * quantize back: sections are named spans of the timeline — verse, chorus — and the
   * command set is the easy half. What makes them worth having is seeing them ON the
   * arrangement and being able to drag their edges, which is a real piece of design
   * over a ruler that already has a playhead, a loop range and clip rails on it.
   *
   * A `section add` console verb with nothing drawn would be a feature you can only
   * verify by saving the file and reading it.
   */
  /*
   * Placement overrides can be WRITTEN from this UI (a note typed into a placement
   * becomes an override) and cannot be reverted, so "back to the shared clip" is
   * unreachable. Worth closing — it is the undo for a whole class of edit — and it
   * wants a control on the clip rather than a console verb, since that is where the
   * override is visible.
   */
  RevertPlacementOverrides: 'gap — no way to send a placement back to its shared clip',
  /*
   * THE SAMPLER (73, 74, 75, and the slice/marker/emit trio), landed across S1-S5 while this
   * side was designing how per-note ops draw. Recorded as one gap with one reason rather than
   * six, because they are one feature and half of them is not useful.
   *
   * The reason is the same one that held sections back, and it is not "no time": a sampler
   * reachable only by console verb is a sampler you can only verify by saving the file and
   * reading it. What makes it worth having is the KIT — seeing which slot a pad is, which
   * slice a marker cut, and dragging that marker while it plays. `RequestSamplerKit` is the
   * read-back that makes any of that drawable, so it is the one to wire first, and the
   * commands follow the surface rather than leading it.
   *
   * What is NOT blocked, and is being built now: the per-note ops that ADDRESS the sampler.
   * `s` (sound slot) and `o` (offset) are row ops on UiClipNote, not sampler commands — they
   * ride the clip-edit path that already exists, which is why they land before the kit does.
   */
  /*
   * `RequestSamplerKit` is NOT in this list any more: the `kit` verb calls it. That was the
   * plan stated when these were first recorded — the read-back is what makes a kit drawable, so
   * it goes first and the commands follow the surface rather than leading it.
   */
  /*
   * MODULES (SaveModule / LoadModule) and the sampler's envelope and bulk carrier, all landed
   * engine-side while this app was elsewhere. Recorded together because they share a reason:
   * each wants a surface before it wants a verb, and half of each is worse than none.
   *
   * `BulkChunk` in particular is a CARRIER, not a feature — it is what lifts the 40-byte inbound
   * cap so a command can send a string. Wiring it with nothing to send would be plumbing with
   * no water.
   */
  SaveModule: 'gap — a module is a saved chain; it wants a browser entry before a verb',
  LoadModule: 'gap — with SaveModule',
  /*
   * SamplerSetEnvelope WAS listed here — "an ADSR wants the drawable envelope, not four numbers
   * on a line" — and that reasoning was right about the surface and wrong about the priority.
   * A freshly loaded slot's default envelope produces NO LEVEL, so a sampler that has loaded a
   * file and been sent a note starts a voice and renders silence; four numbers on a line is what
   * stands between a chop and a sound, and waiting for the drawable version left the whole
   * workflow mute. The drawable envelope is still wanted, and is now an addition rather than a
   * precondition.
   */
  /*
   * The sampler's LFOs. With the envelope, and for the same reason: a modulator wants to be seen
   * moving, and four numbers on a command line is the half of it that is not worth having alone.
   *
   * Backend found tonight that every LFO in the file format was rendered by NOTHING — of the
   * sampler's five modulation targets and two kinds, only two combinations ever reached the
   * audio. So this is newly worth drawing rather than newly existing.
   */
  SamplerSetLfo: 'gap — with SamplerSetEnvelope; a modulator wants to be seen moving',
  BulkChunk: 'gap — a carrier for payloads over 40 bytes; nothing here needs one yet',
  /*
   * SamplerSetSlot WAS recorded here as a gap "with SamplerLoad". It is wired now, and the
   * reason it stopped being deferrable is `gate`: field 2, 0 = a one-shot that IGNORES note-off.
   * The engine has had both settings since the sampler shipped and no surface could reach
   * either, so a sampled note played its whole extent however short it was written — and
   * The ruling is that a note-off has to be able to cut it.
   */
  SamplerMarker: 'gap — with SamplerSlice',
  /*
   * SamplerEmitRows WAS recorded here as a gap "with SamplerSlice". It is wired now, and the
   * reason it stopped being deferrable is that `slice` makes the slots and puts NOTHING in the
   * pattern — so hearing a chop back meant writing a note per slice by hand, eight for an
   * eight-way chop and sixty-four for a kit. That is the gesture, not a convenience.
   */
};

test('the ops cell expands the SELECTED op and collapses the rest', () => {
  /*
   * The collapsed cell is one glyph per op; `cursor.op` selects one of them and that one draws
   * in full. Tested here rather than only in the browser because the condition is four
   * comparisons — track, row, column, and "is anything selected" — and getting any of them wrong
   * shows up as a cell that simply never expands, which looks like a dead keybinding.
   */
  const engine = gridEngine([4]);
  engine.notes = [{ tOn: 0, tOff: 240000, pitch: 60, velocity: 100, column: 0, track: 0, row: 0,
                    retrigger: 3, probability: 20, soundOffset: 20480, id: 1 }];
  engine.noteCount = 1;
  const opsCell = (cursor) => {
    const vm = buildViewModel({ startRow: 0, rowCount: 4, tracks: 1, columns: 3,
                                zoomIndex: 2, engine, cursor }, createBuffer(4, 1, 3));
    return vm.rows[0].cells[2].text;
  };
  assert.equal(opsCell({ row: 0, track: 0, col: 2, op: -1 }), 'rpo',
               'nothing selected: one glyph per op');
  assert.equal(opsCell({ row: 0, track: 0, col: 2, op: 0 }), 'ret3', 'the first op, in full');
  assert.equal(opsCell({ row: 0, track: 0, col: 2, op: 1 }), 'p20', 'the second');
  assert.equal(opsCell({ row: 0, track: 0, col: 2, op: 2 }), 'o80', 'the third');
  // A selection belongs to ONE cell: the same index with the cursor elsewhere expands nothing.
  assert.equal(opsCell({ row: 1, track: 0, col: 2, op: 0 }), 'rpo', 'another row is untouched');
  assert.equal(opsCell({ row: 0, track: 0, col: 0, op: 0 }), 'rpo', 'another field is untouched');
  // An out-of-range index draws the run rather than an empty cell — the ops are still there.
  assert.equal(opsCell({ row: 0, track: 0, col: 2, op: 9 }), 'rpo', 'a stale index still draws');
  // A cursor with no `op` at all (every caller that predates this) behaves as before.
  assert.equal(opsCell({ row: 0, track: 0, col: 2 }), 'rpo', 'a cursor without `op` is unchanged');
});

test('the glyph run, the present-op list and the canonical text are the same sequence', () => {
  /*
   * THE INVARIANT A SUB-CELL CURSOR RIDES ON.
   *
   * The run draws one glyph per op and `opsPresent` lists the ops behind them, so an index into
   * the run addresses an op. They agree because both walk ROW_OPS skipping falsy values — and
   * two walks that agreed by coincidence would put the caret on one glyph and edit another, with
   * the note coming back with the wrong op changed and nothing anywhere reporting an error.
   *
   * Checked over every SUBSET rather than one example, because the orders only diverge when some
   * ops are absent: any two present ops must appear in the same relative order in both walks no
   * matter which of the others are missing.
   */
  const VALUES = { retrigger: 3, probability: 55, delayTicks: 160000, sound: 5, soundOffset: 20480 };
  const fields = ROW_OPS.map((o) => o.field);
  for (let bits = 0; bits < (1 << fields.length); bits++) {
    const note = {};
    fields.forEach((f, i) => { if (bits & (1 << i)) note[f] = VALUES[f]; });
    const run = opsRun(note);
    const present = opsPresent(note);
    assert.equal(run.length, present.length,
                 `one glyph per present op for ${JSON.stringify(note)}`);
    assert.equal(run, present.map(opGlyph).join(''),
                 `the run is the present ops' glyphs, in order, for ${JSON.stringify(note)}`);
    // ...and the token at each index is the word the canonical text has in that position.
    const words = opsText(note, 960000).split(' ').filter(Boolean);
    assert.equal(words.length, present.length, 'the text has one word per present op');
    for (let i = 0; i < present.length; i++) {
      assert.equal(opTokenAt(note, i, 960000), words[i],
                   `token ${i} of ${JSON.stringify(note)} matches the canonical text`);
    }
  }
});

test("the row-op mask bits are the engine's, not this table's order", async () => {
  const { readFileSync } = await import('node:fs');
  const hdr = readFileSync(new URL('../../apps/event_payloads.h', import.meta.url), 'utf8');
  /*
   * WHY THIS EXISTS. ROW_OPS is written in the order a person reads a row — ret, p, d, s, o —
   * and the wire numbers them Retrigger, Probability, Sound, SoundOffset, Delay. Deriving the
   * bits from the table's index produced sound=4, soundOffset=16, delay=8: three ops addressing
   * each other's fields. On the wire that is not an error, it is a different edit, and the note
   * that comes back has a delay where a slot number was typed.
   *
   * So the values are written down twice — once in the header, once in rowops.js — and this is
   * what makes that safe. It reads the enum rather than a copy of it.
   */
  const block = hdr.slice(hdr.indexOf('kRowOpMaskRetrigger'));
  const bits = {};
  for (const m of block.slice(0, block.indexOf('}')).matchAll(/kRowOpMask([A-Za-z]+) = 1u << (\d+)/g)) {
    bits[m[1]] = 1 << Number(m[2]);
  }
  assert.ok(Object.keys(bits).length >= 5,
            `the engine's row-op mask enum was parsed: ${JSON.stringify(bits)}`);

  // Every op in the table must name a bit the engine defines, and agree about its value.
  for (const op of ROW_OPS) {
    assert.ok(op.bit in bits, `${op.prefix} names a wire bit the engine has: ${op.bit}`);
    assert.equal(OP_MASK[op.field], bits[op.bit],
                 `${op.prefix} (${op.field}) uses the engine's bit for ${op.bit}`);
  }
  // ...and every bit the engine defines must belong to an op, so a new one cannot be added
  // engine-side and go unnoticed here.
  const claimed = new Set(ROW_OPS.map((o) => o.bit));
  for (const name of Object.keys(bits)) {
    assert.ok(claimed.has(name), `the engine's ${name} bit is claimed by a row op`);
  }
});

test('every engine command has a caller, or a recorded reason it has none', async () => {
  const { readFileSync } = await import('node:fs');
  const hdr = readFileSync(new URL('../../apps/event_payloads.h', import.meta.url), 'utf8');
  const block = hdr.slice(hdr.indexOf('enum class UiCommandType'));
  /*
   * `[A-Za-z0-9]`, because a NAME WITH A DIGIT IN IT was invisible to this audit.
   *
   * `[A-Z][A-Za-z]+` silently skipped `SetModLinkUid16`, so an engine command was
   * neither reported as unwired nor recorded as a gap — the audit had a hole shaped
   * exactly like the thing it exists to find. Two commands were being dropped when
   * this was measured.
   *
   * The count assertion below is what makes the widening safe to trust: it fails if
   * the pattern ever stops matching the source, which is the failure mode every
   * source-reading check has to guard against.
   */
  const names = [...block.slice(0, block.indexOf('};')).matchAll(/^\s+([A-Z][A-Za-z0-9]+) = \d+,/gm)]
    .map((m) => m[1]).filter((n) => n !== 'None');
  assert.ok(names.length > 15, `the engine's command enum was parsed: ${names.length}`);

  // The sidecar is the UI's only path to the ring, so a command the sidecar never
  // names is a command the UI cannot send. The agent has its own ring and its own
  // reach, so it counts as a caller too.
  const callers = ['../../ui/daw-sidecar/src/main.rs', '../../ui/daw-agent/src/tools.rs']
    .map((f) => readFileSync(new URL(f, import.meta.url), 'utf8')).join('\n');

  const unused = names.filter((n) => !new RegExp(`UiCommandType::${n}\\b`).test(callers));
  const unexplained = unused.filter((n) => !(n in ENGINE_UNUSED));
  assert.deepEqual(unexplained, [],
    'an engine command nothing calls and nothing explains — wire it or record why');
  // And the reverse, so the list cannot rot: something listed as unused that has
  // since been wired must be removed, or the list stops describing the code.
  const stale = Object.keys(ENGINE_UNUSED).filter((n) => !unused.includes(n));
  assert.deepEqual(stale, [],
    'listed as unused but something calls it now — delete the entry');
});

// ---------------------------------------------------------------------------
// ROW OPS: the mirror, and the run that replaced the priority chain.
// ---------------------------------------------------------------------------

test("rowop.rs's own two orders agree — the schema's and the emitter's", async () => {
  /*
   * A MIRROR CANNOT BE RIGHT ABOUT BOTH IF THE SOURCE DISAGREES WITH ITSELF.
   *
   * `OP_SCHEMA` is the list this side mirrors — it fixes the DRAW order of the collapsed glyph
   * run — and `format_row_ops` builds the canonical TEXT. They are two orders of the same ops in
   * one file, and nothing held them equal: the schema listed `o` fifth while the emitter pushed
   * it last, so a row carrying an offset AND a ramp spelled `ret4 o80 rv-60 c1:2` here and
   * `ret4 rv-60 c1:2 o80` there.
   *
   * Nothing BREAKS, because the parser is order-free and both strings mean the same row. But
   * "canonical" means one form, and a person reading the glyphs and then opening the cell would
   * see two different orders for one note. This is the check that says so.
   */
  const { readFileSync } = await import('node:fs');
  const src = readFileSync(new URL('../../ui/daw-bridge/src/rowop.rs', import.meta.url), 'utf8');

  const schemaBlock = src.slice(src.indexOf('OP_SCHEMA'), src.indexOf('];', src.indexOf('OP_SCHEMA')));
  const schema = [...schemaBlock.matchAll(/prefix:\s*"([^"]+)"/g)].map((m) => m[1]);

  // The emitter's order is the order it PUSHES, and each push starts with the op's prefix.
  const emitStart = src.indexOf('pub fn format_row_ops');
  const emitBlock = src.slice(emitStart, src.indexOf('\n}', emitStart));
  const emitted = [];
  for (const m of emitBlock.matchAll(/out\.push\(format!\("([a-z]+)/g)) {
    if (!emitted.includes(m[1])) emitted.push(m[1]);
  }
  assert.ok(schema.length > 4 && emitted.length > 4,
            `parsed both orders: schema ${JSON.stringify(schema)}, emitter ${JSON.stringify(emitted)}`);
  /*
   * THEY AGREE NOW, so this asserts it rather than the gap.
   *
   * It was an INVERTED check for a few hours — asserting the orders differed, so that it would
   * fail the day they were aligned and make me change the mirror. It did exactly that on the
   * merge, which is the whole argument for writing a known limitation as a check rather than a
   * comment: a comment would still be sitting there describing a world that had moved.
   *
   * The order both sides now use is `ret rv p d s o c`, and it is neither of the two that
   * existed: ops that modify each other are adjacent — a ramp is meaningless without a retrigger
   * and an offset addresses the same sample as the slot — and `c` is last because it is the only
   * op about WHEN the row fires rather than what it plays.
   */
  assert.deepEqual(emitted, schema,
    'format_row_ops emits its ops in a different order from OP_SCHEMA, so the canonical text '
    + 'and the collapsed glyph run would disagree about one note');
});

test('the JS row-op mirror matches the Rust schema exactly', async () => {
  /*
   * `ROW_OPS` in rowops.js is a MIRROR of `OP_SCHEMA` in
   * ui/daw-bridge/src/rowop.rs, which is the grammar the CLI, the bridge and the
   * engine all share. Nothing forces the two to agree, and this project has paid
   * for that shape six times in one session with dockApi/__uni.
   *
   * Read from the RUST SOURCE rather than from a copied list, so the check is
   * against the definition rather than against itself. Asked backend whether the
   * schema can be published instead of mirrored; until it is, this is the ratchet.
   */
  const { readFileSync } = await import('node:fs');
  const src = readFileSync(
    // THIS REPO'S COPY, not the sibling checkout's. It read ../../../daw/... — backend's
    // working tree — so the check failed whenever they were mid-edit on ops this side had never
    // been told about, which punishes someone else's work in progress rather than finding drift.
    // The copy here is what the sidecar actually compiles against, which is what has to agree
    // with the mirror.
    new URL('../../ui/daw-bridge/src/rowop.rs', import.meta.url), 'utf8');
  const block = src.slice(src.indexOf('OP_SCHEMA'), src.indexOf('];', src.indexOf('OP_SCHEMA')));
  const rust = [...block.matchAll(/prefix:\s*"([^"]+)"/g)].map((m) => m[1]);
  assert.ok(rust.length > 0, `parsed OP_SCHEMA prefixes: ${JSON.stringify(rust)}`);
  assert.deepEqual(ROW_OPS.map((o) => o.prefix), rust,
    'rowops.js must list the same ops, in the same order, as rowop.rs — order is '
    + 'the draw order, so a mismatch silently reorders every run on screen');
});

test('every op has a distinct glyph', () => {
  // The glyph is IDENTITY: an op can appear at any position in the run, so
  // position cannot say which op it is and the character must. Two ops sharing a
  // glyph is therefore not cosmetic — it makes a run ambiguous to read.
  const seen = new Map();
  for (const op of ROW_OPS) {
    const g = opGlyph(op);
    assert.equal(g.length, 1, `${op.prefix} draws exactly one character, got ${JSON.stringify(g)}`);
    assert.ok(!seen.has(g), `${op.prefix} and ${seen.get(g)} both draw ${JSON.stringify(g)}`);
    seen.set(g, op.prefix);
  }
});

test('a note carrying several ops shows ALL of them', () => {
  /*
   * THE BUG THIS REPLACED. The cell resolved ops by priority —
   *   n.retrigger ? 'R'+n.retrigger : n.probability ? 'P'+n.probability : 'D'
   * — so a note with `ret3 p60 d1/6` drew `R3` and the other two were invisible
   * while the engine played all three. `parse_row_ops` has always taken a
   * whitespace-separated LIST, so the notation was never single-op; the display
   * was.
   */
  // EVERY field in ROW_OPS, built from the table rather than listed — a note that names five of
  // seven would assert "all of them" about five, which is the shape this test exists to catch.
  const all = opsRun({ retrigger: 3, probability: 60, delayTicks: 160000,
                       sound: 5, soundOffset: 32768,
                       retrigRamp: -60, trigCondition: makeTrigCondition(1, 2) });
  assert.equal(all.length, ROW_OPS.length,
    `three ops draw three characters, got ${JSON.stringify(all)}`);
  for (const op of ROW_OPS) {
    assert.ok(all.includes(opGlyph(op)),
      `${op.prefix} is in ${JSON.stringify(all)} — the old chain dropped it silently`);
  }
  // ...and the NEGATIVE CONTROL for the priority chain: under the old code this
  // run was 'R3', which is length 2 and contains neither 'p' nor 'd'. If this
  // assertion could pass against that, the test above would be decoration.
  assert.notEqual(all, 'R3', 'the priority chain would have produced this');
});

test('ops appear in schema order, and only when set', () => {
  // ORDER is what makes two notes with the same ops draw the same string, which
  // is what makes a column scannable at all.
  assert.equal(opsRun({ probability: 60, retrigger: 3 }),
               opsRun({ retrigger: 3, probability: 60 }),
               'the run is ordered by the schema, not by the object');
  // A note with nothing draws NOTHING — an ordinary kit track carries no new ink,
  // which is the whole answer to "columns that are mostly empty".
  assert.equal(opsRun({}), '', 'no ops, no ink');
  /*
   * ZERO IS ABSENCE, NOT A VALUE. `probability: 0` means "always sounds", which
   * is the op being absent; drawing a mark there would claim a note is
   * conditional when it is not.
   */
  assert.equal(opsRun({ probability: 0, retrigger: 0, delayTicks: 0 }), '',
               'zeroes are absence, not ops');
});

test('the canonical text form round-trips what the engine published', () => {
  // What the edit buffer is seeded with and what an agent writes — one grammar.
  assert.equal(opsText({ retrigger: 3, probability: 60 }), 'ret3 p60');
  // ORDER is the schema's, not the object's — the text form is generated from the
  // same list the run is, so the two cannot drift apart.
  /*
   * `o` is DECIMAL 1/256ths, stored as `n * 256` — so 32768 is 128, not 0x80.
   *
   * This line asserted `o80` while `opsText` rendered hex, and the two agreed with each other
   * and with nothing else: `parse_row_ops` reads the token with a decimal `parse::<u32>()`, so
   * the hex form round-tripped to a DIFFERENT offset. A text form whose only contract is that
   * it parses back had exactly one job.
   */
  assert.equal(opsText({ soundOffset: 32768, retrigger: 3, sound: 5 }), 'ret3 s5 o128');
  /*
   * `s9` AND `s09` ARE THE SAME THING — the ruling, and the half that matters.
   *
   * The written form went padded and then back again (SAMPLER_DESIGN section 8 Q1 argued for
   * `s07` to keep a fixed-width column's vertical rhythm; that lost to "the canonical form should
   * be the one a person types"). What must never change is that both spellings, and any number
   * of leading zeros, address the same slot — so a row typed by hand and a row written by the
   * engine are the same row. Pinned here because it is an equivalence rather than a format, and
   * an equivalence is the kind of thing a formatting change breaks by accident.
   */
  for (const [a2, b2] of [['s9', 's09'], ['s09', 's009'], ['s7', 's07']]) {
    assert.deepEqual(parseOps(a2).ops, parseOps(b2).ops, `${a2} and ${b2} address the same slot`);
  }
  // The delay is published in TICKS and authored as a fraction of a beat, so it
  // needs the beat length to be spelled back. 160000 of 960000 is a sixth.
  assert.equal(opsText({ delayTicks: 160000 }, 960000), 'd1/6');
  // Without a beat length it says ticks rather than guessing a fraction — a guess
  // would round-trip a different note than the one on screen.
  assert.equal(opsText({ delayTicks: 160000 }), 'd160000t');
});



test('every node type this UI can edit has a config layout on the wire', async () => {
  /*
   * THE GAP THIS CLOSES, hit for real: SliceSelect arrived with `{base, count}`, the patcher
   * view grew rows for them, and `build_patcher_config` had no arm for type 7 — so every nudge
   * came back "no config layout for that node type". Refused rather than sent as zeros, which is
   * the right refusal and still a control the UI offers and cannot deliver.
   *
   * Both sides are read from source. The sidecar's arms are `N => {` inside the function, and
   * this side's are the types with CONFIG_FIELDS entries — a type with no fields has nothing to
   * send and is correctly absent from both.
   */
  /*
   * THE PACKER MOVED, AND THIS CHECK FOLLOWED IT RATHER THAN BEING DELETED.
   *
   * It used to read `build_patcher_config` in the sidecar. There were two packers by then — the
   * sidecar's, which clamped, and daw-cli's, which cast straight into each field's width — so the
   * same edit made two different nodes depending on the surface. Both now call
   * `pack_patcher_node_config` in daw-bridge, and this reads THAT, because a ratchet pointed at a
   * function nobody calls any more passes on anything.
   *
   * The arms are named constants there rather than bare numbers, which is why this maps the names
   * back to their indices: the constants are also read from source, so a renumbered node type
   * cannot make the two sides agree by accident.
   */
  const rs = readFileSync(new URL('../../ui/daw-bridge/src/layout.rs', import.meta.url), 'utf8');
  const nums = Object.fromEntries([...rs.matchAll(
    /pub const (PATCHER_NODE_[A-Z_]+): u32 = (\d+);/g)].map((m) => [m[1], Number(m[2])]));
  assert.ok(Object.keys(nums).length >= 6, `parsed the node type constants: ${Object.keys(nums)}`);
  const fn = rs.slice(rs.indexOf('pub fn pack_patcher_node_config'));
  const body = fn.slice(0, fn.indexOf('\n}\n'));
  const arms = new Set([...body.matchAll(/^\s{8}(PATCHER_NODE_[A-Z_]+) => \{/gm)]
    .map((m) => nums[m[1]])
    .filter((n) => n !== undefined));
  assert.ok(arms.size >= 3, `parsed the wire's config layouts: ${[...arms]}`);

  // The types this UI draws editable fields for, by their index in NODE_TYPES.
  // `configFields(type)` takes the type INDEX and returns [{name, index}], empty for a type
  // with nothing to edit — which is the same question, asked of the module that owns it.
  const editable = NODE_TYPES.map((_, i) => i).filter((i) => configFields(i).length > 0);
  for (const t of editable) {
    assert.ok(arms.has(t),
      `node type ${t} (${NODE_TYPES[t]}) has editable fields here and no layout on the wire`);
  }
});

test('the sampler slot-field table matches the engine enum', async () => {
  /*
   * WRITTEN BEFORE THE ENUM GROWS, which is the only time a mirror is worth writing.
   *
   * `SLOT_FIELDS` is indexed BY WIRE ID — the index IS the field id the command carries — and it
   * is hand-written. Backend has announced `SourceLocalId = 27` and `SliceId = 28`, so when they
   * land this list is two short and `slot <track> <device> <slot> <field> <value>` simply cannot
   * name them. Silently: an absent name is one the `oneOf` never offers, which reads as the
   * feature not existing rather than as a table being stale.
   *
   * The patcher node-type mirror was written the same way, the day SliceSelect was announced,
   * and caught it on the first run after the merge. A mirror added afterwards has already missed
   * the one event it exists for.
   */
  const { readFileSync } = await import('node:fs');
  const hdr = readFileSync(new URL('../../apps/event_payloads.h', import.meta.url), 'utf8');
  const block = hdr.slice(hdr.indexOf('enum class SamplerSlotField'));
  const body = block.slice(0, block.indexOf('};'));
  const ids = [...body.matchAll(/^\s*([A-Z][A-Za-z]*)\s*=\s*(\d+)/gm)]
    .map((m) => [m[1], Number(m[2])])
    .sort((a, b) => a[1] - b[1]);
  assert.ok(ids.length > 20, `parsed the enum: ${ids.length} fields`);
  // Dense from 0, which is what makes an index-keyed array the right mirror for it at all.
  assert.deepEqual(ids.map((x) => x[1]), ids.map((_, i) => i),
    'SamplerSlotField is dense from 0');
  assert.equal(SLOT_FIELDS.length, ids.length,
    `every slot field is nameable: the engine has ${ids.length} `
    + `(${ids[ids.length - 1][0]} is the last), this side names ${SLOT_FIELDS.length}`);

  /*
   * ...AND THE SIDECAR LETS THEM THROUGH, which is a second gate nobody was watching.
   *
   * `build_sampler_slot` range-checks the field id before putting it on the ring. That bound was
   * `0..=26` when 27 and 28 landed, so `slot ... source|slice` was refused in the sidecar while
   * the page reported success — `samplerSlot` returns true as soon as the message is queued.
   * Every mirror above was correct and the commands still reached nothing.
   *
   * Read as a LITERAL out of the Rust source rather than exercised, because exercising it needs
   * a running stack; ui-web/test/sampler-state.mjs does that end to end. This is the cheap check
   * that fails first.
   */
  const rust = readFileSync(new URL('../../ui/daw-sidecar/src/main.rs', import.meta.url), 'utf8');
  const bound = rust.match(/if !\(0\.\.=(\d+)\)\.contains\(&field\)/);
  assert.ok(bound, "the sidecar's slot-field range check was found");
  assert.equal(Number(bound[1]), ids[ids.length - 1][1],
    `the sidecar accepts field ids 0..${bound[1]} and the engine's last field is `
    + `${ids[ids.length - 1][0]} = ${ids[ids.length - 1][1]} — anything above the bound is `
    + 'refused on the way out and reported as success');
});

test('the patcher node-type table matches the engine enum', async () => {
  /*
   * `NODE_TYPES` mirrors `enum class PatcherNodeType` in the engine, and nothing forced them to
   * agree — the same gap the device-kind table had, where the sampler shipped as kind 5 against a
   * list that stopped at 4 and every sampler card read "kind 5 #9".
   *
   * Written the day backend ANNOUNCED SliceSelect=7, deliberately before it lands: the point of a
   * mirror is to fail when the other side moves, and a mirror added after the move has already
   * missed the one event it exists for. When 7 arrives this test goes red and names it, instead
   * of the patcher quietly drawing a node it has no word for.
   *
   * Parsed from the HEADER, so it is checked against the definition rather than against itself.
   */
  const { readFileSync } = await import('node:fs');
  const src = readFileSync(new URL('../../apps/patcher_graph.h', import.meta.url), 'utf8');
  const block = src.slice(src.indexOf('enum class PatcherNodeType'));
  const body = block.slice(0, block.indexOf('};'));
  const names = [...body.matchAll(/^\s*([A-Z][A-Za-z]*)\s*=\s*(\d+)/gm)]
    .map((m) => [m[1], Number(m[2])])
    .sort((a, b) => a[1] - b[1]);
  assert.ok(names.length > 4, `parsed the enum: ${JSON.stringify(names)}`);
  // Dense from 0, or an index-keyed array is the wrong mirror for it.
  assert.deepEqual(names.map((n) => n[1]), names.map((_, i) => i),
    'PatcherNodeType is dense from 0, which is what makes an array the right mirror');
  assert.equal(NODE_TYPES.length, names.length,
    `every node type is named: engine has ${JSON.stringify(names.map((n) => n[0]))}, `
    + `this side has ${JSON.stringify(NODE_TYPES)}`);
});

test('the device-kind table matches the engine enum', async () => {
  /*
   * `DEVICE_KINDS` mirrors `enum class DeviceKind` in apps/device_chain.h, and nothing forced
   * them to agree. They did not: the sampler shipped as kind 5 while this list stopped at 4, so
   * every sampler card read "kind 5 #9" with a fallback badge — which looks exactly like a
   * device whose kind has no name, rather than like a table that is one entry short.
   *
   * Parsed from the HEADER, so the check is against the definition rather than against itself.
   */
  const { readFileSync } = await import('node:fs');
  const src = readFileSync(new URL('../../apps/device_chain.h', import.meta.url), 'utf8');
  const block = src.slice(src.indexOf('enum class DeviceKind'));
  const body = block.slice(0, block.indexOf('};'));
  const names = [...body.matchAll(/^\s*([A-Z][A-Za-z]*)\s*=\s*(\d+)/gm)]
    .map((m) => [m[1], Number(m[2])])
    .sort((a, b) => a[1] - b[1]);
  assert.ok(names.length > 4, `parsed the enum: ${JSON.stringify(names)}`);
  // The VALUES must be dense from 0, or an index-keyed table is the wrong shape entirely.
  assert.deepEqual(names.map((n) => n[1]), names.map((_, i) => i),
    'DeviceKind is dense from 0, which is what makes an array the right mirror');
  assert.equal(DEVICE_KINDS.length, names.length,
    `every kind is named: engine has ${JSON.stringify(names.map((n) => n[0]))}, `
    + `this side has ${JSON.stringify(DEVICE_KINDS)}`);
});

test('the JS op parser agrees with the Rust one on every case Rust tests', async () => {
  /*
   * THEIR TESTS, RUN AGAINST MY PARSER.
   *
   * The prefix ratchet above holds the op LIST equal and is blind to everything else. It proved
   * that the hard way: backend added `o<N>/<M>` to `parse_row_ops` — a whole second form for an
   * existing op, with its own bounds and its own rounding — and the ratchet passed, because no
   * prefix changed. They expected it to fail and it did not.
   *
   * So this reads the literals out of rowop.rs's own test module and asserts my parser reaches
   * the same verdict on each. It costs nothing to maintain: a case they add is a case I run,
   * and a rule they change breaks here without anyone deciding to check.
   *
   * ACCEPT-OR-REJECT is the claim, plus the VALUE wherever their assertion states one. That is
   * the whole of what a mirror can get wrong.
   */
  const { readFileSync } = await import('node:fs');
  const src = readFileSync(
    new URL('../../ui/daw-bridge/src/rowop.rs', import.meta.url), 'utf8');
  const tests = src.slice(src.indexOf('mod tests'));
  assert.ok(tests.length > 200, 'found the Rust test module');

  // `parse_row_ops("...")` plus enough of what follows to tell a rejection from an acceptance,
  // and to catch `.unwrap().field, value` where one is asserted.
  const RE = /parse_row_ops\("([^"]*)"\)\s*(\.is_err\(\)|\.unwrap\(\)(?:\.([a-z_]+))?)?/g;
  // Rust spells them snake_case; this side is camel.
  const FIELD = { sound_offset: 'soundOffset', sound: 'sound',
                  retrigger: 'retrigger', probability: 'probability' };

  let n = 0, checkedValues = 0;
  for (const m of tests.matchAll(RE)) {
    const [whole, tokens, tail, field] = m;
    const r = parseOps(tokens);
    n++;
    if (tail === '.is_err()') {
      assert.ok(r.error, `Rust rejects ${JSON.stringify(tokens)} and this parser accepted it`);
      continue;
    }
    assert.ok(!r.error,
      `Rust accepts ${JSON.stringify(tokens)} and this parser refused it: ${r.error}`);
    if (!field || !FIELD[field]) continue;
    /*
     * The expected value, when the assertion states one. Their expressions are plain numbers or
     * a simple product (`128 * 256`), so this reads exactly those two shapes and skips anything
     * else rather than evaluating arbitrary Rust — a ratchet that guesses is worse than one that
     * checks less.
     */
    const after = tests.slice(m.index + whole.length);
    const val = /^\s*,\s*(\d+)\s*(?:\*\s*(\d+))?\s*\)/.exec(after);
    if (!val) continue;
    const want = Number(val[1]) * (val[2] ? Number(val[2]) : 1);
    assert.equal(r.ops[FIELD[field]], want,
      `${JSON.stringify(tokens)}: Rust says ${field} is ${want}`);
    checkedValues++;
  }
  assert.ok(n >= 15, `ran a meaningful number of their cases: ${n}`);
  assert.ok(checkedValues >= 3, `and checked real values, not just accept/reject: ${checkedValues}`);
});

test('a modulator that cannot move anything reads as inert, not as absent', () => {
  /*
   * `modMask` has a bit per (target, kind) — `target * 2 + kind`, targets being volume,
   * panning, pitch, cutoff, resonance and kinds envelope, LFO.
   *
   * THE FILTER IS THE TRAP, and backend flagged it before I could fall in: a cutoff or
   * resonance modulator on a filter that is OFF is silent, and the mask does not know that
   * because the filter is a property of the mod set rather than of the modulator. A row that
   * drew a cutoff envelope without checking `filterType` would show a live control over a dead
   * one — the same lie this rack already refuses for an inert modulation link and for a
   * parameter the plugin ignores.
   *
   * SHOWN, NOT HIDDEN. A configured modulator that cannot move anything is worth seeing,
   * because the reason it does nothing is fixable and otherwise invisible.
   */
  assert.deepEqual(modSummary(0, 0), { mark: '', title: '' }, 'no bits, nothing drawn');

  // bit0 = volume envelope. Nothing to do with the filter, so live whatever the filter is.
  assert.equal(modSummary(1 << 0, 0).mark, '~', 'an amp envelope moves with the filter off');
  assert.match(modSummary(1 << 0, 0).title, /volume envelope/);

  // bit6 = cutoff envelope (target 3, kind 0). Silent while the filter is off...
  const off = modSummary(1 << 6, 0);
  assert.equal(off.mark, '!', 'a cutoff envelope with the filter OFF cannot move anything');
  assert.match(off.title, /cutoff envelope \(filter is off\)/,
    'and says why, because the reason is the fixable part');
  // ...and live once it is on.
  assert.equal(modSummary(1 << 6, 1).mark, '~', 'and moves once the filter is on');
  assert.doesNotMatch(modSummary(1 << 6, 1).title, /filter is off/);

  // bit7 = cutoff LFO, bit9 = resonance LFO — both filter-dependent, both inert with it off.
  assert.equal(modSummary(1 << 7, 0).mark, '!', 'so is a cutoff LFO');
  assert.equal(modSummary(1 << 9, 0).mark, '!', 'and a resonance LFO');
  assert.equal(modSummary(1 << 5, 0).mark, '~', 'while a pitch LFO is unaffected by the filter');

  // BOTH at once is the state worth distinguishing: something moves AND something cannot.
  const both = modSummary((1 << 0) | (1 << 6), 0);
  assert.equal(both.mark, '~!', 'a slot with one working and one dead modulator says both');
});

test('the wire constants match the sidecar that writes them', async () => {
  /*
   * `wire.js` decodes a frame the sidecar encodes, and the two agree by hand: the sidecar writes
   * fields sequentially and this side reads them at literal offsets. Tonight the lane block grew
   * from 16 entries to 64 and I shifted twenty-five of those literals with a script — which
   * worked, and which nothing would have caught if it had not.
   *
   * The suites DO catch a mismatch, loudly, because everything after the error decodes as
   * garbage. But they catch it as "twelve suites are failing", which is an expensive way to be
   * told that one number is wrong. These three are the numbers a shift moves, read out of the
   * Rust so neither side is quoting the other from memory:
   *
   *   WIRE_VERSION       a mismatch REJECTS the frame, which is the designed behaviour — so a
   *                      forgotten bump is a blank page rather than a wrong one
   *   FULL_HEADER_BYTES  where the variable section starts; wrong and every section is wrong
   *   NOTE_BYTES         the per-note stride
   *
   * This is the cheap half of what backend called the real check — dumping sizeof/offsetof from
   * both languages and diffing. It compares three declared constants rather than a computed
   * layout, so it catches a forgotten bump and not a mis-ordered field. Worth saying which,
   * because a check that is trusted beyond what it verifies is worse than none.
   */
  const { readFileSync } = await import('node:fs');
  const rs = readFileSync(new URL('../../ui/daw-sidecar/src/main.rs', import.meta.url), 'utf8');
  const js = readFileSync(new URL('../src/wire.js', import.meta.url), 'utf8');

  const rust = (re, what) => {
    const m = re.exec(rs);
    assert.ok(m, `found ${what} in the sidecar`);
    return Number(m[1]);
  };
  const mine = (re, what) => {
    const m = re.exec(js);
    assert.ok(m, `found ${what} in wire.js`);
    return Number(m[1]);
  };

  assert.equal(mine(/export const WIRE_VERSION = (\d+);/, 'WIRE_VERSION'),
               rust(/const WIRE_VERSION: u16 = (\d+);/, 'WIRE_VERSION'),
    'WIRE_VERSION differs — the page would reject every frame, which looks like a dead engine');
  assert.equal(mine(/const HEADER_BYTES = (\d+);/, 'HEADER_BYTES'),
               rust(/const FULL_HEADER_BYTES: usize = (\d+);/, 'FULL_HEADER_BYTES'),
    'the header size differs — every section after it decodes at the wrong offset');
  assert.equal(mine(/const NOTE_BYTES = (\d+);/, 'NOTE_BYTES'),
               rust(/const NOTE_BYTES: usize = (\d+);/, 'NOTE_BYTES'),
    'the note stride differs — notes read fine and everything after them is garbage');
});

// ---------------------------------------------------------------------------
// THE OPS COLUMN: the per-track width the engine publishes, and the copy of
// FIELDS_PER_NOTE that the renderer keeps so it can lay a box out without
// importing the model.
// ---------------------------------------------------------------------------

test("tracker.js's FIELDS_PER_NOTE equals the model's", async () => {
  /*
   * The renderer needs to know which column the ops glyphs are in — it is the column that
   * disappears on a track with no ops — and it must not import the model to find out, because
   * the rule that keeps the paint and the hit test agreeing is that geometry lives in one file.
   *
   * So it holds a copy, and this is what stops a copy from rotting. Both numbers are read out
   * of the SOURCE rather than imported, because importing tracker.js needs a DOM.
   */
  const src = readFileSync(new URL('../src/tracker.js', import.meta.url), 'utf8');
  const m = src.match(/^const FIELDS_PER_NOTE = (\d+);/m);
  assert.ok(m, 'tracker.js declares FIELDS_PER_NOTE');
  const vm = readFileSync(new URL('../src/viewmodel.js', import.meta.url), 'utf8');
  const v = vm.match(/^export const FIELDS_PER_NOTE = (\d+);/m);
  assert.ok(v, 'viewmodel.js declares FIELDS_PER_NOTE');
  assert.equal(Number(m[1]), Number(v[1]),
    `the renderer's copy of FIELDS_PER_NOTE is ${m[1]} and the model's is ${v[1]} — `
    + 'the ops column would be laid out at one index and drawn at another');

  // And that the ops field is the LAST of a note's fields. `cellLeft` counts hidden columns as
  // `floor(col / FIELDS_PER_NOTE)`, and `hitTest` inverts it as `(vis / (FIELDS_PER_NOTE - 1))`;
  // both are only right when the hidden column is the group's last.
  const o = src.match(/^const OPS_FIELD = (\d+);/m);
  assert.ok(o, 'tracker.js declares OPS_FIELD');
  assert.equal(Number(o[1]), Number(m[1]) - 1,
    'the ops column is the last field of a note group — cellLeft and hitTest both assume it');
});

test('the ops column is hidden exactly when no note in the track carries one', async () => {
  /*
   * `computeOpsShow` is not exported (nothing in this file is, past the model), so this drives
   * it through `buildViewModel` — which is the honest instrument anyway: the question is what
   * the RENDERER is handed, and a direct call on the helper could pass while the plumbing that
   * carries it to `buf.opsShow` is missing. That plumbing is exactly what broke the first time
   * a per-track array was added here.
   */
  const engine = {
    opsWidth: new Uint8Array([0, 3, 0, 7]),
    extentCount: 0, extents: [], extentsRevision: 1,
    noteCount: 0, notes: [], notesRevision: 1,
    aggCount: 0, aggs: [], aggsRevision: 1,
    lpb: new Uint8Array(64), trackParent: new Uint32Array(4), trackFlags: new Uint8Array(4),
    chordCount: 0, chords: [], chordsRevision: 0,
    clipVersion: 1, trackCount: 4, quantize: [], quantizeVersion: 0,
    harmony: null, names: [], tempoMilliBpm: 120000,
  };
  const vm = buildViewModel({ startRow: 0, rowCount: 4, tracks: 4, columns: 3, engine }, null);
  assert.deepEqual([...vm.opsShow].slice(0, 4), [0, 1, 0, 1],
    'tracks 1 and 3 carry ops and draw the column; 0 and 2 do not');
  assert.deepEqual([...vm.opsWidth].slice(0, 4), [0, 3, 0, 7],
    'and the widths come through, which is what sizes the cell');

  /*
   * THE SIGNATURE MOVES WHEN A WIDTH MOVES, not only when the on/off set does.
   *
   * The renderer early-returns on an unchanged signature and sizes the ops cell from the glyph
   * count — so a track whose run grows from three ops to five changes the LAYOUT without
   * changing the set, and a signature built from the set alone would leave the cell at its old
   * width and clip the two new glyphs. Silently: the note carries them, the engine plays them,
   * and the cell shows seven of nine.
   *
   * This is GUIDELINES 2.1 for the third time on this one feature (the renderer's SIG, the
   * header's rebuild key, and now this), which is why it is asserted rather than reasoned about.
   */
  const wider = { ...engine, opsWidth: new Uint8Array([0, 5, 0, 7]) };
  const vmWider = buildViewModel(
    { startRow: 0, rowCount: 4, tracks: 4, columns: 3, engine: wider }, null);
  assert.deepEqual([...vmWider.opsShow].slice(0, 4), [0, 1, 0, 1],
    'the same set of tracks still draws the column');
  assert.notEqual(vmWider.opsShowSig, vm.opsShowSig,
    'but the signature moved, because the WIDTH did — a set-only signature would leave the '
    + 'cell at its old size and clip the glyphs the run just gained');

  // The override is ADDITIVE and cannot subtract: forcing track 0 on shows it, and "forcing"
  // track 1 off does not hide a column whose glyphs would then have nowhere to go.
  const forced = new Uint8Array([1, 0, 0, 0]);
  const vm2 = buildViewModel(
    { startRow: 0, rowCount: 4, tracks: 4, columns: 3, engine, opsOverride: forced }, null);
  assert.deepEqual([...vm2.opsShow].slice(0, 4), [1, 1, 0, 1],
    'the override reveals a column the width would hide, and never hides one it shows');
  assert.notEqual(vm2.opsShowSig, vm.opsShowSig,
    'and the signature moved with it, so the renderer rebuilds rather than early-returning');

  // A NEGATIVE CONTROL, because "the array came back all ones" is what a broken read looks
  // like: an engine that publishes no width at all draws no ops column anywhere.
  const vm3 = buildViewModel(
    { startRow: 0, rowCount: 4, tracks: 4, columns: 3, engine: { ...engine, opsWidth: null } },
    null);
  assert.deepEqual([...vm3.opsShow].slice(0, 4), [0, 0, 0, 0],
    'no published width means no column — not every column');
});

test('no console command shadows another', () => {
  /*
   * THE REGISTRY IS AN OBJECT LITERAL, so a duplicate key does not collide — it SILENTLY
   * REPLACES. Adding a second `ops` deleted `ops [tokens]` (set the row ops on the note at the
   * cursor) and thirty checks in ops.mjs failed at once, all of them reporting that the op
   * parser had started rejecting valid tokens.
   *
   * `createCommands` cannot answer this: by the time it returns, the loser is gone and the
   * object looks perfectly well-formed. So this reads the SOURCE, which is the only place both
   * names still exist.
   *
   * Keys are matched at the registry's own indentation (four spaces) to avoid the `run:` bodies
   * and nested option objects; the count assertion below is what makes that safe to trust, the
   * same guard every source-reading check in this file carries.
   */
  const src = readFileSync(new URL('../src/dock.js', import.meta.url), 'utf8');
  const keys = [...src.matchAll(/^ {4}(?:'([a-z0-9-]+)'|([a-z][a-zA-Z0-9-]*)): \{ help:/gm)]
    .map((m) => m[1] || m[2]);
  const declared = Object.keys(createCommands(stubApi()));
  assert.ok(keys.length >= declared.length,
    `the source scan found ${keys.length} commands and the registry has ${declared.length} — `
    + 'the pattern has stopped matching the file');
  const seen = new Set(), dupes = [];
  for (const k of keys) { if (seen.has(k)) dupes.push(k); seen.add(k); }
  assert.deepEqual(dupes, [],
    'a command name declared twice — the later one silently deletes the earlier');
});

test("the sidecar's slot-name length matches the engine's array", async () => {
  /*
   * A SECOND COPY OF SOMEONE ELSE'S RULE, which is normally the wrong instinct and earns its
   * place here: the engine REFUSES an over-long slot name rather than shortening it, and its
   * refusal is a log event. From a browser that is a command that reports success and does
   * nothing — the failure mode this repo keeps finding, most recently when the sidecar's slot
   * FIELD bound went stale at 26 and killed two commands outright.
   *
   * So the sidecar checks the length too, and this holds its copy equal to the C++ constant.
   * Same shape as the field-id bound above and for the same reason.
   */
  const hdr = readFileSync(new URL('../../apps/shared_memory.h', import.meta.url), 'utf8');
  const cpp = hdr.match(/kUiSamplerSlotNameBytes\s*=\s*(\d+)/);
  assert.ok(cpp, 'the engine declares kUiSamplerSlotNameBytes');
  const rust = readFileSync(new URL('../../ui/daw-sidecar/src/main.rs', import.meta.url), 'utf8');
  const mine = rust.match(/const SAMPLER_SLOT_NAME_BYTES: usize = (\d+);/);
  assert.ok(mine, 'the sidecar declares SAMPLER_SLOT_NAME_BYTES');
  assert.equal(Number(mine[1]), Number(cpp[1]),
    `the sidecar allows ${mine[1]} bytes and the engine's array is ${cpp[1]} — a name between `
    + 'the two would be sent, refused into the log, and reported as success');
});

test('the optimistic velocity and the settled one are formatted the same way', async () => {
  /*
   * A TYPED VELOCITY FLASHED THE WRONG NUMBER FOR ONE ROUND TRIP.
   *
   * The settled cell draws hex — `velocityText` — and the optimistic overlay built its own
   * string with `('0' + v).slice(-2)`, the last two DECIMAL digits. That is verbatim the formula
   * viewmodel.js's own comment records as the bug it fixed: 127 reads as "27", 100 as "00", both
   * plausible velocities and neither the one you typed. It settled to the right value when the
   * engine answered, which is what made it survive — a wrong number that corrects itself is
   * harder to notice than one that stays.
   *
   * Read out of the SOURCE rather than exercised, because the overlay is a draw-path detail with
   * no accessor: the claim is that index.html calls the shared formatter instead of repeating
   * its logic, and that is a claim about the text.
   */
  const src = readFileSync(new URL('../index.html', import.meta.url), 'utf8');
  assert.ok(/addPending\([^)]*velocityText\(/.test(src),
    'the pending velocity is formatted by velocityText, not by a second copy of the rule');
  assert.ok(!/addPending\([^)]*'0' \+ v\)/.test(src),
    'and the decimal two-digit formula is gone from the pending write');

  // ...and the shared formatter really is hex, so the check above is worth passing. (The full
  // table is asserted further up; these are the two the decimal formula got wrong.)
  assert.equal(velocityText(127), '7f', '127 is 7f, not "27"');
  assert.equal(velocityText(100), '64', '100 is 64, not "00"');
});

test("the sidecar's patcher node-type bound matches the engine's ceiling", async () => {
  /*
   * THE THIRD TIME A STALE BOUND HAS KILLED A COMMAND, and the second one found by writing a
   * manual rather than by anything failing.
   *
   * `build_patcher_graph` refused `nodeType > 6` with a comment naming EventOut as the last
   * type. SliceSelect = 7 landed, so the node became one the graph could hold, the project could
   * save and the palette offered — and the ADD COMMAND WOULD NOT CREATE IT. Reachable only by
   * hand-editing JSON. The engine hit the identical bound and answered it by adding
   * `kPatcherNodeTypeMax` with the lesson written above it: a bound that names a member goes
   * stale the next time the enum grows.
   *
   * So the sidecar's copy is held to that constant. The sibling of the slot-field bound check
   * above, and written the same way for the same reason.
   */
  const hdr = readFileSync(new URL('../../apps/patcher_graph.h', import.meta.url), 'utf8');
  const block = hdr.slice(hdr.indexOf('enum class PatcherNodeType'));
  const ids = [...block.slice(0, block.indexOf('};')).matchAll(/^\s*([A-Z][A-Za-z]*)\s*=\s*(\d+)/gm)]
    .map((m) => [m[1], Number(m[2])]).sort((a, b) => a[1] - b[1]);
  assert.ok(ids.length > 5, `parsed PatcherNodeType: ${ids.length} types`);
  const max = ids[ids.length - 1];

  // The engine's own constant must still name the LAST member, or its ceiling has gone stale in
  // the way its comment warns about — and this check would then be holding us to a stale number.
  const named = hdr.match(/kPatcherNodeTypeMax\s*=\s*PatcherNodeType::([A-Za-z]+)/);
  assert.ok(named, 'the engine declares kPatcherNodeTypeMax');
  assert.equal(named[1], max[0],
    `kPatcherNodeTypeMax names ${named[1]} and the enum's last member is ${max[0]}`);

  const rust = readFileSync(new URL('../../ui/daw-sidecar/src/main.rs', import.meta.url), 'utf8');
  const mine = rust.match(/const PATCHER_NODE_TYPE_MAX: u32 = (\d+);/);
  assert.ok(mine, 'the sidecar declares PATCHER_NODE_TYPE_MAX');
  assert.equal(Number(mine[1]), max[1],
    `the sidecar accepts node types 0..${mine[1]} and the engine's last is ${max[0]} = ${max[1]}`
    + ' — anything above the bound is refused on the way out and cannot be created at all');

  /*
   * ...AND EVERY TYPE HAS PORTS, which is the second half of the same failure: past the bound,
   * `link` still answered "those two node types have no compatible ports" because `ports_for`
   * had no arm for 7. A node you can create and cannot connect is a node you cannot use.
   */
  const table = rust.slice(rust.indexOf('fn ports_for'));
  const arms = new Set([...table.slice(0, table.indexOf('\n}')).matchAll(/^\s*(\d+) =>/gm)]
    .map((m) => Number(m[1])));
  const missing = ids.map(([, v]) => v).filter((v) => !arms.has(v));
  assert.deepEqual(missing, [],
    `node types with no entry in ports_for: ${JSON.stringify(missing)} — they can be created `
    + 'and never wired');
});

test('the waveform cache key is built in exactly one place', async () => {
  /*
   * FIVE SPELLINGS OF ONE KEY, and updating four of them broke two features in opposite
   * directions within one edit.
   *
   * The key identifies a waveform window: source, sampler address, decimation, first frame. It
   * was a hand-written template string in the requester, the answer handler, the arrangement's
   * painter, the rack's painter and the `__uni.waveform` fixture. When the sampler address
   * joined it, I updated three — the rack's painter then looked up a window that had ARRIVED and
   * every instrument said "not arrived", and then fixing that broke every waveform in the
   * ARRANGEMENT, which had the old spelling and was suddenly a stranger to the cache.
   *
   * Both failures are silent and neither points at the key: one is an empty view with a complete
   * model, the other is a golden that moved by 67,898 pixels.
   *
   * So: one builder in wire.js, and this refuses a sixth. Source-read rather than exercised,
   * because the point is that no OTHER site constructs one — which is a claim about the text.
   */
  const { readFileSync } = await import('node:fs');
  const files = ['../index.html', '../src/arrange.js', '../src/chain.js', '../src/wire.js'];
  const offenders = [];
  for (const f of files) {
    const src = readFileSync(new URL(f, import.meta.url), 'utf8');
    for (const line of src.split('\n')) {
      // A concatenation or template that joins something to `:<decimation>:` is this key being
      // built by hand. The builder's own body is the one legal instance.
      if (/return .*\+ ':' \+ decimation \+ ':' \+ firstFrame/.test(line)) continue;
      if (/(\+ ':' \+ (dec|decim|decimation)\b)|(\$\{(dec|decim|decimation)\}:)/.test(line)
          && !/^\s*[*/]/.test(line)) {
        offenders.push(`${f.split('/').pop()}: ${line.trim().slice(0, 80)}`);
      }
    }
  }
  assert.deepEqual(offenders, [],
    'a waveform cache key built somewhere other than wire.js\'s `waveKey` — five spellings is '
    + 'how a window that had arrived was reported missing, and how fixing that broke the '
    + 'arrangement');
});

test('the instrument-kind mirror still matches the engine', async () => {
  /*
   * THE RULE LIVES IN C++ AND IS MIRRORED IN JS, SO THE MIRROR IS CHECKED AGAINST IT.
   *
   * `chainHasInstrument` decides whether the UI refuses a second instrument in words or lets
   * the engine refuse it with a numeric code. It listed two kinds; the engine's
   * `isInstrumentKind` lists three. The sampler had been added engine-side after I reported it
   * missing, and this side never followed — while a comment here went on stating the old rule
   * and CITING device_chain.cpp as its authority.
   *
   * The result was the ordinary case failing: put a sampler on a track, add a plugin, get
   * "chain error on track 0 (code 1)". A stale mirror does not announce itself, and a comment
   * cannot notice that the file it quotes has changed.
   *
   * So the check reads the predicate out of the engine and compares. It fails on a kind added
   * to either side alone, which is the only failure mode a mirror has.
   */
  const { readFileSync } = await import('node:fs');
  const cpp = readFileSync(new URL('../../apps/device_chain.cpp', import.meta.url), 'utf8');
  const body = cpp.match(/bool isInstrumentKind\([^)]*\)\s*\{([\s\S]*?)\}/);
  assert.ok(body, 'isInstrumentKind was not found in apps/device_chain.cpp — if it moved or was '
                + 'renamed, this check is blind and must be repointed, not deleted');
  const engineKinds = [...body[1].matchAll(/DeviceKind::(\w+)/g)].map((m) => m[1]).sort();

  const html = readFileSync(new URL('../index.html', import.meta.url), 'utf8');
  const list = html.match(/const INSTRUMENT_KINDS = \[([^\]]*)\]/);
  assert.ok(list, 'INSTRUMENT_KINDS was not found in index.html');
  // The mirror names kinds by constant; map them onto the engine's spelling to compare.
  const NAMES = { KIND_PATCHER_INSTRUMENT: 'PatcherInstrument', KIND_VST_INSTRUMENT: 'VstInstrument',
                  KIND_SAMPLER: 'Sampler' };
  const uiKinds = list[1].split(',').map((x) => x.trim()).filter(Boolean)
                         .map((x) => NAMES[x] || `UNMAPPED(${x})`).sort();

  assert.deepEqual(uiKinds, engineKinds,
    `the UI's instrument kinds ${JSON.stringify(uiKinds)} no longer match the engine's `
    + `${JSON.stringify(engineKinds)}. A kind on the engine's list and not the UI's means the UI `
    + `sends an AddDevice the engine refuses with a numeric code; the other way round means the `
    + `UI refuses something that is allowed. Fix INSTRUMENT_KINDS in index.html — and if a kind `
    + `is genuinely new, add it to NAMES here too.`);
});

test('every row op can actually be TYPED into the cell', async () => {
  /*
   * THE CELL'S CHARSET IS AN ALLOWLIST AND IT WENT STALE.
   *
   * `ENTRY_MODES.ops` filters keystrokes to a regex. It was `/[0-9a-zA-Z /]/` — letters, digits,
   * space, slash — which covered every op that existed when it was written. v33 added two that
   * need punctuation and the charset was not revisited:
   *
   *   rv-60   a SIGNED total percent   ->  the `-` was dropped, so a -60 ramp applied as +60
   *   c1:2    a trig condition         ->  the `:` was dropped, so `c12` applied nothing
   *
   * Silent both times, and worse than a refusal: the filter drops the character and keeps going,
   * so the buffer looks fine and commits something else. Every existing test passed because
   * ops.mjs writes ops through the CONSOLE (`run('ops ret4 rv-60 c1:2')`), which has no charset.
   *
   * So the charset is checked against the ops' own documented examples. An op that needs a
   * character its cell cannot accept now fails here rather than in someone's hands.
   */
  const { readFileSync } = await import('node:fs');
  const html = readFileSync(new URL('../index.html', import.meta.url), 'utf8');
  const m = html.match(/ops:\s*\{\s*charset:\s*(\/\[[^\]]*\]\/)/);
  assert.ok(m, 'ENTRY_MODES.ops charset not found — if it moved, repoint this, do not delete it');
  const charset = new RegExp(m[1].slice(1, -1));

  const unusable = [];
  for (const op of ROW_OPS) {
    // The op's own example is the authority: it is what the help overlay shows and what a
    // person copies.
    for (const ch of op.example) {
      if (!charset.test(ch)) unusable.push(`${op.example} needs ${JSON.stringify(ch)}`);
    }
  }
  assert.deepEqual(unusable, [],
    'a row op whose own example cannot be typed into the cell. The charset silently DROPS a '
    + 'character it does not allow rather than refusing the edit, so the op commits as something '
    + 'else. Add the character to ENTRY_MODES.ops in index.html.');
});

test('the roman numerals are spelled out in exactly ONE place', async () => {
  /*
   * THERE WERE THREE. harmonymodel.js drew the lane and the tracker cell, inspectmodel.js wrote
   * the CELL panel, and index.html labelled the optimistic edit that appears before the engine
   * answers — each with its own `['I','II','III',...]` and its own hand-built suffix logic.
   *
   * They agreed while the rule was "upper case, plus a 7". The moment the numeral started
   * carrying QUALITY — `vi` rather than `VI` — three copies meant the grid, the panel and the
   * label could each be right about a different chord. The optimistic one is the worst of the
   * three: it is drawn a frame before the engine's answer replaces it, so a divergence reads as
   * a cell that changes its own text after you type.
   *
   * All three call `nameChord` now. This asserts the copies did not come back — which is a
   * claim about the TEXT, because that is what "no other site spells this out" means.
   */
  const { readFileSync } = await import('node:fs');
  const files = ['../src/harmonymodel.js', '../src/inspectmodel.js', '../src/viewmodel.js',
                 '../src/arrange.js', '../src/chain.js', '../index.html'];
  const spellers = [];
  for (const rel of files) {
    const src = readFileSync(new URL(rel, import.meta.url), 'utf8');
    // A list that starts 'I','II' is a numeral table by any name.
    if (/\[\s*'I'\s*,\s*'II'\s*,/.test(src)) spellers.push(rel.split('/').pop());
  }
  assert.deepEqual(spellers, ['harmonymodel.js'],
    'a roman-numeral table outside harmonymodel.js. `nameChord` is the speller — a second table '
    + 'will agree with it right up until the naming rule changes, and then it will be wrong on '
    + 'screen next to the right one.');
});

test('the numeral carries the chord quality, the way musicians write it', async () => {
  /*
   * CASE IS INFORMATION. `I-V-vi-IV` says at a glance that the sixth is minor; `I-V-VI-IV`
   * throws that away and the numerals stop carrying the one thing the notation exists for.
   * Every numeral here was upper case, so every progression read as though it were all major.
   *
   * DERIVED FROM THE SCALE'S OWN STEPS, not from a table of the major modes — `stepCents` is
   * published for every scale the engine knows, so this answers for the exotic ones instead of
   * assuming everything is major-ish.
   */
  const { nameChord, triadQuality } = await import('../src/harmonymodel.js');
  /*
   * COPIED VERBATIM OFF THE WIRE, and that is the point of this comment.
   *
   * `stepCents` is CUMULATIVE — one offset from the root per degree — not the per-step deltas
   * the name suggests. The first version of this test invented [200,200,100,...] deltas, the
   * first version of the code read them as deltas, and the two agreed perfectly while being
   * wrong about the data. It passed. The app drawing `VI` where `vi` belonged is what caught it.
   *
   * So these are the exact arrays the sidecar sends, and a scale record is `{stepCents,
   * octaveCents}` because the wrap past the last degree needs the octave.
   */
  const MAJOR = { stepCents: [0, 200, 400, 500, 700, 900, 1100], octaveCents: 1200 };
  const MINOR = { stepCents: [0, 200, 300, 500, 700, 800, 1000], octaveCents: 1200 };

  assert.deepEqual([0, 1, 2, 3, 4, 5, 6].map((d) => triadQuality(d, MAJOR.stepCents)),
    ['major', 'minor', 'minor', 'major', 'major', 'minor', 'dim'],
    'the diatonic triads of a major scale');
  assert.deepEqual([0, 1, 2, 3, 4, 5, 6].map((d) => triadQuality(d, MINOR.stepCents)),
    ['minor', 'dim', 'major', 'minor', 'minor', 'major', 'major'],
    'and of a natural minor');

  assert.equal([0, 1, 2, 3, 4, 5, 6].map((d) => nameChord(d, 1, 0, MAJOR)).join(' '),
    'I ii iii IV V vi vii°', 'the major scale, spelled');
  assert.equal([0, 1, 2, 3, 4, 5, 6].map((d) => nameChord(d, 1, 0, MINOR)).join(' '),
    'i ii° III iv v VI VII', 'and the natural minor');

  // The progression everyone knows, entered the way a person says it.
  assert.equal([1, 5, 6, 4].map((n) => nameChord(n - 1, 1, 0, MAJOR)).join('-'),
    'I-V-vi-IV');

  /*
   * WITHOUT A SCALE THE NUMERAL IS NOT CASED. That is the honest rendering of "the quality is
   * not established here" — guessing major would be a claim the document does not make, and the
   * tracker draws chords before the harmony timeline has necessarily been read.
   */
  assert.equal(nameChord(5, 1, 0), 'VI', 'no scale, no claim about quality');
  assert.equal(nameChord(5, 1, 0, MAJOR), 'vi', 'with the scale, the case says minor');

  // A single note is not a chord and must not be cased as one.
  assert.equal(nameChord(5, 0, 0, MAJOR), 'VI', 'quality 0 is one note, not a minor triad');
  // The seventh marker and the inversion still ride along.
  assert.equal(nameChord(5, 2, 1, MAJOR), 'vi7/1');

  /*
   * THE INTERN IS KEYED ON THE SCALE TOO. Keyed only on (degree, quality, inversion) it would
   * hand back the previous key's casing after a modulation — a cached answer to a different
   * question, which is how two chord bugs got here already.
   */
  assert.equal(nameChord(5, 1, 0, MAJOR), 'vi');
  assert.equal(nameChord(5, 1, 0, MINOR), 'VI', 'the same degree, cased for the key it is in');

  // A scale that is not seven notes has no diatonic triads to speak of; say nothing rather than
  // inventing a quality.
  const SIX = { stepCents: [0, 200, 400, 600, 800, 1000], octaveCents: 1200 };
  assert.equal(triadQuality(0, SIX.stepCents), null);
  assert.equal(nameChord(0, 1, 0, SIX), 'I');
});

test('only meter.js decides how long a bar is', async () => {
  // WHY THIS IS A SOURCE SCAN AND NOT A BEHAVIOUR TEST. The rule is one line of arithmetic used
  // in draw paths that need a DOM, so the only cheap way to keep it in one place is to check
  // that nobody re-declares it. That is worth doing because the copies are invisible when you
  // are in 4/4: chrome.js carried `const BEATS_PER_BAR = 4` AND treated a quarter as the beat,
  // which are two separate 4/4 assumptions stacked. In 6/8 the first counts a six-beat bar in
  // fours and the second calls an eighth a quarter, so the position readout under the playhead
  // disagreed with the ruler the notes are drawn against — and every test we have is in 4/4,
  // where both mistakes are exactly right.
  //
  // meter.js is the one place allowed to know it: `ticksPerBeat` is (QUARTER * 4) / denominator
  // and `ticksPerBar` multiplies by the numerator.
  const { readFileSync, readdirSync } = await import('node:fs');
  const dir = new URL('../src/', import.meta.url);
  const offenders = [];
  for (const f of readdirSync(dir).filter((n) => n.endsWith('.js') && n !== 'meter.js')) {
    const src = readFileSync(new URL(f, dir), 'utf8');
    for (const [i, line] of src.split('\n').entries()) {
      if (line.trimStart().startsWith('//') || line.trimStart().startsWith('*')) continue;
      // A named bar/beat length is FINE when it comes from meter.js — arrangemodel, harmonymodel
      // and viewmodel all legitimately keep one, derived via ticksPerBar(meter). What is not fine
      // is a NUMBER: only a literal encodes a signature, and 3840000 is a 4/4 bar written out.
      const decl =
        /\b(?:const|let|var)\s+\w*(?:BEATS_PER_BAR|TICKS_PER_BAR|BAR_TICKS|BAR_BEATS)\w*\s*=(.*)$/
          .exec(line);
      if (decl && /\d/.test(decl[1]) && !/ticksPer(?:Bar|Beat)\s*\(/.test(decl[1])) {
        offenders.push(`${f}:${i + 1}  ${line.trim()}`);
      }
    }
  }
  assert.deepEqual(offenders, [],
    'a bar length declared outside meter.js. Use ticksPerBar(meter) / ticksPerBeat(meter) — a '
    + 'private 4 is invisible until somebody writes in 6/8, and then the readout and the ruler '
    + 'disagree about where the playhead is');
});

test('the note detector recovers pitches it was never told', async () => {
  /*
   * THE AUDIO ASSERTIONS IN THIS REPO ALL ASKED "WAS THERE A LEVEL HERE" — peak, RMS,
   * loud-fraction, envelope shape. None of them can answer the questions that matter about a
   * DAW: did ALL the notes play, is the harmony system applied to this track, is that a strum.
   * A track playing entirely the wrong notes has the same RMS as one playing the right ones.
   *
   * `notes.mjs` answers those, so it needs its own net. Synthetic tones here — deterministic, no
   * engine, no render — and the real validation lives where it belongs: rendered engine audio,
   * in chord-render and harmony-quantize.
   *
   * IT WAS WRONG THE FIRST TIME AND THIS IS HOW I KNOW. Written with plain autocorrelation and a
   * "prefer the shortest nearly-as-good lag" rule to dodge the octave error, it reported a
   * synthesised C-2 as B-6 and put every other tone exactly one semitone sharp — because at tiny
   * lags any smooth waveform correlates with itself near 1.0. Calibrating against tones of KNOWN
   * pitch caught it in one run. Against real audio it would have produced plausible wrong numbers,
   * which is the shape that gets believed.
   */
  const { fundamental, nearestNote, noteName, detectNotes } = await import('./notes.mjs');
  const RATE = 44100;

  /** A note with harmonics — the case a naive detector gets an octave wrong. */
  const tone = (midi, seconds = 0.3, harmonics = [1, 0.5, 0.25]) => {
    const f = 440 * Math.pow(2, (midi - 69) / 12);
    const n = Math.round(RATE * seconds);
    const s = new Float64Array(n);
    for (let i = 0; i < n; i++) {
      let v = 0;
      harmonics.forEach((amp, h) => { v += amp * Math.sin((2 * Math.PI * f * (h + 1) * i) / RATE); });
      s[i] = v * 0.3;
    }
    return s;
  };

  for (const midi of [36, 43, 48, 55, 60, 64, 67, 72, 79, 84]) {
    const got = fundamental(tone(midi), RATE);
    assert.ok(got, `no pitch found for ${noteName(midi)}`);
    const near = nearestNote(got.freq);
    assert.equal(near.midi, midi,
      `${noteName(midi)} came back as ${noteName(near.midi)} (${got.freq.toFixed(1)}Hz)`);
    assert.ok(Math.abs(near.cents) <= 12, `${noteName(midi)} off by ${near.cents} cents`);
  }

  /*
   * THE OCTAVE TRAP, on its own. A tone whose SECOND harmonic is louder than its first is what
   * makes an FFT-peak detector report an octave up, and it is common in real instruments.
   */
  const bright = fundamental(tone(60, 0.3, [0.3, 1.0, 0.6]), RATE);
  assert.equal(nearestNote(bright.freq).midi, 60,
    'a tone whose second harmonic dominates is still its own fundamental, not an octave up');

  /*
   * AND A SEQUENCE, so the onset half is covered too: four notes with silence between them come
   * back as four notes at the right pitches, in order.
   */
  const gap = new Float64Array(Math.round(RATE * 0.25));
  const want = [60, 64, 67, 72];
  const parts = [];
  for (const m of want) { parts.push(tone(m, 0.4), gap); }
  const seq = new Float64Array(parts.reduce((n, p) => n + p.length, 0));
  let at = 0;
  for (const p of parts) { seq.set(p, at); at += p.length; }

  const found = detectNotes(seq, RATE);
  assert.deepEqual(found.map((n) => n.midi), want,
    `a four-note line came back as ${JSON.stringify(found.map((n) => n.name))}`);
  // And roughly where they were put: 0.65s apart, allowing a frame either way.
  found.forEach((n, i) => {
    assert.ok(Math.abs(n.at - i * 0.65) < 0.05,
      `${n.name} detected at ${n.at}s, expected about ${(i * 0.65).toFixed(2)}s`);
  });

  /*
   * OVERLAPPING NOTES — THE CASE THAT WAS SILENTLY WRONG, AND THE ONE MUSIC ACTUALLY IS.
   *
   * The onset rule used to be: rise through the floor, and do not count again until you have
   * fallen back BELOW it. Correct for the sequence above, where the gaps are real silence, and
   * blind to anything that rings over the next note.
   *
   * It cost a real measurement. `demo_pluck_c4.wav` is 1.6 seconds and a sampler slot defaults to
   * one-shot, so the four notes the model wrote half a second apart overlapped and the level never
   * returned to the floor. The detector reported ONE note. That reads exactly like the engine
   * dropping three of them, and it is the failure mode this whole file exists to catch — so it
   * was about to be reported as an engine bug on the strength of a broken instrument.
   *
   * DECAYING tails, so the notes genuinely overlap and the level never reaches silence.
   */
  const decayed = (midi, seconds) => {
    const s2 = tone(midi, seconds);
    for (let i = 0; i < s2.length; i++) s2[i] *= Math.exp(-3 * (i / RATE));
    return s2;
  };
  const overlap = new Float64Array(Math.round(RATE * 4));
  want.forEach((m, i) => {
    const part = decayed(m, 1.6);
    const at2 = Math.round(i * 0.5 * RATE);
    for (let k = 0; k < part.length && at2 + k < overlap.length; k++) overlap[at2 + k] += part[k];
  });
  const over = detectNotes(overlap, RATE, { minConf: 0.4 });
  assert.deepEqual(over.map((n) => n.midi), want,
    `four OVERLAPPING notes came back as ${JSON.stringify(over.map((n) => n.name))} — a detector `
    + 'that needs silence between notes cannot answer "did all the notes play" for real music');
  over.forEach((n, i) => {
    assert.ok(Math.abs(n.at - i * 0.5) < 0.06,
      `overlapping ${n.name} detected at ${n.at}s, expected about ${(i * 0.5).toFixed(2)}s`);
  });

  /*
   * AND THE OTHER DIRECTION, which is the whole risk of the rise rule: one long tone must be ONE
   * note. A rise-based onset detector invents attacks out of ordinary envelope wobble, and a
   * detector that over-counts is worse than one that under-counts here — it would make "all the
   * notes played" pass on a part that played too many.
   *
   * A tremolo is the hostile case: amplitude swinging 2x, twelve times a second, with no attack
   * anywhere in it.
   */
  const trem = tone(60, 2.0);
  for (let i = 0; i < trem.length; i++) {
    trem[i] *= 0.6 + 0.3 * Math.sin((2 * Math.PI * 12 * i) / RATE);
  }
  const tremFound = detectNotes(trem, RATE);
  assert.equal(tremFound.length, 1,
    `a single tremolo'd tone must be ONE note, got ${tremFound.length} — a rise rule that fires `
    + 'on envelope wobble makes over-counting pass as "every note played"');

  // Silence is silence, not a confident guess.
  assert.deepEqual(detectNotes(new Float64Array(RATE), RATE), [],
    'silence must produce no notes at all');
});


/* ══════════════════════════════════════════════════════════════════════════════════════════
 * SHIPPED PRESETS, AND THE RUNBOOK THAT OPENS THEM
 *
 * Two classes of failure that are invisible until the room hears them: a preset that loads a
 * DIFFERENT plugin than the one it names, and a runbook that tells you to look for output no
 * script prints. Both happened this week. Neither had a check.
 *
 * Every rule here carries a control that breaks it, and the controls sabotage COPIES in a temp
 * directory — never the real presets, which a sweep may be reading while this runs.
 * ══════════════════════════════════════════════════════════════════════════════════════════ */

const PRESET_ROOT = new URL('../..', import.meta.url).pathname.replace(/\/$/, '');


/** Every plugin device in every shipped preset, as {file, track, device, ref}. */
function presetPluginDevices(dir = join(PRESET_ROOT, 'presets/projects')) {
  const out = [];
  for (const name of readdirSync(dir)) {
    if (!name.endsWith('.uniproj.json')) continue;
    const doc = JSON.parse(readFileSync(join(dir, name), 'utf8'));
    const tracks = [...(doc.tracks || [])];
    if (doc.master_track) tracks.push({ ...doc.master_track, track_id: 'master' });
    for (const t of tracks) {
      for (const d of (t.device_chain || [])) {
        if (d.vst_ref && (d.kind === 'vst_instrument' || d.kind === 'vst_effect')) {
          out.push({ file: name, track: t.track_id, device: d.device_id, ref: d.vst_ref,
                     slot: d.host_slot_index });
        }
      }
    }
  }
  return out;
}

/*
 * `resolveVstRef` from apps/device_chain.cpp, in its published order. A SECOND COPY, and it is
 * here rather than in the check because the point of the check is to run the engine's rule
 * against the presets — a rule invented for the test would prove the presets satisfy the test.
 */
function resolve(entries, ref) {
  const usable = (e) => e.scan_status === undefined || e.scan_status === 'ok' || !e.error;
  const ok = entries.map((e, i) => ({ e, i })).filter(({ e }) => usable(e));
  if (ref.uid16) {
    const hit = ok.find(({ e }) => e.uid16 === ref.uid16 || e.uid === ref.uid16);
    if (hit) return { how: 'uid16', i: hit.i };
  }
  if (ref.path && ref.name) {
    const hit = ok.find(({ e }) => e.path === ref.path && e.name === ref.name);
    if (hit) return { how: 'path+name', i: hit.i };
  }
  if (ref.name) {
    const hit = ok.find(({ e }) => e.name === ref.name
                                && (!ref.vendor || e.vendor === ref.vendor));
    if (hit) return { how: 'vendor+name', i: hit.i };
  }
  if (ref.path) {
    const at = ok.filter(({ e }) => e.path === ref.path);
    if (at.length === 1) return { how: 'path', i: at[0].i };
  }
  return { how: 'none', i: -1 };
}

test('no shipped preset can silently load a DIFFERENT plugin', () => {
  /*
   * MACHINE-INDEPENDENT, and the rule is about the SILENT swap rather than about strength.
   *
   * uid16, or path+name, are exact. A NAME with no path is weaker but still safe in the way that
   * matters: it either finds that product or finds nothing, and nothing is loud. That is how the
   * portable fixtures are written — the multiout build product lives at a different path in every
   * build dir — and the engine's own lint calls it a warning for exactly this reason.
   *
   * A PATH WITH NO NAME is the dangerous one, and it is the only shape banned here. Zebra2.vst3
   * holds four products at one path, so rule 4 either declines (silence) or, with a scan that
   * lists one, hands back whichever came first — a different plugin after any rescan. That is how
   * a saved Zebra2 came back as ZEBRIFY.
   *
   * I first wrote this as "path AND name required" and it failed on multiout, whose plugin is not
   * in this cache at all because the suites that use it scan their own build dir. The check was
   * wrong about the world, not the preset.
   */
  const weak = presetPluginDevices()
    .filter((d) => !d.ref.uid16 && !d.ref.name)
    .map((d) => `${d.file} track ${d.track} device ${d.device}: `
              + `path=${JSON.stringify(d.ref.path || '')} name=${JSON.stringify(d.ref.name || '')}`);
  assert.deepEqual(weak, [],
    'a shipped preset identifies a plugin by PATH ALONE. Zebra2.vst3 holds four products at one '
    + 'path, so that is a coin flip between them — and the winner changes when the scan is '
    + 'reordered, with nothing to say so.');
});

test('and the plugin it names is the one this machine resolves', () => {
  // The half that needs the cache. Skipping when it is absent would make the check disappear
  // exactly when the machine is misconfigured, so its absence FAILS.
  const cachePath = join(PRESET_ROOT, 'build/plugin_cache.json');
  assert.ok(existsSync(cachePath), `no ${cachePath} — every preset's plugin is unverifiable`);
  const raw = JSON.parse(readFileSync(cachePath, 'utf8'));
  const entries = Array.isArray(raw) ? raw : (raw.plugins || raw.entries || []);
  assert.ok(entries.length > 0, 'the plugin cache is empty');

  const wrong = [];
  for (const d of presetPluginDevices()) {
    const r = resolve(entries, d.ref);
    // NOT RESOLVING is not this check's business — a fixture whose plugin is built into a
    // suite's own scan dir is absent from this cache by design, and absence is loud anyway.
    // The runbook presets are held to more than that, one test below.
    if (r.i < 0) continue;
    const got = entries[r.i];
    // The NAME is the promise a preset makes. Resolving to a different product is the silent
    // swap the whole vst_ref mechanism exists to prevent.
    if (d.ref.name && got.name !== d.ref.name) {
      wrong.push(`${d.file} device ${d.device}: names ${d.ref.name}, resolves (${r.how}) to ${got.name}`);
    }
  }
  assert.deepEqual(wrong, [], 'a preset resolves to a different plugin than the one it names');
});

test('and every plugin the RUNBOOK opens resolves on this machine', () => {
  /*
   * A HIGHER BAR, for the presets a person actually opens on the day — and the list is READ FROM
   * THE RUNBOOK rather than typed here, so a demo that switches projects cannot leave this
   * checking the old one.
   *
   * For these, "resolves to nothing" IS the failure. The device loads as nothing, the track is
   * silent, and the first anyone knows is the room. Zebra2 loading UNLICENSED on this machine is
   * the same failure wearing a different hat, and it is why the demo project names Zebralette.
   */
  const runbook = readFileSync(join(PRESET_ROOT, 'docs/DEMO.md'), 'utf8');
  /*
   * ANY PRESET THE RUNBOOK MENTIONS, not just the one it says to load.
   *
   * The narrow reading — names in backticks — finds exactly `generator`, and it is the one the
   * runbook tells you to open. The wide one finds five, because they are all shipped projects
   * somebody may open on the day and a silent track is just as bad in any of them. The wide net
   * costs a preset being held to this because prose happened to name it, which is a cheap price
   * for not having to guess which four were safe.
   *
   * My first attempt derived the list from `load <word>` and found NOTHING, because the only
   * "load" in the runbook is `load demo_pluck_c4.wav` — a sample, not a project. It reported that
   * as a failure rather than passing on an empty list, which is the only reason I noticed.
   */
  const named = readdirSync(join(PRESET_ROOT, 'presets/projects'))
    .filter((f) => f.endsWith('.uniproj.json'))
    .map((f) => f.replace('.uniproj.json', ''))
    .filter((n) => new RegExp(`\\b${n.replace(/[-]/g, '\\$&')}\\b`).test(runbook));
  assert.ok(named.length > 0,
    'the runbook mentions no shipped project at all — this test went blind rather than red');

  const raw = JSON.parse(readFileSync(join(PRESET_ROOT, 'build/plugin_cache.json'), 'utf8'));
  const entries = Array.isArray(raw) ? raw : (raw.plugins || raw.entries || []);
  const dead = presetPluginDevices()
    .filter((d) => named.includes(d.file.replace('.uniproj.json', '')))
    .filter((d) => resolve(entries, d.ref).i < 0)
    .map((d) => `${d.file} device ${d.device}: ${JSON.stringify(d.ref)}`);
  assert.deepEqual(dead, [],
    `a project the runbook opens (${named.join(', ')}) names a plugin this machine cannot `
    + 'resolve — that track plays silence and nothing says so until the room hears it');
});

test('CONTROL: the resolver can tell the two products in one bundle apart', () => {
  // Without this, both checks above pass on a resolver that returns entry 0 for everything.
  const raw = JSON.parse(readFileSync(join(PRESET_ROOT, 'build/plugin_cache.json'), 'utf8'));
  const entries = Array.isArray(raw) ? raw : (raw.plugins || raw.entries || []);
  const bundle = new Map();
  for (const e of entries) bundle.set(e.path, (bundle.get(e.path) || 0) + 1);
  const shared = [...bundle.entries()].find(([, n]) => n > 1);
  assert.ok(shared, 'no multi-product bundle installed — this control cannot run');
  const [path] = shared;
  const members = entries.filter((e) => e.path === path);
  const a = resolve(entries, { path, name: members[0].name });
  const b = resolve(entries, { path, name: members[1].name });
  assert.notEqual(a.i, b.i,
    `both members of ${path} resolve to the same entry — path+name is not distinguishing them, `
    + 'so the checks above would pass whatever the presets say');
  assert.equal(entries[a.i].name, members[0].name);
  assert.equal(entries[b.i].name, members[1].name);
});

/*
 * ── THE NEGATIVE CONTROLS ───────────────────────────────────────────────────────────────────
 *
 * The three checks above are green, and green is what they would also be if they could not fail.
 * So each is re-run against a preset built to break it — in a TEMP DIRECTORY, never by editing
 * the real ones. A sweep reads these files while this runs, and a sabotage-then-restore that
 * lands in the window between is a suite failure nobody can explain.
 */
const sabotage = (name, mutate) => {
  const dir = mkdtempSync(join(tmpdir(), 'presetrefs-'));
  const doc = JSON.parse(readFileSync(join(PRESET_ROOT, `presets/projects/${name}.uniproj.json`), 'utf8'));
  for (const t of (doc.tracks || [])) {
    for (const d of (t.device_chain || [])) if (d.vst_ref) mutate(d.vst_ref);
  }
  writeFileSync(join(dir, `${name}.uniproj.json`), JSON.stringify(doc));
  return dir;
};

test('CONTROL: a path-only ref IS caught', () => {
  // The check above bans exactly this shape. Strip the name and keep the bundle path — the
  // Zebra2/Zebralette/ZRev/Zebrify coin flip.
  const dir = sabotage('maximal', (r) => { r.name = ''; r.uid16 = ''; });
  const weak = presetPluginDevices(dir).filter((d) => !d.ref.uid16 && !d.ref.name);
  assert.ok(weak.length > 0,
    'a preset with a bare bundle path passed the ban — the check cannot fail and proves nothing');
});

test('CONTROL: a runbook plugin that resolves to nothing IS caught', () => {
  const raw = JSON.parse(readFileSync(join(PRESET_ROOT, 'build/plugin_cache.json'), 'utf8'));
  const entries = Array.isArray(raw) ? raw : (raw.plugins || raw.entries || []);
  // `generator` is the project the runbook tells you to open. Name it after a plugin nobody has.
  const dir = sabotage('generator', (r) => { r.name = 'NoSuchPluginXYZ'; r.path = ''; r.uid16 = ''; });
  const dead = presetPluginDevices(dir).filter((d) => resolve(entries, d.ref).i < 0);
  assert.ok(dead.length > 0,
    'a runbook project naming an uninstalled plugin resolved anyway — the silent-track check '
    + 'is blind');
});

test('CONTROL: a ref that resolves to a DIFFERENT product IS caught', () => {
  /*
   * The shape check 2 exists for, and the one I nearly shipped without a control because I could
   * not think of a way to trigger it.
   *
   * It comes from rule 4. A ref carrying a name the scan no longer has, plus a path that holds
   * exactly one product, skips rules 1-3 and matches on the path alone — handing back whatever
   * now lives there under a different name. That is a plugin renamed by an update, or a bundle
   * whose contents changed, and the preset goes on claiming the old one.
   */
  const raw = JSON.parse(readFileSync(join(PRESET_ROOT, 'build/plugin_cache.json'), 'utf8'));
  const entries = Array.isArray(raw) ? raw : (raw.plugins || raw.entries || []);
  const counts = new Map();
  for (const e of entries) counts.set(e.path, (counts.get(e.path) || 0) + 1);
  const lone = entries.find((e) => counts.get(e.path) === 1);
  assert.ok(lone, 'no single-product bundle in the cache — this control cannot run');

  const r = resolve(entries, { path: lone.path, name: 'NameTheScanNoLongerHas', uid16: '', vendor: '' });
  assert.equal(r.how, 'path', `expected the bare-path rule to fire, got ${r.how}`);
  assert.notEqual(entries[r.i].name, 'NameTheScanNoLongerHas',
    'the resolver returned the name it was given, so check 2 has nothing to compare');
  assert.equal(entries[r.i].name, lone.name,
    `it resolved to ${entries[r.i].name} while the ref says NameTheScanNoLongerHas — which is `
    + 'exactly the mismatch check 2 reports, so that check can fail');
});

/*
 * ── THE RUNBOOK MAY NOT QUOTE OUTPUT NOTHING PRINTS ─────────────────────────────────────────
 *
 * Found by reading, which is the wrong way to find it. DEMO.md said:
 *
 *     Then check the line it prints:
 *         ask     enabled — key file /Users/jak/src/daw-web/.env
 *
 * No version of webstack.sh in this tree prints that. The real line is
 * "ask: an ANTHROPIC_API_KEY resolves". So the instruction on the morning of the demo was to
 * look for a string that would never appear — and the failure mode is the worst kind: you scan
 * the output, do not find it, and cannot tell "not printed" from "I missed it".
 *
 * The `say` helper prefixes with "> ", so a quoted line beginning "> " is a claim about what a
 * script says, and it is checkable against the scripts.
 */
const toolScripts = () => readdirSync(join(PRESET_ROOT, 'tools'))
  .filter((f) => f.endsWith('.sh'))
  .map((f) => readFileSync(join(PRESET_ROOT, 'tools', f), 'utf8'))
  .join('\n');

/*
 * Does any script print this line? Compared on the part BEFORE the em-dash, because the scripts
 * interpolate after it ($ask_key, a path, a timestamp) and a whole-line match would fail on lines
 * that are perfectly correct — which pushes the next person to delete the check instead of
 * fixing the claim.
 */
const scriptPrints = (scripts, line) => {
  const stem = line.split('—')[0].trim();
  return stem.length <= 6 || scripts.includes(stem);
};

test('every script line the runbook quotes is a line a script prints', () => {
  const runbook = readFileSync(join(PRESET_ROOT, 'docs/DEMO.md'), 'utf8');
  const scripts = toolScripts();

  // Only inside fenced blocks, and only lines starting with the `say` prefix — prose that
  // happens to begin with "> " is a markdown quote, not a claim about output.
  const fenced = [...runbook.matchAll(/```[a-z]*\n([\s\S]*?)```/g)].map((m) => m[1]);
  const quoted = fenced.flatMap((b) => b.split('\n'))
    .map((l) => l.trim())
    .filter((l) => l.startsWith('> ') && l.length > 12)
    .map((l) => l.slice(2).trim());

  // Compared on the DISTINCTIVE PREFIX rather than the whole line: the scripts interpolate
  // ($WEB, a timestamp), so a whole-line match would fail on lines that are perfectly correct
  // and push the next person to delete the check instead of the claim.
  const missing = quoted.filter((line) => !scriptPrints(scripts, line));
  assert.deepEqual(missing, [],
    'the runbook tells the reader to look for a line no script in tools/ prints. Not finding it '
    + 'on the day is indistinguishable from missing it.');
});

test('CONTROL: an invented output line IS caught', () => {
  /*
   * THE CONTROL I FIRST WROTE WAS BUILT ON A FALSE PREMISE, and that is worth the space.
   *
   * I "corrected" DEMO.md for quoting `ask     enabled — key file ...`, having grepped
   * webstack.sh, found `ask: an ANTHROPIC_API_KEY resolves`, and stopped at the first hit. The
   * script prints BOTH — one says whether a key resolves, the other says which file it came
   * from — and the runbook was right. This check, still in a scratch file, is what caught it.
   *
   * So the control asserts the PREDICATE, not a claim about any particular line: a line nobody
   * prints must be rejected, and a line somebody does print must be accepted. A control resting
   * on my reading of a script is a control resting on the thing that was wrong.
   */
  const scripts = toolScripts();
  assert.equal(scriptPrints(scripts, 'no script anywhere prints this sentence'), false,
    'an invented line was accepted — the check cannot fail and proves nothing');
  assert.equal(scriptPrints(scripts, 'ask     enabled — key file /somewhere/else/.env'), true,
    'a line a script really does print was rejected; only the text after the em-dash differs, '
    + 'which is interpolated and cannot be matched');
});
