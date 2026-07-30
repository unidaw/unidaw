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
import { readFileSync } from 'node:fs';

import { lcmGrid, ZOOM_LEVELS, buildViewModel, createBuffer } from '../src/viewmodel.js';
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
  // Jaakko's spec, verbatim: "z=C-3 while q=C-4, b=G-3, u=B-4, i=C-5, m=B-3",
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
const API_METHODS = ['setView', 'load', 'save', 'listProjects', 'transport', 'seek', 'tempo',
                     'note', 'del', 'goto', 'zoom', 'octave', 'gain', 'strip', 'state',
                     'engine', 'close', 'follow', 'rename', 'select', 'transpose', 'setLoop',
                     'nodes', 'addNode', 'delNode', 'linkNodes', 'patch', 'copy', 'paste',
                     'cut', 'addTrack', 'removeTrack', 'noteColumns', 'delDevice', 'bypass',
                     'quantize', 'moveDevice', 'chord', 'delChord', 'deleteHarmony',
                     'addDevice', 'openEditor', 'newSong', 'fold', 'edit', 'harmony', 'ask', 'forget',
                     'clips', 'moveClip', 'trimClip', 'delClip', 'addClip',
                     'selectedClip', 'ticksPerBar', 'master',
                     // The spine. Six, because a section has six things you can do to it
                     // and every one is reachable from both surfaces — the strip's click,
                     // drag and double-click all come through these same methods.
                     'sections', 'addSection', 'delSection', 'nameSection', 'secLength',
                     'moveSection',
                     // Modulation. `mapParam` takes a parameter INDEX and resolves the
                     // uid16 itself — the console should not have to type a 32-character
                     // hex string to map a knob.
                     'mods', 'mapParam', 'unmapParam', 'modDepth', 'macro'];

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
  assert.equal(checkArgs('gain', cmds.gain, ['16', '0']),
               'gain: <track> must be between 0 and 15, got 16',
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
  assert.equal(checkArgs('gain', cmds.gain, ['15', '-96']), null);
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
  // invisible — I probed it, got a rect back, and told Jaakko it was there.
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
  new:       { cli: null, agent: null, why: 'gap' },
  deldevice: { cli: null, agent: null, why: 'gap' },
  // Bypass reached the ENGINE from daw-cli first (`do set-bypass`, backend's
  // verb) and this app second, so the CLI path is real and the agent's manifest
  // is what still owes it a tool.
  bypass:    { cli: 'set-bypass', agent: null, why: 'gap' },
  // Reordering reached the engine from this app FIRST — daw-cli has no verb for it —
  // which is the opposite of the usual direction and worth recording as such.
  movedevice: { cli: null, agent: null, why: 'gap' },
  // Chords reached the CLI first (`do chord`) and this app's console never had them at
  // all — writing one meant typing a token into a cell, and removing one was impossible.
  chord:     { cli: 'chord', agent: null, why: 'gap' },
  delchord:  { cli: null,    agent: null, why: 'gap' },
  // The engine has taken DeleteHarmony since before this UI existed and nothing sent it,
  // so a key change could be added to the timeline and never taken off.
  delharmony: { cli: null,   agent: null, why: 'gap' },
  // M1.13. daw-cli shipped `do quantize` with the engine, so the CLI path is real
  // from day one; the agent's manifest still owes it a tool.
  quantize:  { cli: 'quantize', agent: null, why: 'gap' },
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
  mods:        { cli: null, agent: null, why: 'gap' },
  map:         { cli: 'mod-link', agent: null, why: 'gap' },
  unmap:       { cli: 'unmod-link', agent: null, why: 'gap' },
  // `depth` is `mod-link` again: the engine expresses "change the depth" as an add with the
  // same link id, so there is no separate verb on either side.
  depth:       { cli: 'mod-link', agent: null, why: 'gap' },
  /*
   * NO CLI VERB FOR THE MACRO KNOB. `mod-target` names a parameter and `mod-link` makes a
   * link, and nothing there turns a source — which means a link made from daw-cli cannot be
   * heard from daw-cli, because a macro nobody has turned is skipped by the applier.
   * Recorded as a gap on their side rather than left looking covered.
   */
  macro:       { cli: null, agent: null, why: 'gap' },
  sections:    { cli: 'arrangement', agent: null, why: 'gap' },
  section:     { cli: 'section', agent: null, why: 'gap' },
  delsection:  { cli: 'section', agent: null, why: 'gap' },
  namesection: { cli: 'section', agent: null, why: 'gap' },
  seclength:   { cli: 'section', agent: null, why: 'gap' },
  movesection: { cli: 'section', agent: null, why: 'gap' },
  editor:    { cli: null, agent: null, why: 'gap' },
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
  tempo:     { cli: 'set-tempo',   agent: null, why: 'gap' },
  gain:      { cli: 'mixer',       agent: null, why: 'gap' },
  mute:      { cli: 'mixer',       agent: null, why: 'gap' },
  solo:      { cli: 'mixer',       agent: null, why: 'gap' },
  undo:      { cli: null,          agent: 'undo',           why: 'gap' },
  redo:      { cli: null,          agent: 'redo',           why: 'gap' },
  rename:    { cli: null,          agent: 'set_track_name', why: 'gap' },
  stop:      { cli: null,          agent: 'transport',      why: 'gap' },
  // Document operations with NO programmatic path at all. The real hole.
  clear:     { cli: null, agent: null, why: 'gap' },
  copy:      { cli: null, agent: null, why: 'gap' },
  cut:       { cli: null, agent: null, why: 'gap' },
  paste:     { cli: null, agent: null, why: 'gap' },
  transpose: { cli: null, agent: null, why: 'gap' },
  loop:      { cli: null, agent: null, why: 'gap' },
  seek:      { cli: null, agent: null, why: 'gap' },
  addnode:   { cli: null, agent: null, why: 'gap' },
  delnode:   { cli: null, agent: null, why: 'gap' },
  link:      { cli: null, agent: null, why: 'gap' },
  patch:     { cli: null, agent: null, why: 'gap' },
  // View state. An agent has no viewport to address.
  // Edit mode is a property of the KEYBOARD, and an agent has no keyboard — it
  // calls add_notes, which writes regardless. So this is view state for the same
  // reason zoom is, even though it is a command a human wants in the palette.
  edit:      { cli: null, agent: null, why: 'view' },
  fold:      { cli: null, agent: null, why: 'view' },
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
  clips:        { cli: null,          agent: 'clips',           why: 'gap' },
  'move-clip':  { cli: null,          agent: 'move_clip',       why: 'gap' },
  'trim-clip':  { cli: null,          agent: 'trim_clip',       why: 'gap' },
  'del-clip':   { cli: null,          agent: 'remove_clip',     why: 'gap' },
  'add-clip':   { cli: null,          agent: 'add_clip',        why: 'gap' },
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

/** Ops with no CLI path today. This list may SHRINK, never grow. */
const CLI_GAP = ['add-clip', 'addnode', 'clear', 'clips', 'columns', 'copy', 'cut',
                 'del-clip', 'deldevice', 'delnode', 'editor', 'link', 'loop',
                 // Reordering reached the engine from this APP first — the usual
                 // direction is the other way round, so the CLI owes it a verb rather
                 // than the app owing the CLI one.
                 'delchord', 'delharmony', 'move-clip', 'movedevice', 'new', 'paste',
                 'patch', 'redo', 'rename', 'seek', 'stop', 'transpose', 'trim-clip',
                 'undo',
                 /*
                  * `mods` — reading what modulates what — and `macro` — turning the knob —
                  * have no CLI verb at all. The second is the one that matters: daw-cli can
                  * MAKE a modulation link and cannot make it audible, because a macro
                  * nobody has turned is skipped by the applier. Recorded rather than left
                  * looking covered.
                  */
                 'mods', 'macro'];
/** Ops with no agent tool today. Same rule. */
// `bypass` joins the list rather than being smuggled past it: the engine takes
// the command and daw-cli sends it, but the agent's manifest has no tool for it,
// so an agent asked to A/B an insert still cannot. Worth closing — comparing with
// and without a device is exactly the kind of judgement an agent should be able
// to make on its own — and worth recording honestly until it is.
const AGENT_GAP = ['addnode', 'bypass', 'chord', 'clear', 'columns', 'copy', 'cut',
                   'del', 'delchord', 'deldevice', 'delharmony', 'delnode', 'editor',
                   'gain', 'link', 'loop', 'movedevice', 'mute', 'new', 'paste', 'patch',
                   'quantize', 'seek', 'solo', 'tempo', 'transpose',
                   'delsection', 'movesection', 'namesection', 'seclength', 'section',
                   'sections', 'map', 'unmap', 'depth', 'macro', 'mods'];

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
   * Same shape as SetTrackHarmonyQuantize: the command is trivial, the RESULT is
   * unobservable. Nothing publishes whether the file was written — daw-cli learns the
   * path from the engine's stderr, which a browser cannot read — so a "save this graph as
   * a preset" button would report success it has no way to know about. Asked backend for
   * a completion event in the ClipRejected shape.
   */
  SavePatcherPreset: 'gap — no way to know whether the file was written, so the button would lie',
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
  SetTrackHarmonyQuantize: 'gap — the flag is writable but NOT published, so no control can show its state',
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
  SetAutomationTarget: 'gap — automation has no UI at all',
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
   * Automation has no surface at all: no lane, no curve, no way to see a point let
   * alone place one. SetAutomationTarget above is the other half of the same hole.
   */
  WriteAutomationPoint: 'gap — automation has no UI at all, see SetAutomationTarget',
};

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
