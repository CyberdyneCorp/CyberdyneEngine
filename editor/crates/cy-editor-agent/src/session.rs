//! A connection: who is acting, what they may do, and what it cost.
//!
//! This is where the specification's central claim becomes code:
//!
//! > An agent SHALL have **no capability a human user does not have**, and SHALL reach every
//! > capability through the same mechanisms — the command registry, the transaction system, the
//! > selection service, and the engine's own renderer. There SHALL be no agent-only mutation path,
//! > no agent-only query path, and no bypass of a check a human interaction is subject to.
//!
//! [`AgentSession::invoke`] is nine lines of policy around one call to `Registry::invoke`. Everything
//! it adds — the scope, the budget, the confirmation, the recording — is a RESTRICTION. There is no
//! branch in it that does something a human's invocation does not, which is what makes "an agent
//! cannot do what a person cannot" true by inspection rather than by audit.
//!
//! # Attribution, and why it is not authorisation
//!
//! The session sets the editor's actor to [`cy_editor_core::Actor::Agent`] for the duration of an
//! invocation and restores it afterwards, so every transaction the invocation produces names the
//! agent, its session and its stated intent. That is attribution: it answers "who changed this and
//! what were they trying to do".
//!
//! It is emphatically not the permission check. The permission check is
//! [`cy_editor_commands::Scope`], which is a GRANT and a different type, held by the connection
//! rather than claimed by the actor. `cy_editor_core::actor` makes the same argument at greater
//! length, and the reason both files make it is that conflating the two is how attribution becomes a
//! security control by accident — at which point forging an actor becomes worth doing.

use std::collections::BTreeMap;

use cy_editor_commands::context::{CommandContext, Outcome};
use cy_editor_commands::metadata::EffectClass;
use cy_editor_commands::registry::{Arguments, Registry};
use cy_editor_commands::scope::Scope;
use cy_editor_core::Actor;
use cy_editor_core::problem::{Problem, Result};
use cy_editor_core::value::Value;
use cy_editor_services::editor::Editor;

use crate::budget::{Budget, BudgetReport, Spending};
use crate::conflict::{Claim, Conflict};
use crate::observe::{Observation, ViewportRequest, observe};
use crate::recording::{RecordedInvocation, Recording};
use crate::resource::{Resource, ResourceKind, Resources};
use crate::tool::{ToolDescriptor, project};
use cy_editor_viewport::viewmode::{ALL_VIEW_MODES, ViewMode};

/// Who is connected.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct AgentIdentity {
    /// Which agent — not which vendor. What a history entry names.
    pub agent: String,
    /// This connection's session identifier, so a run's edits can be found together.
    pub session: String,
}

/// What a human is being asked to allow.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Confirmation {
    /// Who is asking.
    pub agent: String,
    /// Which command.
    pub command: String,
    /// What class of consequence it has.
    pub effect: EffectClass,
    /// What will happen, in the words the command uses about itself.
    pub what_happens: String,
    /// What will be lost, which `editor-ui-ux` requires a destructive confirmation to state.
    pub what_is_lost: String,
}

/// A human's answer.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Decision {
    /// Go ahead, this once.
    Allow,
    /// Do not.
    Refuse,
}

/// Whoever asks the human.
///
/// A trait rather than a callback so that a test's answer is a type rather than a closure with
/// captured state, and so that a headless session can supply one that always refuses — which is what
/// makes "an irreversible operation performed without confirmation" a thing a test can prove does
/// not happen.
pub trait Confirmer {
    /// Ask.
    fn confirm(&mut self, confirmation: &Confirmation) -> Decision;
}

/// A confirmer that always refuses.
///
/// The default, and the right one: a connection with nobody at the interface must not be able to
/// delete anything, and the failure mode of forgetting to supply a confirmer should be that nothing
/// irreversible happens.
#[derive(Clone, Copy, Debug, Default)]
pub struct RefuseEverything;

impl Confirmer for RefuseEverything {
    fn confirm(&mut self, _confirmation: &Confirmation) -> Decision {
        Decision::Refuse
    }
}

