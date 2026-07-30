// Per-note row ops: the schema, mirrored, and the collapsed glyph run.
//
// The grammar lives in ui/daw-bridge/src/rowop.rs — `OP_SCHEMA` and
// `parse_row_ops` — and is shared by the CLI, the bridge and the engine. This is
// the drawing half. A test in unit.mjs holds the two lists equal, because a
// mirror kept in step by hand is a mirror that drifts on the sixth occasion.
//
// WHAT THE CELL SHOWS, and why it is a run of glyphs rather than one value.
//
// The cell used to resolve ops by PRIORITY:
//
//     c2.text = n.retrigger ? 'R' + n.retrigger
//             : n.probability ? 'P' + n.probability : 'D';
//
// so a note carrying `ret3 p60 d1/6` drew `R3` and the other two were gone. Not
// truncated, not marked — invisible, with the engine playing all three. That is a
// live bug today, before the sampler's ops are even added, and it is the reason
// this file exists.
//
// COLLAPSED IS ONE CHARACTER PER OP. A note with three ops draws three
// characters; a note with forty-three draws forty-three. Nothing outranks
// anything and nothing is dropped. The value is not in the collapsed form —
// that is what collapsed means — and arrives when the cell is entered.
//
// THE GLYPH IS IDENTITY, NOT MAGNITUDE. An op may appear at any position in the
// run, so position cannot say which op it is and the character must. (An earlier
// design reserved a fixed slot per family, which let the character carry
// magnitude instead — that design hardcoded the vocabulary into the layout and
// was dropped: a column bound to `offset` means adding an op relayouts every
// track.)
//
// THE DEFAULT GLYPH IS THE TOKEN'S FIRST CHARACTER — `ret3` draws `r`, `p60`
// draws `p`. Nothing to learn and nothing to declare: the notation you type is
// the mark you read. It stops scaling somewhere past twenty-six ops, which is
// what `glyph` is for; until an op asks for one, it costs nothing.

/**
 * The ops, in the order they are drawn — which is `OP_SCHEMA`'s order in
 * rowop.rs, so a run reads the same way everywhere and two notes with the same
 * ops always draw the same string.
 *
 * `field` is what the wire calls it on a note. `glyph` is optional and defaults
 * to `prefix[0]`.
 */
export const ROW_OPS = [
  { prefix: 'ret', field: 'retrigger', summary: 'retrigger N even strikes over the note',
    example: 'ret3', text: (v) => String(v) },
  { prefix: 'p', field: 'probability', summary: 'probability percent to sound (1-100)',
    example: 'p60', text: (v) => String(v) },
  { prefix: 'd', field: 'delayTicks', summary: 'delay onset by a fraction of a beat',
    example: 'd1/6',
    /*
     * Published as TICKS, authored as a fraction of a beat — so it cannot be spelled back
     * exactly without the beat length. Given one it reduces (160000 of 960000 is a sixth);
     * without one it says ticks rather than guessing, because a guess would round-trip a
     * different note than the one on screen.
     */
    text: (v, beatTicks) => {
      if (!beatTicks) return `${v}t`;
      const [n, d] = reduce(v, beatTicks);
      return `${n}/${d}`;
    } },
  /*
   * v32, the sampler's sound address. `sound == 0` means the keymap picks the slot from pitch,
   * which is every row on an ordinary kit track — so zero is ABSENCE here as it is for every
   * other op, and `opsMask` already treats it that way. Backend's own comment says a UI should
   * draw 0 as EMPTY rather than as "0"; drawing a mark would be the same claim.
   */
  { prefix: 's', field: 'sound', summary: 'play sampler slot N (blank = pitch picks it)',
    example: 's5', text: (v) => String(v) },
  { prefix: 'o', field: 'soundOffset', summary: 'start N/256 into the sample (the 9xx seek)',
    example: 'o80',
    /*
     * THE NOTATION IS COARSER THAN THE STORAGE, and this is where that shows.
     *
     * The wire carries a u16 fraction of the slot's extent — 65536 positions — while the
     * notation is `o80` in 1/256ths, which is 256. So the token can only name one position in
     * every 256 the format can hold, and spelling a stored value back is lossy: an offset a
     * pointer drag placed between two typeable values renders as the nearer one.
     *
     * Rendering the nearest typeable token rather than inventing a finer spelling, because the
     * text form's contract is that it PARSES BACK — a spelling `parse_row_ops` would reject is
     * worse than a rounded one. Raised with backend; if the notation gains the resolution the
     * storage already has, only this function changes.
     */
    text: (v) => Math.round((v / 65536) * 256).toString(16).padStart(2, '0') },
];

/** The mark for one op. Explicit `glyph` wins; otherwise the token's first character. */
export function opGlyph(op) {
  return op.glyph || op.prefix[0];
}

