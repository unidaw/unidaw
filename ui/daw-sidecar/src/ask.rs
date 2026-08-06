//! Asking a model to operate the DAW.
//!
//! The console already turns a typed line into an engine command. This turns a
//! typed SENTENCE into a sequence of them: the prompt goes to the Messages API
//! with `daw-agent`'s tool manifest attached, the model answers with tool calls,
//! we execute them against the same shared memory the UI writes to, hand back
//! the results, and go round again until it stops asking for tools.
//!
//! WHY THE TOOLS ARE THE AGENT'S ONLY REACH. `daw-agent` exposes a manifest of
//! named operations and executes them itself; nothing here constructs a command
//! payload or touches the ring. So the model can do exactly what the manifest
//! says and nothing else, the same list a person can read, and adding a
//! capability is a deliberate act in one file rather than a prompt that happened
//! to work.
//!
//! WHY IT RUNS ON ITS OWN THREAD. A round trip to the API is hundreds of
//! milliseconds at best. The sidecar's job is to poll shared memory at 120 Hz
//! and forward frames; blocking that on a network call would stall the UI of a
//! DAW while a model thinks. The caller spawns this and gets progress back
//! through a channel.

use std::collections::VecDeque;
use std::sync::mpsc::Sender;
use std::time::Duration;

use daw_agent::{AgentSession, ToolCall};
use serde_json::{json, Value};

/// What the loop reports as it goes. The page renders these in the agent log.
#[derive(Debug, Clone)]
pub enum Progress {
    /// Prose from the model.
    Say(String),
    /// A tool the model asked for, and what came back.
    Did { tool: String, args: String, result: String, ok: bool },
    /// The turn finished.
    Done(String),
    /// The turn failed. The string is safe to show; see `scrub`.
    Failed(String),
}

/// What earlier asks left behind.
///
/// WHY AT ALL. Without this every ask starts from nothing, so "now do the same
/// to the lead" is unanswerable — the model has never heard of "the same". That
/// is not a small gap: it is the difference between a text box you issue orders
/// to and one you have a conversation with, and the second is the only one worth
/// having in a DAW, where the next instruction is nearly always a refinement of
/// the last.
///
/// WHY IT IS A LIST OF EXCHANGES AND NOT A LIST OF MESSAGES. The obvious
/// implementation — a ring buffer of messages, drop the oldest when full —
/// produces an INVALID conversation, and the API rejects it outright. A
/// `tool_use` block must be followed by its matching `tool_result`, and a
/// conversation may not begin with a bare `tool_result`. Evicting one message at
/// a time will eventually cut between the two. So the unit of eviction is a
/// whole exchange: a user sentence, every tool round it took, and the model's
/// closing prose. Any suffix of a list of those is itself a valid conversation.
///
/// WHY ONLY CLEAN ENDINGS ARE KEPT. An exchange that died on an API error or ran
/// out of tool rounds ends mid-plan, with edits half applied. Replaying that as
/// context invites the model to carry on from a state neither of us can describe
/// — and its last message may be a dangling `tool_use`, which is the invalid
/// shape again by another route. A failed ask leaves no trace.
///
/// WHY IT IS PER CONNECTION. This lives on the websocket thread, so each browser
/// tab has its own. Two tabs hold two conversations, and a reload starts fresh.
/// That matches what a chat panel looks like it does; the alternative — one
/// history shared by every tab — would have a model answering a question that
/// was asked in a window the person is not looking at.
#[derive(Default)]
pub struct History {
    /// Oldest first.
    exchanges: VecDeque<Exchange>,
}

struct Exchange {
    messages: Vec<Value>,
    bytes: usize,
}

/// How many past exchanges to carry. Enough for "do that again", "no, the other
/// one", "now the lead" — the shape a session actually takes — without turning
/// every ask into a bill for the whole afternoon.
const MAX_EXCHANGES: usize = 6;
/// And a byte ceiling, because six exchanges of a model reading `observe`
/// windows is a different size from six of "make it louder". Roughly 6k tokens.
const MAX_HISTORY_BYTES: usize = 24_000;

impl History {
    pub fn new() -> Self { Self::default() }

    pub fn is_empty(&self) -> bool { self.exchanges.is_empty() }
    pub fn len(&self) -> usize { self.exchanges.len() }