/// A deliberate grant of an effect class, for a stated duration.
///
/// `editor-agent-interface`: an irreversible operation requires confirmation "unless the connection
/// has been granted that effect class **deliberately and for a stated duration**". The duration is
/// an invocation count rather than a clock: a grant that expired after five minutes would either
/// still be open when the human walked away or close in the middle of a batch, and neither is what
/// "I am watching this run" means. A count is what a human can actually reason about.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Grant {
    /// The class granted.
    pub effect: EffectClass,
    /// How many more invocations of that class it covers.
    pub remaining: u32,
}

/// What a read produced.
///
/// Two shapes because a resource is text and an observation is an image, and a transport has to
/// carry them differently. They are one enum rather than two calls because they answer one question.
#[derive(Clone, PartialEq, Debug)]
pub enum Reading {
    /// Something the editor can say in words.
    Text(Resource),
    /// Something it can only show.
    Image(Box<Observation>),
}

/// One agent's connection to the editor.
pub struct AgentSession {
    identity: AgentIdentity,
    intent: String,
    scope: Scope,
    spending: Spending,
    grants: Vec<Grant>,
    recording: Recording,
    paused: bool,
    revoked: bool,
    claim: Option<Claim>,
    viewport: Option<cy_editor_viewport::viewport::ViewportId>,
    running: Vec<std::sync::Arc<cy_editor_core::progress::Operation>>,
}

impl AgentSession {
    /// Open a connection.
    ///
    /// The scope is supplied rather than defaulted to something useful, because
    /// `cy_editor_commands::Scope::default` is read-only over nothing and the specification requires
    /// exactly that: "Scope SHALL default to the narrowest useful setting rather than to full
    /// access."
    #[must_use]
    pub fn new(
        identity: AgentIdentity,
        intent: impl Into<String>,
        scope: Scope,
        budget: Budget,
        starting_revision: impl Into<String>,
        now_millis: u64,
    ) -> Self {
        let intent = intent.into();
        Self {
            identity,
            recording: Recording::new(starting_revision, intent.clone()),
            intent,
            scope,
            spending: Spending::new(budget, now_millis),
            grants: Vec::new(),
            paused: false,
            revoked: false,
            claim: None,
            viewport: None,
            running: Vec::new(),
        }
    }

    /// Who is connected.
    #[must_use]
    pub const fn identity(&self) -> &AgentIdentity {
        &self.identity
    }

    /// What it says it is trying to do. Recorded on every transaction it produces.
    #[must_use]
    pub fn intent(&self) -> &str {
        &self.intent
    }

    /// Restate the intent, for an agent that has moved on to the next thing.
    ///
    /// Worth having rather than making the caller open a second session: a run's edits belong
    /// together, and a history in which one session's entries carry three different intents is more
    /// useful than three sessions nobody can tell apart.
    pub fn state_intent(&mut self, intent: impl Into<String>) {
        self.intent = intent.into();
    }

    /// What the connection may do.
    #[must_use]
    pub const fn scope(&self) -> &Scope {
        &self.scope
    }

    /// What it has spent, and what is left.
    #[must_use]
    pub fn budget(&self, now_millis: u64) -> BudgetReport {
        self.spending.report(now_millis)
    }

    /// The background work this connection started that has not settled.
    ///
    /// `editor-agent-interface` bounds "the number of concurrent operations", and a bound needs
    /// something to count. An operation is counted from the invocation that started it until it
    /// settles, which is measured from the operation itself rather than remembered — a session that
    /// kept its own idea of what was running would eventually disagree with the operation service
    /// about it, and disagree in the direction that leaks slots.
    #[must_use]
    pub fn operations_running(&self) -> usize {
        self.running
            .iter()
            .filter(|operation| !operation.state().is_settled())
            .count()
    }

    /// Give back the slots held by work that has finished.
    fn settle(&mut self) {
        let before = self.running.len();
        self.running
            .retain(|operation| !operation.state().is_settled());
        for _ in self.running.len()..before {
            self.spending.end_operation();
        }
    }

