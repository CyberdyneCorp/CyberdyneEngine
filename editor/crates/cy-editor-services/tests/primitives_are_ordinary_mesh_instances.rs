//! Creating a primitive, and the assertion that nothing can tell it from an import. M8.a tasks
//! 2.1, 2.2 and 2.3.
//!
//! Integration rather than unit for the reason `the_authoring_loop` gives: every claim here is
//! about a join. "One transaction per primitive" is a statement about the registry, the project's
//! file tree and the document's history being the same ones a person's click reaches, and a test
//! that stubbed any of them would prove nothing.
//!
//! THE CASE TO READ FIRST is `a_generated_primitive_and_an_imported_mesh_are_the_same_entity`. Task
//! 2.3 is an assertion rather than a style note, and that is where it is made.

#![allow(
    clippy::float_cmp,
    reason = "a placement asserts the EXACT value it was asked for. An epsilon would hide the \
              defect these cases exist to catch — a value that went through a text round trip, or \
              a placement the command quietly adjusted — which is the argument \
              `the_authoring_loop` makes for the same allow."
)]

use std::path::{Path, PathBuf};

use cy_editor_commands::registry::{Arguments, Registry};
use cy_editor_commands::scope::Scope;
use cy_editor_core::Actor;
use cy_editor_core::ids::NodeId;
use cy_editor_core::value::{Value, ValueKind};
use cy_editor_documents::Document;
use cy_editor_services::builtin;
use cy_editor_services::editor::Editor;
use cy_editor_services::primitives::{
    self, MeshBinding, Origin, Primitive, Shape, create_mesh_instance,
};
use cy_editor_services::project::ProjectService;
use cy_editor_viewport::gizmo::{Transform3, TransformBinding};

/// THE GOLDEN TEXT, PINNED ON BOTH SIDES OF THE PROCESS BOUNDARY.
///
/// `tools/import/tests/test_primitive.cpp` holds the identical string and asserts that the engine's
/// own writer produces it and its parser reads it back. The bytes are hashed into the derivation
/// key, so a spelling that drifts on one side is a second cooked mesh for one primitive — which is
/// why this is a byte comparison and not a parse-and-compare.
const GOLDEN_BOX: &str = "cyprim 1\nshape box\nname Crate\norigin base\nextent 1 2 0.5\n";

/// A directory that removes itself, declared as a project so the editor will write into it.
struct Sandbox(PathBuf);

impl Sandbox {
    fn new(name: &str) -> Self {
        let unique = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map_or(0, |elapsed| elapsed.as_nanos());
        let path = std::env::temp_dir().join(format!(
            "cy-primitives-{name}-{}-{unique}",
            std::process::id()
        ));
        std::fs::create_dir_all(&path).expect("a writable temporary directory");
        std::fs::write(path.join("project.json"), "{}").expect("a project manifest");
        Self(path)
    }

    fn path(&self) -> &Path {
        &self.0
    }

    fn read(&self, relative: &str) -> String {
        std::fs::read_to_string(self.0.join(relative)).expect("the file the editor wrote")
    }

    fn exists(&self, relative: &str) -> bool {
        self.0.join(relative).is_file()
    }
}

