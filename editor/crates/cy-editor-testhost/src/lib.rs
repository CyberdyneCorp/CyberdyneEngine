//! A runtime image that is not the engine. Task 2.2's test double, and task 2.3's proof.
//!
//! Exports the one symbol the C ABI defines — `cy_get_interface` — plus the two the editor SDK's
//! embedded mode looks for, and implements enough of `CyInterface` for the SDK's whole path to be
//! exercised for real: `dlopen`, `dlsym`, the header check, entity and hierarchy operations, the
//! component schema reader, and `CyVar` marshalling in both directions.
//!
//! `editor-rust-application` permits exactly this: "Where a test requires an engine, it SHALL use a
//! hosted or embedded runtime through the SDK, or **a test double implementing the same
//! interfaces**." This is the double, and it implements the interface rather than imitating it —
//! the table it publishes is the generated `#[repr(C)]` `CyInterface`, so a layout or
//! calling-convention mistake fails here rather than against the real engine.
//!
//! --- WHAT IS DELIBERATELY MISSING -----------------------------------------------------------------
//!
//! Behaviours, chunks, borrows, the typed fast paths, and every entry an editor does not call. They
//! are `None` in the table, and the SDK reports a `Missing` naming the entry — which is exactly what
//! a runtime older than the SDK produces, and is therefore worth having a fixture that produces it.
//!
//! --- THREADING ---------------------------------------------------------------------------------
//!
//! None. The C ABI's engine is single-threaded and the SDK's `World` is neither `Send` nor `Sync`
//! for that reason; this fixture takes the same position and does no locking. A test that called it
//! from two threads would be testing something the ABI does not offer.

#![allow(
    unsafe_code,
    reason = "this crate implements a C ABI, which is the definition of an audited interoperation \
              module; every unsafe block below states its contract"
)]

use std::ffi::{CStr, CString, c_char, c_void};

use cy_editor_sdk::abi;
use cy_editor_sdk::generated::enums::{Status, VarType};
use cy_editor_sdk::generated::ffi;

mod world;

pub use world::{HEALTH, SELECTED, TRANSFORM, TestWorld};

/// The library file this crate builds to, as the test harness will find it.
///
/// Searched rather than computed, because Cargo puts a `cdylib` in different places depending on
/// what asked for it: `cargo build` hard-links it into `<target>/<profile>/`, and `cargo test`
/// leaves it in `<target>/<profile>/deps/`. A path computed for one of those is a test that passes
/// under `just build-editor` and fails under `cargo test`, which is the worst of the two failures
/// because it looks like the library is broken.
///
/// Walking from `current_exe` rather than reading `CARGO_TARGET_DIR` keeps this correct under a
/// custom target directory, under `--target`, and under all four profiles — none of which a
/// hard-coded path survives, and the milestone builds in four of them.
///
/// # Panics
///
/// When the library is in neither place, naming both — which means the fixture was not built, and
/// no amount of searching will find it.
#[must_use]
pub fn library_path() -> std::path::PathBuf {
    let file = format!("{DLL_PREFIX}cy_editor_testhost{DLL_SUFFIX}");
    let executable = std::env::current_exe().expect("a test binary knows its own path");
    let directory = executable
        .parent()
        .expect("the test binary is inside a directory")
        .to_path_buf();

    let mut candidates = vec![directory.join(&file)];
    if directory.ends_with("deps") {
        if let Some(profile) = directory.parent() {
            candidates.push(profile.join(&file));
        }
    } else {
        candidates.push(directory.join("deps").join(&file));
    }

    for candidate in &candidates {
        if candidate.is_file() {
            return candidate.clone();
        }
    }
    panic!(
        "the test host library was not built. Looked for:\n{}",
        candidates
            .iter()
            .map(|path| format!("  {}", path.display()))
            .collect::<Vec<_>>()
            .join("\n")
    )
}

#[cfg(target_os = "windows")]
const DLL_PREFIX: &str = "";
#[cfg(not(target_os = "windows"))]
const DLL_PREFIX: &str = "lib";

#[cfg(target_os = "windows")]
const DLL_SUFFIX: &str = ".dll";
#[cfg(target_os = "macos")]
const DLL_SUFFIX: &str = ".dylib";
#[cfg(all(unix, not(target_os = "macos")))]
const DLL_SUFFIX: &str = ".so";

