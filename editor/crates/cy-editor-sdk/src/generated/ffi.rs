// GENERATED FILE — DO NOT EDIT.
//
// Written by tools/gen/rust/sdk_gen.py from src/abi/include/cy/abi/cy_abi.h, through the
// description tools/abi/abi_describe.py produces and tools/abi/abi_gate.py diffs against
// src/abi/abi_baseline.json. Edit the C header or the generator; regenerate with
// `just build-editor --generate`, and `cargo test -p cy-editor-sdk` fails when this file is stale.

//! The C ABI as Rust sees it: `#[repr(C)]` mirrors, opaque handle tags, and the interface table.
//!
//! Nothing here is safe to use directly and nothing here is meant to be. The crate's `interface`
//! module wraps every entry, and the crate root wraps that in an API with no raw pointers in it.
//!
//! WHY THE MIRRORS ARE GENERATED HERE RATHER THAN BOUND BY bindgen. bindgen would need libclang on
//! every machine that builds the editor, would produce a different file for each host it ran on,
//! and would put a second reader of `cy_abi.h` in the repository. There is exactly one reader —
//! `tools/abi/abi_describe.py` — and the ABI gate, the committed baseline, the Swift overlay and
//! this file all come from its single parse. The layout it computes is asserted against the C
//! compiler by `src/abi/tests/test_layout.cpp` and against `rustc` by this crate's `layout` module.

#![allow(non_camel_case_types)]

/// The opaque type behind `CyEngine`.
///
/// A zero-sized member and no constructor: this type exists to give the pointer a name the compiler
/// can distinguish from every other handle's, so that passing a `CyWorld` where a `CyEngine` is
/// wanted does not compile. Nothing in Rust ever holds one of these by value.
#[repr(C)]
pub struct CyEngine_T {
    _private: [u8; 0],
}

/// `CyEngine` — an opaque handle. Null is the ABI's "no such thing".
pub type CyEngine = *mut CyEngine_T;

/// The opaque type behind `CyWorld`.
///
/// A zero-sized member and no constructor: this type exists to give the pointer a name the compiler
/// can distinguish from every other handle's, so that passing a `CyWorld` where a `CyEngine` is
/// wanted does not compile. Nothing in Rust ever holds one of these by value.
#[repr(C)]
pub struct CyWorld_T {
    _private: [u8; 0],
}

/// `CyWorld` — an opaque handle. Null is the ABI's "no such thing".
pub type CyWorld = *mut CyWorld_T;

/// The opaque type behind `CyBehaviourType`.
///
/// A zero-sized member and no constructor: this type exists to give the pointer a name the compiler
/// can distinguish from every other handle's, so that passing a `CyWorld` where a `CyEngine` is
/// wanted does not compile. Nothing in Rust ever holds one of these by value.
#[repr(C)]
pub struct CyBehaviourType_T {
    _private: [u8; 0],
}

/// `CyBehaviourType` — an opaque handle. Null is the ABI's "no such thing".
pub type CyBehaviourType = *mut CyBehaviourType_T;

/// `CyInstance`, the ABI's alias for `void*`.
pub type CyInstance = *mut ::std::ffi::c_void;

/// `CyEntity`, the ABI's alias for `uint64_t`.
pub type CyEntity = u64;

/// `CyComponentTypeId`, the ABI's alias for `uint32_t`.
pub type CyComponentTypeId = u32;

/// `CyVarPayload` — 16 bytes, 8-byte aligned.
#[derive(Clone, Copy)]
#[repr(C)]
pub union CyVarPayload {
    /// `bool` at byte 0.
    pub as_bool: bool,
    /// `int64_t` at byte 0.
    pub as_i64: i64,
    /// `double` at byte 0.
    pub as_f64: f64,
    /// `float` at byte 0.
    pub as_f32: f32,
    /// `float[4]` at byte 0.
    pub as_f32x4: [f32; 4],
    /// `CyEntity` at byte 0.
    pub as_entity: CyEntity,
    /// `const void*` at byte 0.
    pub as_bytes: *const ::std::ffi::c_void,
}

