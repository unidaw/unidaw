// The patcher: the engine's node graph, laid out.
//
// The engine publishes nodes and edges but NO positions — it has no notion of
// layout, and it should not. So this computes one, and node ids are stable, which
// means a hand-placed position could be persisted against an id later without
// changing anything here.
//
// Caveat carried from backend, stated rather than buried: the engine runs ONE
// global graph today, parked on a device. It is real — the engine executes it —
// but it is not yet per-device, so this surface shows one graph however many
// devices a project has. The read shape does not change when that lands.
//
// THE BOX MODEL LIVES HERE, not in patcher.css. It used to live in both: the
// model laid cables out against a 220x98 box while the stylesheet drew a 210x76
// one, so every cable started 10px past the node's right edge and 11px above its
// centre and ended in empty space. Two copies of one number is the same bug as
// GUIDELINES 3.11's hand-computed scroll extent, so the renderer now writes the
// width and height it reads from here and the stylesheet has no opinion.

/** PatcherNodeType, from daw_bridge::layout. */
export const NODE_TYPES = [
  'kernel', 'euclidean', 'passthru', 'audio', 'lfo', 'random', 'out',
];

/** PatcherPortKind, from apps/patcher_graph.h. Also the edge kinds on the wire. */
export const EDGE_KINDS = ['event', 'audio', 'control'];
const KIND_EVENT = 0;
const KIND_AUDIO = 1;
const KIND_CONTROL = 2;

/**
 * Port ids, fixed by the engine (apps/patcher_graph.h). They are global to the
 * whole patcher rather than per node type, which is why a node can be missing
 * one entirely: an `out` node's only port is event-in 0, and asking it for
 * port 1 is a question with no answer.
 */
export const PORT_EVENT_IN = 0;
export const PORT_EVENT_OUT = 1;
export const PORT_CONTROL_IN = 2;
export const PORT_CONTROL_OUT = 3;
export const PORT_AUDIO_IN = 4;
export const PORT_AUDIO_OUT = 5;
const PORT_ID_COUNT = 6;

/**
 * Which ports each node type has, as [portId, kind, isOutput].
 *
 * This mirrors `portsForNode()` in apps/patcher_graph.cpp and `ports_for()` in
 * the sidecar — three copies of one table, which is a mirror and therefore the
 * thing GUIDELINES 5 warns about. It is here rather than derived because the
 * engine does not publish it: the frame carries a node's TYPE and an edge's
 * port ids, and nothing that says what ports a type owns. Keep the three in
 * step; the engine is the one that decides.
 */
const PORTS_BY_TYPE = [
  // kernel (RustKernel): events and control, both ways.
  [[PORT_EVENT_IN, KIND_EVENT, 0], [PORT_EVENT_OUT, KIND_EVENT, 1],
   [PORT_CONTROL_IN, KIND_CONTROL, 0], [PORT_CONTROL_OUT, KIND_CONTROL, 1]],
  // euclidean: a source of events and nothing else.
  [[PORT_EVENT_OUT, KIND_EVENT, 1]],
  // passthru
  [[PORT_EVENT_IN, KIND_EVENT, 0], [PORT_EVENT_OUT, KIND_EVENT, 1]],
  // audio (AudioPassthrough): stereo in, stereo out.
  [[PORT_AUDIO_IN, KIND_AUDIO, 0], [PORT_AUDIO_OUT, KIND_AUDIO, 1]],
  // lfo: control out only.
  [[PORT_CONTROL_OUT, KIND_CONTROL, 1]],
  // random (RandomDegree)
  [[PORT_EVENT_IN, KIND_EVENT, 0], [PORT_EVENT_OUT, KIND_EVENT, 1]],
  // out (EventOut): a sink.
  [[PORT_EVENT_IN, KIND_EVENT, 0]],
];

/** What a port is called on the box. Short, because it sits outside a 224px node. */
const PORT_LABEL = ['ev', 'aud', 'ctl'];

/**
 * Config is eight i32s whose meaning depends on the node type. Rendering them as
 * "cfg[0]=16" would be technically true and useless, so each type names its own.
 * A type with no entry here shows nothing rather than eight anonymous numbers.
 */
const CONFIG_FIELDS = {
  // duration_ticks 0 is a SENTINEL, not a length: backend confirmed the Rust
  // processor substitutes step_ticks/2 (half a Euclidean step) when it sees zero.
  // Showing "dur 0.00b" for something that sounds like half a step is exactly
  // the confident-wrong number this file exists to avoid.
  euclidean: ['steps', 'hits', 'offset', 'degree', 'oct', 'vel', 'base',
              ['dur', 960000, 'b', 'auto']],
  random: ['degree', 'vel', ['dur', 960000, 'b', 'auto']],
  // Stored as milli-units, so they are shown divided rather than as raw i32s.
  lfo: [['freq', 1000, 'Hz'], ['depth', 1000, ''], ['bias', 1000, ''], ['phase', 1000, '']],
};