/// The table this image publishes.
///
/// Every entry the editor SDK calls, and `None` for the rest. `..ffi::CyInterface::EMPTY` is
/// generated, so appending an entry to the ABI leaves this correct rather than failing to compile
/// with a field nobody has heard of — and the SDK reports the new entry as `Missing` until this
/// fixture implements it, which is the honest state of affairs.
static TABLE: ffi::CyInterface = ffi::CyInterface {
    header: ffi::CyInterfaceHeader {
        abi_major: abi::MAJOR,
        abi_minor: abi::MINOR,
        abi_patch: abi::PATCH,
        table_size: abi::TABLE_SIZE,
    },
    get_last_error: Some(get_last_error),
    var_release: Some(var_release),
    engine_world: Some(engine_world),
    world_create_entity: Some(world_create_entity),
    world_destroy_entity: Some(world_destroy_entity),
    world_entity_alive: Some(world_entity_alive),
    world_epoch: Some(world_epoch),
    world_find_component: Some(world_find_component),
    world_add_component: Some(world_add_component),
    world_remove_component: Some(world_remove_component),
    world_has_component: Some(world_has_component),
    component_get_var: Some(component_get_var),
    component_set_var: Some(component_set_var),
    world_component_count: Some(world_component_count),
    world_component_info: Some(world_component_info),
    world_component_field: Some(world_component_field),
    world_parent: Some(world_parent),
    world_set_parent: Some(world_set_parent),
    world_child_count: Some(world_child_count),
    world_child: Some(world_child),
    ..ffi::CyInterface::EMPTY
};

/// The C ABI's one exported symbol.
///
/// # Safety
///
/// Called by a dynamic loader with two integers; nothing is dereferenced.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn cy_get_interface(major: u32, minor: u32) -> *const ffi::CyInterface {
    // The engine's own rule, implemented rather than approximated: a different major is refused,
    // and a minor ABOVE this image's is refused because the caller would read entries that are not
    // there. A minor below is served — that is what append-only means from the newer side.
    if major != abi::MAJOR || minor > abi::MINOR {
        return std::ptr::null();
    }
    &raw const TABLE
}

/// Create an engine in the caller's process. This SDK's convention, not the ABI's.
///
/// # Safety
///
/// Returns a handle the caller must pass to [`cy_editor_embedded_destroy`] exactly once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn cy_editor_embedded_create() -> ffi::CyEngine {
    let world = Box::new(TestWorld::new());
    Box::into_raw(world).cast::<ffi::CyEngine_T>()
}

/// Destroy an engine created by [`cy_editor_embedded_create`].
///
/// # Safety
///
/// `engine` must be a handle that function returned and that has not already been destroyed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn cy_editor_embedded_destroy(engine: ffi::CyEngine) {
    if engine.is_null() {
        return;
    }
    // SAFETY: the caller's contract is that this is a handle from `cy_editor_embedded_create`, which
    // produced it with `Box::into_raw` on exactly this type.
    drop(unsafe { Box::from_raw(engine.cast::<TestWorld>()) });
}

/// Reconstitute the world behind a handle.
///
/// # Safety
///
/// `handle` must be a live engine or world handle this image issued. Both are the same pointer:
/// this fixture's engine *is* its world, which is a simplification the ABI permits because the two
/// are opaque to the caller.
unsafe fn world_of<'a>(handle: *mut c_void) -> Option<&'a mut TestWorld> {
    if handle.is_null() {
        return None;
    }
    // SAFETY: the caller's contract. The returned borrow is confined to one ABI call, and the ABI
    // is single-threaded, so no two of these are live at once.
    Some(unsafe { &mut *handle.cast::<TestWorld>() })
}

unsafe extern "C" fn engine_world(engine: ffi::CyEngine) -> ffi::CyWorld {
    engine.cast::<ffi::CyWorld_T>()
}

unsafe extern "C" fn get_last_error() -> *const c_char {
    c"".as_ptr()
}