    /// Forget everything. Returns whether there was anything to forget, so the
    /// caller can say so rather than announcing a clearing that did nothing.
    pub fn clear(&mut self) -> bool {
        let had = !self.exchanges.is_empty();
        self.exchanges.clear();
        had
    }

    /// The conversation so far, as the API wants it.
    fn prefix(&self) -> Vec<Value> {
        self.exchanges.iter().flat_map(|e| e.messages.iter().cloned()).collect()
    }

    /// Keep an exchange, evicting from the front until it fits.
    fn record(&mut self, messages: Vec<Value>) {
        if messages.is_empty() { return; }
        let bytes = messages.iter().map(|m| m.to_string().len()).sum();
        self.exchanges.push_back(Exchange { messages, bytes });
        while self.exchanges.len() > MAX_EXCHANGES {
            self.exchanges.pop_front();
        }
        // Never evict to empty: a single exchange over the ceiling is still the
        // one the person is in the middle of, and dropping it would make the
        // next ask amnesiac exactly when the conversation got interesting.
        while self.exchanges.len() > 1
            && self.exchanges.iter().map(|e| e.bytes).sum::<usize>() > MAX_HISTORY_BYTES
        {
            self.exchanges.pop_front();
        }
    }
}

const API_URL: &str = "https://api.anthropic.com/v1/messages";
const API_VERSION: &str = "2023-06-01";
/// Claude Sonnet 4.5 — capable enough to plan a few edits, cheap enough to sit
/// behind a text box someone types into all day.
const MODEL: &str = "claude-sonnet-4-5";
/// How many tool round trips one prompt may take. A model that has not finished
/// by here is looping, and a DAW that keeps editing a song by itself is worse
/// than one that stops and says so.
const MAX_TURNS: usize = 12;
const MAX_TOKENS: u32 = 4096;
/// HOW LONG ONE API CALL MAY TAKE BEFORE IT IS A FAILURE RATHER THAN A WAIT.
///
/// ureq applies no timeout by default, which means a request that never answers
/// hangs the ask thread for ever. Nothing downstream recovers from that: the
/// `asking` flag stays set, so every later prompt is refused with "still working
/// on the last one", and the box is dead until the sidecar is restarted. In
/// front of an audience that is indistinguishable from the DAW ignoring you.
///
/// The read bound is generous on purpose — a full 4096-token answer with tool
/// calls legitimately takes tens of seconds, and cutting off a working request
/// would be a worse bug than the one being fixed. Connecting is a different
/// matter: if the socket has not opened in fifteen seconds it is not going to.
const CONNECT_TIMEOUT: Duration = Duration::from_secs(15);
const READ_TIMEOUT: Duration = Duration::from_secs(180);

/// The key, from the environment or the repo's .env.
///
/// Read at ASK time rather than at startup: a sidecar that has been running
/// since before the key existed should pick it up without a restart, and one
/// started without it should say so when asked rather than refusing to boot.
pub fn api_key() -> Option<String> {
    if let Ok(k) = std::env::var("ANTHROPIC_API_KEY") {
        if !k.trim().is_empty() {
            return Some(k.trim().to_string());
        }
    }
    // A named file, then the usual places. `DAW_ENV_FILE` exists because the key
    // does not have to live in this repo — the real one is in a sibling checkout, and
    // a search that walks up from the working directory can never find that. An
    // explicit path is better than a longer guess.
    //
    // Parsed by hand rather than with a crate: it is one line, and this binary
    // has the two dependencies it is allowed.
    let named = std::env::var("DAW_ENV_FILE").unwrap_or_default();
    let mut paths: Vec<String> = Vec::new();
    if !named.is_empty() { paths.push(named); }
    for p in [".env", "../.env", "../../.env"] { paths.push(p.to_string()); }
    for path in paths {
        let Ok(text) = std::fs::read_to_string(&path) else { continue };
        for line in text.lines() {
            let line = line.trim();
            if line.starts_with('#') { continue; }
            let Some((k, v)) = line.split_once('=') else { continue };
            if k.trim() == "ANTHROPIC_API_KEY" {
                let v = v.trim().trim_matches('"').trim_matches('\'');
                if !v.is_empty() { return Some(v.to_string()); }
            }
        }
    }
    None
}

