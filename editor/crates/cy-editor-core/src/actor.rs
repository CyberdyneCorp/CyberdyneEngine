//! Who produced a change. Task 3.8, `editor-documents-and-transactions` and `editor-agent-interface`.
//!
//! "Every transaction SHALL record the **actor** that produced it — the human user, or the agent,
//! session and stated intent defined in `editor-agent-interface`. Attribution SHALL be visible
//! wherever history is: the undo stack, the journal, and semantic diff."
//!
//! And, explicitly: "This is not a security control and SHALL NOT be treated as one. It answers
//! 'who changed this, and what were they trying to do' — a question every collaborator already has,
//! and which an unattributed history cannot answer at all."
//!
//! Nothing in this workspace makes an authorisation decision on the basis of an [`Actor`]. Scope
//! enforcement is a property of a *connection*, in `cy-editor-commands`, and it is deliberately a
//! different type: an actor is a claim about who is acting, and a scope is a grant. Conflating them
//! is how attribution becomes a security control by accident.

use std::fmt;

/// Who produced a transaction.
#[derive(Clone, PartialEq, Eq, Debug)]
pub enum Actor {
    /// A person, working through the interface.
    Human {
        /// The user's name as the editor knows it.
        name: String,
    },
    /// An agent, with the session it is connected on and the intent it stated.
    ///
    /// All three are required. An agent edit with no intent is exactly the history entry a reviewer
    /// cannot act on — they can see *that* something changed and not *what it was for* — so the
    /// type has no way to spell one.
    Agent {
        /// The agent's identity: which agent, not which vendor.
        agent: String,
        /// The connection's session identifier, so a run's edits can be found together.
        session: String,
        /// What the agent stated it was trying to achieve.
        intent: String,
    },
    /// The editor itself: a migration, a repair, a recovered journal being replayed.
    ///
    /// Present so that a change with no human or agent behind it is still attributed rather than
    /// falling into a fourth, unnamed case where attribution is optional.
    System {
        /// Which part of the editor produced it.
        component: String,
    },
}

impl Actor {
    /// A human actor.
    pub fn human(name: impl Into<String>) -> Self {
        Actor::Human { name: name.into() }
    }

    /// An agent actor, with its session and its stated intent.
    pub fn agent(
        agent: impl Into<String>,
        session: impl Into<String>,
        intent: impl Into<String>,
    ) -> Self {
        Actor::Agent {
            agent: agent.into(),
            session: session.into(),
            intent: intent.into(),
        }
    }

    /// The editor acting on its own behalf.
    pub fn system(component: impl Into<String>) -> Self {
        Actor::System {
            component: component.into(),
        }
    }

    /// Whether an agent produced this. A reviewer's first question about a history entry.
    #[must_use]
    pub const fn is_agent(&self) -> bool {
        matches!(self, Actor::Agent { .. })
    }

    /// The intent, when the actor stated one. Only an agent does.
    #[must_use]
    pub fn intent(&self) -> Option<&str> {
        match self {
            Actor::Agent { intent, .. } => Some(intent),
            _ => None,
        }
    }
}

impl fmt::Display for Actor {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Actor::Human { name } => write!(f, "{name}"),
            Actor::Agent {
                agent,
                session,
                intent,
            } => {
                write!(f, "{agent} (session {session}) — {intent}")
            }
            Actor::System { component } => write!(f, "the editor ({component})"),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn an_agents_attribution_carries_its_intent() {
        let actor = Actor::agent("claude", "s-17", "raise the streetlights to 4 m");
        assert!(actor.is_agent());
        assert_eq!(actor.intent(), Some("raise the streetlights to 4 m"));
        assert_eq!(
            actor.to_string(),
            "claude (session s-17) — raise the streetlights to 4 m"
        );
    }

    #[test]
    fn a_human_has_no_intent_to_state() {
        let actor = Actor::human("designer");
        assert!(!actor.is_agent());
        assert_eq!(actor.intent(), None);
    }
}