/// `CyVar` — 32 bytes, 8-byte aligned.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct CyVar {
    /// `uint32_t` at byte 0.
    pub r#type: u32,
    /// `uint32_t` at byte 4.
    pub flags: u32,
    /// `uint64_t` at byte 8.
    pub length: u64,
    /// `CyVarPayload` at byte 16.
    pub payload: CyVarPayload,
}

/// `CyFieldDesc` — 24 bytes, 8-byte aligned.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct CyFieldDesc {
    /// `uint32_t` at byte 0.
    pub struct_size: u32,
    /// `uint32_t` at byte 4.
    pub r#type: u32,
    /// `uint32_t` at byte 8.
    pub offset: u32,
    /// `uint32_t` at byte 12.
    pub size: u32,
    /// `const char*` at byte 16.
    pub name: *const ::std::ffi::c_char,
}

/// `CyComponentTypeDesc` — 32 bytes, 8-byte aligned.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct CyComponentTypeDesc {
    /// `uint32_t` at byte 0.
    pub struct_size: u32,
    /// `uint32_t` at byte 4.
    pub size: u32,
    /// `uint32_t` at byte 8.
    pub alignment: u32,
    /// `uint32_t` at byte 12.
    pub field_count: u32,
    /// `const char*` at byte 16.
    pub name: *const ::std::ffi::c_char,
    /// `const CyFieldDesc*` at byte 24.
    pub fields: *const CyFieldDesc,
}

/// `CyComponentInfo` — 24 bytes, 8-byte aligned.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct CyComponentInfo {
    /// `uint32_t` at byte 0.
    pub struct_size: u32,
    /// `uint32_t` at byte 4.
    pub size: u32,
    /// `uint32_t` at byte 8.
    pub alignment: u32,
    /// `uint32_t` at byte 12.
    pub field_count: u32,
    /// `const char*` at byte 16.
    pub name: *const ::std::ffi::c_char,
}

/// `CyChunk` — 40 bytes, 8-byte aligned.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct CyChunk {
    /// `uint32_t` at byte 0.
    pub struct_size: u32,
    /// `uint32_t` at byte 4.
    pub entity_count: u32,
    /// `const CyEntity*` at byte 8.
    pub entities: *const CyEntity,
    /// `void*` at byte 16.
    pub data: *mut ::std::ffi::c_void,
    /// `uint32_t` at byte 24.
    pub stride: u32,
    /// `uint32_t` at byte 28.
    pub archetype: u32,
    /// `uint64_t` at byte 32.
    pub epoch: u64,
}

/// `CyBehaviourVTable` — 56 bytes, 8-byte aligned.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct CyBehaviourVTable {
    /// `uint32_t` at byte 0.
    pub struct_size: u32,
    /// `uint32_t` at byte 4.
    pub schema_version: u32,
    /// `CyInstance(*)(CyEngine, CyEntity, void*)` at byte 8.
    pub create:
        Option<unsafe extern "C" fn(CyEngine, CyEntity, *mut ::std::ffi::c_void) -> CyInstance>,
    /// `void(*)(CyInstance, void*)` at byte 16.
    pub destroy: Option<unsafe extern "C" fn(CyInstance, *mut ::std::ffi::c_void)>,
    /// `void(*)(CyInstance, float, void*)` at byte 24.
    pub fixed_update: Option<unsafe extern "C" fn(CyInstance, f32, *mut ::std::ffi::c_void)>,
    /// `uint32_t(*)(CyInstance, uint8_t*, uint32_t, void*)` at byte 32.
    pub serialize:
        Option<unsafe extern "C" fn(CyInstance, *mut u8, u32, *mut ::std::ffi::c_void) -> u32>,
    /// `int32_t(*)(CyInstance, const uint8_t*, uint32_t, uint32_t, void*)` at byte 40.
    pub deserialize: Option<
        unsafe extern "C" fn(CyInstance, *const u8, u32, u32, *mut ::std::ffi::c_void) -> i32,
    >,
    /// `void*` at byte 48.
    pub user_data: *mut ::std::ffi::c_void,
}

