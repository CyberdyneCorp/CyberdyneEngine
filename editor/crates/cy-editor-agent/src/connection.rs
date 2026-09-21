// SPDX-License-Identifier: MIT
//! A bounded, transport-neutral request queue for interactive agent connections.
//!
//! Network readers may enqueue from their own thread, but only the UI owner drains requests and
//! touches [`cy_editor_services::Editor`]. Capacity, deadlines, and a per-frame drain budget keep a
//! saturated peer from turning the desktop event loop into its worker thread.

use std::collections::{BTreeMap, BTreeSet, VecDeque};
use std::sync::{Arc, Condvar, Mutex};
use std::time::{Duration, Instant};

use cy_editor_commands::Registry;
use cy_editor_core::problem::{Problem, Result};
use cy_editor_services::Editor;

use crate::{
    AgentRequest, AgentResponse, AgentSession, Confirmation, Confirmer, Decision, RefuseEverything,
};

/// A request admitted to the UI-owned execution path.
#[derive(Clone, PartialEq, Debug)]
pub struct QueuedAgentRequest {
    /// Connection-local identity used to return the response to its caller.
    pub ticket: u64,
    /// The transport-neutral operation.
    pub request: AgentRequest,
    /// Absolute deadline on the host's monotonic millisecond clock.
    pub deadline_millis: u64,
}

#[derive(Clone, PartialEq, Debug)]
struct Pending {
    ticket: u64,
    request: AgentRequest,
    deadline_millis: u64,
}

/// Bounded request and response state for one interactive agent connection.
#[derive(Debug)]
pub struct AgentConnectionService {
    capacity: usize,
    next_ticket: u64,
    connected: bool,
    revoked: bool,
    pending: VecDeque<Pending>,
    in_flight: BTreeSet<u64>,
    responses: BTreeMap<u64, AgentResponse>,
}

impl AgentConnectionService {
    /// Open a connection with room for `capacity` admitted but unanswered requests.
    #[must_use]
    pub fn new(capacity: usize) -> Self {
        Self {
            capacity: capacity.max(1),
            next_ticket: 0,
            connected: true,
            revoked: false,
            pending: VecDeque::new(),
            in_flight: BTreeSet::new(),
            responses: BTreeMap::new(),
        }
    }

    /// Admit a request with an absolute-duration deadline measured on the caller's monotonic clock.
    pub fn submit(
        &mut self,
        request: AgentRequest,
        now_millis: u64,
        timeout_millis: u64,
    ) -> Result<u64> {
        if !self.connected {
            return Err(Problem::new(
                "submit an agent request",
                "the connection is closed",
            ));
        }
        if self.revoked {
            return Err(Problem::new(
                "submit an agent request",
                "the connection's access has been revoked",
            ));
        }
        if self.outstanding() >= self.capacity {
            return Err(Problem::new(
                "submit an agent request",
                format!("the bounded queue already holds {} requests", self.capacity),
            )
            .with_remedy("wait for a response before submitting more work"));
        }
        self.next_ticket = self.next_ticket.saturating_add(1);
        let ticket = self.next_ticket;
        self.pending.push_back(Pending {
            ticket,
            request,
            deadline_millis: now_millis.saturating_add(timeout_millis),
        });
        Ok(ticket)
    }

    /// Move at most `budget` live requests to the UI owner, expiring older work first.
    pub fn drain(&mut self, now_millis: u64, budget: usize) -> Vec<QueuedAgentRequest> {
        let mut ready = Vec::new();
        while ready.len() < budget {
            let Some(pending) = self.pending.pop_front() else {
                break;
            };
            if now_millis >= pending.deadline_millis {
                self.responses.insert(
                    pending.ticket,
                    AgentResponse::Refused {
                        what: "run a queued agent request".into(),
                        because: "its deadline elapsed before the editor could start it".into(),
                        remedy: Some("retry with a later deadline or less concurrent work".into()),
                    },
                );
                continue;
            }
            self.in_flight.insert(pending.ticket);
            ready.push(QueuedAgentRequest {
                ticket: pending.ticket,
                request: pending.request,
                deadline_millis: pending.deadline_millis,
            });
        }
        ready
    }

