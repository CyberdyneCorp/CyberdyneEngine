//! The audit that catches a write around the transaction system. Tasks 3.3 and 3.9.
//!
//! `editor-documents-and-transactions`: "A tool that mutates state outside a transaction SHALL be a
//! defect, and **development builds SHALL detect and report it**" — with the scenario "WHEN
//! development builds detect state changed outside a transaction THEN it SHALL be reported naming
//! the document and the mutating code."
//!
//! --- WHY THIS EXISTS AT ALL, GIVEN THE TYPE SYSTEM ALREADY FORBIDS IT ---------------------------------
//!
//! [`crate::content::WriteToken`] makes an out-of-band write *impossible from outside this crate*.
//! What it cannot prevent is a write path added **inside** this crate — a convenience method someone
//! adds to `DocumentContent` next year that calls the private mutator directly, for a locally
//! excellent reason, and quietly makes undo, autosave, recovery, diff and live editing incomplete
//! for whatever it touches.
//!
//! That is the failure mode the specification is describing, and it is the one nothing points at.
//! So the content counts both kinds of write, and this module compares them.
//!
//! --- WHY IT IS NOT COMPILED OUT IN A SHIPPING BUILD ----------------------------------------------------
//!
//! It could be, and the specification only asks for development builds. It is not, for the same
//! reason `cy_abi.h` gives for `borrow_valid` being a comparison rather than an assertion: a rule
//! that only holds in two configurations is not a rule, and this one costs two `u64` increments per
//! operation against a transaction system that already allocates a `Vec`. The editor reports it
//! rather than aborting, which is what makes it safe to leave on.

use cy_editor_core::ids::DocumentId;
use cy_editor_core::problem::Problem;

use crate::content::DocumentContent;
use crate::document::Document;

/// What the audit found.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Audit {
    /// Which document was audited.
    pub document: DocumentId,
    /// Every mutation the content has seen.
    pub writes: u64,
    /// The ones that carried a transaction's write token.
    pub transactional_writes: u64,
}

impl Audit {
    /// Audit a document's content.
    #[must_use]
    pub fn of(document: &Document) -> Self {
        Self::of_content(document.content())
    }

    /// Audit content directly, for a preview or runtime world with no document around it.
    #[must_use]
    pub fn of_content(content: &DocumentContent) -> Self {
        let (writes, transactional_writes) = content.write_counts();
        Self {
            document: content.document(),
            writes,
            transactional_writes,
        }
    }

    /// How many mutations did not go through a transaction.
    #[must_use]
    pub const fn out_of_band(&self) -> u64 {
        self.writes.saturating_sub(self.transactional_writes)
    }

    /// Whether every mutation went through a transaction.
    #[must_use]
    pub const fn is_clean(&self) -> bool {
        self.out_of_band() == 0
    }

    /// The audit as a pass or a named failure.
    ///
    /// The failure names the document and the count, which is what
    /// `editor-documents-and-transactions` asks for; naming the *code* is what a backtrace does, and
    /// the editor logs one beside this rather than reimplementing it.
    pub fn verify(&self) -> cy_editor_core::problem::Result<()> {
        if self.is_clean() {
            return Ok(());
        }
        Err(Problem::new(
            format!("audit document {}", self.document),
            format!(
                "{} of {} mutations did not go through a transaction, so undo, the journal, crash \
                 recovery, semantic diff and live editing are all incomplete for whatever they \
                 changed",
                self.out_of_band(),
                self.writes
            ),
        )
        .with_remedy("route the mutation through Document::record, which is the only write path"))
    }
}

#[cfg(test)]
mod tests {
    use cy_editor_core::Actor;
    use cy_editor_core::value::{Value, ValueKind};

    use super::*;
    use crate::operation::Operation;

    #[test]
    fn an_ordinary_session_audits_clean() {
        let mut document = Document::new("worlds/city.cyworld");
        let transform = document.schema_mut().declare_type("Transform", false);
        let position = document
            .schema_mut()
            .declare_field(transform, "position", ValueKind::Vec3, "where it is")
            .unwrap();

        document
            .with_transaction("Create lamp", Actor::human("designer"), |document| {
                let node = document.create_node(None)?;
                document.add_component(node, transform, vec![(position, Value::Vec3([0.0; 3]))])?;
                document.set_field(node, transform, position, Value::Vec3([1.0, 2.0, 3.0]))
            })
            .unwrap();

        let audit = Audit::of(&document);
        assert!(audit.is_clean(), "{audit:?}");
        assert!(
            audit.writes > 0,
            "an audit of a document nothing touched proves nothing"
        );
        audit.verify().unwrap();
    }

    /// **Task 3.9.** The test that writes around the transaction system, and fails.
    ///
    /// It uses `DocumentContent::write_around_the_transaction_system`, which exists only in this
    /// crate's test build and exists only to be caught. Without it there would be no way to write
    /// around the system at all — which is the point — and therefore no way to demonstrate that the
    /// audit would notice if there were.
    ///
    /// If a future change makes an out-of-band write possible in a real build, this test is what
    /// says what happens next: it is reported, naming the document, with the five properties that
    /// silently became incomplete listed in the message.
    #[test]
    fn the_test_that_writes_around_the_system() {
        use crate::content::DocumentContent;
        use cy_editor_core::ids::DocumentId;

        let id = DocumentId::of_asset("worlds/city.cyworld");
        let mut content = DocumentContent::new(id);
        let node = content.allocate_node();

        // The bypass. In any build but this crate's own tests, this method does not exist, and
        // `DocumentContent` has no other mutating method that does not demand a `WriteToken`.
        content
            .write_around_the_transaction_system(&Operation::CreateNode { node, parent: None })
            .unwrap();

        let audit = Audit::of_content(&content);
        assert!(
            !audit.is_clean(),
            "the audit must notice a write that carried no transaction"
        );
        assert_eq!(audit.out_of_band(), 1);

        let problem = audit.verify().unwrap_err();
        assert!(
            problem.what.contains(&id.to_string()),
            "the report must name the document"
        );
        assert!(
            problem.because.contains("undo"),
            "and say what it broke: {problem}"
        );
        assert!(problem.remedy.is_some(), "and what to do instead");
    }
}
