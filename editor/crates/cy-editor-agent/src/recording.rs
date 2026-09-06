//! An agent session, as a replayable command log.
//!
//! `editor-agent-interface`, "An agent session is reproducible":
//!
//! > An agent session SHALL be recorded as the sequence of commands it invoked, with their arguments
//! > and their transaction identifiers. The recording SHALL be replayable against the same starting
//! > state to produce the same result, for the same reason `replay-and-rollback` makes one command
//! > log serve replay, rollback and lockstep: a session that can be replayed can be reviewed,
//! > bisected, tested and reported as a defect. A session recording SHALL be exportable and SHALL
//! > carry the project revision it began from.
//!
//! # Why the recording is the invocations and not the outcomes
//!
//! A log of what CHANGED would replay by writing the same values, which reproduces the result and
//! not the session: a command whose behaviour depended on state the log did not capture would
//! replay as its old effect and hide the very defect somebody is bisecting for. A log of what was
//! INVOKED replays through the same registry, the same availability predicates and the same
//! transactions — so a replay that diverges has found something, which is what a replay is for.
//!
//! Outcomes are recorded too, but as a witness rather than as the instruction: a replay compares
//! against them and reports the first that differs.

use std::collections::BTreeMap;
use std::fmt::Write as _;

use cy_editor_core::value::Value;

/// One invocation, as it happened.
#[derive(Clone, PartialEq, Debug)]
pub struct RecordedInvocation {
    /// The command's identifier.
    pub command: String,
    /// The arguments as supplied, before defaults were filled in — because a replay must exercise
    /// the same defaulting the original did, and a log of the resolved arguments would freeze
    /// today's defaults into a session replayed next year.
    pub arguments: BTreeMap<String, Value>,
    /// What the invocation produced, as a witness for a replay to compare against.
    pub summary: String,
    /// The transactions it produced, in order. Empty for a command that changed nothing, which is
    /// itself worth recording: a read that unexpectedly produced a transaction is a defect the log
    /// makes visible.
    pub transactions: Vec<String>,
    /// Whether it was refused, and why. A refusal is part of the session: a replay in which a
    /// command that was refused now succeeds has found a change in behaviour.
    pub refusal: Option<String>,
}

/// A whole session.
#[derive(Clone, PartialEq, Debug, Default)]
pub struct Recording {
    /// The project revision the session began from. Compared before a replay, because replaying
    /// against a different starting state proves nothing.
    pub starting_revision: String,
    /// What the agent said it was trying to do.
    pub intent: String,
    /// The invocations, in order.
    pub invocations: Vec<RecordedInvocation>,
}

impl Recording {
    /// A recording of a session that began at `revision`, toward `intent`.
    #[must_use]
    pub fn new(revision: impl Into<String>, intent: impl Into<String>) -> Self {
        Self {
            starting_revision: revision.into(),
            intent: intent.into(),
            invocations: Vec::new(),
        }
    }

    /// Append one.
    pub fn record(&mut self, invocation: RecordedInvocation) {
        self.invocations.push(invocation);
    }

    /// How many invocations it holds.
    #[must_use]
    pub fn len(&self) -> usize {
        self.invocations.len()
    }