    /// Drain and execute a bounded number of requests through the UI-owned editor and registry.
    ///
    /// Calling this from the desktop frame loop is the ownership boundary: transport threads only
    /// submit plain requests, while transactions remain ordered with human commands on one owner.
    pub fn drain_into(
        &mut self,
        session: &mut AgentSession,
        editor: &mut Editor,
        registry: &Registry,
        confirmer: &mut dyn Confirmer,
        now_millis: u64,
        budget: usize,
    ) -> usize {
        let requests = self.drain(now_millis, budget);
        let count = requests.len();
        for queued in requests {
            let response = crate::execute(
                session,
                &queued.request,
                editor,
                registry,
                confirmer,
                now_millis,
            );
            // `drain` put the ticket in flight, so failure here would mean this service broke its
            // own invariant rather than something the peer can recover from.
            self.respond(queued.ticket, response)
                .expect("a drained request remains in flight until it is answered");
        }
        count
    }

    /// Complete one request and make its response available only under the matching ticket.
    pub fn respond(&mut self, ticket: u64, response: AgentResponse) -> Result<()> {
        if !self.in_flight.remove(&ticket) {
            return Err(Problem::new(
                format!("answer agent request {ticket}"),
                "that request is not in flight",
            ));
        }
        self.responses.insert(ticket, response);
        Ok(())
    }

    /// Take the response for one request, leaving every other caller's response untouched.
    pub fn take_response(&mut self, ticket: u64) -> Option<AgentResponse> {
        self.responses.remove(&ticket)
    }

    /// Disconnect and abandon every request that has not produced a response.
    ///
    /// Returns the abandoned count for diagnostics. Completed responses are also removed because
    /// no peer remains that can receive them.
    pub fn disconnect(&mut self) -> usize {
        self.connected = false;
        let abandoned = self.pending.len() + self.in_flight.len();
        self.pending.clear();
        self.in_flight.clear();
        self.responses.clear();
        abandoned
    }

    /// Revoke the session and answer every uncommitted request with a policy refusal.
    pub fn revoke(&mut self) -> usize {
        self.revoked = true;
        let tickets: Vec<u64> = self
            .pending
            .drain(..)
            .map(|request| request.ticket)
            .chain(std::mem::take(&mut self.in_flight))
            .collect();
        for ticket in &tickets {
            self.responses.insert(
                *ticket,
                AgentResponse::Refused {
                    what: "run an agent request".into(),
                    because: "the human revoked this session before it committed".into(),
                    remedy: Some("open a new session and request a new scope".into()),
                },
            );
        }
        tickets.len()
    }

    /// Requests accepted and not yet answered.
    #[must_use]
    pub fn outstanding(&self) -> usize {
        self.pending.len() + self.in_flight.len()
    }

    /// Whether the peer may submit more requests.
    #[must_use]
    pub const fn is_connected(&self) -> bool {
        self.connected
    }
}

#[derive(Debug)]
struct SharedConnection {
    service: Mutex<AgentConnectionService>,
    responses: Condvar,
}

/// Cloneable transport-thread endpoint for a desktop-hosted agent session.
#[derive(Clone, Debug)]
pub struct DesktopAgentEndpoint {
    shared: Arc<SharedConnection>,
    started: Instant,
}

impl DesktopAgentEndpoint {
    /// Submit without touching Editor-owned state.
    pub fn submit(&self, request: AgentRequest, timeout: Duration) -> Result<u64> {
        let now = self.now_millis();
        self.shared
            .service
            .lock()
            .expect("agent queue mutex poisoned")
            .submit(
                request,
                now,
                u64::try_from(timeout.as_millis()).unwrap_or(u64::MAX),
            )
    }

