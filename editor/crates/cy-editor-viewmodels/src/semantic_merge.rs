// SPDX-License-Identifier: MIT
//! Presentation-only semantic Diff and Merge models.

use cy_editor_core::observe::Watch;
use cy_editor_documents::diff::{Change, Conflict};
use cy_editor_services::{MergeDecision, SemanticMergeService};

/// One identity-keyed semantic difference shown in the Diff panel.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct DiffRow {
    /// Stable target identity, never a serialized-file line number.
    pub target: String,
    /// Human-readable semantic operation.
    pub summary: String,
}

/// Base-to-local and base-to-incoming semantic differences.
#[derive(Default, Debug)]
pub struct DiffViewModel {
    watch: Watch,
    local: Vec<DiffRow>,
    incoming: Vec<DiffRow>,
}

impl DiffViewModel {
    /// Empty model.
    #[must_use]
    pub const fn new() -> Self {
        Self {
            watch: Watch::new(),
            local: Vec::new(),
            incoming: Vec::new(),
        }
    }

    /// Rebuild only when authoritative merge state changes.
    pub fn refresh(&mut self, service: &SemanticMergeService) -> bool {
        let revision = service.revision();
        if !self.watch.changed(revision) {
            return false;
        }
        self.local.clear();
        self.incoming.clear();
        if let Some(session) = service.session() {
            self.local
                .extend(session.local_changes().iter().map(row_for));
            self.incoming
                .extend(session.incoming_changes().iter().map(row_for));
        }
        self.watch.accept(revision);
        true
    }

    /// Local differences from the selected base.
    #[must_use]
    pub fn local(&self) -> &[DiffRow] {
        &self.local
    }

    /// Incoming differences from the selected base.
    #[must_use]
    pub fn incoming(&self) -> &[DiffRow] {
        &self.incoming
    }
}

/// One explicit semantic conflict.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct MergeConflictRow {
    /// Stable row index used by the registered resolution command.
    pub index: usize,
    /// Stable target identity.
    pub target: String,
    /// Local semantic value/change.
    pub local: String,
    /// Incoming semantic value/change.
    pub incoming: String,
    /// Why an explicit choice is required.
    pub reason: String,
    /// Current explicit decision, if any.
    pub decision: Option<String>,
}

/// Presentation state for the pending merge.
#[derive(Default, Debug)]
pub struct MergeViewModel {
    watch: Watch,
    path: String,
    base_revision: String,
    incoming_revision: String,
    automatic_count: usize,
    conflicts: Vec<MergeConflictRow>,
}

impl MergeViewModel {
    /// Empty model.
    #[must_use]
    pub const fn new() -> Self {
        Self {
            watch: Watch::new(),
            path: String::new(),
            base_revision: String::new(),
            incoming_revision: String::new(),
            automatic_count: 0,
            conflicts: Vec::new(),
        }
    }

    /// Rebuild only from the service; this model never holds document content.
    pub fn refresh(&mut self, service: &SemanticMergeService) -> bool {
        let revision = service.revision();
        if !self.watch.changed(revision) {
            return false;
        }
        self.path.clear();
        self.base_revision.clear();
        self.incoming_revision.clear();
        self.automatic_count = 0;
        self.conflicts.clear();
        if let Some(session) = service.session() {
            self.path = session.path().to_string();
            self.base_revision = session.base_revision().to_string();
            self.incoming_revision = session.incoming_revision().to_string();
            self.automatic_count = session.automatic().len();
            self.conflicts.extend(
                session
                    .conflicts()
                    .iter()
                    .zip(session.decisions())
                    .enumerate()
                    .map(|(index, (conflict, decision))| {
                        conflict_row(index, conflict, decision.as_ref())
                    }),
            );
        }
        self.watch.accept(revision);
        true
    }

    /// Asset being merged.
    #[must_use]
    pub fn path(&self) -> &str {
        &self.path
    }

    /// Selected base revision.
    #[must_use]
    pub fn base_revision(&self) -> &str {
        &self.base_revision
    }

    /// Selected incoming revision.
    #[must_use]
    pub fn incoming_revision(&self) -> &str {
        &self.incoming_revision
    }

    /// Non-overlapping changes that will apply automatically.
    #[must_use]
    pub const fn automatic_count(&self) -> usize {
        self.automatic_count
    }

    /// Explicit conflicts in stable order.
    #[must_use]
    pub fn conflicts(&self) -> &[MergeConflictRow] {
        &self.conflicts
    }
}

fn conflict_row(
    index: usize,
    conflict: &Conflict,
    decision: Option<&MergeDecision>,
) -> MergeConflictRow {
    MergeConflictRow {
        index,
        target: target_of(&conflict.ours),
        local: summary_of(&conflict.ours),
        incoming: summary_of(&conflict.theirs),
        reason: conflict.reason.clone(),
        decision: decision.map(|decision| {
            match decision {
                MergeDecision::Local => "local",
                MergeDecision::Incoming(_) => "incoming",
                MergeDecision::Replacement(_) => "replacement",
            }
            .to_string()
        }),
    }
}

fn row_for(change: &Change) -> DiffRow {
    DiffRow {
        target: target_of(change),
        summary: summary_of(change),
    }
}

fn target_of(change: &Change) -> String {
    match change {
        Change::FieldChanged {
            node,
            component,
            field,
            ..
        }
        | Change::AssetReferenceChanged {
            node,
            component,
            field,
            ..
        }
        | Change::OverrideChanged {
            node,
            component,
            field,
            ..
        } => {
            format!(
                "{node}/type#{}/field#{}",
                component.as_u64(),
                field.as_u64()
            )
        }
        Change::ComponentAdded {
            node, component, ..
        }
        | Change::ComponentRemoved {
            node, component, ..
        } => {
            format!("{node}/type#{}", component.as_u64())
        }
        _ => change
            .node()
            .map_or_else(|| "document".into(), |node| node.to_string()),
    }
}

fn summary_of(change: &Change) -> String {
    match change {
        Change::NodeAdded { .. } => "entity added".into(),
        Change::NodeRemoved { .. } => "entity removed".into(),
        Change::NodeReparented { before, after, .. } => format!("parent {before:?} -> {after:?}"),
        Change::NameChanged { before, after, .. } => format!("name {before:?} -> {after:?}"),
        Change::LayerChanged { before, after, .. } => format!("layer {before:?} -> {after:?}"),
        Change::ComponentAdded { .. } => "component added".into(),
        Change::ComponentRemoved { .. } => "component removed".into(),
        Change::FieldChanged { before, after, .. } => format!("{before} -> {after}"),
        Change::AssetReferenceChanged { before, after, .. } => {
            format!("asset {before:?} -> {after:?}")
        }
        Change::OverrideChanged { before, after, .. } => {
            format!("override {before:?} -> {after:?}")
        }
    }
}
