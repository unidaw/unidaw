//! Typed per-note row ops (item 12): the effect column, named instead of hex.
//!
//! A tracker's per-row command is its most powerful feature and its worst
//! notation — `E9x` for retrigger, `EDx` for note delay, all hex because the
//! on-disk format was a byte pair. Uni has no such constraint, so ops are typed,
//! named tokens with a schema. This module is the parser and the schema, shared
//! by every front end and the CLI; the engine applies the resulting fields at
//! playback. Keeping the grammar here means one definition feeds entry,
//! autocomplete, docs and the linter.

/// Row ops on one note. Defaults are inert — a note with no ops plays normally.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct RowOps {
    /// Number of even re-strikes across the note's duration. 0 or 1 = one strike.
    pub retrigger: u8,
    /// Percent chance the note sounds. 0 = always; 1..=100 = that percent.
    pub probability: u8,
    /// Onset delay as a fraction of a beat, e.g. `(1, 6)` = a sixth of a beat.
    /// Stored as a fraction so it is grid-independent; resolved to ticks against
    /// a beat length at the point of use.
    pub delay: Option<(u32, u32)>,
    /// THE SOUND ADDRESS (SAMPLER_DESIGN R2). Which slot of the track's sampler this note plays.
    /// 0 = resolve through the keymap, which is the common case: on an ordinary kit track pitch
    /// picks the slot and this is blank on every row. It fills in only when you want the SAME
    /// slot at a different pitch — one snare, five pitches, one column.
    pub sound: u16,
    /// The 9xx seek, as a FRACTION of the slot's extent (0..65535), not absolute frames.
    /// Absolute breaks when the slot's sample is swapped, and a slot can name a slice, so it
    /// breaks on a re-chop too. Written in 1/256ths for tracker muscle memory (`o80` = half way)
    /// and stored at full resolution.
    pub sound_offset: u16,
    /// THE RETRIGGER VOLUME RAMP: signed TOTAL percent change across the burst's strikes.
    /// `rv-60` lands the last strike at 40% of the first; the first is always at the authored
    /// velocity. 0 is flat. This is the difference between a roll and a stutter, and it is the
    /// half of `ret` the Elektron gesture has and this repo did not.
    pub retrig_ramp: i8,
    /// THE CONDITIONAL TRIG, packed. 0 = no condition. Build it with `make_trig_condition`.
    ///
    /// NOT probability. `p` is a per-pass roll and deliberately unpredictable; this is
    /// deterministic in WHICH PASS the transport is on, which is what lets a phrase resolve
    /// every four bars instead of merely thinning out.
    pub trig_condition: u8,
}

/// Packs an A:B conditional into its wire code, mirroring makeTrigCondition in
/// apps/musical_structures.h. Returns 0 (no condition) for a pair that cannot fire —
/// a > b would never sound, which nobody types on purpose.
pub fn make_trig_condition(a: u8, b: u8) -> u8 {
    if a < 1 || b < 1 || a > 8 || b > 8 || a > b {
        return 0;
    }
    ((a - 1) << 3 | (b - 1)) + 1
}

/// PRE fires when the previous conditional trig ON THE SAME TRACK fired; NOT PRE is its
/// negation. Mirrors kTrigConditionPre / kTrigConditionNotPre in apps/musical_structures.h.
///
/// 128/129 are FILL and NOT FILL, RESERVED AND NOT IMPLEMENTED: a fill trig makes the render
/// depend on a live performance input, so a bounce has to define what fill state it renders
/// under, and that is an owner decision. They are deliberately not parseable here — a token that
/// round-trips through the editor and then always sounds is worse than one the editor refuses.
pub const TRIG_CONDITION_PRE: u8 = 130;
pub const TRIG_CONDITION_NOT_PRE: u8 = 131;

/// Unpacks a condition code to (a, b), or (0, 0) if it is not an A:B form.
pub fn split_trig_condition(code: u8) -> (u8, u8) {
    if code == 0 || code > 64 {
        return (0, 0);
    }
    let packed = code - 1;
    ((packed >> 3) + 1, (packed & 7) + 1)
}

