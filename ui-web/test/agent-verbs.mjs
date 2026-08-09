/*
 * THE AGENT'S TOOLS, DRIVEN AGAINST A LIVE ENGINE — the surface that had no test.
 *
 * daw-cli is driven by cli-verbs, the browser by Playwright, and the agent only by ask.rs, which
 * needs an API key, costs money per run, and is excluded from the sweep. So the tools that tell a
 * model whether its edit landed were themselves the least verified thing in the repo — which is a
 * poor joke, given that task #54 was about those tools claiming success for refused edits.
 *
 * I recorded twice that this could not be fixed without a model. That was wrong: daw_agent::execute
 * is public and the harness's Decider trait documents its test implementation as "a fixed script".
 * The only missing piece was an EngineHandle inside something a suite can run, which is all
 * `daw-agent-run` is.
 *
 * WHAT THIS ASSERTS, and why it is the same list cli-verbs pins for the CLI: that a REFUSED edit
 * comes back ok:false with a reason a reader can act on. Both surfaces share one journal reader
 * (daw_bridge::journal), so the interesting failures are the per-surface wiring — the op names,
 * the scope, and whether the result reaches the caller at all.
 */
import { execFileSync } from 'node:child_process';
import { existsSync, readFileSync } from 'node:fs';
import { resolve, join } from 'node:path';
import { startStack } from './stack.mjs';

const ROOT = resolve(new URL('../..', import.meta.url).pathname);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const stack = await startStack({ numBlocks: 8, keepDir: true });

let pass = 0;
const fails = [];
const check = (ok, what, detail = '') => {
  if (ok) { pass += 1; console.log(`  PASS  ${what}`); }
  else { fails.push(what); console.log(`  FAIL  ${what}${detail ? ` — ${detail}` : ''}`); }
};

/** One agent tool call. Returns the parsed ToolResult, never throws. */
const tool = (name, args = {}) => {
  const bin = join(ROOT, 'ui/target/release/daw-agent-run');
  const env = { ...process.env, DAW_UI_SHM_NAME: stack.shm, DAW_PROJECT_DIR: stack.dir };
  try {
    const out = execFileSync(bin, [name, JSON.stringify(args)],
                             { env, encoding: 'utf8', timeout: 20000 });
    return { exit: 0, ...JSON.parse(out) };
  } catch (e) {
    const out = String(e.stdout || '').trim();
    // exit 1 means the tool reported not-ok, and the payload is still on stdout — that is the
    // case under test, not an error in the harness.
    try { return { exit: 1, ...JSON.parse(out) }; }
    catch { return { exit: 2, ok: false, error: out + String(e.stderr || e.message || '') }; }
  }
};

const cli = (...args) => {
  try {
    return execFileSync(join(ROOT, 'ui/target/release/daw-cli'), args,
      { env: { ...process.env, DAW_UI_SHM_NAME: stack.shm, DAW_PROJECT_DIR: stack.dir },
        encoding: 'utf8', timeout: 20000 });
  } catch (e) { return String(e.stdout || '') + String(e.stderr || ''); }
};

const saved = async (tag) => {
  cli('do', 'save', tag);
  const p = `${stack.dir}/${tag}.uniproj.json`;
  for (let i = 0; i < 40; i++) {
    if (existsSync(p)) { try { return JSON.parse(readFileSync(p, 'utf8')); } catch { /* mid-write */ } }
    await sleep(150);
  }
  return null;
};

console.log('\n[the manifest, which needs no engine]');
/*
 * `|| true` was here for a moment as a placeholder, which would have made this check incapable of
 * failing — the exact shape this repo keeps re-learning. It asserts something now: the manifest
 * lists tools, and lists the ones the checks below actually call.
 */
