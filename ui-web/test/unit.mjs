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

import { lcmGrid, ZOOM_LEVELS } from '../src/viewmodel.js';
import {
  parseToken, parseChord, pitchOf, pitchToToken, hexValue, shiftDigit, NOTE_KEYS,
} from '../src/entry.js';
import {
  gainLabel, panLabel, faderPosition, gainAtPosition, GAIN_MIN, GAIN_MAX,
} from '../src/mixermodel.js';
import { isBlackKey, pitchLabel, fitLowPitch } from '../src/pianomodel.js';
import { describeConfig, configFields, nudgeConfig,
         NODE_TYPES } from '../src/patchermodel.js';
import { snapLoop, TICKS_PER_BAR } from '../src/arrangemodel.js';
import { velocityText } from '../src/viewmodel.js';
import { trackName } from '../src/arrangemodel.js';
import { createField, begin as fBegin, feed as fFeed, cancel as fCancel } from '../src/textfield.js';
import { fillRows, setProjectRow, setPluginRow, makeRow,
         KIND_PROJECT, KIND_PLUGIN } from '../src/browser.js';
import { ticksPerBar, ticksPerBeat, positionOf, createPosition, sameMeter,
         meterText } from '../src/meter.js';
import { buildChainModel, createChainBuffer, createParamEdits, findParamEdit,
         setParamEdit, dropParamEdit, reapParamEdits, MAX_PARAMS,
         EDIT_HOLD_MS } from '../src/chainmodel.js';

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