/** How much one nudge moves each field, and what it may not exceed. */
const CONFIG_LIMITS = {
  euclidean: [[1, 1, 64], [1, 0, 64], [1, 0, 63], [1, 0, 12], [1, -4, 4], [1, 1, 127], [1, 0, 9],
              [120000, 0, 7680000]],
  random: [[1, 0, 12], [1, 1, 127], [120000, 0, 7680000]],
  // Milli-units, so a step of 100 is 0.1 Hz.
  lfo: [[100, 1, 20000], [50, 0, 1000], [50, -1000, 1000], [50, 0, 1000]],
};

/** The most fields any type has, which is how many rows a node box pools. */
const MAX_FIELDS = 8;

/**
 * One shared empty list. A draw with no engine yet — the whole time before the
 * socket comes up, at 60 fps — used to build two fresh `[]` per frame just to
 * read `.length` off them, and a type with no config built a third.
 */
const EMPTY = Object.freeze([]);

/**
 * Each type's field names in order, computed once because they are a function
 * of CONFIG_FIELDS and nothing else.
 *
 * This exists because the draw path wants ONE name — the field the keyboard is
 * on — and the only way to ask for it was `configFields()`, which builds an
 * array plus an object per field. That is up to nine allocations a frame, every
 * frame the patcher is open, to read a string that changes when you press Tab.
 */
const FIELD_NAMES = NODE_TYPES.map(
  (name) => (CONFIG_FIELDS[name] || EMPTY).map((f) => (Array.isArray(f) ? f[0] : f)));

/** The editable fields of a node type, as {name, index}. Empty if none. */
export function configFields(type) {
  const names = FIELD_NAMES[type];
  if (!names) return [];
  return names.map((name, index) => ({ name, index }));
}

/** Nudge one field, clamped. Returns the new eight-value config. */
/**
 * Pixels of vertical drag per one step of a node's value.
 *
 * A DRAG, not a mapping of the bar's width to the field's range. Two reasons:
 * several node fields declare no range at all (the bar is hidden for those, see
 * `frac < 0` in patcher.js), so there is nothing to map; and a value that jumps
 * to wherever you happened to press is the one behaviour nobody wants on a
 * parameter they are dialling in by ear.
 *
 * 6px is a compromise measured against the two extremes it has to serve:
 * `steps` on a euclidean runs 1..64, so a full-height drag should cross it, and
 * `velocity` runs 0..127 in steps of 1, where finer would be unusable.
 */
export const PATCH_DRAG_PX_PER_STEP = 6;

/**
 * How many steps a drag of `dy` pixels has asked for, given how many it has
 * already been credited with.
 *
 * Returns the DELTA to apply now, and the total to remember. Accumulating the
 * total rather than differencing consecutive events is what keeps a slow drag
 * from being rounded to nothing on every frame: five 2px moves are ten pixels
 * and one step, not five moves of zero.
 *
 * Up is positive. Screen y grows downward and every DAW disagrees with it here.
 */
export function dragSteps(dy, alreadyApplied, fine = false) {
  const px = PATCH_DRAG_PX_PER_STEP * (fine ? 4 : 1);
  const total = Math.trunc(-dy / px);
  return { delta: total - alreadyApplied, total };
}

export function nudgeConfig(type, config, fieldIndex, dir) {
  const limits = CONFIG_LIMITS[NODE_TYPES[type]];
  if (!limits || !limits[fieldIndex]) return null;
  const [step, lo, hi] = limits[fieldIndex];
  const out = Array.from(config);
  out[fieldIndex] = Math.max(lo, Math.min(hi, out[fieldIndex] + dir * step));
  return out;
}

/** One field's value as a person reads it — scaled, suffixed, sentinel named. */
function fieldValue(f, v) {
  if (!Array.isArray(f)) return '' + v;
  const [, scale, unit, zeroLabel] = f;
  if (zeroLabel && v === 0) return zeroLabel;
  return (v / scale).toFixed(2) + unit;
}

/** How full a field's bar is, or -1 when the type declares no range for it. */
function fieldFraction(type, i, v) {
  const limits = CONFIG_LIMITS[NODE_TYPES[type]];
  const lim = limits && limits[i];
  if (!lim) return -1;
  const [, lo, hi] = lim;
  if (hi === lo) return -1;
  return Math.max(0, Math.min(1, (v - lo) / (hi - lo)));
}

