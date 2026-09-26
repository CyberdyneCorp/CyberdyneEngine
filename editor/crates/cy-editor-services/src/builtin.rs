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
use cy_editor_viewport::gizmo::{Transform3, TransformBinding};

use crate::project::ProjectService;

/// Register the editor's built-in commands.
///
/// Fails if any of them would not satisfy a caller that has never seen the interface, which makes
/// this function a test of its own contents every time it runs.
pub fn register(registry: &mut Registry) -> Result<()> {
    registry.register(create_entity())?;
    registry.register(delete_entity())?;
    registry.register(select())?;
    registry.register(rename_entity())?;
    registry.register(reparent_entity())?;
    registry.register(undo())?;
    registry.register(redo())?;
    registry.register(save())?;
    // The viewport's own controls — transform modes, pivots, view presets and the engine's debug
    // views. In their own module because there are thirty of them and they are generated from the
    // model rather than written out.
    crate::viewports::register(registry)?;
    // Move, rotate and scale by a stated amount, through the same manipulation a gizmo drag
    // performs — see `crate::manipulate`.
    crate::manipulate::register(registry)?;
    // Writing source, building it, reloading it, and play. See `crate::authoring`.
    crate::authoring::register(registry)?;
    crate::material_commands::register(registry)?;
    crate::vfx_commands::register(registry)?;
    // Creating a box, a sphere, a cylinder, a plane or a capsule — as a generated source asset and
    // an ordinary mesh instance. See `crate::primitives`.
    crate::primitives::register(registry)?;
    crate::scene_actors::register(registry)?;
    // Importing a source asset from inside the editor, and landing it in the world. See
    // `crate::assets`.
    crate::assets::register(registry)?;
    // Adding a physics body and its collider to an entity, as one undoable transaction. See
    // `crate::bodies`.
    crate::bodies::register(registry)?;
    // Shared painting gestures committed as stable terrain layers and non-destructive modifiers.
    crate::terrain::register(registry)?;
    // Project settings and user preferences, through typed command parameters.
    crate::settings::register(registry)?;
    crate::source_control::register_commands(registry)?;
    crate::merge_commands::register(registry)?;
    Ok(())
}

fn rename_entity() -> Command {
    Command::new(
        Metadata::new(
            "scene.rename-entity",
            "Rename Entity",
            "Scene",
            "Changes an entity's author-facing name in one undoable transaction without changing \
             its stable identity.",
            EffectClass::ReversibleMutation,
        )
        .with(ParameterSpec::required(
            "entity",
            ValueKind::Text,
            "The stable identity of the entity whose name changes.",
        ))
        .with(ParameterSpec::required(
            "name",
            ValueKind::Text,
            "The non-empty author-facing name to show in the hierarchy.",
        )),
        |context, arguments| {
            let id = active(context)?;
            let node = parse_node(arguments.text("entity").unwrap_or_default())?
                .ok_or_else(|| Problem::new("rename an entity", "no entity was named"))?;
            let name = arguments.text("name").unwrap_or_default().trim();
            if name.is_empty() || name.chars().count() > 256 || name.chars().any(char::is_control) {
                return Err(Problem::new(
                    "rename an entity",
                    "a name is 1 to 256 visible characters",
                )
                .with_remedy("enter a short, non-empty name with no control characters"));
            }
            let actor = context.actor();
            context
                .document_mut(id)
                .ok_or_else(|| Problem::not_found("the active document"))?
                .with_transaction("Rename entity", actor, |document| {
                    document.set_name(node, name)
                })?;
            Ok(Outcome::new(format!("Renamed entity to {name}")))
        },
    )
}

