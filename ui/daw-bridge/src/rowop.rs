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
}

impl RowOps {
    pub fn is_empty(&self) -> bool {
        self.retrigger == 0 && self.probability == 0 && self.delay.is_none()
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
];

/// Parses space-separated row-op tokens (`"ret3 p60 d1/6"`) into `RowOps`.
/// Unknown or malformed tokens are an error, never a silent no-op — a red cell,
/// not a dropped op, is the tracker rule.
pub fn parse_row_ops(input: &str) -> Result<RowOps, String> {
    let mut ops = RowOps::default();
    for token in input.split_whitespace() {
        // Order matters: "ret" is checked before the single-letter "p"/"d" so
        // it is not mis-read as a probability token.
        if let Some(rest) = token.strip_prefix("ret") {
            let n: u8 = rest.parse().map_err(|_| format!("bad retrigger count in {token:?}"))?;
            if n < 1 {
                return Err(format!("retrigger count must be >= 1 in {token:?}"));
            }
            ops.retrigger = n;
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
        });
        assert!(parse_row_ops("").unwrap().is_empty());
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
    fn malformed_tokens_error_rather_than_silently_drop() {
        assert!(parse_row_ops("ret0").is_err(), "retrigger 0 is invalid");
        assert!(parse_row_ops("p0").is_err(), "probability 0 is invalid (use no op)");
        assert!(parse_row_ops("p150").is_err(), "probability over 100");
        assert!(parse_row_ops("d1").is_err(), "delay needs a fraction");
        assert!(parse_row_ops("d1/0").is_err(), "zero denominator");
        assert!(parse_row_ops("wobble").is_err(), "unknown op");
    }
}