    /// Whether nothing was invoked.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.invocations.is_empty()
    }

    /// A text export: the revision, the intent, then one line per invocation.
    ///
    /// Text and line-oriented for the reason the engine's own canonical scene form is: a recording
    /// is attached to a defect report and read by a person, and one changed invocation should be one
    /// changed line rather than a re-indented block.
    #[must_use]
    pub fn export(&self) -> String {
        let mut out = String::new();
        let _ = writeln!(out, "cy-agent-session 1");
        let _ = writeln!(out, "revision {}", self.starting_revision);
        let _ = writeln!(out, "intent {}", self.intent);
        for invocation in &self.invocations {
            let arguments = invocation
                .arguments
                .iter()
                .map(|(name, value)| format!("{name}={value}"))
                .collect::<Vec<_>>()
                .join(" ");
            let _ = writeln!(out, "invoke {} {arguments}", invocation.command);
            for transaction in &invocation.transactions {
                let _ = writeln!(out, "  transaction {transaction}");
            }
            if let Some(refusal) = &invocation.refusal {
                let _ = writeln!(out, "  refused {refusal}");
            } else {
                let _ = writeln!(out, "  outcome {}", invocation.summary);
            }
        }
        out
    }

    /// Compare a replay against this recording, returning the first divergence.
    ///
    /// The comparison is over the command and its outcome — what was invoked and what came back —
    /// because those are what a defect report is about. It deliberately does NOT compare transaction
    /// identifiers: those are per session by construction, and requiring them to match would make
    /// every replay diverge at the first mutation.
    #[must_use]
    pub fn diverges_from(&self, replay: &Self) -> Option<String> {
        if self.starting_revision != replay.starting_revision {
            return Some(format!(
                "the replay began from revision {:?} and the recording from {:?}",
                replay.starting_revision, self.starting_revision
            ));
        }
        for (index, recorded) in self.invocations.iter().enumerate() {
            let Some(played) = replay.invocations.get(index) else {
                return Some(format!(
                    "the replay stopped after {} of {} invocations",
                    replay.invocations.len(),
                    self.invocations.len()
                ));
            };
            if played.command != recorded.command {
                return Some(format!(
                    "invocation {index} was {:?} and replayed as {:?}",
                    recorded.command, played.command
                ));
            }
            if played.arguments != recorded.arguments {
                return Some(format!(
                    "invocation {index} ({}) replayed with different arguments",
                    recorded.command
                ));
            }
            if played.refusal != recorded.refusal {
                return Some(format!(
                    "invocation {index} ({}) was {} and replayed {}",
                    recorded.command,
                    recorded.refusal.as_ref().map_or("accepted", |_| "refused"),
                    played.refusal.as_ref().map_or("accepted", |_| "refused"),
                ));
            }
            if played.summary != recorded.summary {
                return Some(format!(
                    "invocation {index} ({}) produced {:?} and replayed {:?}",
                    recorded.command, recorded.summary, played.summary
                ));
            }
        }
        if replay.invocations.len() > self.invocations.len() {
            return Some(format!(
                "the replay made {} invocations against the recording's {}",
                replay.invocations.len(),
                self.invocations.len()
            ));
        }
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn invocation(command: &str, summary: &str) -> RecordedInvocation {
        RecordedInvocation {
            command: command.to_string(),
            arguments: BTreeMap::from([("name".to_string(), Value::Text("lamp".to_string()))]),
            summary: summary.to_string(),
            transactions: vec!["t-1".to_string()],
            refusal: None,
        }
    }

    #[test]
    fn an_export_carries_the_revision_the_session_began_from() {
        let mut recording = Recording::new("r-17", "add a lamp at the corner");
        recording.record(invocation("scene.create-entity", "Created 1 entity"));
        let text = recording.export();
        assert!(text.contains("revision r-17"), "{text}");
        assert!(text.contains("intent add a lamp at the corner"), "{text}");
        assert!(
            text.contains("invoke scene.create-entity name=lamp"),
            "{text}"
        );
    }

    #[test]
    fn a_faithful_replay_diverges_nowhere() {
        let mut recording = Recording::new("r-17", "add a lamp");
        recording.record(invocation("scene.create-entity", "Created 1 entity"));
        let replay = recording.clone();
        assert!(recording.diverges_from(&replay).is_none());
    }

    #[test]
    fn a_replay_that_produces_something_else_names_where() {
        let mut recording = Recording::new("r-17", "add a lamp");
        recording.record(invocation("scene.create-entity", "Created 1 entity"));
        let mut replay = Recording::new("r-17", "add a lamp");
        replay.record(invocation("scene.create-entity", "Created 2 entities"));
        let divergence = recording.diverges_from(&replay).unwrap();
        assert!(divergence.contains("invocation 0"), "{divergence}");
        assert!(divergence.contains("Created 2 entities"), "{divergence}");
    }

    #[test]
    fn replaying_from_a_different_starting_state_proves_nothing_and_says_so() {
        let recording = Recording::new("r-17", "add a lamp");
        let replay = Recording::new("r-18", "add a lamp");
        assert!(
            recording
                .diverges_from(&replay)
                .unwrap()
                .contains("revision")
        );
    }

    #[test]
    fn a_refusal_is_part_of_the_session() {
        // A replay in which a command that WAS refused now succeeds has found a change in
        // behaviour, which is exactly what a bisect is looking for.
        let mut recording = Recording::new("r-17", "delete the lamp");
        let mut refused = invocation("assets.delete", "");
        refused.refusal = Some("the scope does not grant irreversible-mutation".to_string());
        recording.record(refused);

        let mut replay = Recording::new("r-17", "delete the lamp");
        replay.record(invocation("assets.delete", "Deleted"));
        let divergence = recording.diverges_from(&replay).unwrap();
        assert!(divergence.contains("refused"), "{divergence}");
    }

    #[test]
    fn transaction_identifiers_do_not_have_to_match() {
        // They are per session by construction; requiring them to match would make every replay
        // diverge at the first mutation and make the whole comparison useless.
        let mut recording = Recording::new("r-17", "add a lamp");
        recording.record(invocation("scene.create-entity", "Created 1 entity"));
        let mut replay = Recording::new("r-17", "add a lamp");
        let mut played = invocation("scene.create-entity", "Created 1 entity");
        played.transactions = vec!["t-99".to_string()];
        replay.record(played);
        assert!(recording.diverges_from(&replay).is_none());
    }
}
