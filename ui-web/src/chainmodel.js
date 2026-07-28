// The device chain view-model: one card per device on the cursor's track.
//
// The engine's chain is published as ChainSnapshot diffs on its outbound ring —
// one entry per device, all sharing a chainVersion — which the sidecar drains
// and accumulates (it is a single-consumer ring, so one drainer and a shared
// buffer). By the time it reaches here it is already per-track and already
// deduplicated by version.
//
// CONTENTS arrive separately, and later. The chain snapshot carries a device's
// IDENTITY only (id, kind, position, patcher node, host slot, capabilities,
// bypass). Its NAME and PARAMETERS live in the plugin host, so the engine has to
// go and ask — one device per query (SHM v17, RequestDeviceParams) — and the
// answer lands in a region the sidecar forwards. So a card is drawn twice: once
// as identity, then again with contents when the answer comes back. Until it
// does the card says what it knows and no more; the alternative, a slider with a
// plausible number on it, is the failure mode GUIDELINES 2.1 is about.
//
// EVERY STRING HERE IS MEMOISED on the inputs it is built from. A rack of eight
// Zebra2s is 2,048 parameters, and this function runs on every draw: building
// one title, one capability phrase and one value string per row per frame is
// ~11 KB/draw of garbage, which is precisely the number GUIDELINES 3.0 exists to
// keep at zero. The rule for anything added below: if it concatenates, it caches
// the values it concatenated from, and it is rebuilt only when one of them moves.

/** DeviceKind, from apps/device_chain.h. */
export const DEVICE_KINDS = [
  'patcher event', 'patcher instrument', 'patcher audio', 'VST instrument', 'VST effect',
];

/** The short badge each kind gets, matching the design's PATCHER / VST3 / UNI. */
const KIND_BADGE = ['PATCHER', 'PATCHER', 'PATCHER', 'VST3', 'VST3'];

/** DeviceCapability bits, same header. */
const CAPS = [
  [1, 'midi in'],
  [2, 'midi out'],
  [4, 'audio'],
];

/**
 * kUiMaxDeviceParams, from apps/shared_memory.h.
 *
 * The engine's region holds 256 and Zebra2 fills it, so this is the number the
 * rack has to be able to SHOW rather than a limit it chose. It is still a cap —
 * a plugin with more parameters than the region carries is truncated by the
 * engine before it ever reaches here — which is why `more` below counts what was
 * dropped instead of quietly listing a prefix.
 */
export const MAX_PARAMS = 256;

/**
 * Sentinels from device_chain.h. `kDeviceIdAuto` doubles as the engine's
 * "this chain is empty" marker in a snapshot, and `kHostSlotIndexDirect` means
 * the device is not hosted out-of-process at all.
 */
export const DEVICE_ID_AUTO = 0xffffffff;
const HOST_SLOT_DIRECT = 0xfffffffe;

/**
 * The stand-in for a chain entry that carries no `devices` at all.
 *
 * `entry.devices || []` looked free and was one array per draw for as long as
 * the sidecar had told us about a track without listing anything on it. Shared
 * because nothing ever writes to it — every reader below only measures it.
 */
const NO_DEVICES = [];

/**
 * Which capabilities a mask names, as a short phrase.
 *
 * Memoised by mask: there are three bits, so there are eight possible answers
 * for the lifetime of the process, and building one of them per card per frame
 * was an array plus a join per card per draw.
 */
const CAP_TEXT = [];
function describeCaps(mask) {
  const m = mask & 7;
  let s = CAP_TEXT[m];
  if (s !== undefined) return s;
  const on = [];
  for (const [bit, name] of CAPS) if (m & bit) on.push(name);
  s = on.join(' · ');
  CAP_TEXT[m] = s;
  return s;
}

/** One pooled parameter row's worth of view-model. Mutated, never replaced. */
function createParamSlot() {
  return {
    index: 0,       // the engine's own ordering index; NOT a durable identity
    uid: '',        // hashStableId16 hex — the durable one, see shared_memory.h
    name: '',
    display: '',
    frac: 0,        // 0..1, already normalised by the bridge
    milli: 0,       // the same value in the engine's own unit, for comparison
    _dv: NaN,       // what `display` was derived from, when we had to derive it
  };
}

/**
 * The bus summary, interned on its own content.
 *
 * The domain is tiny — a device has at most 32 buses — and the string is a pure
 * function of two small counts, so the key IS the content and there is nothing to
 * invalidate. Same reasoning as the lane labels in viewmodel.js: a table keyed on a
 * POSITION needs a guard and a clear; a table keyed on what it spells does not.
 */