    /// Wait only for this request's correlated response.
    pub fn wait_response(&self, ticket: u64, timeout: Duration) -> AgentResponse {
        let started = Instant::now();
        let mut service = self
            .shared
            .service
            .lock()
            .expect("agent queue mutex poisoned");
        loop {
            if let Some(response) = service.take_response(ticket) {
                return response;
            }
            let remaining = timeout.saturating_sub(started.elapsed());
            if remaining.is_zero() {
                return AgentResponse::Refused {
                    what: "wait for the desktop editor".into(),
                    because: "the request deadline elapsed before a response arrived".into(),
                    remedy: Some("retry after the editor finishes its current frame".into()),
                };
            }
            let (next, _) = self
                .shared
                .responses
                .wait_timeout(service, remaining)
                .expect("agent queue mutex poisoned while waiting");
            service = next;
        }
    }

    /// Notify the UI owner that the peer disconnected.
    pub fn disconnect(&self) {
        self.shared
            .service
            .lock()
            .expect("agent queue mutex poisoned")
            .disconnect();
        self.shared.responses.notify_all();
    }

    /// Milliseconds since this desktop bridge opened.
    #[must_use]
    pub fn now_millis(&self) -> u64 {
        u64::try_from(self.started.elapsed().as_millis()).unwrap_or(u64::MAX)
    }
}

/// A destructive/external request waiting for a human decision.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct PendingAgentConfirmation {
    /// Queue ticket, used for the decision.
    pub ticket: u64,
    /// What the agent asked to do and what would be lost.
    pub confirmation: Confirmation,
}

#[derive(Clone, PartialEq, Debug)]
struct AwaitingConfirmation {
    queued: QueuedAgentRequest,
    confirmation: Confirmation,
}

/// Read-only state rendered by the Agent Sessions panel.
#[derive(Clone, PartialEq, Debug)]
pub struct AgentSessionStatus {
    /// Connected identity.
    pub identity: crate::AgentIdentity,
    /// Whether the transport peer remains attached.
    pub connected: bool,
    /// Stated intent.
    pub intent: String,
    /// Scope name, effect classes, and directory prefixes.
    pub scope: String,
    /// Current budget state.
    pub budget: crate::BudgetReport,
    /// Current operation or pending confirmation.
    pub current_operation: Option<String>,
    /// Paused state, in words as well as presentation colour.
    pub paused: bool,
    /// Permanent revocation state.
    pub revoked: bool,
    /// Number of queued or executing requests.
    pub outstanding: usize,
}

/// UI-owned half of a desktop MCP session.
pub struct DesktopAgentHost {
    endpoint: DesktopAgentEndpoint,
    session: AgentSession,
    awaiting: BTreeMap<u64, AwaitingConfirmation>,
    approved: VecDeque<QueuedAgentRequest>,
    was_connected: bool,
}

impl DesktopAgentHost {
    /// Build both halves. The endpoint may move to a transport thread; the host stays with Editor.
    #[must_use]
    pub fn new(session: AgentSession, capacity: usize) -> (Self, DesktopAgentEndpoint) {
        let started = Instant::now();
        let endpoint = DesktopAgentEndpoint {
            shared: Arc::new(SharedConnection {
                service: Mutex::new(AgentConnectionService::new(capacity)),
                responses: Condvar::new(),
            }),
            started,
        };
        (
            Self {
                endpoint: endpoint.clone(),
                session,
                awaiting: BTreeMap::new(),
                approved: VecDeque::new(),
                was_connected: true,
            },
            endpoint,
        )
    }

    /// Drain a bounded number of requests on the UI owner. No transport lock is held while a
    /// command executes.
    pub fn pump(&mut self, editor: &mut Editor, registry: &Registry, budget: usize) -> usize {
        let now = self.endpoint.now_millis();
        self.observe_connection_state();
        self.expire_confirmations(now);
        if self.session.is_paused() || self.session.is_revoked() {
            self.session.publish_diagnostics(editor);
            return 0;
        }
        let mut ready = Vec::new();
        while ready.len() < budget {
            let Some(request) = self.approved.pop_front() else {
                break;
            };
            ready.push(request);
        }
        if ready.len() < budget {
            let remaining = budget - ready.len();
            ready.extend(
                self.endpoint
                    .shared
                    .service
                    .lock()
                    .expect("agent queue mutex poisoned")
                    .drain(now, remaining),
            );
        }
        let count = ready.len();
        for queued in ready {
            if self.hold_for_confirmation(editor, registry, &queued, now) {
                continue;
            }
            let response = crate::execute(
                &mut self.session,
                &queued.request,
                editor,
                registry,
                &mut RefuseEverything,
                now,
            );
            self.respond(queued.ticket, response);
        }
        self.session.publish_diagnostics(editor);
        count
    }

