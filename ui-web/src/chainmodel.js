// The device chain view-model: one card per device on the cursor's track.
//
// The engine's chain is published as ChainSnapshot diffs on its outbound ring —
// one entry per device, all sharing a chainVersion — which the sidecar drains
// and accumulates (it is a single-consumer ring, so one drainer and a shared
// buffer). By the time it reaches here it is already per-track and already
// deduplicated by version.
//
// WHAT THIS SURFACE DOES NOT KNOW, and says so on screen rather than inventing:
// the engine publishes a device's IDENTITY (id, kind, position, patcher node,
// host slot, capabilities, bypass) and nothing about its CONTENTS. There are no
// names, no parameters, no macro values. The design's cards show "cutoff 0.62"
// and eight named macro knobs; none of that crosses the wire today. Drawing a
// slider with a plausible number on it would be the worst option available —
// see GUIDELINES 2.1, and the mixer, which carried its own "this is a local
// guess" line for exactly as long as it was one.

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
                 sub: '', caps: '', bypass: false, selected: false, patcherNode: -1 };
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
  const { track = 0, chains = null, selected = -1, trackName = '' } = opts;
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
  buf.notice = devices.length
    ? 'names and parameters are not published yet — identity only'
    : 'no devices on this track';

  const n = Math.min(devices.length, buf.cards.length);
  for (let i = 0; i < n; i++) {
    const d = devices[i];
    const c = buf.cards[i];
    c.id = d.id;
    c.kind = d.kind;
    c.badge = KIND_BADGE[d.kind] || 'DEV';
    // The engine gives no name, so the card is titled by what it IS plus the id
    // that identifies it. An invented name would be the only thing on the card
    // that was not true.
    c.title = (DEVICE_KINDS[d.kind] || ('kind ' + d.kind)) + ' #' + d.id;
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