impl RowOps {
    /// True when the note carries no ops at all.
    ///
    /// `sound` and `sound_offset` COUNT. They were missing, which meant a row whose only op is a
    /// sound address reported itself as empty — and R5's rule is that the ops column appears per
    /// track when something in that track uses one, with a marker in the note cell. A row that
    /// says "play slot 7" would have shown no marker and no column, so the one op you cannot see
    /// from the pitch would have been the one op the editor hid.
    pub fn is_empty(&self) -> bool {
        self.retrigger == 0
            && self.probability == 0
            && self.delay.is_none()
            && self.sound == 0
            && self.sound_offset == 0
            && self.retrig_ramp == 0
            && self.trig_condition == 0
    }

    /// The onset delay in absolute nanoticks for a given beat length.
    pub fn delay_nanoticks(&self, nanoticks_per_beat: u64) -> u32 {
        match self.delay {
            Some((num, den)) if den != 0 => {
                (nanoticks_per_beat * num as u64 / den as u64) as u32
            }
            _ => 0,
        }
    }
}

/// One schema entry, so entry/autocomplete/docs come from the same table the
/// parser uses.
pub struct OpSpec {
    pub prefix: &'static str,
    pub summary: &'static str,
    pub example: &'static str,
}

// THE CANONICAL ORDER, and it is ONE order — this table fixes both the draw order of a collapsed
// glyph run and the order `format_row_ops` emits. They disagreed once (`o` fifth here, last there)
// and nothing broke, because the parser is order-free and both strings meant the same row; but
// "canonical" means one form, and a person reading a cell and then opening it should not find the
// ops rearranged. `op_schema_and_emitter_agree_on_order` is what keeps them one fact.
//
// The order groups ops that MODIFY EACH OTHER: `ret` and `rv` adjacent because a ramp is
// meaningless without a retrigger, `s` and `o` adjacent because both address the sample. `c` is
// last because it is the only op about WHEN the row fires rather than about what it plays.
// (Proposed by the web-UI agent, who has to draw the run and is the one it reads badly for.)
pub const OP_SCHEMA: &[OpSpec] = &[
    OpSpec { prefix: "ret", summary: "retrigger N even strikes over the note", example: "ret3" },
    OpSpec { prefix: "rv", summary: "retrigger volume ramp, signed total percent across the strikes", example: "rv-60" },
    OpSpec { prefix: "p", summary: "probability percent to sound (1-100)", example: "p60" },
    OpSpec { prefix: "d", summary: "delay onset by a fraction of a beat", example: "d1/6" },
    OpSpec { prefix: "s", summary: "play sampler slot N (blank = pitch picks it)", example: "s7" },
    OpSpec { prefix: "o", summary: "start N/256 into the sample, or a fraction like o1/3 (the 9xx seek)", example: "o80" },
    OpSpec { prefix: "c", summary: "conditional trig: fire on pass A of every B, or cpre/cnpre for the previous conditional's result", example: "c1:2" },
];