    fn observe_connection_state(&mut self) {
        let connected = self
            .endpoint
            .shared
            .service
            .lock()
            .expect("agent queue mutex poisoned")
            .is_connected();
        if self.was_connected && !connected {
            self.session.audit(
                crate::AgentAuditKind::Connection,
                crate::PrivacyClass::Public,
                "disconnected",
            );
        }
        self.was_connected = connected;
    }

    fn hold_for_confirmation(
        &mut self,
        editor: &mut Editor,
        registry: &Registry,
        queued: &QueuedAgentRequest,
        now: u64,
    ) -> bool {
        let AgentRequest::Invoke { command, arguments } = &queued.request else {
            return false;
        };
        let built = match crate::tool::arguments(registry, command, arguments) {
            Ok(arguments) => arguments,
            Err(problem) => {
                self.respond(
                    queued.ticket,
                    AgentResponse::Refused {
                        what: format!("invoke {command}"),
                        because: problem.because,
                        remedy: problem.remedy,
                    },
                );
                return true;
            }
        };
        match self
            .session
            .confirmation_for(editor, registry, command, &built, now)
        {
            Ok(Some(confirmation)) => {
                self.session.audit(
                    crate::AgentAuditKind::Invocation,
                    crate::PrivacyClass::ProjectMetadata,
                    format!("{command} is awaiting desktop confirmation"),
                );
                self.awaiting.insert(
                    queued.ticket,
                    AwaitingConfirmation {
                        queued: queued.clone(),
                        confirmation,
                    },
                );
                true
            }
            Ok(None) => false,
            Err(problem) => {
                self.respond(
                    queued.ticket,
                    AgentResponse::Refused {
                        what: format!("invoke {command}"),
                        because: problem.because,
                        remedy: problem.remedy,
                    },
                );
                true
            }
        }
    }

    fn respond(&self, ticket: u64, response: AgentResponse) {
        let _ = self
            .endpoint
            .shared
            .service
            .lock()
            .expect("agent queue mutex poisoned")
            .respond(ticket, response);
        self.endpoint.shared.responses.notify_all();
    }

    fn expire_confirmations(&mut self, now: u64) {
        let expired: Vec<u64> = self
            .awaiting
            .iter()
            .filter_map(|(ticket, pending)| {
                (now >= pending.queued.deadline_millis).then_some(*ticket)
            })
            .collect();
        for ticket in expired {
            self.awaiting.remove(&ticket);
            self.respond(
                ticket,
                AgentResponse::Refused {
                    what: "confirm an agent request".into(),
                    because: "the request deadline elapsed while awaiting confirmation".into(),
                    remedy: Some("submit it again if the operation is still intended".into()),
                },
            );
        }
    }

    /// Pending confirmation, if any. One is shown at a time in stable ticket order.
    #[must_use]
    pub fn pending_confirmation(&self) -> Option<PendingAgentConfirmation> {
        self.awaiting
            .values()
            .next()
            .map(|pending| PendingAgentConfirmation {
                ticket: pending.queued.ticket,
                confirmation: pending.confirmation.clone(),
            })
    }

    /// Apply a human decision. A duration creates an explicitly time-bounded grant; otherwise the
    /// approval covers only this request.
    pub fn decide(&mut self, ticket: u64, decision: Decision, grant_for: Option<Duration>) {
        let Some(pending) = self.awaiting.remove(&ticket) else {
            return;
        };
        match decision {
            Decision::Allow => {
                let now = self.endpoint.now_millis();
                match grant_for {
                    Some(duration) => self.session.grant_for(
                        pending.confirmation.effect,
                        now,
                        u64::try_from(duration.as_millis()).unwrap_or(u64::MAX),
                    ),
                    None => self.session.grant(pending.confirmation.effect, 1),
                }
                self.approved.push_back(pending.queued);
            }
            Decision::Refuse => {
                self.session.audit(
                    crate::AgentAuditKind::Refusal,
                    crate::PrivacyClass::ProjectMetadata,
                    format!("{} was refused by the human", pending.confirmation.command),
                );
                self.respond(
                    ticket,
                    AgentResponse::Refused {
                        what: format!("invoke {}", pending.confirmation.command),
                        because: "the human refused the pending desktop confirmation".into(),
                        remedy: Some(
                            "choose a reversible operation or ask again with more context".into(),
                        ),
                    },
                );
            }
        }
    }

