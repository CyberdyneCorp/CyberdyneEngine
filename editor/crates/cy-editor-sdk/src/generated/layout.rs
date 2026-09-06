// GENERATED FILE — DO NOT EDIT.
//
// Written by tools/gen/rust/sdk_gen.py from src/abi/include/cy/abi/cy_abi.h, through the
// description tools/abi/abi_describe.py produces and tools/abi/abi_gate.py diffs against
// src/abi/abi_baseline.json. Edit the C header or the generator; regenerate with
// `just build-editor --generate`, and `cargo test -p cy-editor-sdk` fails when this file is stale.

//! The layout `tools/abi/abi_describe.py` computed, asserted against what `rustc` produced.
//!
//! `src/abi/tests/test_layout.cpp` asserts the same numbers against the C compiler. Between them,
//! the description is checked from both sides of the boundary it describes: if the layout model is
//! ever wrong on a platform, one of the two fails there rather than the description quietly
//! describing a struct that does not exist.
//!
//! Compiled only under `cfg(test)` — they cost nothing at run time either way, but a build failure
//! in a dependency's release build is a worse way to learn this than a failing `cargo test`.

use std::mem::{align_of, offset_of, size_of};

use super::ffi;

const _: () = assert!(
    size_of::<ffi::CyVarPayload>() == 16,
    "CyVarPayload is not 16 bytes; the ABI description and rustc disagree"
);
const _: () = assert!(
    align_of::<ffi::CyVarPayload>() == 8,
    "CyVarPayload is not 8-byte aligned"
);

