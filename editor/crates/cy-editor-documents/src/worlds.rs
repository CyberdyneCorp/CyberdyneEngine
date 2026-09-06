//! Authoring, preview and runtime worlds, kept distinct — and checkably so. Task 3.1.
//!
//! "The editor SHALL maintain three kinds of world, and SHALL NOT collapse them ... Authoring-only
//! data SHALL NOT exist in a runtime world. **Because a compilation step separates them, this SHALL
//! be a checkable property rather than a convention.**"
//!
//! | World | Contains |
//! |---|---|
//! | Authoring | The edited project: authoring-only data, prefab provenance, override state, unresolved references, selection metadata |
//! | Preview | Isolated worlds for asset editors, each with its own services |
//! | Runtime | The world play mode instantiates, produced *from* authoring data by a compilation step |
//!
//! --- WHERE THE CHECK LIVES, AND WHY IT IS NOT AN ASSERTION IN THE COMPILER -------------------------------
//!
//! [`compile`] produces a runtime world, and [`authoring_leaks`] independently reads the *result*
//! and reports anything authoring-only it finds. Two functions rather than one, because a compiler
//! that also decided whether its own output was correct would agree with itself by construction —
//! the check has to be able to fail on output the compiler was happy with, and the test
//! `a_deliberately_broken_compilation_is_caught` is what proves it can.
//!
//! Prefab provenance and override state are dropped by the compilation rather than flagged by it:
//! they are authoring concepts every prefab instance carries, so flagging them would report the
//! normal case as a defect. What [`authoring_leaks`] looks for is a *component type the schema marks
//! authoring-only* surviving into the runtime world, which is the case that means something went
//! wrong.

use cy_editor_core::ids::{DocumentId, NodeId, TypeId};

use crate::content::DocumentContent;
use crate::schema::DocumentSchema;

/// Which of the three kinds a world is.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum WorldKind {
    /// The edited project.
    Authoring,
    /// An asset editor's isolated world.
    Preview,
    /// What play mode instantiates.
    Runtime,
}

impl WorldKind {
    /// Whether authoring-only data may exist in a world of this kind.
    #[must_use]
    pub const fn allows_authoring_data(self) -> bool {
        matches!(self, WorldKind::Authoring | WorldKind::Preview)
    }
}

/// A world of one of the three kinds, with its content.
///
/// A preview world "SHALL have its own preview world, and neither SHALL affect the edited project" —
/// which here is a property of ownership: a `World` owns its content outright, so two of them cannot
/// share a node however hard a panel tries.
#[derive(Clone, Debug)]
pub struct World {
    kind: WorldKind,
    content: DocumentContent,
}

impl World {
    /// A world of `kind` for `document`.
    #[must_use]
    pub fn new(kind: WorldKind, document: DocumentId) -> Self {
        Self {
            kind,
            content: DocumentContent::new(document),
        }
    }

    /// Which kind this is.
    #[must_use]
    pub const fn kind(&self) -> WorldKind {
        self.kind
    }

    /// The world's content.
    #[must_use]
    pub const fn content(&self) -> &DocumentContent {
        &self.content
    }
}

/// What a compilation dropped, so that a build log can say what happened rather than only what it
/// produced.
#[derive(Clone, PartialEq, Eq, Debug, Default)]
pub struct CompileReport {
    /// Nodes carried through.
    pub nodes: usize,
    /// Components dropped because their type is authoring-only.
    pub authoring_components_dropped: usize,
    /// Prefab provenance records dropped.
    pub prefab_records_dropped: usize,
    /// Override records dropped, having been folded into the values they override.
    pub overrides_folded: usize,
}

/// Produce a runtime world from authoring content.
///
/// Three things happen, and each is one of the specification's "authoring-only data": components
/// whose type is marked authoring-only are dropped, prefab provenance is dropped, and overrides are
/// **folded into the values they override** rather than dropped — an override that vanished would
/// change what the game does, which is a different failure from leaking editor data into it.
#[must_use]
pub fn compile(
    authoring: &DocumentContent,
    schema: &DocumentSchema,
) -> (DocumentContent, CompileReport) {
    let mut runtime = DocumentContent::new(authoring.document());
    let mut report = CompileReport::default();

    // The runtime world is built by replaying operations through the same token-gated path as
    // everything else, so that a compiled world is not a second way to construct content. The token
    // names the compilation rather than a user transaction, which is what an audit of a runtime
    // world then reports.
    let token = crate::content::WriteToken::issue(0);

    for node in authoring.nodes() {
        let Some(state) = authoring.node(node) else {
            continue;
        };
        let mut compiled = state.clone();

        compiled.components.retain(|component, _| {
            let authoring_only = schema
                .type_of(*component)
                .is_some_and(|definition| definition.authoring_only);
            if authoring_only {
                report.authoring_components_dropped += 1;
            }
            !authoring_only
        });

        for ((component, field), value) in std::mem::take(&mut compiled.overrides) {
            if let Some(fields) = compiled.components.get_mut(&component) {
                fields.insert(field, value);
                report.overrides_folded += 1;
            }
        }

        if compiled.prefab.take().is_some() {
            report.prefab_records_dropped += 1;
        }

        let operation = crate::operation::Operation::RestoreNode {
            node,
            state: Box::new(compiled),
        };
        if runtime.apply(&operation, &token).is_ok() {
            report.nodes += 1;
        }
    }

    (runtime, report)
}

