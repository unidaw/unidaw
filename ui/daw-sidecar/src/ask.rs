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

use std::sync::mpsc::Sender;

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
    // does not have to live in this repo — Jaakko's is in a sibling checkout, and
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
fn system_prompt(session: &AgentSession) -> String {
    // The TEXT form, and the SHAPE rather than every note.
    //
    // This used to embed the whole song as JSON — ~114 bytes per note, which is
    // 2.2 MB on a large session: past what can be sent at all, and past it
    // silently. The same song's shape is under a kilobyte and answers the
    // question the prompt below actually poses ("which track is the bass")
    // better than twenty thousand note objects do. Notes come from the `observe`
    // tool, for the window being worked on.
    let obs = session.observe().to_text();
    format!(
        "You are operating a digital audio workstation through its tool API. You \
         are not describing what to do — the tools ARE the doing, and the person \
         will hear the result.\n\n\
         Work in small steps and check the observation after edits that matter. \
         Prefer the smallest change that answers the request. If a request is \
         ambiguous in a way that changes the music — which track, which bar — ask \
         rather than guess. If a tool refuses, read the refusal: it names what \
         was wrong.\n\n\
         Ticks are nanoticks; there are 960000 per quarter note. Pitches are MIDI \
         numbers, 60 is middle C. Track ids are stable and do not renumber when a \
         track is removed.\n\n\
         Below is the song's SHAPE, not its notes: each track's name, how many \
         notes it has, the beats it spans and the pitch range it covers. To see \
         actual notes, call `observe` with `from_beat` for the part you are \
         working on. A track marked TRUNCATED has more notes than the engine \
         publishes, so do not conclude it ends where the count stops.\n\n\
         This shape was taken before your first tool call and is NOT refreshed as \
         you work — call `observe` again after edits that matter.\n\n\
         {obs}"
    )
}

/// One prompt, to as many tool round trips as it takes.
pub fn run(session: &AgentSession, prompt: &str, tx: &Sender<Progress>) {
    let Some(key) = api_key() else {
        let _ = tx.send(Progress::Failed(
            "no ANTHROPIC_API_KEY — export it, put it in .env at the repo root, \
             or point DAW_ENV_FILE at the file that has it, then ask again".into(),
        ));
        return;
    };

    let tools = tools_json(session);
    let system = system_prompt(session);
    // The running conversation. Tool results have to come back in the same
    // structure the model sent the calls in, so this accumulates rather than
    // being rebuilt per turn.
    let mut messages: Vec<Value> = vec![json!({ "role": "user", "content": prompt })];

    for turn in 0..MAX_TURNS {
        let body = json!({
            "model": MODEL,
            "max_tokens": MAX_TOKENS,
            "system": system,
            "tools": tools,
            "messages": messages,
        });

        let resp = ureq::post(API_URL)
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

    /// The env var wins over the file, so a caller can override without editing
    /// anything on disk.
    #[test]
    fn env_key_wins() {
        std::env::set_var("ANTHROPIC_API_KEY", "from-env");
        assert_eq!(api_key().as_deref(), Some("from-env"));
        std::env::remove_var("ANTHROPIC_API_KEY");
    }
}