export function describeConfig(type, config) {
  const fields = CONFIG_FIELDS[NODE_TYPES[type]];
  if (!fields) return '';
  const parts = [];
  for (let i = 0; i < fields.length; i++) {
    const f = fields[i];
    parts.push((Array.isArray(f) ? f[0] : f) + ' ' + fieldValue(f, config[i]));
  }
  return parts.join('  ');
}

// ---------------------------------------------------------------------------
// Geometry. Measured off design/redesign/Uni.dc.html with the patcher open:
// its nodes are a 27px header over a body whose first port centre sits 18px
// below it, ports 20px apart, and the box ends 6px past the last port's step.
// The numbers here are that shape at this app's smaller type — the header is
// the design system's own 24px header height, and 13px rows match the device
// chain's parameter rows so a patcher node and a rack card read as one kit.
const NODE_W = 224;
const HEADER_H = 24;
/** First port centre, below the header. */
const PORT_TOP = 16;
const PORT_STEP = 18;
const ROW_H = 13;
const BODY_PAD = 8;
/** The port dot's diameter, which the renderer centres on the node's edge. */
export const PORT_SIZE = 10;

// A port's label hangs OUTSIDE its node — 21px clear of the edge plus the word
// — so the left margin and the column gap are not decoration: at the design's
// 24px padding the first column's `ev` label sat at x = -17 and the scroller
// simply never showed it.
const PAD_X = 64;
const PAD_Y = 24;
/** Both nodes' labels, back to back, plus a readable run of cable between them. */
const GAP_X = 124;
const GAP_Y = 26;

/**
 * Where every port of a node type sits on its box, computed once per type.
 *
 * `inY`/`outY` are indexed by PORT ID rather than by position, because that is
 * what an edge names: the frame carries `srcPort: 3`, not "the second output".
 * A -1 means this type has no such port in that direction.
 */
function layoutPorts(type) {
  const list = [];
  const inY = new Float64Array(PORT_ID_COUNT).fill(-1);
  const outY = new Float64Array(PORT_ID_COUNT).fill(-1);
  let ins = 0;
  let outs = 0;
  for (const [id, kind, out] of PORTS_BY_TYPE[type] || []) {
    const seat = out ? outs++ : ins++;
    const dy = HEADER_H + PORT_TOP + seat * PORT_STEP;
    list.push({ id, kind, kindName: EDGE_KINDS[kind], out: !!out,
                label: PORT_LABEL[kind], dx: out ? NODE_W : 0, dy });
    (out ? outY : inY)[id] = dy;
  }
  return { list, inY, outY, ins, outs };
}

const PORT_LAYOUT = NODE_TYPES.map((_, t) => layoutPorts(t));

/**
 * A node type this build has never heard of — one the engine learned since.
 * It draws NO ports rather than borrowing another type's, because the box would
 * then claim connections the node does not have, and an edge into it goes
 * undrawn rather than being aimed at an invented handle.
 */
const UNKNOWN_LAYOUT = { list: [], inY: new Float64Array(PORT_ID_COUNT).fill(-1),
                         outY: new Float64Array(PORT_ID_COUNT).fill(-1), ins: 0, outs: 0 };

/** The port layout for a node type. Never null; empty for a type we do not know. */
export function portsForType(type) {
  return PORT_LAYOUT[type] || UNKNOWN_LAYOUT;
}

/** A type's box height: whichever of its rows and its ports needs more room. */
const NODE_H = NODE_TYPES.map((name, t) => {
  const rows = (CONFIG_FIELDS[name] || []).length;
  const p = PORT_LAYOUT[t];
  const bodyRows = rows ? rows * ROW_H + BODY_PAD * 2 : BODY_PAD;
  const bodyPorts = PORT_TOP + Math.max(p.ins, p.outs, 1) * PORT_STEP + 6;
  return HEADER_H + Math.max(bodyRows, bodyPorts);
});
const UNKNOWN_H = HEADER_H + PORT_TOP + PORT_STEP + 6;

/** A type's box height, including one this build does not know. */
function heightOf(type) {
  return NODE_H[type] === undefined ? UNKNOWN_H : NODE_H[type];
}