unsafe extern "C" fn var_release(var: *mut ffi::CyVar) {
    if var.is_null() {
        return;
    }
    // SAFETY: the ABI's contract is that `var` points at a `CyVar` the caller owns. This image
    // never returns an owned var — every value it produces is inline — so there is nothing to free;
    // clearing to nil is what makes a double release a no-op, which the header requires.
    unsafe {
        (*var).r#type = VarType::Nil.as_raw();
        (*var).flags = 0;
        (*var).length = 0;
    }
}

unsafe extern "C" fn world_epoch(world: ffi::CyWorld) -> u64 {
    // SAFETY: `world` is a handle this image issued; see `world_of`.
    unsafe { world_of(world.cast::<c_void>()) }.map_or(0, |world| world.epoch())
}

unsafe extern "C" fn world_create_entity(world: ffi::CyWorld) -> ffi::CyEntity {
    // SAFETY: as above.
    unsafe { world_of(world.cast::<c_void>()) }.map_or(0, TestWorld::create_entity)
}

unsafe extern "C" fn world_destroy_entity(world: ffi::CyWorld, entity: ffi::CyEntity) -> i32 {
    // SAFETY: as above.
    let Some(world) = (unsafe { world_of(world.cast::<c_void>()) }) else {
        return Status::InvalidArgument.as_raw();
    };
    status(world.destroy_entity(entity))
}

unsafe extern "C" fn world_entity_alive(world: ffi::CyWorld, entity: ffi::CyEntity) -> bool {
    // SAFETY: as above.
    unsafe { world_of(world.cast::<c_void>()) }.is_some_and(|world| world.is_alive(entity))
}

unsafe extern "C" fn world_find_component(
    world: ffi::CyWorld,
    name: *const c_char,
) -> ffi::CyComponentTypeId {
    // SAFETY: as above, and `name` is a NUL-terminated string borrowed for this call, which is the
    // ABI's default argument rule.
    let Some(world) = (unsafe { world_of(world.cast::<c_void>()) }) else {
        return u32::MAX;
    };
    if name.is_null() {
        return u32::MAX;
    }
    let Ok(name) = (unsafe { CStr::from_ptr(name) }).to_str() else {
        return u32::MAX;
    };
    world.find_component(name).unwrap_or(u32::MAX)
}

unsafe extern "C" fn world_add_component(
    world: ffi::CyWorld,
    entity: ffi::CyEntity,
    component: ffi::CyComponentTypeId,
    _initial: *const c_void,
) -> i32 {
    // SAFETY: as above. `_initial` is ignored: this image zero-initialises, which the ABI permits
    // for a null pointer and which is all the SDK asks for.
    let Some(world) = (unsafe { world_of(world.cast::<c_void>()) }) else {
        return Status::InvalidArgument.as_raw();
    };
    status(world.add_component(entity, component))
}

unsafe extern "C" fn world_remove_component(
    world: ffi::CyWorld,
    entity: ffi::CyEntity,
    component: ffi::CyComponentTypeId,
) -> i32 {
    // SAFETY: as above.
    let Some(world) = (unsafe { world_of(world.cast::<c_void>()) }) else {
        return Status::InvalidArgument.as_raw();
    };
    status(world.remove_component(entity, component))
}

unsafe extern "C" fn world_has_component(
    world: ffi::CyWorld,
    entity: ffi::CyEntity,
    component: ffi::CyComponentTypeId,
) -> bool {
    // SAFETY: as above.
    unsafe { world_of(world.cast::<c_void>()) }
        .is_some_and(|world| world.has_component(entity, component))
}

unsafe extern "C" fn component_get_var(
    world: ffi::CyWorld,
    entity: ffi::CyEntity,
    component: ffi::CyComponentTypeId,
    field: u32,
    out_value: *mut ffi::CyVar,
) -> i32 {
    // SAFETY: as above, and `out_value` is a writable `CyVar` the caller supplied for this call.
    let Some(world) = (unsafe { world_of(world.cast::<c_void>()) }) else {
        return Status::InvalidArgument.as_raw();
    };
    if out_value.is_null() {
        return Status::InvalidArgument.as_raw();
    }
    match world.get_field(entity, component, field) {
        Ok(var) => {
            unsafe { out_value.write(var) };
            Status::Ok.as_raw()
        }
        Err(status) => status.as_raw(),
    }
}

