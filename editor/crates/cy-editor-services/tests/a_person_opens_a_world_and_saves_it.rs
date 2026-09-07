//! Open a world, select an object, drag a gizmo, undo, save — and open it again. M6 task 2.8.
//!
//! --- WHY THIS FILE AND NOT ONLY THE WINDOW ------------------------------------------------------
//!
//! `samples/05b-editor-window` drives the same sequence through synthesised X11 input, and that is
//! the artefact. This is the same sequence through the same services with no window, for the reason
//! `editor-rust-application` gives — "Services, models, view models, and commands SHALL be testable
//! **headlessly**" — and for one more: a continuous-integration machine has no display, and a
//! sequence that can only be checked on a developer's desk is a sequence nothing checks.
//!
//! Every step below goes through the path a person's keystroke goes through. The document is opened
//! by `Editor::open_document`, the entity is created by the registered `scene.create-entity`, the
//! drag is a real `Drag` against a real ray, the undo is `edit.undo`, and the save is `file.save`.
//! Nothing here constructs a document by hand — which is what M5.5's suites had to do, and exactly
//! why the gap this closes went unnoticed for a milestone: every test declared the `Transform` the
//! product could not.
//!
//! --- WHAT MAKES THE SCHEMA THE ENGINE'S ---------------------------------------------------------
//!
//! The project below carries `types.cytypes`, and the copy it carries is the sample project's —
//! which `cy_test_unit_scene_serialization` regenerates from `cy::reflect::TypeRegistry` and
//! compares byte for byte. So if the engine's components change and this file still passes, the C++
//! suite fails; and if somebody edits the manifest by hand to make this pass, the same suite fails.

#![allow(
    clippy::float_cmp,
    reason = "these tests assert a manipulation produced EXACTLY the value it was asked for, and \
              that undo restored EXACTLY the value it replaced. An epsilon would hide the defect \
              they exist to catch."
)]
#![allow(
    clippy::cast_precision_loss,
    reason = "a viewport is a few thousand pixels across, far below the 2^24 an f32 represents \
              exactly"
)]

use std::path::{Path, PathBuf};

use cy_editor_commands::registry::{Arguments, Registry};
use cy_editor_commands::scope::Scope;
use cy_editor_core::Actor;
use cy_editor_core::ids::NodeId;
use cy_editor_core::value::Value;
use cy_editor_documents::selection::Selection;
use cy_editor_services::editor::Editor;
use cy_editor_services::project::ProjectService;
use cy_editor_services::{builtin, worldfile};
use cy_editor_viewport::gizmo::{
    Drag, DragInput, DragRequest, GizmoRegistry, GizmoSpace, Handle, Pivot, TransformBinding,
};
use cy_editor_viewport::snapping::{SnapModes, SnapSettings};

const WORLD: &str = "worlds/city.cyworld";

/// The manifest the engine writes, as the sample project carries it.
///
/// Included from the sample rather than copied into this file: a copy would be a second thing to
/// keep in step with the generator, and the whole point of the manifest is that there is one.
const MANIFEST: &str = include_str!("../../../../samples/05b-editor-window/project/types.cytypes");

struct Sandbox(PathBuf);