/// The canonical text for a set of ops — the inverse of `parse_row_ops`.
///
/// THE SOUND ADDRESS IS NOT PADDED — `s7`, not `s07` (owner, revising section 8 Q1: "s9 and s09
/// should be the same thing"). It is the id rather than a name, so a rename rewrites nothing and
/// a row means the same thing after any amount of kit editing.
///
/// The equivalence was ALWAYS true: `s7`, `s07` and `s007` all parse to slot 7, and always did.
/// Padding only ever decided which of them gets written back out, so this is a change to the
/// canonical form and to nothing else — no parser change, no wire change, no migration, and old
/// rows keep meaning what they meant. `a_padded_sound_address_is_the_same_slot` pins the
/// equivalence so a later formatting opinion cannot quietly make the parser strict to match.
///
/// The argument it lost to is worth keeping: a ragged column does break the vertical rhythm a
/// tracker is read by — but the fix for that is alignment in the CELL, which is the frontend's
/// job (R5), not characters in a string a person has to type back.
///
/// This lives beside the parser so there is exactly one definition of the spelling. Display is
/// the frontend's call, but "which characters mean slot 7" is not, or the two sides drift.
pub fn format_row_ops(ops: &RowOps) -> String {
    let mut out: Vec<String> = Vec::new();
    // IN OP_SCHEMA'S ORDER. Keep the two together — see the table's comment, and the test that
    // fails if they drift apart again.
    if ops.retrigger > 1 {
        out.push(format!("ret{}", ops.retrigger));
    }
    if ops.retrig_ramp != 0 {
        out.push(format!("rv{}", ops.retrig_ramp));
    }
    if ops.probability > 0 {
        out.push(format!("p{}", ops.probability));
    }
    if let Some((num, den)) = ops.delay {
        if den != 0 && num != 0 {
            out.push(format!("d{num}/{den}"));
        }
    }
    if ops.sound != 0 {
        out.push(format!("s{}", ops.sound));
    }
    if ops.sound_offset != 0 {
        // COARSE WHEN IT IS EXACTLY COARSE, a fraction otherwise. `o80` is the tracker muscle
        // memory and covers everything typed by hand; a value that did not come from a whole
        // 1/256th came from a fraction, and emitting the reduced fraction is the only form that
        // survives a round trip. Rounding it to the nearest 1/256th would move the seek.
        if ops.sound_offset % 256 == 0 {
            out.push(format!("o{}", ops.sound_offset / 256));
        } else {
            fn gcd(a: u32, b: u32) -> u32 {
                if b == 0 { a } else { gcd(b, a % b) }
            }
            let g = gcd(ops.sound_offset as u32, 65535);
            out.push(format!("o{}/{}", ops.sound_offset as u32 / g, 65535 / g));
        }
    }
    if ops.trig_condition != 0 {
        if ops.trig_condition == TRIG_CONDITION_PRE {
            out.push("cpre".to_string());
        } else if ops.trig_condition == TRIG_CONDITION_NOT_PRE {
            out.push("cnpre".to_string());
        } else {
            let (a, b) = split_trig_condition(ops.trig_condition);
            if a != 0 {
                out.push(format!("c{a}:{b}"));
            }
        }
    }
    out.join(" ")
}