const _: () = assert!(
    size_of::<ffi::CyVar>() == 32,
    "CyVar is not 32 bytes; the ABI description and rustc disagree"
);
const _: () = assert!(align_of::<ffi::CyVar>() == 8, "CyVar is not 8-byte aligned");
const _: () = assert!(
    offset_of!(ffi::CyVar, r#type) == 0,
    "CyVar::type is not at byte 0"
);
const _: () = assert!(
    offset_of!(ffi::CyVar, flags) == 4,
    "CyVar::flags is not at byte 4"
);
const _: () = assert!(
    offset_of!(ffi::CyVar, length) == 8,
    "CyVar::length is not at byte 8"
);
const _: () = assert!(
    offset_of!(ffi::CyVar, payload) == 16,
    "CyVar::payload is not at byte 16"
);

const _: () = assert!(
    size_of::<ffi::CyFieldDesc>() == 24,
    "CyFieldDesc is not 24 bytes; the ABI description and rustc disagree"
);
const _: () = assert!(
    align_of::<ffi::CyFieldDesc>() == 8,
    "CyFieldDesc is not 8-byte aligned"
);
const _: () = assert!(
    offset_of!(ffi::CyFieldDesc, struct_size) == 0,
    "CyFieldDesc::struct_size is not at byte 0"
);
const _: () = assert!(
    offset_of!(ffi::CyFieldDesc, r#type) == 4,
    "CyFieldDesc::type is not at byte 4"
);
const _: () = assert!(
    offset_of!(ffi::CyFieldDesc, offset) == 8,
    "CyFieldDesc::offset is not at byte 8"
);
const _: () = assert!(
    offset_of!(ffi::CyFieldDesc, size) == 12,
    "CyFieldDesc::size is not at byte 12"
);
const _: () = assert!(
    offset_of!(ffi::CyFieldDesc, name) == 16,
    "CyFieldDesc::name is not at byte 16"
);

const _: () = assert!(
    size_of::<ffi::CyComponentTypeDesc>() == 32,
    "CyComponentTypeDesc is not 32 bytes; the ABI description and rustc disagree"
);
const _: () = assert!(
    align_of::<ffi::CyComponentTypeDesc>() == 8,
    "CyComponentTypeDesc is not 8-byte aligned"
);
const _: () = assert!(
    offset_of!(ffi::CyComponentTypeDesc, struct_size) == 0,
    "CyComponentTypeDesc::struct_size is not at byte 0"
);
const _: () = assert!(
    offset_of!(ffi::CyComponentTypeDesc, size) == 4,
    "CyComponentTypeDesc::size is not at byte 4"
);
const _: () = assert!(
    offset_of!(ffi::CyComponentTypeDesc, alignment) == 8,
    "CyComponentTypeDesc::alignment is not at byte 8"
);
const _: () = assert!(
    offset_of!(ffi::CyComponentTypeDesc, field_count) == 12,
    "CyComponentTypeDesc::field_count is not at byte 12"
);
const _: () = assert!(
    offset_of!(ffi::CyComponentTypeDesc, name) == 16,
    "CyComponentTypeDesc::name is not at byte 16"
);
const _: () = assert!(
    offset_of!(ffi::CyComponentTypeDesc, fields) == 24,
    "CyComponentTypeDesc::fields is not at byte 24"
);

const _: () = assert!(
    size_of::<ffi::CyComponentInfo>() == 24,
    "CyComponentInfo is not 24 bytes; the ABI description and rustc disagree"
);
const _: () = assert!(
    align_of::<ffi::CyComponentInfo>() == 8,
    "CyComponentInfo is not 8-byte aligned"
);
const _: () = assert!(
    offset_of!(ffi::CyComponentInfo, struct_size) == 0,
    "CyComponentInfo::struct_size is not at byte 0"
);
const _: () = assert!(
    offset_of!(ffi::CyComponentInfo, size) == 4,
    "CyComponentInfo::size is not at byte 4"
);
const _: () = assert!(
    offset_of!(ffi::CyComponentInfo, alignment) == 8,
    "CyComponentInfo::alignment is not at byte 8"
);
const _: () = assert!(
    offset_of!(ffi::CyComponentInfo, field_count) == 12,
    "CyComponentInfo::field_count is not at byte 12"
);
const _: () = assert!(
    offset_of!(ffi::CyComponentInfo, name) == 16,
    "CyComponentInfo::name is not at byte 16"
);

const _: () = assert!(
    size_of::<ffi::CyChunk>() == 40,
    "CyChunk is not 40 bytes; the ABI description and rustc disagree"
);
const _: () = assert!(
    align_of::<ffi::CyChunk>() == 8,
    "CyChunk is not 8-byte aligned"
);
const _: () = assert!(
    offset_of!(ffi::CyChunk, struct_size) == 0,
    "CyChunk::struct_size is not at byte 0"
);
const _: () = assert!(
    offset_of!(ffi::CyChunk, entity_count) == 4,
    "CyChunk::entity_count is not at byte 4"
);
const _: () = assert!(
    offset_of!(ffi::CyChunk, entities) == 8,
    "CyChunk::entities is not at byte 8"
);
const _: () = assert!(
    offset_of!(ffi::CyChunk, data) == 16,
    "CyChunk::data is not at byte 16"
);
const _: () = assert!(
    offset_of!(ffi::CyChunk, stride) == 24,
    "CyChunk::stride is not at byte 24"
);
const _: () = assert!(
    offset_of!(ffi::CyChunk, archetype) == 28,
    "CyChunk::archetype is not at byte 28"
);
const _: () = assert!(
    offset_of!(ffi::CyChunk, epoch) == 32,
    "CyChunk::epoch is not at byte 32"
);

const _: () = assert!(
    size_of::<ffi::CyBehaviourVTable>() == 56,
    "CyBehaviourVTable is not 56 bytes; the ABI description and rustc disagree"
);
const _: () = assert!(
    align_of::<ffi::CyBehaviourVTable>() == 8,
    "CyBehaviourVTable is not 8-byte aligned"
);
const _: () = assert!(
    offset_of!(ffi::CyBehaviourVTable, struct_size) == 0,
    "CyBehaviourVTable::struct_size is not at byte 0"
);
const _: () = assert!(
    offset_of!(ffi::CyBehaviourVTable, schema_version) == 4,
    "CyBehaviourVTable::schema_version is not at byte 4"
);
const _: () = assert!(
    offset_of!(ffi::CyBehaviourVTable, create) == 8,
    "CyBehaviourVTable::create is not at byte 8"
);
const _: () = assert!(
    offset_of!(ffi::CyBehaviourVTable, destroy) == 16,
    "CyBehaviourVTable::destroy is not at byte 16"
);
const _: () = assert!(
    offset_of!(ffi::CyBehaviourVTable, fixed_update) == 24,
    "CyBehaviourVTable::fixed_update is not at byte 24"
);
const _: () = assert!(
    offset_of!(ffi::CyBehaviourVTable, serialize) == 32,
    "CyBehaviourVTable::serialize is not at byte 32"
);
const _: () = assert!(
    offset_of!(ffi::CyBehaviourVTable, deserialize) == 40,
    "CyBehaviourVTable::deserialize is not at byte 40"
);
const _: () = assert!(
    offset_of!(ffi::CyBehaviourVTable, user_data) == 48,
    "CyBehaviourVTable::user_data is not at byte 48"
);

const _: () = assert!(
    size_of::<ffi::CyBorrow>() == 16,
    "CyBorrow is not 16 bytes; the ABI description and rustc disagree"
);
const _: () = assert!(
    align_of::<ffi::CyBorrow>() == 8,
    "CyBorrow is not 8-byte aligned"
);
const _: () = assert!(
    offset_of!(ffi::CyBorrow, data) == 0,
    "CyBorrow::data is not at byte 0"
);
const _: () = assert!(
    offset_of!(ffi::CyBorrow, epoch) == 8,
    "CyBorrow::epoch is not at byte 8"
);

const _: () = assert!(
    size_of::<ffi::CyInterfaceHeader>() == 16,
    "CyInterfaceHeader is not 16 bytes; the ABI description and rustc disagree"
);
const _: () = assert!(
    align_of::<ffi::CyInterfaceHeader>() == 4,
    "CyInterfaceHeader is not 4-byte aligned"
);
const _: () = assert!(
    offset_of!(ffi::CyInterfaceHeader, abi_major) == 0,
    "CyInterfaceHeader::abi_major is not at byte 0"
);
const _: () = assert!(
    offset_of!(ffi::CyInterfaceHeader, abi_minor) == 4,
    "CyInterfaceHeader::abi_minor is not at byte 4"
);
const _: () = assert!(
    offset_of!(ffi::CyInterfaceHeader, abi_patch) == 8,
    "CyInterfaceHeader::abi_patch is not at byte 8"
);
const _: () = assert!(
    offset_of!(ffi::CyInterfaceHeader, table_size) == 12,
    "CyInterfaceHeader::table_size is not at byte 12"
);

const _: () = assert!(
    size_of::<ffi::CyInterface>() == 320,
    "CyInterface is not 320 bytes; the ABI description and rustc disagree"
);
const _: () = assert!(
    align_of::<ffi::CyInterface>() == 8,
    "CyInterface is not 8-byte aligned"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, header) == 0,
    "CyInterface::header is not at byte 0"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, log) == 16,
    "CyInterface::log is not at byte 16"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, get_last_error) == 24,
    "CyInterface::get_last_error is not at byte 24"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, get_last_error_code) == 32,
    "CyInterface::get_last_error_code is not at byte 32"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, set_last_error) == 40,
    "CyInterface::set_last_error is not at byte 40"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, var_make_string) == 48,
    "CyInterface::var_make_string is not at byte 48"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, var_make_bytes) == 56,
    "CyInterface::var_make_bytes is not at byte 56"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, var_clone) == 64,
    "CyInterface::var_clone is not at byte 64"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, var_release) == 72,
    "CyInterface::var_release is not at byte 72"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, var_live_count) == 80,
    "CyInterface::var_live_count is not at byte 80"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, engine_world) == 88,
    "CyInterface::engine_world is not at byte 88"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, world_create_entity) == 96,
    "CyInterface::world_create_entity is not at byte 96"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, world_destroy_entity) == 104,
    "CyInterface::world_destroy_entity is not at byte 104"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, world_entity_alive) == 112,
    "CyInterface::world_entity_alive is not at byte 112"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, world_epoch) == 120,
    "CyInterface::world_epoch is not at byte 120"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, world_register_component) == 128,
    "CyInterface::world_register_component is not at byte 128"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, world_find_component) == 136,
    "CyInterface::world_find_component is not at byte 136"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, world_add_component) == 144,
    "CyInterface::world_add_component is not at byte 144"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, world_remove_component) == 152,
    "CyInterface::world_remove_component is not at byte 152"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, world_has_component) == 160,
    "CyInterface::world_has_component is not at byte 160"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, world_borrow_component) == 168,
    "CyInterface::world_borrow_component is not at byte 168"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, borrow_valid) == 176,
    "CyInterface::borrow_valid is not at byte 176"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, component_get_var) == 184,
    "CyInterface::component_get_var is not at byte 184"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, component_set_var) == 192,
    "CyInterface::component_set_var is not at byte 192"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, component_get_f32) == 200,
    "CyInterface::component_get_f32 is not at byte 200"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, component_set_f32) == 208,
    "CyInterface::component_set_f32 is not at byte 208"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, component_get_vec3) == 216,
    "CyInterface::component_get_vec3 is not at byte 216"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, component_set_vec3) == 224,
    "CyInterface::component_set_vec3 is not at byte 224"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, register_behaviour) == 232,
    "CyInterface::register_behaviour is not at byte 232"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, find_behaviour) == 240,
    "CyInterface::find_behaviour is not at byte 240"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, behaviour_generation) == 248,
    "CyInterface::behaviour_generation is not at byte 248"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, world_component_count) == 256,
    "CyInterface::world_component_count is not at byte 256"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, world_component_info) == 264,
    "CyInterface::world_component_info is not at byte 264"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, world_component_field) == 272,
    "CyInterface::world_component_field is not at byte 272"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, world_parent) == 280,
    "CyInterface::world_parent is not at byte 280"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, world_set_parent) == 288,
    "CyInterface::world_set_parent is not at byte 288"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, world_child_count) == 296,
    "CyInterface::world_child_count is not at byte 296"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, world_child) == 304,
    "CyInterface::world_child is not at byte 304"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, world_chunks) == 312,
    "CyInterface::world_chunks is not at byte 312"
);

