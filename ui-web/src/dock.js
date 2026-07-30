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

import { ZOOM_LEVELS } from './viewmodel.js';
import { EDGE_KINDS } from './patchermodel.js';

const MAX_LINES = 300;

function div(cls, parent) {
  const el = document.createElement('div');
  el.className = cls;
  if (parent) parent.appendChild(el);
  return el;
}

// ---------------------------------------------------------------------------
// The argument schema
//
// Every command says what it takes twice: once as prose in `help`, which is
// what a person and a model read, and once as `args`, which is what the
// validator reads. Nothing used to force those two to agree, and they did not:
// `select <row0> <row1>` documented two required rows and accepted one. A
// `test/unit.mjs` case parses the prose and compares it to the schema, so the
// pair cannot drift again — that test is the reason the schema is a schema and
// not a second thing to keep in step by hand.
//
// An argument is:
//   { name, type, min, max, values, optional, rest }
//   int    a whole number; `min`/`max` are inclusive
//   num    any finite number
//   text   one word, taken as typed; `rest: true` swallows the rest of the line
//   enum   one of `values`, which ARE the name in the prose — `[on|off]`
//   optional: true is what the [brackets] in the prose mean
//
// Ranges name the limit the app enforces further in. They exist so a bad
// argument is refused BY NAME rather than clamped into something nobody asked
// for: `zoom 99` used to land on the last zoom level and `gain 400` on +12 dB,
// both indistinguishable from a command that worked.

/** Frozen so a shared empty spec cannot be pushed onto by mistake. */
const NONE = Object.freeze([]);
/** An optional on|off word. Absent means "toggle", which is what a key does. */
const ON_OFF = Object.freeze({ name: 'on|off', type: 'enum', values: ['on', 'off'],
                               optional: true });

// Sixteen mixer strips exist (`createMixerState(16)` in index.html), and the
// tracker's cursor is clamped to the same count. `gain 16 0` indexed past the
// end of that array and reported "Cannot read properties of undefined", which
// names nothing the person typed.
const MAX_TRACK = 15;
const MIDI_MAX = 127;
// The wire carries milli-BPM, so anything under a thousandth rounds to a tempo
// of zero — which the engine refuses, several layers further away from the
// argument that caused it.
const MIN_BPM = 0.001;

const A_TRACK = { name: 'track', type: 'int', min: 0, max: MAX_TRACK };
const A_TRACK_OPT = { name: 'track', type: 'int', min: 0, max: MAX_TRACK, optional: true };

/**
 * An enum's values ARE its name in the prose, so one list serves both and there
 * is nothing to keep in step: `follow [on|off]` reads as English and parses as a
 * spec. The drift test compares the prose token against `values.join('|')`, so
 * adding a value here without saying so in the help string fails the build.
 */
function oneOf(values, optional) {
  return { name: values.join('|'), type: 'enum', values, optional: !!optional };
}

const VIEWS = ['tracker', 'arrange', 'piano', 'mixer', 'patcher'];

/**
 * Quantize grids, as a musician names them, in NANOTICKS.
 *
 * The engine takes a tick count rather than a subdivision on purpose — a lane can
 * quantize to something its display grid does not show — but nobody should have to
 * type 240000 to mean a sixteenth. 960000 per quarter; a triplet is a third of the
 * straight value above it, which is why 1/8t is 320000 and not 240000.
 *
 * `off` is 0, and 0 is a real value the engine understands rather than an absent
 * argument: it is how a lane stops being quantized.
 */
const QUANTIZE_GRIDS = Object.freeze({
  off: 0,
  '1/4': 960000, '1/8': 480000, '1/16': 240000, '1/32': 120000,
  '1/4t': 640000, '1/8t': 320000, '1/16t': 160000,
});
const GRID_NAMES = Object.freeze(Object.keys(QUANTIZE_GRIDS));

/**
 * The order a click cycles through, which is the MUSICAL order and not the numeric one.
 *
 * They disagree: 1/4t is 640000 ticks, which sits between 1/8 and 1/4 by value and after
 * every straight grid by feel. Cycling by tick value would step from 1/32 straight to a
 * triplet, which reads as a bug rather than as a groove.
 *
 * `off` is first, so the cycle passes through it: turning quantize off has to be as
 * ordinary as changing it.
 */
export const QUANTIZE_CYCLE = GRID_NAMES;

/** The nanoticks a grid name means, or 0. The inverse of quantizeGridName. */
export function quantizeGridTicks(name) {
  return QUANTIZE_GRIDS[name] || 0;
}

/** The name for a grid in nanoticks, for reading a lane's setting back. */
export function quantizeGridName(nanoticks) {
  for (const k of GRID_NAMES) if (QUANTIZE_GRIDS[k] === nanoticks) return k;
  // A grid the engine holds that this table cannot name is still a real setting —
  // showing the raw ticks is honest where showing "off" would be a lie.
  return nanoticks > 0 ? nanoticks + 't' : 'off';
}

/**
 * WHY A COMMAND FAILED, in the app's own words.
 *
 * Every one of these `run` bodies had `: 'no engine'` on the false branch, which is one
 * cause of failure standing in for all of them: `namemarker 2` aimed at a marker that
 * had been removed answered "no engine" with the engine right there. The app already
 * writes a reason to the reject line for a person to read — this is the same sentence,
 * for whoever is reading the dock instead.
 */
function refusal(api) {
  const s = api.state && api.state();
  return (s && s.reject) || 'no engine';
}

/** `gain <track> <dB>` — built from the schema, quoted by every refusal. */
function signatureOf(name, args) {
  let s = name;
  for (let i = 0; i < args.length; i++) {
    const a = args[i];
    s += a.optional ? ' [' + a.name + ']' : ' <' + a.name + '>';
  }
  return s;
}