/// Parses space-separated row-op tokens (`"ret3 p60 d1/6"`) into `RowOps`.
/// Unknown or malformed tokens are an error, never a silent no-op — a red cell,
/// not a dropped op, is the tracker rule.
pub fn parse_row_ops(input: &str) -> Result<RowOps, String> {
    let mut ops = RowOps::default();
    for token in input.split_whitespace() {
        // Order matters: "ret" is checked before the single-letter "p"/"d" so
        // it is not mis-read as a probability token.
        // Order matters throughout: the multi-letter prefixes are checked before the
        // single-letter ones, or "ret3" would parse as a malformed probability.
        if let Some(rest) = token.strip_prefix("rv") {
            // BEFORE "ret", or `rv-60` would fall through to the single-letter arms and be
            // rejected as a malformed something-else. "rv" and "ret" share no prefix, but the
            // ordering rule in this loop is "longest and most specific first" and keeping to it
            // is what stops the next op from being a puzzle.
            let n: i32 = rest.parse().map_err(|_| format!("bad retrigger ramp in {token:?}"))?;
            if !(-100..=100).contains(&n) {
                return Err(format!(
                    "retrigger ramp must be -100..100 percent in {token:?}"));
            }
            ops.retrig_ramp = n as i8;
        } else if let Some(rest) = token.strip_prefix("ret") {
            let n: u8 = rest.parse().map_err(|_| format!("bad retrigger count in {token:?}"))?;
            if n < 1 {
                return Err(format!("retrigger count must be >= 1 in {token:?}"));
            }
            ops.retrigger = n;
        } else if let Some(rest) = token.strip_prefix('c') {
            // The stateful forms come first: they have no colon, so the A:B split below would
            // reject them with a message about A:B that is true and unhelpful.
            if rest == "pre" {
                ops.trig_condition = TRIG_CONDITION_PRE;
                continue;
            }
            if rest == "npre" {
                ops.trig_condition = TRIG_CONDITION_NOT_PRE;
                continue;
            }
            let (a_text, b_text) = rest
                .split_once(':')
                .ok_or_else(|| format!(
                    "conditional trig needs A:B in {token:?}, e.g. c1:2 — or cpre/cnpre"))?;
            let a: u8 = a_text.parse().map_err(|_| format!("bad A in {token:?}"))?;
            let b: u8 = b_text.parse().map_err(|_| format!("bad B in {token:?}"))?;
            let code = make_trig_condition(a, b);
            if code == 0 {
                // REFUSED, not normalised. a > b can never fire, and a note that never sounds is
                // not something anyone typed on purpose — a red cell beats a silent row.
                return Err(format!(
                    "conditional trig must be 1..8:1..8 with A <= B in {token:?}"));
            }
            ops.trig_condition = code;
        } else if let Some(rest) = token.strip_prefix('s') {
            let n: u32 = rest.parse().map_err(|_| format!("bad sound slot in {token:?}"))?;
            if n == 0 || n > 65535 {
                // 0 is not a slot id — it is the SENTINEL meaning "let pitch pick". Accepting
                // `s0` would give two ways to say the same thing, one of which looks like an
                // explicit choice and is not.
                return Err(format!("sound slot must be 1..65535 in {token:?} (blank means the keymap picks)"));
            }
            ops.sound = n as u16;
        } else if let Some(rest) = token.strip_prefix('o') {
            // TWO FORMS, ONE FIELD. `o80` is 1/256ths — tracker muscle memory, and what 9xx
            // taught everyone's hands. `o<N>/<M>` is a plain fraction reaching the full u16, so
            // the notation can say what the storage can already hold.
            //
            // The storage was NEVER the narrow part: sound_offset has always been a u16 fraction
            // of the slot's extent, which is 0.076 ms on a five-second break. Only the parser
            // was coarse, so the format was surgical and the grammar could not reach it — the
            // exact complaint 9xx's 256-frame granularity earned, arriving one layer up. Asked
            // for by the web-UI agent, whose own doc had specified the fraction form while their
            // parser mirrored this one and refused it.
            //
            // The fraction form is `d1/6`'s, deliberately: not a new idea in the grammar, just
            // the one that is already there applied to a second op.
            if let Some((num, den)) = rest.split_once('/') {
                let num: u32 = num
                    .parse()
                    .map_err(|_| format!("bad offset numerator in {token:?}"))?;
                let den: u32 = den
                    .parse()
                    .map_err(|_| format!("bad offset denominator in {token:?}"))?;
                if den == 0 {
                    return Err(format!("offset denominator must not be zero in {token:?}"));
                }
                if den > 65535 {
                    return Err(format!("offset denominator is at most 65535 in {token:?}"));
                }
                if num >= den {
                    // An offset of the WHOLE extent starts at the end and plays nothing, and
                    // past it is not a position at all. Refused rather than clamped: a row that
                    // says o5/4 is a typo, and silently playing from the end would be a note
                    // that vanishes for a reason nothing states.
                    return Err(format!(
                        "offset must be less than the whole extent in {token:?} (N < M)"
                    ));
                }
                // Rounded, not truncated: o1/3 should land as close to a third as u16 allows.
                ops.sound_offset = ((num as u64 * 65535 + den as u64 / 2) / den as u64) as u16;
            } else {
                let n: u32 = rest.parse().map_err(|_| format!("bad sample offset in {token:?}"))?;
                if n > 255 {
                    return Err(format!(
                        "sample offset is 0..255 in 1/256ths, or a fraction like o1/3, in {token:?}"
                    ));
                }
                ops.sound_offset = (n * 256) as u16;
            }
        } else if let Some(rest) = token.strip_prefix('p') {
            let n: u32 = rest.parse().map_err(|_| format!("bad probability in {token:?}"))?;
            if !(1..=100).contains(&n) {
                return Err(format!("probability must be 1..=100 in {token:?}"));
            }
            ops.probability = n as u8;
        } else if let Some(rest) = token.strip_prefix('d') {
            let (num, den) = rest
                .split_once('/')
                .ok_or_else(|| format!("delay needs a fraction like d1/6, got {token:?}"))?;
            let num: u32 = num.parse().map_err(|_| format!("bad delay numerator in {token:?}"))?;
            let den: u32 = den.parse().map_err(|_| format!("bad delay denominator in {token:?}"))?;
            if den == 0 {
                return Err(format!("delay denominator must be nonzero in {token:?}"));
            }
            ops.delay = Some((num, den));
        } else {
            return Err(format!("unknown row op {token:?}"));
        }
    }
    Ok(ops)
}

#[cfg(test)]
mod tests {
    use super::*;
    const QPB: u64 = 960_000;

    #[test]
    fn parses_the_three_ops() {
        let ops = parse_row_ops("ret3 p60 d1/6").unwrap();
        assert_eq!(ops.retrigger, 3);
        assert_eq!(ops.probability, 60);
        assert_eq!(ops.delay, Some((1, 6)));
    }