fn reparent_entity() -> Command {
    Command::new(
        Metadata::new(
            "scene.reparent-entity",
            "Reparent Entity",
            "Scene",
            "Moves an entity under another stable entity identity, or to the world root, in one \
             undoable transaction.",
            EffectClass::ReversibleMutation,
        )
        .with(ParameterSpec::required(
            "entity",
            ValueKind::Text,
            "The stable identity of the entity to move.",
        ))
        .with(ParameterSpec::optional(
            "parent",
            ValueKind::Text,
            "The stable identity of the new parent; empty moves to the root.",
            Value::Text(String::new()),
        )),
        |context, arguments| {
            let id = active(context)?;
            let node = parse_node(arguments.text("entity").unwrap_or_default())?
                .ok_or_else(|| Problem::new("reparent an entity", "no entity was named"))?;
            let parent = parse_node(arguments.text("parent").unwrap_or_default())?;
            let actor = context.actor();
            context
                .document_mut(id)
                .ok_or_else(|| Problem::not_found("the active document"))?
                .with_transaction("Reparent entity", actor, |document| {
                    document.reparent_node(node, parent)
                })?;
            Ok(Outcome::new("Reparented entity"))
        },
    )
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
        .with(ParameterSpec::optional(
            "template",
            ValueKind::Text,
            "The registered entity template to instantiate. The built-in empty template is named \
             `empty`; omit it for the same result.",
            Value::Text("empty".into()),
        ))
        .bound_to("Ctrl+Shift+N"),
        |context, arguments| {
            let id = active(context)?;
            let actor = context.actor();
            let template = arguments.text("template").unwrap_or("empty");
            if template != "empty" {
                return Err(Problem::new(
                    "create an entity from a template",
                    format!("template `{template}` is not registered"),
                )
                .with_remedy("use the built-in `empty` template"));
            }
            let parent = parse_node(arguments.text("parent").unwrap_or_default())?;
            let document = context
                .document_mut(id)
                .ok_or_else(|| Problem::not_found("the active document"))?;
            let node = document.with_transaction("Create entity", actor, |document| {
                let node = document.create_node(parent)?;
                place(document, node)?;
                Ok(node)
            })?;

            let mut selection = Selection::new();
            selection.add_node(node);
            context.set_selection(selection);

            Ok(Outcome::new("Created an entity").with("entity", Value::Text(node.to_string())))
        },
    )
}

