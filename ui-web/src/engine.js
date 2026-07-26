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

export function connectEngine({ url = 'ws://127.0.0.1:8174', onChange, onStatus } = {}) {
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

  open();

  return {
    store,
    stats: () => ({ framesIn, gaps, lastSeq, connected: ws && ws.readyState === 1 }),
    close() { closed = true; if (ws) ws.close(); },
  };
}
