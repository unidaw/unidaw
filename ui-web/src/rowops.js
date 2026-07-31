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
    /*
     * DECIMAL, 0..255, and the stored value is `n * 256`.
     *
     * An earlier version rendered this as HEX, on the assumption that `o80` was 9xx muscle
     * memory all the way down. It is not: `parse_row_ops` reads it with `rest.parse::<u32>()`,
     * which is decimal, so `o80` is eighty two-hundred-fifty-sixths and not half. Rendering it
     * as hex produced a token that parsed back to a DIFFERENT offset — the one failure a text
     * form must not have.
     */
    text: (v) => String(Math.round(v / 256)) },
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

/*
 * The mask bits SetRowOps takes — `ROW_OP_MASK_*` in ui/daw-bridge/src/layout.rs.
 *
 * A bit CLEAR leaves that op alone; a bit SET with a zero value CLEARS it. That rule is the
 * whole reason the command has a mask: without it, "set the probability" is a read-modify-write
 * that silently drops whatever retrigger arrived between the read and the write.
 *
 * Which means DELETING an op is a bit SET and a value of zero, never an omission — the single
 * easiest thing to get backwards here, and it fails by leaving the old value in place, which
 * looks like the edit not landing rather than like the mask being wrong.
 */
export const OP_MASK = { retrigger: 1 << 0, probability: 1 << 1, sound: 1 << 2,
                         soundOffset: 1 << 3, delay: 1 << 4 };

/**
 * Parse a canonical op string, mirroring `parse_row_ops` in ui/daw-bridge/src/rowop.rs.
 *
 * Returns `{ ops }` or `{ error }`. An unknown or malformed token is an ERROR and never a
 * silent no-op — a red cell, not a dropped op, is the tracker rule and it is stated in that
 * file. The refusals are worded the same way for the same reason: a person who types `p0` in
 * one surface and the console in another should be told the same thing.
 *
 * ORDER MATTERS: the multi-letter prefixes are tested before the single-letter ones, or `ret3`
 * parses as a malformed probability.
 */
export function parseOps(text) {
  const ops = { retrigger: 0, probability: 0, sound: 0, soundOffset: 0, delayNum: 0, delayDen: 0 };
  const seen = new Set();
  for (const token of String(text || '').trim().split(/\s+/)) {
    if (!token) continue;
    let field = null;
    if (token.startsWith('ret')) {
      const n = int(token.slice(3));
      if (n === null) return { error: `bad retrigger count in "${token}"` };
      if (n < 1) return { error: `retrigger count must be >= 1 in "${token}"` };
      ops.retrigger = n; field = 'retrigger';
    } else if (token[0] === 's') {
      const n = int(token.slice(1));
      if (n === null) return { error: `bad sound slot in "${token}"` };
      if (n === 0 || n > 65535) {
        return { error: `sound slot must be 1..65535 in "${token}" (blank means the keymap picks)` };
      }
      ops.sound = n; field = 'sound';
    } else if (token[0] === 'o') {
      const n = int(token.slice(1));
      if (n === null) return { error: `bad sample offset in "${token}"` };
      if (n > 255) return { error: `sample offset is 0..255 (in 1/256ths) in "${token}"` };
      // Written in 1/256ths, stored at full u16 resolution — the coarse notation is for muscle
      // memory, not a limit on what can be expressed.
      ops.soundOffset = n * 256; field = 'soundOffset';
    } else if (token[0] === 'p') {
      const n = int(token.slice(1));
      if (n === null) return { error: `bad probability in "${token}"` };
      if (n < 1 || n > 100) return { error: `probability must be 1..=100 in "${token}"` };
      ops.probability = n; field = 'probability';
    } else if (token[0] === 'd') {
      const rest = token.slice(1);
      const slash = rest.indexOf('/');
      if (slash < 0) return { error: `delay needs a fraction like d1/6, got "${token}"` };
      const num = int(rest.slice(0, slash)), den = int(rest.slice(slash + 1));
      if (num === null) return { error: `bad delay numerator in "${token}"` };
      if (den === null) return { error: `bad delay denominator in "${token}"` };
      if (den === 0) return { error: `delay denominator must be nonzero in "${token}"` };
      ops.delayNum = num; ops.delayDen = den; field = 'delay';
    } else {
      return { error: `unknown row op "${token}"` };
    }
    /*
     * A repeated family is an ERROR, not last-wins.
     *
     * `ret3 ret5` is a person who meant one of them and cannot be asked which. Rust's parser
     * takes the last silently; saying so is strictly better and costs a Set.
     */
    if (seen.has(field)) return { error: `"${field}" given twice` };
    seen.add(field);
  }
  return { ops };
}

/** A non-negative decimal integer, or null. Rust's `parse::<u32>()` rejects everything else. */
function int(s) {
  if (!/^\d+$/.test(s)) return null;
  const n = Number(s);
  return Number.isFinite(n) ? n : null;
}

/**
 * The grammar, as one line to put in front of somebody typing it.
 *
 * Built from ROW_OPS — the same list the parser and the renderer read — so an op added to the
 * schema appears here without anyone remembering to mention it. That is the whole reason
 * `OP_SCHEMA` carries `summary` and `example` in the first place: its own comment says one
 * definition should feed "entry, autocomplete, docs and the linter", and until now this side
 * used none of them.
 *
 * Examples rather than prose, because the example IS the syntax: `ret3` teaches the shape of
 * `ret<n>` faster than `ret<n>` does, and it is a string you can type as-is.
 */
export function opsHint() {
  return ROW_OPS.map((o) => o.example).join('  ');
}

/** What one op means, for the op a partly-typed token is reaching for. */
export function opHelpFor(token) {
  const t = String(token || '').trim();
  if (!t) return '';
  /*
   * Longest prefix first, or `ret3` matches `r`-nothing and then the single-letter branches —
   * the same ordering trap the parser documents, arriving in the help.
   */
  const byLength = [...ROW_OPS].sort((a, b) => b.prefix.length - a.prefix.length);
  for (const o of byLength) if (t.startsWith(o.prefix)) return `${o.example} — ${o.summary}`;
  return '';
}