/// Give a new entity the transform the document's schema describes, if it describes one.
///
/// An entity with no transform cannot be placed, cannot be framed, and cannot be dragged: the gizmo
/// binds to a component and there is none. Before M6 the schema was empty and this could not have
/// been written; now that a world carries its schema, "Create Entity" means an entity that is
/// somewhere rather than an entity that is nowhere.
///
/// A document whose schema declares no transform still creates the entity. That is not a fallback
/// with a hidden cost — it is the honest answer for a document kind that has no spatial meaning,
/// and `TransformBinding` exists precisely so the answer is a lookup rather than a hard-coded rule.
fn place(
    document: &mut cy_editor_documents::Document,
    node: cy_editor_core::ids::NodeId,
) -> Result<()> {
    let Some(binding) = TransformBinding::of_schema(document.schema()) else {
        return Ok(());
    };
    let identity = Transform3::default();
    document.add_component(
        node,
        binding.component,
        vec![
            (
                binding.translation,
                Value::Vec3(identity.translation.to_array()),
            ),
            (binding.rotation, Value::Quat(identity.rotation.to_array())),
            (binding.scale, Value::Vec3(identity.scale.to_array())),
        ],
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
            let undone = document.undo()?;
            // A source edit's operation is one the document model records and does not interpret,
            // so undoing it in the document is only half of the work: the file has to go back too.
            // See `crate::project::rewind` for why that lives here rather than in the document.
            rewind_sources(context, id, undone.as_ref());
            match undone {
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
            let redone = document.redo()?;
            replay_sources(context, id, redone.as_ref());
            match redone {
                Some(transaction) => Ok(Outcome::new(format!("Redid {}", transaction.name))),
                None => Ok(Outcome::new("Redid nothing")),
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
            // The root is read BEFORE the document is borrowed mutably, and it is what turns a save
            // into a file. M5.5 wrote "writing the assets themselves is the serialisation layer's,
            // at a later task"; this is that task, and `crate::worldfile` is that layer.
            let root = context.project().map(|project| project.project_root());
            let document = context
                .document_mut(id)
                .ok_or_else(|| Problem::not_found("the active document"))?;
            let asset = document.assets().first().cloned().unwrap_or_default();
            // The sequence is the part with a correctness argument: the journal is reset only after
            // the write reports success, so a failed write leaves the work recoverable.
            // ONE RULE, THE SAME ONE THE DOCUMENTS ARE ROOTED BY: a project is a directory that
            // says it is one. Without it a save writes a world wherever the editor happened to be
            // started, which put a `worlds/` directory in this repository the first time this
            // command learned to write one — see `ProjectService::is_declared`.
            let target = root
                .map(std::path::PathBuf::from)
                .filter(|root| !asset.is_empty() && ProjectService::declares(root))
                .map(|root| root.join(&asset));
            document.save(|document| match &target {
                Some(path) => crate::worldfile::write_to(document, path),
                None => Ok(()),
            })?;
            Ok(Outcome::new("Saved").with("asset", Value::Text(asset)))
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

/// Put the files a just-undone transaction changed back to what they were.
///
/// Nothing happens for a transaction with no source operations in it, which is almost all of them.
fn rewind_sources(
    context: &mut dyn CommandContext,
    document: cy_editor_core::ids::DocumentId,
    transaction: Option<&cy_editor_documents::transaction::Transaction>,
) {
    apply_sources(context, document, transaction, false);
}

/// Put them back to what it made them.
fn replay_sources(
    context: &mut dyn CommandContext,
    document: cy_editor_core::ids::DocumentId,
    transaction: Option<&cy_editor_documents::transaction::Transaction>,
) {
    apply_sources(context, document, transaction, true);
}

/// The half the two share, expressed against the [`CommandContext`] so that undo works the same
/// whether it was invoked by a key, the palette or an agent.
fn apply_sources(
    context: &mut dyn CommandContext,
    document: cy_editor_core::ids::DocumentId,
    transaction: Option<&cy_editor_documents::transaction::Transaction>,
    forward: bool,
) {
    let Some(transaction) = transaction else {
        return;
    };
    let Some(path) = context
        .document(document)
        .and_then(|document| document.assets().first().cloned())
    else {
        return;
    };
    let wanted: Vec<Option<String>> = transaction
        .operations
        .iter()
        .filter_map(|operation| match operation {
            cy_editor_documents::operation::Operation::Domain {
                kind,
                before,
                after,
                ..
            } if kind == crate::project::SOURCE_DOMAIN => {
                Some(crate::project::decode_source(if forward {
                    after
                } else {
                    before
                }))
            }
            _ => None,
        })
        .collect();
    let moves: Vec<(String, String, String)> = transaction
        .operations
        .iter()
        .filter_map(|operation| match operation {
            cy_editor_documents::operation::Operation::Domain {
                kind,
                before,
                after,
                ..
            } if kind == crate::assets::ASSET_MOVE_DOMAIN => {
                let (from, expected) =
                    crate::assets::decode_asset_move(if forward { before } else { after })?;
                let (to, _) =
                    crate::assets::decode_asset_move(if forward { after } else { before })?;
                Some((from, to, expected))
            }
            _ => None,
        })
        .collect();
    let graphs: Vec<(String, Option<String>, Option<String>)> = transaction
        .operations
        .iter()
        .filter_map(|operation| match operation {
            cy_editor_documents::operation::Operation::Domain {
                kind,
                before,
                after,
                ..
            } => {
                let reference = kind.strip_prefix(crate::material_graph::GRAPH_DOMAIN_PREFIX)?;
                let (graph, source) =
                    crate::material_graph::decode_pair(if forward { after } else { before })
                        .ok()?;
                Some((reference.to_owned(), graph, source))
            }
            _ => None,
        })
        .collect();
    let vfx_documents = vfx_sources(transaction, forward);
    if wanted.is_empty() && moves.is_empty() && graphs.is_empty() && vfx_documents.is_empty() {
        return;
    }
    let Some(project) = context.project() else {
        return;
    };
    for contents in wanted {
        // Swallowed deliberately: the history has already moved, and refusing here would leave the
        // editor's history and the file system disagreeing with nothing able to say so. See
        // `crate::project::apply_domain`, which makes the same argument at greater length.
        let _ = project.put_source(&path, contents.as_deref());
    }
    for (from, to, expected) in moves {
        let _ = project.move_asset_if_unchanged(&from, &to, &expected);
    }
    for (reference, graph, source) in graphs {
        let canvas = std::path::Path::new(&reference).with_extension("cymatcanvas");
        let _ = project.put_source(&reference, graph.as_deref());
        let _ = project.put_source(&canvas.to_string_lossy(), source.as_deref());
        if let Some(source) = source.as_deref() {
            let _ = project.material_graph_preview(&reference, source);
        }
    }
    for (reference, source) in vfx_documents {
        let _ = project.put_source(&reference, source.as_deref());
    }
}

fn vfx_sources(
    transaction: &cy_editor_documents::transaction::Transaction,
    forward: bool,
) -> Vec<(String, Option<String>)> {
    transaction
        .operations
        .iter()
        .filter_map(|operation| match operation {
            cy_editor_documents::operation::Operation::Domain {
                kind,
                before,
                after,
                ..
            } => Some((
                kind.strip_prefix(crate::vfx_document::DOMAIN_PREFIX)?
                    .to_owned(),
                crate::project::decode_source(if forward { after } else { before }),
            )),
            _ => None,
        })
        .collect()
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
        // added without metadata cannot slip through by simply not being registered. The number
        // grew twice at M5.5: with the viewport's own controls — thirty-seven generated in
        // `crate::viewports` from the four transform modes, four pivots, seven view presets,
        // nineteen debug views and three switches — and with the authoring loop: three stated
        // manipulations in `crate::manipulate`, and two source commands, a build, a reload and
        // three play states in `crate::authoring`.
        // M8.a grew it again: two primitive commands in `crate::primitives`
        // (`scene.create-primitive` and `asset.write-primitive`), `asset.import` in
        // `crate::assets`, and three body commands in `crate::bodies` (`scene.add-body`,
        // `scene.add-collider` and `scene.remove-body`).
        // The editor-completion change adds identity-based rename/reparent, six typed settings
        // commands, seven provider-neutral source-control commands, two semantic-merge commands,
        // and six asset-browser operations, including asynchronous external import.
        // Terrain authoring adds create, add-layer, commit-stroke, enable, and reorder commands.
        // Scene actors add camera and light creation.
        // Material graphs add read, preview, save, and status commands; VFX drafts add read/save
        // and five engine preview commands.
        let mut registry = Registry::new();
        register(&mut registry).unwrap();
        assert_eq!(
            registry.len(),
            8 + 37 + 3 + 7 + 2 + 1 + 3 + 6 + 7 + 2 + 6 + 5 + 2 + 4 + 7
        );
        for metadata in registry.all() {
            metadata.validate().unwrap();
            assert!(!metadata.description.is_empty());
        }
    }

    #[test]
    fn material_graph_save_is_undoable_through_the_registered_commands() {
        let root =
            std::env::temp_dir().join(format!("cy-graph-undo-command-{}", std::process::id()));
        std::fs::create_dir_all(&root).unwrap();
        let mut editor = Editor::default().with_project(crate::project::ProjectService::new(&root));
        let scene = editor.open_document("worlds/city.cyworld").unwrap();
        let reference = "materials/cube.cygraph";
        editor
            .project
            .put_source(reference, Some("new graph"))
            .unwrap();
        editor
            .project
            .put_source("materials/cube.cymatcanvas", Some("new source"))
            .unwrap();
        editor
            .documents
            .get_mut(scene)
            .unwrap()
            .with_transaction("Save material graph", Actor::human("designer"), |doc| {
                doc.record(cy_editor_documents::operation::Operation::Domain {
                    node: None,
                    kind: format!("{}{reference}", crate::material_graph::GRAPH_DOMAIN_PREFIX),
                    before: crate::material_graph::encode_pair(
                        Some("old graph"),
                        Some("old source"),
                    ),
                    after: crate::material_graph::encode_pair(
                        Some("new graph"),
                        Some("new source"),
                    ),
                })
            })
            .unwrap();
        let mut registry = Registry::new();
        register(&mut registry).unwrap();
        editor
            .invoke(
                &registry,
                "edit.undo",
                &Scope::unrestricted(),
                &Arguments::new(),
            )
            .unwrap();
        assert_eq!(editor.project.read_source(reference).unwrap(), "old graph");
        assert_eq!(
            editor
                .project
                .read_source("materials/cube.cymatcanvas")
                .unwrap(),
            "old source"
        );
        editor
            .invoke(
                &registry,
                "edit.redo",
                &Scope::unrestricted(),
                &Arguments::new(),
            )
            .unwrap();
        assert_eq!(editor.project.read_source(reference).unwrap(), "new graph");
        assert_eq!(
            editor
                .project
                .read_source("materials/cube.cymatcanvas")
                .unwrap(),
            "new source"
        );
        std::fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn vfx_document_commands_save_read_undo_and_redo_one_project_asset() {
        let root = std::env::temp_dir().join(format!(
            "cy-vfx-document-command-{}-{:?}",
            std::process::id(),
            std::thread::current().id()
        ));
        std::fs::create_dir_all(&root).unwrap();
        let mut editor = Editor::default().with_project(crate::project::ProjectService::new(&root));
        editor.open_document("worlds/city.cyworld").unwrap();
        let mut registry = Registry::new();
        register(&mut registry).unwrap();
        let reference = "effects/sparks.cyvfxdoc";
        let source = "cyvfxdoc 1\n0100000006000000737061726b730000000000000000";
        let scope = Scope::unrestricted();
        let save = Arguments::new()
            .with("reference", Value::Text(reference.into()))
            .with("source", Value::Text(source.into()));
        editor
            .invoke(&registry, "vfx.document.save", &scope, &save)
            .unwrap();
        let read = Arguments::new().with("reference", Value::Text(reference.into()));
        let outcome = editor
            .invoke(&registry, "vfx.document.read", &scope, &read)
            .unwrap();
        assert_eq!(
            outcome.values.get("source"),
            Some(&Value::Text(source.into()))
        );

        editor
            .invoke(&registry, "edit.undo", &scope, &Arguments::new())
            .unwrap();
        assert!(!editor.project.source_exists(reference));
        editor
            .invoke(&registry, "edit.redo", &scope, &Arguments::new())
            .unwrap();
        assert_eq!(editor.project.read_source(reference).unwrap(), source);
        assert!(
            editor
                .invoke(
                    &registry,
                    "vfx.document.save",
                    &scope,
                    &Arguments::new()
                        .with("reference", Value::Text("../outside.cyvfxdoc".into()))
                        .with("source", Value::Text(source.into())),
                )
                .is_err()
        );
        std::fs::remove_dir_all(root).unwrap();
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

    #[test]
    fn rename_and_reparent_are_transactional_identity_based_and_exactly_undoable() {
        let (mut editor, registry) = editor_with_registry();
        let document_id = editor.workspace.active().unwrap();
        let (parent, child) = editor
            .documents
            .get_mut(document_id)
            .unwrap()
            .with_transaction("Build", Actor::human("designer"), |document| {
                Ok((document.create_node(None)?, document.create_node(None)?))
            })
            .unwrap();
        let scope = Scope::unrestricted();

        editor
            .invoke(
                &registry,
                "scene.rename-entity",
                &scope,
                &Arguments::new()
                    .with("entity", Value::Text(child.to_string()))
                    .with("name", Value::Text("Lamp".into())),
            )
            .unwrap();
        editor
            .invoke(
                &registry,
                "scene.reparent-entity",
                &scope,
                &Arguments::new()
                    .with("entity", Value::Text(child.to_string()))
                    .with("parent", Value::Text(parent.to_string())),
            )
            .unwrap();
        let state = editor
            .documents
            .get(document_id)
            .unwrap()
            .content()
            .node(child)
            .unwrap();
        assert_eq!(state.name, "Lamp");
        assert_eq!(state.parent, Some(parent));

        editor
            .invoke(&registry, "edit.undo", &scope, &Arguments::new())
            .unwrap();
        assert_eq!(
            editor
                .documents
                .get(document_id)
                .unwrap()
                .content()
                .node(child)
                .unwrap()
                .parent,
            None
        );
        editor
            .invoke(&registry, "edit.undo", &scope, &Arguments::new())
            .unwrap();
        assert_eq!(
            editor
                .documents
                .get(document_id)
                .unwrap()
                .content()
                .node(child)
                .unwrap()
                .name,
            ""
        );
    }

    #[test]
    fn hierarchy_authoring_refuses_invalid_targets_cycles_and_templates() {
        let (mut editor, registry) = editor_with_registry();
        let document_id = editor.workspace.active().unwrap();
        let child = editor
            .documents
            .get_mut(document_id)
            .unwrap()
            .with_transaction("Build", Actor::human("designer"), |document| {
                document.create_node(None)
            })
            .unwrap();
        let scope = Scope::unrestricted();
        let invalid = editor
            .invoke(
                &registry,
                "scene.rename-entity",
                &scope,
                &Arguments::new()
                    .with("entity", Value::Text(child.to_string()))
                    .with("name", Value::Text("  ".into())),
            )
            .unwrap_err();
        assert!(invalid.remedy.is_some());

        let missing = NodeId::from_u128(u128::MAX);
        let missing_error = editor
            .invoke(
                &registry,
                "scene.rename-entity",
                &scope,
                &Arguments::new()
                    .with("entity", Value::Text(missing.to_string()))
                    .with("name", Value::Text("Missing".into())),
            )
            .unwrap_err();
        assert!(
            missing_error.because.contains("there is no"),
            "{missing_error}"
        );

        let cycle = editor
            .invoke(
                &registry,
                "scene.reparent-entity",
                &scope,
                &Arguments::new()
                    .with("entity", Value::Text(child.to_string()))
                    .with("parent", Value::Text(child.to_string())),
            )
            .unwrap_err();
        assert!(cycle.because.contains("inside itself"), "{cycle}");

        let unknown_template = editor
            .invoke(
                &registry,
                "scene.create-entity",
                &scope,
                &Arguments::new().with("template", Value::Text("camera".into())),
            )
            .unwrap_err();
        assert!(unknown_template.because.contains("not registered"));
    }
}
