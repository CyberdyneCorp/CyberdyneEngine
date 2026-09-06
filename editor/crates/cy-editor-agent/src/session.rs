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
use crate::recording::{RecordedInvocation, Recording};
use crate::tool::{Exclusions, ToolDescriptor, project};

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

/// One agent's connection to the editor.
pub struct AgentSession {
    identity: AgentIdentity,
    intent: String,
    scope: Scope,
    exclusions: Exclusions,
    spending: Spending,
    grants: Vec<Grant>,
    recording: Recording,
    paused: bool,
    revoked: bool,
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
            exclusions: Exclusions::new(),
            spending: Spending::new(budget, now_millis),
            grants: Vec::new(),
            paused: false,
            revoked: false,
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

    /// The commands this connection is not offered, and why.
    #[must_use]
    pub const fn exclusions(&self) -> &Exclusions {
        &self.exclusions
    }

    /// The commands this connection is not offered, mutably.
    pub const fn exclusions_mut(&mut self) -> &mut Exclusions {
        &mut self.exclusions
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
    #[must_use]
    pub fn tools(&self, registry: &Registry) -> Vec<ToolDescriptor> {
        project(registry, &self.exclusions)
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

    fn attempt(
        &mut self,
        editor: &mut Editor,
        registry: &Registry,
        confirmer: &mut dyn Confirmer,
        command: &str,
        arguments: &Arguments,
        now_millis: u64,
    ) -> Result<Outcome> {
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
        if let Some(reason) = self.exclusions.reason(command) {
            return Err(Problem::new(
                format!("invoke {command}"),
                format!("this command is not offered to agents: {reason}"),
            )
            .with_remedy("ask a person to do it, or use a command that is offered"));
        }

        let metadata = registry.metadata(command).ok_or_else(|| {
            Problem::not_found(format!("a command named {command:?}"))
                .with_remedy("list the tools to see what there is")
        })?;
        let effect = metadata.effect;

        self.spending.charge_invocation(now_millis)?;

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
        let outcome = editor.invoke(registry, command, &self.scope, arguments);
        editor.acting_as(previous);
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
