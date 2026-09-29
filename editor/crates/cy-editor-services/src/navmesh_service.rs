// SPDX-License-Identifier: MIT
//! The client of the engine's `navigation.*` service. Issue #28, task 4.2.
//!
//! Kept apart from [`crate::backend::BackendServices`] on purpose: that service already settles
//! six request kinds in one `accept`, and navigation adds a bake that reports progress per tile
//! plus four queries. This one owns its request, matches events by request identity, checks the
//! schema, decodes PROGRESS and the terminal events, and fails its request when the runtime goes.
//!
//! The engine session holds one navigation request at a time (a second is answered
//! `navigation.busy`), so this client does the same and refuses a second request locally, with a
//! remedy, rather than sending one it knows will be refused.
//!
//! A completed bake is not recorded here: the service has no documents. It is handed to
//! [`Editor::finish_nav_bake`], which records ONE transaction on the world's node. A failed bake
//! records nothing and keeps its diagnostics for `navigation.bake.status`.

use cy_editor_commands::{CommandContext, NavmeshHost, Outcome};
use cy_editor_core::Actor;
use cy_editor_core::ids::{DocumentId, NodeId};
use cy_editor_core::problem::{Problem, Result};
use cy_editor_core::value::Value;
use cy_editor_protocol::{Message, RequestId, ServiceEventKind};

use crate::backend::{
    NAVIGATION_BAKE_OPERATION, NAVIGATION_FLOWFIELD_OPERATION, NAVIGATION_PATH_OPERATION,
    NAVIGATION_PICK_OPERATION, NAVIGATION_STATUS_OPERATION, SERVICE_SCHEMA_VERSION,
};
use crate::editor::Editor;
use crate::nav_bake::{
    NavBakeFailure, NavBakeProgress, NavBakeReport, NavFlowField, NavPathResult, NavPick,
    NavStatusReport,
};
use crate::notifications::Notification;
use crate::runtime::RuntimeSession;

/// The read requests a navigation command may send.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum NavQuery {
    /// `navigation.status`: is the recorded bake stale?
    Status,
    /// `navigation.path.query`.
    Path,
    /// `navigation.flowfield.query`.
    FlowField,
    /// `navigation.point.pick`.
    Pick,
}

impl NavQuery {
    const ALL: [NavQuery; 4] = [
        NavQuery::Status,
        NavQuery::Path,
        NavQuery::FlowField,
        NavQuery::Pick,
    ];

    /// The engine operation.
    #[must_use]
    pub const fn operation(self) -> &'static str {
        match self {
            NavQuery::Status => NAVIGATION_STATUS_OPERATION,
            NavQuery::Path => NAVIGATION_PATH_OPERATION,
            NavQuery::FlowField => NAVIGATION_FLOWFIELD_OPERATION,
            NavQuery::Pick => NAVIGATION_PICK_OPERATION,
        }
    }

    /// The query an operation names, if it is a navigation read.
    #[must_use]
    pub fn from_operation(operation: &str) -> Option<Self> {
        Self::ALL
            .into_iter()
            .find(|query| query.operation() == operation)
    }
}

/// Where a completed bake is recorded.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct BakeTarget {
    /// The scene document.
    pub document: DocumentId,
    /// The node carrying the `NavigationWorld`.
    pub node: NodeId,
    /// Who asked, so the transaction is theirs.
    pub actor: Actor,
}

#[derive(Clone, PartialEq, Eq, Debug)]
enum Pending {
    Bake(RequestId, BakeTarget),
    Query(RequestId, NavQuery),
}

impl Pending {
    const fn request(&self) -> RequestId {
        match self {
            Pending::Bake(request, _) | Pending::Query(request, _) => *request,
        }
    }

    const fn operation(&self) -> &'static str {
        match self {
            Pending::Bake(..) => NAVIGATION_BAKE_OPERATION,
            Pending::Query(_, query) => query.operation(),
        }
    }
}

/// Where the last bake stands.
#[derive(Clone, PartialEq, Eq, Debug)]
pub enum NavBakeState {
    /// No bake has been asked for.
    Idle,
    /// A bake is running; `progress` is its last PROGRESS event.
    Pending {
        /// The request.
        request: RequestId,
        /// The last tile finished, when one has.
        progress: Option<NavBakeProgress>,
    },
    /// The engine completed it.
    Completed {
        /// The request.
        request: RequestId,
        /// What it produced.
        report: NavBakeReport,
    },
    /// The engine refused it, the runtime went, or it could not be recorded.
    Failed {
        /// The request, when one was sent.
        request: Option<RequestId>,
        /// The diagnostics.
        failure: NavBakeFailure,
    },
    /// The engine cancelled it.
    Cancelled {
        /// The request.
        request: RequestId,
    },
}