    #[test]
    fn order_is_free_and_empty_parses_empty() {
        assert_eq!(parse_row_ops("p50 ret2").unwrap(), RowOps {
            retrigger: 2,
            probability: 50,
            delay: None,
            sound: 0,
            sound_offset: 0,
            retrig_ramp: 0,
            trig_condition: 0,
        });
        assert!(parse_row_ops("").unwrap().is_empty());
    }

    #[test]
    fn sound_address_parses_and_zero_is_refused() {
        let ops = parse_row_ops("s5").unwrap();
        assert_eq!(ops.sound, 5);
        // `s0` is REFUSED, not accepted as 0. Zero is the SENTINEL meaning "let pitch pick the
        // slot", so accepting it would give two ways to say one thing — and one of them looks
        // like an explicit choice while being the opposite.
        assert!(parse_row_ops("s0").is_err());
        assert!(parse_row_ops("sx").is_err());
        // An unset sound is 0, which is how a blank cell reads.
        assert_eq!(parse_row_ops("ret2").unwrap().sound, 0);
    }

    #[test]
    fn sample_offset_is_written_coarse_and_stored_fine() {
        // Written in 1/256ths for tracker muscle memory (the 9xx notation), stored at full u16
        // resolution — the coarse notation is for the hands, not a limit on what is expressible.
        assert_eq!(parse_row_ops("o128").unwrap().sound_offset, 128 * 256);
        assert_eq!(parse_row_ops("o0").unwrap().sound_offset, 0);
        // THE FRACTION FORM reaches resolutions 1/256ths cannot express. o1/3 is not
        // representable as N/256 at all, which is the point of having it.
        assert_eq!(parse_row_ops("o1/2").unwrap().sound_offset, 32768);
        assert_eq!(parse_row_ops("o1/3").unwrap().sound_offset, 21845);
        assert_eq!(parse_row_ops("o1/65535").unwrap().sound_offset, 1);
        // The two forms agree where they overlap: 128/256 and 1/2 are the same place.
        assert_eq!(
            parse_row_ops("o128").unwrap().sound_offset,
            parse_row_ops("o1/2").unwrap().sound_offset
        );
        // Refused, not clamped.
        assert!(parse_row_ops("o5/4").is_err());
        assert!(parse_row_ops("o1/0").is_err());
        assert!(parse_row_ops("o1/70000").is_err());
        assert!(parse_row_ops("o256").is_err());
        assert!(parse_row_ops("o256").is_err());
        assert!(parse_row_ops("oz").is_err());
    }

    #[test]
    fn sound_token_is_not_mistaken_for_anything_else() {
        // The single-letter prefixes must not swallow each other. "s" is checked before "p"/"d",
        // and "ret" before all of them.
        let ops = parse_row_ops("ret3 s7 o64 p60 d1/6").unwrap();
        assert_eq!(ops.retrigger, 3);
        assert_eq!(ops.sound, 7);
        assert_eq!(ops.sound_offset, 64 * 256);
        assert_eq!(ops.probability, 60);
        assert_eq!(ops.delay, Some((1, 6)));
    }

    #[test]
    fn ret_is_not_mistaken_for_probability() {
        // "ret3" starts with no 'p'/'d', but the token also is not a bare number;
        // the ret-first check keeps it out of the probability branch.
        let ops = parse_row_ops("ret3").unwrap();
        assert_eq!(ops.retrigger, 3);
        assert_eq!(ops.probability, 0);
    }

    #[test]
    fn delay_resolves_against_the_beat() {
        let ops = parse_row_ops("d1/6").unwrap();
        assert_eq!(ops.delay_nanoticks(QPB), (QPB / 6) as u32);
        let half = parse_row_ops("d1/2").unwrap();
        assert_eq!(half.delay_nanoticks(QPB), (QPB / 2) as u32);
    }

    #[test]
    fn the_sound_address_is_written_unpadded() {
        // THE RULED SPELLING (owner, revising section 8 Q1): the id, UNPADDED. "s9 and s09 should
        // be the same thing" — so the canonical form is the one a person types.
        assert_eq!(format_row_ops(&parse_row_ops("s7").unwrap()), "s7");
        assert_eq!(format_row_ops(&parse_row_ops("s137").unwrap()), "s137");
        assert_eq!(parse_row_ops(&format_row_ops(&parse_row_ops("s7").unwrap())).unwrap(),
                   parse_row_ops("s7").unwrap());
    }

