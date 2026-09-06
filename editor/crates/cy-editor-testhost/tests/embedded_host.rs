//! The SDK against a real interface table, reached the way it reaches an engine. Tasks 2.2 and 2.3.
//!
//! Everything here goes through `dlopen` and `dlsym` on a `.so` this workspace built: the header
//! check, the schema reader, the hierarchy, and `CyVar` marshalling in both directions. What it
//! proves is the part no unit test can — that the generated `#[repr(C)]` table, the calling
//! convention, and the layout the ABI description computed all agree with what a compiled library
//! actually publishes.
//!
//! The library is this workspace's fixture rather than the engine, and that is a real limit stated
//! plainly: it proves the SDK's side of the boundary. The engine's side is proved by
//! `src/abi/tests/`, against the same description.

use cy_editor_core::Value;
use cy_editor_sdk::abi;
use cy_editor_sdk::host::{CREATE_SYMBOL, RuntimeLibrary};
use cy_editor_testhost::{SELECTED, TRANSFORM, library_path};

/// Open the fixture, failing with the path when it is not where it should be.
fn open() -> RuntimeLibrary {
    let path = library_path();
    RuntimeLibrary::open(&path)
        .unwrap_or_else(|problem| panic!("could not open {}: {problem}", path.display()))
}

#[test]
fn a_library_publishes_a_table_the_sdk_accepts() {
    let library = open();
    let header = library.interface().header();
    assert_eq!(header.abi_major, abi::MAJOR);
    assert_eq!(header.abi_minor, abi::MINOR);
    assert_eq!(
        header.table_size,
        abi::TABLE_SIZE,
        "the fixture's CyInterface is a different size from the SDK's declaration, which means the \
         generated layout and the compiler disagree"
    );
    assert!(library.can_embed(), "the fixture exports {CREATE_SYMBOL}");
}

#[test]
fn an_entity_can_be_created_read_and_destroyed_through_the_abi() {
    let library = open();
    let engine = library
        .create_engine()
        .expect("the fixture can host in process");
    let world = engine.world().expect("the fixture's engine has a world");

    let before = world.epoch().unwrap();
    let entity = world.create_entity().unwrap();
    assert!(world.entity_alive(entity).unwrap());
    assert!(
        world.epoch().unwrap() > before,
        "creating an entity is a structural change"
    );

    world.destroy_entity(entity).unwrap();
    assert!(!world.entity_alive(entity).unwrap());
}

#[test]
fn a_stale_entity_identifier_does_not_resolve_to_a_new_entity() {
    // The generational check, end to end. Without it a handle held across a destroy would name
    // whatever now occupies the slot — the failure that is never noticed because everything still
    // works, on the wrong object.
    let library = open();
    let engine = library.create_engine().unwrap();
    let world = engine.world().unwrap();

    let first = world.create_entity().unwrap();
    world.destroy_entity(first).unwrap();
    let second = world.create_entity().unwrap();

    assert_ne!(first, second);
    assert!(!world.entity_alive(first).unwrap());
    assert!(world.entity_alive(second).unwrap());
}

#[test]
fn the_schema_reader_describes_a_world_the_editor_did_not_build() {
    // This is the entry set ABI 1.1 added and the reason it was added: before it, the ABI could
    // describe only components the caller had registered, which is the wrong side of the boundary
    // for an editor.
    let library = open();
    let engine = library.create_engine().unwrap();
    let world = engine.world().unwrap();

    let schemas = world.component_schemas().unwrap();
    assert_eq!(schemas.len(), 3);

    let transform = &schemas[TRANSFORM as usize];
    assert_eq!(transform.name, "Transform");
    assert_eq!(transform.size, 24);
    assert_eq!(transform.fields.len(), 2);
    assert_eq!(transform.fields[0].name, "position");
    assert_eq!(
        transform.fields[0].kind,
        cy_editor_core::value::ValueKind::Vec3
    );
    assert_eq!(transform.fields[1].offset, 12);

    let tag = &schemas[SELECTED as usize];
    assert!(tag.is_tag(), "a component with no column reports size zero");
    assert!(tag.fields.is_empty());
}

