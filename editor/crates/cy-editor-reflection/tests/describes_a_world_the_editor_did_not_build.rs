//! The catalogue against a real `CyInterface` table, reached through `dlopen`. Task 4.6, and the
//! close of M4's debt 1.3.
//!
//! M4 recorded: *"Nothing is reflected, so the editor's inspector has nothing to read."* ABI 1.1
//! answered the engine's half. This is the editor's half, exercised the way it will be exercised
//! against the engine: a shared library publishing `cy_get_interface`, opened at run time, its
//! component registry read through `world_component_count`, `world_component_info` and
//! `world_component_field`, and every value marshalled through `CyVar`.
//!
//! The library is this workspace's fixture rather than the engine, which is a real limit stated
//! plainly — `cy-editor-testhost` implements the ABI, so a layout or calling-convention mistake
//! fails here, and a *semantic* difference in the engine's registry does not. The engine's side of
//! the same description is proved by `src/abi/tests/`.

use cy_editor_core::Value;
use cy_editor_reflection::presentation::{Range, Unit};
use cy_editor_reflection::{Catalogue, Origin, Overrides, PropertyOverride};
use cy_editor_sdk::host::RuntimeLibrary;
use cy_editor_testhost::{TRANSFORM, library_path};

/// Open the fixture, failing with the path when it is not where it should be.
fn open() -> RuntimeLibrary {
    let path = library_path();
    RuntimeLibrary::open(&path)
        .unwrap_or_else(|problem| panic!("could not open {}: {problem}", path.display()))
}

#[test]
fn the_engines_own_component_types_arrive_as_a_catalogue() {
    let library = open();
    let engine = library
        .create_engine()
        .expect("the fixture hosts in process");
    let world = engine.world().expect("the fixture's engine has a world");

    let catalogue = Catalogue::of_world(&world).expect("the world describes itself");
    assert_eq!(catalogue.origin(), Origin::Runtime);
    assert!(
        catalogue.types().len() >= 3,
        "the fixture registers three component types"
    );

    let transform = catalogue
        .type_named("Transform")
        .expect("a component type the EDITOR never registered");
    assert_eq!(transform.id.as_u64(), u64::from(TRANSFORM));
    assert!(!transform.is_tag);
    assert_eq!(
        transform.fields.len(),
        2,
        "position and scale, as the engine describes them"
    );
    assert_eq!(transform.fields[0].name, "position");
    assert_eq!(
        transform.fields[0].kind,
        cy_editor_core::value::ValueKind::Vec3
    );
    assert_eq!(
        transform.fields[0].presentation.order, 0,
        "declaration order is presentation order until something says otherwise"
    );
}

#[test]
fn a_tag_component_is_described_as_a_tag_rather_than_as_an_empty_form() {
    let library = open();
    let engine = library.create_engine().unwrap();
    let world = engine.world().unwrap();
    let catalogue = Catalogue::of_world(&world).unwrap();

    let selected = catalogue.type_named("Selected").expect("the fixture's tag");
    assert!(selected.is_tag, "a tag has no data and no column");
    assert!(selected.fields.is_empty());
    assert!(
        !selected.partially_described,
        "a tag is fully described by having nothing to describe, which is not the same as a \
         component whose members the ABI cannot name"
    );
}

#[test]
fn a_reflected_field_round_trips_a_value_through_the_abi() {
    // The property the generated inspector rests on: a form built from the catalogue addresses a
    // field by the index the catalogue carries, and writing through it changes the engine's world.
    let library = open();
    let engine = library.create_engine().unwrap();
    let world = engine.world().unwrap();
    let catalogue = Catalogue::of_world(&world).unwrap();

    let transform = catalogue.type_named("Transform").unwrap();
    let position = transform.field_named("position").unwrap();

    let entity = world.create_entity().unwrap();
    world.add_component(entity, transform.id).unwrap();
    world
        .set_field(
            entity,
            transform.id,
            position.index,
            &Value::Vec3([1.0, 2.0, 3.0]),
        )
        .unwrap();

    assert_eq!(
        world
            .get_field(entity, transform.id, position.index)
            .unwrap(),
        Value::Vec3([1.0, 2.0, 3.0]),
        "the field the catalogue described is the field that changed"
    );
}

#[test]
fn overrides_supply_the_metadata_the_abi_cannot_yet_carry() {
    // ABI 1.1 describes a field's storage and not its meaning. This is the mechanism that stands in
    // until it does — and the one `editor-ui-ux` requires for plugins regardless.
    let library = open();
    let engine = library.create_engine().unwrap();
    let world = engine.world().unwrap();

    let mut overrides = Overrides::new();
    overrides.register_property(
        "Transform",
        "position",
        PropertyOverride::new()
            .with_unit(Unit::Metres)
            .with_range(Range::new(-4096.0, 4096.0))
            .with_tooltip("Where the node sits in its parent's space."),
    );

    let catalogue = Catalogue::of_world(&world)
        .unwrap()
        .with_overrides(&overrides);
    let position = catalogue
        .type_named("Transform")
        .unwrap()
        .field_named("position")
        .unwrap();

    assert_eq!(position.presentation.unit, Some(Unit::Metres));
    assert_eq!(
        position.tooltip(),
        "Where the node sits in its parent's space."
    );
    assert!(
        position
            .presentation
            .validate("position", &Value::Vec3([0.0; 3]))
            .is_ok()
    );
}

#[test]
fn a_field_the_engine_described_without_a_sentence_still_says_what_it_is() {
    // The state of affairs today, asserted rather than hidden: no `CyFieldDesc` carries a
    // description, so every runtime field's tooltip falls back to its name and kind. When the ABI
    // grows attributes, this test is what notices.
    let library = open();
    let engine = library.create_engine().unwrap();
    let world = engine.world().unwrap();
    let catalogue = Catalogue::of_world(&world).unwrap();

    for reflected in catalogue.types() {
        for field in &reflected.fields {
            assert!(
                field.description.is_empty(),
                "{}::{} arrived with a description, which means the ABI grew one and \
                 cy-editor-reflection should carry it rather than falling back",
                reflected.name,
                field.name
            );
            assert_eq!(field.tooltip(), format!("{} ({})", field.name, field.kind));
        }
    }
}