// `<name>` or `[name]`, and nothing that mixes the two brackets.
const PLACEHOLDER = /<([^<>[\]]+)>|\[([^<>[\]]+)\]/g;

/**
 * The placeholders a help string declares, in order. Only the signature is
 * read — the em dash starts commentary, and `tempo <bpm> [tick] — whole song,
 * or one point from <tick>` mentions an argument there that it does not take a
 * second time.
 *
 * This is the lint half of the schema: it is what lets a test say that the
 * prose and the spec describe the same command.
 */
export function parseHelpArgs(help) {
  const sig = String(help).split('—')[0];
  const out = [];
  PLACEHOLDER.lastIndex = 0;
  let m;
  while ((m = PLACEHOLDER.exec(sig)) !== null) {
    out.push({ name: m[1] !== undefined ? m[1] : m[2], optional: m[1] === undefined });
  }
  return out;
}

function rangeText(a) {
  if (a.min !== undefined && a.max !== undefined) return 'be between ' + a.min + ' and ' + a.max;
  if (a.min !== undefined) return 'be at least ' + a.min;
  return 'be at most ' + a.max;
}

/**
 * Check `args` against a command's schema. Returns the refusal to show, or null.
 *
 * One place, so every command refuses in the same words and names the argument
 * it is talking about. Before this each `run` either coerced silently — `num(a[0],
 * 3)` turns "loud" into NaN and sails on — or threw a sentence written on the
 * spot, so the same mistake read differently depending on which command you made
 * it in, and in most of them read as success.
 */
export function checkArgs(name, cmd, args) {
  const spec = cmd.args;
  // A command with no schema would accept anything, which is the failure this
  // whole table exists to close. Refuse rather than wave it through.
  if (!spec) return name + ': has no argument schema';
  const rest = spec.length && spec[spec.length - 1].rest;
  for (let i = 0; i < spec.length; i++) {
    const a = spec[i];
    const v = args[i];
    if (v === undefined) {
      if (a.optional) continue;
      // The whole signature only when there is more to it than the argument
      // already named: `view: missing <tracker|arrange|…> — view
      // <tracker|arrange|…>` says one thing twice and reads as a stutter.
      return name + ': missing <' + a.name + '>' + (spec.length > 1 ? ' — ' + cmd.sig : '');
    }
    if (a.type === 'enum') {
      if (a.values.indexOf(v) < 0) {
        return name + ': "' + v + '" is not one of ' + a.values.join(', ');
      }
      continue;
    }
    if (a.type === 'text') continue;
    const n = Number(v);
    if (a.type === 'int' ? !Number.isInteger(n) : !Number.isFinite(n)) {
      return name + ': <' + a.name + '> must be a '
        + (a.type === 'int' ? 'whole number' : 'number') + ', got "' + v + '"';
    }
    if ((a.min !== undefined && n < a.min) || (a.max !== undefined && n > a.max)) {
      return name + ': <' + a.name + '> must ' + rangeText(a) + ', got ' + n;
    }
  }
  if (args.length > spec.length && !rest) {
    return spec.length ? name + ': too many arguments — ' + cmd.sig
                       : name + ': takes no arguments';
  }
  return null;
}

/**
 * The one way a command is executed. The dock and the palette both come through
 * here, so neither can be the surface that skipped the check — two call sites
 * doing their own validation is how the prose and the behaviour parted company
 * in the first place.
 *
 * Refuses by throwing, because that is the shape both callers already handle: a
 * bad argument now lands on the same line an engine refusal does.
 */
export function runCommand(name, cmd, args, host) {
  const bad = checkArgs(name, cmd, args);
  if (bad) throw new Error(bad);
  return cmd.run(args, host);
}

/**
 * The command grammar. Deliberately terse and positional — this is typed by a
 * person in a hurry or emitted by a model, and neither wants JSON.
 *
 * Each entry: `help` (prose), `args` (the same statement, checkable), and `run`,
 * which gets (args, host) and returns a string to log or throws with a message
 * that becomes the error line. `run` is reached only through `runCommand`, so it
 * may assume every argument named in `args` is present and inside its range.
 */