#[test]
fn a_field_written_through_the_abi_reads_back_exactly() {
    let library = open();
    let engine = library.create_engine().unwrap();
    let world = engine.world().unwrap();

    let transform = world.find_component("Transform").unwrap();
    let health = world.find_component("Health").unwrap();
    let entity = world.create_entity().unwrap();
    world.add_component(entity, transform).unwrap();
    world.add_component(entity, health).unwrap();

    world
        .set_field(entity, transform, 0, &Value::Vec3([1.5, -2.0, 3.25]))
        .unwrap();
    world
        .set_field(entity, health, 0, &Value::Float(0.75))
        .unwrap();

    assert_eq!(
        world.get_field(entity, transform, 0).unwrap(),
        Value::Vec3([1.5, -2.0, 3.25])
    );
    assert_eq!(
        world.get_field(entity, health, 0).unwrap(),
        Value::Float(0.75)
    );
}

#[test]
fn a_failure_arrives_as_a_reason_rather_than_a_code() {
    let library = open();
    let engine = library.create_engine().unwrap();
    let world = engine.world().unwrap();

    let problem = world.find_component("NoSuchComponent").unwrap_err();
    assert!(problem.because.contains("no component type"), "{problem}");
    assert!(
        problem.remedy.is_some(),
        "a lookup failure must say what to try"
    );

    let transform = world.find_component("Transform").unwrap();
    let entity = world.create_entity().unwrap();
    // The component was never added, so reading a field of it is a `NOT_FOUND` from the engine —
    // which must arrive as a sentence, not as the number 4.
    let problem = world.get_field(entity, transform, 0).unwrap_err();
    assert_eq!(problem.because, "the named thing does not exist");
}

#[test]
fn the_hierarchy_is_readable_and_its_order_is_the_storages() {
    let library = open();
    let engine = library.create_engine().unwrap();
    let world = engine.world().unwrap();

    let root = world.create_entity().unwrap();
    let first = world.create_entity().unwrap();
    let second = world.create_entity().unwrap();
    let third = world.create_entity().unwrap();
    for child in [first, second, third] {
        world.set_parent(child, Some(root)).unwrap();
    }

    assert_eq!(world.parent(first).unwrap(), Some(root));
    assert_eq!(world.parent(root).unwrap(), None);
    assert_eq!(world.children(root).unwrap().len(), 3);

    // Removing a child swaps the last into the gap — `cy_abi.h` states this in capitals, and a
    // hierarchy panel that assumed insertion order would reorder itself when an unrelated sibling
    // was deleted. Asserted here so that the property is documented by a test rather than by hope.
    world.set_parent(first, None).unwrap();
    let remaining = world.children(root).unwrap();
    assert_eq!(remaining.len(), 2);
    assert_eq!(
        remaining[0], third,
        "the last child was swapped into the gap"
    );
}

#[test]
fn an_entry_the_runtime_does_not_have_is_reported_by_name() {
    // The fixture implements no behaviour entries, which is exactly the shape of an engine older
    // than the SDK. The SDK must say which entry is absent rather than calling a null pointer.
    let library = open();
    let interface = library.interface();
    assert!(
        interface.table().behaviour_generation.is_none(),
        "this test is about an absent entry; the fixture must not have grown one"
    );

    // SAFETY: `behaviour_generation` takes an opaque handle it never dereferences when the entry is
    // absent — the SDK returns before calling anything.
    let error = unsafe { interface.behaviour_generation(std::ptr::null_mut()) }.unwrap_err();
    assert_eq!(
        error.to_string(),
        "the runtime's interface table has no `behaviour_generation` entry"
    );
}