    /// The session so far.
    #[must_use]
    pub const fn recording(&self) -> &Recording {
        &self.recording
    }

    /// Grant an effect class deliberately, for a stated number of invocations.
    pub fn grant(&mut self, effect: EffectClass, invocations: u32) {
        if let Some(existing) = self.grants.iter_mut().find(|grant| grant.effect == effect) {
            existing.remaining = existing.remaining.saturating_add(invocations);
            return;
        }
        self.grants.push(Grant {
            effect,
            remaining: invocations,
        });
    }

    /// Stop the agent without disconnecting it.
    ///
    /// "The human SHALL be able to see that an agent is connected, see what it is currently doing,
    /// **pause it**, and revoke its access mid-session, without restarting the editor or losing
    /// work."
    pub const fn pause(&mut self) {
        self.paused = true;
    }

    /// Let it go again.
    pub const fn resume(&mut self) {
        self.paused = false;
    }

    /// Whether it is paused.
    #[must_use]
    pub const fn is_paused(&self) -> bool {
        self.paused
    }

    /// Revoke access, permanently for this session.
    ///
    /// "WHEN access is revoked mid-operation THEN the in-flight transaction SHALL be abandoned
    /// rather than half-applied." The abandonment is the caller's — it holds the document — and this
    /// is the half that makes every subsequent invocation refuse.
    pub const fn revoke(&mut self) {
        self.revoked = true;
    }

    /// Whether access has been revoked.
    #[must_use]
    pub const fn is_revoked(&self) -> bool {
        self.revoked
    }

    /// The tools this connection sees.
    ///
    /// Every registered command, including the ones it may not invoke, each carrying why. The scope
    /// is not applied here on purpose: a tool an agent cannot invoke *under its current grant* is
    /// still a tool a human could widen the grant for, and hiding it would make the refusal
    /// unactionable.
    #[must_use]
    pub fn tools(&self, registry: &Registry) -> Vec<ToolDescriptor> {
        project(registry)
    }

    /// Everything this connection can read, including its own budget.
    ///
    /// The budget is listed beside the editor's own resources rather than answered by a separate
    /// call, so that "the interface SHALL report those limits to the agent" is satisfied by the
    /// mechanism the agent is already using rather than by one more thing to know about.
    #[must_use]
    pub fn resources(&self, editor: &Editor) -> Vec<(String, ResourceKind, String)> {
        let mut listing = Resources::list(editor);
        listing.push((
            "budget:".to_string(),
            ResourceKind::Budget,
            "What this connection may spend, what it has spent, and when the window resets."
                .to_string(),
        ));
        listing.push((
            "viewport:".to_string(),
            ResourceKind::Viewport,
            "The engine's rendered image with nothing drawn over it — what the project will \
             actually look like. Costs one of this connection's renders."
                .to_string(),
        ));
        listing.push((
            "viewport:overlays".to_string(),
            ResourceKind::Viewport,
            "The same image with the editor's own drawing on it: gizmos, selection outlines and \
             the overlays. Not what the project looks like."
                .to_string(),
        ));
        listing.push((
            "viewport:Normals".to_string(),
            ResourceKind::Viewport,
            "A debug visualisation. Any of the engine's view modes may be named after the colon; \
             read the tools for the full list."
                .to_string(),
        ));
        listing
    }

    /// Read one resource, by its address.
    ///
    /// `budget:` is this connection's; everything else is the editor's, read through the same
    /// services its own panels read.
    pub fn read_resource(&self, editor: &Editor, uri: &str, now_millis: u64) -> Result<Resource> {
        if uri == "budget:" {
            return Ok(Resource {
                uri: uri.to_string(),
                kind: ResourceKind::Budget,
                description: "This connection's limits and what is left of them.".to_string(),
                content: format!(
                    "{}\nagent: {}\nsession: {}\nintent: {}\nscope: {}\n",
                    self.budget(now_millis).describe(),
                    self.identity.agent,
                    self.identity.session,
                    self.intent,
                    self.scope.name
                ),
            });
        }
        Resources::read(editor, uri)
    }