impl NavBakeState {
    /// `idle`, `pending`, `completed`, `failed` or `cancelled`.
    #[must_use]
    pub const fn name(&self) -> &'static str {
        match self {
            NavBakeState::Idle => "idle",
            NavBakeState::Pending { .. } => "pending",
            NavBakeState::Completed { .. } => "completed",
            NavBakeState::Failed { .. } => "failed",
            NavBakeState::Cancelled { .. } => "cancelled",
        }
    }
}

/// The editor's view of the engine's navigation service.
#[derive(Clone, PartialEq, Debug)]
pub struct NavmeshService {
    pending: Option<Pending>,
    bake: NavBakeState,
    unrecorded: Option<(BakeTarget, NavBakeReport)>,
    status: Option<NavStatusReport>,
    path: Option<NavPathResult>,
    flow_field: Option<NavFlowField>,
    pick: Option<NavPick>,
    pick_answers: u64,
    query_failure: Option<(NavQuery, NavBakeFailure)>,
}

impl Default for NavmeshService {
    fn default() -> Self {
        Self::new()
    }
}

fn refuse_busy(pending: &Pending) -> Problem {
    Problem::new(
        "ask the engine's navigation service",
        format!("{} is still pending", pending.operation()),
    )
    .with_remedy("wait for navigation.bake.status to report it finished")
}

impl NavmeshService {
    /// No request, no bake, no answers.
    #[must_use]
    pub const fn new() -> Self {
        Self {
            pending: None,
            bake: NavBakeState::Idle,
            unrecorded: None,
            status: None,
            path: None,
            flow_field: None,
            pick: None,
            pick_answers: 0,
            query_failure: None,
        }
    }

    /// Send `navigation.bake`; the result is recorded on `target` when it completes.
    pub fn request_bake(
        &mut self,
        runtime: &RuntimeSession,
        target: BakeTarget,
        payload: Vec<u8>,
    ) -> Result<RequestId> {
        if let Some(pending) = &self.pending {
            return Err(refuse_busy(pending));
        }
        let request =
            runtime.service_request(SERVICE_SCHEMA_VERSION, NAVIGATION_BAKE_OPERATION, payload)?;
        self.pending = Some(Pending::Bake(request, target));
        self.bake = NavBakeState::Pending {
            request,
            progress: None,
        };
        Ok(request)
    }

    /// Send one navigation read.
    pub fn request_query(
        &mut self,
        runtime: &RuntimeSession,
        query: NavQuery,
        payload: Vec<u8>,
    ) -> Result<RequestId> {
        if let Some(pending) = &self.pending {
            return Err(refuse_busy(pending));
        }
        let request =
            runtime.service_request(SERVICE_SCHEMA_VERSION, query.operation(), payload)?;
        self.pending = Some(Pending::Query(request, query));
        Ok(request)
    }

    /// Settle a service event for this client's request. Every other message is ignored.
    pub fn accept(&mut self, message: &Message) -> Option<Problem> {
        let Message::ServiceEvent {
            request,
            kind,
            schema_version,
            payload,
        } = message
        else {
            return None;
        };
        if self.pending.as_ref().map(Pending::request) != Some(*request) {
            return None;
        }
        match kind {
            ServiceEventKind::Accepted => None,
            ServiceEventKind::Progress => self.accept_progress(*schema_version, payload),
            ServiceEventKind::Completed
            | ServiceEventKind::Failed
            | ServiceEventKind::Cancelled => {
                let pending = self.pending.take()?;
                if *schema_version != SERVICE_SCHEMA_VERSION {
                    let failure = NavBakeFailure::local(
                        "navigation.schema.unsupported",
                        "the engine answered with an incompatible navigation schema",
                    );
                    return Some(self.fail(&pending, failure));
                }
                match pending {
                    Pending::Bake(request, target) => {
                        self.settle_bake(request, target, *kind, payload)
                    }
                    Pending::Query(_, query) => self.settle_query(query, *kind, payload),
                }
            }
        }
    }