/**
 * Which ops a note carries, as a bit per entry in ROW_OPS.
 *
 * A number rather than a list so the run can be interned against it — see
 * `opsRun`. Reading a missing field as absent rather than as zero matters:
 * `probability: 0` means "always sounds", which is the ABSENCE of the op, and
 * drawing a `p` there would claim a note is conditional when it is not.
 */
export function opsMask(note) {
  let bits = 0;
  for (let i = 0; i < ROW_OPS.length; i++) {
    const v = note[ROW_OPS[i].field];
    if (v) bits |= 1 << i;
  }
  return bits;
}

/*
 * Every run that can be drawn, built once and looked up by mask.
 *
 * 2^n strings for n ops — eight today. The draw path assigns a pointer and
 * allocates nothing, which is the rule for anything on that path (GUIDELINES 3).
 * Built lazily so adding an op to ROW_OPS cannot leave a stale table behind.
 */
const RUNS = [];

/**
 * The collapsed run for a note: one character per op it carries, in schema
 * order. Empty when the note has none, so an ordinary kit track draws no ink.
 */
export function opsRun(note) {
  const bits = opsMask(note);
  const hit = RUNS[bits];
  if (hit !== undefined) return hit;
  let s = '';
  for (let i = 0; i < ROW_OPS.length; i++) if (bits & (1 << i)) s += opGlyph(ROW_OPS[i]);
  RUNS[bits] = s;
  return s;
}

/**
 * The canonical text form of a note's ops — what `parse_row_ops` accepts, what
 * an agent writes, and what the cell holds when it is opened for editing.
 *
 * NOT built on the draw path: this is for the status line and the edit buffer,
 * both of which run per gesture rather than per frame.
 *
 * The delay is published as TICKS and authored as a fraction of a beat, so it
 * cannot be spelled back exactly without the beat length. It is rendered here
 * only as far as it is known, and the caller passes `beatTicks` when it wants
 * the fraction — a guess would round-trip a different note than the one on
 * screen.
 */
export function opsText(note, beatTicks) {
  let out = '';
  for (const op of ROW_OPS) {
    const v = note[op.field];
    if (!v) continue;
    if (out) out += ' ';
    out += op.prefix + op.text(v, beatTicks);
  }
  return out;
}

function reduce(n, d) {
  let a = n, b = d;
  while (b) { const t = a % b; a = b; b = t; }
  const g = a || 1;
  return [n / g, d / g];
}

/**
 * THE FULLEST FORM THAT FITS, without allocating on the draw path.
 *
 * Collapsed-is-one-glyph is right when forty-three ops share a cell and wrong when one op has
 * the cell to itself: `p100` became `p` in the first version of this, losing a value from a
 * cell with room for it. So the rule is the obvious one — draw the canonical text when it fits
 * the cell, the glyph run when it does not — and the whole difficulty is doing that without
 * building a string per cell per frame.
 *
 * THE CACHE IS ON THE CELL, compared FIELD BY FIELD rather than by a packed key. Packing five
 * values into one number is not safe here (`sound` and `soundOffset` are u16, `delayTicks` is
 * u32, and 53 bits does not go round), and a string key would allocate to compare. Five integer
 * compares are cheaper than either and are exact.
 *
 * Cells are pooled and persist across frames, so after the first frame this rebuilds only when
 * a value actually changes or the cell is rebound by scrolling — which already rebinds
 * everything.
 *
 * @param cell   the view-model cell, used as the cache
 * @param note   the note
 * @param chars  how many characters the cell can show; 0 means "never fits", i.e. always the run
 * @param beatTicks so the delay can be spelled as the fraction it was authored as
 */
export function opsCellText(cell, note, chars, beatTicks) {
  const run = opsRun(note);
  if (!run) return '';
  // The run is interned and always fits by construction (one character per op is the shortest
  // form there is), so a cell too small for the text falls back to something never rebuilt.
  if (!chars) return run;

  let changed = cell._opsBeat !== beatTicks || cell._opsChars !== chars;
  for (let i = 0; i < ROW_OPS.length; i++) {
    const v = note[ROW_OPS[i].field] || 0;
    if (cell._opsVals === undefined) { changed = true; break; }
    if (cell._opsVals[i] !== v) { changed = true; }
  }
  if (!changed) return cell._opsStr;

  if (cell._opsVals === undefined) cell._opsVals = new Array(ROW_OPS.length).fill(-1);
  for (let i = 0; i < ROW_OPS.length; i++) cell._opsVals[i] = note[ROW_OPS[i].field] || 0;
  cell._opsBeat = beatTicks;
  cell._opsChars = chars;
  const full = opsText(note, beatTicks);
  cell._opsStr = full.length <= chars ? full : run;
  return cell._opsStr;
}
