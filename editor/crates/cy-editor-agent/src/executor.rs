// SPDX-License-Identifier: MIT
//! Execution of transport-neutral agent requests against the editor's shared command surface.
//!
//! A wire transport decodes into [`AgentRequest`](crate::AgentRequest), then hands the request here.
//! Keeping this function outside MCP means a desktop queue and the headless JSON-RPC connection
//! execute the same projection rather than growing two command paths.

use cy_editor_commands::Registry;
use cy_editor_commands::context::Outcome;
use cy_editor_core::problem::Problem;
use cy_editor_services::Editor;

use crate::session::{AgentSession, Confirmer, Reading};
use crate::transport::{AgentRequest, AgentResponse};

/// Execute one transport-neutral request synchronously on the owner of [`Editor`].
///
/// The owner chooses when to call this. Desktop integrations drain a bounded queue during a frame;
/// the headless transport calls it directly because no window shares that thread.
pub fn execute(
    session: &mut AgentSession,
    request: &AgentRequest,
    editor: &mut Editor,
    registry: &Registry,
    confirmer: &mut dyn Confirmer,
    now_millis: u64,
) -> AgentResponse {
    match request {
        AgentRequest::ListTools => AgentResponse::Tools(session.tools(registry)),
        AgentRequest::ListResources => AgentResponse::Resources(
            session
                .resources(editor)
                .into_iter()
                .map(|(uri, _, description)| (uri, description))
                .collect(),
        ),
        AgentRequest::ReadResource { uri } => match session.read(editor, uri, now_millis) {
            Ok(Reading::Text(resource)) => AgentResponse::Content(Box::new(resource)),
            Ok(Reading::Image(observation)) => AgentResponse::Image(observation),
            Err(problem) => refused(format!("read {uri}"), &problem),
        },
        AgentRequest::Observe(viewport) => match session.observe(editor, viewport, now_millis) {
            Ok(observation) => AgentResponse::Image(Box::new(observation)),
            Err(problem) => refused("observe a viewport", &problem),
        },
        AgentRequest::Invoke { command, arguments } => {
            let built = crate::tool::arguments(registry, command, arguments);
            let outcome = built.and_then(|arguments| {
                session.invoke(editor, registry, confirmer, command, &arguments, now_millis)
            });
            invocation_response(command, outcome)
        }
    }
}

fn invocation_response(command: &str, outcome: Result<Outcome, Problem>) -> AgentResponse {
    match outcome {
        Ok(outcome) => AgentResponse::Outcome {
            summary: outcome.summary,
            values: outcome
                .values
                .into_iter()
                .map(|(name, value)| (name, value.to_string()))
                .collect(),
        },
        Err(problem) => refused(format!("invoke {command}"), &problem),
    }
}

fn refused(what: impl Into<String>, problem: &Problem) -> AgentResponse {
    AgentResponse::Refused {
        what: what.into(),
        because: problem.because.clone(),
        remedy: problem.remedy.clone(),
    }
}