    fn accept_progress(&mut self, schema_version: u32, payload: &[u8]) -> Option<Problem> {
        let NavBakeState::Pending { progress, .. } = &mut self.bake else {
            return None;
        };
        if schema_version != SERVICE_SCHEMA_VERSION
            || !matches!(self.pending, Some(Pending::Bake(..)))
        {
            return None;
        }
        match NavBakeProgress::decode(payload) {
            Ok(decoded) => {
                *progress = Some(decoded);
                None
            }
            Err(problem) => Some(problem),
        }
    }

    fn settle_bake(
        &mut self,
        request: RequestId,
        target: BakeTarget,
        kind: ServiceEventKind,
        payload: &[u8],
    ) -> Option<Problem> {
        let settled = match kind {
            ServiceEventKind::Completed => NavBakeReport::decode(payload).map_err(|problem| {
                NavBakeFailure::local("navigation.result.unreadable", &problem.because)
            }),
            ServiceEventKind::Cancelled => {
                self.bake = NavBakeState::Cancelled { request };
                return None;
            }
            _ => Err(decode_failure(payload)),
        };
        match settled {
            Ok(report) => {
                self.unrecorded = Some((target, report.clone()));
                self.bake = NavBakeState::Completed { request, report };
                None
            }
            Err(failure) => Some(self.fail(&Pending::Bake(request, target), failure)),
        }
    }

    fn settle_query(
        &mut self,
        query: NavQuery,
        kind: ServiceEventKind,
        payload: &[u8],
    ) -> Option<Problem> {
        if query == NavQuery::Pick {
            self.pick_answers += 1;
            self.pick = None;
        }
        let failure = match kind {
            ServiceEventKind::Completed => match self.store_answer(query, payload) {
                Ok(()) => {
                    self.query_failure = None;
                    return None;
                }
                Err(problem) => {
                    NavBakeFailure::local("navigation.result.unreadable", &problem.because)
                }
            },
            ServiceEventKind::Cancelled => {
                NavBakeFailure::local("navigation.cancelled", "the engine cancelled the query")
            }
            _ => decode_failure(payload),
        };
        let problem = Problem::new(query.operation(), failure.summary());
        self.query_failure = Some((query, failure));
        Some(problem)
    }

    fn store_answer(&mut self, query: NavQuery, payload: &[u8]) -> Result<()> {
        match query {
            NavQuery::Status => self.status = Some(NavStatusReport::decode(payload)?),
            NavQuery::Path => self.path = Some(NavPathResult::decode(payload)?),
            NavQuery::FlowField => self.flow_field = Some(NavFlowField::decode(payload)?),
            NavQuery::Pick => self.pick = Some(NavPick::decode(payload)?),
        }
        Ok(())
    }

    fn fail(&mut self, pending: &Pending, failure: NavBakeFailure) -> Problem {
        let problem = Problem::new(pending.operation(), failure.summary());
        match pending {
            Pending::Bake(request, _) => {
                self.bake = NavBakeState::Failed {
                    request: Some(*request),
                    failure,
                };
            }
            Pending::Query(_, query) => self.query_failure = Some((*query, failure)),
        }
        problem
    }

    /// The runtime went: the pending request can no longer end, so it fails now.
    pub fn disconnect(&mut self) {
        if let Some(pending) = self.pending.take() {
            let _ = self.fail(
                &pending,
                NavBakeFailure::local(
                    "service-disconnected",
                    "the runtime disconnected before the navigation request finished",
                ),
            );
        }
    }

    /// The completed bake awaiting its transaction, once.
    pub fn take_completed(&mut self) -> Option<(BakeTarget, NavBakeReport)> {
        self.unrecorded.take()
    }

    /// A completed bake that could not be recorded, such as when its world was removed meanwhile.
    pub fn fail_recording(&mut self, problem: &Problem) {
        let request = match &self.bake {
            NavBakeState::Completed { request, .. } => Some(*request),
            _ => None,
        };
        self.bake = NavBakeState::Failed {
            request,
            failure: NavBakeFailure::local("navigation.bake.unrecorded", &problem.because),
        };
    }

    /// Where the last bake stands.
    #[must_use]
    pub const fn bake_state(&self) -> &NavBakeState {
        &self.bake
    }

    /// The request in flight, if any.
    #[must_use]
    pub fn pending_request(&self) -> Option<RequestId> {
        self.pending.as_ref().map(Pending::request)
    }

