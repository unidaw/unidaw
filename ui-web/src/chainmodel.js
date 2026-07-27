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
 * Sentinels from device_chain.h. `kDeviceIdAuto` doubles as the engine's
 * "this chain is empty" marker in a snapshot, and `kHostSlotIndexDirect` means
 * the device is not hosted out-of-process at all.
 */
export const DEVICE_ID_AUTO = 0xffffffff;
const HOST_SLOT_DIRECT = 0xfffffffe;

/** Which capabilities a mask names, as a short phrase. Empty when none. */
function describeCaps(mask) {
  const on = [];
  for (const [bit, name] of CAPS) if (mask & bit) on.push(name);
  return on.join(' · ');
}

export function createChainBuffer(cap = 16) {
  const cards = new Array(cap);
  for (let i = 0; i < cap; i++) {
    cards[i] = { id: 0, kind: 0, badge: '', title: '', pos: 0,
                 sub: '', caps: '', bypass: false, selected: false, patcherNode: -1,
                 named: false, params: [], paramCount: 0, more: '' };
  }
  return { cards, cardCount: 0, track: 0, trackName: '', version: -1,
           known: false, notice: '', _cap: cap };
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
  /** How many parameters a card shows before it stops listing them. */
  const SHOWN = 5;
  const entry = chains ? chains[track] : null;

  buf.track = track;
  buf.trackName = trackName;
  buf.version = entry ? entry.version : -1;
  buf.known = !!entry;
  buf.cardCount = 0;

  if (!entry) {
    buf.notice = 'the engine publishes a chain only when it changes — '
               + 'press r to ask for this track’s';
    return buf;
  }

  const devices = entry.devices || [];
  // How much of the rack has actually answered. A blanket "identity only" was
  // right when nothing could answer and is wrong now that some cards have.
  let named = 0;
  for (const d of devices) if (params && params[d.id] && params[d.id].name) named++;
  buf.notice = !devices.length
    ? 'no devices on this track'
    : (named === devices.length
        ? ''
        : 'asking the hosts for names and parameters — ' + named + ' of ' + devices.length);

  const n = Math.min(devices.length, buf.cards.length);
  for (let i = 0; i < n; i++) {
    const d = devices[i];
    const c = buf.cards[i];
    c.id = d.id;
    c.kind = d.kind;
    c.badge = KIND_BADGE[d.kind] || 'DEV';
    // The plugin's own name once the host has answered, and what it IS until
    // then. Both are true statements; only one of them is the device's name.
    const dp = params ? params[d.id] : null;
    c.named = !!(dp && dp.name);
    c.title = c.named ? dp.name : (DEVICE_KINDS[d.kind] || ('kind ' + d.kind)) + ' #' + d.id;
    c.params.length = 0;
    if (dp && dp.params) {
      for (let k = 0; k < dp.params.length && k < SHOWN; k++) {
        const q = dp.params[k];
        c.params.push({
          name: q.name,
          // The host's own display string when it has one — it knows whether a
          // value is dB, Hz or a note name, and this side does not.
          display: q.display || (Math.round(q.value / 10) / 100).toFixed(2),
          frac: Math.max(0, Math.min(1, q.value / 1000)),
        });
      }
      c.more = dp.params.length > SHOWN ? '+' + (dp.params.length - SHOWN) + ' more' : '';
    } else {
      c.more = '';
    }
    c.paramCount = c.params.length;
    c.pos = d.pos;
    c.patcherNode = d.node === DEVICE_ID_AUTO ? -1 : d.node;
    c.caps = describeCaps(d.caps);
    c.bypass = !!d.bypass;
    c.selected = i === selected;
    c.sub = d.slot === HOST_SLOT_DIRECT
      ? 'in-process'
      : (d.slot === DEVICE_ID_AUTO ? 'slot unassigned' : 'slot ' + d.slot);
    if (c.patcherNode >= 0) c.sub += ' · node ' + c.patcherNode;
    buf.cardCount++;
  }
  return buf;
}