/// `CyBorrow` — 16 bytes, 8-byte aligned.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct CyBorrow {
    /// `void*` at byte 0.
    pub data: *mut ::std::ffi::c_void,
    /// `uint64_t` at byte 8.
    pub epoch: u64,
}

/// `CyInterfaceHeader` — 16 bytes, 4-byte aligned.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct CyInterfaceHeader {
    /// `uint32_t` at byte 0.
    pub abi_major: u32,
    /// `uint32_t` at byte 4.
    pub abi_minor: u32,
    /// `uint32_t` at byte 8.
    pub abi_patch: u32,
    /// `uint32_t` at byte 12.
    pub table_size: u32,
}

/// `CyInterface` — 320 bytes, 8-byte aligned.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct CyInterface {
    /// `CyInterfaceHeader` at byte 0.
    pub header: CyInterfaceHeader,
    /// `void(*)(CyEngine, uint32_t, const char*)` at byte 16.
    pub log: Option<unsafe extern "C" fn(CyEngine, u32, *const ::std::ffi::c_char)>,
    /// `const char*(*)()` at byte 24.
    pub get_last_error: Option<unsafe extern "C" fn() -> *const ::std::ffi::c_char>,
    /// `CyResult(*)()` at byte 32.
    pub get_last_error_code: Option<unsafe extern "C" fn() -> i32>,
    /// `void(*)(CyResult, const char*)` at byte 40.
    pub set_last_error: Option<unsafe extern "C" fn(i32, *const ::std::ffi::c_char)>,
    /// `CyVar(*)(CyEngine, const char*, uint64_t)` at byte 48.
    pub var_make_string:
        Option<unsafe extern "C" fn(CyEngine, *const ::std::ffi::c_char, u64) -> CyVar>,
    /// `CyVar(*)(CyEngine, const void*, uint64_t)` at byte 56.
    pub var_make_bytes:
        Option<unsafe extern "C" fn(CyEngine, *const ::std::ffi::c_void, u64) -> CyVar>,
    /// `CyVar(*)(const CyVar*)` at byte 64.
    pub var_clone: Option<unsafe extern "C" fn(*const CyVar) -> CyVar>,
    /// `void(*)(CyVar*)` at byte 72.
    pub var_release: Option<unsafe extern "C" fn(*mut CyVar)>,
    /// `uint64_t(*)(CyEngine)` at byte 80.
    pub var_live_count: Option<unsafe extern "C" fn(CyEngine) -> u64>,
    /// `CyWorld(*)(CyEngine)` at byte 88.
    pub engine_world: Option<unsafe extern "C" fn(CyEngine) -> CyWorld>,
    /// `CyEntity(*)(CyWorld)` at byte 96.
    pub world_create_entity: Option<unsafe extern "C" fn(CyWorld) -> CyEntity>,
    /// `CyResult(*)(CyWorld, CyEntity)` at byte 104.
    pub world_destroy_entity: Option<unsafe extern "C" fn(CyWorld, CyEntity) -> i32>,
    /// `bool(*)(CyWorld, CyEntity)` at byte 112.
    pub world_entity_alive: Option<unsafe extern "C" fn(CyWorld, CyEntity) -> bool>,
    /// `uint64_t(*)(CyWorld)` at byte 120.
    pub world_epoch: Option<unsafe extern "C" fn(CyWorld) -> u64>,
    /// `CyComponentTypeId(*)(CyWorld, const CyComponentTypeDesc*)` at byte 128.
    pub world_register_component:
        Option<unsafe extern "C" fn(CyWorld, *const CyComponentTypeDesc) -> CyComponentTypeId>,
    /// `CyComponentTypeId(*)(CyWorld, const char*)` at byte 136.
    pub world_find_component:
        Option<unsafe extern "C" fn(CyWorld, *const ::std::ffi::c_char) -> CyComponentTypeId>,
    /// `CyResult(*)(CyWorld, CyEntity, CyComponentTypeId, const void*)` at byte 144.
    pub world_add_component: Option<
        unsafe extern "C" fn(
            CyWorld,
            CyEntity,
            CyComponentTypeId,
            *const ::std::ffi::c_void,
        ) -> i32,
    >,
    /// `CyResult(*)(CyWorld, CyEntity, CyComponentTypeId)` at byte 152.
    pub world_remove_component:
        Option<unsafe extern "C" fn(CyWorld, CyEntity, CyComponentTypeId) -> i32>,
    /// `bool(*)(CyWorld, CyEntity, CyComponentTypeId)` at byte 160.
    pub world_has_component:
        Option<unsafe extern "C" fn(CyWorld, CyEntity, CyComponentTypeId) -> bool>,
    /// `CyBorrow(*)(CyWorld, CyEntity, CyComponentTypeId)` at byte 168.
    pub world_borrow_component:
        Option<unsafe extern "C" fn(CyWorld, CyEntity, CyComponentTypeId) -> CyBorrow>,
    /// `bool(*)(CyWorld, CyBorrow)` at byte 176.
    pub borrow_valid: Option<unsafe extern "C" fn(CyWorld, CyBorrow) -> bool>,
    /// `CyResult(*)(CyWorld, CyEntity, CyComponentTypeId, uint32_t, CyVar*)` at byte 184.
    pub component_get_var:
        Option<unsafe extern "C" fn(CyWorld, CyEntity, CyComponentTypeId, u32, *mut CyVar) -> i32>,
    /// `CyResult(*)(CyWorld, CyEntity, CyComponentTypeId, uint32_t, const CyVar*)` at byte 192.
    pub component_set_var: Option<
        unsafe extern "C" fn(CyWorld, CyEntity, CyComponentTypeId, u32, *const CyVar) -> i32,
    >,
    /// `CyResult(*)(CyWorld, CyEntity, CyComponentTypeId, uint32_t, float*)` at byte 200.
    pub component_get_f32:
        Option<unsafe extern "C" fn(CyWorld, CyEntity, CyComponentTypeId, u32, *mut f32) -> i32>,
    /// `CyResult(*)(CyWorld, CyEntity, CyComponentTypeId, uint32_t, float)` at byte 208.
    pub component_set_f32:
        Option<unsafe extern "C" fn(CyWorld, CyEntity, CyComponentTypeId, u32, f32) -> i32>,
    /// `CyResult(*)(CyWorld, CyEntity, CyComponentTypeId, uint32_t, float*)` at byte 216.
    pub component_get_vec3:
        Option<unsafe extern "C" fn(CyWorld, CyEntity, CyComponentTypeId, u32, *mut f32) -> i32>,
    /// `CyResult(*)(CyWorld, CyEntity, CyComponentTypeId, uint32_t, const float*)` at byte 224.
    pub component_set_vec3:
        Option<unsafe extern "C" fn(CyWorld, CyEntity, CyComponentTypeId, u32, *const f32) -> i32>,
    /// `CyBehaviourType(*)(CyEngine, const char*, const CyBehaviourVTable*)` at byte 232.
    pub register_behaviour: Option<
        unsafe extern "C" fn(
            CyEngine,
            *const ::std::ffi::c_char,
            *const CyBehaviourVTable,
        ) -> CyBehaviourType,
    >,
    /// `CyBehaviourType(*)(CyEngine, const char*)` at byte 240.
    pub find_behaviour:
        Option<unsafe extern "C" fn(CyEngine, *const ::std::ffi::c_char) -> CyBehaviourType>,
    /// `uint32_t(*)(CyBehaviourType)` at byte 248.
    pub behaviour_generation: Option<unsafe extern "C" fn(CyBehaviourType) -> u32>,
    /// `uint32_t(*)(CyWorld)` at byte 256.
    pub world_component_count: Option<unsafe extern "C" fn(CyWorld) -> u32>,
    /// `CyResult(*)(CyWorld, CyComponentTypeId, CyComponentInfo*)` at byte 264.
    pub world_component_info:
        Option<unsafe extern "C" fn(CyWorld, CyComponentTypeId, *mut CyComponentInfo) -> i32>,
    /// `CyResult(*)(CyWorld, CyComponentTypeId, uint32_t, CyFieldDesc*)` at byte 272.
    pub world_component_field:
        Option<unsafe extern "C" fn(CyWorld, CyComponentTypeId, u32, *mut CyFieldDesc) -> i32>,
    /// `CyEntity(*)(CyWorld, CyEntity)` at byte 280.
    pub world_parent: Option<unsafe extern "C" fn(CyWorld, CyEntity) -> CyEntity>,
    /// `CyResult(*)(CyWorld, CyEntity, CyEntity)` at byte 288.
    pub world_set_parent: Option<unsafe extern "C" fn(CyWorld, CyEntity, CyEntity) -> i32>,
    /// `uint32_t(*)(CyWorld, CyEntity)` at byte 296.
    pub world_child_count: Option<unsafe extern "C" fn(CyWorld, CyEntity) -> u32>,
    /// `CyEntity(*)(CyWorld, CyEntity, uint32_t)` at byte 304.
    pub world_child: Option<unsafe extern "C" fn(CyWorld, CyEntity, u32) -> CyEntity>,
    /// `CyResult(*)(CyWorld, CyComponentTypeId, CyChunk*, uint32_t, uint32_t*)` at byte 312.
    pub world_chunks: Option<
        unsafe extern "C" fn(CyWorld, CyComponentTypeId, *mut CyChunk, u32, *mut u32) -> i32,
    >,
}

