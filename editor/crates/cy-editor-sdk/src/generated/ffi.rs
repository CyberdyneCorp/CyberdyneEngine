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

/// The opaque type behind `CyServiceSession`.
///
/// A zero-sized member and no constructor: this type exists to give the pointer a name the compiler
/// can distinguish from every other handle's, so that passing a `CyWorld` where a `CyEngine` is
/// wanted does not compile. Nothing in Rust ever holds one of these by value.
#[repr(C)]
pub struct CyServiceSession_T {
    _private: [u8; 0],
}

/// `CyServiceSession` — an opaque handle. Null is the ABI's "no such thing".
pub type CyServiceSession = *mut CyServiceSession_T;

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

/// `CyServiceRequest` — 40 bytes, 8-byte aligned.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct CyServiceRequest {
    /// `uint32_t` at byte 0.
    pub struct_size: u32,
    /// `uint32_t` at byte 4.
    pub schema_version: u32,
    /// `uint64_t` at byte 8.
    pub request_id: u64,
    /// `const char*` at byte 16.
    pub operation: *const ::std::ffi::c_char,
    /// `const uint8_t*` at byte 24.
    pub payload: *const u8,
    /// `uint64_t` at byte 32.
    pub payload_size: u64,
}

/// `CyServiceEvent` — 40 bytes, 8-byte aligned.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct CyServiceEvent {
    /// `uint32_t` at byte 0.
    pub struct_size: u32,
    /// `uint32_t` at byte 4.
    pub kind: u32,
    /// `uint64_t` at byte 8.
    pub request_id: u64,
    /// `uint32_t` at byte 16.
    pub schema_version: u32,
    /// `uint32_t` at byte 20.
    pub reserved: u32,
    /// `const uint8_t*` at byte 24.
    pub payload: *const u8,
    /// `uint64_t` at byte 32.
    pub payload_size: u64,
}

/// `CyBehaviourVTable` — 64 bytes, 8-byte aligned.
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
    /// `void(*)(CyInstance, float, void*)` at byte 56.
    pub frame_update: Option<unsafe extern "C" fn(CyInstance, f32, *mut ::std::ffi::c_void)>,
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

/// `CyPose` — 28 bytes, 4-byte aligned.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct CyPose {
    /// `float[3]` at byte 0.
    pub position: [f32; 3],
    /// `float[4]` at byte 12.
    pub rotation: [f32; 4],
}

/// `CyRay` — 28 bytes, 4-byte aligned.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct CyRay {
    /// `float[3]` at byte 0.
    pub origin: [f32; 3],
    /// `float[3]` at byte 12.
    pub direction: [f32; 3],
    /// `float` at byte 24.
    pub max_distance: f32,
}

/// `CyTime` — 48 bytes, 8-byte aligned.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct CyTime {
    /// `uint32_t` at byte 0.
    pub struct_size: u32,
    /// `uint32_t` at byte 4.
    pub phase: u32,
    /// `uint64_t` at byte 8.
    pub tick: u64,
    /// `double` at byte 16.
    pub fixed_delta: f64,
    /// `double` at byte 24.
    pub frame_delta: f64,
    /// `double` at byte 32.
    pub interpolation: f64,
    /// `uint32_t` at byte 40.
    pub flags: u32,
    /// `uint32_t` at byte 44.
    pub reserved: u32,
}

/// `CyInputAction`, the ABI's alias for `uint32_t`.
pub type CyInputAction = u32;

/// `CyInputContext`, the ABI's alias for `uint64_t`.
pub type CyInputContext = u64;

/// `CyInputActionState` — 32 bytes, 8-byte aligned.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct CyInputActionState {
    /// `uint32_t` at byte 0.
    pub struct_size: u32,
    /// `uint32_t` at byte 4.
    pub flags: u32,
    /// `float[3]` at byte 8.
    pub value: [f32; 3],
    /// `uint16_t` at byte 20.
    pub press_count: u16,
    /// `uint16_t` at byte 22.
    pub release_count: u16,
    /// `uint64_t` at byte 24.
    pub tick: u64,
}