export function createPatcherBuffer(nodeCap = 64, edgeCap = 128) {
  const nodes = new Array(nodeCap);
  for (let i = 0; i < nodeCap; i++) {
    const rows = new Array(MAX_FIELDS);
    for (let k = 0; k < MAX_FIELDS; k++) rows[k] = { name: '', value: '', frac: -1, selected: false };
    nodes[i] = { id: 0, type: 0, typeName: '', config: '', x: 0, y: 0, w: NODE_W, h: NODE_H[0],
                 selected: false, field: -1, fieldName: '', ports: PORT_LAYOUT[0].list,
                 rows, rowCount: 0, unpublished: false,
                 // The cache key for the row strings: everything they are computed
                 // from, which is the type, the published/pending eight, and
                 // whether there is a config at all.
                 _cfgType: -1, _cfgHas: false, _cfg: new Int32Array(MAX_FIELDS),
                 // The type this box's name and port list were bound for. -1 so
                 // the first bind always happens, whatever type lands here.
                 _typeFor: -1 };
  }
  const edges = new Array(edgeCap);
  for (let i = 0; i < edgeCap; i++) {
    edges[i] = { src: 0, dst: 0, kind: 0, kindName: '', path: '',
                 x1: 0, y1: 0, x2: 0, y2: 0, anchored: false, exact: false,
                 // The kind `kindName` was built for; -1 so the first bind runs.
                 _kindFor: -1 };
  }
  return { nodes, nodeCount: 0, edges, edgeCount: 0,
           version: -1, device: 0, empty: true, addType: '', linkFrom: -1,
           width: 0, height: 0, unresolved: 0,
           // The graph the current layout was computed FROM, so a graph that
           // changed without changing its version cannot keep a stale layout.
           _shapeN: -1, _shapeM: -1,
           _shapeIds: new Int32Array(nodeCap), _shapeTypes: new Int32Array(nodeCap),
           _shapeE: new Int32Array(edgeCap * 4),
           _shape: `${nodeCap}x${edgeCap}` };
}

/**
 * Has anything the LAYOUT is computed from moved?
 *
 * Deliberately not `patcherVersion`: a fixture installs its own version, and two
 * different graphs can carry the same number in one page's lifetime. The layout
 * depends on the node ids, their types (which decide box height and port seats)
 * and every edge's endpoints — so those are what is compared. Config values are
 * absent on purpose: nudging `hits` moves no box.
 */
function sameShape(buf, srcNodes, srcEdges, n, m) {
  if (buf._shapeN !== n || buf._shapeM !== m) return false;
  for (let i = 0; i < n; i++) {
    if (buf._shapeIds[i] !== srcNodes[i].id || buf._shapeTypes[i] !== srcNodes[i].type) return false;
  }
  for (let i = 0; i < m; i++) {
    const e = srcEdges[i];
    const b = i * 4;
    if (buf._shapeE[b] !== e.src || buf._shapeE[b + 1] !== e.dst
        || buf._shapeE[b + 2] !== e.srcPort || buf._shapeE[b + 3] !== e.dstPort) return false;
  }
  return true;
}

function recordShape(buf, srcNodes, srcEdges, n, m) {
  buf._shapeN = n;
  buf._shapeM = m;
  for (let i = 0; i < n; i++) {
    buf._shapeIds[i] = srcNodes[i].id;
    buf._shapeTypes[i] = srcNodes[i].type;
  }
  for (let i = 0; i < m; i++) {
    const e = srcEdges[i];
    const b = i * 4;
    buf._shapeE[b] = e.src; buf._shapeE[b + 1] = e.dst;
    buf._shapeE[b + 2] = e.srcPort; buf._shapeE[b + 3] = e.dstPort;
  }
}

/**
 * Where an edge's end attaches, as a y offset inside the node's box.
 *
 * By port id first, because that is what the engine published and it is exact.
 * The fallbacks exist because a port id is only meaningful against a port TABLE,
 * and this side's table is a mirror: a fixture, or a node type the engine
 * learned and this file has not, can name a port that resolves to nothing here.
 * Dropping the edge would make the graph look emptier than it is, so it lands on
 * the nearest port of the same KIND instead, and `exact` says which happened —
 * the count is on screen rather than swallowed.
 */
function anchorY(layout, portId, kind, out) {
  const byId = out ? layout.outY : layout.inY;
  if (portId >= 0 && portId < PORT_ID_COUNT && byId[portId] >= 0) return { y: byId[portId], exact: true };
  for (const p of layout.list) if (p.out === out && p.kind === kind) return { y: p.dy, exact: false };
  for (const p of layout.list) if (p.out === out) return { y: p.dy, exact: false };
  return { y: -1, exact: false };
}

/**
 * Columns by longest path, ordered inside a column by where their sources sit.
 *
 * The naive version stacked each column in publication order, which reads fine
 * for a chain and turns into a knot the moment two generators feed one sink —
 * cables crossed the boxes they were not attached to. One barycentre pass per
 * column, left to right, is enough to untangle the 4-8 node graphs this surface
 * actually shows, and it is deterministic: ties break on node id, so the same
 * graph always lays out the same way.
 *
 * Cycles are possible in a patcher, so the depth relaxation is bounded by the
 * node count rather than recursing — a feedback edge must not hang the UI.
 */
