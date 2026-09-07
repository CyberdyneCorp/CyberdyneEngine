//! The second half of the loop, as tests: manipulate, write a script, build, reload, play.
//! M5.5 tasks 3.5 through 3.8.
//!
//! Integration tests rather than unit tests because every claim here is about a **join**. "An
//! agent's move is a human's move" is a statement about the registry, the viewport's settings, the
//! gizmo's manipulators and the document's history all being the same ones; a test that stubbed any
//! of them would prove nothing about the sentence it is named after.

#![allow(
    clippy::float_cmp,
    reason = "these tests assert that a manipulation produced EXACTLY the value it was asked for.               An epsilon would hide the defect they exist to catch — a drag that snapped, or a               value that went through a text round trip — which is the same argument               `Drag::commit` makes for comparing transforms as bits."
)]
#![allow(
    clippy::cast_precision_loss,
    reason = "a viewport is a few thousand pixels across, far below the 2^24 an f32 represents               exactly"
)]

use std::path::{Path, PathBuf};
use std::sync::Arc;

use cy_editor_commands::metadata::EffectClass;
use cy_editor_commands::registry::{Arguments, Registry};
use cy_editor_commands::scope::{DocumentScope, Scope};
use cy_editor_core::Actor;
use cy_editor_core::ids::NodeId;
use cy_editor_core::value::{Value, ValueKind};
use cy_editor_documents::Document;
use cy_editor_documents::selection::Selection;
use cy_editor_services::builtin;
use cy_editor_services::editor::Editor;
use cy_editor_services::project::{BuildRequest, ModuleBuilder, ProjectService};
use cy_editor_viewport::gizmo::{
    Drag, DragInput, DragRequest, GizmoSpace, Pivot, TransformBinding,
};
use cy_editor_viewport::math::Quat;
use cy_editor_viewport::snapping::{SnapModes, SnapSettings};

/// A directory that removes itself.
struct Sandbox(PathBuf);