/// `CyInputPointer` — 48 bytes, 4-byte aligned.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct CyInputPointer {
    /// `uint32_t` at byte 0.
    pub struct_size: u32,
    /// `uint32_t` at byte 4.
    pub flags: u32,
    /// `uint32_t` at byte 8.
    pub buttons: u32,
    /// `uint32_t` at byte 12.
    pub buttons_pressed: u32,
    /// `uint32_t` at byte 16.
    pub buttons_released: u32,
    /// `float[2]` at byte 20.
    pub position: [f32; 2],
    /// `float[2]` at byte 28.
    pub delta: [f32; 2],
    /// `float[2]` at byte 36.
    pub wheel: [f32; 2],
    /// `uint32_t` at byte 44.
    pub reserved: u32,
}

/// `CyCamera`, the ABI's alias for `uint64_t`.
pub type CyCamera = u64;

/// `CyCameraView` — 68 bytes, 4-byte aligned.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct CyCameraView {
    /// `uint32_t` at byte 0.
    pub struct_size: u32,
    /// `uint32_t` at byte 4.
    pub flags: u32,
    /// `CyPose` at byte 8.
    pub pose: CyPose,
    /// `float` at byte 36.
    pub vertical_fov: f32,
    /// `float` at byte 40.
    pub ortho_height: f32,
    /// `float` at byte 44.
    pub near_plane: f32,
    /// `float` at byte 48.
    pub far_plane: f32,
    /// `float[4]` at byte 52.
    pub viewport: [f32; 4],
}

/// `CyScreenPoint` — 16 bytes, 4-byte aligned.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct CyScreenPoint {
    /// `float[2]` at byte 0.
    pub position: [f32; 2],
    /// `float` at byte 8.
    pub depth: f32,
    /// `uint32_t` at byte 12.
    pub flags: u32,
}

/// `CyCameraTarget` — 48 bytes, 8-byte aligned.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct CyCameraTarget {
    /// `uint32_t` at byte 0.
    pub struct_size: u32,
    /// `uint32_t` at byte 4.
    pub flags: u32,
    /// `CyEntity` at byte 8.
    pub entity: CyEntity,
    /// `float[3]` at byte 16.
    pub position: [f32; 3],
    /// `float` at byte 28.
    pub yaw: f32,
    /// `float` at byte 32.
    pub pitch: f32,
    /// `float` at byte 36.
    pub distance: f32,
    /// `float` at byte 40.
    pub blend_seconds: f32,
    /// `uint32_t` at byte 44.
    pub reserved: u32,
}

/// `CyShape` — 24 bytes, 4-byte aligned.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct CyShape {
    /// `uint32_t` at byte 0.
    pub kind: u32,
    /// `float` at byte 4.
    pub radius: f32,
    /// `float` at byte 8.
    pub half_height: f32,
    /// `float[3]` at byte 12.
    pub half_extents: [f32; 3],
}

/// `CyQueryFilter` — 32 bytes, 8-byte aligned.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct CyQueryFilter {
    /// `uint32_t` at byte 0.
    pub struct_size: u32,
    /// `uint32_t` at byte 4.
    pub layer: u32,
    /// `uint32_t` at byte 8.
    pub mask: u32,
    /// `uint32_t` at byte 12.
    pub flags: u32,
    /// `const CyEntity*` at byte 16.
    pub ignore: *const CyEntity,
    /// `uint32_t` at byte 24.
    pub ignore_count: u32,
    /// `uint32_t` at byte 28.
    pub reserved: u32,
}

/// `CyPhysicsHit` — 48 bytes, 8-byte aligned.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct CyPhysicsHit {
    /// `uint32_t` at byte 0.
    pub flags: u32,
    /// `uint32_t` at byte 4.
    pub reserved: u32,
    /// `CyEntity` at byte 8.
    pub entity: CyEntity,
    /// `float[3]` at byte 16.
    pub point: [f32; 3],
    /// `float[3]` at byte 28.
    pub normal: [f32; 3],
    /// `float` at byte 40.
    pub distance: f32,
    /// `float` at byte 44.
    pub fraction: f32,
}

/// `CyNavQuery`, the ABI's alias for `uint64_t`.
pub type CyNavQuery = u64;