const BUS_TEXT = new Map();
function busSummary(ins, outs) {
  const key = ins * 64 + outs;
  let t = BUS_TEXT.get(key);
  if (t === undefined) {
    t = outs && ins ? outs + ' out \u00b7 ' + ins + ' in'
      : outs ? outs + ' out'
      : ins ? ins + ' in'
      : '';
    BUS_TEXT.set(key, t);
  }
  return t;
}

/** "3/8" while a device's buses are still arriving. Same interning argument. */
const BUS_PARTIAL = new Map();
function busPartialText(have, want) {
  const key = have * 64 + want;
  let t = BUS_PARTIAL.get(key);
  if (t === undefined) { t = 'buses ' + have + '/' + want; BUS_PARTIAL.set(key, t); }
  return t;
}

export function createChainBuffer(cap = 16) {
  const cards = new Array(cap);
  for (let i = 0; i < cap; i++) {
    cards[i] = { id: 0, kind: 0, badge: '', title: '', pos: 0,
                 sub: '', caps: '', bypass: false, selected: false, patcherNode: -1,
                 named: false, params: [], paramCount: 0, more: '',
                 // The device's audio buses (kShmVersion 20): what it can actually
                 // route, which is the difference between "a plugin" and "a plugin
                 // with eight stems and a sidechain input". Empty until the engine
                 // publishes a chain that carries them.
                 busText: '', busTruncated: false, busPartial: false,
                 _bKey: -1,
                 // Memo keys for the three strings this card builds. Named for
                 // what they cache so a fourth input added to one of them is
                 // obviously missing from its key.
                 _tName: null, _tKind: -1, _tId: -1,
                 _sSlot: -1, _sNode: -2, _sCount: -1, _sMore: -1,
                 _mCount: -1,
                 // And the key for the parameter slots themselves: which answer
                 // filled them, for which device, and how much of it was taken.
                 _pSrc: undefined, _pId: -1, _pCount: -1 };
  }
  return { cards, cardCount: 0, track: 0, trackName: '', version: -1,
           known: false, notice: '', _cap: cap,
           _nNamed: -1, _nTotal: -1 };
}

/**
 * @param {{engine:object|null, track:number, chains:object|null,
 *          selected:number, trackName:string}} opts
 *
 * `chains` is the sidecar's accumulated per-track state, keyed by track id.
 * A track absent from it is a track the engine has never published a chain for
 * — which today is every track until something changes or somebody asks. That
 * is a different thing from a track with no devices, and the two must not look
 * the same.
 */
