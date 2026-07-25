//! Per-lane tracker grids (Mock B: beat-anchored, per-lane subdivision).
//!
//! The tracker's rows are a *view* over tick positions, not storage — notes
//! carry absolute nanoticks and never move when the grid changes. This module
//! is the projection between the two, and it is deliberately free of any UI
//! framework so every front end (and the CLI, and tests) shares one definition.
//!
//! The invariant that keeps polyrhythm readable: **beats always land on a row
//! boundary**, whatever a lane's subdivision. Beat 2 is at the same tick for a
//! 16th lane and a triplet lane; only the rows *within* a beat differ. So
//! "which row am I on" is only ever asked within one lane, and cross-lane
//! questions ("what plays at the same time") are asked in ticks or beats, which
//! are shared.

/// Nanoticks per quarter note. Highly divisible (2^9 · 3 · 5^4) so the common
/// musical subdivisions — halves, triplets, 16ths, sextuplets, etc. — divide it
/// exactly and their beats align to the tick.
pub const NANOTICKS_PER_QUARTER: u64 = 960_000;

/// A lane's subdivision. `lines_per_beat` is the number of rows one beat is cut
/// into: 4 = 16ths, 3 = triplets, 6 = sextuplets, 1 = one row per beat.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct LaneGrid {
    pub lines_per_beat: u32,
}

impl LaneGrid {
    pub const fn new(lines_per_beat: u32) -> Self {
        Self {
            lines_per_beat: if lines_per_beat == 0 { 1 } else { lines_per_beat },
        }
    }

    /// Whether this subdivision divides a beat exactly, so its rows align to
    /// the tick with no rounding. The common subdivisions do; a 7-tuplet does
    /// not, and its grid is approximate (still beat-anchored, but rows within a
    /// beat are not all equal length).
    pub fn is_exact(&self) -> bool {
        NANOTICKS_PER_QUARTER % self.lines_per_beat as u64 == 0
    }

    /// Duration of one row in this lane, in nanoticks.
    pub fn row_nanoticks(&self) -> u64 {
        NANOTICKS_PER_QUARTER / self.lines_per_beat as u64
    }

    /// The beat a tick belongs to (0-based), independent of subdivision.
    pub fn beat_of_tick(&self, tick: u64) -> u64 {
        tick / NANOTICKS_PER_QUARTER
    }

    /// The subdivision index within its beat for a tick (0..lines_per_beat).
    pub fn sub_of_tick(&self, tick: u64) -> u32 {
        let within = tick % NANOTICKS_PER_QUARTER;
        (within / self.row_nanoticks()) as u32
    }

    /// The tick at the top of `sub` within `beat` — the beat-anchored position.
    pub fn tick_of(&self, beat: u64, sub: u32) -> u64 {
        beat * NANOTICKS_PER_QUARTER + sub as u64 * self.row_nanoticks()
    }

    /// Absolute row index for a tick, counting rows from tick 0. Two lanes with
    /// different subdivisions give different absolute rows for the same tick —
    /// which is exactly why cross-lane navigation goes through ticks, not rows.
    pub fn row_of_tick(&self, tick: u64) -> u64 {
        self.beat_of_tick(tick) * self.lines_per_beat as u64
            + self.sub_of_tick(tick) as u64
    }

    /// The tick at the top of an absolute row.
    pub fn tick_of_row(&self, row: u64) -> u64 {
        let beat = row / self.lines_per_beat as u64;
        let sub = (row % self.lines_per_beat as u64) as u32;
        self.tick_of(beat, sub)
    }

    /// Snaps a tick down to this lane's nearest row boundary at or before it.
    pub fn snap(&self, tick: u64) -> u64 {
        self.tick_of_row(self.row_of_tick(tick))
    }
}

/// A selection is a half-open time span across a set of lanes — never a row
/// span, so it means the same thing regardless of each lane's subdivision.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TimeSpan {
    pub start_tick: u64,
    pub end_tick: u64,
}

impl TimeSpan {
    pub fn contains(&self, tick: u64) -> bool {
        tick >= self.start_tick && tick < self.end_tick
    }

    /// Length in whole beats, rounded down.
    pub fn beats(&self) -> u64 {
        self.end_tick.saturating_sub(self.start_tick) / NANOTICKS_PER_QUARTER
    }
}

