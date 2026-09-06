//! A failure that says what went wrong *and what would make it succeed*.
//!
//! `editor-agent-interface`: "An error SHALL state what went wrong and what would make it succeed.
//! An agent that receives 'invalid argument' learns nothing; one that receives 'the property is
//! read-only because the object is a prefab instance; override it first or edit the prefab' can
//! act." And: "Validation failures SHALL be reported as structured results the agent can reason
//! about, not as prose it must parse."
//!
//! So a [`Problem`] has three fields rather than one string. `what` names the thing, `because`
//! gives the reason, and `remedy` is the operation that would make the call succeed — absent when
//! there genuinely is not one, which is itself information.
//!
//! It is the same type a person sees. A second error type for humans would drift from this one, and
//! the half that drifted would be the half nobody reads until it matters.

use std::fmt;

/// The editor's error type: a structured failure with a reason and, where one exists, a remedy.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Problem {
    /// What was being attempted or what was not found. A noun phrase, not a sentence.
    pub what: String,
    /// Why it did not work. A clause that completes "because ...".
    pub because: String,
    /// What would make it succeed, when something would. A caller — human or machine — can act on
    /// this; `None` means the operation is not available at all rather than not available yet.
    pub remedy: Option<String>,
}

impl Problem {
    /// A failure with no remedy: nothing the caller can do makes this call succeed.
    pub fn new(what: impl Into<String>, because: impl Into<String>) -> Self {
        Self {
            what: what.into(),
            because: because.into(),
            remedy: None,
        }
    }

    /// Attach the operation that would make this call succeed.
    #[must_use]
    pub fn with_remedy(mut self, remedy: impl Into<String>) -> Self {
        self.remedy = Some(remedy.into());
        self
    }

    /// Something the editor was asked for and does not have.
    pub fn not_found(what: impl Into<String>) -> Self {
        let what = what.into();
        Self {
            because: format!("there is no {what}"),
            what,
            remedy: None,
        }
    }
}

impl fmt::Display for Problem {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}: {}", self.what, self.because)?;
        if let Some(remedy) = &self.remedy {
            write!(f, " ({remedy})")?;
        }
        Ok(())
    }
}

impl std::error::Error for Problem {}

/// The editor's result type. Every fallible call in the workspace returns one of these.
pub type Result<T> = std::result::Result<T, Problem>;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_remedy_is_part_of_the_message_a_caller_reads() {
        let problem = Problem::new("translate", "the object is a prefab instance")
            .with_remedy("override the transform, or edit the prefab");
        assert_eq!(
            problem.to_string(),
            "translate: the object is a prefab instance \
             (override the transform, or edit the prefab)"
        );
    }

    #[test]
    fn a_problem_with_no_remedy_says_so_by_omission() {
        let problem = Problem::not_found("a document with that identity");
        assert!(problem.remedy.is_none());
        assert_eq!(
            problem.to_string(),
            "a document with that identity: there is no a document with that identity"
        );
    }
}
