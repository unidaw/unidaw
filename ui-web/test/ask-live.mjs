/**
 * The agent, against the real API.
 *
 * Everything else about `ask` can be tested without a network: the manifest, the
 * tool execution, the history's eviction rules, the key handling. What CANNOT be
 * faked is whether the thing works — whether a model handed this observation and
 * these tools does what a person meant. That question has one honest test and it
 * costs a few cents.
 *
 * SKIPS WITHOUT A KEY, loudly. A suite that silently passes when it did not run
 * is worse than one that fails: it is the reason `ask` shipped with four tools
 * reporting `applied: false` on every call.
 *
 *   DAW_ENV_FILE=~/src/daw/.env node test/ask-live.mjs
 */

import { startStack } from './stack.mjs';   // WebSocket is a Node global since 22.

const KEY = process.env.ANTHROPIC_API_KEY || process.env.DAW_ENV_FILE;
if (!KEY) {
  console.log('SKIPPED — no ANTHROPIC_API_KEY and no DAW_ENV_FILE. This suite is the '
              + 'only check that the agent actually works; run it before shipping '
              + 'anything that touches ask.rs.');
  process.exit(0);
}

let pass = 0, fail = 0;
const check = (ok, what, detail) => {
  if (ok) { pass++; console.log('  PASS ', what); }
  else { fail++; console.log('  FAIL ', what, detail === undefined ? '' : `— ${detail}`); }
};

/**
 * One ask, start to finish.
 *
 * Returns every line the sidecar sent: the prose, the tool calls with their
 * results, and how it ended. The assertions below read the TOOL CALLS rather
 * than the prose — a model saying "I turned it down" is not evidence, and the
 * whole point of this interface is that the tools are the doing.
 */
function ask(ws, text, ms = 90000) {
  return new Promise((resolve, reject) => {
    const lines = [];
    const timer = setTimeout(() => { done(new Error(`ask timed out: ${text}`)); }, ms);
    const onMsg = (ev) => {
      const raw = typeof ev.data === 'string' ? ev.data : '';
      if (raw.indexOf('"agent"') < 0) return;
      let a = null;
      try { a = JSON.parse(raw); } catch { return; }
      if (!a || !a.agent) return;
      lines.push(a);
      if (a.agent === 'done' || a.agent === 'failed') done(null);
    };
    function done(err) {
      clearTimeout(timer);
      ws.removeEventListener('message', onMsg);
      err ? reject(err) : resolve(lines);
    }
    ws.addEventListener('message', onMsg);
    ws.send(JSON.stringify({ type: 'ask', text }));
  });
}

const calls = (lines) => lines.filter((l) => l.agent === 'did');
const said = (lines) => lines.filter((l) => l.agent === 'say').map((l) => l.text).join('\n');
const failed = (lines) => lines.find((l) => l.agent === 'failed');
/** A tool call by name, with its arguments parsed. */
function callTo(lines, tool) {
  const l = calls(lines).find((c) => c.text === tool);
  if (!l) return null;
  const arrow = (l.detail || '').indexOf(' -> ');
  try { return JSON.parse((l.detail || '').slice(0, arrow)); } catch { return {}; }
}

const stack = await startStack();
const ws = new WebSocket(`ws://127.0.0.1:${stack.base + 2}`);
await new Promise((r, j) => {
  ws.addEventListener('open', r);
  ws.addEventListener('error', () => j(new Error('cmd socket refused')));
});

