//! The generated inspector against a real `CyInterface` table. Task 4.6, and criterion 7.5:
//! *"The generated inspector edits a reflected type with no per-type editor code."*
//!
//! The chain this exercises, end to end:
//!
//! 1. a shared library publishing `cy_get_interface` is opened at run time — `dlopen`, `dlsym`, the
//!    ABI header check;
//! 2. its component registry is read through `world_component_count`, `world_component_info` and
//!    `world_component_field` — component types the **editor never registered**;
//! 3. `cy_editor_reflection::Catalogue::of_world` turns them into descriptions;
//! 4. `cy_editor_interface::inspector` generates rows and controls from those descriptions, with no
//!    code anywhere that names `Transform` or `Health`;
//! 5. an edit through the inspector produces one transaction, and the value it produced is the value
//!    the engine accepts.
//!
//! Two limits, stated rather than implied. The library is this workspace's ABI fixture and not the
//! engine, so this proves the editor's side of the boundary; the engine's side is proved by
//! `src/abi/tests/` against the same description. And step 5 writes to the world **directly**, not
//! over the live bridge — the transport is task 5.2's, and what is asserted here is only that the
//! value the inspector committed is one the engine takes without conversion.

use cy_editor_commands::Registry;
use cy_editor_core::Actor;
use cy_editor_core::ids::TypeId;
use cy_editor_core::value::{Value, ValueKind};
use cy_editor_documents::selection::Selection;
use cy_editor_interface::inspector::{Control, GeneratedInspector};
use cy_editor_interface::shell::Shell;
use cy_editor_reflection::presentation::{Range, Unit};
use cy_editor_reflection::{Catalogue, Origin, Overrides, PropertyOverride};
use cy_editor_sdk::host::RuntimeLibrary;
use cy_editor_services::{Editor, builtin};
use cy_editor_testhost::library_path;

/// Open the ABI fixture.
fn open() -> RuntimeLibrary {
    let path = library_path();
    RuntimeLibrary::open(&path)
        .unwrap_or_else(|problem| panic!("could not open {}: {problem}", path.display()))
}

/// An editor whose document carries the **engine's** component types, and the catalogue that
/// describes them.
///
/// The document's content is keyed by the identities the engine's registry assigned, which is what
/// makes the inspector lay out a runtime type: nothing in the document's own schema describes them,
/// and the inspector reads the catalogue.
fn editor_over(catalogue: &Catalogue) -> (Editor, TypeId, Vec<cy_editor_core::ids::NodeId>) {
    let transform = catalogue
        .type_named("Transform")
        .expect("the engine's Transform");
    let position = transform
        .field_named("position")
        .expect("its position field");

    let mut editor = Editor::default();
    let id = editor.open_document("worlds/city.cyworld").unwrap();
    let document = editor.documents.get_mut(id).unwrap();
    let nodes = document
        .with_transaction("Build", Actor::human("designer"), |document| {
            let mut nodes = Vec::new();
            for _ in 0..3 {
                let node = document.create_node(None)?;
                document.add_component(
                    node,
                    transform.id,
                    vec![(position.id, Value::Vec3([0.0, 0.0, 0.0]))],
                )?;
                nodes.push(node);
            }
            Ok(nodes)
        })
        .unwrap();

    let mut selection = Selection::new();
    selection.set_nodes(nodes.clone());
    editor.selection.set(selection);
    (editor, transform.id, nodes)
}

#[test]
fn a_component_type_the_editor_never_registered_gets_an_inspector() {
    let library = open();
    let engine = library
        .create_engine()
        .expect("the fixture hosts in process");
    let world = engine.world().expect("the fixture's engine has a world");
    let catalogue = Catalogue::of_world(&world).expect("the world describes itself");
    assert_eq!(catalogue.origin(), Origin::Runtime);

    let (editor, transform, _) = editor_over(&catalogue);
    let mut inspector = GeneratedInspector::with_catalogue(catalogue);
    assert!(inspector.refresh(&editor));

    let section = inspector
        .sections()
        .iter()
        .find(|section| section.component == transform)
        .expect("a section for a type described entirely by the engine");
    assert_eq!(section.title, "Transform");
    assert_eq!(section.origin, Origin::Runtime);
    assert_eq!(
        section.rows.len(),
        2,
        "the section shows what the ENGINE says the type has — position and scale — rather than \
         what this document's nodes happen to carry"
    );
    let position = section
        .rows
        .iter()
        .find(|row| row.name == "position")
        .expect("the engine's own field name");
    assert_eq!(
        position.control,
        Control::Vector(3),
        "the control came from the field's kind, which came from the ABI"
    );
}

#[test]
fn editing_a_reflected_type_is_one_transaction_and_the_engine_takes_the_value() {
    let library = open();
    let engine = library.create_engine().unwrap();
    let world = engine.world().unwrap();
    let catalogue = Catalogue::of_world(&world).unwrap();

    let transform = catalogue.type_named("Transform").unwrap().clone();
    let position = transform.field_named("position").unwrap().clone();
    let (mut editor, component, nodes) = editor_over(&catalogue);

    let mut inspector = GeneratedInspector::with_catalogue(catalogue);
    inspector.refresh(&editor);

    let id = editor.workspace.active().unwrap();
    let entries = editor.documents.get(id).unwrap().history().entries().len();

    let moved = Value::Vec3([1.5, 2.5, -3.0]);
    inspector.begin_edit(component, position.id, moved.clone());
    assert_eq!(
        inspector.commit_edit(&mut editor).unwrap(),
        nodes.len(),
        "one edit reached every selected node"
    );

    let history = editor.documents.get(id).unwrap().history();
    assert_eq!(
        history.entries().len(),
        entries + 1,
        "and it was one transaction"
    );
    assert_eq!(
        history.entries().last().unwrap().operations.len(),
        nodes.len()
    );

    // The value the inspector produced, handed to the engine unchanged. The transport that will
    // carry it is task 5.2's; what this asserts is that no conversion sits between the two.
    let entity = world.create_entity().unwrap();
    world.add_component(entity, component).unwrap();
    world
        .set_field(entity, component, position.index, &moved)
        .unwrap();
    assert_eq!(
        world.get_field(entity, component, position.index).unwrap(),
        moved
    );
}