    /// The last `navigation.status` answer.
    #[must_use]
    pub const fn status_report(&self) -> Option<&NavStatusReport> {
        self.status.as_ref()
    }

    /// The last test path.
    #[must_use]
    pub const fn path(&self) -> Option<&NavPathResult> {
        self.path.as_ref()
    }

    /// The last flow field.
    #[must_use]
    pub const fn flow_field(&self) -> Option<&NavFlowField> {
        self.flow_field.as_ref()
    }

    /// The last navmesh pick.
    #[must_use]
    pub const fn pick(&self) -> Option<&NavPick> {
        self.pick.as_ref()
    }

    /// How many `navigation.point.pick` requests have ended, answered or refused. A caller that
    /// armed a pick compares it with the count it saw when it sent the request: a larger count
    /// means [`NavmeshService::pick`] now holds that request's answer, or `None` when it failed.
    #[must_use]
    pub const fn pick_answers(&self) -> u64 {
        self.pick_answers
    }

    /// The last query refusal.
    #[must_use]
    pub fn query_failure(&self) -> Option<(NavQuery, &NavBakeFailure)> {
        self.query_failure
            .as_ref()
            .map(|(query, failure)| (*query, failure))
    }

    /// Everything above, for `navigation.bake.status`.
    #[must_use]
    pub fn outcome(&self) -> Outcome {
        let outcome = Outcome::new(format!("Navigation bake {}", self.bake.name()))
            .with("bake", Value::Text(self.bake.name().into()))
            .with(
                "pending",
                Value::Text(self.pending.as_ref().map_or("", Pending::operation).into()),
            );
        let outcome = self.with_bake(outcome);
        let outcome = self.with_status(outcome);
        let outcome = self.with_path(outcome);
        self.with_other_queries(outcome)
    }

    fn with_bake(&self, outcome: Outcome) -> Outcome {
        match &self.bake {
            NavBakeState::Idle => outcome,
            NavBakeState::Pending { request, progress } => {
                let (done, total) =
                    progress.map_or((0, 0), |progress| (progress.done, progress.total));
                outcome
                    .with("request", request_value(*request))
                    .with("done", Value::Int(i64::from(done)))
                    .with("total", Value::Int(i64::from(total)))
            }
            NavBakeState::Completed { request, report } => outcome
                .with("request", request_value(*request))
                .with("report_identity", hex(report.identity))
                .with("report_fingerprint", hex(report.fingerprint))
                .with(
                    "tiles_built",
                    Value::Int(i64::from(report.counters.tiles_built)),
                )
                .with(
                    "tiles_empty",
                    Value::Int(i64::from(report.counters.tiles_empty)),
                )
                .with("polys", Value::Int(i64::from(report.counters.polys)))
                .with("link_failures", Value::Int(i64::from(report.link_failures))),
            NavBakeState::Failed { request, failure } => {
                let outcome = outcome.with("diagnostics", Value::Text(failure.summary()));
                match request {
                    Some(request) => outcome.with("request", request_value(*request)),
                    None => outcome,
                }
            }
            NavBakeState::Cancelled { request } => outcome.with("request", request_value(*request)),
        }
    }

    fn with_status(&self, outcome: Outcome) -> Outcome {
        let Some(status) = &self.status else {
            return outcome;
        };
        outcome
            .with("stale", Value::Bool(status.stale))
            .with("engine_baked", Value::Bool(status.baked))
            .with("sidecar_missing", Value::Bool(status.sidecar_missing))
            .with("engine_identity", hex(status.identity))
            .with("current_fingerprint", hex(status.current_fingerprint))
            .with(
                "resident_tiles",
                Value::Int(i64::from(status.resident_tiles)),
            )
    }

    fn with_path(&self, outcome: Outcome) -> Outcome {
        let Some(path) = &self.path else {
            return outcome;
        };
        let points: Vec<String> = path
            .points
            .iter()
            .map(|point| {
                let [x, y, z] = point.position;
                format!("{x} {y} {z}")
            })
            .collect();
        outcome
            .with("path_found", Value::Bool(path.found))
            .with("path_partial", Value::Bool(path.partial))
            .with("path_cost", Value::Float(path.cost))
            .with("path_points", Value::Text(points.join("; ")))
    }

