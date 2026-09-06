//! The editor's first commands — and the point at which "from the first command" stops being advice.
//!
//! Every command below carries typed parameters with their meaning, a description written for a
//! caller that cannot see the interface, and a declared effect class, because
//! [`cy_editor_commands::Registry::register`] refuses one that does not. There is no first command
//! registered without them, which is what `editor-agent-interface` means by the rule applying "from
//! the first command registered": a later migration would be an entry-by-entry rewrite of an
//! established registry, and nobody does those.
//!
//! Each of these is also the only implementation of its action. A menu item, a keyboard binding, the
//! palette, a script, a test and an agent all reach the same function through
//! [`cy_editor_commands::Registry::invoke`] — so "an action reachable only through a specific widget
//! SHALL be a defect" is a property of there being nowhere else to put one.

use cy_editor_commands::{
    Availability, Command, CommandContext, EffectClass, Metadata, Outcome, ParameterSpec, Registry,
};
use cy_editor_core::ids::NodeId;
use cy_editor_core::problem::{Problem, Result};
use cy_editor_core::value::{Value, ValueKind};
use cy_editor_documents::selection::Selection;

/// Register the editor's built-in commands.
///
/// Fails if any of them would not satisfy a caller that has never seen the interface, which makes
/// this function a test of its own contents every time it runs.
pub fn register(registry: &mut Registry) -> Result<()> {
    registry.register(create_entity())?;
    registry.register(delete_entity())?;
    registry.register(select())?;
    registry.register(undo())?;
    registry.register(redo())?;
    registry.register(save())?;
    Ok(())
}

/// The document a command with no explicit target acts on, or a refusal that says what to do.
fn active(context: &dyn CommandContext) -> Result<cy_editor_core::ids::DocumentId> {
    context.active_document().ok_or_else(|| {
        Problem::new("run this command", "no document is open").with_remedy("open a document first")
    })
}

fn create_entity() -> Command {
    Command::new(
        Metadata::new(
            "scene.create-entity",
            "Create Entity",
            "Scene",
            "Creates an empty entity in the active document and selects it. Records one undoable \
             transaction. With no parent it becomes a root.",
            EffectClass::ReversibleMutation,
        )
        .with(ParameterSpec::optional(
            "parent",
            ValueKind::Text,
            "The identity of the entity to create the new one under, as printed by this command's \
             own result; omit it to create a root.",
            Value::Text(String::new()),
        ))
        .bound_to("Ctrl+Shift+N"),
        |context, arguments| {
            let id = active(context)?;
            let actor = context.actor();
            let parent = parse_node(arguments.text("parent").unwrap_or_default())?;
            let document = context
                .document_mut(id)
                .ok_or_else(|| Problem::not_found("the active document"))?;
            let node = document.with_transaction("Create entity", actor, |document| {
                document.create_node(parent)
            })?;

            let mut selection = Selection::new();
            selection.add_node(node);
            context.set_selection(selection);

            Ok(Outcome::new("Created an entity").with("entity", Value::Text(node.to_string())))
        },
    )
}

fn delete_entity() -> Command {
    Command::new(
        Metadata::new(
            "scene.delete-entity",
            "Delete Entity",
            "Scene",
            "Deletes an entity and its children from the active document. Undo restores it \
             exactly, including its components and their values.",
            EffectClass::ReversibleMutation,
        )
        .with(ParameterSpec::required(
            "entity",
            ValueKind::Text,
            "The identity of the entity to delete, as printed by scene.create-entity.",
        )),
        |context, arguments| {
            let id = active(context)?;
            let actor = context.actor();
            let node = parse_node(arguments.text("entity").unwrap_or_default())?
                .ok_or_else(|| Problem::new("delete an entity", "no entity was named"))?;
            let document = context
                .document_mut(id)
                .ok_or_else(|| Problem::not_found("the active document"))?;
            document.with_transaction("Delete entity", actor, |document| {
                document.delete_node(node)
            })?;
            Ok(Outcome::new("Deleted an entity"))
        },
    )
}