#[test]
fn no_per_type_editor_code_exists_anywhere_in_the_interface_crate() {
    // The requirement is about the *absence* of code, so the test reads the source. Crude, and the
    // only honest way to assert an absence: a `match` on a component's name in a panel is exactly
    // what "generated from reflection" forbids, and it would otherwise be caught by nobody, because
    // every functional test still passes when a special case is added beside the general one.
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("src");
    let mut offenders = Vec::new();
    let mut stack = vec![root];
    while let Some(directory) = stack.pop() {
        for entry in std::fs::read_dir(&directory).expect("the crate's sources are readable") {
            let path = entry.expect("a readable entry").path();
            if path.is_dir() {
                stack.push(path);
                continue;
            }
            if path.extension().is_none_or(|extension| extension != "rs") {
                continue;
            }
            let text = std::fs::read_to_string(&path).expect("a readable source file");
            // Everything above `#[cfg(test)]` is the crate's shipping source. A test may name a
            // component type — it has to, to build a fixture — and the requirement is about the
            // code that lays out the inspector.
            let shipping = text
                .split_once("#[cfg(test)]")
                .map_or(text.as_str(), |(before, _)| before);
            for (number, line) in shipping.lines().enumerate() {
                let code = line.split("//").next().unwrap_or_default();
                // The names of engine component types, in the position a special case would put
                // them: a string literal being compared against, in non-test code.
                for name in [
                    "\"Transform\"",
                    "\"Health\"",
                    "\"MeshInstance\"",
                    "\"Light\"",
                ] {
                    if code.contains(name) {
                        offenders.push(format!("{}:{}", path.display(), number + 1));
                    }
                }
            }
        }
    }
    assert!(
        offenders.is_empty(),
        "the interface names a component type outside its tests, which is per-type editor code:\n  \
         {}",
        offenders.join("\n  ")
    );
}

#[test]
fn a_custom_editor_overrides_the_generated_form_without_a_special_case_in_the_generator() {
    let library = open();
    let engine = library.create_engine().unwrap();
    let world = engine.world().unwrap();

    let mut overrides = Overrides::new();
    overrides.register_property(
        "Transform",
        "position",
        PropertyOverride::new()
            .with_editor("world-position-picker")
            .with_unit(Unit::Metres)
            .with_range(Range::new(-4096.0, 4096.0)),
    );
    let catalogue = Catalogue::of_world(&world)
        .unwrap()
        .with_overrides(&overrides);
    let (editor, component, _) = editor_over(&catalogue);

    let mut inspector = GeneratedInspector::with_catalogue(catalogue);
    inspector.refresh(&editor);
    let row = &inspector
        .sections()
        .iter()
        .find(|section| section.component == component)
        .unwrap()
        .rows[0];

    assert_eq!(row.control, Control::Custom("world-position-picker".into()));
    assert_eq!(row.unit, Some(Unit::Metres));
}

#[test]
fn the_shell_shows_a_runtime_type_the_moment_a_runtime_is_attached() {
    // The sequence a session actually follows: the editor opens a document with no runtime, and a
    // runtime arrives later. The inspector is handed a new catalogue and rebuilds; nothing else in
    // the interface changes, and no panel moves.
    let library = open();
    let engine = library.create_engine().unwrap();
    let world = engine.world().unwrap();
    let catalogue = Catalogue::of_world(&world).unwrap();
    let (editor, ..) = editor_over(&catalogue);

    let mut registry = Registry::new();
    builtin::register(&mut registry).unwrap();
    let mut shell = Shell::new(&registry).unwrap();

    shell.refresh(&editor);
    assert!(
        shell.inspector.sections().is_empty(),
        "with nothing describing the types, there is nothing to lay out"
    );
    let layout = shell.workspaces.current().encode();

    shell.describe_with(catalogue);
    shell.refresh(&editor);

    assert_eq!(shell.inspector.sections().len(), 1);
    assert_eq!(shell.inspector.summary().count, 3);
    assert_eq!(
        shell.workspaces.current().encode(),
        layout,
        "attaching a runtime moved no panel"
    );
}

#[test]
fn a_field_kind_the_engine_reports_maps_onto_a_control_for_every_kind() {
    // Every `ValueKind` the ABI can report has a control, so a component using one this editor has
    // never seen is still editable. The mapping is total by construction; this is what notices when
    // a new kind is added and the mapping is not.
    let kinds = [
        ValueKind::Bool,
        ValueKind::Int,
        ValueKind::Float,
        ValueKind::Double,
        ValueKind::Vec2,
        ValueKind::Vec3,
        ValueKind::Vec4,
        ValueKind::Quat,
        ValueKind::Text,
        ValueKind::Bytes,
        ValueKind::Entity,
        ValueKind::Nil,
    ];
    for kind in kinds {
        let field = cy_editor_reflection::ReflectedField {
            id: cy_editor_core::ids::FieldId::from_raw(1),
            name: "field".into(),
            kind,
            description: String::new(),
            index: 0,
            presentation: cy_editor_reflection::Presentation::at(0),
        };
        let control = cy_editor_interface::inspector::control_for(&field);
        assert!(
            !matches!(control, Control::Custom(_)),
            "{kind} resolved to a custom editor with none registered"
        );
    }
}