    /// Read anything this connection can see, including what it can look at.
    ///
    /// The one entry point a transport needs. `viewport:` addresses cost a render and are answered
    /// with an image; everything else is text and costs nothing. They are one call rather than two
    /// because they are one question — "show me X" — and a transport that had to know which kind an
    /// address was would be a second place the read surface is enumerated.
    ///
    /// The addresses are `viewport:` for the shipping frame, `viewport:overlays` for the editor's
    /// own image, and `viewport:<debug view>` for a buffer — the same names
    /// `cy_editor_viewport::viewmode::ViewMode::engine_name` gives, so a caller that read the
    /// `viewport.view-mode.*` commands already knows them.
    pub fn read(&mut self, editor: &mut Editor, uri: &str, now_millis: u64) -> Result<Reading> {
        let Some(rest) = uri.strip_prefix("viewport:") else {
            return self
                .read_resource(editor, uri, now_millis)
                .map(Reading::Text);
        };
        let mut request = ViewportRequest::shipping_frame(editor.viewports.focused());
        match rest {
            "" => {}
            "overlays" => request.include_overlays = true,
            name => {
                request.view_mode = ALL_VIEW_MODES
                    .into_iter()
                    .find(|mode| mode.engine_name().eq_ignore_ascii_case(name))
                    .ok_or_else(|| {
                        Problem::new(
                            format!("read {uri:?}"),
                            format!("there is no debug view called {name:?}"),
                        )
                        .with_remedy(format!(
                            "the views are: {}",
                            ALL_VIEW_MODES
                                .into_iter()
                                .map(ViewMode::engine_name)
                                .collect::<Vec<_>>()
                                .join(", ")
                        ))
                    })?;
            }
        }
        self.observe(editor, &request, now_millis)
            .map(|observation| Reading::Image(Box::new(observation)))
    }

    /// Stake a claim on the objects this connection is about to work on.
    ///
    /// `editor-agent-interface`: "Where a human action and an agent action conflict, **the human
    /// action SHALL win**, and the agent SHALL be told its operation was superseded and why." A
    /// claim is what makes "conflict" answerable: without one, an agent's edit and a human's edit
    /// are two edits in one history and neither is superseded by anything. See [`crate::conflict`].
    pub fn claim(&mut self, editor: &Editor, nodes: &[cy_editor_core::ids::NodeId]) -> Result<()> {
        self.claim = Some(Claim::stake(editor, nodes)?);
        Ok(())
    }

    /// Give the claim up, for an agent that has finished with those objects.
    pub fn release_claim(&mut self) {
        self.claim = None;
    }

    /// What the connection is working on, when it has said.
    #[must_use]
    pub const fn claimed(&self) -> Option<&Claim> {
        self.claim.as_ref()
    }

    /// Look through a viewport.
    ///
    /// Charged against the render half of the budget, which is separate from the invocation half
    /// because a render costs a frame of the editor's own budget and an invocation usually costs
    /// nothing. `editor-agent-interface` requires an agent's render to be "scheduled against the same
    /// frame budget the editor's own viewport holds"; this is the ceiling on how often one may be
    /// asked for, and `cy_editor_viewport::budget` is the scheduling.
    pub fn observe(
        &mut self,
        editor: &mut Editor,
        request: &ViewportRequest,
        now_millis: u64,
    ) -> Result<Observation> {
        if self.revoked {
            return Err(Problem::new(
                "observe a viewport",
                "this connection's access has been revoked",
            )
            .with_remedy("open a new connection; the human at the interface decides"));
        }
        self.spending.charge_render(now_millis)?;
        observe(editor, request, &mut self.viewport)
    }

