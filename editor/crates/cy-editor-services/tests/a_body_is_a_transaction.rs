//! A body added in the inspector is a transaction, and undo removes it. M8.a task 4.3.
//!
//! Integration rather than unit for the reason `the_authoring_loop` gives: every claim here is
//! about a join. "One transaction" is a statement about the registry, the document's schema and the
//! document's history being the same ones a person's click reaches.
//!
//! THE CASE TO READ FIRST is `the_names_the_engine_reads_are_the_names_the_editor_writes`. Physics'
//! components are registered in the ECS by name with no reflected type behind them, so a saved
//! `.cyworld` carries them by name and `src/gameplay/play/src/session.cpp` reads them back by name.
//! That makes four type names and six field names a contract across a process and a language
//! boundary, and the only way to keep such a contract is to hold the strings in both languages'
//! tests. This file is the Rust half.

use cy_editor_commands::registry::{Arguments, Registry};
use cy_editor_commands::scope::Scope;
use cy_editor_core::Actor;
use cy_editor_core::ids::NodeId;
use cy_editor_core::value::{Value, ValueKind};
use cy_editor_documents::Document;
use cy_editor_services::bodies::{
    self, BodyBinding, BodyKind, ColliderBinding, ColliderShape, ColliderSpec,
};
use cy_editor_services::builtin;
use cy_editor_services::editor::Editor;

/// THE GOLDEN NAMES, PINNED ON BOTH SIDES OF THE PROCESS BOUNDARY.
///
/// `src/gameplay/play/src/session.cpp` holds these same strings as `kRigidBody`, `kStaticBody`,
/// `kKinematicBody`, `kCollider`, `kFieldMass`, `kFieldGravityScale`, `kFieldShape`, `kFieldExtent`,
/// `kFieldRadius` and `kFieldHeight`, and `src/gameplay/play/tests/test_play.cpp` writes a whole
/// `.cyworld` using them. A spelling that drifts on one side is a body the runtime silently does
/// not simulate — so it is a string comparison here rather than a comment.
const ENGINE_TYPE_NAMES: [&str; 4] = ["RigidBody", "StaticBody", "KinematicBody", "Collider"];
const ENGINE_FIELD_NAMES: [&str; 6] = [
    "mass",
    "gravity_scale",
    "shape",
    "extent",
    "radius",
    "height",
];

fn registry() -> Registry {
    let mut registry = Registry::new();
    builtin::register(&mut registry).expect("the built-in commands satisfy their own metadata");
    registry
}

/// An editor with one open world holding one node, which is what a person has when they click an
/// entity and reach for the inspector.
fn editor_with_an_entity() -> (Editor, Registry, NodeId) {
    let mut editor = Editor::new(Actor::human("designer"));
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

    let registry = registry();
    let outcome = editor
        .invoke(
            &registry,
            "scene.create-entity",
            &Scope::unrestricted(),
            &Arguments::new(),
        )
        .expect("an entity");
    let node = match outcome.values.get("entity") {
        Some(Value::Text(text)) => u128::from_str_radix(text, 16)
            .map(NodeId::from_u128)
            .expect("the identity the command printed"),
        other => panic!("no entity in the outcome: {other:?}"),
    };
    (editor, registry, node)
}

