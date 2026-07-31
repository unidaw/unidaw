// A batch of edits somebody has PROPOSED, held before any of them are sent.
//
// The design's right-hand dock shows an agent proposing a diff and the user
// applying or discarding the whole batch at once. This is the model behind that
// card, and it is deliberately less than the design implies:
//
//   * NOTHING IN THE ENGINE PROPOSES EDITS. There is no "here is a diff" message
//     on the wire and this file does not invent one. Every proposal comes from
//     whoever drives the console — today that is an agent typing the same command
//     grammar a person types.
//   * APPLY DOES NOT SEND. It hands the ops back to the caller, which is the side
//     that owns the socket. `conn.sendBatch` is what an Apply ends up in: one
//     frame, re-based op by op by the sidecar, because the engine arbitrates by
//     base_version and N separate sends would have the first accepted and the
//     rest rejected.
//   * NOTHING IS PREVIEWED YET. index.html does have `pending` cells, but those
//     are the optimistic echo of an edit ALREADY SENT, waiting for the engine's
//     clipVersion to catch up — the opposite end of this one. A proposal has not
//     been sent, so no ghost cell stands for it. `previewed` says so out loud and
//     stays false until something actually draws one; the design's "ghost cells
//     previewing in the tracker" is a sentence this model is not yet allowed to
//     say (GUIDELINES 4.5 — a surface must say what it does not know).
//
// The state IS the view-model. It changes only on a transition, so every string
// the card draws is built once, here, instead of being derived per draw — which
// is what makes the renderer allocation-free without it having to try.

export const IDLE = 'idle';
export const PENDING = 'pending';
export const APPLIED = 'applied';
export const DISCARDED = 'discarded';

/** The card's name, fixed. Written once at construction by the renderer. */
export const TITLE = 'pending diff';

const NOTHING = 'nothing pending — a proposal comes from whoever drives the '
              + 'console; the engine does not offer them';
/** The honest version of the design's ghost-cell line, until ghosts exist. */
const HELD = 'held here only — nothing is drawn in the tracker and nothing is '
           + 'sent until you apply';
/** The design's line, and it becomes true the moment something draws a ghost. */
const GHOSTS = 'ghost cells previewing in the tracker — nothing committed';

/**
 * One op as a count and a noun.
 *
 * `note` and `delete` are the two the console sends and the two worth reading in
 * English. Anything else is named rather than guessed at: a plural rule invented
 * for a type this file has never seen produces a confident wrong word, and the
 * type name is already the truest thing available.
 */
function word(type, n) {
  if (type === 'note') return n + (n === 1 ? ' note' : ' notes');
  if (type === 'delete') return n + (n === 1 ? ' delete' : ' deletes');
  return n + ' ' + type;
}

/**
 * What a batch of ops amounts to, in one line: "3 notes · track 2".
 *
 * Derived here and nowhere else. The proposer's own words go in `label`, so the
 * two cannot drift into disagreeing about the same batch — a caller-supplied
 * count that no longer matches the ops is exactly the failure GUIDELINES 2.1 is
 * about, one step earlier than usual.
 */
export function summarise(ops) {
  if (!ops || !ops.length) return 'no ops';
  const kinds = [];         // first-seen order: the batch reads as it was built
  const counts = [];
  const tracks = [];
  for (const op of ops) {
    const type = op && op.type ? String(op.type) : 'op';
    const at = kinds.indexOf(type);
    if (at < 0) { kinds.push(type); counts.push(1); } else counts[at]++;
    if (typeof op.track === 'number' && tracks.indexOf(op.track) < 0) tracks.push(op.track);
  }
  let out = '';
  for (let i = 0; i < kinds.length; i++) out += (i ? ' · ' : '') + word(kinds[i], counts[i]);
  if (!tracks.length) return out;
  tracks.sort((a, b) => a - b);
  // Named while they still fit; counted once naming them would be a wall of
  // numbers nobody reads.
  if (tracks.length === 1) return out + ' · track ' + tracks[0];
  if (tracks.length <= 3) return out + ' · tracks ' + tracks.join(', ');
  return out + ' · ' + tracks.length + ' tracks';
}

/** An empty holder in the IDLE state. One per dock; reused across proposals. */
export function createPending() {
  return {
    status: IDLE,
    /** The proposer's own words for the batch, e.g. "+8 cents edits". */
    label: '',
    /** Derived from the ops, never supplied. */
    summary: '',
    /** What the head line draws: label and summary, joined when both exist. */
    meta: '',
    /** The line under it. Whoever owns the current status wrote it. */
    reason: NOTHING,
    /** The proposed ops, in order, as the same shapes the console sends. */
    ops: [],
    opCount: 0,
    /** The clipVersion the ops were composed against; -1 when unknown. */
    version: -1,
    applyLabel: 'Apply',
    /** Whether anything on screen shows these ops. See the header. */
    previewed: false,
    /** Proposal counter — an identity for the batch, so a log can name it. */
    seq: 0,
    /** Whether the buttons mean anything right now. */
    actionable: false,
  };
}