function relayout(buf, srcNodes, srcEdges, n, m) {
  const indexOf = new Map();
  for (let i = 0; i < n; i++) indexOf.set(srcNodes[i].id, i);

  const depth = new Int32Array(n);
  for (let pass = 0; pass < n; pass++) {
    let moved = false;
    for (let k = 0; k < m; k++) {
      const e = srcEdges[k];
      const s = indexOf.get(e.src);
      const d = indexOf.get(e.dst);
      if (s === undefined || d === undefined) continue;
      if (depth[d] < depth[s] + 1) { depth[d] = depth[s] + 1; moved = true; }
    }
    if (!moved) break;
  }

  let maxDepth = 0;
  for (let i = 0; i < n; i++) if (depth[i] > maxDepth) maxDepth = depth[i];
  const columns = [];
  for (let d = 0; d <= maxDepth; d++) columns.push([]);
  for (let i = 0; i < n; i++) columns[depth[i]].push(i);

  // Which row each node ended up in, so the next column can average over it.
  const row = new Int32Array(n).fill(-1);
  for (let d = 0; d <= maxDepth; d++) {
    const col = columns[d];
    const key = new Float64Array(col.length);
    for (let j = 0; j < col.length; j++) {
      let sum = 0;
      let count = 0;
      for (let k = 0; k < m; k++) {
        const e = srcEdges[k];
        if (indexOf.get(e.dst) !== col[j]) continue;
        const s = indexOf.get(e.src);
        if (s === undefined || row[s] < 0) continue;   // a feedback edge; no opinion
        sum += row[s];
        count++;
      }
      // No placed source — a root, or the far end of a cycle. Falls to the
      // bottom of its column in id order, which is stable and says nothing false.
      key[j] = count ? sum / count : 1e6 + srcNodes[col[j]].id;
    }
    // Sort a permutation of the SEATS, not the seats themselves: `key` is
    // indexed by position in `col`, and a comparator that looked its operands
    // up in the array being sorted would read keys that had already moved.
    const seats = [];
    for (let j = 0; j < col.length; j++) seats.push(j);
    seats.sort((a, b) => (key[a] - key[b]) || (srcNodes[col[a]].id - srcNodes[col[b]].id));
    const ordered = seats.map((j) => col[j]);
    columns[d] = ordered;
    for (let j = 0; j < ordered.length; j++) row[ordered[j]] = j;
  }

  // Column heights first, so short columns can be centred against the tallest
  // one. Top-aligning them hangs a single-node column off the ceiling with its
  // cable diving to the middle of the graph.
  let tallest = 0;
  const colH = new Float64Array(maxDepth + 1);
  for (let d = 0; d <= maxDepth; d++) {
    let h = 0;
    for (const i of columns[d]) h += heightOf(srcNodes[i].type) + GAP_Y;
    if (columns[d].length) h -= GAP_Y;
    colH[d] = h;
    if (h > tallest) tallest = h;
  }

  for (let d = 0; d <= maxDepth; d++) {
    const x = PAD_X + d * (NODE_W + GAP_X);
    let y = PAD_Y + (tallest - colH[d]) / 2;
    for (const i of columns[d]) {
      const node = buf.nodes[i];
      const h = heightOf(srcNodes[i].type);
      node.x = x;
      node.y = Math.round(y);
      node.w = NODE_W;
      node.h = h;
      y += h + GAP_Y;
    }
  }
  buf.width = PAD_X * 2 + (maxDepth + 1) * NODE_W + maxDepth * GAP_X;
  buf.height = PAD_Y * 2 + tallest;

  // Cables, in the same pass: they are a function of the layout and of nothing
  // else, so building them here is what keeps the draw path free of the string
  // concatenation that a per-frame rebuild would cost.
  buf.unresolved = 0;
  for (let k = 0; k < m; k++) {
    const e = srcEdges[k];
    const edge = buf.edges[k];
    edge.anchored = false;
    edge.exact = false;
    edge.path = '';
    const s = indexOf.get(e.src);
    const d = indexOf.get(e.dst);
    if (s === undefined || d === undefined || s >= n || d >= n) continue;
    const a = buf.nodes[s];
    const b = buf.nodes[d];
    const from = anchorY(portsForType(a.type), e.srcPort, e.kind, true);
    const to = anchorY(portsForType(b.type), e.dstPort, e.kind, false);
    if (from.y < 0 || to.y < 0) continue;
    if (!from.exact || !to.exact) buf.unresolved++;
    const x1 = a.x + a.w;
    const y1 = a.y + from.y;
    const x2 = b.x;
    const y2 = b.y + to.y;
    // The design's curvature, measured: horizontal control points at 48% of the
    // horizontal run, never closer than 56px. That is what makes a cable leave a
    // port sideways and arrive sideways instead of cutting the corner, and the
    // floor is what keeps a short hop from collapsing into a straight line.
    const c = Math.max(56, Math.abs(x2 - x1) * 0.48);
    edge.x1 = x1; edge.y1 = y1; edge.x2 = x2; edge.y2 = y2;
    edge.anchored = true;
    edge.exact = from.exact && to.exact;
    edge.path = `M${x1},${y1} C${x1 + c},${y1} ${x2 - c},${y2} ${x2},${y2}`;
  }
}

