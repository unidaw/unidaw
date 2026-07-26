// WebSocket client for the sidecar.
//
// Connects, decodes into a pooled store, and calls back when something changed.
// Reconnects with backoff, because the engine is a separate process that can be
// restarted underneath us and the UI should recover rather than need a reload.
//
// The socket is deliberately NOT the render loop. Frames arrive at the engine's
// ~86 Hz publish rate and the UI draws at display rate; coupling them would make
// the renderer run at whatever cadence the audio block size implies. Instead we
// mark dirty and let the existing rAF scheduler pick it up.

import { createStore, decode } from './wire.js';

export function connectEngine({ url = 'ws://127.0.0.1:8174', cmdUrl = 'ws://127.0.0.1:8175',
                                onChange, onStatus, onAck } = {}) {
  const store = createStore();
  let ws = null;
  let closed = false;
  let backoff = 250;
  let framesIn = 0;
  let lastSeq = -1;
  let gaps = 0;

  const status = (s, detail) => onStatus && onStatus(s, detail);

  function open() {
    if (closed) return;
    status('connecting');
    ws = new WebSocket(url);
    ws.binaryType = 'arraybuffer';

    ws.onopen = () => { backoff = 250; status('connected'); };

    ws.onmessage = (ev) => {
      if (typeof ev.data === 'string') {          // the sidecar's error channel
        status('error', ev.data);
        return;
      }
      if (!decode(ev.data, store)) { status('error', 'undecodable frame'); return; }
      framesIn++;
      // A gap means the socket dropped a frame, which on loopback should never
      // happen — worth surfacing rather than silently smoothing over.
      if (lastSeq >= 0 && store.seq !== lastSeq + 1) gaps++;
      lastSeq = store.seq;
      onChange && onChange(store);
    };

    ws.onclose = () => {
      if (closed) return;
      status('disconnected');
      setTimeout(open, backoff);
      backoff = Math.min(backoff * 2, 4000);
    };

    // onerror is followed by onclose; let close drive the retry.
    ws.onerror = () => {};
  }

  // Commands go out on their OWN socket. The state socket is write-only, and
  // commands are a different shape anyway: rare, individually meaningful, and
  // wanting an acknowledgement. They do not need to be synchronised with a
  // frame — each carries the clip version it was composed against and the engine
  // arbitrates by version, not by arrival order.
  let cmdWs = null;
  let cmdBackoff = 250;
  // Last viewport we sent, replayed on (re)connect. The socket is usually not
  // open yet when the first frame draws, and a restarted sidecar comes back at
  // its 4-lines-per-beat default — in both cases the client would otherwise sit
  // there believing it had already said what it was looking at.
  let lastVp = null;
  function openCmd() {
    if (closed) return;
    cmdWs = new WebSocket(cmdUrl);
    cmdWs.onopen = () => { cmdBackoff = 250; if (lastVp) cmdWs.send(lastVp); };
    cmdWs.onmessage = (ev) => { if (onAck) onAck(ev.data); };
    cmdWs.onclose = () => {
      if (closed) return;
      setTimeout(openCmd, cmdBackoff);
      cmdBackoff = Math.min(cmdBackoff * 2, 4000);
    };
    cmdWs.onerror = () => {};
  }

  open();
  openCmd();

  return {
    store,
    canSend: () => !!cmdWs && cmdWs.readyState === 1,
    /** Fire-and-reconcile: the reply is an ack, the truth arrives in a frame. */
    send(obj) {
      if (!cmdWs || cmdWs.readyState !== 1) return false;
      obj.base = store.clipVersion;      // what this edit was composed against
      cmdWs.send(JSON.stringify(obj));
      return true;
    },
    /**
     * Open or save a project by name. The engine resolves it inside its own
     * project directory — the name is a name, never a path, and the sidecar
     * refuses anything that could climb out of that directory.
     */
    loadProject(name) { return this.send({ type: 'load', name }); },
    saveProject(name) { return this.send({ type: 'save', name }); },
    /**
     * Tell the sidecar what we are looking at; it does the projection.
     *
     * Sent on the COMMAND socket. The state socket is write-only on the sidecar
     * side, so this went nowhere for as long as it existed — every frame came
     * back projected at the sidecar's default 4 lines per beat, which looks
     * exactly like a correct tracker until a lane disagrees about its grid.
     */
    setViewport(linesPerBeat, firstRow, rowCount) {
      lastVp = `{"linesPerBeat":${linesPerBeat},"firstRow":${firstRow},"rowCount":${rowCount}}`;
      if (!cmdWs || cmdWs.readyState !== 1) return false;
      cmdWs.send(lastVp);
      return true;
    },
    stats: () => ({ framesIn, gaps, lastSeq, connected: ws && ws.readyState === 1 }),
    close() { closed = true; if (ws) ws.close(); if (cmdWs) cmdWs.close(); },
  };
}