    /// Invoke a command as this agent.
    ///
    /// In order: the connection must be live and not paused; the command must not be excluded; the
    /// budget must admit it; an irreversible or external effect must be confirmed or granted; and
    /// then `Registry::invoke` applies the scope, validates the arguments and consults the
    /// availability predicate — exactly as it does for a person.
    ///
    /// Every outcome, including every refusal, is recorded. A refusal is part of the session: a
    /// replay in which a refused command now succeeds has found a change in behaviour.
    pub fn invoke(
        &mut self,
        editor: &mut Editor,
        registry: &Registry,
        confirmer: &mut dyn Confirmer,
        command: &str,
        arguments: &Arguments,
        now_millis: u64,
    ) -> Result<Outcome> {
        let supplied: BTreeMap<String, Value> = arguments
            .names()
            .filter_map(|name| {
                arguments
                    .get(name)
                    .map(|value| (name.to_string(), value.clone()))
            })
            .collect();

        let result = self.attempt(editor, registry, confirmer, command, arguments, now_millis);
        let mut recorded = RecordedInvocation {
            command: command.to_string(),
            arguments: supplied,
            summary: String::new(),
            transactions: Vec::new(),
            refusal: None,
        };
        match &result {
            Ok(outcome) => {
                recorded.summary.clone_from(&outcome.summary);
                // The transactions this invocation produced, taken from the document's own history
                // rather than from anything this crate keeps: an agent's edits are in the same
                // history a human's are, and reading them from anywhere else would be a second
                // record to disagree with the first.
                if let Some(last) = editor
                    .workspace
                    .active()
                    .and_then(|id| editor.documents.get(id))
                    .and_then(|document| document.history().entries().last())
                {
                    recorded.transactions.push(format!("{:?}", last.id));
                }
            }
            Err(problem) => recorded.refusal = Some(problem.because.clone()),
        }
        self.recording.record(recorded);
        result
    }

    /// Whether this connection may act at all, before any particular command is considered.
    ///
    /// Revocation, pausing and supersession, in that order, and all three before anything is spent:
    /// an operation that was never going to happen must not cost the connection the invocation it
    /// could have used on the corrected one.
    fn may_act(&mut self, editor: &Editor, command: &str) -> Result<()> {
        if self.revoked {
            return Err(Problem::new(
                format!("invoke {command}"),
                "this connection's access has been revoked",
            )
            .with_remedy("open a new connection; the human at the interface decides"));
        }
        if self.paused {
            return Err(
                Problem::new(format!("invoke {command}"), "this connection is paused")
                    .with_remedy("wait: the human at the interface paused it and can resume it"),
            );
        }
        if let Some(claim) = &self.claim
            && let Conflict::Superseded { by, what, nodes } = claim.check(editor)
        {
            self.claim = None;
            return Err(Problem::new(
                format!("invoke {command}"),
                format!(
                    "a person changed {} of the objects this connection claimed, so the operation \
                     was superseded by {what:?} ({by})",
                    nodes.len()
                ),
            )
            .with_remedy(
                "read the document again and claim the objects afresh; the human's change stands",
            ));
        }
        Ok(())
    }