/** Rebuild a node's config rows, but only when what they are made of moved. */
function bindConfig(node, type, cfg, has) {
  let same = node._cfgType === type && node._cfgHas === has;
  if (same) {
    for (let i = 0; i < MAX_FIELDS; i++) {
      if (node._cfg[i] !== (has ? cfg[i] : 0)) { same = false; break; }
    }
  }
  if (same) return;
  node._cfgType = type;
  node._cfgHas = has;
  for (let i = 0; i < MAX_FIELDS; i++) node._cfg[i] = has ? cfg[i] : 0;

  const table = CONFIG_FIELDS[NODE_TYPES[type]] || EMPTY;
  const fields = has ? table : EMPTY;
  node.rowCount = Math.min(fields.length, MAX_FIELDS);
  // A type that HAS settings whose values the engine has not published is a
  // different thing from a type that has none, and an empty box says neither.
  // The box is sized by type either way, so this fills a hole that would
  // otherwise read as a rendering failure.
  node.unpublished = !has && table.length > 0;
  for (let i = 0; i < node.rowCount; i++) {
    const f = fields[i];
    const r = node.rows[i];
    r.name = Array.isArray(f) ? f[0] : f;
    r.value = fieldValue(f, cfg[i]);
    r.frac = fieldFraction(type, i, cfg[i]);
  }
  node.config = has ? describeConfig(type, cfg) : '';
}

/**
 * @param {{engine:object|null, selectedNode:number, selectedField:number,
 *          pending:Map<number,number[]>|null}} opts
 *
 * `pending` is the caller's map of edits sent but not yet confirmed, keyed on
 * node id. It wins over the engine's value so the box shows what you just typed
 * — the same optimism the faders have.
 */
/**
 * The nodes belonging to ONE device, out of the pooled graph.
 *
 * The engine publishes every device's patcher nodes in one array — a POOL, not a
 * graph — with each device's nodes in a disjoint id block and each device's own
 * output node named by its `patcher_node_id`. Rendering the pool unfiltered is
 * what made the patcher view show track 1's euclidean while you stood on track
 * 2, edit it when you thought you were editing track 2's, and report a generator
 * on a track that has no devices at all. Three separate bug reports, one cause.
 *
 * A device's graph is everything that FEEDS its output, so this walks backwards
 * from the root over `dst -> src`. Backwards rather than forwards because a
 * generator's output node is the only node the engine names: the nodes are known
 * by what they reach, not by what reaches them.
 *
 * WRITES A BITMASK, not a Set, and that is a performance decision with a
 * measurement behind it. `Set.prototype.clear()` allocates a fresh backing table
 * in V8, and this runs whenever the selected device changes — which on the rack
 * is every frame you hold an arrow key. Measured at 844 B/draw on the chain
 * scene alone. A `Uint8Array` filled with `fill(0)` allocates nothing after the
 * first sizing.
 *
 * A root that names no node yields an EMPTY mask, not a full one — "I do not
 * know which nodes are yours" must not render as "all of them", which is the
 * failure being fixed.
 *
 * Returns the mask, grown if the pool's ids outran it.
 */
export function subgraphFrom(nodes, edges, rootNodeId, mask) {
  let m = mask;
  let top = 0;
  for (let i = 0; i < nodes.length; i++) if (nodes[i].id > top) top = nodes[i].id;
  if (!m || m.length <= top) m = new Uint8Array(top + 8);
  m.fill(0);
  if (rootNodeId === undefined || rootNodeId === null || rootNodeId < 0
      || rootNodeId >= m.length) return m;
  let exists = false;
  for (let i = 0; i < nodes.length; i++) if (nodes[i].id === rootNodeId) { exists = true; break; }
  if (!exists) return m;
  m[rootNodeId] = 1;
  // Iterate to a fixed point rather than recursing: the pool is small, a graph
  // may name its inputs in any order, and a cycle — which the engine refuses but
  // this side must not hang on — terminates naturally when nothing new is added.
  for (let pass = 0; pass < nodes.length + 1; pass++) {
    let added = false;
    for (let i = 0; i < edges.length; i++) {
      const e = edges[i];
      if (e.dst < m.length && e.src < m.length && m[e.dst] && !m[e.src]) {
        m[e.src] = 1; added = true;
      }
    }
    if (!added) break;
  }
  return m;
}