const _: () = assert!(
    size_of::<ffi::CyModuleInit>() == 40,
    "CyModuleInit is not 40 bytes; the ABI description and rustc disagree"
);
const _: () = assert!(
    align_of::<ffi::CyModuleInit>() == 8,
    "CyModuleInit is not 8-byte aligned"
);
const _: () = assert!(
    offset_of!(ffi::CyModuleInit, struct_size) == 0,
    "CyModuleInit::struct_size is not at byte 0"
);
const _: () = assert!(
    offset_of!(ffi::CyModuleInit, abi_major) == 4,
    "CyModuleInit::abi_major is not at byte 4"
);
const _: () = assert!(
    offset_of!(ffi::CyModuleInit, abi_minor) == 8,
    "CyModuleInit::abi_minor is not at byte 8"
);
const _: () = assert!(
    offset_of!(ffi::CyModuleInit, reserved) == 12,
    "CyModuleInit::reserved is not at byte 12"
);
const _: () = assert!(
    offset_of!(ffi::CyModuleInit, initialize) == 16,
    "CyModuleInit::initialize is not at byte 16"
);
const _: () = assert!(
    offset_of!(ffi::CyModuleInit, shutdown) == 24,
    "CyModuleInit::shutdown is not at byte 24"
);
const _: () = assert!(
    offset_of!(ffi::CyModuleInit, user_data) == 32,
    "CyModuleInit::user_data is not at byte 32"
);

#[test]
fn the_table_has_every_entry_the_description_declares() {
    // A count rather than a name check: the fields ARE the names, so a missing one is a compile
    // error above. What a count catches is the other direction — a hand edit that added a field to
    // the generated table without the description having one, which would move every entry after
    // it and be invisible to a compiler that only sees Rust.
    assert_eq!(
        (size_of::<ffi::CyInterface>() - size_of::<ffi::CyInterfaceHeader>()) / size_of::<usize>(),
        38,
        "CyInterface has a different number of function-pointer entries than the ABI description"
    );
}