fn select() -> Command {
    Command::new(
        Metadata::new(
            "edit.select",
            "Select",
            "Edit",
            "Replaces the selection with the named entity. Selection is a service every panel \
             observes, so this affects the inspector, the viewport and the status bar at once.",
            EffectClass::Read,
        )
        .with(ParameterSpec::required(
            "entity",
            ValueKind::Text,
            "The identity of the entity to select.",
        )),
        |context, arguments| {
            let node = parse_node(arguments.text("entity").unwrap_or_default())?
                .ok_or_else(|| Problem::new("select", "no entity was named"))?;
            let mut selection = Selection::new();
            selection.add_node(node);
            context.set_selection(selection);
            Ok(Outcome::new("Selected one entity"))
        },
    )
}

fn undo() -> Command {
    Command::new(
        Metadata::new(
            "edit.undo",
            "Undo",
            "Edit",
            "Reverses the most recent change to the active document, restoring the values it \
             recorded before it applied them.",
            EffectClass::ReversibleMutation,
        )
        .bound_to("Ctrl+Z"),
        |context, _| {
            let id = active(context)?;
            let document = context
                .document_mut(id)
                .ok_or_else(|| Problem::not_found("the active document"))?;
            match document.undo()? {
                Some(transaction) => Ok(Outcome::new(format!("Undid {}", transaction.name))),
                None => Ok(Outcome::new("Nothing to undo")),
            }
        },
    )
    .available_when(|context| undo_availability(context, true))
}

fn redo() -> Command {
    Command::new(
        Metadata::new(
            "edit.redo",
            "Redo",
            "Edit",
            "Reapplies the most recently undone change to the active document.",
            EffectClass::ReversibleMutation,
        )
        .bound_to("Ctrl+Shift+Z"),
        |context, _| {
            let id = active(context)?;
            let document = context
                .document_mut(id)
                .ok_or_else(|| Problem::not_found("the active document"))?;
            match document.redo()? {
                Some(transaction) => Ok(Outcome::new(format!("Redid {}", transaction.name))),
                None => Ok(Outcome::new("Nothing to redo")),
            }
        },
    )
    .available_when(|context| undo_availability(context, false))
}

/// Why undo or redo is unavailable, in the words a person would be given.
///
/// "WHEN a command is disabled THEN the reason SHALL be reportable rather than the control being
/// merely greyed." Two different reasons, and telling them apart is the whole value: "nothing is
/// open" and "nothing has been done yet" call for different actions.
fn undo_availability(context: &dyn CommandContext, undoing: bool) -> Availability {
    let Some(id) = context.active_document() else {
        return Availability::Unavailable(
            Problem::new(if undoing { "undo" } else { "redo" }, "no document is open")
                .with_remedy("open a document first"),
        );
    };
    let Some(document) = context.document(id) else {
        return Availability::Unavailable(Problem::not_found("the active document"));
    };
    let status = document.history_status();
    let available = if undoing {
        status.undoable > 0
    } else {
        status.redoable > 0
    };
    if available {
        return Availability::Available;
    }
    Availability::Unavailable(if undoing {
        Problem::new("undo", "nothing has been changed in this document yet")
            .with_remedy("make a change first")
    } else {
        Problem::new("redo", "nothing has been undone in this document")
            .with_remedy("undo something first")
    })
}

fn save() -> Command {
    Command::new(
        Metadata::new(
            "file.save",
            "Save",
            "File",
            "Writes the active document's backing assets and discards its recovery journal. The \
             journal is cleared only after the write succeeds.",
            EffectClass::IrreversibleMutation,
        )
        .bound_to("Ctrl+S"),
        |context, _| {
            let id = active(context)?;
            let document = context
                .document_mut(id)
                .ok_or_else(|| Problem::not_found("the active document"))?;
            // Writing the assets themselves is the serialisation layer's, at a later task. What is
            // implemented here is the *sequence*, which is the part with a correctness argument:
            // the journal is reset only after the write reports success.
            document.save(|_| Ok(()))?;
            Ok(Outcome::new("Saved"))
        },
    )
    .available_when(|context| match context.active_document() {
        Some(id)
            if context
                .document(id)
                .is_some_and(cy_editor_documents::Document::is_dirty) =>
        {
            Availability::Available
        }
        Some(_) => Availability::Unavailable(
            Problem::new("save", "the document has no unsaved changes")
                .with_remedy("make a change first"),
        ),
        None => Availability::Unavailable(
            Problem::new("save", "no document is open").with_remedy("open a document first"),
        ),
    })
}

