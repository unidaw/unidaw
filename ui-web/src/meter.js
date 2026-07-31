// The song's meter, in one place.
//
// 4/4 was hardcoded as the literal 3840000 in arrangemodel.js, harmonymodel.js and
// viewmodel.js, and as the string '4/4' in chrome.js — four files each deriving
// where a bar starts, independently, from a constant none of them named. A project
// in 3/4 would have been mislabelled in four different ways, and fixing three of
// them would have looked like a fix.
//
// A clip owns its own meter now (ProjectClip.timeSigNumerator/Denominator, engine
// commit 3c56901), but bar NUMBERING stays global: the time gutter and the
// arrangement ruler count the song's bars, and a clip in 7/8 draws its own accents
// inside that. So there are two meters and they are not interchangeable — this
// module is the SONG's, the one you count in. A clip's meter belongs to the clip
// and is read from the clip.
//
// Allocation-free: everything here is arithmetic on numbers, and the one function
// that yields more than a single value writes into a caller-owned record. These
// run per row per frame.

/** Nanoticks in a quarter note. The engine's kNanoticksPerQuarter. */
export const NANOTICKS_PER_QUARTER = 960000;

/**
 * The meter to count in until the engine publishes one.
 *
 * `timebase` in a project carries only `nanoticks_per_quarter` — there is no song
 * time signature in the model yet, so this is the assumption every one of those
 * four files was already making silently. Naming it does not make it less of an
 * assumption; it makes it one assumption instead of four, and one place to delete
 * when the engine starts publishing.
 */
export const DEFAULT_METER = Object.freeze({ numerator: 4, denominator: 4 });

/**
 * Nanoticks in one beat, where a beat is the meter's DENOMINATOR unit.
 *
 * In 6/8 a beat is an eighth, not a quarter — that is what the denominator means,
 * and treating it as a quarter is the mistake that makes 6/8 draw as 6/4. A
 * quarter is 4/denominator of the unit, hence the ratio.
 */
export function ticksPerBeat(meter) {
  return (NANOTICKS_PER_QUARTER * 4) / meter.denominator;
}

/** Nanoticks in one bar. 4/4 -> 3840000, which is the constant this replaces. */
export function ticksPerBar(meter) {
  return ticksPerBeat(meter) * meter.numerator;
}

/**
 * Where a tick falls, as bar / beat / row-within-beat, written into `out`.
 *
 * Bars and beats are 1-based because that is how musicians count and how every
 * DAW displays them; `sub` is 0-based because it is a row offset, and row 0 of a
 * beat is the beat itself. That asymmetry is deliberate and is why they are named
 * differently.
 *
 * `rowTicks` is the display grid's row size, which is a projection choice and NOT
 * a property of the meter — the same tick is row 2 at one zoom and row 8 at
 * another. Passing it in keeps this function about the music and leaves zoom to
 * the caller.
 *
 * Writes into a caller-owned record rather than returning one: this is called per
 * visible row per frame, and GUIDELINES 3 forbids an object per call in a draw
 * path. The record is only valid until the caller's next call with the same one.
 */
export function positionOf(tick, meter, rowTicks, out) {
  const perBeat = ticksPerBeat(meter);
  const perBar = perBeat * meter.numerator;
  const inBar = tick % perBar;
  out.bar = Math.floor(tick / perBar) + 1;
  out.beat = Math.floor(inBar / perBeat) + 1;
  out.sub = rowTicks > 0 ? Math.round((inBar % perBeat) / rowTicks) : 0;
  out.onBar = inBar === 0;
  out.onBeat = inBar % perBeat === 0;
  return out;
}

/** A record for positionOf to fill. One per caller, allocated once. */
export function createPosition() {
  return { bar: 1, beat: 1, sub: 0, onBar: true, onBeat: true };
}

/**
 * How a meter is written, e.g. "7/8". Interned, because the chrome draws it every
 * frame and the set of meters a project uses is tiny.
 *
 * Keyed on the two numbers rather than on an object identity: the caller may well
 * rebuild the meter record, and a cache that missed on that would rebuild the
 * string every frame while looking like it worked.
 */
const METER_TEXT = new Map();
export function meterText(meter) {
  const key = meter.numerator * 64 + meter.denominator;
  let s = METER_TEXT.get(key);
  if (s === undefined) {
    s = meter.numerator + '/' + meter.denominator;
    METER_TEXT.set(key, s);
  }
  return s;
}

/**
 * Whether two meters say the same thing.
 *
 * Compares the VALUES, not the records. The engine will republish a meter on every
 * frame it publishes anything, and a guard keyed on object identity would rebuild
 * every bar label sixty times a second — GUIDELINES 2.1, from the other direction:
 * a key that changes when the content does not.
 */
export function sameMeter(a, b) {
  return a === b || (!!a && !!b
    && a.numerator === b.numerator && a.denominator === b.denominator);
}