/**
 * Which node types EMIT events a graph was not given — the generators.
 *
 * A set rather than a list because the question asked of it is always membership,
 * and it is asked once per node per rebuild. `euclidean` fires on a grid and
 * `random` picks a degree; both make notes nobody wrote, which is the whole reason
 * anyone needs to be told they are there.
 */
export const GENERATOR_TYPES = new Set(['euclidean', 'random']);

/**
 * The generator node types reachable from `rootNodeId`, as a phrase.
 *
 * SCOPED TO ONE DEVICE, which is the entire point. The published patcher region is
 * a POOL holding every graph in the song, so naming its generators names the whole
 * project's — and both the rack card and the chrome did exactly that: a device with
 * a plain passthrough graph on track 3 read "generates: euclidean + random" because
 * track 0 had a euclidean somewhere in the pool. The per-device bit on the chain
 * snapshot says WHETHER a device generates; this says WHAT, from that device's own
 * subgraph and nothing else.
 *
 * The walk is UPSTREAM from the device's root — `subgraphFrom` follows edges
 * backwards from the output — because that is what feeds this device and therefore
 * what it can be blamed for.
 *
 * Returns '' when there is nothing to say, so a caller can test it as a boolean.
 * `mask` is reused across calls to keep this off the allocation path.
 */
export function generatorsFrom(nodes, edges, rootNodeId, mask) {
  if (!nodes || !nodes.length || rootNodeId === undefined || rootNodeId < 0) return '';
  const m = subgraphFrom(nodes, edges, rootNodeId, mask);
  let found = '';
  for (let i = 0; i < nodes.length; i++) {
    const n = nodes[i];
    if (!m[n.id]) continue;
    const name = NODE_TYPES[n.type] || '';
    if (!GENERATOR_TYPES.has(name)) continue;
    // Each type once: a graph with three euclideans is still "euclidean", and
    // listing it three times says nothing a reader can act on.
    if (found.indexOf(name) >= 0) continue;
    found = found ? found + ' + ' + name : name;
  }
  return found;
}