/// Authoring data that survived into a runtime world. Empty on a correct compilation.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Leak {
    /// Which node carries it.
    pub node: NodeId,
    /// Which authoring-only component type.
    pub component: TypeId,
    /// The component's name, so the report reads without a schema in hand.
    pub name: String,
}

/// Every authoring-only component that reached a runtime world.
///
/// Reads the *result*, independently of what produced it — see the module note for why that
/// separation is the point rather than an accident.
#[must_use]
pub fn authoring_leaks(runtime: &DocumentContent, schema: &DocumentSchema) -> Vec<Leak> {
    let mut leaks = Vec::new();
    for node in runtime.nodes() {
        let Some(state) = runtime.node(node) else {
            continue;
        };
        for component in state.components.keys() {
            if let Some(definition) = schema
                .type_of(*component)
                .filter(|definition| definition.authoring_only)
            {
                leaks.push(Leak {
                    node,
                    component: *component,
                    name: definition.name.clone(),
                });
            }
        }
        if state.prefab.is_some() {
            // Prefab provenance is authoring-only by definition. Reported as a leak on the node's
            // first component so that the report names something a person can select, and with the
            // name that says what it is.
            if let Some(component) = state.components.keys().next() {
                leaks.push(Leak {
                    node,
                    component: *component,
                    name: "prefab provenance".to_string(),
                });
            }
        }
    }
    leaks
}

#[cfg(test)]
mod tests {
    use cy_editor_core::Actor;
    use cy_editor_core::value::{Value, ValueKind};

    use super::*;
    use crate::document::Document;

    /// A document with a runtime `Transform` and an authoring-only `EditorNote`.
    fn authored() -> (Document, TypeId, TypeId) {
        let mut document = Document::new("worlds/city.cyworld");
        let transform = document.schema_mut().declare_type("Transform", false);
        let position = document
            .schema_mut()
            .declare_field(transform, "position", ValueKind::Vec3, "where it is")
            .unwrap();
        let annotation = document.schema_mut().declare_type("EditorNote", true);
        let text = document
            .schema_mut()
            .declare_field(annotation, "text", ValueKind::Text, "a designer's note")
            .unwrap();

        document
            .with_transaction("Build", Actor::human("designer"), |document| {
                let node = document.create_node(None)?;
                document.add_component(
                    node,
                    transform,
                    vec![(position, Value::Vec3([1.0, 2.0, 3.0]))],
                )?;
                document.add_component(
                    node,
                    annotation,
                    vec![(text, Value::Text("check the shadow here".into()))],
                )
            })
            .unwrap();
        (document, transform, annotation)
    }

    #[test]
    fn editor_data_cannot_leak_into_a_runtime_world() {
        let (document, transform, annotation) = authored();
        let (runtime, report) = compile(document.content(), document.schema());

        assert_eq!(report.nodes, 1);
        assert_eq!(report.authoring_components_dropped, 1);
        assert!(authoring_leaks(&runtime, document.schema()).is_empty());

        let compiled = runtime.nodes().next().unwrap();
        assert!(
            runtime.has_component(compiled, transform),
            "the runtime component survived"
        );
        assert!(
            !runtime.has_component(compiled, annotation),
            "the authoring-only one did not"
        );
    }

    #[test]
    fn a_deliberately_broken_compilation_is_caught() {
        // The check has to be able to FAIL on output a compiler was happy with, or it is only the
        // compiler agreeing with itself. Here the authoring content is handed straight to the
        // leak detector as if it were a runtime world, which is exactly what a compilation step
        // that forgot its filter would produce.
        let (document, _, annotation) = authored();
        let leaks = authoring_leaks(document.content(), document.schema());
        assert_eq!(leaks.len(), 1);
        assert_eq!(leaks[0].component, annotation);
        assert_eq!(leaks[0].name, "EditorNote");
    }

    #[test]
    fn an_override_is_folded_rather_than_dropped() {
        let (mut document, transform, _annotation) = authored();
        let position = document.schema().type_named("Transform").unwrap().fields[0].id;
        let node = document.content().nodes().next().unwrap();

        document
            .with_transaction("Override", Actor::human("designer"), |document| {
                document.record(crate::operation::Operation::SetOverride {
                    node,
                    component: transform,
                    field: position,
                    before: None,
                    after: Some(Value::Vec3([9.0, 9.0, 9.0])),
                })
            })
            .unwrap();

        let (runtime, report) = compile(document.content(), document.schema());
        assert_eq!(report.overrides_folded, 1);
        assert_eq!(
            runtime.field(node, transform, position),
            Some(&Value::Vec3([9.0, 9.0, 9.0])),
            "an override that vanished would change what the game does"
        );
    }

    #[test]
    fn previews_are_isolated_by_ownership() {
        let material = World::new(
            WorldKind::Preview,
            DocumentId::of_asset("materials/brick.cymat"),
        );
        let animation = World::new(
            WorldKind::Preview,
            DocumentId::of_asset("animations/walk.cyanim"),
        );
        assert_ne!(
            material.content().document(),
            animation.content().document()
        );
        assert!(WorldKind::Preview.allows_authoring_data());
        assert!(!WorldKind::Runtime.allows_authoring_data());
    }
}