export function buildChainModel(opts, buf) {
  const { track = 0, chains = null, selected = -1, trackName = '', params = null } = opts;
  const entry = chains ? chains[track] : null;

  buf.track = track;
  buf.trackName = trackName;
  buf.version = entry ? entry.version : -1;
  buf.known = !!entry;
  buf.cardCount = 0;

  if (!entry) {
    buf.notice = 'the engine publishes a chain only when it changes — '
               + 'press r to ask for this track’s';
    // And forget what the "asking the hosts" line was last built from. Without
    // this, moving the cursor to a track the engine has never published a chain
    // for and back again left the press-r line on screen: the counts were still
    // 1-of-3, so the memo below decided the notice already said so, while the
    // notice had been overwritten in between. Content moved, key stood still —
    // GUIDELINES 2.1 exactly.
    buf._nNamed = -1; buf._nTotal = -1;
    return buf;
  }

  const devices = entry.devices || NO_DEVICES;
  // How much of the rack has actually answered. A blanket "identity only" was
  // right when nothing could answer and is wrong now that some cards have.
  //
  // An index loop, not `for...of`: this runs on every draw, and a for-of over
  // the device list is an iterator object per frame for a count of at most 16.
  let named = 0;
  for (let i = 0; i < devices.length; i++) {
    const dp = params ? params[devices[i].id] : null;
    if (dp && dp.name) named++;
  }
  if (!devices.length) {
    buf.notice = 'no devices on this track';
    buf._nNamed = -1; buf._nTotal = -1;
  } else if (named === devices.length) {
    buf.notice = '';
    buf._nNamed = -1; buf._nTotal = -1;
  } else if (buf._nNamed !== named || buf._nTotal !== devices.length) {
    buf._nNamed = named; buf._nTotal = devices.length;
    buf.notice = 'asking the hosts for names and parameters — '
               + named + ' of ' + devices.length;
  }

  const n = Math.min(devices.length, buf.cards.length);
  for (let i = 0; i < n; i++) {
    const d = devices[i];
    const c = buf.cards[i];
    c.id = d.id;
    c.kind = d.kind;
    c.badge = KIND_BADGE[d.kind] || 'DEV';

    /**
     * The device's buses, and — the part that matters — whether we have them ALL.
     *
     * `busCount` is what the engine said was coming; `buses.length` is what has
     * arrived. While those differ the set is INCOMPLETE, and the card says so
     * instead of summarising what it happens to hold. A rack that renders "2 out"
     * from the first two of eight and silently becomes "8 out" a frame later is the
     * draw-then-rearrange this field was added to prevent — and it is worse than
     * saying nothing, because the wrong number is one somebody might act on.
     *
     * Guarded on a key built from the three inputs, so a device whose buses have
     * not moved costs one integer compare per draw rather than a walk of its list.
     */
    const bl = d.buses;
    const bn = bl ? bl.length : 0;
    const want = d.busCount || 0;
    const bKey = (want << 12) | (bn << 4) | (d.busTruncated ? 1 : 0);
    if (c._bKey !== bKey) {
      c._bKey = bKey;
      c.busTruncated = !!d.busTruncated;
      c.busPartial = bn < want;
      if (!want && !bn) {
        c.busText = '';
      } else if (bn < want) {
        c.busText = busPartialText(bn, want);
      } else {
        let ins = 0, outs = 0;
        for (let k = 0; k < bn; k++) { if (bl[k].input) ins++; else outs++; }
        c.busText = busSummary(ins, outs);
      }
    }

    // The plugin's own name once the host has answered, and what it IS until
    // then. Both are true statements; only one of them is the device's name.
    const dp = params ? params[d.id] : null;
    const nm = (dp && dp.name) ? dp.name : null;
    c.named = !!nm;
    if (c._tName !== nm || c._tKind !== d.kind || c._tId !== d.id) {
      c._tName = nm; c._tKind = d.kind; c._tId = d.id;
      // No concatenation at all in the named case: the host's string is used as
      // it arrived, and only the fallback builds anything.
      c.title = nm || ((DEVICE_KINDS[d.kind] || ('kind ' + d.kind)) + ' #' + d.id);
    }

    const src = (dp && dp.params) ? dp.params : null;
    const total = src ? src.length : 0;
    const shown = Math.min(total, MAX_PARAMS);
    // Grow the pool to what this card needs and never shrink it: the count moves
    // when you click along a rack, and rebuilding the slots on that transition
    // is an allocation per parameter per selection.
    const slots = c.params;
    while (slots.length < shown) slots.push(createParamSlot());
    // Copy the host's answer into the slots ONLY when it is a different answer.
    //
    // Keyed on (the answer array itself, the device it describes, how many of it
    // we take). A host's answer is never edited in place — it arrives from
    // JSON.parse and `deviceParams[id]` is replaced wholesale — so the same
    // array is the same values, and the two extra terms cover the case the array
    // identity cannot: card slot `i` is reused when the rack changes shape, so a
    // key that named only the array would let device B's values stand under
    // device A's name. The key is stored beside the slots it filled, which is
    // what makes that safe — after B has written them the stored key names B.
    //
    // Eight Zebra2s is 2,048 of these, sixty times a second, to write back the
    // numbers already there.
    if (c._pSrc !== src || c._pId !== d.id || c._pCount !== shown) {
      c._pSrc = src; c._pId = d.id; c._pCount = shown;
      for (let k = 0; k < shown; k++) {
        const q = src[k];
        const p = slots[k];
        // The engine's index, kept because the wire orders by it — but the
        // command that eventually writes a value should carry `uid` too; the
        // region's own comment says the index is for ordering and the uid is
        // the identity.
        p.index = q.index === undefined ? k : q.index;
        p.uid = q.uid || '';
        p.name = q.name;
        // ALREADY NORMALISED. The engine publishes milli-units and the bridge
        // divides by 1000 before it reaches here, so dividing again put every
        // bar at a thousandth of its length — all of them empty, next to
        // values that were right. A bar that is always zero beside a correct
        // number is worse than no bar: it reads as "this parameter is off".
        const frac = q.value < 0 ? 0 : (q.value > 1 ? 1 : q.value);
        p.frac = frac;
        p.milli = Math.round(frac * 1000);
        // The host's own display string when it has one — it knows whether a
        // value is dB, Hz or a note name, and this side does not. Only the
        // fallback formats, and only when the number it formats has moved.
        if (q.display) {
          p.display = q.display;
          p._dv = NaN;
        } else if (p._dv !== q.value) {
          p._dv = q.value;
          p.display = q.value.toFixed(2);
        }
      }
    }
    c.paramCount = shown;
    // Honest about what was dropped. With the list scrollable nothing is hidden
    // by the CARD any more, so this can only fire when the ENGINE truncated —
    // its region holds 256 — and saying "+N more" then is the difference between
    // a rack that is complete and one that looks it.
    const moreCount = total - shown;
    if (c._mCount !== moreCount) {
      c._mCount = moreCount;
      c.more = moreCount > 0 ? '+' + moreCount + ' more' : '';
    }

    c.pos = d.pos;
    c.patcherNode = d.node === DEVICE_ID_AUTO ? -1 : d.node;
    c.caps = describeCaps(d.caps);
    c.bypass = !!d.bypass;
    c.selected = i === selected;
    if (c._sSlot !== d.slot || c._sNode !== c.patcherNode
        || c._sCount !== shown || c._sMore !== moreCount) {
      c._sSlot = d.slot; c._sNode = c.patcherNode;
      c._sCount = shown; c._sMore = moreCount;
      c.sub = d.slot === HOST_SLOT_DIRECT
        ? 'in-process'
        : (d.slot === DEVICE_ID_AUTO ? 'slot unassigned' : 'slot ' + d.slot);
      if (c.patcherNode >= 0) c.sub += ' · node ' + c.patcherNode;
      // The count belongs on screen, not just in the scrollbar: six rows of 256
      // with a thin scrollbar reads as "this plugin has six parameters" unless
      // something says otherwise. This is the something.
      if (shown) c.sub += ' · ' + shown + ' params';
      if (moreCount > 0) c.sub += ' · ' + c.more;
    }
    buf.cardCount++;
  }
  return buf;
}

