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

import { createStore, decode, decodeWaveform, createWaveform } from './wire.js';

/**
 * A connection that connects to nothing.
 *
 * The page calls `conn.close()`, `conn.send()` and friends unconditionally, and
 * making every one of those sites null-check would be eight chances to forget.
 * This is what `?engine=off` hands back instead: the same shape, every send
 * refused. `send` returning false is exactly what a real connection with no open
 * socket returns, so the callers' "no engine" path is the tested one.
 */
export function noEngine() {
  return {
    store: createStore(),
    canSend: () => false,
    send: () => false,
    sendBatch: () => false,
    loadProject: () => false,
    saveProject: () => false,
    setViewport: () => {},
    connected: () => false,
    stats: () => ({ framesIn: 0, gaps: 0, lastSeq: -1, connected: false }),
    close: () => {},
  };
}

export function connectEngine({ url = 'ws://127.0.0.1:8174', cmdUrl = 'ws://127.0.0.1:8175',
                                onChange, onStatus, onAck, onEngineEvent, onChains, onScales,
                                onDeviceParams, onWaveform, onAudioSources } = {}) {
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
      if (typeof ev.data === 'string') {
        // Two kinds of text on this socket. {"engine":[...]} is the engine's own
        // outbound ring, drained by the sidecar — a refusal the engine made that
        // nothing else can tell us about. Anything else is the sidecar's error
        // channel, as before. Parse alone in the try; act outside it.
        let parsed = null;
        try { parsed = JSON.parse(ev.data); } catch (err) { /* not JSON: raw */ }
        // One device's parameters, in answer to a reqparams. The engine fills a
        // single region per query, so this is the answer to the most recent
        // question rather than accumulated state.
        if (parsed && parsed.deviceParams) {
          onDeviceParams && onDeviceParams(parsed.deviceParams);
          return;
        }
        // The audio source table: which files this project decodes, and which
        // clip reads which. Version-gated by the sidecar, so it arrives on a
        // project load and not on every frame — the frame rate is 86 Hz and these
        // carry u64 frame counts, which would be a BigInt per field per frame.
        if (parsed && parsed.audioSources) {
          onAudioSources && onAudioSources(parsed.audioSources);
          return;
        }
        // The scale registry. Written once by the engine at startup, so it
        // arrives once per connection and never changes under us.
        if (parsed && Array.isArray(parsed.scales)) {
          onScales && onScales(parsed.scales);
          return;
        }
        // Per-track device chains, accumulated by the sidecar from the engine's
        // ChainSnapshot diffs. State, not news — a fresh tab is told all of it.
        if (parsed && Array.isArray(parsed.chains)) {
          onChains && onChains(parsed.chains, parsed.rev);
          return;
        }
        if (parsed && Array.isArray(parsed.engine)) {
          onEngineEvent && onEngineEvent(parsed.engine, parsed.missed || 0);
          return;
        }
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
  // One decode target, reused. The answer's `pairs` is a view over the incoming
  // buffer rather than a copy, so the record itself holds no data and the
  // consumer must read it before the next frame arrives — which it does, because
  // the consumer is a synchronous callback.
  const waveBuf = createWaveform();
  let cmdBackoff = 250;
  // Last viewport we sent, replayed on (re)connect. The socket is usually not
  // open yet when the first frame draws, and a restarted sidecar comes back at
  // its 4-lines-per-beat default — in both cases the client would otherwise sit
  // there believing it had already said what it was looking at.
  let lastVp = null;
  function openCmd() {
    if (closed) return;
    cmdWs = new WebSocket(cmdUrl);
    // Waveform answers come back on this socket as BINARY — a full slot is 98 KB
    // of int16 and JSON numbers would be four times that in text, for something
    // the renderer wants as a typed array. Without this the frames arrive as Blob
    // and every reader has to go async to look at them.
    cmdWs.binaryType = 'arraybuffer';
    cmdWs.onopen = () => { cmdBackoff = 250; if (lastVp) cmdWs.send(lastVp); };
    cmdWs.onmessage = (ev) => {
      if (ev.data instanceof ArrayBuffer) {
        // Decoded HERE, like the state frame, so the page never sees a raw
        // buffer and there is one place that knows the wire format.
        const w = decodeWaveform(ev.data, waveBuf);
        if (w && onWaveform) onWaveform(w);
        return;
      }
      if (onAck) onAck(ev.data);
    };
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
      /*
       * WHAT THIS EDIT WAS COMPOSED AGAINST — and this side no longer guesses.
       *
       * It used to stamp `store.clipVersion`, the GLOBAL counter, on anything
       * that had not named a base. That was right until M2.17 made acceptance
       * PER TRACK: the counters diverge on the first edit, and an edit on track 1
       * quoting the global is not refused with a message — it is DROPPED. Note
       * entry, chords and transpose all stopped working on every track except the
       * one edited most recently, with nothing on screen to say so. Measured on
       * `maximal`: three notes on track 0 left global 5, track 0 at 5, and every
       * other track still at 1.
       *
       * The page cannot stamp the right number because the per-track counters are
       * not on the wire, and putting them there would mean every client
       * re-deriving what the sidecar can read in one call. So the sidecar fills
       * it in — the same argument it already makes for re-basing a BATCH, which
       * has to happen on the side that can wait for the engine.
       *
       * A caller that DOES know still wins: harmony validates against its own
       * counter and `setHarmony` looks that up, so it names a base and this must
       * not overwrite it. That exact overwrite once had the engine reporting
       * `base=4 current=3` to a page that had sent 3, and I spent a while reading
       * it as an engine race.
       */
      cmdWs.send(JSON.stringify(obj));
      return true;
    },
    /**
     * Several edits as one frame, applied in order by the sidecar.
     *
     * Not a convenience: the engine arbitrates by base_version, so a client that
     * stamps every op of a transpose with the same version gets the first applied
     * and the rest rejected. The sidecar re-bases each op on the version the
     * previous one produced, which is the only place that can be done — it is the
     * side that can wait for the engine to acknowledge.
     */
    sendBatch(objs) {
      if (!cmdWs || cmdWs.readyState !== 1 || !objs.length) return false;
      let msg = 'BATCH';
      for (const o of objs) msg += '\n' + JSON.stringify(o);
      cmdWs.send(msg);
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
    /**
     * Just the one bit, without the object.
     *
     * `stats()` builds a fresh record every call, which is right for the HUD and
     * for a test reading it once — but the chrome asks "are we live?" on every
     * frame from updateChrome(), and that was one object per frame to read one
     * boolean off it. Callers that want the whole record still get it.
     */
    connected: () => !!(ws && ws.readyState === 1),
    stats: () => ({ framesIn, gaps, lastSeq, connected: ws && ws.readyState === 1 }),
    close() { closed = true; if (ws) ws.close(); if (cmdWs) cmdWs.close(); },
  };
}
