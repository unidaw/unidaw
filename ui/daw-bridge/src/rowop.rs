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

pub const OP_SCHEMA: &[OpSpec] = &[
    OpSpec { prefix: "ret", summary: "retrigger N even strikes over the note", example: "ret3" },
    OpSpec { prefix: "p", summary: "probability percent to sound (1-100)", example: "p60" },
    OpSpec { prefix: "d", summary: "delay onset by a fraction of a beat", example: "d1/6" },
    OpSpec { prefix: "s", summary: "play sampler slot N, zero-padded (blank = pitch picks it)", example: "s07" },
    OpSpec { prefix: "o", summary: "start N/256 into the sample, or a fraction like o1/3 (the 9xx seek)", example: "o80" },
];

/// The canonical text for a set of ops — the inverse of `parse_row_ops`.
///
/// THE SOUND ADDRESS IS ZERO-PADDED (`s07`), by owner ruling (docs/SAMPLER_DESIGN.md section 8
/// Q1). It is the id, not a name, so a rename rewrites nothing and a row means the same thing
/// after any amount of kit editing. Padded because a tracker cell lives in a fixed-width grid and
/// ragged `s7` / `s13` breaks the vertical rhythm that makes a tracker readable at a glance.
///
/// `sound_width` is the track's field width — two digits normally, three for a track that
/// references an id >= 100, which a long-edited 64-pad kit can reach because `next_slot_id` never
/// reuses. It is per track for the same reason R5 makes the ops column per track. A width smaller
/// than the number needs is widened rather than truncated: a truncated id is a DIFFERENT slot,
/// and silently playing the wrong sound to keep a column narrow is not a trade worth making.
///
/// This lives beside the parser so there is exactly one definition of the spelling. Display is
/// the frontend's call (R5), but "which characters mean slot 7" is not, or the two sides drift.
pub fn format_row_ops(ops: &RowOps, sound_width: usize) -> String {
    let mut out: Vec<String> = Vec::new();
    if ops.retrigger > 1 {
        out.push(format!("ret{}", ops.retrigger));
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
        let digits = ops.sound.to_string().len();
        let width = sound_width.max(digits);
        out.push(format!("s{:0width$}", ops.sound, width = width));
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
        if let Some(rest) = token.strip_prefix("ret") {
            let n: u8 = rest.parse().map_err(|_| format!("bad retrigger count in {token:?}"))?;
            if n < 1 {
                return Err(format!("retrigger count must be >= 1 in {token:?}"));
            }
            ops.retrigger = n;
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
    fn the_sound_address_formats_zero_padded_and_round_trips() {
        // THE RULED SPELLING (section 8 Q1): the id, zero-padded, so a tracker column stays
        // aligned and a rename rewrites nothing.
        let ops = parse_row_ops("s7").unwrap();
        assert_eq!(format_row_ops(&ops, 2), "s07");
        assert_eq!(format_row_ops(&ops, 3), "s007");

        // WIDENED, NEVER TRUNCATED. A truncated id is a different slot, and playing the wrong
        // sound to keep a column narrow is not a trade worth making.
        let big = parse_row_ops("s137").unwrap();
        assert_eq!(format_row_ops(&big, 2), "s137");

        // And the padding must not change what it parses back to, which is the whole point of
        // choosing the id over the name.
        assert_eq!(parse_row_ops("s07").unwrap().sound, 7);
        assert_eq!(parse_row_ops("s007").unwrap().sound, 7);
        assert_eq!(parse_row_ops(&format_row_ops(&ops, 2)).unwrap(), ops);
    }

    #[test]
    fn formatting_round_trips_every_op_including_the_awkward_offsets() {
        // The emitter is only worth having if it is the parser's inverse. A coarse offset comes
        // back coarse; one that never was a whole 1/256th comes back as the reduced fraction,
        // because rounding it to the nearest 1/256th would MOVE THE SEEK.
        for text in ["ret3", "p60", "d1/6", "s07", "o80", "o1/3",
                     "ret4 p25 d1/8 s12 o128", "ret2 s99 o1/7"] {
            let parsed = parse_row_ops(text).unwrap();
            let printed = format_row_ops(&parsed, 2);
            let reparsed = parse_row_ops(&printed)
                .unwrap_or_else(|e| panic!("{text:?} printed as {printed:?} which will not parse: {e}"));
            assert_eq!(parsed, reparsed, "{text:?} printed as {printed:?}");
        }
        assert_eq!(format_row_ops(&RowOps::default(), 2), "");
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