/// Re-grids a set of tick positions copied from one lane into another: the
/// span is shifted to `dest_start` and each position keeps its offset, then
/// snaps to the destination lane's grid. This is paste-by-time — copy a beat
/// from a triplet lane, paste into a 16th lane, notes land where they fall.
pub fn remap_ticks(
    ticks: &[u64],
    source: TimeSpan,
    dest_start: u64,
    dest_grid: LaneGrid,
) -> Vec<u64> {
    ticks
        .iter()
        .filter(|&&t| source.contains(t))
        .map(|&t| dest_grid.snap(dest_start + (t - source.start_tick)))
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn beats_are_anchored_across_subdivisions() {
        // The whole design rests on this: beat N is the same tick for every
        // lane, whatever its subdivision.
        for lpb in [1u32, 2, 3, 4, 6, 8, 12, 16, 24] {
            let g = LaneGrid::new(lpb);
            for beat in 0..8u64 {
                assert_eq!(
                    g.tick_of(beat, 0),
                    beat * NANOTICKS_PER_QUARTER,
                    "lpb={lpb} beat={beat} not anchored"
                );
                // The row at the start of a beat lands exactly on the beat.
                let row = beat * lpb as u64;
                assert_eq!(g.tick_of_row(row), beat * NANOTICKS_PER_QUARTER);
            }
        }
    }

    #[test]
    fn triplets_are_expressible_and_exact() {
        let g = LaneGrid::new(3);
        assert!(g.is_exact(), "triplets divide a beat exactly");
        assert_eq!(g.row_nanoticks(), 320_000);
        // Three rows span exactly one beat.
        assert_eq!(g.tick_of(0, 0), 0);
        assert_eq!(g.tick_of(0, 1), 320_000);
        assert_eq!(g.tick_of(0, 2), 640_000);
        assert_eq!(g.tick_of(1, 0), 960_000);
    }

    #[test]
    fn tick_round_trips_through_row() {
        for lpb in [3u32, 4, 6] {
            let g = LaneGrid::new(lpb);
            for row in 0..32u64 {
                let tick = g.tick_of_row(row);
                assert_eq!(g.row_of_tick(tick), row, "lpb={lpb} row={row}");
            }
        }
    }

    #[test]
    fn four_against_three_offsets_are_visible() {
        // The 2nd 16th (row 1) and the 2nd triplet (row 1) of the same beat are
        // at different ticks — the polyrhythm the grid must show honestly.
        let sixteenths = LaneGrid::new(4);
        let triplets = LaneGrid::new(3);
        assert_eq!(sixteenths.tick_of(0, 1), 240_000);
        assert_eq!(triplets.tick_of(0, 1), 320_000);
        assert_ne!(sixteenths.tick_of(0, 1), triplets.tick_of(0, 1));
        // ...but both share beat boundaries.
        assert_eq!(sixteenths.tick_of(1, 0), triplets.tick_of(1, 0));
    }

    #[test]
    fn snap_lands_on_a_row_boundary() {
        let g = LaneGrid::new(4); // 240_000 per row
        assert_eq!(g.snap(0), 0);
        assert_eq!(g.snap(239_999), 0);
        assert_eq!(g.snap(240_000), 240_000);
        assert_eq!(g.snap(300_000), 240_000);
    }

    #[test]
    fn paste_by_time_regrids_into_the_destination_lane() {
        // Two notes on beats 0 and 0.5 of a 16th lane, pasted into a triplet
        // lane starting at beat 2: they keep their time offset and snap to the
        // triplet grid.
        let src = TimeSpan { start_tick: 0, end_tick: NANOTICKS_PER_QUARTER };
        let notes = [0u64, 480_000]; // beat 0, and the 8th-note
        let dest = LaneGrid::new(3);
        let out = remap_ticks(&notes, src, 2 * NANOTICKS_PER_QUARTER, dest);
        assert_eq!(out.len(), 2);
        assert_eq!(out[0], 2 * NANOTICKS_PER_QUARTER, "first note at the paste point");
        // 480_000 offset snaps to the nearest triplet row (320_000 or 640_000).
        assert_eq!(out[1], dest.snap(2 * NANOTICKS_PER_QUARTER + 480_000));
        assert_eq!(out[1], 2 * NANOTICKS_PER_QUARTER + 320_000);
    }

    #[test]
    fn non_divisor_subdivision_is_flagged_inexact() {
        assert!(!LaneGrid::new(7).is_exact());
        assert!(LaneGrid::new(5).is_exact()); // 960000 / 5 = 192000
    }
}
