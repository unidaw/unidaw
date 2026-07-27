// Agent dock: a console over the same commands the UI itself sends.
//
// This is requirement (d) — AI-legible and AI-operable — made concrete. Every
// edit the UI performs goes out as a command on the same socket, so a console
// that can send those commands can drive the whole application, and a log of
// them is a readable account of what just happened. An agent gets one surface to
// learn instead of a keymap per view.
//
// It is NOT the engine's agent ring (ShmHeader::ring_ui_agent_offset). That
// format is not published yet; when it is, its traffic joins this log rather
// than needing a second dock.

const MAX_LINES = 300;

function div(cls, parent) {
  const el = document.createElement('div');
  el.className = cls;
  if (parent) parent.appendChild(el);
  return el;
}

/**
 * The command grammar. Deliberately terse and positional — this is typed by a
 * person in a hurry or emitted by a model, and neither wants JSON.
 *
 * Each entry: [name, argSpec, help]. `run` gets (args, api) and returns a string
 * to log, or throws with a message that becomes the error line.
 */
export function createCommands(api) {
  const num = (v, d) => (v === undefined ? d : Number(v));
  return {
    help: { help: 'list commands', run: (a, x) =>
      Object.keys(x.commands).sort().map((k) => k + ' — ' + x.commands[k].help).join('\n') },
    view: { help: 'view tracker|arrange|piano|mixer|patcher', run: (a) => 'view ' + api.setView(a[0]) },
    load: { help: 'load <project>', run: (a) => { api.load(a[0]); return 'loading ' + a[0]; } },
    save: { help: 'save <project>', run: (a) => { api.save(a[0]); return 'saving ' + a[0]; } },
    projects: { help: 'list projects on disk', run: () => { api.listProjects(); return 'listing…'; } },
    play: { help: 'play/pause toggle', run: () => { api.transport('play'); return 'toggle'; } },
    stop: { help: 'stop and rewind', run: () => { api.transport('stop'); return 'stop'; } },
    seek: { help: 'seek <tick>', run: (a) => { api.seek(num(a[0], 0)); return 'seek ' + num(a[0], 0); } },
    note: { help: 'note <pitch> [dur] [vel] — at the cursor', run: (a) => {
      const p = num(a[0], 60);
      if (!(p >= 0 && p <= 127)) throw new Error('pitch out of range: ' + a[0]);
      api.note(p, num(a[1], undefined), num(a[2], undefined));
      return 'note ' + p;
    } },
    del: { help: 'delete the note at the cursor', run: () => { api.del(); return 'delete'; } },
    goto: { help: 'goto <row> [track]', run: (a) => {
      api.goto(num(a[0], 0), a[1] === undefined ? undefined : num(a[1]));
      return 'cursor ' + num(a[0], 0) + (a[1] !== undefined ? ' t' + a[1] : '');
    } },
    zoom: { help: 'zoom <index>', run: (a) => { api.zoom(num(a[0], 3)); return 'zoom ' + num(a[0], 3); } },
    oct: { help: 'oct <n>', run: (a) => { api.octave(num(a[0], 4)); return 'octave ' + num(a[0], 4); } },
    gain: { help: 'gain <track> <dB>', run: (a) => {
      api.gain(num(a[0], 0), num(a[1], 0)); return 'gain t' + num(a[0], 0) + ' ' + num(a[1], 0) + 'dB'; } },
    mute: { help: 'mute <track>', run: (a) => { api.strip(num(a[0], 0), 'mute'); return 'mute t' + num(a[0], 0); } },
    solo: { help: 'solo <track>', run: (a) => { api.strip(num(a[0], 0), 'solo'); return 'solo t' + num(a[0], 0); } },
    state: { help: 'dump UI state', run: () => JSON.stringify(api.state()) },
    engine: { help: 'dump engine state', run: () => JSON.stringify(api.engine()) },
    undo: { help: 'undo', run: () => { api.transport('undo'); return 'undo'; } },
    redo: { help: 'redo', run: () => { api.transport('redo'); return 'redo'; } },
    follow: { help: 'follow [on|off] — keep the playhead in view', run: (a) =>
      'follow ' + (api.follow(a[0] === undefined ? undefined : a[0] !== 'off') ? 'on' : 'off') },
    rename: { help: 'rename <track> <name>', run: (a) => {
      const t = num(a[0], -1);
      if (!(t >= 0)) throw new Error('rename needs a track number');
      const name = a.slice(1).join(' ').trim();
      if (!name) throw new Error('rename needs a name');
      api.rename(t, name);
      return 'renamed t' + t + ' to ' + name;
    } },
    select: { help: 'select <row0> <row1> [track] — a tracker range', run: (a) => {
      const r0 = num(a[0], 0), r1 = num(a[1], r0);
      const tr = a[2] === undefined ? undefined : num(a[2]);
      return 'selected ' + api.select(r0, r1, tr) + ' note(s)';
    } },
    transpose: { help: 'transpose <semitones> — the selection', run: (a) => {
      const n = num(a[0], 0);
      if (!n) throw new Error('transpose by how much?');
      api.transpose(n);
      return 'transposed ' + (n > 0 ? '+' : '') + n;
    } },
    copy: { help: 'copy the selection', run: () => (api.copy() ? 'copied' : 'nothing to copy') },
    paste: { help: 'paste at the cursor', run: () => (api.paste() ? 'pasted' : 'clipboard empty') },
    cut: { help: 'cut the selection', run: () => (api.cut() ? 'cut' : 'nothing to cut') },
    nodes: { help: 'list patcher nodes with their editable fields', run: () => {
      const ns = api.nodes();
      if (!ns.length) return 'no patcher nodes';
      return ns.map((n) => '#' + n.id + ' ' + n.type
        + (n.fields.length ? '  [' + n.fields.join(' ') + ']' : '  (no config)')).join('\n');
    } },
    patch: { help: 'patch <node> <field> <steps> — nudge a patcher config field', run: (a) => {
      if (a.length < 3) throw new Error('patch <node> <field> <steps>');
      return api.patch(num(a[0], 0), a[1], num(a[2], 0));
    } },
    clear: { help: 'clear the log', run: (a, x) => { x.clear(); return null; } },
  };
}

