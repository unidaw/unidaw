//! daw-agent — the in-app agent's substrate.
//!
//! Two halves over the engine's shared memory:
//!   * perception — `observe`, a structured, legible read of the whole song;
//!   * action — a self-describing `tool_manifest` and `execute`, carrying typed
//!     tool calls to the engine's command ring.
//!
//! An `AgentSession` attaches on the agent's OWN command ring (v9), so an
//! acting agent never contends with the UI for the single-producer write ring.
//! Nothing here talks to a model or a network; an LLM harness maps its tool-call
//! format onto `ToolCall`/`execute` and drives the loop.

pub mod harness;
pub mod observe;
pub mod tools;

pub use harness::{run_agent_loop, CallOutcome, Decider, ScriptedDecider, StepOutcome};
pub use observe::{observe, NoteView, Observation, TrackView, Transport};
pub use tools::{
    execute, manifest_json, tool_manifest, ToolCall, ToolResult, ToolSpec,
};

use daw_bridge::control::EngineHandle;

/// A live agent session bound to one engine. Reads are lock-free and unlimited;
/// writes go to the agent's own SPSC command ring.
pub struct AgentSession {
    handle: EngineHandle,
}

impl AgentSession {
    /// Attach to the engine named by `shm_name` (see
    /// `daw_bridge::control::default_shm_name`). Uses the agent command ring.
    pub fn attach(shm_name: &str) -> Result<Self, String> {
        Ok(Self {
            handle: EngineHandle::attach_agent(shm_name)?,
        })
    }

    pub fn handle(&self) -> &EngineHandle {
        &self.handle
    }

    /// Perceive the whole song.
    pub fn observe(&self) -> Observation {
        observe::observe(&self.handle, 0)
    }

    /// The tools this agent can call, as a discoverable manifest.
    pub fn manifest(&self) -> Vec<ToolSpec> {
        tool_manifest()
    }

    /// Run one tool call against the engine.
    pub fn execute(&self, call: &ToolCall) -> ToolResult {
        execute(&self.handle, call)
    }
}