fn document(editor: &Editor) -> &Document {
    editor
        .documents
        .get(editor.workspace.active().expect("a document is active"))
        .expect("the active document")
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

fn entity_argument(node: NodeId) -> Arguments {
    Arguments::new().with("entity", Value::Text(node.to_string()))
}

/// How many transactions have been APPLIED — the history's cursor, not its length. Undo moves the
/// cursor and keeps the entry, which is what makes redo possible, so a length would not change.
fn history_len(editor: &Editor) -> usize {
    document(editor).history().cursor()
}

#[test]
fn the_names_the_engine_reads_are_the_names_the_editor_writes() {
    // The four component names, as the enumeration spells them.
    let from_enum: Vec<&str> = BodyKind::ALL
        .iter()
        .map(|kind| kind.component())
        .chain(std::iter::once(ColliderBinding::COMPONENT))
        .collect();
    assert_eq!(from_enum, ENGINE_TYPE_NAMES);
    assert_eq!(BodyBinding::COMPONENTS, ENGINE_TYPE_NAMES[..3]);

    let fields = [
        BodyBinding::MASS,
        BodyBinding::GRAVITY_SCALE,
        ColliderBinding::SHAPE,
        ColliderBinding::EXTENT,
        ColliderBinding::RADIUS,
        ColliderBinding::HEIGHT,
    ];
    assert_eq!(fields, ENGINE_FIELD_NAMES);

    // And the four shape words `session.cpp`'s `shape_of` branches on.
    let shapes: Vec<&str> = ColliderShape::ALL
        .iter()
        .map(|shape| shape.keyword())
        .collect();
    assert_eq!(shapes, ["box", "sphere", "capsule", "cylinder"]);
}

#[test]
fn adding_a_body_is_one_transaction_and_undo_removes_it() {
    let (mut editor, registry, node) = editor_with_an_entity();
    let before = history_len(&editor);

    let summary = invoke(
        &mut editor,
        &registry,
        "scene.add-body",
        &entity_argument(node),
    );
    assert!(summary.contains("dynamic"), "{summary}");
    assert!(summary.contains("box"), "{summary}");

    // ONE transaction, for a body AND its collider. A dynamic body with no collider has no volume
    // and the engine refuses it by name, so writing the two separately would mean one undo step
    // that leaves a world which cannot simulate.
    assert_eq!(history_len(&editor), before + 1);
    assert_eq!(
        bodies::body_of(document(&editor), node),
        Some(BodyKind::Dynamic)
    );
    assert!(bodies::has_collider(document(&editor), node));

    invoke(&mut editor, &registry, "edit.undo", &Arguments::new());
    assert_eq!(history_len(&editor), before);
    assert_eq!(bodies::body_of(document(&editor), node), None);
    assert!(!bodies::has_collider(document(&editor), node));

    // And redo puts BOTH back, on the same entity.
    invoke(&mut editor, &registry, "edit.redo", &Arguments::new());
    assert_eq!(
        bodies::body_of(document(&editor), node),
        Some(BodyKind::Dynamic)
    );
    assert!(bodies::has_collider(document(&editor), node));
}

#[test]
fn the_values_a_caller_gave_are_the_values_the_document_holds() {
    let (mut editor, registry, node) = editor_with_an_entity();
    let arguments = entity_argument(node)
        .with("kind", Value::Text("dynamic".to_string()))
        .with("mass", Value::Float(12.5))
        .with("shape", Value::Text("capsule".to_string()))
        .with("radius", Value::Float(0.3))
        .with("height", Value::Float(1.8));
    invoke(&mut editor, &registry, "scene.add-body", &arguments);

    let document = document(&editor);
    let body = BodyBinding::of_schema(document.schema(), BodyKind::Dynamic).expect("a body");
    let mass = body.mass.expect("a dynamic body has a mass field");
    assert_eq!(
        document.content().field(node, body.component, mass),
        Some(&Value::Float(12.5))
    );

    let collider = ColliderBinding::of_schema(document.schema()).expect("a collider");
    assert_eq!(
        document
            .content()
            .field(node, collider.component, collider.shape),
        Some(&Value::Text("capsule".to_string()))
    );
    assert_eq!(
        document
            .content()
            .field(node, collider.component, collider.radius),
        Some(&Value::Float(0.3))
    );
    assert_eq!(
        document
            .content()
            .field(node, collider.component, collider.height),
        Some(&Value::Float(1.8))
    );
}

#[test]
fn a_static_body_carries_no_authored_value_and_is_still_a_component() {
    let (mut editor, registry, node) = editor_with_an_entity();
    let arguments = entity_argument(node)
        .with("kind", Value::Text("static".to_string()))
        .with("extent", Value::Vec3([5.0, 0.5, 5.0]));
    invoke(&mut editor, &registry, "scene.add-body", &arguments);

    assert_eq!(
        bodies::body_of(document(&editor), node),
        Some(BodyKind::Static)
    );
    let binding =
        BodyBinding::of_schema(document(&editor).schema(), BodyKind::Static).expect("a body");
    // NO FIELDS, deliberately: what a static body IS is its placement and its colliders, and both
    // are other components. A field invented to avoid an empty component would be a value a person
    // could set and nothing would read.
    assert!(binding.mass.is_none());
    assert!(binding.gravity_scale.is_none());
    assert!(
        document(&editor)
            .content()
            .has_component(node, binding.component)
    );

    let collider = ColliderBinding::of_schema(document(&editor).schema()).expect("a collider");
    assert_eq!(
        document(&editor)
            .content()
            .field(node, collider.component, collider.extent),
        Some(&Value::Vec3([5.0, 0.5, 5.0]))
    );
}

#[test]
fn a_second_body_on_one_entity_is_refused_by_name() {
    let (mut editor, registry, node) = editor_with_an_entity();
    invoke(
        &mut editor,
        &registry,
        "scene.add-body",
        &entity_argument(node),
    );
    let before = history_len(&editor);

    let arguments = entity_argument(node).with("kind", Value::Text("static".to_string()));
    let refused = refusal(&mut editor, &registry, "scene.add-body", &arguments);
    assert!(refused.contains("already has"), "{refused}");
    // A REFUSED COMMAND RECORDS NOTHING. An entity with two body components is one the solver has
    // to break a tie for, and a half-written transaction would be worse than the refusal.
    assert_eq!(history_len(&editor), before);
}

#[test]
fn a_body_with_no_collider_is_possible_and_says_so() {
    let (mut editor, registry, node) = editor_with_an_entity();
    let arguments = entity_argument(node).with("shape", Value::Text("none".to_string()));
    let summary = invoke(&mut editor, &registry, "scene.add-body", &arguments);
    assert!(summary.contains("no collider"), "{summary}");
    assert!(!bodies::has_collider(document(&editor), node));

    // And the collider can be added afterwards, as its own transaction.
    let before = history_len(&editor);
    invoke(
        &mut editor,
        &registry,
        "scene.add-collider",
        &entity_argument(node),
    );
    assert_eq!(history_len(&editor), before + 1);
    assert!(bodies::has_collider(document(&editor), node));
}

#[test]
fn removing_a_body_is_a_transaction_and_undo_restores_the_values_it_had() {
    let (mut editor, registry, node) = editor_with_an_entity();
    let arguments = entity_argument(node).with("mass", Value::Float(7.25));
    invoke(&mut editor, &registry, "scene.add-body", &arguments);

    invoke(
        &mut editor,
        &registry,
        "scene.remove-body",
        &entity_argument(node),
    );
    assert_eq!(bodies::body_of(document(&editor), node), None);
    // The collider stays: a collider is geometry, and removing it is a separate decision.
    assert!(bodies::has_collider(document(&editor), node));

    invoke(&mut editor, &registry, "edit.undo", &Arguments::new());
    let document = document(&editor);
    let binding = BodyBinding::of_schema(document.schema(), BodyKind::Dynamic).expect("a body");
    // THE MASS IT HAD, not a default one. The operation recorded the values it removed, which is
    // what makes this undo exact rather than approximately right.
    assert_eq!(
        document
            .content()
            .field(node, binding.component, binding.mass.expect("mass")),
        Some(&Value::Float(7.25))
    );
}

#[test]
fn a_command_with_no_entity_acts_on_the_selection_and_says_when_it_cannot() {
    let (mut editor, registry, node) = editor_with_an_entity();
    // `scene.create-entity` selected it, so the bare command finds it.
    invoke(&mut editor, &registry, "scene.add-body", &Arguments::new());
    assert_eq!(
        bodies::body_of(document(&editor), node),
        Some(BodyKind::Dynamic)
    );

    editor
        .selection
        .set(cy_editor_documents::selection::Selection::new());
    let refused = refusal(&mut editor, &registry, "scene.add-body", &Arguments::new());
    assert!(refused.contains("nothing is selected"), "{refused}");
}

#[test]
fn what_a_caller_may_not_say_is_refused_with_a_remedy() {
    let (mut editor, registry, node) = editor_with_an_entity();

    let arguments = entity_argument(node).with("kind", Value::Text("ragdoll".to_string()));
    let refused = refusal(&mut editor, &registry, "scene.add-body", &arguments);
    assert!(refused.contains("body kind"), "{refused}");
    assert!(refused.contains("kinematic"), "{refused}");

    let arguments = entity_argument(node).with("shape", Value::Text("torus".to_string()));
    let refused = refusal(&mut editor, &registry, "scene.add-body", &arguments);
    assert!(refused.contains("collider shape"), "{refused}");

    let arguments = entity_argument(node).with("mass", Value::Float(-1.0));
    let refused = refusal(&mut editor, &registry, "scene.add-body", &arguments);
    assert!(refused.contains("negative"), "{refused}");

    // Nothing above wrote anything.
    assert_eq!(bodies::body_of(document(&editor), node), None);
}

#[test]
fn the_bindings_are_idempotent_so_a_second_body_addresses_the_same_identifiers() {
    // A second `declare` that issued new identifiers would give one name two identities, and every
    // history entry addressing the first would stop applying. The same property
    // `primitives::MeshBinding::declare` keeps, checked here because a second body in one world is
    // the ordinary case rather than the exotic one.
    let mut document = Document::new("worlds/city.cyworld");
    let first = BodyBinding::declare(document.schema_mut(), BodyKind::Dynamic);
    let second = BodyBinding::declare(document.schema_mut(), BodyKind::Dynamic);
    assert_eq!(first, second);

    let one = ColliderBinding::declare(document.schema_mut());
    let two = ColliderBinding::declare(document.schema_mut());
    assert_eq!(one, two);

    // And the three body components are three distinct types, not one under three names.
    let statik = BodyBinding::declare(document.schema_mut(), BodyKind::Static);
    let kinematic = BodyBinding::declare(document.schema_mut(), BodyKind::Kinematic);
    assert_ne!(first.component, statik.component);
    assert_ne!(statik.component, kinematic.component);
}

#[test]
fn the_constructors_work_without_a_command_so_an_importer_can_call_them() {
    // `add_body_to` and `add_collider_to` are public for the same reason
    // `primitives::create_mesh_instance` is: an import that lands a mesh with a generated collider
    // should build the same components a person's click builds, through the same function.
    let mut document = Document::new("worlds/city.cyworld");
    let node = document
        .with_transaction("Build", Actor::human("test"), |document| {
            let node = document.create_node(None)?;
            bodies::add_body_to(document, node, BodyKind::Dynamic, 3.0)?;
            bodies::add_collider_to(
                document,
                node,
                ColliderSpec {
                    shape: ColliderShape::Sphere,
                    radius: 0.25,
                    ..ColliderSpec::default()
                },
            )?;
            Ok(node)
        })
        .expect("one transaction");

    assert_eq!(bodies::body_of(&document, node), Some(BodyKind::Dynamic));
    assert!(bodies::has_collider(&document, node));
    assert_eq!(document.history().cursor(), 1);
}
