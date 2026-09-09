//! `asset.import`: import from inside the editor, and land what it produced in the world.
//! M8.a tasks 3.1, 3.3 and 3.5.
//!
//! --- WHAT IS BEING ASSERTED, AND WHY A DOUBLE IS THE RIGHT SEAM --------------------------------
//!
//! The importers themselves are C++ and are tested in C++: `tools/import/tests/test_obj.cpp` is
//! where "an OBJ produces a mesh" is asserted, and `test_pipeline.cpp` is where "one cache serves
//! three formats" is. Repeating either here would be slower and would prove less.
//!
//! What is being asserted here is everything the EDITOR owns, which is the half that was missing
//! before this milestone: that import is a registered command and therefore an agent tool, that it
//! refuses what it cannot do with a reason a caller can act on, that what it produced becomes an
//! entity through a transaction, that undo removes that entity, and that the steps a format cannot
//! express arrive as a statement rather than as a warning.
//!
//! [`Recording`] stands in for the importer for the reason `the_authoring_loop`'s `Fake` stands in
//! for the Swift toolchain: the seam is a declared trait, so a test needs no built engine, and what
//! crosses it is asserted rather than assumed. The real `cy_import_cli` is not run here and
//! deliberately: the editor's suite must not need the engine's C++ tools built, and the bytes that
//! cross the boundary are pinned instead by `assets.rs`' own unit test, which parses a report a real
//! run produced.

use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};

use cy_editor_commands::registry::{Arguments, Registry};
use cy_editor_commands::scope::{DocumentScope, Scope};
use cy_editor_commands::{AssetImportOutcome, AssetImportRequest, EffectClass, ImportedSubAsset};
use cy_editor_core::Actor;
use cy_editor_core::ids::NodeId;
use cy_editor_core::value::{Value, ValueKind};
use cy_editor_documents::Document;
use cy_editor_services::assets::{AssetImportService, ImportRunner};
use cy_editor_services::builtin;
use cy_editor_services::editor::Editor;
use cy_editor_services::primitives::{MeshBinding, mesh_of};
use cy_editor_services::project::ProjectService;
use cy_editor_viewport::gizmo::TransformBinding;

/// An importer that answers from a script and records what it was asked.
///
/// The answers are the shape `cy_import_cli --json` really produces — `assets.rs`' own unit test
/// parses the exact bytes of a real run, so the two halves cannot drift without one of them going
/// red.
struct Recording {
    calls: Mutex<Vec<AssetImportRequest>>,
    extensions: Vec<String>,
    /// When set, every import is refused with this reason.
    refuse: Option<String>,
}

impl Recording {
    fn new() -> Arc<Self> {
        Arc::new(Self {
            calls: Mutex::new(Vec::new()),
            extensions: vec![
                ".gltf".to_string(),
                ".glb".to_string(),
                ".fbx".to_string(),
                ".obj".to_string(),
                ".tga".to_string(),
            ],
            refuse: None,
        })
    }

    fn refusing(reason: &str) -> Arc<Self> {
        Arc::new(Self {
            calls: Mutex::new(Vec::new()),
            extensions: vec![".obj".to_string()],
            refuse: Some(reason.to_string()),
        })
    }

    fn calls(&self) -> Vec<AssetImportRequest> {
        self.calls
            .lock()
            .expect("the lock is never poisoned")
            .clone()
    }
}

impl ImportRunner for Recording {
    fn describe(&self) -> String {
        "a recording double".to_string()
    }

    fn extensions(&self) -> Vec<String> {
        self.extensions.clone()
    }

    fn run(
        &self,
        _root: &Path,
        request: &AssetImportRequest,
    ) -> cy_editor_core::problem::Result<AssetImportOutcome> {
        self.calls
            .lock()
            .expect("the lock is never poisoned")
            .push(request.clone());
        if let Some(reason) = &self.refuse {
            return Err(cy_editor_core::problem::Problem::new(
                format!("import {}", request.source),
                reason.clone(),
            ));
        }
        Ok(AssetImportOutcome {
            source: request.source.clone(),
            importer: "obj".to_string(),
            id: "5e014c7f8f1666b05d2b42948d633399".to_string(),
            cache: "miss".to_string(),
            sub_assets: vec![
                ImportedSubAsset {
                    name: "mesh/Seat".to_string(),
                    id: "5e014c7f8f1666b05d2b42948d633399".to_string(),
                },
                ImportedSubAsset {
                    name: "material/Oak".to_string(),
                    id: "d658eeb594dfd3c1b877fef741678723".to_string(),
                },
            ],
            warnings: 0,
            errors: 0,
            steps_not_reached: "7 (import skeletons), 8 (import animations), 10 (produce a prefab \
                                of the hierarchy)"
                .to_string(),
        })
    }
}

