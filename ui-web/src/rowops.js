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
  /*
   * v33. RETRIGGER VOLUME RAMP — signed TOTAL percent across a retrigger's strikes.
   *
   * `rv-60` over four strikes gives 100%, 80%, 60%, 40% of the authored velocity: the first is
   * always at full level and the last lands at 40%. Stated as a TOTAL rather than a per-strike
   * drop because that is what the ear judges and what the hand wants to set — per-strike would
   * make the same number mean something different at every retrigger count.
   *
   * Signed, so a crescendo is the same op with the other sign. It means nothing without `ret`,
   * and a ramp on a row with no retrigger is a no-op rather than a silence.
   *
   * GLYPH 'v', not 'r': `ret` already owns 'r', and two ops sharing a glyph makes a collapsed
   * run ambiguous to read — the glyph is the only thing saying which op it is, since position
   * cannot.
   */
  { prefix: 'rv', field: 'retrigRamp', bit: 'RetrigRamp', glyph: 'v',
    summary: 'retrigger volume ramp, signed total percent across the strikes',
    example: 'rv-60', text: (v) => String(v) },
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
    example: 's7',
    /*
     * NOT ZERO-PADDED — Jaakko, 2026-07-31: "s9 and s09 should be the same thing. feel free to
     * change it to not-zero-padded."
     *
     * This revises SAMPLER_DESIGN section 8 Q1, which had ruled for `s07` so a fixed-width grid
     * keeps its vertical rhythm. The rhythm argument is real and it lost to a simpler one: the
     * two spellings mean the same slot, so the canonical form should be the one a person types.
     *
     * BOTH ARE ACCEPTED ON INPUT and always were — `s9`, `s09` and `s009` all parse to 9 — which
     * is the part that actually had to be true. Padding was only ever about which of them is
     * written back out.
     */
    text: (v) => String(v) },
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
  /*
   * v33. CONDITIONAL TRIG — fire on pass A of every B.
   *
   * NOT probability, and the distinction is the whole point: `p60` is a per-pass roll and
   * deliberately unpredictable, `c1:2` is DETERMINISTIC in which pass of the loop the transport
   * is on. That is what lets a phrase resolve every four bars rather than merely thin out, and
   * `c1:2` with `c2:2` covers every pass exactly once — the call-and-response gesture.
   *
   * Packed as ((a-1) << 3 | (b-1)) + 1 so 0 stays "no condition"; A and B are 1..8 and A <= B.
   * A > B is refused rather than normalised: it could never fire, and a note that never sounds
   * is not something anyone types on purpose.
   */
  { prefix: 'c', field: 'trigCondition', bit: 'TrigCondition',
    summary: 'conditional trig: fire on pass A of every B, or cpre/cnpre for the previous '
           + "conditional's result",
    example: 'c1:2',
    text: (v) => {
      if (v === TRIG_CONDITION_PRE) return 'pre';
      if (v === TRIG_CONDITION_NOT_PRE) return 'npre';
      const [a, b] = splitTrigCondition(v);
      return a ? `${a}:${b}` : '';
    } },
];

/**
 * Pack and unpack an A:B conditional, mirroring `make_trig_condition` / `split_trig_condition`.
 *
 * Mirrored rather than invented: the code is a single byte on the wire and the two sides have to
 * agree about which byte means 2:4. Backend exports both halves for exactly this reason.
 */
export function makeTrigCondition(a, b) {
  if (!(a >= 1 && b >= 1 && a <= 8 && b <= 8 && a <= b)) return 0;
  return (((a - 1) << 3) | (b - 1)) + 1;
}
export function splitTrigCondition(code) {
  if (!code || code > 64) return [0, 0];
  const packed = code - 1;
  return [(packed >> 3) + 1, (packed & 7) + 1];
}

/**
 * PRE and NOT PRE: fire when the PREVIOUS conditional trig on the same track fired, or when it
 * did not. Mirrors TRIG_CONDITION_PRE / TRIG_CONDITION_NOT_PRE in rowop.rs.
 *
 * Outside the A:B space on purpose — 1..64 is the packed A:B range, so these sit above it and
 * `splitTrigCondition` already answers [0, 0] for them rather than decoding a nonsense pass.
 *
 * 128 and 129 are FILL and NOT FILL, RESERVED AND NOT PARSEABLE on either side: a fill trig makes
 * the render depend on a live performance input, so a bounce would need to define what fill state
 * it renders under, and that is the owner's decision to make. A token that round-trips through
 * the editor and then always sounds is worse than one the editor refuses.
 */