export function createCommands(api) {
  // Optional arguments only: the gate guarantees the required ones arrived and
  // are numbers, so a default there would be dead code pretending to be a check.
  const num = (v, d) => (v === undefined ? d : Number(v));
  const cmds = {
    help: { help: 'list commands', args: NONE, run: (a, x) =>
      Object.keys(x.commands).sort().map((k) => k + ' — ' + x.commands[k].help).join('\n') },
    // The view names are checked rather than passed through: `view trackr` set
    // the view to "trackr", which hides every surface and blames nothing.
    view: { help: 'view <tracker|arrange|piano|mixer|patcher>', args: [oneOf(VIEWS)],
      run: (a) => 'view ' + api.setView(a[0]) },
    // Start a song. There was no way to do this: the only route was to open a
    // preset and overwrite it, so every project began as somebody else's.
    new: { help: 'new [name] — an empty song', args: [{ name: 'name', type: 'text', optional: true }],
      run: (a) => (api.newSong(a[0]) ? 'new ' + (a[0] || 'untitled') : 'no engine') },
    // Remove a device. `adddevice` had a path and this did not, which is a plain
    // asymmetry: you could fill a chain and never empty it.
    deldevice: { help: 'deldevice <track> <device>',
      args: [A_TRACK, { name: 'device', type: 'int', min: 0 }],
      run: (a) => (api.delDevice(Number(a[0]), Number(a[1])) ? 'removed' : 'no engine') },
    // Switch a device off without removing it. The state has been on the wire and
    // drawn on the card since v20 with nothing able to SET it; `b` in the rack
    // sets it now, and this is the same action for anything that is not a hand.
    //
    // It takes the STATE, not a toggle: a caller that has to read the current
    // value to ask for the other one races anything else on the ring, and an
    // agent asking twice would land back where it started.
    bypass: { help: 'bypass <track> <device> [on] — switch a device off (on: 1 bypassed, 0 active)',
      args: [A_TRACK, { name: 'device', type: 'int', min: 0 },
             { name: 'on', type: 'int', min: 0, max: 1, optional: true }],
      run: (a) => (api.bypass(Number(a[0]), Number(a[1]),
                              a[2] === undefined ? 1 : Number(a[2]))
        ? (Number(a[2] === undefined ? 1 : a[2]) ? 'bypassed' : 'active') : 'no engine') },
    /*
     * A CHORD AT THE CURSOR, and the removal of one.
     *
     * Neither existed here. A chord could only be made by typing a degree token into a
     * tracker cell, and could not be unmade at all: `del` looked for a NOTE and
     * answered "no note here" while a chord name sat in the cell. Being able to create
     * something you cannot delete is worse than not being able to create it.
     *
     * Degrees are 1-BASED here, as musicians write them — I, II, III — and 0-based on
     * the wire. Converted once, at this boundary, which is the same rule the entry
     * tokens follow.
     */
    chord: { help: 'chord <degree> [triad|seventh|degree] [inv] [oct] — at the cursor',
      args: [{ name: 'degree', type: 'int', min: 1, max: 13 },
             oneOf(['triad', 'seventh', 'degree'], true),
             { name: 'inv', type: 'int', min: 0, max: 3, optional: true },
             { name: 'oct', type: 'int', min: 0, max: 9, optional: true }],
      run: (a) => {
        // The engine's own numbering: 0 = the degree alone, 1 = triad, 2 = seventh.
        const QUALITY = { degree: 0, triad: 1, seventh: 2 };
        const q = a[1] === undefined ? 1 : QUALITY[a[1]];
        return api.chord(Number(a[0]) - 1, q, a[2] === undefined ? 0 : Number(a[2]),
                         a[3] === undefined ? 4 : Number(a[3]))
          ? `chord ${a[0]}` : 'refused';
      } },
    delchord: { help: 'delchord — remove the chord at the cursor',
      args: [],
      run: () => (api.delChord() ? 'removed' : 'no chord here') },
    /*
     * Reorder a device. `pos` is where it ENDS UP, counted from 0.
     *
     * Order is what a chain is, and the rack could add and remove without it — so the
     * only way to reorder was delete-and-re-add, which throws the device's settings
     * away to change its position.
     */
    movedevice: { help: 'movedevice <track> <device> <pos> — where it ends up, from 0',
      args: [A_TRACK, { name: 'device', type: 'int', min: 0 },
             { name: 'pos', type: 'int', min: 0 }],
      run: (a) => (api.moveDevice(Number(a[0]), Number(a[1]), Number(a[2]))
        ? `device ${a[1]} to ${a[2]}` : 'no engine') },
    /*
     * MODULATION: what moves what.
     *
     * A LINK THE ENGINE ACCEPTS CAN STILL MOVE NOTHING, three separate ways, and every one
     * of them is silent — the link needs a parameter NAME (the engine addresses a VST
     * parameter by uid16 and never by index), the source needs a VALUE (a macro nobody has
     * turned is not in the source table and the link is skipped), and the source must sit
     * STRICTLY EARLIER in the chain than its target (the applier requires it; the command
     * validator does not). `map` does all three, which is why it takes a track and a
     * parameter and not a link's worth of arguments.
     */
    /*
     * AUTOMATION. A read, because that is what this app can do with it: the lanes and the
     * curves are published (v28) and authoring a point is the next piece — said in the help
     * rather than left for someone to discover by looking for a write command.
     */
    automation: { help: 'automation [track] — which parameters are automated',
      args: [A_TRACK_OPT],
      run: (a) => {
        const t = a[0] === undefined ? undefined : Number(a[0]);
        const m = api.automation(t);
        if (!m) return 'no engine';
        if (!m.list.length) {
          return t === undefined ? 'nothing in this song is automated'
                                 : `nothing on track ${t} is automated`;
        }
        const rows = m.list.map((l) =>
          `t${l.track}  ${l.param}  ${l.points} point${l.points === 1 ? '' : 's'}`
          + `  ${l.discrete ? 'stepped' : 'ramped'}`);
        // Truncation is reported. An incomplete list that says nothing reads as a complete one.
        if (m.truncated) rows.push(`… and ${m.truncated} more the engine could not publish`);
        return rows.join('\n');
      } },
    autopoint: { help: 'autopoint <track> <param> <tick> <value> — write one automation point',
      args: [A_TRACK, { name: 'param', type: 'text' },
             { name: 'tick', type: 'int', min: 0 },
             { name: 'value', type: 'num', min: 0, max: 1 }],
      // Writing the same tick again REPLACES that point. Said in the reply, because "wrote a
      // point" and "moved the point that was there" look the same from outside and only one of
      // them is what a second call at the same tick does.
      run: (a) => (api.writeAutomation(Number(a[0]), a[1], Number(a[2]), Number(a[3]))
        ? `${a[1]} = ${a[3]} at ${a[2]} (a point at that tick is replaced)` : refusal(api)) },
    curve: { help: 'curve <track> <param> — one automation lane, point by point',
      args: [A_TRACK, { name: 'param', type: 'text' }],
      run: (a) => {
        const c = api.automationPoints(Number(a[0]), a[1]);
        // `null` is "asked, not answered yet" — a different fact from "nothing automates
        // that", which the engine says explicitly with found:false.
        // The answer arrives on the ack channel and prints ITSELF into this log — see the
        // `automation` branch in `onAck`. This line is a receipt for the question, not an
        // instruction to ask it again.
        if (!c) return 'asking the engine…';
        if (!c.found) return `nothing automates ${a[1]} on track ${a[0]}`;
        const rows = c.points.map(([tick, v]) => `${tick}  ${v.toFixed(3)}`);
        if (c.truncated) rows.push(`… and ${c.truncated} more points than the slot could carry`);
        return `${c.points.length} points, ${c.discrete ? 'stepped' : 'ramped'}\n`
             + rows.join('\n');
      } },
    mods: { help: 'mods [track] — what modulates what', args: [A_TRACK_OPT],
      run: (a) => {
        const t = a[0] === undefined ? undefined : Number(a[0]);
        const m = api.mods(t);
        if (!m) return 'no engine';
        if (!m.links.length) {
          /*
           * "Nothing" and "not published yet" ARE THE SAME ANSWER HERE, and saying so is
           * more use than picking one. The engine publishes a track's modulation only when
           * it changes, and its load-time publish runs BEFORE the links are installed — so
           * a project that has modulation shows none until the first edit. Reported to
           * backend; until it is fixed, a bare "nothing" would be a confident lie half the
           * time.
           */
          return `nothing modulates anything on track ${m.track}`
               + '\n(the engine publishes a track\u2019s modulation only when it changes,'
               + ' so a just-loaded project may not have said yet)';
        }
        return m.links.map((l) => {
          // A link that CANNOT WORK is reported as such, with which of the three reasons.
          // Listing it as ordinary is the same lie the badge would tell by lighting for it.
          const why = l.unnamed ? '  NOT WORKING (names no parameter)'
                    : l.orphan ? '  NOT WORKING (this device has no such parameter)'
                    : l.notForward ? '  NOT WORKING (source is not before its target)'
                    : '';
          return `${l.id}  dev${l.sourceDevice}:${l.sourceKindName}${l.sourceId}`
               + ` -> dev${l.targetDevice}:${l.targetId}`
               + `  depth ${l.depth.toFixed(2)}  ${l.enabled ? l.rateName : 'off'}${why}`;
        }).join('\n');
      } },
    map: { help: 'map <track> <device> <param> — modulate a parameter from the macro',
      args: [A_TRACK, { name: 'device', type: 'int', min: 0 },
             { name: 'param', type: 'int', min: 0 }],
      run: (a) => (api.mapParam(Number(a[0]), Number(a[1]), Number(a[2]))
        ? `mapped dev${a[1]} param ${a[2]}` : refusal(api)) },
    unmap: { help: 'unmap <track> <link> — stop modulating',
      args: [A_TRACK, { name: 'link', type: 'int', min: 1 }],
      run: (a) => (api.unmapParam(Number(a[0]), Number(a[1]))
        ? `link ${a[1]} removed` : refusal(api)) },
    depth: { help: 'depth <track> <link> <amount> — how far the source sweeps it, 0 to 1',
      args: [A_TRACK, { name: 'link', type: 'int', min: 1 },
             { name: 'amount', type: 'num', min: 0, max: 1 }],
      run: (a) => (api.modDepth(Number(a[0]), Number(a[1]), Number(a[2]))
        ? `link ${a[1]} depth ${a[2]}` : refusal(api)) },
    macro: { help: 'macro <track> <device> <value> — turn a macro knob, 0 to 1',
      args: [A_TRACK, { name: 'device', type: 'int', min: 0 },
             { name: 'value', type: 'num', min: 0, max: 1 }],
      // The knob itself. Separate from `map` because turning it is the ORDINARY action —
      // mapping happens once and turning happens all afternoon.
      run: (a) => (api.macro(Number(a[0]), Number(a[1]), Number(a[2]))
        ? `macro ${a[2]}` : refusal(api)) },
    /*
     * THE SPINE: named ticks, and the one command that moves music.
     *
     * A MARKER IS A NAMED TICK AND STORES NO LENGTH. Two adjacent markers are a span, so a
     * section's "length" is the next marker's tick minus this one's — which means the four
     * marker ops are TOTAL: they move nothing and can fail only on a bad id.
     *
     * `time` is the other half and is deliberately a DIFFERENT command with a different name.
     * It moves everything at or after a tick — every placement on every track, the tempo points,
     * the key changes, the automation points, the meter points and the later markers — in one
     * transaction the engine refuses whole and undoes whole. Editing a label used to do that as
     * a side effect, with no undo entry big enough to hold it.
     */
    markers: { help: 'markers — the song\u2019s named ticks, bar by bar', args: NONE,
      run: () => {
        const m = api.markers();
        if (!m) return 'no engine';
        if (!m.count) return 'no markers \u2014 `marker 0 intro` names the start of the song';
        const rows = m.list.map((x) =>
          `${x.id}  bar ${x.bar}.${x.beat}  ${x.bars ? x.bars + 'b' : '\u2192 end'}  ${x.name}`);
        // Truncation is REPORTED. A short list that says nothing reads as the whole song.
        if (m.truncated) rows.push(`\u2026 and ${m.truncated} more the engine could not publish`);
        return rows.join('\n');
      } },
    marker: { help: 'marker <tick> [name] \u2014 name a point in the song',
      args: [{ name: 'tick', type: 'int', min: 0 },
             { name: 'name', type: 'text', rest: true, optional: true }],
      run: (a) => {
        const name = a.slice(1).join(' ');
        return api.addMarker(Number(a[0]), name)
          ? `${name || 'Marker'} at ${a[0]}` : refusal(api);
      } },
    delmarker: { help: 'delmarker <id> \u2014 unname the point; the music stays put',
      args: [{ name: 'id', type: 'int', min: 1 }],
      run: (a) => (api.delMarker(Number(a[0])) ? `marker ${a[0]} removed` : refusal(api)) },
    namemarker: { help: 'namemarker <id> <name> \u2014 rename a marker',
      args: [{ name: 'id', type: 'int', min: 1 },
             { name: 'name', type: 'text', rest: true }],
      run: (a) => {
        // `a.slice(1).join(' ')`, not `a[1]`: a `rest` argument arrives as the separate words it
        // was typed as, and taking the first renamed one to "VERSE" when "VERSE A" was typed.
        const name = a.slice(1).join(' ');
        return api.nameMarker(Number(a[0]), name) ? `marker ${a[0]}: ${name}` : refusal(api);
      } },
    movemarker: { help: 'movemarker <id> <tick> \u2014 move the marker ALONE; no music follows',
      args: [{ name: 'id', type: 'int', min: 1 }, { name: 'tick', type: 'int', min: 0 }],
      run: (a) => (api.moveMarker(Number(a[0]), Number(a[1]))
        ? `marker ${a[0]} to ${a[1]}` : refusal(api)) },
    time: { help: 'time <tick> <bars> \u2014 insert bars of time (negative removes); everything after moves',
      args: [{ name: 'tick', type: 'int', min: 0 },
             { name: 'bars', type: 'int', min: -9999, max: 9999 }],
      run: (a) => {
        const bars = Number(a[1]);
        return api.insertTime(Number(a[0]), bars)
          ? `${bars > 0 ? 'inserted' : 'removed'} ${Math.abs(bars)} bars at ${a[0]} \u2014 `
            + 'everything after it moved, and `undo` puts it back'
          : refusal(api);
      } },
    timesig: { help: 'timesig <sig> [tick] \u2014 the meter from a point, like 7/8',
      args: [{ name: 'sig', type: 'text' },
             { name: 'tick', type: 'int', min: 0, optional: true }],
      // Bar NUMBERING is a prefix sum through the meter map, which is why the engine resolves
      // each marker's bar rather than publishing a tick for the client to divide.
      run: (a) => (api.setTimeSig(a[0], a[1] === undefined ? 0 : Number(a[1]))
        ? `${a[0]} from ${a[1] === undefined ? 'the start' : 'tick ' + a[1]}` : refusal(api)) },

    /*
     * Pull a lane toward a grid — NON-DESTRUCTIVELY. Nothing on disk moves: the
     * engine applies this to a separate scheduling copy, so the authored tick is
     * still what is stored, saved and drawn, and only where the note SOUNDS
     * changes. That is what makes it worth having over a destructive quantize,
     * which throws the performance away on the first pass with no way back.
     *
     * PERCENT, not thousandths. The wire wants 0..1000 and a person thinks in
     * 0..100; converting at this boundary keeps the engine's units out of the
     * hands of anyone typing. Swing is signed here and biased once, in Rust.
     */
    quantize: { help: 'quantize <track> <off|1/4|1/8|1/16|1/32|1/4t|1/8t|1/16t> [strength] [swing]'
                    + ' — pull a lane toward a grid; nothing moves on disk',
      args: [A_TRACK, oneOf(GRID_NAMES),
             { name: 'strength', type: 'int', min: 0, max: 100, optional: true },
             { name: 'swing', type: 'int', min: -50, max: 50, optional: true }],
      run: (a) => {
        const grid = QUANTIZE_GRIDS[a[1]];
        const strength = a[2] === undefined ? 100 : Number(a[2]);
        const swing = a[3] === undefined ? 0 : Number(a[3]);
        if (!api.quantize(Number(a[0]), grid, strength * 10, swing * 10)) return 'no engine';
        return grid === 0 ? 'lane ' + a[0] + ' unquantized'
          : `lane ${a[0]} ${a[1]} at ${strength}%` + (swing ? ` swing ${swing}%` : '');
      } },
    // Open a plugin's own window. The engine has accepted OpenPluginEditor since
    // before this UI existed and nothing ever sent it.
    editor: { help: 'editor <track> <device> — open the plugin\'s own window',
      args: [A_TRACK, { name: 'device', type: 'int', min: 0 }],
      run: (a) => (api.openEditor(Number(a[0]), Number(a[1])) ? 'opened' : 'no engine') },
    load: { help: 'load <project>', args: [{ name: 'project', type: 'text' }],
      run: (a) => { api.load(a[0]); return 'loading ' + a[0]; } },
    save: { help: 'save <project>', args: [{ name: 'project', type: 'text' }],
      run: (a) => { api.save(a[0]); return 'saving ' + a[0]; } },
    projects: { help: 'list projects on disk', args: NONE,
      run: () => { api.listProjects(); return 'listing…'; } },
    play: { help: 'play/pause toggle', args: NONE, run: () => { api.transport('play'); return 'toggle'; } },
    stop: { help: 'stop and rewind', args: NONE, run: () => { api.transport('stop'); return 'stop'; } },
    seek: { help: 'seek <tick>', args: [{ name: 'tick', type: 'int', min: 0 }],
      run: (a) => { const t = Number(a[0]); api.seek(t); return 'seek ' + t; } },
    // Two arities on purpose: `tempo 128` is the whole song, `tempo 128 <tick>`
    // is one point from there on. Omitting the position is what means "all of
    // it" — `tempo 128 0` replaces the point at bar 1 and leaves later tempo
    // changes standing, which is a different edit and a person means it.
    tempo: { help: 'tempo <bpm> [tick] — whole song, or one point from <tick>',
      args: [{ name: 'bpm', type: 'num', min: MIN_BPM },
             { name: 'tick', type: 'int', min: 0, optional: true }],
      run: (a) => {
        const bpm = Number(a[0]);
        api.tempo(bpm, a[1] === undefined ? undefined : num(a[1]));
        return 'tempo ' + bpm + (a[1] === undefined ? ' (whole song)' : ' from ' + num(a[1]));
      } },
    // A duration of zero ticks is a note nothing can hear and nothing can select
    // — the silent no-op in note form — so the floor is one tick, not zero.
    note: { help: 'note <pitch> [dur] [vel] — at the cursor',
      args: [{ name: 'pitch', type: 'int', min: 0, max: MIDI_MAX },
             { name: 'dur', type: 'int', min: 1, optional: true },
             { name: 'vel', type: 'int', min: 0, max: MIDI_MAX, optional: true }],
      run: (a) => {
        const p = Number(a[0]);
        api.note(p, num(a[1], undefined), num(a[2], undefined));
        return 'note ' + p;
      } },
    del: { help: 'delete the note at the cursor', args: NONE,
      run: () => { api.del(); return 'delete'; } },
    goto: { help: 'goto <row> [track]',
      args: [{ name: 'row', type: 'int', min: 0 }, A_TRACK_OPT],
      run: (a) => {
        api.goto(Number(a[0]), a[1] === undefined ? undefined : num(a[1]));
        return 'cursor ' + Number(a[0]) + (a[1] !== undefined ? ' t' + a[1] : '');
      } },
    // The zoom levels are a table, so the ceiling comes from the table. A number
    // written here would be right until someone adds a level.
    zoom: { help: 'zoom <index>',
      args: [{ name: 'index', type: 'int', min: 0, max: ZOOM_LEVELS.length - 1 }],
      run: (a) => { const z = Number(a[0]); api.zoom(z); return 'zoom ' + z; } },
    oct: { help: 'oct <n>', args: [{ name: 'n', type: 'int', min: 0, max: 9 }],
      run: (a) => { const o = Number(a[0]); api.octave(o); return 'octave ' + o; } },
    gain: { help: 'gain <track> <dB>',
      args: [A_TRACK, { name: 'dB', type: 'num', min: -96, max: 12 }],
      run: (a) => {
        const t = Number(a[0]), db = Number(a[1]);
        api.gain(t, db); return 'gain t' + t + ' ' + db + 'dB';
      } },
    mute: { help: 'mute <track>', args: [A_TRACK],
      run: (a) => { const t = Number(a[0]); api.strip(t, 'mute'); return 'mute t' + t; } },
    // Collapse is a VIEW decision, and it still gets a command: hard requirement 4
    // is that an agent can drive the UI, and a fold reachable only by a keystroke
    // is a fold an agent cannot reach. The op-registry test enforces exactly this.
    // Edit mode: whether a note key writes or plays. A command as well as a key
    // so it is nameable — a mode you can only reach by a keystroke is a mode you
    // cannot ask about, and Escape is not a discoverable place to look for it.
    edit: { help: 'edit [on|off] — whether note keys write', args: [ON_OFF],
      run: (a) => (api.edit(a[0] === undefined ? undefined : a[0] === 'on') ? 'edit on' : 'edit off') },
    fold: { help: 'fold <track> — hide a parent\'s child tracks', args: [A_TRACK],
      run: (a) => { const t = Number(a[0]); return api.fold(t) ? 'fold ' + t : 'not a parent'; } },
    solo: { help: 'solo <track>', args: [A_TRACK],
      run: (a) => { const t = Number(a[0]); api.strip(t, 'solo'); return 'solo t' + t; } },
    state: { help: 'dump UI state', args: NONE, run: () => JSON.stringify(api.state()) },
    engine: { help: 'dump engine state', args: NONE, run: () => JSON.stringify(api.engine()) },
    undo: { help: 'undo', args: NONE, run: () => { api.transport('undo'); return 'undo'; } },
    redo: { help: 'redo', args: NONE, run: () => { api.transport('redo'); return 'redo'; } },
    // Bare `follow` toggles; the word is how you ask for a particular state. Any
    // other word used to read as "on", so `follow of` quietly did the opposite.
    follow: { help: 'follow [on|off] — keep the playhead in view',
      args: [oneOf(['on', 'off'], true)],
      run: (a) => 'follow ' + (api.follow(a[0] === undefined ? undefined : a[0] !== 'off') ? 'on' : 'off') },
    // Harmony was readable everywhere and writable nowhere. `root` is a pitch
    // class (0=C), `scale` a name from the engine's own table — `Major`,
    // `Minor`, `Dorian`, `Mixolydian` — or its id, so nothing here keeps a
    // second list to fall out of step.
    harmony: { help: 'harmony <root> <scale> [tick] — set the key from here on',
      args: [{ name: 'root', type: 'int', min: 0, max: 11 },
             { name: 'scale', type: 'text' },
             { name: 'tick', type: 'int', min: 0, optional: true }],
      run: (a) => (api.harmony(Number(a[0]), a[1], a[2] === undefined ? 0 : Number(a[2]))
                   ? `key set` : 'refused') },
    /*
     * ...and taking one off. A timeline you can only add to is one you stop using, and
     * that is what this was: the engine has taken DeleteHarmony since before this UI
     * existed and nothing sent it.
     *
     * By TICK, which is what the engine matches on. `tick` defaults to 0 because the
     * event at 0 is the song's key and the one most likely to be wrong first.
     */
    delharmony: { help: 'delharmony [tick] — remove the key change at a tick',
      args: [{ name: 'tick', type: 'int', min: 0, optional: true }],
      run: (a) => (api.deleteHarmony(a[0] === undefined ? 0 : Number(a[0]))
                   ? 'removed' : 'no engine') },
    /*
     * PLACEMENTS. The arrangement's drag, said in words.
     *
     * Every one of these takes the placement id explicitly rather than acting on
     * the selection. The rack's `remove-track` note applies: a destructive or
     * repositioning command that means "whatever happens to be selected" is the
     * one place guessing is expensive, and `clips` prints the ids.
     */
    clips: { help: 'clips — list the placements in the arrangement',
      args: [],
      run: () => {
        const cs = api.clips();
        if (!cs.length) return 'no clips';
        // Ticks are unreadable; bars are what the ruler shows. Length in bars
        // too, so "4" reads as four bars rather than 3840000.
        const bar = api.ticksPerBar();
        return cs.map((c) => `${c.id}: tr${c.track} bar ${(c.at / bar + 1).toFixed(2)}`
                           + ` len ${(c.len / bar).toFixed(2)} ${c.name || ''}`.trimEnd())
                 .join('\n');
      } },
    'move-clip': { help: 'move-clip <id> <track> <bar> [toTrack] — 1-based bar',
      args: [{ name: 'id', type: 'int', min: 0 }, A_TRACK,
             { name: 'bar', type: 'num', min: 1 },
             { name: 'toTrack', type: 'int', min: 0, max: 63, optional: true }],
      run: (a) => (api.moveClip(+a[0], +a[1], (+a[2] - 1) * api.ticksPerBar(),
                                a[3] === undefined ? undefined : +a[3])
                   ? 'moved' : 'refused') },
    // Either edge, or both. Omitting one leaves it alone, which is what makes a
    // one-edge trim a single command — see the sidecar's ResizePlacement.
    'trim-clip': { help: 'trim-clip <id> <track> [bar] [bars] — omit either to leave that edge',
      args: [{ name: 'id', type: 'int', min: 0 }, A_TRACK,
             { name: 'bar', type: 'num', min: 1, optional: true },
             { name: 'bars', type: 'num', min: 0, optional: true }],
      run: (a) => {
        const bar = api.ticksPerBar();
        const at = a[2] === undefined ? undefined : (+a[2] - 1) * bar;
        const len = a[3] === undefined ? undefined : +a[3] * bar;
        if (at === undefined && len === undefined) return 'trim what? give a bar, a length, or both';
        return api.trimClip(+a[0], +a[1], at, len) ? 'trimmed' : 'refused';
      } },
    'del-clip': { help: 'del-clip <id> <track>',
      args: [{ name: 'id', type: 'int', min: 0 }, A_TRACK],
      run: (a) => (api.delClip(+a[0], +a[1]) ? 'clip removed' : 'refused') },
    'add-clip': { help: 'add-clip <clip> <track> <bar> <bars> — place a clip, 1-based bar',
      args: [{ name: 'clip', type: 'int', min: 0 }, A_TRACK,
             { name: 'bar', type: 'num', min: 1 },
             { name: 'bars', type: 'num', min: 0 }],
      run: (a) => (api.addClip(+a[0], +a[1], (+a[2] - 1) * api.ticksPerBar(),
                               +a[3] * api.ticksPerBar())
                   ? 'clip placed' : 'refused') },
    // Declared as a COMMAND, not left to fall through to the agent — an
    // unrecognised word here gets sent to the model as a prompt, so "forget"
    // would otherwise be a sentence asking a model to forget something, which
    // is the one instruction it cannot carry out about itself.
    // The master has no lane, so there is no cursor position that means it. This
    // is the only way to reach the chain a global patcher lives on.
    master: { help: 'master [on|off] — show the master chain in the rack',
      args: [ON_OFF],
      run: (a) => 'rack: ' + api.master(a[0] === undefined ? undefined : a[0] === 'on') },
    forget: { help: 'forget — start a new agent conversation',
      args: [],
      run: () => (api.forget() ? 'forgetting…' : 'not connected') },
    columns: { help: 'columns <n> — how many note columns each track shows',
      args: [{ name: 'n', type: 'int', min: 1, max: 8 }],
      run: (a) => 'note columns: ' + api.noteColumns(Number(a[0])) },
    'add-track': { help: 'add-track — append a track at the end',
      args: [],
      run: () => (api.addTrack() ? 'track added' : 'refused') },
    // Takes the track explicitly rather than defaulting to the cursor. Every
    // other destructive op here names its target, and a remove that quietly
    // means "wherever the cursor happens to be" is the one command where
    // guessing is expensive — v1 has no undo for it.
    'remove-track': { help: 'remove-track <track>',
      args: [A_TRACK],
      run: (a) => (api.removeTrack(Number(a[0])) ? 'track removed' : 'refused') },
    rename: { help: 'rename <track> <name>',
      args: [A_TRACK, { name: 'name', type: 'text', rest: true }],
      run: (a) => {
        const t = Number(a[0]);
        const name = a.slice(1).join(' ');
        api.rename(t, name);
        return 'renamed t' + t + ' to ' + name;
      } },
    // <row1> is optional and always was: `select 8` selects the single row 8.
    // The prose said it was required, which is the drift this schema is for.
    select: { help: 'select <row0> [row1] [track] — a tracker range',
      args: [{ name: 'row0', type: 'int', min: 0 },
             { name: 'row1', type: 'int', min: 0, optional: true }, A_TRACK_OPT],
      run: (a) => {
        const r0 = Number(a[0]), r1 = num(a[1], r0);
        const tr = a[2] === undefined ? undefined : num(a[2]);
        return 'selected ' + api.select(r0, r1, tr) + ' note(s)';
      } },
    transpose: { help: 'transpose <semitones> — the selection',
      args: [{ name: 'semitones', type: 'int' }],
      run: (a) => {
        const n = Number(a[0]);
        // Zero is inside every range and still means nothing — a check about
        // sense, not about type, which is why the gate cannot make it.
        if (!n) throw new Error('transpose by how much?');
        api.transpose(n);
        return 'transposed ' + (n > 0 ? '+' : '') + n;
      } },
    copy: { help: 'copy the selection', args: NONE,
      run: () => (api.copy() ? 'copied' : 'nothing to copy') },
    paste: { help: 'paste at the cursor', args: NONE,
      run: () => (api.paste() ? 'pasted' : 'clipboard empty') },
    cut: { help: 'cut the selection', args: NONE, run: () => (api.cut() ? 'cut' : 'nothing to cut') },
    nodes: { help: 'list patcher nodes with their editable fields', args: NONE, run: () => {
      const ns = api.nodes();
      if (!ns.length) return 'no patcher nodes';
      return ns.map((n) => '#' + n.id + ' ' + n.type
        + (n.fields.length ? '  [' + n.fields.join(' ') + ']' : '  (no config)')).join('\n');
    } },
    // <field> is a name, not an index, and which names exist depends on the
    // node's type — so the gate checks that it is a word and `api.patch` answers
    // with the list the node actually has.
    patch: { help: 'patch <node> <field> <steps> — nudge a patcher config field',
      args: [{ name: 'node', type: 'int', min: 0 }, { name: 'field', type: 'text' },
             { name: 'steps', type: 'int' }],
      run: (a) => api.patch(Number(a[0]), a[1], Number(a[2])) },
    // A type name or its index, and `api.addNode` answers an unknown one with
    // the whole list — a better refusal than an enum here could give.
    addnode: { help: 'addnode <type> — a patcher node, by type name',
      args: [{ name: 'type', type: 'text' }], run: (a) => api.addNode(a[0]) },
    delnode: { help: 'delnode <node>', args: [{ name: 'node', type: 'int', min: 0 }],
      run: (a) => api.delNode(Number(a[0])) },
    link: { help: 'link <src> <dst> [kind] — ports are worked out from the types',
      args: [{ name: 'src', type: 'int', min: 0 }, { name: 'dst', type: 'int', min: 0 },
             { name: 'kind', type: 'int', min: 0, max: EDGE_KINDS.length - 1, optional: true }],
      run: (a) => api.linkNodes(Number(a[0]), Number(a[1]),
                                a[2] === undefined ? undefined : num(a[2])) },
    loop: { help: 'loop <fromBar> <toBar> — bars are 1-based, as on the ruler',
      args: [{ name: 'fromBar', type: 'int', min: 1 }, { name: 'toBar', type: 'int', min: 1 }],
      run: (a) => api.setLoop(Number(a[0]), Number(a[1])) },
    clear: { help: 'clear the log', args: NONE, run: (a, x) => { x.clear(); return null; } },
  };
  // Built once, from the schema rather than from the prose: the schema is the
  // half the gate reads, so a signature built from it cannot describe an
  // argument the gate does not check.
  for (const name in cmds) cmds[name].sig = signatureOf(name, cmds[name].args);
  return cmds;
}

