// SPDX-License-Identifier: MIT
//! A joint authored in the physics panel is a transaction, and undo takes it back. Issue #29.
//!
//! The case to read first is `the_names_the_engine_reads_are_the_names_the_editor_writes`: the
//! `Joint` component crosses a process and a language boundary by NAME, so the names are read out
//! of the engine's own header here rather than restated, and
//! `src/gameplay/play/tests/test_joints.cpp` holds a golden world written with them.

#![allow(
    clippy::float_cmp,
    reason = "a joint field asserts the EXACT value it was set to, through the command's text \
              argument. An epsilon would hide a value the command adjusted, which is what these \
              cases exist to catch."
)]

use cy_editor_commands::registry::{Arguments, Registry};
use cy_editor_commands::scope::Scope;
use cy_editor_core::Actor;
use cy_editor_core::ids::NodeId;
use cy_editor_core::value::{Value, ValueKind};
use cy_editor_documents::Document;
use cy_editor_services::builtin;
use cy_editor_services::editor::Editor;
use cy_editor_services::joints::{self, JointBinding, JointField, JointKind};
use cy_editor_services::mirror::engine_identity;
use cy_editor_services::worldfile;

fn registry() -> Registry {
    let mut registry = Registry::new();
    builtin::register(&mut registry).expect("the built-in commands satisfy their own metadata");
    registry
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

fn history_len(editor: &Editor) -> usize {
    document(editor).history().cursor()
}

fn entity(node: NodeId) -> Arguments {
    Arguments::new().with("entity", Value::Text(node.to_string()))
}

fn created(editor: &mut Editor, registry: &Registry) -> NodeId {
    let outcome = editor
        .invoke(
            registry,
            "scene.create-entity",
            &Scope::unrestricted(),
            &Arguments::new(),
        )
        .expect("an entity");
    match outcome.values.get("entity") {
        Some(Value::Text(text)) => u128::from_str_radix(text, 16)
            .map(NodeId::from_u128)
            .expect("the identity the command printed"),
        other => panic!("no entity in the outcome: {other:?}"),
    }
}

/// An open world with a door and a frame, each with a dynamic body, and a third entity with none.
fn door_and_frame() -> (Editor, Registry, NodeId, NodeId, NodeId) {
    let mut editor = Editor::new(Actor::human("designer"));
    let mut document = Document::new("worlds/door.cyworld");
    let transform = document.schema_mut().declare_type("Transform", false);
    for (name, kind) in [
        ("translation", ValueKind::Vec3),
        ("rotation", ValueKind::Quat),
        ("scale", ValueKind::Vec3),
    ] {
        document
            .schema_mut()
            .declare_field(transform, name, kind, "part of a transform")
            .expect("a fresh schema");
    }
    let id = editor.documents.insert(document);
    editor.workspace.opened(id);
    let registry = registry();
    let door = created(&mut editor, &registry);
    let frame = created(&mut editor, &registry);
    let bare = created(&mut editor, &registry);
    for node in [door, frame] {
        invoke(&mut editor, &registry, "scene.add-body", &entity(node));
    }
    (editor, registry, door, frame, bare)
}

/// The `joint_fields` names in the engine's `joints.h`, in declaration order.
fn engine_names() -> Vec<String> {
    let header = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
        .ancestors()
        .nth(3)
        .expect("the crate is at editor/crates/<name>/")
        .join("src/gameplay/play/include/cy/gameplay/play/joints.h");
    let text = std::fs::read_to_string(&header).expect("the engine's joint header");
    let block = text
        .split("namespace joint_fields {")
        .nth(1)
        .expect("the names block")
        .split("}  // namespace joint_fields")
        .next()
        .expect("its end");
    block
        .lines()
        .filter_map(|line| line.split(" = \"").nth(1))
        .filter_map(|rest| rest.split('"').next())
        .map(str::to_string)
        .collect()
}

#[test]
fn the_names_the_engine_reads_are_the_names_the_editor_writes() {
    let engine = engine_names();
    let mut editor: Vec<String> = vec![JointBinding::COMPONENT.to_string()];
    editor.extend(JointField::ALL.iter().map(|field| field.name().to_string()));
    assert_eq!(
        editor, engine,
        "joints.rs and joints.h name the Joint differently"
    );
}

#[test]
fn adding_a_joint_is_one_transaction_and_undo_removes_it() {
    let (mut editor, registry, door, frame, _) = door_and_frame();
    let before = history_len(&editor);
    let summary = invoke(
        &mut editor,
        &registry,
        "physics.joint.add",
        &entity(door)
            .with("kind", Value::Text("hinge".into()))
            .with("target", Value::Text(frame.to_string()))
            .with("anchor", Value::Vec3([-0.5, 0.0, 0.0]))
            .with("axis", Value::Vec3([0.0, 1.0, 0.0])),
    );
    assert!(summary.contains("hinge"), "{summary}");
    assert_eq!(history_len(&editor), before + 1);

    let spec = joints::joint_of(document(&editor), door).expect("the door has a joint");
    assert_eq!(spec.kind, JointKind::Hinge);
    assert_eq!(spec.target, engine_identity(frame));
    assert_eq!(spec.anchor, [-0.5, 0.0, 0.0]);
    assert_eq!(spec.axis, [0.0, 1.0, 0.0]);
    assert_eq!(
        joints::node_of_identity(document(&editor), spec.target),
        Some(frame)
    );

    invoke(&mut editor, &registry, "edit.undo", &Arguments::new());
    assert_eq!(history_len(&editor), before);
    assert!(joints::joint_of(document(&editor), door).is_none());
    invoke(&mut editor, &registry, "edit.redo", &Arguments::new());
    assert_eq!(joints::joint_of(document(&editor), door), Some(spec));
}

#[test]
fn a_joint_that_could_not_simulate_is_refused_and_records_nothing() {
    let (mut editor, registry, door, frame, bare) = door_and_frame();
    let before = history_len(&editor);
    let no_body = refusal(&mut editor, &registry, "physics.joint.add", &entity(bare));
    assert!(no_body.contains("no physics body"), "{no_body}");
    let bodiless_target = refusal(
        &mut editor,
        &registry,
        "physics.joint.add",
        &entity(door).with("target", Value::Text(bare.to_string())),
    );
    assert!(
        bodiless_target.contains("target entity has no physics body"),
        "{bodiless_target}"
    );
    let itself = refusal(
        &mut editor,
        &registry,
        "physics.joint.add",
        &entity(door).with("target", Value::Text(door.to_string())),
    );
    assert!(itself.contains("joined to itself"), "{itself}");
    let unknown = refusal(
        &mut editor,
        &registry,
        "physics.joint.add",
        &entity(door).with("kind", Value::Text("ball".into())),
    );
    assert!(unknown.contains("not a joint kind"), "{unknown}");
    assert_eq!(
        history_len(&editor),
        before,
        "a refusal is not a transaction"
    );

    invoke(&mut editor, &registry, "physics.joint.add", &entity(frame));
    let second = refusal(&mut editor, &registry, "physics.joint.add", &entity(frame));
    assert!(second.contains("already has a joint"), "{second}");
}

fn set(field: &str, value: &str, node: NodeId) -> Arguments {
    entity(node)
        .with("field", Value::Text(field.into()))
        .with("value", Value::Text(value.into()))
}

#[test]
fn each_field_change_is_its_own_transaction_and_undoes_alone() {
    let (mut editor, registry, door, frame, _) = door_and_frame();
    invoke(
        &mut editor,
        &registry,
        "physics.joint.add",
        &entity(door).with("target", Value::Text(frame.to_string())),
    );
    let before = history_len(&editor);
    invoke(
        &mut editor,
        &registry,
        "physics.joint.set",
        &set("limit_min", "-0.75", door),
    );
    invoke(
        &mut editor,
        &registry,
        "physics.joint.set",
        &set("limit_max", "1.25", door),
    );
    invoke(
        &mut editor,
        &registry,
        "physics.joint.set",
        &set("anchor", "0 1 0", door),
    );
    invoke(
        &mut editor,
        &registry,
        "physics.joint.set",
        &set("motor_max_force", "40", door),
    );
    invoke(
        &mut editor,
        &registry,
        "physics.joint.set",
        &set("collide_connected", "true", door),
    );
    assert_eq!(history_len(&editor), before + 5);
    let spec = joints::joint_of(document(&editor), door).unwrap();
    assert_eq!(spec.limit, [-0.75, 1.25]);
    assert_eq!(spec.anchor, [0.0, 1.0, 0.0]);
    assert_eq!(spec.motor, [0.0, 40.0]);
    assert!(spec.collide_connected);

    invoke(&mut editor, &registry, "edit.undo", &Arguments::new());
    let spec = joints::joint_of(document(&editor), door).unwrap();
    assert!(
        !spec.collide_connected,
        "undo took back exactly the last field"
    );
    assert_eq!(spec.motor, [0.0, 40.0]);

    // Joined to the world by an empty target, and back to the frame.
    invoke(
        &mut editor,
        &registry,
        "physics.joint.set",
        &set("target", "", door),
    );
    assert_eq!(joints::joint_of(document(&editor), door).unwrap().target, 0);
}

#[test]
fn a_change_the_engine_would_refuse_is_refused_now_and_records_nothing() {
    let (mut editor, registry, door, _, bare) = door_and_frame();
    invoke(&mut editor, &registry, "physics.joint.add", &entity(door));
    let before = history_len(&editor);
    for (field, value, because) in [
        ("break_force", "-1", "break threshold is negative"),
        ("axis", "0 0 0", "axis has no direction"),
        ("limit_min", "wide", "is not a finite number"),
        ("anchor", "1 2", "three finite numbers"),
        ("target", &bare.to_string(), "no physics body"),
        ("spring", "1", "has no field"),
    ] {
        let problem = refusal(
            &mut editor,
            &registry,
            "physics.joint.set",
            &set(field, value, door),
        );
        assert!(problem.contains(because), "{field}={value}: {problem}");
    }
    assert_eq!(history_len(&editor), before);
}

#[test]
fn a_hinge_becoming_a_distance_joint_takes_a_span_rather_than_being_refused() {
    let (mut editor, registry, door, _, _) = door_and_frame();
    invoke(&mut editor, &registry, "physics.joint.add", &entity(door));
    let before = history_len(&editor);
    invoke(
        &mut editor,
        &registry,
        "physics.joint.set",
        &set("kind", "distance", door),
    );
    // One transaction for the kind and the span it needs, so one undo takes back both.
    assert_eq!(history_len(&editor), before + 1);
    let spec = joints::joint_of(document(&editor), door).unwrap();
    assert_eq!(spec.kind, JointKind::Distance);
    assert_eq!(spec.limit, [0.0, 1.0]);
    invoke(&mut editor, &registry, "edit.undo", &Arguments::new());
    let spec = joints::joint_of(document(&editor), door).unwrap();
    assert_eq!(spec.kind, JointKind::Hinge);
    assert_eq!(spec.limit, [1.0, -1.0], "the free range is back");
}

#[test]
fn removing_a_joint_is_undone_with_every_value_it_had() {
    let (mut editor, registry, door, frame, _) = door_and_frame();
    invoke(
        &mut editor,
        &registry,
        "physics.joint.add",
        &entity(door)
            .with("kind", Value::Text("slider".into()))
            .with("target", Value::Text(frame.to_string())),
    );
    invoke(
        &mut editor,
        &registry,
        "physics.joint.set",
        &set("limit_max", "2", door),
    );
    let authored = joints::joint_of(document(&editor), door).unwrap();
    invoke(
        &mut editor,
        &registry,
        "physics.joint.remove",
        &entity(door),
    );
    assert!(joints::joint_of(document(&editor), door).is_none());
    let again = refusal(
        &mut editor,
        &registry,
        "physics.joint.remove",
        &entity(door),
    );
    assert!(again.contains("no joint"), "{again}");
    invoke(&mut editor, &registry, "edit.undo", &Arguments::new());
    assert_eq!(joints::joint_of(document(&editor), door), Some(authored));
}

#[test]
fn a_saved_joint_is_the_world_the_engine_plays() {
    // The component a save writes carries every field by the name the engine reads, and the
    // target as the POSITION of the frame's node, which is what the engine resolves at play.
    let (mut editor, registry, door, frame, _) = door_and_frame();
    invoke(
        &mut editor,
        &registry,
        "physics.joint.add",
        &entity(door)
            .with("kind", Value::Text("point".into()))
            .with("target", Value::Text(frame.to_string())),
    );
    let text = worldfile::write_world(document(&editor));
    for field in JointField::ALL {
        assert!(
            text.contains(&format!(" {} \"{}\" ", field.kind().name(), field.name())),
            "the saved world declares {}: {text}",
            field.name()
        );
    }
    assert!(text.contains("\"point\""), "{text}");
    // The door, the frame and the bare entity are positions 0, 1 and 2.
    let mut reopened = Document::new("worlds/door.cyworld");
    worldfile::load(&text, &mut reopened, Actor::human("designer")).unwrap();
    let door_again = reopened.content().roots()[0];
    let frame_again = reopened.content().roots()[1];
    let spec = joints::joint_of(&reopened, door_again).expect("the joint survived the save");
    assert_eq!(spec.target, engine_identity(frame_again));
}