export const TRIG_CONDITION_PRE = 130;
export const TRIG_CONDITION_NOT_PRE = 131;

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

/**
 * The ops actually PRESENT on a note, in the order the glyph run draws them.
 *
 * The run and this list are the same sequence by construction — both walk ROW_OPS and skip a
 * falsy value — which is what lets a cursor index into the run address an op. Two walks that
 * agreed by coincidence would put the caret on one glyph and edit another, and the note would
 * come back with the wrong op changed and no error anywhere.
 */
export function opsPresent(note) {
  const out = [];
  if (!note) return out;
  for (const op of ROW_OPS) if (note[op.field]) out.push(op);
  return out;
}

/**
 * The canonical token for ONE of a note's ops, by its index in the run — `p60`, `d1/6`.
 *
 * This is what the cell shows when a single op is selected and what the edit buffer is seeded
 * with. It is the same spelling `opsText` would produce for that op, from the same table, so a
 * token read out of a cell can be typed back into it.
 */
export function opTokenAt(note, index, beatTicks) {
  const present = opsPresent(note);
  const op = present[index];
  if (!op) return '';
  return op.prefix + op.text(note[op.field], beatTicks);
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
                   SoundOffset: 1 << 3, Delay: 1 << 4,
                   // v33, from ROW_OP_MASK_RETRIG_RAMP / ROW_OP_MASK_TRIG_CONDITION.
                   RetrigRamp: 1 << 5, TrigCondition: 1 << 6 };

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
    } else if (token.startsWith('rv')) {
      /*
       * BEFORE the single-letter branches, and its own branch rather than an 'r' one: `ret` and
       * `rv` share a first letter, so whichever is tested by one character swallows the other.
       * Longest-prefix-first is the rule the whole file runs on and this is where it bites.
       *
       * Signed, because a crescendo is the same op with the other sign, and REFUSED outside
       * -100..100 rather than clamped: a ramp of -150 is not a quieter roll, it is a number the
       * author did not mean, and clamping would accept it as if they had.
       */
      const n = int(token.slice(2), true);
      if (n === null) return { error: `bad retrigger ramp in "${token}"` };
      if (n < -100 || n > 100) {
        return { error: `retrigger ramp must be -100..100 percent in "${token}"` };
      }
      ops.retrigRamp = n; field = 'retrigRamp';
    } else if (token[0] === 'c') {
      /*
       * A:B, both 1..8, A <= B. A > B is REFUSED rather than normalised — it could never fire,
       * and a note that never sounds is not something anyone types on purpose.
       */
      /*
       * PRE AND NOT PRE ARE NOT A:B FORMS, so they are taken first — the split below would call
       * `cpre` malformed. `cpre` fires when the previous conditional trig on the same track
       * fired and `cnpre` when it did not, which is how a call and its response are written as
       * two rows that cannot both sound.
       */
      const rest = token.slice(1);
      field = 'trigCondition';
      if (rest === 'pre') {
        ops.trigCondition = TRIG_CONDITION_PRE;
      } else if (rest === 'npre') {
        ops.trigCondition = TRIG_CONDITION_NOT_PRE;
      } else {
        const parts = rest.split(':');
        if (parts.length !== 2) {
          return { error: `a conditional is A:B, like c1:2, or cpre/cnpre, in "${token}"` };
        }
        const a = int(parts[0]), b = int(parts[1]);
        if (a === null || b === null) return { error: `bad conditional in "${token}"` };
        if (a < 1 || b < 1 || a > 8 || b > 8) {
          return { error: `conditional A and B must be 1..8 in "${token}"` };
        }
        if (a > b) {
          return { error: `conditional A must not exceed B in "${token}" — it could never fire` };
        }
        ops.trigCondition = makeTrigCondition(a, b);
      }
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
/**
 * A non-negative integer, or a SIGNED one when asked.
 *
 * Unsigned by default and deliberately strict: every op but the ramp counts something that
 * cannot be negative, and accepting `-` there would turn a typo into a value.
 */
function int(s, signed = false) {
  if (!(signed ? /^-?\d+$/ : /^\d+$/).test(s)) return null;
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