    /// Pause before the next invocation.
    pub const fn pause(&mut self) {
        self.session.pause();
    }

    /// Resume a paused session.
    pub const fn resume(&mut self) {
        self.session.resume();
    }

    /// Permanently revoke the session, its grants, queued requests, and uncommitted transactions.
    pub fn revoke(&mut self, editor: &mut Editor) -> Result<usize> {
        self.session.revoke_grants();
        self.session.revoke();
        self.session.audit(
            crate::AgentAuditKind::Connection,
            crate::PrivacyClass::Public,
            "revoked by the human",
        );
        self.awaiting.clear();
        self.approved.clear();
        let abandoned = self
            .endpoint
            .shared
            .service
            .lock()
            .expect("agent queue mutex poisoned")
            .revoke();
        self.endpoint.shared.responses.notify_all();
        editor
            .documents
            .cancel_agent_session(&self.session.identity().session)?;
        Ok(abandoned)
    }

    /// Current UI state.
    #[must_use]
    pub fn status(&self) -> AgentSessionStatus {
        let now = self.endpoint.now_millis();
        let scope = self.session.scope();
        let service = self
            .endpoint
            .shared
            .service
            .lock()
            .expect("agent queue mutex poisoned");
        AgentSessionStatus {
            identity: self.session.identity().clone(),
            connected: service.is_connected(),
            intent: self.session.intent().to_string(),
            scope: format!(
                "{} · effects: {} · directories: {}",
                scope.name,
                scope
                    .effects
                    .iter()
                    .map(|effect| effect.name())
                    .collect::<Vec<_>>()
                    .join(", "),
                if scope.directories.is_empty() {
                    "none".to_string()
                } else {
                    scope.directories.join(", ")
                }
            ),
            budget: self.session.budget(now),
            current_operation: self
                .pending_confirmation()
                .map(|pending| format!("Awaiting confirmation: {}", pending.confirmation.command))
                .or_else(|| self.session.current_operation().map(str::to_string)),
            paused: self.session.is_paused(),
            revoked: self.session.is_revoked(),
            outstanding: service.outstanding(),
        }
    }

    /// Session audit rows for the diagnostic/status UI.
    #[must_use]
    pub fn audit_records(&self) -> &[crate::AgentAuditRecord] {
        self.session.audit_records()
    }
}

#[cfg(test)]
mod tests {
    use cy_editor_commands::scope::DocumentScope;
    use cy_editor_commands::{
        Arguments, Command, CommandContext, EffectClass, Metadata, Outcome, ParameterSpec, Scope,
    };
    use cy_editor_core::Actor;
    use cy_editor_core::problem::Problem;
    use cy_editor_core::value::ValueKind;

    use super::*;
    use crate::{AgentIdentity, Budget, RefuseEverything};

    fn ping() -> AgentRequest {
        AgentRequest::ListResources
    }

    fn desktop_registry() -> Registry {
        let mut registry = Registry::new();
        registry
            .register(Command::new(
                Metadata::new(
                    "scene.create-entity",
                    "Create Entity",
                    "Scene",
                    "Creates an entity in the active document.",
                    EffectClass::ReversibleMutation,
                ),
                |context: &mut dyn CommandContext, _: &Arguments| {
                    let id = context
                        .active_document()
                        .ok_or_else(|| Problem::new("create an entity", "no document is active"))?;
                    let actor = context.actor();
                    context
                        .document_mut(id)
                        .expect("active document exists")
                        .with_transaction("Create Entity", actor, |document| {
                            document.create_node(None).map(|_| ())
                        })?;
                    Ok(Outcome::new("Created 1 entity"))
                },
            ))
            .unwrap();
        registry
            .register(Command::new(
                Metadata::new(
                    "assets.delete",
                    "Delete Asset",
                    "Assets",
                    "Deletes an asset from disk.",
                    EffectClass::IrreversibleMutation,
                )
                .with(ParameterSpec::required(
                    "asset",
                    ValueKind::Text,
                    "The project-relative asset path.",
                )),
                |_: &mut dyn CommandContext, _: &Arguments| Ok(Outcome::new("Deleted 1 asset")),
            ))
            .unwrap();
        registry
    }