/// Never let a key reach a log or a UI. ureq puts the request URL in some error
/// strings and a future refactor could put a header there too; this is cheap
/// insurance on a value that must not travel.
fn scrub(s: &str, key: &str) -> String {
    if key.is_empty() { return s.to_string(); }
    s.replace(key, "<key>")
}

/// The manifest as the API wants it: name, description, input_schema.
fn tools_json(session: &AgentSession) -> Value {
    Value::Array(
        session
            .manifest()
            .into_iter()
            .map(|t| json!({
                "name": t.name,
                "description": t.description,
                "input_schema": t.params,
            }))
            .collect(),
    )
}

/// What the model is told about the instrument it is holding.
///
/// The observation is included because a request like "make the bass louder"
/// needs to know which track is the bass, and asking the model to call a tool to
/// find out costs a round trip before it can start.
/// The half that never changes. Split out so it can carry the cache breakpoint:
/// everything up to and including a marked block is reused across turns and
/// across asks, and this text plus the tool manifest is the bulk of what gets
/// re-sent twelve times in a tool loop.
const INSTRUCTIONS: &str = "\
You are operating a digital audio workstation through its tool API. You are not \
describing what to do — the tools ARE the doing, and the person will hear the \
result.\n\n\
NEVER REPORT A CHANGE YOU HAVE NOT MADE WITH A TOOL. Asked for a kick pattern, \
one model added the track, explained that four-on-the-floor means MIDI 36 on \
every quarter, and finished with \"you can now play it back\" — over a song with \
no notes in it. Describing the music you would write is not writing it, and the \
person cannot tell the difference until they press play.\n\n\
A REQUEST FOR MUSICAL MATERIAL IS A REQUEST TO WRITE NOTES. A bassline, a beat, \
a melody, a chord part, \"something in C minor\" — all of them mean call \
`add_notes`. Adding or naming a track is preparation, not the answer; if you \
have added a track and written nothing, you have not finished. Explain briefly \
afterwards if it helps, never instead.\n\n\
THIS DOES NOT OVERRIDE ASKING. \"Write me something\" with no way to tell WHICH \
track or WHAT material is meant is still a question, not an instruction, and a \
pronoun with nothing to point at — \"now solo it\" after the conversation was \
dropped — refers to nothing and must be asked about rather than resolved to \
whatever seems likeliest. Acting on a guess is the failure this pair of rules \
is balanced between; do the work when the request is clear, ask when it is not.\n\n\
Work in small steps and check the observation after edits that matter. Prefer \
the smallest change that answers the request. If a request is ambiguous in a way \
that changes the music — which track, which bar — ask rather than guess. If a \
tool refuses, read the refusal: it names what was wrong.\n\n\
Ticks are nanoticks; there are 960000 per quarter note. Pitches are MIDI \
numbers, 60 is middle C. Track ids are stable and do not renumber when a track \
is removed.\n\n\
You are given the song's SHAPE, not its notes: each track's name, how many notes \
it has, the beats it spans and the pitch range it covers. To see actual notes, \
call `observe` with `from_beat` for the part you are working on. A track marked \
TRUNCATED has more notes than the engine publishes, so do not conclude it ends \
where the count stops.\n\n\
A track's `devices:` line lists its chain in audio order with each device's ID. \
Those IDs are what `patcher_node`, `device_params`, `set_bypass`, `remove_device` \
and `modulate` mean by `device` — take them from there rather than guessing or \
asking. A track with no `devices:` line has an empty chain.\n\n\
The shape is taken before your first tool call and is NOT refreshed as you work \
— call `observe` again after edits that matter. A device you just ADDED will not \
be in it: add it, then `observe` to learn its id.";