/// A node identity as this editor prints it, or `None` for an empty string.
fn parse_node(text: &str) -> Result<Option<NodeId>> {
    if text.is_empty() {
        return Ok(None);
    }
    u128::from_str_radix(text, 16)
        .map(NodeId::from_u128)
        .map(Some)
        .map_err(|_| {
            Problem::new(
                format!("read the entity {text:?}"),
                "it is not an entity identity",
            )
            .with_remedy("use the identity a command's result printed, which is 32 hex digits")
        })
}

#[cfg(test)]
mod tests {
    use cy_editor_commands::{Arguments, Scope};
    use cy_editor_core::Actor;

    use super::*;
    use crate::editor::Editor;

    fn editor_with_registry() -> (Editor, Registry) {
        let mut registry = Registry::new();
        register(&mut registry).unwrap();
        let mut editor = Editor::default();
        editor.open_document("worlds/city.cyworld").unwrap();
        (editor, registry)
    }

    #[test]
    fn every_built_in_command_satisfies_a_caller_that_cannot_see_the_interface() {
        // `register` fails if any of them does not. Asserting the count as well means a command
        // added without metadata cannot slip through by simply not being registered.
        let mut registry = Registry::new();
        register(&mut registry).unwrap();
        assert_eq!(registry.len(), 6);
        for metadata in registry.all() {
            metadata.validate().unwrap();
            assert!(!metadata.description.is_empty());
        }
    }

    #[test]
    fn creating_an_entity_selects_it_and_records_one_transaction() {
        let (mut editor, registry) = editor_with_registry();
        let outcome = editor
            .invoke(
                &registry,
                "scene.create-entity",
                &Scope::unrestricted(),
                &Arguments::new(),
            )
            .unwrap();

        let printed = outcome.values["entity"].as_text().unwrap().to_string();
        assert_eq!(editor.selection.get().node_count(), 1);

        let id = editor.workspace.active().unwrap();
        assert_eq!(
            editor.documents.get(id).unwrap().history().entries().len(),
            1
        );

        // And the identity it printed is the one a later command can name.
        let node = parse_node(&printed).unwrap().unwrap();
        assert!(
            editor
                .documents
                .get(id)
                .unwrap()
                .content()
                .node(node)
                .is_some()
        );
    }

    #[test]
    fn an_agents_create_is_a_humans_create_with_a_different_name_on_it() {
        // `editor-agent-interface`: "WHEN an agent translates an object and a human translates it
        // identically with the gizmo THEN the resulting transform SHALL be identical, and each
        // SHALL produce one undoable transaction."
        let (mut human, registry) = editor_with_registry();
        human
            .invoke(
                &registry,
                "scene.create-entity",
                &Scope::unrestricted(),
                &Arguments::new(),
            )
            .unwrap();

        let (mut agent, _) = editor_with_registry();
        agent.acting_as(Actor::agent("claude", "s-1", "place a lamp"));
        agent
            .invoke(
                &registry,
                "scene.create-entity",
                &Scope::unrestricted(),
                &Arguments::new(),
            )
            .unwrap();

        let human_document = human
            .documents
            .get(human.workspace.active().unwrap())
            .unwrap();
        let agent_document = agent
            .documents
            .get(agent.workspace.active().unwrap())
            .unwrap();

        assert_eq!(
            human_document.content().node_count(),
            agent_document.content().node_count(),
            "the same operation produced the same content"
        );
        assert_eq!(human_document.history().entries().len(), 1);
        assert_eq!(agent_document.history().entries().len(), 1);
        assert!(!human_document.history().entries()[0].actor.is_agent());
        assert!(agent_document.history().entries()[0].actor.is_agent());
    }