    fn with_other_queries(&self, outcome: Outcome) -> Outcome {
        let outcome = match &self.pick {
            Some(pick) => outcome
                .with("pick_hit", Value::Bool(pick.hit))
                .with("pick_point", Value::Vec3(pick.point)),
            None => outcome,
        };
        let outcome = match &self.flow_field {
            Some(flow) => outcome
                .with("flow_width", Value::Int(i64::from(flow.width)))
                .with("flow_depth", Value::Int(i64::from(flow.depth)))
                .with("flow_unreachable", Value::Int(i64::from(flow.unreachable))),
            None => outcome,
        };
        match &self.query_failure {
            Some((query, failure)) => outcome.with(
                "query_diagnostics",
                Value::Text(format!("{}: {}", query.operation(), failure.summary())),
            ),
            None => outcome,
        }
    }
}

fn decode_failure(payload: &[u8]) -> NavBakeFailure {
    NavBakeFailure::decode(payload).unwrap_or_else(|problem| {
        NavBakeFailure::local("navigation.failure.unreadable", &problem.because)
    })
}

fn request_value(request: RequestId) -> Value {
    Value::Int(i64::try_from(request.as_u64()).unwrap_or(i64::MAX))
}

fn hex(value: u64) -> Value {
    Value::Text(format!("0x{value:016x}"))
}

impl Editor {
    /// Record a completed bake as one transaction, and fail a pending request whose runtime went.
    /// Called once per frame from [`Editor::pump`], after the frame's messages were accepted.
    pub fn finish_nav_bake(&mut self) {
        if !self.runtime.is_connected() {
            self.navmesh.disconnect();
        }
        let Some((target, report)) = self.navmesh.take_completed() else {
            return;
        };
        // The engine's bake consumed the terrain's stale regions when it committed.
        self.terrain.navigation_rebaked();
        let recorded = self
            .documents
            .get_mut(target.document)
            .ok_or_else(|| {
                Problem::new(
                    "record a navigation bake",
                    "its scene document closed before the bake completed",
                )
            })
            .and_then(|document| {
                crate::navmesh::record_bake(document, target.node, &report, target.actor)
            });
        if let Err(problem) = recorded {
            self.navmesh.fail_recording(&problem);
            self.notifications.post(Notification::error(
                "The navigation bake could not be recorded",
                problem,
            ));
        }
    }
}

impl NavmeshHost for Editor {
    fn bake(&mut self, document: DocumentId, node: NodeId, payload: Vec<u8>) -> Result<u64> {
        let target = BakeTarget {
            document,
            node,
            actor: CommandContext::actor(self),
        };
        self.navmesh
            .request_bake(&self.runtime, target, payload)
            .map(RequestId::as_u64)
    }

    fn query(&mut self, operation: &str, payload: Vec<u8>) -> Result<u64> {
        let query = NavQuery::from_operation(operation).ok_or_else(|| {
            Problem::new(
                "ask the engine's navigation service",
                format!("{operation} is not a navigation read"),
            )
        })?;
        self.navmesh
            .request_query(&self.runtime, query, payload)
            .map(RequestId::as_u64)
    }

    fn status(&self) -> Outcome {
        self.navmesh.outcome()
    }
}

#[cfg(test)]
mod tests {
    use std::time::{Duration, Instant};

    use cy_editor_commands::scope::Scope;
    use cy_editor_commands::{Arguments, Registry};
    use cy_editor_protocol::{Session, read_frame, write_frame};

    use super::*;
    use crate::nav_bake::tests::{
        completed_payload, failure_payload, progress_payload, sample_report,
    };
    use crate::navmesh::NavmeshSettings;

    struct Rig {
        editor: Editor,
        registry: Registry,
        document: DocumentId,
        runtime_reader: std::io::PipeReader,
        runtime_writer: std::io::PipeWriter,
    }

    fn rig() -> Rig {
        let (editor_reader, runtime_writer) = std::io::pipe().unwrap();
        let (runtime_reader, editor_writer) = std::io::pipe().unwrap();
        let mut editor = Editor::new(Actor::human("designer"));
        editor.runtime = RuntimeSession::over(Session::over(editor_reader, editor_writer));
        let document = editor.open_document("worlds/nav.cyworld").unwrap();
        let mut registry = Registry::new();
        crate::builtin::register(&mut registry).unwrap();
        editor
            .invoke(
                &registry,
                "navigation.world.create",
                &Scope::unrestricted(),
                &Arguments::new(),
            )
            .unwrap();
        Rig {
            editor,
            registry,
            document,
            runtime_reader,
            runtime_writer,
        }
    }