unsafe extern "C" fn component_set_var(
    world: ffi::CyWorld,
    entity: ffi::CyEntity,
    component: ffi::CyComponentTypeId,
    field: u32,
    value: *const ffi::CyVar,
) -> i32 {
    // SAFETY: as above, and `value` is borrowed for the duration of this call.
    let Some(world) = (unsafe { world_of(world.cast::<c_void>()) }) else {
        return Status::InvalidArgument.as_raw();
    };
    if value.is_null() {
        return Status::InvalidArgument.as_raw();
    }
    status(world.set_field(entity, component, field, unsafe { &*value }))
}

unsafe extern "C" fn world_component_count(world: ffi::CyWorld) -> u32 {
    // SAFETY: as above.
    unsafe { world_of(world.cast::<c_void>()) }.map_or(0, |world| world.component_count())
}

unsafe extern "C" fn world_component_info(
    world: ffi::CyWorld,
    component: ffi::CyComponentTypeId,
    out_info: *mut ffi::CyComponentInfo,
) -> i32 {
    // SAFETY: as above, and `out_info` is a writable struct the caller supplied.
    let Some(world) = (unsafe { world_of(world.cast::<c_void>()) }) else {
        return Status::InvalidArgument.as_raw();
    };
    if out_info.is_null() {
        return Status::InvalidArgument.as_raw();
    }
    match world.component_info(component) {
        Ok(info) => {
            unsafe { out_info.write(info) };
            Status::Ok.as_raw()
        }
        Err(status) => status.as_raw(),
    }
}

unsafe extern "C" fn world_component_field(
    world: ffi::CyWorld,
    component: ffi::CyComponentTypeId,
    field: u32,
    out_field: *mut ffi::CyFieldDesc,
) -> i32 {
    // SAFETY: as above.
    let Some(world) = (unsafe { world_of(world.cast::<c_void>()) }) else {
        return Status::InvalidArgument.as_raw();
    };
    if out_field.is_null() {
        return Status::InvalidArgument.as_raw();
    }
    match world.component_field(component, field) {
        Ok(descriptor) => {
            unsafe { out_field.write(descriptor) };
            Status::Ok.as_raw()
        }
        Err(status) => status.as_raw(),
    }
}

unsafe extern "C" fn world_parent(world: ffi::CyWorld, entity: ffi::CyEntity) -> ffi::CyEntity {
    // SAFETY: as above.
    unsafe { world_of(world.cast::<c_void>()) }.map_or(0, |world| world.parent(entity))
}

unsafe extern "C" fn world_set_parent(
    world: ffi::CyWorld,
    child: ffi::CyEntity,
    parent: ffi::CyEntity,
) -> i32 {
    // SAFETY: as above.
    let Some(world) = (unsafe { world_of(world.cast::<c_void>()) }) else {
        return Status::InvalidArgument.as_raw();
    };
    status(world.set_parent(child, parent))
}

unsafe extern "C" fn world_child_count(world: ffi::CyWorld, entity: ffi::CyEntity) -> u32 {
    // SAFETY: as above.
    unsafe { world_of(world.cast::<c_void>()) }.map_or(0, |world| world.child_count(entity))
}

unsafe extern "C" fn world_child(
    world: ffi::CyWorld,
    entity: ffi::CyEntity,
    index: u32,
) -> ffi::CyEntity {
    // SAFETY: as above.
    unsafe { world_of(world.cast::<c_void>()) }.map_or(0, |world| world.child(entity, index))
}

/// The C representation of a result this fixture produced.
fn status(result: std::result::Result<(), Status>) -> i32 {
    match result {
        Ok(()) => Status::Ok.as_raw(),
        Err(status) => status.as_raw(),
    }
}

/// A NUL-terminated copy of `name`, leaked.
///
/// The ABI requires a component or field name to outlive its registration — `cy_abi.h` says so at
/// the descriptor — and this fixture's names are fixed at construction, so leaking a handful of
/// small strings for the process's lifetime is the shape the contract asks for rather than a defect.
pub(crate) fn static_name(name: &str) -> *const c_char {
    let owned = CString::new(name).expect("a fixture's component names contain no NUL");
    Box::leak(owned.into_boxed_c_str()).as_ptr()
}