export class Dock {
  constructor(host, api) {
    this.host = host;
    this.host.className = 'dk';
    this.api = api;
    this.commands = createCommands(api);

    this.logEl = div('dk-log', host);
    const row = div('dk-row', host);
    this.prompt = div('dk-prompt', row);
    this.prompt.appendChild(document.createTextNode('>'));
    this.input = document.createElement('input');
    this.input.className = 'dk-input';
    this.input.spellcheck = false;
    this.input.autocomplete = 'off';
    this.input.placeholder = 'help';
    row.appendChild(this.input);

    this.lines = [];
    this.pool = [];
    this.history = [];
    this.historyAt = -1;
    this._dirty = true;

    this.input.addEventListener('keydown', (e) => {
      // Stops here rather than bubbling to the app: a console that also plays
      // notes while you type into it is unusable.
      e.stopPropagation();
      if (e.key === 'Enter') { this.submit(this.input.value); this.input.value = ''; }
      else if (e.key === 'ArrowUp') { this.recall(-1); e.preventDefault(); }
      else if (e.key === 'ArrowDown') { this.recall(1); e.preventDefault(); }
      else if (e.key === 'Escape') { this.api.close(); }
    });
  }

  focus() { this.input.focus(); }

  recall(d) {
    if (!this.history.length) return;
    this.historyAt = Math.max(0, Math.min(this.history.length - 1,
      this.historyAt < 0 ? this.history.length - 1 : this.historyAt + d));
    this.input.value = this.history[this.historyAt];
  }

  log(kind, text) {
    if (text === null || text === undefined) return;
    for (const part of String(text).split('\n')) {
      this.lines.push({ kind, text: part });
      if (this.lines.length > MAX_LINES) this.lines.shift();
    }
    this._dirty = true;
  }

  clear() { this.lines.length = 0; this._dirty = true; }

  submit(raw) {
    const line = raw.trim();
    if (!line) return;
    this.history.push(line);
    this.historyAt = -1;
    this.log('in', '> ' + line);
    const [name, ...args] = line.split(/\s+/);
    const cmd = this.commands[name];
    if (!cmd) { this.log('err', 'unknown: ' + name + ' (try help)'); return; }
    try {
      this.log('out', cmd.run(args, this));
    } catch (err) {
      // Surfaced, never swallowed — the same rule the cells follow.
      this.log('err', String(err && err.message ? err.message : err));
    }
  }

  render() {
    if (!this._dirty) return;
    this._dirty = false;
    const n = this.lines.length;
    while (this.pool.length < n) {
      const el = div('dk-line', this.logEl);
      el.appendChild(document.createTextNode(''));
      el._text = null; el._kind = null;
      this.pool.push(el);
    }
    for (let i = 0; i < this.pool.length; i++) {
      const el = this.pool[i];
      const on = i < n;
      const disp = on ? '' : 'none';
      if (el.style.display !== disp) el.style.display = disp;
      if (!on) continue;
      const l = this.lines[i];
      if (el._text !== l.text) { el._text = l.text; el.firstChild.nodeValue = l.text; }
      if (el._kind !== l.kind) {
        el._kind = l.kind;
        el.className = 'dk-line ' + l.kind;
      }
    }
    this.logEl.scrollTop = this.logEl.scrollHeight;
  }

  probe() {
    return {
      lines: this.lines.length,
      last: this.lines.slice(-6).map((l) => l.kind + ': ' + l.text),
      commands: Object.keys(this.commands).sort(),
    };
  }
}