/// `CyModuleInit` — 40 bytes, 8-byte aligned.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct CyModuleInit {
    /// `uint32_t` at byte 0.
    pub struct_size: u32,
    /// `uint32_t` at byte 4.
    pub abi_major: u32,
    /// `uint32_t` at byte 8.
    pub abi_minor: u32,
    /// `uint32_t` at byte 12.
    pub reserved: u32,
    /// `void(*)(CyEngine, CyInitLevel, void*)` at byte 16.
    pub initialize: Option<unsafe extern "C" fn(CyEngine, i32, *mut ::std::ffi::c_void)>,
    /// `void(*)(CyEngine, CyInitLevel, void*)` at byte 24.
    pub shutdown: Option<unsafe extern "C" fn(CyEngine, i32, *mut ::std::ffi::c_void)>,
    /// `void*` at byte 32.
    pub user_data: *mut ::std::ffi::c_void,
}

/// `CyModuleEntryFn`, an entry point the engine looks up in a module image.
pub type CyModuleEntryFn =
    Option<unsafe extern "C" fn(*const CyInterface, CyEngine, *mut CyModuleInit) -> bool>;

/// `CyModuleShutdownFn`, an entry point the engine looks up in a module image.
pub type CyModuleShutdownFn = Option<unsafe extern "C" fn()>;

