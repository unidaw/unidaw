# Tracker Scrolling + Editing Plan

## Current State (Audit)
- `VISIBLE_ROWS`, `LINES_PER_BEAT`, `ROW_HEIGHT` are fixed constants in
  `ui/daw-app/src/main.rs`.
- `scroll_row_offset` + `cursor_row` are row indices; nanoticks are derived from
  `(scroll_row_offset + cursor_row) * row_nanoticks`.
- Follow-playhead recenters by rows in render.
- No zoom; no mouse wheel.
- No selection model; only click to focus a cell or harmony row.
- Rendering is literal; no aggregation or semantic zoom.

## Goals
- Infinite timeline viewport: cursor stays grid-snapped, viewport scrolls in
  nanoticks.
- Zoom changes nanoticks-per-row only; no quantization.
- Page ops target visible rows by default; selection ops target explicit ranges.
- Semantic zoom aggregates only when data would overlap.
- Minimap ends at last data point; click jumps viewport.

## Phase A — Model + Input
1) Viewport model
   - Add `nanoticks_per_row` (derived from zoom level).
   - Convert `scroll_row_offset` to `scroll_nanotick_offset` (or keep rows but
     computed from nanoticks-per-row).
   - Cursor stays snapped to row grid at current zoom.

2) Input + zoom
   - `Cmd+Wheel`: zoom in/out (changes nanoticks-per-row).
   - `Shift+Wheel`: micro-scroll the viewport (no row snap).
  - `Cmd+G`: jump dialog; parse:
    - `24` -> `24:1:0`
    - `24:2:0` -> exact bar:beat:tick.
  - Follow-playhead centers viewport using nanoticks.
  - `PageUp/PageDown` (Fn+Up/Down on macOS): jump viewport by one visible page.
  - Keyboard shortcut to halve/double page size (nanoticks displayed).

3) Selection model
   - Selection = nanotick range + column mask.
   - Mouse paint:
     - Click-drag creates selection.
     - Shift-drag extends.
     - Alt-drag subtracts.
   - Keyboard expansion:
     - `Shift+Up/Down`: expand by 1 row.
     - `Cmd+Shift+Up/Down`: expand to bar boundary.
     - `Shift+Left/Right`: adjust column mask.

## Phase B — Page + Ops
4) Page semantics
   - Page = visible rows in viewport.
   - `Shift-F3/F4/F5`: cut/copy/paste Page always (even if selection exists).
   - `Cmd+C/X/V`: selection only; no selection -> toast.

5) Loop
   - `Cmd+L`: loop selection; if none, loop Page.

## Edge Cases to Watch
- Duplicate boundary: `Cmd+D` should paste to the next row boundary at the
  current zoom (bar-level zoom pastes to next bar; 64th-level to next 64th).
- Mouse paint selection should be row-snapped (tracker feel), not raw nanoticks.
- Follow-playhead should use a dead zone or smooth catch-up to avoid jitter
  (only scroll when playhead crosses a threshold like 50% or 75%).

## Phase C — Rendering + Semantic Zoom
6) Row rendering
   - Use nanoticks-per-row for time labels and playhead mapping.
   - Empty rows show grid only; content rows scale text.

7) Semantic zoom
   - Aggregate only when tokens would overlap.
   - Note column pills:
     - `[16]` or `[16x C-1]` if all same pitch.
   - Harmony column pills:
     - `[3 scales]` or `maj->dor->lyd` when space allows.
   - Optional density bar behind pills.

## Phase D — Minimap
8) Minimap
   - Density strip ends at last data point (notes/automation/harmony).
   - Click to jump viewport; drag to scrub.

## Phase E — Tests
9) Tests to add
   - Page cut/copy/paste (Shift-F3/F4/F5).
   - `Cmd+C` no selection -> toast.
   - Cmd-G parsing (`24` -> `24:1:0`).
   - Zoom aggregation only when data overlaps.
   - Mouse paint selection range + column mask.
