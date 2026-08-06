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
pub use observe::{observe, DeviceView, NoteView, Observation, PluginView, TrackView, Transport};
pub use tools::{
    execute_in,
    execute, manifest_json, tool_manifest, ToolCall, ToolResult, ToolSpec,
};

use daw_bridge::control::EngineHandle;

/// A live agent session bound to one engine. Reads are lock-free and unlimited;
/// writes go to the agent's own SPSC command ring.
pub struct AgentSession {
    handle: EngineHandle,
    /// Where projects live, for the tools that touch the filesystem rather than the ring.
    /// A caller-supplied fact — see `execute_in`.
    project_dir: String,
}

impl AgentSession {
    /// Attach to the engine named by `shm_name` (see
    /// `daw_bridge::control::default_shm_name`). Uses the agent command ring.
    pub fn attach(shm_name: &str) -> Result<Self, String> {
        Ok(Self {
            handle: EngineHandle::attach_agent(shm_name)?,
            project_dir: daw_bridge::project::engine_project_dir(),
        })
    }

    /// Point this session at a project directory other than the environment's.
    ///
    /// For a HOST that knows better than the environment does — and for tests, which start an
    /// engine on a private temp directory passed to the child process only. Without this a test
    /// would have to mutate the process environment, which is shared by every test in the binary.
    pub fn with_project_dir(mut self, dir: impl Into<String>) -> Self {
        self.project_dir = dir.into();
        self
    }

    /// Where this session believes projects live.
    pub fn project_dir(&self) -> &str {
        &self.project_dir
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
        execute_in(&self.handle, call, &self.project_dir)
    }
}