    impl Rig {
        fn invoke(&mut self, id: &str) -> Outcome {
            self.editor
                .invoke(
                    &self.registry,
                    id,
                    &Scope::unrestricted(),
                    &Arguments::new(),
                )
                .unwrap()
        }

        /// The next service request the runtime receives, skipping world syncs.
        fn request(&mut self, expected: &str) -> (RequestId, Vec<u8>) {
            loop {
                let frame = read_frame(&mut self.runtime_reader).unwrap().unwrap();
                match Message::decode(&frame).unwrap() {
                    Message::ServiceRequest {
                        request,
                        operation,
                        payload,
                        ..
                    } if operation == expected => return (request, payload),
                    Message::ServiceRequest { .. }
                    | Message::SyncWorld { .. }
                    | Message::Apply { .. } => {}
                    other => panic!("unexpected {other:?}"),
                }
            }
        }

        fn bake(&mut self) -> RequestId {
            let outcome = self.invoke("navigation.bake");
            let (request, payload) = self.request(NAVIGATION_BAKE_OPERATION);
            assert_eq!(outcome.values["request"], request_value(request));
            assert_eq!(
                payload,
                crate::nav_bake::bake_request(1, &crate::nav_bake::NavSettingsBlock::DEFAULT)
            );
            request
        }

        fn history(&self) -> usize {
            self.editor
                .documents
                .get(self.document)
                .unwrap()
                .history()
                .entries()
                .len()
        }

        fn recorded_identity(&self) -> u64 {
            NavmeshSettings::find(self.editor.documents.get(self.document).unwrap(), 1)
                .unwrap()
                .bake_identity
        }

        fn settle(&mut self, message: &Message) -> Option<Problem> {
            let problem = self.editor.navmesh.accept(message);
            self.editor.finish_nav_bake();
            problem
        }
    }

    fn event(request: RequestId, kind: ServiceEventKind, payload: Vec<u8>) -> Message {
        Message::ServiceEvent {
            request,
            kind,
            schema_version: 1,
            payload,
        }
    }

    #[test]
    fn a_mismatched_request_id_is_ignored() {
        let mut rig = rig();
        let request = rig.bake();
        let stranger = RequestId::from_raw(request.as_u64() + 100);
        let report = sample_report(1, 42);
        let message = event(
            stranger,
            ServiceEventKind::Completed,
            completed_payload(&report),
        );
        assert!(rig.settle(&message).is_none());
        assert_eq!(rig.editor.navmesh.pending_request(), Some(request));
        assert_eq!(rig.editor.navmesh.bake_state().name(), "pending");
        assert_eq!(
            rig.history(),
            1,
            "nothing recorded for someone else's event"
        );
    }

    #[test]
    fn progress_updates_the_progress() {
        let mut rig = rig();
        let request = rig.bake();
        let tile = sample_report(1, 1).tiles[0];
        let message = event(
            request,
            ServiceEventKind::Progress,
            progress_payload(1, 3, &tile),
        );
        assert!(rig.settle(&message).is_none());
        let NavBakeState::Pending { progress, .. } = rig.editor.navmesh.bake_state() else {
            panic!("the bake is still pending");
        };
        assert_eq!(
            progress.map(|progress| (progress.done, progress.total)),
            Some((1, 3))
        );
        let status = rig.invoke("navigation.bake.status");
        assert_eq!(status.values["done"], Value::Int(1));
        assert_eq!(status.values["total"], Value::Int(3));
        assert_eq!(rig.history(), 1, "progress records nothing");
    }

    #[test]
    fn completed_records_exactly_one_bake_transaction_and_undo_restores_the_identity() {
        let mut rig = rig();
        let first = rig.bake();
        let message = event(
            first,
            ServiceEventKind::Completed,
            completed_payload(&sample_report(1, 0x11)),
        );
        assert!(rig.settle(&message).is_none());
        assert_eq!(rig.history(), 2, "one bake transaction");
        assert_eq!(rig.recorded_identity(), 0x11);
        rig.editor.finish_nav_bake();
        assert_eq!(rig.history(), 2, "a completion is recorded once");

        let second = rig.bake();
        let report = sample_report(1, 0xFFFF_0000_0000_0022);
        let message = event(
            second,
            ServiceEventKind::Completed,
            completed_payload(&report),
        );
        assert!(rig.settle(&message).is_none());
        assert_eq!(rig.history(), 3);
        assert_eq!(rig.recorded_identity(), 0xFFFF_0000_0000_0022);
        let settings =
            NavmeshSettings::find(rig.editor.documents.get(rig.document).unwrap(), 1).unwrap();
        assert_eq!(settings.source_fingerprint, report.fingerprint);
        assert_eq!(settings.tile_count, 2);
        assert_eq!(settings.sidecar, report.sidecar);

        rig.invoke("edit.undo");
        assert_eq!(
            rig.recorded_identity(),
            0x11,
            "undo restores the previous bake identity"
        );
        rig.invoke("edit.redo");
        assert_eq!(rig.recorded_identity(), 0xFFFF_0000_0000_0022);
    }