try {
  /*
   * A SIX-TRACK FIXTURE, and that is load-bearing.
   *
   * The first draft of this ran against `addtrack`, which has ONE track. Every
   * check passed. Three of them were passing because "now do the same to track
   * 1" names a track that does not exist, so the model asked a question — which
   * is exactly what the negative controls below look for. The controls proved
   * nothing: they would have passed with the history feature deleted.
   *
   * Here every track referred to exists, so the only reason to ask is the one
   * being tested — that "the same" no longer refers to anything.
   */
  ws.send(JSON.stringify({ type: 'load', name: 'webtest' }));
  await new Promise((r) => setTimeout(r, 1200));

  console.log('\nnaming things');
  {
    const l = await ask(ws, 'how many tracks are there, and what are they called? '
                          + 'do not change anything');
    check(!failed(l), 'the ask completed', failed(l) && failed(l).text);
    // The shape is in the system prompt, so this needs NO tool call at all. That
    // is the point of putting it there: the commonest question a person asks is
    // answered before the model has spent a round trip.
    check(calls(l).length === 0, 'answered from the shape, no tool call',
          calls(l).map((c) => c.text).join(','));
    check(/\btrack/i.test(said(l)), 'said something about tracks');
  }

  // ── HISTORY. The second sentence is meaningless on its own. ─────────────────
  console.log('\nconversation');
  {
    // Asserted, not assumed. A setup step that quietly failed would leave the
    // history empty and make the real check below fail for the wrong reason —
    // which is how the fixture bug above hid for a whole run.
    const setup = await ask(ws, 'set track 0 to -4 dB');
    const s0 = callTo(setup, 'set_mixer');
    check(s0 && s0.track === 0 && Math.abs(s0.gain_db - (-4)) < 0.01,
          'setup: track 0 set to -4 dB', JSON.stringify(s0));
    // No track named, no amount named, no operation named. Everything this needs
    // is in the previous exchange. Before history this could only fail.
    const l = await ask(ws, 'now do the same to track 1');
    check(!failed(l), 'the follow-up completed', failed(l) && failed(l).text);
    const m = callTo(l, 'set_mixer');
    check(m !== null, 'it called set_mixer without being told to',
          calls(l).map((c) => c.text).join(','));
    check(m && m.track === 1, 'on track 1', m && JSON.stringify(m));
    check(m && Math.abs(m.gain_db - (-4)) < 0.01, 'at -4 dB, carried from the last ask',
          m && JSON.stringify(m));
  }

  /*
   * ── FORGETTING ─────────────────────────────────────────────────────────────
   *
   * A NEGATIVE CONTROL HAS TO BE UNANSWERABLE WITHOUT THE THING IT TESTS.
   *
   * The first version cleared the history and then asked "now do the same to
   * track 2", expecting the model to be lost. It was not lost, and it was not
   * remembering either: tracks 0 and 1 were sitting at -4 dB from the exchanges
   * above, the shape says so, and "the same" is recoverable from that. The
   * control was reading a fact the SONG supplied and scoring it as memory.
   *
   * So the referent here is a pronoun for something never written down: which
   * track was named in a previous answer. Nothing in the shape records that a
   * question was asked, let alone what it was answered with. "It" resolves from
   * the transcript or not at all — which is the whole claim under test.
   *
   * Proven in both directions on the same sentence: with the transcript it acts,
   * without it it asks.
   */
  console.log('\nforgetting');
  {
    await ask(ws, 'which single track has the highest-pitched notes? '
                + 'answer with its number and change nothing');
    const yes = await ask(ws, 'now solo it');
    check(callTo(yes, 'set_mixer') !== null,
          'with the transcript, "it" resolves and it acts',
          calls(yes).map((c) => c.text).join(','));

    ws.send(JSON.stringify({ type: 'forget' }));
    await new Promise((r) => setTimeout(r, 300));

    const no = await ask(ws, 'now solo it');
    check(callTo(no, 'set_mixer') === null,
          'without it, the same sentence edits nothing',
          JSON.stringify(callTo(no, 'set_mixer')));
    check(/\?|which|what|clarif|not sure|unclear|don't know|do not know|no prior|previous/i
            .test(said(no)),
          'and says so', said(no).slice(0, 200));
  }

  // ── A DIFFERENT SONG clears the conversation on its own. ───────────────────
  //
  // Same pronoun, same reason. A load has to do what `forget` does, because the
  // transcript is about a document that is no longer open.
  console.log('\nloading forgets too');
  {
    await ask(ws, 'which single track has the lowest-pitched notes? '
                + 'answer with its number and change nothing');
    let noted = false;
    const onMsg = (ev) => {
      const raw = typeof ev.data === 'string' ? ev.data : '';
      if (raw.indexOf('"note"') >= 0 && raw.indexOf('song changed') >= 0) noted = true;
    };
    ws.addEventListener('message', onMsg);
    ws.send(JSON.stringify({ type: 'load', name: 'maximal' }));
    await new Promise((r) => setTimeout(r, 1500));
    ws.removeEventListener('message', onMsg);
    check(noted, 'loading said the conversation was dropped');
    const l = await ask(ws, 'now solo it');
    check(callTo(l, 'set_mixer') === null,
          'and the model no longer knows what "it" was',
          JSON.stringify(callTo(l, 'set_mixer')));
  }
} finally {
  try { ws.close(); } catch {}
  stack.stop();
}

console.log(`\n${fail === 0 ? `ALL PASS (${pass} checks)` : `${fail} of ${pass + fail} FAILED`}`);
process.exit(fail === 0 ? 0 : 1);
