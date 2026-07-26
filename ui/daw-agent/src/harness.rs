//! The agent loop: perceive -> decide -> act, repeated.
//!
//! This is the model-agnostic half of an LLM harness. A [`Decider`] is whatever
//! chooses the next tool calls from the current observation — a real one wraps an
//! LLM (feed it `observation` + `manifest`, parse its tool calls back into
//! [`ToolCall`]s), a test one is a fixed script. [`run_agent_loop`] does the rest:
//! observe the song, ask the decider, execute each call on the engine, repeat
//! until the decider stops or a step budget is hit. No network and no model live
//! here, in keeping with this crate's charter — a networked decider is a separate
//! crate that plugs a key in and implements this one trait.

use crate::{AgentSession, Observation, ToolCall, ToolResult};

/// Chooses the next tool calls to run. Returning an empty vec ends the loop
/// (the decider considers the task done, or has nothing more to do).
pub trait Decider {
    /// Given the current observation and the (unchanging) tool manifest, decide
    /// what to do at `step`. `history` is the outcome of every prior step, oldest
    /// first — a real decider feeds this back to the model as context.
    fn decide(
        &mut self,
        observation: &Observation,
        manifest_json: &str,
        history: &[StepOutcome],
        step: usize,
    ) -> Vec<ToolCall>;
}

/// One tool call and what the engine returned for it.
#[derive(Debug, Clone)]
pub struct CallOutcome {
    pub call: ToolCall,
    pub result: ToolResult,
}

/// Everything that happened in one loop step.
#[derive(Debug, Clone, Default)]
pub struct StepOutcome {
    pub calls: Vec<CallOutcome>,
}

impl StepOutcome {
    /// True if every call this step succeeded.
    pub fn ok(&self) -> bool {
        self.calls.iter().all(|c| c.result.ok)
    }
}

/// Drive the engine: observe -> decide -> execute, until the decider returns no
/// calls or `max_steps` is reached. Returns the transcript of every step.
pub fn run_agent_loop(
    session: &AgentSession,
    decider: &mut dyn Decider,
    max_steps: usize,
) -> Vec<StepOutcome> {
    let manifest = crate::manifest_json();
    let mut history: Vec<StepOutcome> = Vec::new();
    for step in 0..max_steps {
        let observation = session.observe();
        let calls = decider.decide(&observation, &manifest, &history, step);
        if calls.is_empty() {
            break;
        }
        let mut outcome = StepOutcome::default();
        for call in calls {
            let result = session.execute(&call);
            outcome.calls.push(CallOutcome { call, result });
        }
        history.push(outcome);
    }
    history
}

/// A decider that plays a fixed list of steps, one per loop iteration, then
/// stops. Useful for tests and scripted runs — the same shape a real LLM decider
/// fills in, minus the model call.
pub struct ScriptedDecider {
    steps: std::vec::IntoIter<Vec<ToolCall>>,
}

impl ScriptedDecider {
    pub fn new(steps: Vec<Vec<ToolCall>>) -> Self {
        Self {
            steps: steps.into_iter(),
        }
    }
}

impl Decider for ScriptedDecider {
    fn decide(
        &mut self,
        _observation: &Observation,
        _manifest_json: &str,
        _history: &[StepOutcome],
        _step: usize,
    ) -> Vec<ToolCall> {
        self.steps.next().unwrap_or_default()
    }
}
