//! Run ONE agent tool against a live engine, with no model in the loop.
//!
//! WHY THIS EXISTS. The agent's tools were the last surface with no way to test them: daw-cli is
//! driven by cli-verbs against a real engine, the browser by Playwright, and the agent only by
//! ask.rs — which needs an API key, costs money per run, and is excluded from the sweep. So the
//! tools that tell a model whether its edit landed were themselves unverified, which is a poor
//! joke given what task #54 was about.
//!
//! Nothing here needs a model. `daw_agent::execute` is public and the harness's own Decider trait
//! documents its test implementation as "a fixed script" — the only missing piece was an
//! EngineHandle attached inside something a suite can run. That is all this binary is.
//!
//! It is a RUNNER FOR AN EXISTING SURFACE, not a fourth surface: it adds no capability the agent
//! does not already have, and it should never grow one. If a thing cannot be done by a tool in
//! the manifest, it does not belong here either.
//!
//!     daw-agent-run <tool> '<json args>'
//!
//! Prints the ToolResult as JSON on stdout and exits 0 when the tool reports ok, 1 when it does
//! not — so a shell or a suite can branch on either the exit code or the payload. Attaches via
//! DAW_UI_SHM_NAME exactly as daw-cli does, so a test stack needs no new plumbing.

use daw_agent::ToolCall;
use daw_bridge::control::{default_shm_name, EngineHandle};

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let Some(tool) = args.first() else {
        eprintln!("daw-agent-run: needs a tool name\n\n    daw-agent-run <tool> '<json args>'\n\n\
                   `daw-agent-run --tools` lists what this build can run.");
        std::process::exit(2);
    };

    // Discoverability without a live engine: the manifest is a pure function of the build, so
    // this arm answers before attaching to anything.
    if tool == "--tools" {
        for spec in daw_agent::tool_manifest() {
            println!("{}", spec.name);
        }
        return;
    }

    // Absent args mean "no arguments", not "empty object is an error" — several tools take none.
    let raw = args.get(1).map(String::as_str).unwrap_or("{}");
    let parsed: serde_json::Value = match serde_json::from_str(raw) {
        Ok(v) => v,
        Err(err) => {
            eprintln!("daw-agent-run: the arguments are not JSON: {err}\n  got: {raw}");
            std::process::exit(2);
        }
    };

    // Writable: every tool worth testing here sends a command.
    let handle = match EngineHandle::attach(&default_shm_name(), true) {
        Ok(h) => h,
        Err(err) => {
            eprintln!("daw-agent-run: {err}");
            std::process::exit(2);
        }
    };

    let result = daw_agent::execute(&handle, &ToolCall { tool: tool.clone(), args: parsed });
    // The whole ToolResult, not a summary. A refusal's REASON is the thing under test, and a
    // caller that only got "ok: false" would be back where this task started.
    match serde_json::to_string(&result) {
        Ok(json) => println!("{json}"),
        Err(err) => {
            eprintln!("daw-agent-run: could not serialise the result: {err}");
            std::process::exit(2);
        }
    }
    std::process::exit(if result.ok { 0 } else { 1 });
}