/**
 * Hold a batch. Supersedes whatever was pending — the newer proposal is the one
 * the user is being asked about, and two live cards would be a choice nobody
 * offered to make.
 *
 * @param {object} state         from createPending()
 * @param {{label?:string, reason?:string, ops:Array<object>, version?:number,
 *          previewed?:boolean}} p
 */
export function propose(state, p) {
  const ops = p && p.ops;
  // Refused out loud, not absorbed: a card offering to apply nothing is a
  // control that silently does nothing, and the console's catch turns this into
  // a readable error line.
  if (!Array.isArray(ops) || !ops.length) throw new Error('a proposal needs at least one op');
  for (let i = 0; i < ops.length; i++) {
    const op = ops[i];
    if (!op || typeof op !== 'object' || !op.type) {
      throw new Error('op ' + i + ' has no type — it would go on the wire as junk');
    }
  }

  // A caller can legitimately re-propose the array applyPending handed back —
  // it is the model's OWN array, so clearing it here would empty the very thing
  // the next line reads, producing the empty proposal the guard above exists to
  // refuse. Copy first when they are the same object.
  const incoming = ops === state.ops ? ops.slice() : ops;
  state.ops.length = 0;                       // drops the previous batch's refs
  for (const op of incoming) state.ops.push(op);
  state.opCount = state.ops.length;
  state.status = PENDING;
  state.seq++;
  state.label = p.label ? String(p.label) : '';
  state.summary = summarise(state.ops);
  state.meta = state.label ? state.label + ' · ' + state.summary : state.summary;
  state.previewed = !!p.previewed;
  state.reason = p.reason ? String(p.reason) : (state.previewed ? GHOSTS : HELD);
  state.version = typeof p.version === 'number' && p.version >= 0 ? p.version : -1;
  // "composed against v2141", not "will land as v2141". The sidecar re-bases
  // every op in a batch onto the version the previous one produced, so the
  // number on the button names where the proposal came FROM — which is the part
  // that decides whether it still makes sense.
  state.applyLabel = state.version >= 0 ? 'Apply · v' + state.version : 'Apply';
  state.actionable = true;
  return state;
}

/**
 * Hand the batch to the caller, which owns the socket.
 *
 * Returns the model's OWN ops array — not a copy, so nothing is allocated on the
 * click. It stays valid until the next propose() reuses it, and `conn.sendBatch`
 * serialises synchronously, so the ordinary caller is safe. Anything that wants
 * to keep the ops past the next proposal has to copy them itself.
 */
export function applyPending(state) {
  if (state.status !== PENDING) throw new Error('nothing pending to apply');
  return state.ops;
}

/**
 * Record what happened to a batch the caller just tried to send.
 *
 * SEPARATE FROM applyPending on purpose. It used to flip to APPLIED and write
 * "handed N ops to the caller" before the caller had tried the socket — so with
 * no engine the send failed, the batch was already gone, and the card stood as a
 * permanent record of a hand-off that reached nothing. Every other write path in
 * this app checks sendBatch's return BEFORE committing local state, and an API
 * that makes that order impossible is the wrong API.
 */
export function settlePending(state, sent) {
  if (state.status !== PENDING) return state;
  if (!sent) {
    // Still pending, so it can be tried again. The card says why rather than
    // pretending the click did nothing.
    state.reason = 'nothing was sent — no engine. The batch is still here.';
    return state;
  }
  state.status = APPLIED;
  state.actionable = false;
  state.reason = 'sent ' + state.opCount + ' op' + (state.opCount === 1 ? '' : 's')
               + ' — whether the engine took them shows up as a clipVersion';
  return state;
}

/**
 * Whether the clip has moved since the proposal was composed.
 *
 * The button says "Apply · v2141" and that number named where the proposal came
 * FROM. It was recorded, drawn, and never compared — so it stood still while the
 * clip moved underneath it, which is the label lying by omission. A caller that
 * passes the current version gets told.
 */
export function isStale(state, currentVersion) {
  return state.status === PENDING && state.version >= 0
      && typeof currentVersion === 'number' && currentVersion > state.version;
}

/** Drop the batch. Nothing was ever sent, so there is nothing to undo. */
export function discardPending(state) {
  if (state.status !== PENDING) throw new Error('nothing pending to discard');
  state.ops.length = 0;
  state.opCount = 0;
  state.status = DISCARDED;
  state.actionable = false;
  state.reason = 'discarded — nothing was sent';
  return state;
}

/** Back to empty, after the caller has shown the outcome. */
export function resetPending(state) {
  state.ops.length = 0;
  state.opCount = 0;
  state.status = IDLE;
  state.label = ''; state.summary = ''; state.meta = '';
  state.reason = NOTHING;
  state.version = -1;
  state.applyLabel = 'Apply';
  state.previewed = false;
  state.actionable = false;
  return state;
}
