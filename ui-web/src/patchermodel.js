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

/** PatcherNodeType, from daw_bridge::layout. */
export const NODE_TYPES = [
  'kernel', 'euclidean', 'passthru', 'audio', 'lfo', 'random', 'out',
];

/** PatcherPortKind. */
export const EDGE_KINDS = ['event', 'audio', 'control'];

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

/** The editable fields of a node type, as {name, index}. Empty if none. */
export function configFields(type) {
  const fields = CONFIG_FIELDS[NODE_TYPES[type]];
  if (!fields) return [];
  return fields.map((f, i) => ({ name: Array.isArray(f) ? f[0] : f, index: i }));
}

/** Nudge one field, clamped. Returns the new eight-value config. */
export function nudgeConfig(type, config, fieldIndex, dir) {
  const limits = CONFIG_LIMITS[NODE_TYPES[type]];
  if (!limits || !limits[fieldIndex]) return null;
  const [step, lo, hi] = limits[fieldIndex];
  const out = Array.from(config);
  out[fieldIndex] = Math.max(lo, Math.min(hi, out[fieldIndex] + dir * step));
  return out;
}

export function describeConfig(type, config) {
  const fields = CONFIG_FIELDS[NODE_TYPES[type]];
  if (!fields) return '';
  const parts = [];
  for (let i = 0; i < fields.length; i++) {
    const f = fields[i];
    if (Array.isArray(f)) {
      const [name, scale, unit, zeroLabel] = f;
      parts.push(zeroLabel && config[i] === 0
        ? name + ' ' + zeroLabel
        : name + ' ' + (config[i] / scale).toFixed(2) + unit);
    } else {
      parts.push(f + ' ' + config[i]);
    }
  }
  return parts.join('  ');
}

// Wide enough for the longest config line a node type produces, and tall enough
// for all of it. Euclidean's fields wrapped to three lines and spilled out of the
// box at the first size I picked; they are eight now, so this has room for four
// lines plus the field cursor along the bottom.
const NODE_W = 220;
const NODE_H = 98;
const GAP_X = 76;
const GAP_Y = 24;

export function createPatcherBuffer(nodeCap = 64, edgeCap = 128) {
  const nodes = new Array(nodeCap);
  for (let i = 0; i < nodeCap; i++) {
    nodes[i] = { id: 0, type: 0, typeName: '', config: '', x: 0, y: 0, w: NODE_W, h: NODE_H,
                 selected: false, field: -1, fieldName: '' };
  }
  const edges = new Array(edgeCap);
  for (let i = 0; i < edgeCap; i++) {
    edges[i] = { src: 0, dst: 0, kind: 0, kindName: '', path: '' };
  }
  return { nodes, nodeCount: 0, edges, edgeCount: 0,
           version: -1, device: 0, empty: true, addType: '', linkFrom: -1,
           _shape: `${nodeCap}x${edgeCap}` };
}

/**
 * Depth of each node: 0 for anything with no incoming edge, otherwise one past
 * its deepest source. Cycles are possible in a patcher, so this is bounded by
 * the node count rather than recursing — a feedback edge should not hang the UI.
 */
function computeDepths(nodes, edges, depth) {
  for (let i = 0; i < nodes.length; i++) depth[i] = 0;
  const indexOf = new Map();
  for (let i = 0; i < nodes.length; i++) indexOf.set(nodes[i].id, i);
  for (let pass = 0; pass < nodes.length; pass++) {
    let moved = false;
    for (const e of edges) {
      const s = indexOf.get(e.src), d = indexOf.get(e.dst);
      if (s === undefined || d === undefined) continue;
      if (depth[d] < depth[s] + 1) { depth[d] = depth[s] + 1; moved = true; }
    }
    if (!moved) break;
  }
  return indexOf;
}

/**
 * @param {{engine:object|null, selectedNode:number, selectedField:number,
 *          pending:Map<number,number[]>|null}} opts
 *
 * `pending` is the caller's map of edits sent but not yet confirmed, keyed on
 * node id. It wins over the engine's value so the box shows what you just typed
 * — the same optimism the faders have.
 */
export function buildPatcherModel(opts, buf) {
  const { engine = null, selectedNode = -1, selectedField = 0, pending = null,
          addType = -1, linkFrom = null } = opts;
  const srcNodes = engine ? engine.patcherNodes : [];
  const srcEdges = engine ? engine.patcherEdges : [];

  buf.version = engine ? engine.patcherVersion : -1;
  buf.device = engine ? engine.patcherDevice : 0;
  buf.empty = srcNodes.length === 0;
  buf.addType = NODE_TYPES[addType] || '';
  // Which node an in-progress connection started from, so the next keystroke's
  // meaning is on screen rather than in the user's head.
  buf.linkFrom = linkFrom === null ? -1 : linkFrom;

  const depth = new Array(srcNodes.length);
  const indexOf = computeDepths(srcNodes, srcEdges, depth);

  // Columns by depth, stacked vertically within a column.
  const perColumn = [];
  const n = Math.min(srcNodes.length, buf.nodes.length);
  for (let i = 0; i < n; i++) {
    const d = depth[i];
    perColumn[d] = (perColumn[d] || 0);
    const row = perColumn[d]++;
    const node = buf.nodes[i];
    const src = srcNodes[i];
    node.id = src.id;
    node.type = src.type;
    node.typeName = NODE_TYPES[src.type] || ('type' + src.type);
    const sent = pending ? pending.get(src.id) : undefined;
    const cfg = sent || src.config;
    node.config = (src.hasConfig || sent) ? describeConfig(src.type, cfg) : '';
    node.x = 24 + d * (NODE_W + GAP_X);
    node.y = 24 + row * (NODE_H + GAP_Y);
    node.w = NODE_W;
    node.h = NODE_H;
    node.selected = src.id === selectedNode;
    // Which field the keyboard is on, so the box can show it. -1 when this node
    // is not selected: an edit cursor on an unselected node would be a lie about
    // what the next keystroke does.
    node.field = node.selected ? selectedField : -1;
    node.fieldName = node.selected
      ? (configFields(src.type)[selectedField] || {}).name || ''
      : '';
  }
  buf.nodeCount = n;

  const m = Math.min(srcEdges.length, buf.edges.length);
  for (let i = 0; i < m; i++) {
    const e = srcEdges[i];
    const edge = buf.edges[i];
    edge.src = e.src; edge.dst = e.dst; edge.kind = e.kind;
    edge.kindName = EDGE_KINDS[e.kind] || 'kind' + e.kind;
    const si = indexOf.get(e.src), di = indexOf.get(e.dst);
    if (si === undefined || di === undefined || si >= n || di >= n) { edge.path = ''; continue; }
    const a = buf.nodes[si], b = buf.nodes[di];
    const x1 = a.x + a.w, y1 = a.y + a.h / 2;
    const x2 = b.x, y2 = b.y + b.h / 2;
    // A cubic with horizontal control points, so an edge leaves a node's right
    // edge horizontally and arrives at the next one's left edge horizontally —
    // which is what makes a graph readable at a glance rather than a web.
    const c = Math.max(30, (x2 - x1) / 2);
    edge.path = `M${x1},${y1} C${x1 + c},${y1} ${x2 - c},${y2} ${x2},${y2}`;
  }
  buf.edgeCount = m;
  return buf;
}