impl CyInterface {
    /// A table with a zeroed header and no entries at all.
    ///
    /// The starting point for a host that implements a subset — a test double, or a
    /// runtime older than this SDK. Every absent entry is a `None`, which the SDK
    /// reports by name rather than jumping to zero.
    pub const EMPTY: CyInterface = CyInterface {
        header: CyInterfaceHeader {
            abi_major: 0,
            abi_minor: 0,
            abi_patch: 0,
            table_size: 0,
        },
        log: None,
        get_last_error: None,
        get_last_error_code: None,
        set_last_error: None,
        var_make_string: None,
        var_make_bytes: None,
        var_clone: None,
        var_release: None,
        var_live_count: None,
        engine_world: None,
        world_create_entity: None,
        world_destroy_entity: None,
        world_entity_alive: None,
        world_epoch: None,
        world_register_component: None,
        world_find_component: None,
        world_add_component: None,
        world_remove_component: None,
        world_has_component: None,
        world_borrow_component: None,
        borrow_valid: None,
        component_get_var: None,
        component_set_var: None,
        component_get_f32: None,
        component_set_f32: None,
        component_get_vec3: None,
        component_set_vec3: None,
        register_behaviour: None,
        find_behaviour: None,
        behaviour_generation: None,
        world_component_count: None,
        world_component_info: None,
        world_component_field: None,
        world_parent: None,
        world_set_parent: None,
        world_child_count: None,
        world_child: None,
        world_chunks: None,
    };
}