impl Drop for Sandbox {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

fn registry() -> Registry {
    let mut registry = Registry::new();
    builtin::register(&mut registry).expect("the built-in commands satisfy their own metadata");
    registry
}

/// An editor over a declared project, with an open world whose schema carries a Transform — which
/// is what an opened `.cyworld` gives it, and what `crate::worldfile` reads out of the engine's own
/// manifest.
fn editor_with_a_world(sandbox: &Sandbox) -> Editor {
    let mut editor =
        Editor::new(Actor::human("designer")).with_project(ProjectService::new(sandbox.path()));
    let mut document = Document::new("worlds/city.cyworld");
    let component = document.schema_mut().declare_type("Transform", false);
    for (name, kind) in [
        ("translation", ValueKind::Vec3),
        ("rotation", ValueKind::Quat),
        ("scale", ValueKind::Vec3),
    ] {
        document
            .schema_mut()
            .declare_field(component, name, kind, "part of a transform")
            .expect("a fresh schema");
    }
    let id = editor.documents.insert(document);
    editor.workspace.opened(id);
    editor
}

fn invoke(editor: &mut Editor, registry: &Registry, id: &str, arguments: &Arguments) -> String {
    editor
        .invoke(registry, id, &Scope::unrestricted(), arguments)
        .unwrap_or_else(|problem| panic!("{id} was refused: {problem:?}"))
        .summary
}

fn refusal(editor: &mut Editor, registry: &Registry, id: &str, arguments: &Arguments) -> String {
    format!(
        "{:?}",
        editor
            .invoke(registry, id, &Scope::unrestricted(), arguments)
            .expect_err("this invocation is refused")
    )
}

fn document(editor: &Editor) -> &Document {
    editor
        .documents
        .get(editor.workspace.active().expect("a document is active"))
        .expect("the active document")
}

fn only_node(editor: &Editor) -> NodeId {
    let nodes: Vec<NodeId> = document(editor).content().nodes().collect();
    assert_eq!(nodes.len(), 1, "one node: {nodes:?}");
    nodes[0]
}

fn translation_of(editor: &Editor, node: NodeId) -> [f32; 3] {
    let document = document(editor);
    let binding = TransformBinding::of_schema(document.schema()).expect("a Transform");
    match document
        .content()
        .field(node, binding.component, binding.translation)
    {
        Some(Value::Vec3(lanes)) => *lanes,
        other => panic!("no translation: {other:?}"),
    }
}

fn a_box() -> Arguments {
    Arguments::new().with("shape", Value::Text("box".to_string()))
}

// --- The source asset ----------------------------------------------------------------------------

#[test]
fn the_canonical_source_text_is_the_one_the_engine_parses() {
    let crate_box = Primitive {
        shape: Shape::Box,
        name: "Crate".to_string(),
        origin: Origin::Base,
        extent: [1.0, 2.0, 0.5],
        ..Primitive::default()
    };
    assert_eq!(crate_box.source_text(), GOLDEN_BOX);
    assert_eq!(
        crate_box.default_asset_path(),
        "assets/primitives/Crate.cyprim"
    );
}

#[test]
fn every_shape_writes_only_the_parameters_it_takes() {
    // The refusal in the source format is that a parameter a shape does not take is an error. This
    // is the writer's half: it never emits one, so a source this editor writes always parses.
    for shape in Shape::ALL {
        let text = Primitive {
            shape,
            name: shape.label().to_string(),
            ..Primitive::default()
        }
        .source_text();
        assert!(text.starts_with("cyprim 1\n"), "{text}");
        assert!(
            text.contains(&format!("shape {}\n", shape.keyword())),
            "{text}"
        );
        assert_eq!(
            text.contains("extent "),
            matches!(shape, Shape::Box | Shape::Plane)
        );
        assert_eq!(
            text.contains("radius "),
            matches!(shape, Shape::Sphere | Shape::Cylinder | Shape::Capsule)
        );
        assert_eq!(
            text.contains("height "),
            matches!(shape, Shape::Cylinder | Shape::Capsule)
        );
        assert_eq!(text.contains("subdivisions "), shape == Shape::Plane);
        assert_eq!(
            text.contains("rings "),
            matches!(shape, Shape::Sphere | Shape::Capsule)
        );
    }
}

#[test]
fn a_created_primitive_writes_a_source_asset_into_the_project() {
    let sandbox = Sandbox::new("writes");
    let registry = registry();
    let mut editor = editor_with_a_world(&sandbox);

    let said = invoke(
        &mut editor,
        &registry,
        "scene.create-primitive",
        &a_box()
            .with("name", Value::Text("Crate".to_string()))
            .with("origin", Value::Text("base".to_string()))
            .with("extent", Value::Vec3([1.0, 2.0, 0.5])),
    );
    assert!(said.contains("Crate"), "{said}");
    assert_eq!(sandbox.read("assets/primitives/Crate.cyprim"), GOLDEN_BOX);
}

#[test]
fn creating_the_same_primitive_twice_reuses_one_asset() {
    // Two identical boxes are two entities and ONE source file, so the cook cache answers the
    // second one — which is what task 2.4 means downstream of the editor.
    let sandbox = Sandbox::new("reuse");
    let registry = registry();
    let mut editor = editor_with_a_world(&sandbox);

    invoke(&mut editor, &registry, "scene.create-primitive", &a_box());
    invoke(&mut editor, &registry, "scene.create-primitive", &a_box());

    assert_eq!(document(&editor).content().node_count(), 2);
    assert_eq!(
        sandbox.read("assets/primitives/Box.cyprim").lines().count(),
        5
    );
}

#[test]
fn a_different_primitive_at_the_same_path_is_refused_rather_than_overwritten() {
    let sandbox = Sandbox::new("collide");
    let registry = registry();
    let mut editor = editor_with_a_world(&sandbox);

    invoke(&mut editor, &registry, "scene.create-primitive", &a_box());
    let said = refusal(
        &mut editor,
        &registry,
        "scene.create-primitive",
        &a_box().with("extent", Value::Vec3([4.0, 4.0, 4.0])),
    );
    assert!(said.contains("already"), "{said}");
    // And nothing was created: a refused command leaves one entity, not two.
    assert_eq!(document(&editor).content().node_count(), 1);
}

#[test]
fn the_parameters_are_editable_through_the_asset_command() {
    // "with editable parameters", as task 2.1 asks. Editing them is editing the ASSET, which is
    // what makes every entity drawing it follow — and what makes a primitive a mesh rather than a
    // node with a shape on it.
    let sandbox = Sandbox::new("edit");
    let registry = registry();
    let mut editor = editor_with_a_world(&sandbox);

    invoke(&mut editor, &registry, "scene.create-primitive", &a_box());
    let before = sandbox.read("assets/primitives/Box.cyprim");

    invoke(
        &mut editor,
        &registry,
        "asset.write-primitive",
        &a_box().with("extent", Value::Vec3([3.0, 0.25, 3.0])),
    );
    let after = sandbox.read("assets/primitives/Box.cyprim");
    assert_ne!(before, after);
    assert!(after.contains("extent 3 0.25 3\n"), "{after}");
    // The entity is untouched: it references a path, and the mesh behind the path changed.
    assert_eq!(document(&editor).content().node_count(), 1);
}

#[test]
fn a_parameter_the_shape_does_not_take_is_refused() {
    let sandbox = Sandbox::new("refuse");
    let registry = registry();
    let mut editor = editor_with_a_world(&sandbox);

    let said = refusal(
        &mut editor,
        &registry,
        "scene.create-primitive",
        &a_box().with("radius", Value::Float(2.0)),
    );
    assert!(said.contains("radius"), "{said}");
    assert!(!sandbox.exists("assets/primitives/Box.cyprim"));

    let out_of_range = refusal(
        &mut editor,
        &registry,
        "scene.create-primitive",
        &Arguments::new()
            .with("shape", Value::Text("sphere".to_string()))
            .with("segments", Value::Int(2)),
    );
    assert!(out_of_range.contains("segments"), "{out_of_range}");

    let unknown = refusal(
        &mut editor,
        &registry,
        "scene.create-primitive",
        &Arguments::new().with("shape", Value::Text("dodecahedron".to_string())),
    );
    assert!(unknown.contains("dodecahedron"), "{unknown}");
}

// --- The transaction ------------------------------------------------------------------------------

#[test]
fn a_primitive_is_one_transaction_that_undoes_and_redoes_to_the_same_identity() {
    // Task 2.2, in one case: ONE transaction per primitive, undo removes it, redo restores the same
    // identity — which is what makes a reference to it survive an undo somebody changed their mind
    // about.
    let sandbox = Sandbox::new("undo");
    let registry = registry();
    let mut editor = editor_with_a_world(&sandbox);

    invoke(
        &mut editor,
        &registry,
        "scene.create-primitive",
        &a_box().with("at", Value::Vec3([1.0, 2.0, 3.0])),
    );
    let created = only_node(&editor);
    assert_eq!(translation_of(&editor, created), [1.0, 2.0, 3.0]);
    assert_eq!(document(&editor).history_status().undoable, 1);

    invoke(&mut editor, &registry, "edit.undo", &Arguments::new());
    assert_eq!(document(&editor).content().node_count(), 0);

    invoke(&mut editor, &registry, "edit.redo", &Arguments::new());
    assert_eq!(only_node(&editor), created);
    assert_eq!(translation_of(&editor, created), [1.0, 2.0, 3.0]);
    assert_eq!(
        primitives::mesh_of(document(&editor), created).as_deref(),
        Some("assets/primitives/Box.cyprim")
    );
}

#[test]
fn undo_leaves_no_content_behind() {
    // The residue question, asked of the content rather than of the schema: after an undo the
    // document holds no node, no component and no asset reference. The schema keeps the type it
    // declared, exactly as it keeps Transform — a `.cyworld` already declares seven types that no
    // node in it uses.
    //
    // The comparison is of the CONTENT rather than of the whole `DocumentContent`, because the
    // ordinal counter and the revision are monotonic on purpose: "ordinals are never reused, so a
    // deleted node's identifier stays retired and an undo that recreates it can put back the same
    // one", which is the property the case above depends on. A counter that went backwards here
    // would break redo, not fix residue.
    let sandbox = Sandbox::new("residue");
    let registry = registry();
    let mut editor = editor_with_a_world(&sandbox);
    let saved_before = cy_editor_services::write_world(document(&editor));

    invoke(&mut editor, &registry, "scene.create-primitive", &a_box());
    invoke(&mut editor, &registry, "edit.undo", &Arguments::new());

    let after = document(&editor).content();
    assert_eq!(after.node_count(), 0);
    assert!(after.roots().is_empty());
    assert_eq!(after.nodes().count(), 0);

    // And what would be SAVED differs only by the type the schema declared: no node line, no
    // component line, no value.
    let saved_after = cy_editor_services::write_world(document(&editor));
    assert!(!saved_after.contains("node "), "{saved_after}");
    assert!(!saved_after.contains("component "), "{saved_after}");
    assert_eq!(
        saved_after.replace(
            &format!(
                "type 2 runtime \"{}\"\n  field 4 text \"{}\" \"The mesh asset this entity draws.\"\n",
                MeshBinding::COMPONENT,
                MeshBinding::FIELD
            ),
            ""
        ),
        saved_before,
        "the only difference a created-then-undone primitive leaves is the declared type"
    );
}

#[test]
fn a_created_primitive_is_selected_and_placed_where_it_was_asked_for() {
    let sandbox = Sandbox::new("selected");
    let registry = registry();
    let mut editor = editor_with_a_world(&sandbox);

    invoke(
        &mut editor,
        &registry,
        "scene.create-primitive",
        &Arguments::new()
            .with("shape", Value::Text("sphere".to_string()))
            .with("at", Value::Vec3([0.0, 4.0, 0.0])),
    );
    let sphere = only_node(&editor);
    assert_eq!(
        editor.selection.get().nodes().collect::<Vec<_>>(),
        vec![sphere]
    );
    assert_eq!(translation_of(&editor, sphere), [0.0, 4.0, 0.0]);
}

// --- Task 2.3 ---------------------------------------------------------------------------------------

#[test]
fn a_generated_primitive_and_an_imported_mesh_are_the_same_entity() {
    // TASK 2.3, WHICH IS AN ASSERTION AND NOT A STYLE NOTE. A generated box and an imported mesh of
    // the same shape are put in one world, and every question a downstream system asks — what
    // components has it, what does the gizmo bind to, what does the inspector show, what does the
    // saved file say, which asset does the cook follow — is asked of both.
    //
    // They are the same because they are built by the same function. `create_mesh_instance` is what
    // an import calls too (task 3.5), and it has no parameter that could make them differ.
    let sandbox = Sandbox::new("indistinguishable");
    let registry = registry();
    let mut editor = editor_with_a_world(&sandbox);

    invoke(
        &mut editor,
        &registry,
        "scene.create-primitive",
        &a_box().with("at", Value::Vec3([1.0, 0.0, 0.0])),
    );
    let generated = only_node(&editor);

    // The import's half, through the same constructor with an imported file's path.
    let id = editor.workspace.active().expect("a document is active");
    let imported = editor
        .documents
        .get_mut(id)
        .expect("the active document")
        .with_transaction("Import crate", Actor::human("designer"), |document| {
            create_mesh_instance(
                document,
                None,
                "assets/models/crate.gltf",
                Transform3 {
                    translation: cy_editor_viewport::math::Vec3::new(1.0, 0.0, 0.0),
                    ..Transform3::default()
                },
            )
        })
        .expect("the import lands");

    let document = document(&editor);
    let generated_state = document.content().node(generated).expect("the box");
    let imported_state = document.content().node(imported).expect("the crate");

    // Selection and the inspector read components: the same set, in the same shape.
    assert_eq!(
        generated_state.components.keys().collect::<Vec<_>>(),
        imported_state.components.keys().collect::<Vec<_>>()
    );
    // The gizmo binds to a component and a field: the same ones, holding the same value.
    let transform = TransformBinding::of_schema(document.schema()).expect("a Transform");
    assert_eq!(
        document
            .content()
            .field(generated, transform.component, transform.translation),
        document
            .content()
            .field(imported, transform.component, transform.translation)
    );
    // The mesh reference is the same field on the same component, and only the path differs.
    let mesh = MeshBinding::of_schema(document.schema()).expect("a MeshRenderer");
    assert!(generated_state.components.contains_key(&mesh.component));
    assert!(imported_state.components.contains_key(&mesh.component));
    assert_eq!(
        primitives::mesh_of(document, generated).as_deref(),
        Some("assets/primitives/Box.cyprim")
    );
    assert_eq!(
        primitives::mesh_of(document, imported).as_deref(),
        Some("assets/models/crate.gltf")
    );

    // Saving: the two nodes are written by the same writer, and the only difference in their lines
    // is the path each references. Compare the two blocks with the paths removed.
    let written = cy_editor_services::write_world(document);
    let blocks: Vec<&str> = written.split("node ").skip(1).collect();
    assert_eq!(blocks.len(), 2, "{written}");
    let strip = |text: &str| {
        text.replace("assets/primitives/Box.cyprim", "<asset>")
            .replace("assets/models/crate.gltf", "<asset>")
    };
    assert_eq!(
        strip(blocks[0])
            .split_once(' ')
            .map(|(_, rest)| rest.to_string()),
        strip(blocks[1])
            .split_once(' ')
            .map(|(_, rest)| rest.to_string()),
        "{written}"
    );
}

#[test]
fn the_commands_declare_themselves_the_way_the_registry_demands() {
    // `Registry::register` refuses a command a caller that cannot see the interface could not use,
    // so this passing at all is most of the assertion. What it adds is that both commands are
    // listed, and that the shape parameter says what the five shapes are — the one thing a machine
    // caller cannot guess.
    let registry = registry();
    let listing = registry.projection().join("\n");
    assert!(listing.contains("scene.create-primitive"), "{listing}");
    assert!(listing.contains("asset.write-primitive"), "{listing}");
    for shape in Shape::ALL {
        assert!(listing.contains(shape.keyword()), "{listing}");
    }
}

#[test]
fn a_primitive_survives_a_save_and_a_load() {
    // Saving is one of the six things task 2.3 lists, and a reference that wrote but did not read
    // back would satisfy the writer comparison above and still lose the mesh. So the world is
    // written and loaded into a second document — the path `file.save` and opening a world take —
    // and the asset is asked for again on the other side.
    let sandbox = Sandbox::new("roundtrip");
    let registry = registry();
    let mut editor = editor_with_a_world(&sandbox);

    invoke(
        &mut editor,
        &registry,
        "scene.create-primitive",
        &Arguments::new()
            .with("shape", Value::Text("capsule".to_string()))
            .with("name", Value::Text("Actor".to_string()))
            .with("at", Value::Vec3([2.0, 0.5, -1.0])),
    );
    let written = cy_editor_services::write_world(document(&editor));

    let mut reopened = Document::new("worlds/city.cyworld");
    cy_editor_services::worldfile::load(&written, &mut reopened, Actor::human("designer"))
        .expect("the world this editor just wrote loads back");

    let nodes: Vec<NodeId> = reopened.content().nodes().collect();
    assert_eq!(nodes.len(), 1);
    assert_eq!(
        primitives::mesh_of(&reopened, nodes[0]).as_deref(),
        Some("assets/primitives/Actor.cyprim")
    );
    // And it writes back byte for byte, which is what makes a save that changed nothing a no-op
    // diff rather than a whole-file one.
    assert_eq!(cy_editor_services::write_world(&reopened), written);
}