    /// Terrain edits and the navmesh share one stale flag: the engine's bake consumes the regions
    /// the terrain flagged, and the editor's copy of them clears with the completed bake. A failed
    /// bake consumes nothing.
    #[test]
    fn a_completed_bake_clears_the_navigation_the_terrain_flagged_stale() {
        let mut rig = rig();
        let terrain = cy_editor_core::ids::NodeId::from_u128(0x29);
        assert!(
            rig.editor
                .terrain
                .maintain(&rig.editor.runtime, Some((terrain, Ok(vec![1]))))
                .is_none()
        );
        let (evaluation, _) = rig.request(crate::terrain_engine::TERRAIN_EVALUATE_OPERATION);
        let stroked = crate::terrain_engine::tests::reply(3, &[[1.0, 2.0, 3.0, 4.0]], 0, &[]);
        assert!(
            rig.editor
                .terrain
                .accept(&event(evaluation, ServiceEventKind::Completed, stroked))
                .is_none()
        );
        assert_eq!(rig.editor.terrain.stale_navigation().len(), 1);

        let failed = rig.bake();
        let refusal = failure_payload("navigation.surface.missing", "no including NavMeshSurface");
        assert!(
            rig.settle(&event(failed, ServiceEventKind::Failed, refusal))
                .is_some()
        );
        assert_eq!(rig.editor.terrain.stale_navigation().len(), 1);

        let baked = rig.bake();
        let completed = completed_payload(&sample_report(1, 0x11));
        assert!(
            rig.settle(&event(baked, ServiceEventKind::Completed, completed))
                .is_none()
        );
        assert!(rig.editor.terrain.stale_navigation().is_empty());
        assert_eq!(
            rig.editor.terrain.status().values["navigation_stale"],
            Value::Bool(false)
        );
    }

    #[test]
    fn failed_records_nothing_and_keeps_the_diagnostics() {
        let mut rig = rig();
        let request = rig.bake();
        let message = event(
            request,
            ServiceEventKind::Failed,
            failure_payload("navigation.surface.missing", "no including NavMeshSurface"),
        );
        let problem = rig.settle(&message).expect("a failure is reported");
        assert!(problem.because.contains("navigation.surface.missing"));
        assert_eq!(rig.history(), 1);
        assert_eq!(rig.recorded_identity(), 0);
        let status = rig.invoke("navigation.bake.status");
        assert_eq!(status.values["bake"], Value::Text("failed".into()));
        assert_eq!(
            status.values["diagnostics"],
            Value::Text("navigation.surface.missing: no including NavMeshSurface".into())
        );
        assert!(rig.editor.navmesh.pending_request().is_none());
    }

    #[test]
    fn a_second_request_while_one_is_pending_is_refused_locally() {
        let mut rig = rig();
        rig.bake();
        let refused = rig
            .editor
            .invoke(
                &rig.registry,
                "navigation.bake",
                &Scope::unrestricted(),
                &Arguments::new(),
            )
            .unwrap_err();
        assert!(refused.because.contains("still pending"), "{refused}");
    }

    #[test]
    fn a_foreign_schema_fails_the_bake_without_recording() {
        let mut rig = rig();
        let request = rig.bake();
        let message = Message::ServiceEvent {
            request,
            kind: ServiceEventKind::Completed,
            schema_version: 2,
            payload: completed_payload(&sample_report(1, 5)),
        };
        assert!(rig.settle(&message).is_some());
        assert_eq!(rig.history(), 1);
        assert_eq!(rig.editor.navmesh.bake_state().name(), "failed");
    }