// ---------------------------------------------------------------------------
// Parameter edits in flight.
//
// A parameter belongs to the PLUGIN, not to this side: the only value that is
// true is the one its host reports. So an edit is held here, apart from the
// view-model, for exactly as long as it takes the engine to agree — and if the
// engine does not agree, it is dropped and the bar goes back to where the plugin
// says it is. That is the whole reason this is not simply "write the new value
// into the model": a model that has adopted a value nobody accepted is a
// confident control over a guess (GUIDELINES 4.5).
//
// A flat array rather than a Map: it is read on every frame while a drag is
// live, and `for (const x of map)` allocates an iterator every time. There are
// never more of these than fingers.
// ---------------------------------------------------------------------------

/** How long an edit waits for the engine before it is treated as refused. */
export const EDIT_HOLD_MS = 1200;

export function createParamEdits(cap = 8) {
  const slots = new Array(cap);
  for (let i = 0; i < cap; i++) {
    slots[i] = { device: -1, index: -1, milli: 0, at: 0 };
  }
  return { slots, count: 0, cap };
}

/** The pending edit for one parameter, or null. */
export function findParamEdit(edits, device, index) {
  for (let i = 0; i < edits.count; i++) {
    const s = edits.slots[i];
    if (s.device === device && s.index === index) return s;
  }
  return null;
}

/**
 * Record (or move) an optimistic value. Returns the slot, or null when there is
 * no room — which is a refusal, not a silent drop: eight parameters cannot be
 * mid-flight at once from one pointer, so a ninth means something is leaking.
 */
export function setParamEdit(edits, device, index, milli, now) {
  const found = findParamEdit(edits, device, index);
  if (found) { found.milli = milli; found.at = now; return found; }
  if (edits.count >= edits.cap) return null;
  const s = edits.slots[edits.count++];
  s.device = device; s.index = index; s.milli = milli; s.at = now;
  return s;
}

export function dropParamEdit(edits, slot) {
  for (let i = 0; i < edits.count; i++) {
    if (edits.slots[i] !== slot) continue;
    // Swap with the last live slot rather than splice: the array is the pool.
    const last = edits.slots[edits.count - 1];
    edits.slots[i] = last;
    edits.slots[edits.count - 1] = slot;
    edits.count--;
    return true;
  }
  return false;
}

/**
 * Settle what the engine has answered and expire what it has not.
 *
 * `engineMilli(device, index)` returns the published value, or -1 when the
 * parameter is not on screen at all. `held` is the one the pointer is still on,
 * which never expires — a person resting mid-drag has not been refused.
 *
 * Returns the number of edits that expired, so the caller can say so. An edit
 * that SETTLED is dropped silently: that is just the value arriving.
 */
export function reapParamEdits(edits, engineMilli, now, held, holdMs = EDIT_HOLD_MS) {
  let expired = 0;
  for (let i = edits.count - 1; i >= 0; i--) {
    const s = edits.slots[i];
    const v = engineMilli(s.device, s.index);
    // Within a milli-unit is the same value: the engine rounds to the same grid
    // this does, but a plugin is free to quantise a parameter to its own steps.
    if (v >= 0 && Math.abs(v - s.milli) <= 1) { dropParamEdit(edits, s); continue; }
    if (s === held) continue;
    if (now - s.at > holdMs) { dropParamEdit(edits, s); expired++; }
  }
  return expired;
}