impl Sandbox {
    fn new(name: &str) -> Self {
        let unique = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map_or(0, |elapsed| elapsed.as_nanos());
        let path = std::env::temp_dir().join(format!(
            "cy-authoring-{name}-{}-{unique}",
            std::process::id()
        ));
        std::fs::create_dir_all(path.join("game")).expect("a writable temporary directory");
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

/// A builder that writes a file rather than running a compiler, so this suite needs no toolchain.
struct Fake;

impl ModuleBuilder for Fake {
    fn describe(&self) -> String {
        "a test double that writes an empty library".to_string()
    }

    fn build(&self, request: &BuildRequest) -> cy_editor_core::problem::Result<PathBuf> {
        let library = request
            .work
            .join(format!("libCyGame_g{}.so", request.generation));
        std::fs::create_dir_all(&request.work).expect("a writable work directory");
        std::fs::write(&library, b"").expect("a writable library");
        Ok(library)
    }
}

fn registry() -> Registry {
    let mut registry = Registry::new();
    builtin::register(&mut registry).expect("the built-in commands satisfy their own metadata");
    registry
}

/// A document declaring the transform the gizmos edit, with one lamp two metres along X.
fn with_a_lamp(editor: &mut Editor) -> (NodeId, TransformBinding) {
    let mut document = Document::new("worlds/city.cyworld");
    let component = document.schema_mut().declare_type("Transform", false);
    let translation = document
        .schema_mut()
        .declare_field(component, "translation", ValueKind::Vec3, "where it is")
        .expect("a fresh schema");
    let rotation = document
        .schema_mut()
        .declare_field(component, "rotation", ValueKind::Quat, "which way it faces")
        .expect("a fresh schema");
    let scale = document
        .schema_mut()
        .declare_field(component, "scale", ValueKind::Vec3, "how big it is")
        .expect("a fresh schema");
    let binding = TransformBinding {
        component,
        translation,
        rotation,
        scale,
    };
    let lamp = document
        .with_transaction("Place a lamp", Actor::human("designer"), |document| {
            let lamp = document.create_node(None)?;
            document.add_component(
                lamp,
                component,
                vec![
                    (translation, Value::Vec3([2.0, 0.0, 0.0])),
                    (rotation, Value::Quat(Quat::IDENTITY.to_array())),
                    (scale, Value::Vec3([1.0, 1.0, 1.0])),
                ],
            )?;
            Ok(lamp)
        })
        .expect("the lamp is placed");
    let id = editor.documents.insert(document);
    editor.workspace.opened(id);
    let mut selection = Selection::new();
    selection.add_node(lamp);
    editor.selection.set(selection);
    (lamp, binding)
}

fn translation_of(editor: &Editor, node: NodeId, binding: TransformBinding) -> [f32; 3] {
    match editor
        .documents
        .get(editor.workspace.active().expect("a document is active"))
        .and_then(|document| document.content().node(node))
        .and_then(|state| state.components.get(&binding.component))
        .and_then(|fields| {
            fields
                .iter()
                .find(|(field, _)| **field == binding.translation)
        }) {
        Some((_, Value::Vec3(lanes))) => *lanes,
        other => panic!("the lamp has no translation: {other:?}"),
    }
}

fn scale_of(editor: &Editor, node: NodeId, binding: TransformBinding) -> [f32; 3] {
    match editor
        .documents
        .get(editor.workspace.active().expect("a document is active"))
        .and_then(|document| document.content().node(node))
        .and_then(|state| state.components.get(&binding.component))
        .and_then(|fields| fields.iter().find(|(field, _)| **field == binding.scale))
    {
        Some((_, Value::Vec3(lanes))) => *lanes,
        other => panic!("the lamp has no scale: {other:?}"),
    }
}

fn rotation_of(editor: &Editor, node: NodeId, binding: TransformBinding) -> [f32; 4] {
    match editor
        .documents
        .get(editor.workspace.active().expect("a document is active"))
        .and_then(|document| document.content().node(node))
        .and_then(|state| state.components.get(&binding.component))
        .and_then(|fields| fields.iter().find(|(field, _)| **field == binding.rotation))
    {
        Some((_, Value::Quat(lanes))) => *lanes,
        other => panic!("the lamp has no rotation: {other:?}"),
    }
}

#[test]
fn a_stated_move_and_a_gizmo_drag_reach_the_same_transform() {
    // "WHEN an agent translates an object and a human translates it identically with the gizmo THEN
    // the resulting transform SHALL be identical, and each SHALL produce one undoable transaction."
    //
    // The human's half is driven here with a real `Drag` against a real ray; the agent's half is the
    // registered command. Snapping is off on both sides, because `SnapSettings::default()` has the
    // grid ON and a test that left it on would be measuring the grid.
    let registry = registry();
    let mut editor = Editor::new(Actor::human("designer"));
    let (lamp, binding) = with_a_lamp(&mut editor);
    editor.viewports.all_mut().iter_mut().for_each(|viewport| {
        viewport.snap.modes = SnapModes {
            grid: false,
            angle: false,
            scale: false,
            vertex: false,
            surface: false,
        };
    });

    editor
        .invoke(
            &registry,
            "scene.translate",
            &Scope::unrestricted(),
            &Arguments::new().with("amount", Value::Vec3([1.5, 0.0, 0.0])),
        )
        .expect("the move is performed");
    let after_command = translation_of(&editor, lamp, binding);
    assert_eq!(after_command, [3.5, 0.0, 0.0]);

    // Now the same move, by hand, on a second copy of the same document.
    let mut by_hand = Editor::new(Actor::human("designer"));
    let (lamp_two, binding_two) = with_a_lamp(&mut by_hand);
    let view = by_hand.viewports.focused().interaction_view().clone();
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
    let document_id = by_hand.workspace.active().expect("a document is active");
    let gizmos = cy_editor_viewport::gizmo::GizmoRegistry::with_builtins();
    let document = by_hand
        .documents
        .get_mut(document_id)
        .expect("the document is open");
    let mut drag = Drag::begin(
        &gizmos,
        document,
        &DragRequest {
            manipulator: "translate",
            handle: cy_editor_viewport::gizmo::Handle::AxisX,
            space: GizmoSpace::World,
            pivot: Pivot::Pivot,
            nodes: &[lamp_two],
            binding: binding_two,
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
    drag.advance_by(&gizmos, document, 1.5, &no_snapping, DragInput::NONE)
        .expect("the drag advances");
    drag.commit(document).expect("the drag commits");

    assert_eq!(
        translation_of(&by_hand, lamp_two, binding_two),
        after_command,
        "the command and the drag are the same manipulation"
    );
}

#[test]
fn a_manipulation_across_three_axes_is_one_history_entry() {
    // "exactly one transaction per manipulation", however many axes it touches — otherwise one undo
    // leaves an object two thirds moved, which is the shape of defect nobody reports and everybody
    // works around.
    let registry = registry();
    let mut editor = Editor::new(Actor::human("designer"));
    let (lamp, binding) = with_a_lamp(&mut editor);
    let document_id = editor.workspace.active().expect("a document is active");
    let before = editor
        .documents
        .get(document_id)
        .expect("open")
        .history()
        .entries()
        .len();

    editor
        .invoke(
            &registry,
            "scene.translate",
            &Scope::unrestricted(),
            &Arguments::new().with("amount", Value::Vec3([1.0, 2.0, 3.0])),
        )
        .expect("the move is performed");

    let entries = editor
        .documents
        .get(document_id)
        .expect("open")
        .history()
        .entries()
        .len();
    assert_eq!(entries, before + 1, "three axes, one entry");

    editor
        .invoke(
            &registry,
            "edit.undo",
            &Scope::unrestricted(),
            &Arguments::new(),
        )
        .expect("undo");
    assert_eq!(
        translation_of(&editor, lamp, binding),
        [2.0, 0.0, 0.0],
        "one undo puts all three axes back"
    );
}

#[test]
fn an_absolute_manipulation_ends_at_the_value_it_was_given() {
    let registry = registry();
    let mut editor = Editor::new(Actor::human("designer"));
    let (lamp, binding) = with_a_lamp(&mut editor);

    editor
        .invoke(
            &registry,
            "scene.translate",
            &Scope::unrestricted(),
            &Arguments::new()
                .with("amount", Value::Vec3([9.0, -1.0, 0.5]))
                .with("absolute", Value::Bool(true)),
        )
        .expect("the move is performed");
    assert_eq!(translation_of(&editor, lamp, binding), [9.0, -1.0, 0.5]);

    editor
        .invoke(
            &registry,
            "scene.scale",
            &Scope::unrestricted(),
            &Arguments::new()
                .with("amount", Value::Vec3([2.0, 2.0, 2.0]))
                .with("absolute", Value::Bool(true)),
        )
        .expect("the scale is performed");
    assert_eq!(scale_of(&editor, lamp, binding), [2.0, 2.0, 2.0]);
}

#[test]
fn a_stated_rotation_turns_about_the_axis_it_named() {
    // A quarter turn about Y, stated in degrees because that is what a person types. The stored
    // value is a quaternion, and the one that matters is the Y lane: sin(45°).
    let registry = registry();
    let mut editor = Editor::new(Actor::human("designer"));
    let (lamp, binding) = with_a_lamp(&mut editor);
    editor.viewports.all_mut().iter_mut().for_each(|viewport| {
        viewport.snap.modes.angle = false;
        viewport.snap.modes.grid = false;
    });

    editor
        .invoke(
            &registry,
            "scene.rotate",
            &Scope::unrestricted(),
            &Arguments::new().with("amount", Value::Vec3([0.0, 90.0, 0.0])),
        )
        .expect("the rotation is performed");

    let quaternion = rotation_of(&editor, lamp, binding);
    let expected = std::f32::consts::FRAC_1_SQRT_2;
    assert!(
        (quaternion[1].abs() - expected).abs() < 1e-3,
        "a quarter turn about Y, got {quaternion:?}"
    );
    assert!(quaternion[0].abs() < 1e-3 && quaternion[2].abs() < 1e-3);
}

#[test]
fn writing_a_script_is_a_transaction_that_undoes() {
    // Task 3.6 and `design.md` §4. The file is written, the history has an entry naming the actor,
    // and undo puts the file back — which is what makes source authoring "no special path".
    let sandbox = Sandbox::new("undo");
    let registry = registry();
    let mut editor =
        Editor::new(Actor::human("designer")).with_project(ProjectService::new(sandbox.path()));

    editor
        .invoke(
            &registry,
            "source.write",
            &Scope::unrestricted(),
            &Arguments::new()
                .with("path", Value::Text("game/Player.swift".into()))
                .with("contents", Value::Text("struct Player {}".into())),
        )
        .expect("the file is written");
    assert_eq!(
        std::fs::read_to_string(sandbox.path().join("game/Player.swift")).expect("written"),
        "struct Player {}"
    );

    editor
        .invoke(
            &registry,
            "source.write",
            &Scope::unrestricted(),
            &Arguments::new()
                .with("path", Value::Text("game/Player.swift".into()))
                .with(
                    "contents",
                    Value::Text("struct Player { var hp = 3 }".into()),
                ),
        )
        .expect("the file is rewritten");

    editor
        .invoke(
            &registry,
            "edit.undo",
            &Scope::unrestricted(),
            &Arguments::new(),
        )
        .expect("undo");
    assert_eq!(
        std::fs::read_to_string(sandbox.path().join("game/Player.swift")).expect("still there"),
        "struct Player {}",
        "undo puts the previous contents back"
    );

    editor
        .invoke(
            &registry,
            "edit.undo",
            &Scope::unrestricted(),
            &Arguments::new(),
        )
        .expect("undo again");
    assert!(
        !sandbox.path().join("game/Player.swift").exists(),
        "undoing the creation of a file removes it"
    );

    editor
        .invoke(
            &registry,
            "edit.redo",
            &Scope::unrestricted(),
            &Arguments::new(),
        )
        .expect("redo");
    assert_eq!(
        std::fs::read_to_string(sandbox.path().join("game/Player.swift")).expect("back"),
        "struct Player {}"
    );
}

#[test]
fn a_source_write_carries_the_actor_and_the_intent_that_made_it() {
    let sandbox = Sandbox::new("actor");
    let registry = registry();
    let mut editor =
        Editor::new(Actor::human("designer")).with_project(ProjectService::new(sandbox.path()));
    editor.acting_as(Actor::agent(
        "claude",
        "s-9",
        "give the player a health value",
    ));

    editor
        .invoke(
            &registry,
            "source.write",
            &Scope::unrestricted(),
            &Arguments::new()
                .with("path", Value::Text("game/Player.swift".into()))
                .with("contents", Value::Text("struct Player {}".into())),
        )
        .expect("the file is written");

    let document = editor
        .documents
        .ids()
        .find_map(|id| editor.documents.get(id))
        .expect("the document standing for the file");
    let entry = document
        .history()
        .entries()
        .last()
        .expect("one history entry");
    assert!(entry.actor.is_agent());
    assert_eq!(entry.actor.intent(), Some("give the player a health value"));
    assert!(entry.name.contains("game/Player.swift"), "{}", entry.name);
}

#[test]
fn the_effect_class_of_a_source_write_is_computed_from_whether_it_can_be_reversed() {
    // `design.md` §4: reversible where the editor holds the prior contents, irreversible where it
    // cannot. The declared class is the worst case, and this is the computed one.
    let sandbox = Sandbox::new("effect");
    let registry = registry();
    let mut editor =
        Editor::new(Actor::human("designer")).with_project(ProjectService::new(sandbox.path()));

    let new_file = Arguments::new()
        .with("path", Value::Text("game/New.swift".into()))
        .with("contents", Value::Text("// new".into()));
    assert_eq!(
        registry
            .effect_of("source.write", &mut editor, &new_file)
            .expect("the class is computable"),
        EffectClass::ReversibleMutation,
        "creating a file undoes to deleting it"
    );

    std::fs::write(sandbox.path().join("game/blob.bin"), [0xff, 0xfe, 0x00])
        .expect("a writable sandbox");
    let opaque = Arguments::new()
        .with("path", Value::Text("game/blob.bin".into()))
        .with("contents", Value::Text("// replaced".into()));
    assert_eq!(
        registry
            .effect_of("source.write", &mut editor, &opaque)
            .expect("the class is computable"),
        EffectClass::IrreversibleMutation,
        "the editor never read it, so it cannot put it back"
    );

    assert_eq!(
        registry
            .metadata("source.write")
            .expect("registered")
            .effect,
        EffectClass::IrreversibleMutation,
        "the declared class is the ceiling a listing shows"
    );
}

#[test]
fn a_connection_that_was_granted_one_directory_cannot_write_outside_it() {
    // "Operations outside that scope SHALL be refused with the scope as the reason."
    let sandbox = Sandbox::new("scope");
    let registry = registry();
    let mut editor =
        Editor::new(Actor::human("designer")).with_project(ProjectService::new(sandbox.path()));
    let scope = Scope::new(
        "scripts only",
        DocumentScope::All,
        [
            EffectClass::Read,
            EffectClass::ReversibleMutation,
            EffectClass::IrreversibleMutation,
        ],
    )
    .with_directory("game/");

    editor
        .invoke(
            &registry,
            "source.write",
            &scope,
            &Arguments::new()
                .with("path", Value::Text("game/Player.swift".into()))
                .with("contents", Value::Text("// fine".into())),
        )
        .expect("inside the grant");

    let refused = editor
        .invoke(
            &registry,
            "source.write",
            &scope,
            &Arguments::new()
                .with("path", Value::Text("secrets/keys.txt".into()))
                .with("contents", Value::Text("// not fine".into())),
        )
        .expect_err("outside the grant");
    assert!(refused.because.contains("scripts only"), "{refused}");
    assert!(!sandbox.path().join("secrets/keys.txt").exists());
}

#[test]
fn a_path_that_leaves_the_project_is_refused_before_anything_is_written() {
    let sandbox = Sandbox::new("escape");
    let registry = registry();
    let mut editor =
        Editor::new(Actor::human("designer")).with_project(ProjectService::new(sandbox.path()));
    let refused = editor
        .invoke(
            &registry,
            "source.write",
            &Scope::unrestricted(),
            &Arguments::new()
                .with("path", Value::Text("../escaped.txt".into()))
                .with("contents", Value::Text("no".into())),
        )
        .expect_err("a path that leaves the project");
    assert!(refused.remedy.is_some(), "{refused}");
    assert!(
        !sandbox
            .path()
            .parent()
            .unwrap()
            .join("escaped.txt")
            .exists()
    );
}

#[test]
fn a_build_runs_off_the_interface_thread_and_a_reload_then_finds_what_it_produced() {
    // Task 3.7. `project.build` returns as soon as the work is queued — "the editor stays usable
    // while an agent works" is not satisfiable by a build that blocks its caller.
    let sandbox = Sandbox::new("build");
    let registry = registry();
    let mut editor = Editor::new(Actor::human("designer"))
        .with_project(ProjectService::new(sandbox.path()).with_builder(Arc::new(Fake)));

    let refused = editor
        .invoke(
            &registry,
            "project.reload",
            &Scope::unrestricted(),
            &Arguments::new(),
        )
        .expect_err("nothing is built yet");
    assert!(
        refused.because.contains("nothing has been built"),
        "{refused}"
    );

    editor
        .invoke(
            &registry,
            "project.build",
            &Scope::unrestricted(),
            &Arguments::new(),
        )
        .expect("the build is queued");

    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(10);
    while std::time::Instant::now() < deadline {
        editor.pump();
        if editor.project.built().is_ok() {
            break;
        }
        std::thread::sleep(std::time::Duration::from_millis(2));
    }
    let (library, generation) = editor.project.built().expect("the build settles");
    assert_eq!(generation, 1);
    assert!(library.is_file(), "{library:?}");

    // With no runtime attached, the reload is refused with the remedy rather than an error the
    // caller has to treat as a build failure.
    let refused = editor
        .invoke(
            &registry,
            "project.reload",
            &Scope::unrestricted(),
            &Arguments::new(),
        )
        .expect_err("no runtime is attached");
    assert!(refused.because.contains("no runtime"), "{refused}");
    assert!(
        refused
            .remedy
            .as_deref()
            .unwrap()
            .contains("still be there"),
        "{refused}"
    );
}

#[test]
fn play_is_entered_and_left_and_says_what_happens_to_edits() {
    // Task 3.8. The state is the runtime's rather than a panel's, so every viewport agrees.
    let registry = registry();
    let mut editor = Editor::new(Actor::human("designer"));
    assert_eq!(
        cy_editor_commands::ProjectHost::play_state(&editor),
        "editing"
    );

    let entered = editor
        .invoke(
            &registry,
            "play.enter",
            &Scope::unrestricted(),
            &Arguments::new(),
        )
        .expect("play begins");
    assert!(entered.summary.contains("PLAYING"), "{}", entered.summary);
    assert!(
        entered.summary.contains("discarded"),
        "the persistence policy is always stated: {}",
        entered.summary
    );
    assert_eq!(
        cy_editor_commands::ProjectHost::play_state(&editor),
        "playing"
    );

    let again = registry.availability("play.enter", &editor);
    assert!(
        !again.is_available(),
        "entering play twice is a caller that has lost track"
    );

    editor
        .invoke(
            &registry,
            "play.leave",
            &Scope::unrestricted(),
            &Arguments::new(),
        )
        .expect("play ends");
    assert_eq!(
        cy_editor_commands::ProjectHost::play_state(&editor),
        "editing"
    );
}

#[test]
fn a_manipulation_with_nothing_selected_says_what_would_have_worked() {
    let registry = registry();
    let mut editor = Editor::new(Actor::human("designer"));
    editor
        .open_document("worlds/city.cyworld")
        .expect("a document opens");
    let refused = editor
        .invoke(
            &registry,
            "scene.translate",
            &Scope::unrestricted(),
            &Arguments::new().with("amount", Value::Vec3([1.0, 0.0, 0.0])),
        )
        .expect_err("nothing is selected");
    assert!(refused.because.contains("nothing is selected"), "{refused}");
    assert!(
        refused.remedy.as_deref().unwrap().contains("edit.select"),
        "{refused}"
    );
}