impl Sandbox {
    fn new(name: &str) -> Self {
        let unique = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map_or(0, |elapsed| elapsed.as_nanos());
        let path =
            std::env::temp_dir().join(format!("cy-world-{name}-{}-{unique}", std::process::id()));
        std::fs::create_dir_all(path.join("worlds")).expect("a writable temporary project");
        // A project is a directory that says it is one — `ProjectService::is_declared`. Writing the
        // manifest here rather than assuming it is what makes these tests the same configuration
        // the artefacts run in.
        std::fs::write(
            path.join(cy_editor_services::ProjectService::MANIFEST),
            "{\n  \"name\": \"Sandbox\",\n  \"engine\": \"CyberEngine\",\n  \"worlds\": \"worlds\"\n}\n",
        )
        .expect("the project manifest");
        std::fs::write(path.join(worldfile::TYPE_MANIFEST), MANIFEST).expect("the type manifest");
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

/// An editor opened on `sandbox`, the way the application opens one.
fn editor_on(sandbox: &Sandbox) -> Editor {
    Editor::new(Actor::human("designer")).with_project(ProjectService::new(sandbox.path()))
}

fn translation_of(editor: &Editor, node: NodeId, binding: TransformBinding) -> [f32; 3] {
    let document = editor
        .documents
        .get(editor.workspace.active().expect("a document is active"))
        .expect("the document is open");
    match document
        .content()
        .field(node, binding.component, binding.translation)
    {
        Some(Value::Vec3(lanes)) => *lanes,
        other => panic!("the entity has no translation: {other:?}"),
    }
}

/// Drag the X handle by `metres`, exactly as a pointer would.
fn drag_x(editor: &mut Editor, node: NodeId, binding: TransformBinding, metres: f32) {
    let view = editor.viewports.focused().interaction_view().clone();
    let no_snapping = SnapSettings {
        modes: SnapModes {
            grid: false,
            angle: false,
            scale: false,
            vertex: false,
            surface: false,
        },
        ..SnapSettings::default()
    };
    let gizmos = GizmoRegistry::with_builtins();
    let id = editor.workspace.active().expect("a document is active");
    let document = editor.documents.get_mut(id).expect("the document is open");
    let mut drag = Drag::begin(
        &gizmos,
        document,
        &DragRequest {
            manipulator: "translate",
            handle: Handle::AxisX,
            space: GizmoSpace::World,
            pivot: Pivot::Pivot,
            nodes: &[node],
            binding,
            view: &view,
            pixel: (
                view.viewport.width as f32 / 2.0,
                view.viewport.height as f32 / 2.0,
            ),
            actor: Actor::human("designer"),
            duplicate: false,
            bounds: None,
        },
    )
    .expect("the drag begins");
    drag.advance_by(&gizmos, document, metres, &no_snapping, DragInput::NONE)
        .expect("the drag advances");
    drag.commit(document).expect("the drag commits");
}

#[test]
fn opening_a_world_declares_the_engines_components_and_a_created_entity_has_a_transform() {
    // M5.5's recorded defect, inverted into an assertion. "`DocumentService::open` calls
    // `Document::new` — a name and an EMPTY SCHEMA — because there is no world loader."
    let sandbox = Sandbox::new("schema");
    let mut editor = editor_on(&sandbox);
    let id = editor.open_document(WORLD).expect("the world opens");
    let document = editor.documents.get(id).expect("the document is open");

    let binding = TransformBinding::of_schema(document.schema())
        .expect("the schema declares a Transform the gizmo can bind to");
    assert_eq!(
        document.schema().type_of(binding.component).unwrap().name,
        "Transform"
    );

    // And an entity created through the ordinary command has one, which is what makes it placeable.
    let registry = registry();
    editor
        .invoke(
            &registry,
            "scene.create-entity",
            &Scope::unrestricted(),
            &Arguments::new(),
        )
        .expect("the entity is created");
    let node = editor
        .selection
        .get()
        .nodes()
        .next()
        .expect("creating an entity selects it");
    assert_eq!(translation_of(&editor, node, binding), [0.0, 0.0, 0.0]);
}

#[test]
fn a_person_opens_a_world_drags_a_gizmo_undoes_and_saves() {
    let sandbox = Sandbox::new("session");
    let registry = registry();
    let mut editor = editor_on(&sandbox);
    editor.open_document(WORLD).expect("the world opens");
    editor.viewports.all_mut().iter_mut().for_each(|viewport| {
        viewport.snap.modes = SnapModes {
            grid: false,
            angle: false,
            scale: false,
            vertex: false,
            surface: false,
        };
    });

    // --- creates something, and selects it -----------------------------------------------------
    editor
        .invoke(
            &registry,
            "scene.create-entity",
            &Scope::unrestricted(),
            &Arguments::new(),
        )
        .expect("the entity is created");
    let node = editor
        .selection
        .get()
        .nodes()
        .next()
        .expect("creating an entity selects it");
    let id = editor.workspace.active().expect("a document is active");
    let binding = TransformBinding::of_schema(editor.documents.get(id).unwrap().schema())
        .expect("a Transform");
    let mut selection = Selection::new();
    selection.add_node(node);
    editor.selection.set(selection);

    // --- drags a gizmo -------------------------------------------------------------------------
    let before = editor.documents.get(id).unwrap().history().entries().len();
    drag_x(&mut editor, node, binding, 1.5);
    assert_eq!(translation_of(&editor, node, binding), [1.5, 0.0, 0.0]);
    assert_eq!(
        editor.documents.get(id).unwrap().history().entries().len(),
        before + 1,
        "one manipulation is one transaction"
    );

    // --- undoes --------------------------------------------------------------------------------
    editor
        .invoke(
            &registry,
            "edit.undo",
            &Scope::unrestricted(),
            &Arguments::new(),
        )
        .expect("undo");
    assert_eq!(
        translation_of(&editor, node, binding),
        [0.0, 0.0, 0.0],
        "undo restores the exact value the drag replaced"
    );
    editor
        .invoke(
            &registry,
            "edit.redo",
            &Scope::unrestricted(),
            &Arguments::new(),
        )
        .expect("redo");
    assert_eq!(translation_of(&editor, node, binding), [1.5, 0.0, 0.0]);

    // --- and saves -----------------------------------------------------------------------------
    save_and_reopen(&sandbox, &mut editor, &registry, id);
}

/// The last two steps, in a function of their own so that neither this nor the test above is longer
/// than a person reads in one go. The split is where the session ends and the FILE begins.
fn save_and_reopen(
    sandbox: &Sandbox,
    editor: &mut Editor,
    registry: &Registry,
    id: cy_editor_core::ids::DocumentId,
) {
    assert!(editor.documents.get(id).unwrap().is_dirty());
    editor
        .invoke(
            registry,
            "file.save",
            &Scope::unrestricted(),
            &Arguments::new(),
        )
        .expect("the save");
    assert!(
        !editor.documents.get(id).unwrap().is_dirty(),
        "a saved document is clean"
    );

    let written = sandbox.path().join(WORLD);
    assert!(written.is_file(), "file.save wrote {}", written.display());
    let text = std::fs::read_to_string(&written).expect("the world reads back");
    assert!(text.starts_with("cyworld 1\n"), "{text:.40}");
    assert!(
        text.contains("runtime \"Transform\""),
        "the world carries the schema it was written against"
    );

    // --- a second editor opens what the first wrote ----------------------------------------------
    let mut second = editor_on(sandbox);
    let reopened = second.open_document(WORLD).expect("the world opens again");
    let document = second.documents.get(reopened).expect("the document");
    assert_eq!(document.content().node_count(), 1);
    let binding = TransformBinding::of_schema(document.schema()).expect("a Transform");
    let node = document.content().nodes().next().expect("the entity");
    assert_eq!(
        document
            .content()
            .field(node, binding.component, binding.translation),
        Some(&Value::Vec3([1.5, 0.0, 0.0])),
        "what was dragged is where it was left"
    );
    assert!(
        !document.is_dirty(),
        "and opening it is not an unsaved change"
    );
    assert!(
        document.history().peek_undo().is_none(),
        "nor something undo may take away"
    );
}

#[test]
fn an_editor_started_somewhere_that_is_not_a_project_writes_nothing_into_it() {
    // THE DEFECT THIS STANDS FOR HAPPENED. `ProjectService::default()` roots at the working
    // directory, and the first run of the M5 session sample after `file.save` learned to write a
    // world left a `worlds/` directory in the REPOSITORY — because the sample runs the editor with
    // the source tree as its working directory. A directory is a project when it says so.
    let sandbox = Sandbox::new("undeclared");
    std::fs::remove_file(
        sandbox
            .path()
            .join(cy_editor_services::ProjectService::MANIFEST),
    )
    .expect("take the manifest away, so the directory is not a project");
    let previous = std::env::current_dir().expect("a working directory");
    std::env::set_current_dir(sandbox.path()).expect("move into the sandbox");
    let outcome = std::panic::catch_unwind(|| {
        let mut editor = Editor::new(Actor::human("designer"));
        let registry = registry();
        editor.open_document(WORLD).expect("it opens");
        editor
            .invoke(
                &registry,
                "scene.create-entity",
                &Scope::unrestricted(),
                &Arguments::new(),
            )
            .expect("an entity");
        editor
            .invoke(
                &registry,
                "file.save",
                &Scope::unrestricted(),
                &Arguments::new(),
            )
            .expect("the save reports success");
    });
    std::env::set_current_dir(previous).expect("go back");
    outcome.expect("the session ran");

    assert!(
        !sandbox.path().join(WORLD).exists(),
        "an undeclared working directory is not somewhere to save a world"
    );
}

#[test]
fn a_project_with_no_type_manifest_still_opens_and_says_nothing_it_cannot_support() {
    // A project that has never been cooked has no `types.cytypes`. Opening a world in it is an
    // ordinary thing to do: it opens, the schema is empty, and the gizmo has nothing to bind to —
    // which it says by being absent rather than by the editor refusing to start.
    let sandbox = Sandbox::new("bare");
    std::fs::remove_file(sandbox.path().join(worldfile::TYPE_MANIFEST)).expect("remove it");
    let mut editor = editor_on(&sandbox);
    let id = editor.open_document(WORLD).expect("it opens anyway");
    let document = editor.documents.get(id).expect("the document");
    assert_eq!(document.schema().types().count(), 0);
    assert!(TransformBinding::of_schema(document.schema()).is_none());
}