    #[test]
    fn disconnect_fails_the_pending_request() {
        let mut rig = rig();
        rig.bake();
        drop(rig.runtime_writer);
        let deadline = Instant::now() + Duration::from_secs(5);
        while rig.editor.runtime.is_connected() && Instant::now() < deadline {
            rig.editor.pump();
            std::thread::sleep(Duration::from_millis(1));
        }
        rig.editor.pump();
        let NavBakeState::Failed { failure, .. } = rig.editor.navmesh.bake_state() else {
            panic!(
                "the bake failed with the runtime: {:?}",
                rig.editor.navmesh.bake_state()
            );
        };
        assert_eq!(failure.code, "service-disconnected");
        assert!(rig.editor.navmesh.pending_request().is_none());
    }

    #[test]
    fn a_path_answer_is_reported_by_status() {
        let mut rig = rig();
        rig.editor
            .invoke(
                &rig.registry,
                "navigation.path.query",
                &Scope::unrestricted(),
                &Arguments::new()
                    .with("start", Value::Vec3([0.0, 0.0, 0.0]))
                    .with("end", Value::Vec3([4.0, 0.0, 0.0])),
            )
            .unwrap();
        let (request, payload) = rig.request(NAVIGATION_PATH_OPERATION);
        assert_eq!(
            payload,
            crate::nav_bake::path_request(1, [0.0; 3], [4.0, 0.0, 0.0], [1.0, 2.0, 1.0])
        );
        let mut answer = cy_editor_core::codec::Writer::new();
        answer.u8(1);
        answer.u8(0);
        answer.u8(0);
        answer.f32(4.0);
        answer.u32(3);
        answer.u32(0);
        let message = event(request, ServiceEventKind::Completed, answer.finish());
        assert!(rig.settle(&message).is_none());
        let status = rig.invoke("navigation.bake.status");
        assert_eq!(status.values["path_found"], Value::Bool(true));
        assert_eq!(rig.history(), 1, "a query records nothing");
    }

    fn pick(rig: &mut Rig) -> RequestId {
        rig.editor
            .invoke(
                &rig.registry,
                "navigation.point.pick",
                &Scope::unrestricted(),
                &Arguments::new()
                    .with("frame", Value::Int(7))
                    .with("x", Value::Float(10.0))
                    .with("y", Value::Float(20.0)),
            )
            .unwrap();
        rig.request(NAVIGATION_PICK_OPERATION).0
    }

    #[test]
    fn every_ended_pick_is_counted_and_a_refused_one_leaves_no_stale_point() {
        let mut rig = rig();
        assert_eq!(rig.editor.navmesh.pick_answers(), 0);
        let request = pick(&mut rig);
        let mut answer = cy_editor_core::codec::Writer::new();
        answer.u8(1);
        for lane in [1.0_f32, 0.0, 2.0] {
            answer.f32(lane);
        }
        answer.u64(9);
        answer.f32(5.0);
        let message = event(request, ServiceEventKind::Completed, answer.finish());
        assert!(rig.settle(&message).is_none());
        assert_eq!(rig.editor.navmesh.pick_answers(), 1);
        assert_eq!(
            rig.editor.navmesh.pick().map(|pick| pick.point),
            Some([1.0, 0.0, 2.0])
        );

        let request = pick(&mut rig);
        let refused = event(
            request,
            ServiceEventKind::Failed,
            failure_payload("navigation.point.pick.ray-failed", "no frame"),
        );
        assert!(rig.settle(&refused).is_some());
        assert_eq!(rig.editor.navmesh.pick_answers(), 2);
        assert!(
            rig.editor.navmesh.pick().is_none(),
            "a refused pick left the previous point to be read as its answer"
        );
    }

    #[test]
    fn the_runtime_pipe_carries_a_whole_bake_through_pump() {
        let mut rig = rig();
        let request = rig.bake();
        let tile = sample_report(1, 9).tiles[0];
        for message in [
            event(
                request,
                ServiceEventKind::Progress,
                progress_payload(1, 2, &tile),
            ),
            event(
                request,
                ServiceEventKind::Completed,
                completed_payload(&sample_report(1, 9)),
            ),
        ] {
            write_frame(&mut rig.runtime_writer, &message.encode()).unwrap();
        }
        let deadline = Instant::now() + Duration::from_secs(5);
        while rig.recorded_identity() != 9 && Instant::now() < deadline {
            rig.editor.pump();
            std::thread::sleep(Duration::from_millis(1));
        }
        assert_eq!(rig.recorded_identity(), 9);
        assert_eq!(rig.history(), 2);
    }
}