const tools = (() => {
  try {
    return execFileSync(join(ROOT, 'ui/target/release/daw-agent-run'), ['--tools'],
                        { encoding: 'utf8', timeout: 20000 }).trim().split('\n');
  } catch (e) { return [String(e.stderr || e.message || '')]; }
})();
check(tools.length > 40 && tools.includes('sampler_slot') && tools.includes('load_sample'),
      'the manifest lists the tools these checks drive — and needs no engine to say so',
      `${tools.length} tools: ${tools.slice(0, 4).join(',')}...`)
const obs = tool('observe');
check(obs.ok === true, 'observe answers — the runner is attached to a live engine',
      JSON.stringify(obs).slice(0, 140));

/*
 * A SAMPLER, BUILT WITH THE PRODUCT'S OWN VERBS. The stack's default project puts a vst_instrument
 * at the head of track 0's chain, and a track takes ONE head-of-chain instrument — so the VST has
 * to go before a sampler can arrive. Learned by watching add_device get refused for exactly that
 * reason, which is the refusal working.
 */
console.log('\n[a sampler to aim at]');
check(tool('remove_device', { track: 0, device: 1 }).ok === true, 'the vst comes off track 0');
check(tool('add_device', { track: 0, kind: 'sampler' }).ok === true, 'a sampler goes on');
const doc = await saved('agentverbs_a');
const dev = (doc?.tracks?.find((t) => t.track_id === 0)?.device_chain || [])
  .find((d) => /sampler/i.test(d.kind || ''));
// `device_id`, not `id` — the saved schema's own spelling, read rather than guessed.
check(!!dev, 'and the saved project holds it, so later calls have a real id to use',
      JSON.stringify((doc?.tracks?.[0]?.device_chain || []).map((d) => d.kind)));
const devId = dev?.device_id;

console.log('\n[a refused edit must come back refused]');
/*
 * THE WHOLE POINT OF #54. Before the wiring, every one of these answered {"sent": true} and a model
 * would have believed it. 4242 rather than a merely-stale id, so the refusal cannot be a race.
 */
const badSlot = tool('sampler_slot', { track: 0, device: devId, slot: 4242, field: 'root', value: 60 });
check(badSlot.ok === false && /no slot with that id/.test(badSlot.error || ''),
      'A REFUSED SAMPLER EDIT IS AN ERROR WITH A REASON, not {"sent": true}',
      JSON.stringify(badSlot).slice(0, 160));

const badLoad = tool('load_sample', { track: 0, device: devId, file: 'no_such_sample.wav', root: 60 });
check(badLoad.ok === false && /would not load/.test(badLoad.error || ''),
      'A SAMPLE THAT CANNOT LOAD IS REPORTED — the #50 refusal reaches the agent too',
      JSON.stringify(badLoad).slice(0, 160));

console.log('\n[and an edit that IS applied must not be reported as refused]');
/*
 * THE CONTROL WITHOUT WHICH THE TWO ABOVE PROVE NOTHING: a waiter that reported everything as
 * refused would pass both. This is the same edit shape, differing only in that the slot exists.
 */
const kit = tool('sampler_kit', { track: 0 });
// `output.slots[].slot`, read from sampler_kit's own json! rather than guessed. The first draft
// said `.id`, which would have left slotId undefined and quietly skipped the control below —
// a check that does not run looks exactly like a check that passed.
const slotId = kit.output?.slots?.[0]?.slot;
check(slotId !== undefined, 'the kit names a slot that exists', JSON.stringify(kit).slice(0, 160));
if (slotId !== undefined) {
  const good = tool('sampler_slot', { track: 0, device: devId, slot: slotId, field: 'root', value: 72 });
  check(good.ok === true, 'A REAL SLOT EDIT SUCCEEDS — the refusal path is not swallowing everything',
        JSON.stringify(good).slice(0, 160));
}

console.log(`\n${fails.length ? `FAILURES (${pass + fails.length} checks, ${fails.length} failed)`
                               : `ALL PASS (${pass} checks)`}`);
await stack.stop();
process.exit(fails.length ? 1 : 0);
