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
  { prefix: 'ret', field: 'retrigger', bit: 'Retrigger',
    summary: 'retrigger N even strikes over the note',
    example: 'ret3', text: (v) => String(v) },
  { prefix: 'p', field: 'probability', bit: 'Probability',
    summary: 'probability percent to sound (1-100)',
    example: 'p60', text: (v) => String(v) },
  { prefix: 'd', field: 'delayTicks', bit: 'Delay',
    summary: 'delay onset by a fraction of a beat',
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
  { prefix: 's', field: 'sound', bit: 'Sound',
    summary: 'play sampler slot N (blank = pitch picks it)',
    example: 's5', text: (v) => String(v) },
  { prefix: 'o', field: 'soundOffset', bit: 'SoundOffset',
    summary: 'start N/256 into the sample (the 9xx seek)',
    example: 'o80',
    /*
     * SPELLED BACK EXACTLY, in whichever of the two forms is exact.
     *
     * A multiple of 256 came from `o80` and goes back as `o80` — the form a tracker's hands
     * expect. Anything else came from a fraction, and rendering it as the nearest 1/256th would
     * be a token that parses back to a DIFFERENT offset: `o1/3` is 21845 and `o85` is 21760.
     * The text form has one contract and it is that it round-trips.
     *
     * `v / 65535` reduced, because 65535 is the scale the parser uses — a third is exactly
     * 21845/65535, so the fraction that made a value is the fraction that comes back.
     *
     * (This rendered HEX once, on the assumption that `o80` was 9xx muscle memory all the way
     * down. It is not: the parser reads it with a decimal `parse::<u32>()`. Same failure as
     * above, found the same way.)
     */
    text: (v) => {
      if (v % 256 === 0) return String(v / 256);
      const [n, d] = reduce(v, 65535);
      return `${n}/${d}`;
    } },
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
/*
 * THE WIRE BITS, ONE PER OP, DERIVED FROM THE TABLE ABOVE.
 *
 * `apps/event_payloads.h` is the authority — kRowOpMaskRetrigger = 1u << 0 and so on — and each
 * entry names its bit there rather than repeating a number. Written as a hand-kept object, this
 * had already drifted in shape from the table it describes: the mask said `delay` where the op
 * says `delayTicks`, so a lookup by an op's own field name silently missed and the edit went out
 * with that bit clear, which the engine reads as "leave it alone". An op that refuses to change
 * looks like a broken engine, not like a typo in a key.
 *
 * Keyed by BOTH names for the same reason: callers hold an op and reach for `op.field`, and
 * OP_MASK's older callers hold 'delay'. Neither should have to know about the other.
 */
/*
 * THE WIRE'S BIT ORDER IS NOT THIS TABLE'S ORDER.
 *
 * `apps/event_payloads.h:490` numbers them Retrigger, Probability, Sound, SoundOffset, Delay.
 * ROW_OPS is written in the order a person reads a row — ret, p, d, s, o — with the delay third
 * because that is where a tracker hand expects it. Deriving the bits from the table's INDEX
 * therefore produced sound=4, soundOffset=16, delay=8: three ops silently addressing each
 * other's fields, which on the wire is not an error, it is a different edit.
 *
 * So each op names its bit and the values live here, mirroring the engine's enum. unit.mjs
 * parses that enum and fails if these disagree, which is the only reason it is safe to write a
 * number down twice.
 */
const WIRE_BIT = { Retrigger: 1 << 0, Probability: 1 << 1, Sound: 1 << 2,
                   SoundOffset: 1 << 3, Delay: 1 << 4 };

/*
 * Keyed by BOTH names: callers hold an op and reach for `op.field`, and OP_MASK's older callers
 * hold 'delay'. The hand-kept version had only the second, so a lookup by an op's own field name
 * missed and the edit went out with that bit clear — which the engine reads as "leave it alone".
 * An op that refuses to change looks like a broken engine, not like a mismatched key.
 */
export const OP_MASK = ROW_OPS.reduce((m, op) => {
  const bit = WIRE_BIT[op.bit];
  if (!bit) throw new Error(`row op ${op.prefix} names no wire bit`);
  m[op.field] = bit;                                              // 'delayTicks'
  m[op.bit.charAt(0).toLowerCase() + op.bit.slice(1)] = bit;      // 'delay'
  return m;
}, {});

export function opMaskOf(nameOrOp) {
  const key = typeof nameOrOp === 'string' ? nameOrOp : nameOrOp && nameOrOp.field;
  const bit = OP_MASK[key];
  if (!bit) throw new Error(`no row-op mask bit named ${key}`);
  return bit;
}

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
      /*
       * TWO FORMS, ONE FIELD, mirroring `rowop.rs`.
       *
       * `o80` is 1/256ths — what 9xx taught everyone's hands. `o<N>/<M>` is a plain fraction
       * reaching the full u16, so the notation can say what the storage has always been able to
       * hold. The storage was never the narrow part; the parser was.
       *
       * THE SCALE IS 65535, NOT 65536, and it has to be exactly that or the two surfaces
       * disagree in the last bit. Rounded, not truncated, for the same reason.
       */
      const orest = token.slice(1);
      const oslash = orest.indexOf('/');
      if (oslash >= 0) {
        const num = int(orest.slice(0, oslash)), den = int(orest.slice(oslash + 1));
        if (num === null) return { error: `bad offset numerator in "${token}"` };
        if (den === null) return { error: `bad offset denominator in "${token}"` };
        if (den === 0) return { error: `offset denominator must not be zero in "${token}"` };
        if (den > 65535) return { error: `offset denominator is at most 65535 in "${token}"` };
        if (num >= den) {
          // An offset of the WHOLE extent starts at the end and plays nothing, and past it is
          // not a position at all. Refused rather than clamped: `o5/4` is a typo, and a note
          // that silently vanishes for an unstated reason is the worse outcome.
          return { error: `offset must be less than the whole extent in "${token}" (N < M)` };
        }
        ops.soundOffset = Math.floor((num * 65535 + Math.floor(den / 2)) / den);
      } else {
        const n = int(orest);
        if (n === null) return { error: `bad sample offset in "${token}"` };
        if (n > 255) {
          return { error: `sample offset is 0..255 in 1/256ths, or a fraction like o1/3, in "${token}"` };
        }
        ops.soundOffset = n * 256;
      }
      field = 'soundOffset';
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

/**
 * Which op a token is reaching for, by LONGEST PREFIX.
 *
 * Ordering is the whole content of this function: sorted the other way `ret3` matches nothing,
 * because no single-letter prefix starts it and `r` is not an op — and `d1/6` would match `d`
 * either way, which is what makes the trap quiet. The parser documents the same rule and this is
 * the one place it is implemented, so a third caller cannot get a fourth answer.
 */
export function opForToken(token) {
  const t = String(token || '').trim();
  if (!t) return null;
  const byLength = [...ROW_OPS].sort((a, b) => b.prefix.length - a.prefix.length);
  return byLength.find((o) => t.startsWith(o.prefix)) || null;
}

/** What one op means, for the op a partly-typed token is reaching for. */
export function opHelpFor(token) {
  const o = opForToken(token);
  return o ? `${o.example} — ${o.summary}` : '';
}

/**
 * Split a single-op edit into the text to parse and the field to restrict the mask to.
 *
 * `p60` sets probability and touches nothing else; a BARE prefix — `p` — clears it, because the
 * mask protocol says a bit set with a zero value is a CLEAR while a bit left clear means "leave
 * this alone". That distinction is the whole reason a one-op edit is expressible at all: without
 * it, changing a retrigger means resending the probability, the delay, the slot and the offset,
 * and two people editing one row would overwrite each other's ops with their own stale copies.
 *
 * Returns null when the token names no op, so the caller can refuse rather than send a no-op.
 */
export function oneOpEdit(token) {
  const t = String(token || '').trim();
  const op = opForToken(t);
  if (!op) return null;
  return { field: op.field, text: t === op.prefix ? '' : t, clears: t === op.prefix };
}