/// `CyNavPathRequest` — 64 bytes, 8-byte aligned.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct CyNavPathRequest {
    /// `uint32_t` at byte 0.
    pub struct_size: u32,
    /// `uint32_t` at byte 4.
    pub world: u32,
    /// `float[3]` at byte 8.
    pub start: [f32; 3],
    /// `float[3]` at byte 20.
    pub end: [f32; 3],
    /// `float[3]` at byte 32.
    pub extents: [f32; 3],
    /// `uint32_t` at byte 44.
    pub node_budget: u32,
    /// `uint64_t` at byte 48.
    pub area_mask: u64,
    /// `uint64_t` at byte 56.
    pub capabilities: u64,
}

/// `CyNavPathResult` — 24 bytes, 4-byte aligned.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct CyNavPathResult {
    /// `uint32_t` at byte 0.
    pub struct_size: u32,
    /// `uint32_t` at byte 4.
    pub flags: u32,
    /// `uint32_t` at byte 8.
    pub point_count: u32,
    /// `uint32_t` at byte 12.
    pub state: u32,
    /// `float` at byte 16.
    pub cost: f32,
    /// `float` at byte 20.
    pub length: f32,
}

/// `CyNavAgentParams` — 48 bytes, 8-byte aligned.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct CyNavAgentParams {
    /// `uint32_t` at byte 0.
    pub struct_size: u32,
    /// `uint32_t` at byte 4.
    pub world: u32,
    /// `float` at byte 8.
    pub radius: f32,
    /// `float` at byte 12.
    pub height: f32,
    /// `float` at byte 16.
    pub max_speed: f32,
    /// `float` at byte 20.
    pub max_acceleration: f32,
    /// `float` at byte 24.
    pub arrival_distance: f32,
    /// `uint32_t` at byte 28.
    pub priority: u32,
    /// `uint64_t` at byte 32.
    pub area_mask: u64,
    /// `uint64_t` at byte 40.
    pub capabilities: u64,
}

/// `CyNavAgentState` — 56 bytes, 4-byte aligned.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct CyNavAgentState {
    /// `uint32_t` at byte 0.
    pub struct_size: u32,
    /// `uint32_t` at byte 4.
    pub status: u32,
    /// `uint32_t` at byte 8.
    pub flags: u32,
    /// `uint32_t` at byte 12.
    pub reserved: u32,
    /// `float[3]` at byte 16.
    pub position: [f32; 3],
    /// `float[3]` at byte 28.
    pub velocity: [f32; 3],
    /// `float[3]` at byte 40.
    pub target: [f32; 3],
    /// `float` at byte 52.
    pub remaining_distance: f32,
}

/// `CyAudioCue`, the ABI's alias for `uint64_t`.
pub type CyAudioCue = u64;

/// `CyAudioBus`, the ABI's alias for `uint64_t`.
pub type CyAudioBus = u64;

/// `CyAudioVoice`, the ABI's alias for `uint64_t`.
pub type CyAudioVoice = u64;

/// `CyAudioPlay` — 56 bytes, 8-byte aligned.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct CyAudioPlay {
    /// `uint32_t` at byte 0.
    pub struct_size: u32,
    /// `uint32_t` at byte 4.
    pub flags: u32,
    /// `CyAudioCue` at byte 8.
    pub cue: CyAudioCue,
    /// `CyEntity` at byte 16.
    pub attach_to: CyEntity,
    /// `CyAudioBus` at byte 24.
    pub bus: CyAudioBus,
    /// `float[3]` at byte 32.
    pub position: [f32; 3],
    /// `float` at byte 44.
    pub volume: f32,
    /// `float` at byte 48.
    pub pitch: f32,
    /// `float` at byte 52.
    pub fade_in_seconds: f32,
}

/// `CyPrefab`, the ABI's alias for `uint64_t`.
pub type CyPrefab = u64;

/// `CySpawnParams` — 56 bytes, 8-byte aligned.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct CySpawnParams {
    /// `uint32_t` at byte 0.
    pub struct_size: u32,
    /// `uint32_t` at byte 4.
    pub flags: u32,
    /// `CyEntity` at byte 8.
    pub parent: CyEntity,
    /// `CyPose` at byte 16.
    pub pose: CyPose,
    /// `float[3]` at byte 44.
    pub scale: [f32; 3],
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