export function buildPatcherModel(opts, buf) {
  const { engine = null, selectedNode = -1, selectedField = 0, pending = null,
          addType = -1, linkFrom = null,
          // Which DEVICE's graph to show, named by its output node. See
          // `subgraphFrom`: the engine publishes one pool for every device on
          // every track, and showing it whole is how a patcher on track 1 came
          // to be edited by someone standing on track 2.
          //
          // -1 means "no device selected", which shows NOTHING rather than
          // everything. A view that falls back to the pool when it does not know
          // whose nodes these are is the bug, not a convenience.
          rootNode = -1,
          // Every patcher device's root on this track, so a node belonging to
          // NONE of them can be told from one belonging to another device. See
          // the orphan rule below.
          allRoots = EMPTY_ROOTS } = opts;
  const poolNodes = engine ? engine.patcherNodes : EMPTY;
  const poolEdges = engine ? engine.patcherEdges : EMPTY;
  /*
   * WHICH NODES ARE ON SCREEN.
   *
   * Two masks: `scope` is the selected device's subgraph, `owned` is every
   * device's. A node is drawn when it is in `scope`, or in no device's at all.
   *
   * ORPHANS ARE SHOWN, which is the second half and not an afterthought. A node
   * just added with `addnode` is wired to nothing, so it belongs to no device's
   * subgraph and pure scoping made it vanish the moment it was created — which
   * makes building a graph impossible, since every graph starts as unconnected
   * nodes. `owned` is what tells an orphan (nobody's yet, and therefore yours)
   * from a node belonging to ANOTHER device (hidden, which is the whole point).
   */
  buf._scope = subgraphFrom(poolNodes, poolEdges, rootNode, buf._scope);
  const scope = buf._scope;
  let owned = buf._owned;
  if (!owned || owned.length < scope.length) owned = buf._owned = new Uint8Array(scope.length);
  owned.fill(0);
  for (let i = 0; i < allRoots.length; i++) {
    buf._tmp = subgraphFrom(poolNodes, poolEdges, allRoots[i], buf._tmp);
    const t = buf._tmp;
    const n = t.length < owned.length ? t.length : owned.length;
    for (let j = 0; j < n; j++) if (t[j]) owned[j] = 1;
  }
  buf.scoped = rootNode >= 0;
  /*
   * FILLED BY INDEX, and the count kept beside the array rather than in its
   * `length`.
   *
   * `srcNodes.length = 0` followed by `push` is the obvious way to reuse an
   * array and it is not free: V8 shrinks the backing store on the truncation and
   * grows it again on the pushes, which measured ~450 B/draw here — on the
   * patcher AT REST, where nothing has changed and the whole path should be
   * doing nothing. Assigning by index never resizes.
   */
  const srcNodes = buf._fn || (buf._fn = []);
  const srcEdges = buf._fe || (buf._fe = []);
  let srcNodeCount = 0;
  for (let i = 0; i < poolNodes.length; i++) {
    const id = poolNodes[i].id;
    if (id < scope.length && (scope[id] || !owned[id])) srcNodes[srcNodeCount++] = poolNodes[i];
  }
  let srcEdgeCount = 0;
  for (let i = 0; i < poolEdges.length; i++) {
    // BOTH ends visible. An edge with one end outside is not one this view can
    // draw, and drawing it would put a wire into empty space.
    const e = poolEdges[i];
    if (e.src < scope.length && e.dst < scope.length
        && (scope[e.src] || !owned[e.src]) && (scope[e.dst] || !owned[e.dst])) {
      srcEdges[srcEdgeCount++] = e;
    }
  }
  // How many nodes the pool holds that are NOT this device's. The view needs it
  // to say "select a device" rather than "this song has no patcher", which are
  // very different things to be told.
  buf.poolCount = poolNodes.length;

  buf.version = engine ? engine.patcherVersion : -1;
  buf.empty = srcNodeCount === 0;
  buf.addType = NODE_TYPES[addType] || '';
  // Which node an in-progress connection started from, so the next keystroke's
  // meaning is on screen rather than in the user's head.
  buf.linkFrom = linkFrom === null ? -1 : linkFrom;

  // The COUNTS, not the arrays' lengths: the arrays are reused and hold stale
  // entries past the count by design (see above).
  const n = Math.min(srcNodeCount, buf.nodes.length);
  const m = Math.min(srcEdgeCount, buf.edges.length);

  for (let i = 0; i < n; i++) {
    const node = buf.nodes[i];
    const src = srcNodes[i];
    node.id = src.id;
    node.type = src.type;
    // The name and the port list are a function of the TYPE alone, so they are
    // rebound only when a pooled box starts holding a node of a different type
    // — which is the one case where they can move. Unguarded, a type this build
    // does not know built `'type' + n` fresh for its box on every frame.
    if (node._typeFor !== src.type) {
      node._typeFor = src.type;
      node.typeName = NODE_TYPES[src.type] || ('type' + src.type);
      node.ports = portsForType(src.type).list;
    }
    const sent = pending ? pending.get(src.id) : undefined;
    bindConfig(node, src.type, sent || src.config, !!(src.hasConfig || sent));
    node.selected = src.id === selectedNode;
    // Which field the keyboard is on, so the box can show it. -1 when this node
    // is not selected: an edit cursor on an unselected node would be a lie about
    // what the next keystroke does.
    node.field = node.selected ? selectedField : -1;
    // Read out of the precomputed name table rather than out of a freshly built
    // configFields() array, which allocated the array, one object per field and
    // an empty object for the out-of-range case — every frame, for the selected
    // node — to end up with a string that only changes when you press Tab.
    const names = FIELD_NAMES[src.type];
    node.fieldName = node.selected && names ? (names[selectedField] || '') : '';
    for (let k = 0; k < node.rowCount; k++) node.rows[k].selected = node.field === k;
  }
  buf.nodeCount = n;
  buf.edgeCount = m;

  // Positions and cables are a function of the GRAPH, not of the frame. Laying
  // out on every draw was affordable and still wrong in principle: it rebuilt a
  // path string per edge per frame, which is the allocation shape GUIDELINES 3
  // is about.
  if (!sameShape(buf, srcNodes, srcEdges, n, m)) {
    relayout(buf, srcNodes, srcEdges, n, m);
    recordShape(buf, srcNodes, srcEdges, n, m);
  }

  for (let i = 0; i < m; i++) {
    const e = srcEdges[i];
    const edge = buf.edges[i];
    edge.src = e.src; edge.dst = e.dst; edge.kind = e.kind;
    // Guarded on the kind alone because that is the only thing the name is made
    // of. It has to stay outside the shape guard — an edge can change kind
    // without moving either end — but a kind the engine learned since this build
    // otherwise concatenated `'kind' + n` per cable per frame.
    if (edge._kindFor !== e.kind) {
      edge._kindFor = e.kind;
      edge.kindName = EDGE_KINDS[e.kind] || ('kind' + e.kind);
    }
  }
  return buf;
}