    fn desktop_host() -> (DesktopAgentHost, DesktopAgentEndpoint) {
        let session = AgentSession::new(
            AgentIdentity {
                agent: "builder".into(),
                session: "desktop-1".into(),
            },
            "arrange the scene",
            Scope::new("author", DocumentScope::All, EffectClass::ALL),
            Budget::default(),
            "base",
            0,
        );
        DesktopAgentHost::new(session, 64)
    }

    #[test]
    fn capacity_applies_backpressure_until_a_request_is_answered() {
        let mut connection = AgentConnectionService::new(1);
        let ticket = connection.submit(ping(), 10, 100).unwrap();
        assert!(connection.submit(ping(), 10, 100).is_err());
        let request = connection.drain(11, 1).pop().unwrap();
        assert_eq!(request.ticket, ticket);
        connection
            .respond(ticket, AgentResponse::Resources(Vec::new()))
            .unwrap();
        assert!(connection.submit(ping(), 12, 100).is_ok());
    }

    #[test]
    fn deadlines_and_drain_budgets_are_enforced_before_execution() {
        let mut connection = AgentConnectionService::new(4);
        let expired = connection.submit(ping(), 0, 5).unwrap();
        let first = connection.submit(ping(), 0, 50).unwrap();
        let second = connection.submit(ping(), 0, 50).unwrap();
        let ready = connection.drain(5, 1);
        assert_eq!(ready.len(), 1);
        assert_eq!(ready[0].ticket, first);
        assert!(matches!(
            connection.take_response(expired),
            Some(AgentResponse::Refused { .. })
        ));
        assert_eq!(connection.outstanding(), 2);
        assert_eq!(connection.drain(6, 1)[0].ticket, second);
    }

    #[test]
    fn responses_are_correlated_and_disconnect_cleans_everything() {
        let mut connection = AgentConnectionService::new(3);
        let first = connection.submit(ping(), 0, 50).unwrap();
        let second = connection.submit(ping(), 0, 50).unwrap();
        let drained = connection.drain(1, 1);
        connection
            .respond(drained[0].ticket, AgentResponse::Resources(Vec::new()))
            .unwrap();
        assert!(connection.take_response(second).is_none());
        assert!(connection.take_response(first).is_some());
        assert_eq!(connection.disconnect(), 1);
        assert_eq!(connection.outstanding(), 0);
        assert!(!connection.is_connected());
        assert!(connection.submit(ping(), 2, 50).is_err());
    }

    #[test]
    fn ui_owned_draining_orders_agent_and_human_transactions() {
        let mut editor = Editor::new(Actor::human("designer"));
        let document = editor.open_document("worlds/city.cyworld").unwrap();
        let mut registry = Registry::new();
        cy_editor_services::builtin::register(&mut registry).unwrap();
        let scope = Scope::new(
            "author",
            DocumentScope::All,
            [EffectClass::Read, EffectClass::ReversibleMutation],
        );
        let mut session = AgentSession::new(
            AgentIdentity {
                agent: "builder".into(),
                session: "interactive-1".into(),
            },
            "add props",
            scope,
            Budget::default(),
            "base",
            0,
        );
        let mut confirmer = RefuseEverything;
        let request = AgentRequest::Invoke {
            command: "scene.create-entity".into(),
            arguments: Vec::new(),
        };
        let mut connection = AgentConnectionService::new(4);
        connection.submit(request.clone(), 0, 100).unwrap();
        connection.submit(request, 0, 100).unwrap();

        assert_eq!(
            connection.drain_into(&mut session, &mut editor, &registry, &mut confirmer, 1, 1,),
            1
        );
        editor
            .invoke(
                &registry,
                "scene.create-entity",
                &Scope::unrestricted(),
                &Arguments::new(),
            )
            .unwrap();
        connection.drain_into(&mut session, &mut editor, &registry, &mut confirmer, 2, 1);

        let actors: Vec<_> = editor
            .documents
            .get(document)
            .unwrap()
            .history()
            .entries()
            .iter()
            .map(|entry| entry.actor.is_agent())
            .collect();
        assert_eq!(actors, [true, false, true]);
    }