    fn attempt(
        &mut self,
        editor: &mut Editor,
        registry: &Registry,
        confirmer: &mut dyn Confirmer,
        command: &str,
        arguments: &Arguments,
        now_millis: u64,
    ) -> Result<Outcome> {
        self.may_act(editor, command)?;

        let metadata = registry.metadata(command).ok_or_else(|| {
            Problem::not_found(format!("a command named {command:?}"))
                .with_remedy("list the tools to see what there is")
        })?;
        if let Some(reason) = metadata.agent_exclusion() {
            return Err(Problem::new(
                format!("invoke {command}"),
                format!("this command is not offered to agents: {reason}"),
            )
            .with_remedy("ask a person to do it, or use a command that is offered"));
        }

        // The class this INVOCATION has, which for a command that computes it is narrower than the
        // one it declares — see `design.md` §4 on writing source. Asking for the declared class
        // here would prompt a human for an edit undo already covers, which is the habit the
        // confirmation rule exists to avoid.
        let effect = registry
            .effect_of(command, editor, arguments)
            .unwrap_or(metadata.effect);

        // What this connection started and has since finished gives its slots back, before anything
        // asks for one. Doing it here rather than on a timer means the accounting is exact at every
        // point a decision is made, and costs nothing when there is no background work.
        self.settle();

        // The scope is checked BEFORE anybody is asked to confirm. `Registry::invoke` checks it too
        // and would refuse the same call a moment later, but a moment later is after a human has
        // been prompted about an operation the connection was never allowed to perform — which
        // trains the person to approve without reading, which is the habit
        // `editor-agent-interface`'s confirmation rule exists to prevent.
        self.scope.admit_effect(metadata, effect)?;

        self.spending.charge_invocation(now_millis)?;

        // A concurrency slot is taken BEFORE the invocation and given back immediately if it turned
        // out to start no background work.
        //
        // Reserving rather than counting afterwards is what makes the bound a bound: nothing can
        // say in advance whether a command will start a build, so an accounting that only charged
        // once work existed could never refuse the invocation that exceeded the ceiling — it could
        // only notice. A read is exempt and always is: an agent has to be able to poll the
        // operations resource to find out when its own work finished, and a bound that blocked that
        // would deadlock the very agent it was protecting the editor from.
        let reserves = effect != EffectClass::Read;
        if reserves {
            self.spending.begin_operation(now_millis)?;
        }

        if effect.needs_confirmation() && !self.spend_grant(effect) {
            let confirmation = Confirmation {
                agent: self.identity.agent.clone(),
                command: command.to_string(),
                effect,
                what_happens: metadata.description.clone(),
                what_is_lost: match effect {
                    EffectClass::IrreversibleMutation => "undo cannot put this back".to_string(),
                    _ => "this reaches outside the editor and cannot be taken back".to_string(),
                },
            };
            if confirmer.confirm(&confirmation) == Decision::Refuse {
                if reserves {
                    self.spending.end_operation();
                }
                return Err(Problem::new(
                    format!("invoke {command}"),
                    format!(
                        "it is an {} and the human did not confirm it",
                        effect.name()
                    ),
                )
                .with_remedy(
                    "ask a person to confirm, or have the connection granted that effect class \
                     deliberately",
                ));
            }
        }

        // Act as the agent for the duration, then put the editor's own actor back. Every
        // transaction produced in between names the agent, its session and its stated intent, which
        // is the whole of the attribution requirement.
        let previous = editor.actor();
        editor.acting_as(Actor::agent(
            self.identity.agent.clone(),
            self.identity.session.clone(),
            self.intent.clone(),
        ));
        let before: Vec<*const cy_editor_core::progress::Operation> = editor
            .operations
            .all()
            .iter()
            .map(std::sync::Arc::as_ptr)
            .collect();
        let outcome = editor.invoke(registry, command, &self.scope, arguments);
        editor.acting_as(previous);

        // Whatever this invocation started is this connection's until it settles. Compared by
        // pointer rather than by label, because two builds of the same module have the same label
        // and are different operations.
        let started: Vec<_> = editor
            .operations
            .all()
            .iter()
            .filter(|operation| {
                !before.contains(&std::sync::Arc::as_ptr(operation))
                    && !operation.state().is_settled()
            })
            .map(std::sync::Arc::clone)
            .collect();
        if reserves {
            if started.is_empty() {
                // It started nothing, so the slot it reserved goes straight back. Holding it would
                // make an agent that only ever edits run out of concurrency it never used.
                self.spending.end_operation();
            } else {
                // One slot was reserved and one is kept, whichever of the started operations is
                // held: an invocation that started three would be a command doing something this
                // accounting does not model, and taking three slots for it would be inventing a
                // rule nobody wrote down.
                self.running.push(started[0].clone());
            }
        }
        outcome
    }

    /// Spend one invocation of a deliberate grant, if there is one.
    fn spend_grant(&mut self, effect: EffectClass) -> bool {
        let Some(grant) = self
            .grants
            .iter_mut()
            .find(|grant| grant.effect == effect && grant.remaining > 0)
        else {
            return false;
        };
        grant.remaining -= 1;
        true
    }
}
