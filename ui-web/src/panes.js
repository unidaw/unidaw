/**
 * The shell's resizable, collapsible regions.
 *
 * splitter.js has been complete for a long time — handles, keyboard, clamping,
 * double-click-to-home, persistence helpers, its own stylesheet. Nothing ever
 * called `new Splitter`. So the rail, the right dock, the chain strip and the
 * dock's own cells were all fixed at whatever shell.css said, and the pending
 * card sat at 83px with its text clipped while 300px of empty log went spare
 * directly underneath it. Jaakko: "I can't read what's in the second one."
 *
 * That is the third time this session: a capability, a surface, and nothing
 * joining them. It does not show up as a bug because nothing is broken — the
 * code is right, the CSS is right, and the feature is simply absent.
 *
 * This file is the join. It owns no layout of its own: every size it sets is a
 * CSS custom property declared in shell.css, so the rules stay in one place and
 * this stays a wiring file.
 */

import { Splitter, splitterSizes, restoreSplitterSizes, refreshSplitters } from './splitter.js';

/** Smallest a region may be dragged to. Below this it is not a small region, it
 *  is a region you cannot get back without knowing the handle is still there. */
const MIN_RAIL = 180, MIN_DOCK = 240, MIN_CHAIN = 96, MIN_CELL = 64;

/**
 * A collapse toggle in a cell's header.
 *
 * The chevron is the affordance AND the state: pointing down means "there is
 * more below", pointing right means "this is folded". No separate label, because
 * the header already says what the cell is and the only question left is whether
 * you can see it.
 */
function collapseButton(cell, headSel, onChange) {
  const head = cell.querySelector(headSel);
  if (!head) return null;
  const b = document.createElement('button');
  b.className = 'cell-fold';
  b.type = 'button';
  const i = document.createElement('i');
  i.className = 'ph ph-caret-down';
  b.appendChild(i);
  b.title = 'Collapse';
  b.addEventListener('click', (e) => {
    e.stopPropagation();                 // the header may select something
    const now = cell.classList.toggle('cell-collapsed');
    i.className = now ? 'ph ph-caret-right' : 'ph ph-caret-down';
    b.title = now ? 'Expand' : 'Collapse';
    b.setAttribute('aria-expanded', String(!now));
    if (onChange) onChange();
  });
  b.setAttribute('aria-expanded', 'true');
  head.appendChild(b);
  return b;
}

/**
 * Build every splitter and every fold toggle.
 *
 * @param {object} opts
 * @param {() => void} opts.onSize  redraw — a region changing width changes how
 *   many rows and tracks fit, which is a layout the surfaces measure.
 * @param {() => void} opts.onEnd   save the session.
 */
export function createPanes({ onSize = () => {}, onEnd = () => {} } = {}) {
  const $ = (id) => document.getElementById(id);
  const made = [];
  const add = (host, opts) => {
    if (!host) return null;
    const s = new Splitter(host, { onSize, onEnd, ...opts });
    made.push(s);
    return s;
  };

  // The three shell regions. Each is grabbed on the edge that faces the centre,
  // because that is the edge a person reads as "the boundary of this panel".
  add($('browser'), {
    key: 'rail', edge: 'right', prop: '--shell-rail-w', label: 'Browser width',
    min: MIN_RAIL, max: () => Math.max(MIN_RAIL, innerWidth * 0.5), home: 266,
  });
  add($('rdock'), {
    key: 'dock', edge: 'left', prop: '--shell-dock-w', label: 'Right dock width',
    min: MIN_DOCK, max: () => Math.max(MIN_DOCK, innerWidth * 0.5), home: 332,
  });
  add($('chain'), {
    key: 'chain', edge: 'top', prop: '--shell-chain-h', label: 'Device chain height',
    min: MIN_CHAIN, max: () => Math.max(MIN_CHAIN, innerHeight * 0.6), home: 196,
  });

  // The right dock's own cells. Grabbed on the BOTTOM, so the handle sits on the
  // boundary with the cell underneath — dragging down grows the cell you grabbed,
  // which is the direction people expect from a handle they see at its foot.
  //
  // The agent takes the remainder and so has no splitter of its own: it is sized
  // by what the two above it leave, and giving it a handle would let the three
  // heights disagree about how tall the dock is.
  add($('harmony'), {
    key: 'harmonyH', edge: 'bottom', prop: '--rdock-harmony-h', label: 'Harmony height',
    min: MIN_CELL, max: () => Math.max(MIN_CELL, innerHeight * 0.7), home: 345,
  });
  add($('pending'), {
    key: 'pendingH', edge: 'bottom', prop: '--rdock-pending-h', label: 'Pending height',
    min: MIN_CELL, max: () => Math.max(MIN_CELL, innerHeight * 0.7), home: 150,
  });

  /*
   * The divider between the two centre panes.
   *
   * On its own element rather than on the surface below it. The pane's contents
   * change — it can hold any view — and a handle bound to whichever surface
   * happened to be there would have to be rebound every time the view changed,
   * or would vanish with it. `#paneEdge` exists whether or not the split is
   * open, so the splitter binds once.
   *
   * Grabbed on its TOP edge, so dragging up gives the lower pane more room —
   * the direction the handle's position implies.
   */
  add($('paneEdge'), {
    key: 'splitH', edge: 'top', prop: '--stage-split-h', label: 'Second pane height',
    min: 120, max: () => Math.max(120, innerHeight * 0.7), home: 300,
  });

  const folds = [
    collapseButton($('harmony'), '.hm-head', () => { onSize(); onEnd(); }),
    collapseButton($('pending'), '.pd-head', () => { onSize(); onEnd(); }),
    collapseButton($('dock'), '.dk-head', () => { onSize(); onEnd(); }),
  ].filter(Boolean);

  addEventListener('resize', refreshSplitters);

  return {
    splitters: made,
    foldCount: folds.length,
    sizes: splitterSizes,
    restore: restoreSplitterSizes,
    /** Which cells are folded, for the session record. */
    folded() {
      const out = {};
      for (const id of ['harmony', 'pending', 'dock']) {
        const el = $(id);
        if (el) out[id] = el.classList.contains('cell-collapsed');
      }
      return out;
    },
    /** Apply saved fold state. Unknown ids and non-booleans are ignored. */
    setFolded(saved) {
      if (!saved || typeof saved !== 'object') return 0;
      let n = 0;
      for (const id of ['harmony', 'pending', 'dock']) {
        if (typeof saved[id] !== 'boolean') continue;
        const el = $(id);
        if (!el) continue;
        el.classList.toggle('cell-collapsed', saved[id]);
        const icon = el.querySelector('.cell-fold i');
        if (icon) icon.className = saved[id] ? 'ph ph-caret-right' : 'ph ph-caret-down';
        n++;
      }
      return n;
    },
  };
}