/// A directory that removes itself, declared as a project so the editor will act inside it.
struct Sandbox(PathBuf);

impl Sandbox {
    fn new(name: &str) -> Self {
        let unique = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map_or(0, |elapsed| elapsed.as_nanos());
        let path =
            std::env::temp_dir().join(format!("cy-import-{name}-{}-{unique}", std::process::id()));
        std::fs::create_dir_all(&path).expect("a writable temporary directory");
        std::fs::write(path.join("project.json"), "{}").expect("a project manifest");
        Self(path)
    }

    fn path(&self) -> &Path {
        &self.0
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

/// An editor over a declared project with an open world, importing through `runner`.
fn editor_with_a_world(sandbox: &Sandbox, runner: Arc<dyn ImportRunner>) -> Editor {
    let mut editor = Editor::new(Actor::human("designer"))
        .with_project(ProjectService::new(sandbox.path()))
        .with_importer(AssetImportService::new(sandbox.path()).with_runner(runner));
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

fn invoke(
    editor: &mut Editor,
    registry: &Registry,
    id: &str,
    arguments: &Arguments,
) -> cy_editor_commands::Outcome {
    editor
        .invoke(registry, id, &Scope::unrestricted(), arguments)
        .unwrap_or_else(|problem| panic!("{id} was refused: {problem:?}"))
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

fn nodes(editor: &Editor) -> Vec<NodeId> {
    document(editor).content().nodes().collect()
}

fn a_chair() -> Arguments {
    Arguments::new().with("path", Value::Text("models/chair.obj".to_string()))
}

// --- Task 3.1: import is a command, and therefore an agent tool ----------------------------------

#[test]
fn asset_import_is_in_the_registry_with_everything_a_machine_caller_needs() {
    // The whole of task 3.1 in one assertion. `editor-agent-interface` projects the registry rather
    // than maintaining a list, so a command with this metadata IS an agent tool the same day —
    // there is no second place to add it and no way to forget.
    let registry = registry();
    let metadata = registry
        .metadata("asset.import")
        .expect("asset.import is registered");

    assert_eq!(metadata.category, "Asset");
    assert_eq!(metadata.effect, EffectClass::ReversibleMutation);
    assert!(
        metadata.description.len() > 120,
        "a caller that cannot see the interface needs more than a label: {:?}",
        metadata.description
    );

    let path = metadata
        .parameters
        .iter()
        .find(|parameter| parameter.name == "path")
        .expect("a path parameter");
    assert_eq!(path.kind, ValueKind::Text);
    assert!(path.required);
    assert!(!path.description.is_empty());

    for optional in ["at", "parent", "options", "place", "force"] {
        assert!(
            metadata
                .parameters
                .iter()
                .any(|parameter| parameter.name == optional),
            "asset.import declares {optional}"
        );
    }
}

// --- Task 3.5: what was imported lands in the world, through a transaction -----------------------

#[test]
fn an_import_lands_a_mesh_in_the_open_world_as_one_entity() {
    let sandbox = Sandbox::new("lands");
    let registry = registry();
    let runner = Recording::new();
    let mut editor = editor_with_a_world(&sandbox, runner.clone());

    assert!(nodes(&editor).is_empty(), "the world starts empty");
    let outcome = invoke(
        &mut editor,
        &registry,
        "asset.import",
        &a_chair().with("at", Value::Vec3([1.0, 2.0, 3.0])),
    );

    let created = nodes(&editor);
    assert_eq!(created.len(), 1, "one entity: {created:?}");
    let node = created[0];

    // It is a mesh instance: a transform where the gizmo binds, and a reference to the file that
    // was imported.
    let document = document(&editor);
    let transform = TransformBinding::of_schema(document.schema()).expect("a Transform");
    assert_eq!(
        document
            .content()
            .field(node, transform.component, transform.translation),
        Some(&Value::Vec3([1.0, 2.0, 3.0]))
    );
    assert_eq!(
        mesh_of(document, node).as_deref(),
        Some("models/chair.obj"),
        "the entity draws the file that was imported"
    );

    // The structured result is what an agent acts on: it names the entity, the identity the source
    // holds, and what the cache did.
    assert_eq!(
        outcome.values.get("entity"),
        Some(&Value::Text(node.to_string()))
    );
    assert_eq!(
        outcome.values.get("id"),
        Some(&Value::Text("5e014c7f8f1666b05d2b42948d633399".to_string()))
    );
    assert_eq!(
        outcome.values.get("cache"),
        Some(&Value::Text("miss".to_string()))
    );
    assert_eq!(outcome.values.get("sub-assets"), Some(&Value::Int(2)));

    // One import, with the path the caller gave.
    let calls = runner.calls();
    assert_eq!(calls.len(), 1);
    assert_eq!(calls[0].source, "models/chair.obj");
    assert!(!calls[0].force);
}

#[test]
fn undo_removes_the_entity_and_does_not_re_run_the_importer() {
    // "A created primitive, an imported mesh and an added body are each one transaction, and undo
    // returns the world to empty." The cooked asset is NOT undone, and deliberately: it belongs to
    // the file rather than to the world, exactly as a `.gltf` a person copied in does.
    let sandbox = Sandbox::new("undo");
    let registry = registry();
    let runner = Recording::new();
    let mut editor = editor_with_a_world(&sandbox, runner.clone());

    invoke(&mut editor, &registry, "asset.import", &a_chair());
    assert_eq!(nodes(&editor).len(), 1);

    invoke(&mut editor, &registry, "edit.undo", &Arguments::new());
    assert!(
        nodes(&editor).is_empty(),
        "undo returns the world to empty: {:?}",
        nodes(&editor)
    );
    assert_eq!(runner.calls().len(), 1, "undo does not cook anything");

    invoke(&mut editor, &registry, "edit.redo", &Arguments::new());
    assert_eq!(nodes(&editor).len(), 1, "redo puts it back");
    assert_eq!(runner.calls().len(), 1, "and redo does not cook either");
}

#[test]
fn an_imported_entity_is_built_by_the_same_function_a_primitive_is() {
    // Task 2.3 from the import side, through the COMMANDS rather than through the constructor —
    // `primitives_are_ordinary_mesh_instances` asserts it at the constructor, and this asserts that
    // the command really reaches it. Two entities, one made each way, with the same component set.
    let sandbox = Sandbox::new("indistinguishable");
    let registry = registry();
    let mut editor = editor_with_a_world(&sandbox, Recording::new());

    invoke(
        &mut editor,
        &registry,
        "scene.create-primitive",
        &Arguments::new().with("shape", Value::Text("box".to_string())),
    );
    invoke(&mut editor, &registry, "asset.import", &a_chair());

    let created = nodes(&editor);
    assert_eq!(created.len(), 2);
    let document = document(&editor);
    let first = document.content().node(created[0]).expect("the first");
    let second = document.content().node(created[1]).expect("the second");
    assert_eq!(
        first.components.keys().collect::<Vec<_>>(),
        second.components.keys().collect::<Vec<_>>(),
        "a generated primitive and an imported mesh carry the same components"
    );
    let mesh = MeshBinding::of_schema(document.schema()).expect("a MeshRenderer");
    assert!(first.components.contains_key(&mesh.component));
    assert!(second.components.contains_key(&mesh.component));
}

#[test]
fn placing_can_be_declined_and_then_the_world_is_untouched() {
    // A batch import cooks and does not populate. It is still one command rather than two, because
    // two would be two implementations of the same action.
    let sandbox = Sandbox::new("no_place");
    let registry = registry();
    let runner = Recording::new();
    let mut editor = editor_with_a_world(&sandbox, runner.clone());

    let outcome = invoke(
        &mut editor,
        &registry,
        "asset.import",
        &a_chair().with("place", Value::Bool(false)),
    );
    assert_eq!(runner.calls().len(), 1, "it still cooked");
    assert!(nodes(&editor).is_empty(), "and left the world alone");
    assert!(!outcome.values.contains_key("entity"));
}

// --- Task 3.3: the absent steps are named, and are not warnings ----------------------------------

#[test]
fn the_steps_a_format_cannot_express_are_stated_and_are_not_warnings() {
    let sandbox = Sandbox::new("steps");
    let registry = registry();
    let mut editor = editor_with_a_world(&sandbox, Recording::new());

    let outcome = invoke(&mut editor, &registry, "asset.import", &a_chair());

    // Named, with their numbers, so a caller can look each one up in the specification's sequence.
    let named = match outcome.values.get("steps-not-reached") {
        Some(Value::Text(text)) => text.clone(),
        other => panic!("the outcome names the absent steps: {other:?}"),
    };
    assert!(named.contains("7 (import skeletons)"), "{named}");
    assert!(named.contains("10 (produce a prefab"), "{named}");

    // And NOT as warnings. `asset-import-pipeline`: "A step skipped for that reason is not a
    // warning about the file and SHALL NOT be reported as one."
    assert_eq!(outcome.values.get("warnings"), Some(&Value::Int(0)));
    let said = outcome.summary.to_lowercase();
    assert!(said.contains("does not carry"), "{}", outcome.summary);
    assert!(!said.contains("warning"), "{}", outcome.summary);
    assert!(!said.contains("missing"), "{}", outcome.summary);
    assert!(!said.contains("skipped"), "{}", outcome.summary);
}

// --- Refusals a caller can act on ------------------------------------------------------------------

#[test]
fn a_format_no_importer_claims_is_refused_naming_what_this_build_imports() {
    let sandbox = Sandbox::new("unclaimed");
    let registry = registry();
    let runner = Recording::new();
    let mut editor = editor_with_a_world(&sandbox, runner.clone());

    let refused = refusal(
        &mut editor,
        &registry,
        "asset.import",
        &Arguments::new().with("path", Value::Text("models/chair.blend".to_string())),
    );
    assert!(refused.contains("no importer"), "{refused}");
    // The remedy lists what this build CAN import, read from the importer itself — so a project's
    // own importer appears there the day it is registered.
    assert!(refused.contains(".obj"), "{refused}");
    assert!(refused.contains(".fbx"), "{refused}");
    assert!(
        runner.calls().is_empty(),
        "a refusal costs no process launch"
    );
}

#[test]
fn an_import_that_failed_leaves_no_entity_behind() {
    // The order matters and is deliberate: the cook happens first and the entity second, so an
    // entity referencing an asset that never cooked cannot exist.
    let sandbox = Sandbox::new("failed");
    let registry = registry();
    let mut editor = editor_with_a_world(&sandbox, Recording::refusing("the file is not a mesh"));

    let refused = refusal(&mut editor, &registry, "asset.import", &a_chair());
    assert!(refused.contains("not a mesh"), "{refused}");
    assert!(nodes(&editor).is_empty(), "and no entity was created");
    assert!(
        !document(&editor).is_transaction_open(),
        "and no transaction was left open"
    );
}

#[test]
fn an_option_that_is_not_name_equals_value_is_refused_rather_than_dropped() {
    let sandbox = Sandbox::new("options");
    let registry = registry();
    let runner = Recording::new();
    let mut editor = editor_with_a_world(&sandbox, runner.clone());

    let refused = refusal(
        &mut editor,
        &registry,
        "asset.import",
        &a_chair().with("options", Value::Text("lod-count".to_string())),
    );
    assert!(refused.contains("lod-count"), "{refused}");
    assert!(runner.calls().is_empty());

    // And a well-formed one reaches the importer exactly as it was written.
    invoke(
        &mut editor,
        &registry,
        "asset.import",
        &a_chair().with(
            "options",
            Value::Text("lod-count=3,collision-mode=convex".to_string()),
        ),
    );
    let calls = runner.calls();
    assert_eq!(calls.len(), 1);
    assert_eq!(
        calls[0].options.get("lod-count").map(String::as_str),
        Some("3")
    );
    assert_eq!(
        calls[0].options.get("collision-mode").map(String::as_str),
        Some("convex")
    );
}

#[test]
fn a_path_outside_the_connections_scope_is_refused_with_the_scope_as_the_reason() {
    // `editor-agent-interface`: "Operations outside that scope SHALL be refused with the scope as
    // the reason." An import writes cooked bytes and two sidecars into the project, so it is a
    // mutation whatever else it is and the same rule applies to it as to a source write.
    let sandbox = Sandbox::new("scope");
    let registry = registry();
    let runner = Recording::new();
    let mut editor = editor_with_a_world(&sandbox, runner.clone());

    let scope =
        Scope::new("models only", DocumentScope::All, EffectClass::ALL).with_directory("models/");
    let refused = format!(
        "{:?}",
        editor
            .invoke(
                &registry,
                "asset.import",
                &scope,
                &Arguments::new().with("path", Value::Text("vendor/chair.obj".to_string())),
            )
            .expect_err("outside the scope")
    );
    assert!(refused.contains("models only"), "{refused}");
    assert!(runner.calls().is_empty());

    // And inside it the same command works.
    editor
        .invoke(
            &registry,
            "asset.import",
            &scope,
            &Arguments::new().with("path", Value::Text("models/chair.obj".to_string())),
        )
        .expect("inside the scope");
    assert_eq!(runner.calls().len(), 1);
}