    #[test]
    fn a_padded_sound_address_is_the_same_slot() {
        // AN EQUIVALENCE, NOT A FORMAT, and pinned as one deliberately.
        //
        // `s7`, `s07` and `s007` have always parsed to the same slot; padding only ever decided
        // which of them got written back out. That makes this the assertion a FORMATTING opinion
        // could break by accident — someone deciding padding is better after all and making the
        // parser strict to match would pass every test about the formatter and silently change
        // what old rows mean.
        let bare = parse_row_ops("s9").unwrap();
        assert_eq!(parse_row_ops("s09").unwrap(), bare);
        assert_eq!(parse_row_ops("s009").unwrap(), bare);
        assert_eq!(bare.sound, 9);
    }

    #[test]
    fn formatting_round_trips_every_op_including_the_awkward_offsets() {
        // The emitter is only worth having if it is the parser's inverse. A coarse offset comes
        // back coarse; one that never was a whole 1/256th comes back as the reduced fraction,
        // because rounding it to the nearest 1/256th would MOVE THE SEEK.
        for text in ["ret3", "p60", "d1/6", "s07", "o80", "o1/3",
                     "ret4 p25 d1/8 s12 o128", "ret2 s99 o1/7"] {
            let parsed = parse_row_ops(text).unwrap();
            let printed = format_row_ops(&parsed);
            let reparsed = parse_row_ops(&printed)
                .unwrap_or_else(|e| panic!("{text:?} printed as {printed:?} which will not parse: {e}"));
            assert_eq!(parsed, reparsed, "{text:?} printed as {printed:?}");
        }
        assert_eq!(format_row_ops(&RowOps::default()), "");
    }

    #[test]
    fn the_elektron_ops_parse_format_and_round_trip() {
        // THE RAMP is a signed total, and the sign is the whole gesture: a decrescendo roll and
        // a crescendo roll are the same op with the other sign.
        assert_eq!(parse_row_ops("rv-60").unwrap().retrig_ramp, -60);
        assert_eq!(parse_row_ops("rv50").unwrap().retrig_ramp, 50);
        assert_eq!(format_row_ops(&parse_row_ops("rv-60").unwrap()), "rv-60");

        // `rv` MUST NOT BE MISTAKEN FOR `ret`. Both start with "r", and the loop's rule is
        // longest-and-most-specific-first; this is the assertion that keeps it true.
        let both = parse_row_ops("ret4 rv-60").unwrap();
        assert_eq!((both.retrigger, both.retrig_ramp), (4, -60));

        // THE CONDITION round-trips through its packing and prints as it was typed.
        let c = parse_row_ops("c3:4").unwrap();
        assert_eq!(split_trig_condition(c.trig_condition), (3, 4));
        assert_eq!(format_row_ops(&c), "c3:4");

        // REFUSED, not normalised. A condition that can never fire is not something anyone
        // typed on purpose, and a red cell beats a row that silently never sounds.
        assert!(parse_row_ops("c3:2").is_err(), "A > B must be refused");
        assert!(parse_row_ops("c0:4").is_err(), "A = 0 must be refused");
        assert!(parse_row_ops("c1:9").is_err(), "B > 8 must be refused");
        assert!(parse_row_ops("c2").is_err(), "a condition without a colon must be refused");

        // PRE AND NOT PRE round-trip as their own tokens (#107). They carry no A:B, so
        // split_trig_condition must report them as NOT an A:B form rather than unpacking the
        // code into a pair — 130 would otherwise decode to a plausible-looking (a, b) and print
        // as a c-form, which is the quiet way a stateful condition becomes a wrong pass filter.
        let pre = parse_row_ops("cpre").unwrap();
        assert_eq!(pre.trig_condition, TRIG_CONDITION_PRE);
        assert_eq!(format_row_ops(&pre), "cpre");
        assert_eq!(split_trig_condition(pre.trig_condition), (0, 0));
        let npre = parse_row_ops("cnpre").unwrap();
        assert_eq!(npre.trig_condition, TRIG_CONDITION_NOT_PRE);
        assert_eq!(format_row_ops(&npre), "cnpre");
        assert!(!pre.is_empty(), "a PRE trig is an op, so the ops column must appear for it");

        // FILL IS NOT PARSEABLE. It is reserved (128/129) and unimplemented, and a token that
        // round-trips through the editor and then always sounds is worse than a refusal.
        assert!(parse_row_ops("cfill").is_err(), "FILL must be refused until it is implemented");
        assert!(parse_row_ops("rv200").is_err(), "a ramp past +-100% must be refused");

        // AND THE WHOLE SET SURVIVES A ROUND TRIP TOGETHER, which is the property that catches
        // an emitter that prints two ops in an order the parser cannot read back.
        let all = parse_row_ops("ret4 p60 d1/6 rv-60 c1:2 s07 o80").unwrap();
        assert_eq!(parse_row_ops(&format_row_ops(&all)).unwrap(), all);
    }