/// `CyInterface` — 664 bytes, 8-byte aligned.
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
    /// `CyResult(*)(CyEngine, CyServiceSession*)` at byte 320.
    pub service_open: Option<unsafe extern "C" fn(CyEngine, *mut CyServiceSession) -> i32>,
    /// `void(*)(CyEngine, CyServiceSession)` at byte 328.
    pub service_close: Option<unsafe extern "C" fn(CyEngine, CyServiceSession)>,
    /// `CyResult(*)(CyEngine, CyServiceSession, const CyServiceRequest*)` at byte 336.
    pub service_submit:
        Option<unsafe extern "C" fn(CyEngine, CyServiceSession, *const CyServiceRequest) -> i32>,
    /// `CyResult(*)(CyEngine, CyServiceSession, uint64_t)` at byte 344.
    pub service_cancel: Option<unsafe extern "C" fn(CyEngine, CyServiceSession, u64) -> i32>,
    /// `CyResult(*)(CyEngine, CyServiceSession, CyServiceEvent*, bool*)` at byte 352.
    pub service_poll: Option<
        unsafe extern "C" fn(CyEngine, CyServiceSession, *mut CyServiceEvent, *mut bool) -> i32,
    >,
    /// `CyResult(*)(CyEngine, CyTime*)` at byte 360.
    pub time_get: Option<unsafe extern "C" fn(CyEngine, *mut CyTime) -> i32>,
    /// `CyResult(*)(CyEngine, const char*, CyInputAction*)` at byte 368.
    pub input_find_action: Option<
        unsafe extern "C" fn(CyEngine, *const ::std::ffi::c_char, *mut CyInputAction) -> i32,
    >,
    /// `CyResult(*)(CyEngine, uint32_t, CyInputAction, CyInputActionState*)` at byte 376.
    pub input_action_state:
        Option<unsafe extern "C" fn(CyEngine, u32, CyInputAction, *mut CyInputActionState) -> i32>,
    /// `CyResult(*)(CyEngine, uint32_t, const char*, CyInputActionState*)` at byte 384.
    pub input_action_state_by_name: Option<
        unsafe extern "C" fn(
            CyEngine,
            u32,
            *const ::std::ffi::c_char,
            *mut CyInputActionState,
        ) -> i32,
    >,
    /// `CyResult(*)(CyEngine, uint32_t, CyInputPointer*)` at byte 392.
    pub input_pointer: Option<unsafe extern "C" fn(CyEngine, u32, *mut CyInputPointer) -> i32>,
    /// `CyResult(*)(CyEngine, uint32_t, uint32_t*)` at byte 400.
    pub input_modifiers: Option<unsafe extern "C" fn(CyEngine, u32, *mut u32) -> i32>,
    /// `CyResult(*)(CyEngine, const char*, CyInputContext*)` at byte 408.
    pub input_find_context: Option<
        unsafe extern "C" fn(CyEngine, *const ::std::ffi::c_char, *mut CyInputContext) -> i32,
    >,
    /// `CyResult(*)(CyEngine, uint32_t, CyInputContext, int32_t)` at byte 416.
    pub input_push_context: Option<unsafe extern "C" fn(CyEngine, u32, CyInputContext, i32) -> i32>,
    /// `CyResult(*)(CyEngine, uint32_t, CyInputContext)` at byte 424.
    pub input_pop_context: Option<unsafe extern "C" fn(CyEngine, u32, CyInputContext) -> i32>,
    /// `CyResult(*)(CyEngine, CyCamera*)` at byte 432.
    pub camera_active: Option<unsafe extern "C" fn(CyEngine, *mut CyCamera) -> i32>,
    /// `CyResult(*)(CyEngine, CyCamera, CyCameraView*)` at byte 440.
    pub camera_view: Option<unsafe extern "C" fn(CyEngine, CyCamera, *mut CyCameraView) -> i32>,
    /// `CyResult(*)(CyEngine, CyCamera, const float*, CyRay*)` at byte 448.
    pub camera_screen_to_ray:
        Option<unsafe extern "C" fn(CyEngine, CyCamera, *const f32, *mut CyRay) -> i32>,
    /// `CyResult(*)(CyEngine, CyCamera, const float*, uint32_t, CyScreenPoint*)` at byte 456.
    pub camera_world_to_screen: Option<
        unsafe extern "C" fn(CyEngine, CyCamera, *const f32, u32, *mut CyScreenPoint) -> i32,
    >,
    /// `CyResult(*)(CyEngine, CyCamera, const CyCameraTarget*)` at byte 464.
    pub camera_set_target:
        Option<unsafe extern "C" fn(CyEngine, CyCamera, *const CyCameraTarget) -> i32>,
    /// `CyResult(*)(CyEngine, CyCamera, const CyPose*)` at byte 472.
    pub camera_set_pose: Option<unsafe extern "C" fn(CyEngine, CyCamera, *const CyPose) -> i32>,
    /// `CyResult(*)(CyEngine, CyCamera)` at byte 480.
    pub camera_clear_pose: Option<unsafe extern "C" fn(CyEngine, CyCamera) -> i32>,
    /// `CyResult(*)(CyEngine, const CyRay*, const CyQueryFilter*, CyPhysicsHit*, bool*)` at byte 488.
    pub physics_raycast: Option<
        unsafe extern "C" fn(
            CyEngine,
            *const CyRay,
            *const CyQueryFilter,
            *mut CyPhysicsHit,
            *mut bool,
        ) -> i32,
    >,
    /// `CyResult(*)(CyEngine, const CyRay*, const CyQueryFilter*, CyPhysicsHit*, uint32_t, uint32_t*)` at byte 496.
    pub physics_raycast_all: Option<
        unsafe extern "C" fn(
            CyEngine,
            *const CyRay,
            *const CyQueryFilter,
            *mut CyPhysicsHit,
            u32,
            *mut u32,
        ) -> i32,
    >,
    /// `CyResult(*)(CyEngine, const CyShape*, const CyPose*, const float*, float, const CyQueryFilter*, CyPhysicsHit*, bool*)` at byte 504.
    pub physics_shape_cast: Option<
        unsafe extern "C" fn(
            CyEngine,
            *const CyShape,
            *const CyPose,
            *const f32,
            f32,
            *const CyQueryFilter,
            *mut CyPhysicsHit,
            *mut bool,
        ) -> i32,
    >,
    /// `CyResult(*)(CyEngine, const CyShape*, const CyPose*, const CyQueryFilter*, CyEntity*, uint32_t, uint32_t*)` at byte 512.
    pub physics_overlap: Option<
        unsafe extern "C" fn(
            CyEngine,
            *const CyShape,
            *const CyPose,
            *const CyQueryFilter,
            *mut CyEntity,
            u32,
            *mut u32,
        ) -> i32,
    >,
    /// `CyResult(*)(CyEngine, const CyNavPathRequest*, float*, uint32_t, CyNavPathResult*)` at byte 520.
    pub nav_find_path: Option<
        unsafe extern "C" fn(
            CyEngine,
            *const CyNavPathRequest,
            *mut f32,
            u32,
            *mut CyNavPathResult,
        ) -> i32,
    >,
    /// `CyResult(*)(CyEngine, const CyNavPathRequest*, CyNavQuery*)` at byte 528.
    pub nav_request_path:
        Option<unsafe extern "C" fn(CyEngine, *const CyNavPathRequest, *mut CyNavQuery) -> i32>,
    /// `CyResult(*)(CyEngine, CyNavQuery, float*, uint32_t, CyNavPathResult*)` at byte 536.
    pub nav_poll_path: Option<
        unsafe extern "C" fn(CyEngine, CyNavQuery, *mut f32, u32, *mut CyNavPathResult) -> i32,
    >,
    /// `CyResult(*)(CyEngine, CyNavQuery)` at byte 544.
    pub nav_cancel_path: Option<unsafe extern "C" fn(CyEngine, CyNavQuery) -> i32>,
    /// `CyResult(*)(CyEngine, CyEntity, const CyNavAgentParams*)` at byte 552.
    pub nav_agent_configure:
        Option<unsafe extern "C" fn(CyEngine, CyEntity, *const CyNavAgentParams) -> i32>,
    /// `CyResult(*)(CyEngine, CyEntity, const float*)` at byte 560.
    pub nav_agent_move_to: Option<unsafe extern "C" fn(CyEngine, CyEntity, *const f32) -> i32>,
    /// `CyResult(*)(CyEngine, CyEntity)` at byte 568.
    pub nav_agent_stop: Option<unsafe extern "C" fn(CyEngine, CyEntity) -> i32>,
    /// `CyResult(*)(CyEngine, CyEntity, CyNavAgentState*)` at byte 576.
    pub nav_agent_state:
        Option<unsafe extern "C" fn(CyEngine, CyEntity, *mut CyNavAgentState) -> i32>,
    /// `CyResult(*)(CyEngine, const char*, CyAudioCue*)` at byte 584.
    pub audio_find_cue:
        Option<unsafe extern "C" fn(CyEngine, *const ::std::ffi::c_char, *mut CyAudioCue) -> i32>,
    /// `CyResult(*)(CyEngine, const CyAudioPlay*, CyAudioVoice*)` at byte 592.
    pub audio_play:
        Option<unsafe extern "C" fn(CyEngine, *const CyAudioPlay, *mut CyAudioVoice) -> i32>,
    /// `CyResult(*)(CyEngine, CyAudioVoice, float)` at byte 600.
    pub audio_stop: Option<unsafe extern "C" fn(CyEngine, CyAudioVoice, f32) -> i32>,
    /// `bool(*)(CyEngine, CyAudioVoice)` at byte 608.
    pub audio_voice_playing: Option<unsafe extern "C" fn(CyEngine, CyAudioVoice) -> bool>,
    /// `CyResult(*)(CyEngine, const char*, CyAudioBus*)` at byte 616.
    pub audio_find_bus:
        Option<unsafe extern "C" fn(CyEngine, *const ::std::ffi::c_char, *mut CyAudioBus) -> i32>,
    /// `CyResult(*)(CyEngine, CyAudioBus, float, float)` at byte 624.
    pub audio_set_bus_volume: Option<unsafe extern "C" fn(CyEngine, CyAudioBus, f32, f32) -> i32>,
    /// `CyResult(*)(CyEngine, const char*, CyPrefab*)` at byte 632.
    pub spawn_resolve:
        Option<unsafe extern "C" fn(CyEngine, *const ::std::ffi::c_char, *mut CyPrefab) -> i32>,
    /// `CyResult(*)(CyEngine, CyPrefab, const CySpawnParams*, CyEntity*)` at byte 640.
    pub spawn_instantiate: Option<
        unsafe extern "C" fn(CyEngine, CyPrefab, *const CySpawnParams, *mut CyEntity) -> i32,
    >,
    /// `CyResult(*)(CyEngine, CyPrefab, CyEntity, const CyPose*, uint32_t, CyEntity*)` at byte 648.
    pub spawn_instantiate_many: Option<
        unsafe extern "C" fn(
            CyEngine,
            CyPrefab,
            CyEntity,
            *const CyPose,
            u32,
            *mut CyEntity,
        ) -> i32,
    >,
    /// `CyResult(*)(CyEngine, CyEntity)` at byte 656.
    pub spawn_destroy: Option<unsafe extern "C" fn(CyEngine, CyEntity) -> i32>,
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
        service_open: None,
        service_close: None,
        service_submit: None,
        service_cancel: None,
        service_poll: None,
        time_get: None,
        input_find_action: None,
        input_action_state: None,
        input_action_state_by_name: None,
        input_pointer: None,
        input_modifiers: None,
        input_find_context: None,
        input_push_context: None,
        input_pop_context: None,
        camera_active: None,
        camera_view: None,
        camera_screen_to_ray: None,
        camera_world_to_screen: None,
        camera_set_target: None,
        camera_set_pose: None,
        camera_clear_pose: None,
        physics_raycast: None,
        physics_raycast_all: None,
        physics_shape_cast: None,
        physics_overlap: None,
        nav_find_path: None,
        nav_request_path: None,
        nav_poll_path: None,
        nav_cancel_path: None,
        nav_agent_configure: None,
        nav_agent_move_to: None,
        nav_agent_stop: None,
        nav_agent_state: None,
        audio_find_cue: None,
        audio_play: None,
        audio_stop: None,
        audio_voice_playing: None,
        audio_find_bus: None,
        audio_set_bus_volume: None,
        spawn_resolve: None,
        spawn_instantiate: None,
        spawn_instantiate_many: None,
        spawn_destroy: None,
    };
}