    #[test]
    fn desktop_prompts_only_for_irreversible_work_and_can_refuse_it() {
        let mut editor = Editor::new(Actor::human("designer"));
        editor.open_document("worlds/city.cyworld").unwrap();
        let registry = desktop_registry();
        let (mut host, endpoint) = desktop_host();

        let reversible = endpoint
            .submit(
                AgentRequest::Invoke {
                    command: "scene.create-entity".into(),
                    arguments: Vec::new(),
                },
                Duration::from_secs(1),
            )
            .unwrap();
        assert_eq!(host.pump(&mut editor, &registry, 4), 1);
        assert!(host.pending_confirmation().is_none());
        assert!(matches!(
            endpoint.wait_response(reversible, Duration::from_millis(1)),
            AgentResponse::Outcome { .. }
        ));

        let irreversible = endpoint
            .submit(
                AgentRequest::Invoke {
                    command: "assets.delete".into(),
                    arguments: vec![("asset".into(), "models/ship.mesh".into())],
                },
                Duration::from_secs(1),
            )
            .unwrap();
        host.pump(&mut editor, &registry, 4);
        let pending = host
            .pending_confirmation()
            .expect("human decision is pending");
        assert_eq!(pending.ticket, irreversible);
        assert!(pending.confirmation.what_is_lost.contains("undo"));
        host.decide(irreversible, Decision::Refuse, None);
        assert!(matches!(
            endpoint.wait_response(irreversible, Duration::from_millis(1)),
            AgentResponse::Refused { .. }
        ));
    }

    #[test]
    fn temporary_grants_expire_and_revocation_rolls_back_agent_work() {
        let mut editor = Editor::new(Actor::human("designer"));
        let document = editor.open_document("worlds/city.cyworld").unwrap();
        let registry = desktop_registry();
        let (mut host, _) = desktop_host();
        host.session
            .grant_for(EffectClass::IrreversibleMutation, 10, 5);
        let arguments = Arguments::new().with(
            "asset",
            cy_editor_core::value::Value::Text("models/ship.mesh".into()),
        );
        assert!(
            host.session
                .confirmation_for(&mut editor, &registry, "assets.delete", &arguments, 14)
                .unwrap()
                .is_none()
        );
        assert!(
            host.session
                .confirmation_for(&mut editor, &registry, "assets.delete", &arguments, 15)
                .unwrap()
                .is_some()
        );

        let open = editor.documents.get_mut(document).unwrap();
        open.begin(
            "Agent drag",
            Actor::agent("builder", "desktop-1", "arrange the scene"),
        );
        open.create_node(None).unwrap();
        assert_eq!(open.content().node_count(), 1);
        host.revoke(&mut editor).unwrap();
        assert_eq!(
            editor
                .documents
                .get(document)
                .unwrap()
                .content()
                .node_count(),
            0
        );
        assert!(host.status().revoked);
    }

    #[test]
    fn a_saturated_desktop_client_gets_only_the_frame_budget() {
        let mut editor = Editor::new(Actor::human("designer"));
        let registry = desktop_registry();
        let (mut host, endpoint) = desktop_host();
        for _ in 0..64 {
            endpoint
                .submit(AgentRequest::ListResources, Duration::from_secs(1))
                .unwrap();
        }
        assert!(
            endpoint
                .submit(AgentRequest::ListResources, Duration::from_secs(1))
                .is_err()
        );
        assert_eq!(host.pump(&mut editor, &registry, 4), 4);
        assert_eq!(host.status().outstanding, 60);
    }