/// The half that does: the song as it stands, plus a warning about the past.
fn shape_block(session: &AgentSession, has_history: bool, devices: &DeviceLookup) -> String {
    // The TEXT form, and the SHAPE rather than every note.
    //
    // This used to embed the whole song as JSON — ~114 bytes per note, which is
    // 2.2 MB on a large session: past what can be sent at all, and past it
    // silently. The same song's shape is under a kilobyte and answers the
    // question the prompt above actually poses ("which track is the bass")
    // better than twenty thousand note objects do.
    /*
     * THE CHAINS ARE FILLED IN HERE, and they cannot come from `observe` itself.
     *
     * The engine publishes device chains as DIFFS on a single-consumer ring — whoever drains
     * it takes those entries away from everybody else. daw-agent attaching its own consumer
     * would silently steal half the browser's chain updates, which is a rack that draws a
     * device short and never corrects itself. The sidecar's drainer already accumulates them;
     * this asks that accumulation rather than competing with it.
     */
    let mut obs = session.observe();
    for t in 0..obs.tracks.len() {
        let track_id = obs.tracks[t].track_id;
        let list = devices(track_id);
        if !list.is_empty() {
            obs.attach_devices(track_id, list);
        }
    }
    let obs = obs.to_text();
    let stale = if has_history {
        // Said out loud because the two contexts disagree by design. Earlier
        // turns describe the song at the moment they were spoken; the person has
        // very likely edited it by hand since, and nothing replays those edits
        // into the transcript. Without this line a model reading "track 2 has 8
        // notes" three exchanges up will believe it over the shape below.
        "\nEarlier turns in this conversation describe the song AS IT WAS THEN. \
         The person has been editing it by hand in between. Where the two \
         disagree, the shape below is what is true now.\n"
    } else {
        ""
    };
    format!("{stale}\n{obs}")
}

/// Mark a message as a cache breakpoint.
///
/// Everything before it — the tools, the system prompt, every earlier message —
/// is billed at a tenth and read back instead of re-sent. In a twelve-turn tool
/// loop the same prefix goes over the wire twelve times, so this is most of the
/// cost of an ask.
fn mark_cacheable(msg: &mut Value) {
    let bp = json!({ "type": "ephemeral" });
    match &mut msg["content"] {
        // A plain-string content has no block to hang the marker on; promote it.
        Value::String(s) => {
            let text = std::mem::take(s);
            msg["content"] = json!([{ "type": "text", "text": text, "cache_control": bp }]);
        }
        Value::Array(blocks) => {
            if let Some(last) = blocks.last_mut() { last["cache_control"] = bp; }
        }
        _ => {}
    }
}

/// One prompt, to as many tool round trips as it takes.
/// How the caller answers "what is on track N".
///
/// A closure rather than a store, because the store is the sidecar's and this crate must not
/// grow a dependency on it just to read a list.
pub type DeviceLookup<'a> = dyn Fn(u32) -> Vec<daw_agent::DeviceView> + 'a;

