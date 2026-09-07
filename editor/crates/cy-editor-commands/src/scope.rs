//! What a connection is permitted to do. `editor-agent-interface`, "Connections declare a scope".
//!
//! "An agent connection SHALL declare the **scope** it operates within: which documents, which
//! directories, and which effect classes it may invoke. Operations outside that scope SHALL be
//! refused with the scope as the reason ... **Scope SHALL default to the narrowest useful setting**
//! rather than to full access."
//!
//! So [`Scope::default`] is read-only over nothing. Widening is explicit, and every refusal names
//! the scope that excluded the call rather than saying "denied".
//!
//! A human's own invocations run under [`Scope::unrestricted`]. That is not a claim that the human
//! is trusted more than the agent; it is the observation that the human is *at* the interface, where
//! confirmation dialogs and undo already live, and a scope that refused a person's menu item would
//! be the interface refusing itself.

use std::collections::BTreeSet;

use cy_editor_core::ids::DocumentId;
use cy_editor_core::problem::{Problem, Result};

use crate::metadata::{EffectClass, Metadata};

/// Which documents a scope covers.
#[derive(Clone, PartialEq, Eq, Debug, Default)]
pub enum DocumentScope {
    /// No document at all. The default.
    #[default]
    None,
    /// Exactly these documents.
    These(BTreeSet<DocumentId>),
    /// Every document. Granted deliberately, never by omission.
    All,
}

/// What a connection may invoke.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Scope {
    /// A name for the scope, used in refusals so that a caller learns which grant excluded it.
    pub name: String,
    /// Which documents it may act on.
    pub documents: DocumentScope,
    /// Which effect classes it may invoke.
    pub effects: BTreeSet<EffectClass>,
    /// Which project directories it may touch, as path prefixes. Empty means none.
    pub directories: Vec<String>,
}

impl Default for Scope {
    /// The narrowest useful setting: read-only, no documents, no directories.
    fn default() -> Self {
        Self {
            name: "default".into(),
            documents: DocumentScope::None,
            effects: BTreeSet::from([EffectClass::Read]),
            directories: Vec::new(),
        }
    }
}

impl Scope {
    /// A scope with no restrictions, for the human at the interface. See the module note.
    #[must_use]
    pub fn unrestricted() -> Self {
        Self {
            name: "interactive".into(),
            documents: DocumentScope::All,
            effects: EffectClass::ALL.into_iter().collect(),
            directories: vec![String::new()],
        }
    }

    /// A named scope over some documents and some effect classes.
    pub fn new(
        name: impl Into<String>,
        documents: DocumentScope,
        effects: impl IntoIterator<Item = EffectClass>,
    ) -> Self {
        Self {
            name: name.into(),
            documents,
            effects: effects.into_iter().collect(),
            directories: Vec::new(),
        }
    }

    /// Grant a directory prefix.
    #[must_use]
    pub fn with_directory(mut self, prefix: impl Into<String>) -> Self {
        self.directories.push(prefix.into());
        self
    }

    /// Whether this scope covers a document.
    #[must_use]
    pub fn covers_document(&self, document: DocumentId) -> bool {
        match &self.documents {
            DocumentScope::None => false,
            DocumentScope::These(set) => set.contains(&document),
            DocumentScope::All => true,
        }
    }

    /// Whether this scope covers a project path.
    #[must_use]
    pub fn covers_path(&self, path: &str) -> bool {
        self.directories
            .iter()
            .any(|prefix| path.starts_with(prefix.as_str()))
    }

    /// Refuse a command this scope does not cover, naming the scope.
    ///
    /// "WHEN an agent invokes a command outside its declared scope THEN it SHALL be refused, and the
    /// refusal SHALL name the scope that excluded it."
    pub fn admit(&self, metadata: &Metadata) -> Result<()> {
        self.admit_effect(metadata, metadata.effect)
    }

    /// Refuse a *particular invocation* whose effect class this scope does not grant.
    ///
    /// The class is passed rather than read off the metadata, because a command may work its class
    /// out from the invocation — see `crate::registry::Command::effect_when`. Writing a source file
    /// the editor can restore is a reversible mutation; writing one it cannot is not, and the same
    /// command is both.
    pub fn admit_effect(&self, metadata: &Metadata, effect: EffectClass) -> Result<()> {
        if self.effects.contains(&effect) {
            return Ok(());
        }
        Err(Problem::new(
            format!("invoke {}", metadata.id),
            format!(
                "the scope {:?} does not grant the effect class {} ({})",
                self.name,
                effect.name(),
                effect.consequence()
            ),
        )
        .with_remedy(format!(
            "grant {} to this connection deliberately, or invoke a command whose effect class is \
             one of: {}",
            effect.name(),
            self.granted_effects()
        )))
    }

    /// Refuse a document this scope does not cover, naming the scope.
    pub fn admit_document(&self, document: DocumentId) -> Result<()> {
        if self.covers_document(document) {
            return Ok(());
        }
        Err(Problem::new(
            format!("act on document {document}"),
            format!("the scope {:?} does not include it", self.name),
        )
        .with_remedy("add the document to the connection's scope"))
    }

    fn granted_effects(&self) -> String {
        let mut names: Vec<&str> = self.effects.iter().map(|effect| effect.name()).collect();
        names.sort_unstable();
        if names.is_empty() {
            "none".to_string()
        } else {
            names.join(", ")
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn deleting() -> Metadata {
        Metadata::new(
            "assets.delete",
            "Delete Asset",
            "Assets",
            "Deletes an asset from disk. This cannot be undone from within the editor.",
            EffectClass::IrreversibleMutation,
        )
    }

    #[test]
    fn the_default_scope_is_the_narrowest_useful_one() {
        let scope = Scope::default();
        assert_eq!(scope.effects, BTreeSet::from([EffectClass::Read]));
        assert!(!scope.covers_document(DocumentId::of_asset("worlds/city.cyworld")));
        assert!(!scope.covers_path("assets/"));
    }

    #[test]
    fn a_refusal_names_the_scope_that_excluded_it_and_what_it_does_grant() {
        let scope = Scope::new(
            "review",
            DocumentScope::All,
            [EffectClass::Read, EffectClass::ReversibleMutation],
        );
        let problem = scope.admit(&deleting()).unwrap_err();
        assert!(problem.because.contains("\"review\""), "{problem}");
        assert!(
            problem
                .remedy
                .as_deref()
                .unwrap()
                .contains("reversible-mutation"),
            "{problem}"
        );
    }

    #[test]
    fn a_document_outside_the_scope_is_refused_by_name() {
        let city = DocumentId::of_asset("worlds/city.cyworld");
        let forest = DocumentId::of_asset("worlds/forest.cyworld");
        let scope = Scope::new(
            "one world",
            DocumentScope::These([city].into()),
            [EffectClass::Read],
        );
        scope.admit_document(city).unwrap();
        assert!(scope.admit_document(forest).is_err());
    }

    #[test]
    fn the_interactive_scope_grants_everything() {
        let scope = Scope::unrestricted();
        scope.admit(&deleting()).unwrap();
        assert!(scope.covers_path("anything/at/all"));
    }
}