export class Dock {
  constructor(host, api) {
    this.host = host;
    this.host.className = 'dk';
    this.api = api;
    this.commands = createCommands(api);

    /**
     * A header, so the pane says what it is.
     *
     * Jaakko, looking at the right-hand side: "is that supposed to be the agent
     * pane?" It was, and nothing on it said so — a console with an unlabelled
     * log and a bare `>` is indistinguishable from a status readout until you
     * type into it. The design names this pane; the build had not.
     *
     * It is also where the fold control goes: a cell you can collapse needs a bar
     * that survives the collapse, or there is nothing left to click to get it
     * back.
     */
    const head = div('dk-head', host);
    // Written once at construction, so no per-draw guard is owed.
    div('dk-name', head).textContent = 'AGENT';
    div('dk-hint', head).textContent = 'runs the same commands you do';

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

  /**
   * Give the keyboard back.
   *
   * `api.close()` moved `state.focus` off the dock and left the input holding DOM
   * focus, so the app believed the tracker had the keyboard while the browser
   * went on delivering every keystroke to a text field. `?` vanished into the
   * console; so did every other key the app reads. Two notions of focus that can
   * disagree are one notion too many — the same failure as a selection that looks
   * like focus and is not, from the other side.
   */
  blur() { if (document.activeElement === this.input) this.input.blur(); }

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
    /**
     * A sentence, not a command: ask the agent.
     *
     * "ask — it runs the same commands you do" is what the design puts under
     * this box, and this is the line that makes it true. The agent reaches the
     * song through the same named operations the console does, so there is one
     * grammar with two ways in rather than a second, softer interface that can
     * do things you cannot.
     *
     * Anything that IS a command still runs locally and instantly: nobody should
     * wait on a model to press play.
     */
    if (!cmd) {
      if (this.api.ask && this.api.ask(line)) {
        this.log('out', 'asking…');
      } else {
        this.log('err', 'unknown: ' + name + ' (try help)');
      }
      return;
    }
    try {
      // Through the gate, never straight at `run`: a console that validated its
      // own way would be a second opinion about the same grammar.
      this.log('out', runCommand(name, cmd, args, this));
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