pub fn run(
    session: &AgentSession,
    prompt: &str,
    tx: &Sender<Progress>,
    history: &std::sync::Mutex<History>,
    devices: &DeviceLookup,
) {
    let Some(key) = api_key() else {
        let _ = tx.send(Progress::Failed(
            "no ANTHROPIC_API_KEY — export it, put it in .env at the repo root, \
             or point DAW_ENV_FILE at the file that has it, then ask again".into(),
        ));
        return;
    };

    // Snapshot the past under a brief lock. Held across the API call it would
    // block the websocket thread's `clear` for as long as a model takes to
    // think, which is exactly when someone hits undo.
    let (mut prefix, past) = {
        let h = history.lock().unwrap_or_else(|e| e.into_inner());
        (h.prefix(), h.len())
    };
    // The history is the same bytes on every turn of this loop AND on the next
    // ask, so it is worth a breakpoint of its own.
    if let Some(last) = prefix.last_mut() { mark_cacheable(last); }

    // One agent for every turn of the loop below, so the timeouts apply to each
    // call and the connection is reused across tool rounds.
    let http = ureq::AgentBuilder::new()
        .timeout_connect(CONNECT_TIMEOUT)
        .timeout_read(READ_TIMEOUT)
        .build();

    let tools = tools_json(session);
    let system = json!([
        { "type": "text", "text": INSTRUCTIONS, "cache_control": { "type": "ephemeral" } },
        { "type": "text", "text": shape_block(session, past > 0, devices) },
    ]);

    // The running conversation. Tool results have to come back in the same
    // structure the model sent the calls in, so this accumulates rather than
    // being rebuilt per turn.
    let first_new = prefix.len();
    let mut messages: Vec<Value> = prefix;
    messages.push(json!({ "role": "user", "content": prompt }));

    for turn in 0..MAX_TURNS {
        let body = json!({
            "model": MODEL,
            "max_tokens": MAX_TOKENS,
            "system": system,
            "tools": tools,
            "messages": messages,
        });

        let resp = http.post(API_URL)
            .set("x-api-key", &key)
            .set("anthropic-version", API_VERSION)
            .set("content-type", "application/json")
            .send_json(body);

        let value: Value = match resp {
            Ok(r) => match r.into_json() {
                Ok(v) => v,
                Err(e) => {
                    let _ = tx.send(Progress::Failed(scrub(&e.to_string(), &key)));
                    return;
                }
            },
            Err(ureq::Error::Status(code, r)) => {
                // The API's own message says more than the status does — a bad
                // key, an unknown model and a rate limit are three different
                // problems and only one of them is worth retrying.
                let detail = r.into_string().unwrap_or_default();
                let msg = serde_json::from_str::<Value>(&detail)
                    .ok()
                    .and_then(|v| v["error"]["message"].as_str().map(String::from))
                    .unwrap_or(detail);
                let _ = tx.send(Progress::Failed(format!("API {code}: {}", scrub(&msg, &key))));
                return;
            }
            Err(e) => {
                let _ = tx.send(Progress::Failed(scrub(&e.to_string(), &key)));
                return;
            }
        };

        let content = value["content"].as_array().cloned().unwrap_or_default();
        let mut calls: Vec<(String, String, Value)> = Vec::new();
        for block in &content {
            match block["type"].as_str() {
                Some("text") => {
                    if let Some(t) = block["text"].as_str() {
                        if !t.trim().is_empty() { let _ = tx.send(Progress::Say(t.to_string())); }
                    }
                }
                Some("tool_use") => {
                    let id = block["id"].as_str().unwrap_or_default().to_string();
                    let name = block["name"].as_str().unwrap_or_default().to_string();
                    calls.push((id, name, block["input"].clone()));
                }
                _ => {}
            }
        }

        if calls.is_empty() {
            // No tools asked for: the model has finished.
            //
            // This is the ONE exit that records history. The turn ended with the
            // model's own prose and no dangling tool call, which is both a
            // complete thought and the only message shape the API will accept
            // back. Every other way out of this function — an API error, running
            // out of rounds — leaves a half-executed plan, and replaying that as
            // context is worse than starting clean.
            if !content.is_empty() {
                messages.push(json!({ "role": "assistant", "content": content }));
                let exchange: Vec<Value> = messages.split_off(first_new);
                let mut h = history.lock().unwrap_or_else(|e| e.into_inner());
                h.record(exchange);
            }
            // Done carries NOTHING. Every text block in this response has already
            // gone out as a `Say` a few lines above, and sending it again printed
            // the closing sentence twice — once as prose and once as the ending.
            // The turn ending is a fact about the conversation, not another thing
            // to read.
            let _ = tx.send(Progress::Done(String::new()));
            return;
        }

        // The assistant's turn goes back verbatim, tool_use blocks and all — the
        // API matches results to calls by id, and a reconstructed message loses
        // the ids.
        messages.push(json!({ "role": "assistant", "content": content }));

        let mut results: Vec<Value> = Vec::new();
        for (id, name, input) in calls {
            let call = ToolCall { tool: name.clone(), args: input.clone() };
            let out = session.execute(&call);
            // The model reads this, so a refusal has to carry its reason: an
            // `ok: false` with an empty body teaches it nothing and it will try
            // the same call again.
            let body = if out.ok {
                serde_json::to_string(&out.output).unwrap_or_else(|_| "{}".to_string())
            } else {
                json!({ "error": out.error.clone().unwrap_or_else(|| "refused".into()) })
                    .to_string()
            };
            let _ = tx.send(Progress::Did {
                tool: name,
                args: input.to_string(),
                result: body.clone(),
                ok: out.ok,
            });
            results.push(json!({
                "type": "tool_result",
                "tool_use_id": id,
                "content": body,
                "is_error": !out.ok,
            }));
        }
        messages.push(json!({ "role": "user", "content": results }));

        if turn + 1 == MAX_TURNS {
            let _ = tx.send(Progress::Failed(format!(
                "stopped after {MAX_TURNS} tool rounds — the request is either \
                 too big for one ask or the model is going in circles"
            )));
            return;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A key must never reach a log line or the UI, whatever shape the error is.
    #[test]
    fn scrub_removes_the_key() {
        let k = "sk-ant-secret";
        assert_eq!(scrub("failed with sk-ant-secret in it", k), "failed with <key> in it");
        // An empty key must not turn every string into a redaction.
        assert_eq!(scrub("nothing to hide", ""), "nothing to hide");
    }

    /// One exchange, the shape the loop actually records: a prompt, a tool
    /// round, and the model's closing prose.
    fn exchange(prompt: &str) -> Vec<Value> {
        vec![
            json!({ "role": "user", "content": prompt }),
            json!({ "role": "assistant", "content": [
                { "type": "tool_use", "id": "t1", "name": "set_mixer", "input": {} }]}),
            json!({ "role": "user", "content": [
                { "type": "tool_result", "tool_use_id": "t1", "content": "{}" }]}),
            json!({ "role": "assistant", "content": [{ "type": "text", "text": "done" }]}),
        ]
    }

    /// THE INVARIANT THIS TYPE EXISTS FOR. However much gets evicted, what is
    /// left must still be a conversation the API will accept: it may not begin
    /// with a `tool_result`, and every `tool_use` must be answered.
    ///
    /// A message-at-a-time ring buffer fails this on the second eviction, which
    /// is why eviction is per exchange.
    #[test]
    fn eviction_leaves_a_valid_conversation() {
        let mut h = History::new();
        for i in 0..(MAX_EXCHANGES * 3) {
            h.record(exchange(&format!("ask {i}")));
            let p = h.prefix();
            assert!(!p.is_empty());
            // Never starts mid-tool-call.
            assert_eq!(p[0]["role"], "user");
            assert!(p[0]["content"].is_string(), "a conversation may not open with a tool_result");
            // Every tool_use is answered by the message after it.
            for (n, m) in p.iter().enumerate() {
                let uses: Vec<&str> = m["content"].as_array().map(|bs| bs.iter()
                    .filter(|b| b["type"] == "tool_use")
                    .filter_map(|b| b["id"].as_str()).collect()).unwrap_or_default();
                if uses.is_empty() { continue; }
                let next = p.get(n + 1).expect("a tool_use must not be the last message");
                for id in uses {
                    assert!(next["content"].as_array().unwrap().iter().any(
                        |b| b["type"] == "tool_result" && b["tool_use_id"] == id),
                        "tool_use {id} lost its result");
                }
            }
        }
        assert_eq!(h.len(), MAX_EXCHANGES, "the count ceiling holds");
    }

    /// A single enormous exchange is kept whole rather than evicted to nothing:
    /// it is the one the person is in the middle of.
    #[test]
    fn one_oversized_exchange_survives() {
        let mut h = History::new();
        h.record(exchange(&"x".repeat(MAX_HISTORY_BYTES * 2)));
        assert_eq!(h.len(), 1);
        // And the next one pushes it out rather than both being kept.
        h.record(exchange("small"));
        assert_eq!(h.len(), 1);
        assert_eq!(h.prefix()[0]["content"], "small");
    }

    /// A failed ask records nothing, so `clear` on an empty history reports that
    /// it did nothing rather than announcing a clearing to the log.
    #[test]
    fn clear_reports_whether_it_did_anything() {
        let mut h = History::new();
        assert!(!h.clear());
        h.record(exchange("hello"));
        assert!(h.clear());
        assert!(h.is_empty());
    }

    /// The breakpoint has to land on a block, and a user message's content is a
    /// bare string — promoting it is the only way to mark it.
    #[test]
    fn cache_marker_lands_on_both_content_shapes() {
        let mut s = json!({ "role": "user", "content": "hello" });
        mark_cacheable(&mut s);
        assert_eq!(s["content"][0]["text"], "hello");
        assert_eq!(s["content"][0]["cache_control"]["type"], "ephemeral");

        let mut a = json!({ "role": "assistant", "content": [
            { "type": "text", "text": "one" }, { "type": "text", "text": "two" }]});
        mark_cacheable(&mut a);
        assert!(a["content"][0]["cache_control"].is_null(), "only the last block");
        assert_eq!(a["content"][1]["cache_control"]["type"], "ephemeral");
    }

    /// The env var wins over the file, so a caller can override without editing
    /// anything on disk.
    #[test]
    fn env_key_wins() {
        std::env::set_var("ANTHROPIC_API_KEY", "from-env");
        assert_eq!(api_key().as_deref(), Some("from-env"));
        std::env::remove_var("ANTHROPIC_API_KEY");
    }
}