    #[test]
    fn op_schema_and_emitter_agree_on_order() {
        // ONE CANONICAL ORDER, ASSERTED — not two that happen to match.
        //
        // OP_SCHEMA and format_row_ops each fixed an order independently and they disagreed: the
        // table said `o` fifth, the emitter put it last. Nothing broke, because the parser is
        // order-free and both strings mean the same row — which is exactly why it survived. But
        // OP_SCHEMA is what fixes the draw order of a collapsed glyph run, so a person read
        // `r v p d s o c` in the cell and then opened it to find the ops rearranged. Same note,
        // two orders, one file.
        //
        // Found by the web-UI agent, whose mirror ratchet held their table equal to OP_SCHEMA and
        // was blind to the emitter entirely — a mirror cannot be right about both when the source
        // disagrees with itself. So the comparison that matters is this one, inside the source.
        let all = parse_row_ops("ret4 rv-60 p60 d1/6 s7 o80 c1:2").unwrap();
        let printed = format_row_ops(&all);

        // Which prefix each emitted token starts with, LONGEST FIRST — "ret" and "rv" share an
        // 'r', so a shortest-match scan would call `ret4` an `r`-something and prove nothing.
        let mut by_len: Vec<&str> = OP_SCHEMA.iter().map(|s| s.prefix).collect();
        by_len.sort_by_key(|p| std::cmp::Reverse(p.len()));
        let emitted: Vec<&str> = printed
            .split_whitespace()
            .map(|tok| {
                *by_len
                    .iter()
                    .find(|p| tok.starts_with(**p))
                    .unwrap_or_else(|| panic!("emitted token {tok:?} matches no OP_SCHEMA prefix"))
            })
            .collect();
        let schema: Vec<&str> = OP_SCHEMA.iter().map(|s| s.prefix).collect();
        assert_eq!(
            emitted, schema,
            "format_row_ops emits its ops in a different order from OP_SCHEMA. Both are \
             canonical and there can only be one — fix whichever is wrong, and remember the \
             table also fixes the draw order of a collapsed cell."
        );
    }

    #[test]
    fn a_row_carrying_only_an_elektron_op_is_not_empty() {
        // Same rule as the sound address: these change what you HEAR, so a row carrying one
        // must show the marker R5 describes or the op is invisible in the editor.
        assert!(!parse_row_ops("rv-60").unwrap().is_empty());
        assert!(!parse_row_ops("c1:2").unwrap().is_empty());
    }

    #[test]
    fn a_row_carrying_only_a_sound_address_is_not_empty() {
        // R5 draws the ops column per track and marks the note cell when a row carries an op. A
        // sound address IS an op — and the one you cannot infer from the pitch — so a row with
        // nothing but `s07` reporting itself empty would have hidden exactly the op that most
        // needs showing.
        assert!(!parse_row_ops("s7").unwrap().is_empty());
        assert!(!parse_row_ops("o80").unwrap().is_empty());
        assert!(parse_row_ops("").unwrap().is_empty());
    }

    #[test]
    fn malformed_tokens_error_rather_than_silently_drop() {
        assert!(parse_row_ops("ret0").is_err(), "retrigger 0 is invalid");
        assert!(parse_row_ops("p0").is_err(), "probability 0 is invalid (use no op)");
        assert!(parse_row_ops("p150").is_err(), "probability over 100");
        assert!(parse_row_ops("d1").is_err(), "delay needs a fraction");
        assert!(parse_row_ops("d1/0").is_err(), "zero denominator");
        assert!(parse_row_ops("wobble").is_err(), "unknown op");
    }
}