    #[test]
    fn undo_is_unavailable_for_a_reason_a_person_can_act_on() {
        let (mut editor, registry) = editor_with_registry();
        let availability = registry.availability("edit.undo", &editor);
        assert_eq!(
            availability.reason().unwrap().because,
            "nothing has been changed in this document yet"
        );

        editor
            .invoke(
                &registry,
                "scene.create-entity",
                &Scope::unrestricted(),
                &Arguments::new(),
            )
            .unwrap();
        assert!(registry.availability("edit.undo", &editor).is_available());
        assert!(!registry.availability("edit.redo", &editor).is_available());
    }

    #[test]
    fn undo_and_redo_round_trip_a_session_through_commands_alone() {
        let (mut editor, registry) = editor_with_registry();
        let scope = Scope::unrestricted();
        for _ in 0..3 {
            editor
                .invoke(&registry, "scene.create-entity", &scope, &Arguments::new())
                .unwrap();
        }
        let id = editor.workspace.active().unwrap();
        assert_eq!(editor.documents.get(id).unwrap().content().node_count(), 3);

        for _ in 0..3 {
            editor
                .invoke(&registry, "edit.undo", &scope, &Arguments::new())
                .unwrap();
        }
        assert_eq!(editor.documents.get(id).unwrap().content().node_count(), 0);

        for _ in 0..3 {
            editor
                .invoke(&registry, "edit.redo", &scope, &Arguments::new())
                .unwrap();
        }
        assert_eq!(editor.documents.get(id).unwrap().content().node_count(), 3);
    }

    #[test]
    fn a_read_only_scope_cannot_reach_a_mutating_command() {
        use cy_editor_commands::scope::DocumentScope;
        let (mut editor, registry) = editor_with_registry();
        let read_only =
            cy_editor_commands::Scope::new("reader", DocumentScope::All, [EffectClass::Read]);

        let problem = editor
            .invoke(
                &registry,
                "scene.create-entity",
                &read_only,
                &Arguments::new(),
            )
            .unwrap_err();
        assert!(problem.because.contains("\"reader\""), "{problem}");

        // And the read-only command it *can* reach still works.
        let id = editor.workspace.active().unwrap();
        let node = editor.documents.get_mut(id).unwrap();
        let created = node
            .with_transaction("Create", Actor::human("designer"), |document| {
                document.create_node(None)
            })
            .unwrap();
        editor
            .invoke(
                &registry,
                "edit.select",
                &read_only,
                &Arguments::new().with("entity", Value::Text(created.to_string())),
            )
            .unwrap();
        assert_eq!(editor.selection.get().node_count(), 1);
    }

    #[test]
    fn a_malformed_entity_identity_is_refused_with_the_shape_that_would_work() {
        let (mut editor, registry) = editor_with_registry();
        let problem = editor
            .invoke(
                &registry,
                "edit.select",
                &Scope::unrestricted(),
                &Arguments::new().with("entity", Value::Text("the third lamp".into())),
            )
            .unwrap_err();
        assert!(
            problem.remedy.as_deref().unwrap().contains("32 hex digits"),
            "{problem}"
        );
    }

    #[test]
    fn saving_is_declared_irreversible_and_therefore_needs_an_agents_confirmation() {
        let mut registry = Registry::new();
        register(&mut registry).unwrap();
        let metadata = registry.metadata("file.save").unwrap();
        assert_eq!(metadata.effect, EffectClass::IrreversibleMutation);
        assert!(metadata.effect.needs_confirmation());
        // And an ordinary edit does not, because undo is the confirmation.
        assert!(
            !registry
                .metadata("scene.create-entity")
                .unwrap()
                .effect
                .needs_confirmation()
        );
    }
}