    #[test]
    fn pausing_holds_queued_work_until_the_human_resumes_it() {
        let mut editor = Editor::new(Actor::human("designer"));
        let registry = desktop_registry();
        let (mut host, endpoint) = desktop_host();
        endpoint
            .submit(AgentRequest::ListResources, Duration::from_secs(1))
            .unwrap();
        host.pause();
        assert_eq!(host.pump(&mut editor, &registry, 4), 0);
        assert_eq!(host.status().outstanding, 1);
        host.resume();
        assert_eq!(host.pump(&mut editor, &registry, 4), 1);
        assert_eq!(host.status().outstanding, 0);
    }

    #[test]
    fn desktop_and_headless_execution_expose_identical_tools_and_resources() {
        let mut headless_editor = Editor::new(Actor::human("designer"));
        let mut desktop_editor = Editor::new(Actor::human("designer"));
        for editor in [&mut headless_editor, &mut desktop_editor] {
            editor.open_document("worlds/city.cyworld").unwrap();
        }
        let registry = desktop_registry();
        let scope = Scope::new("author", DocumentScope::All, EffectClass::ALL);
        let mut headless = AgentSession::new(
            AgentIdentity {
                agent: "builder".into(),
                session: "headless-1".into(),
            },
            "compare the surface",
            scope,
            Budget::default(),
            "base",
            0,
        );
        let (mut desktop, endpoint) = desktop_host();

        for request in [AgentRequest::ListTools, AgentRequest::ListResources] {
            let expected = crate::execute(
                &mut headless,
                &request,
                &mut headless_editor,
                &registry,
                &mut RefuseEverything,
                0,
            );
            let ticket = endpoint.submit(request, Duration::from_secs(1)).unwrap();
            desktop.pump(&mut desktop_editor, &registry, 4);
            assert_eq!(
                endpoint.wait_response(ticket, Duration::from_millis(1)),
                expected
            );
        }
    }

    #[test]
    fn diagnostics_are_session_attributed_and_do_not_copy_arguments() {
        let mut editor = Editor::new(Actor::human("designer"));
        editor.open_document("worlds/city.cyworld").unwrap();
        let registry = desktop_registry();
        let (mut host, endpoint) = desktop_host();
        endpoint
            .submit(
                AgentRequest::Invoke {
                    command: "scene.create-entity".into(),
                    arguments: Vec::new(),
                },
                Duration::from_secs(1),
            )
            .unwrap();
        host.pump(&mut editor, &registry, 4);
        let render = crate::ViewportRequest::shipping_frame(editor.viewports.focused());
        let _ = host.session.observe(&mut editor, &render, 0);
        let secret_path = "private/customer-project/secret.mesh";
        endpoint
            .submit(
                AgentRequest::Invoke {
                    command: "assets.delete".into(),
                    arguments: vec![("asset".into(), secret_path.into())],
                },
                Duration::from_secs(1),
            )
            .unwrap();
        host.pump(&mut editor, &registry, 4);
        host.decide(
            host.pending_confirmation().unwrap().ticket,
            Decision::Refuse,
            None,
        );
        host.pump(&mut editor, &registry, 0);
        for record in host.audit_records() {
            assert_eq!(record.session, "desktop-1");
            assert!(!record.summary.contains(secret_path));
        }
        assert!(
            host.audit_records()
                .iter()
                .any(|record| record.kind == crate::AgentAuditKind::Connection)
        );
        for kind in [
            crate::AgentAuditKind::Invocation,
            crate::AgentAuditKind::Refusal,
            crate::AgentAuditKind::Transaction,
            crate::AgentAuditKind::RenderRequest,
            crate::AgentAuditKind::Cost,
        ] {
            assert!(
                host.audit_records()
                    .iter()
                    .any(|record| record.kind == kind),
                "missing {kind:?}"
            );
        }
        let mut cursor = cy_editor_core::observe::Cursor::default();
        let diagnostics = editor.notifications.drain_from(&mut cursor);
        assert!(diagnostics.iter().any(|notification| {
            notification.message.contains("[desktop-1]")
                && notification.message.contains("privacy:")
        }));
    }
}
