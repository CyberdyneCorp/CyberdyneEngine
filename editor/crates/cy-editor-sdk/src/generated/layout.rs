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
    size_of::<ffi::CyServiceRequest>() == 40,
    "CyServiceRequest is not 40 bytes; the ABI description and rustc disagree"
);
const _: () = assert!(
    align_of::<ffi::CyServiceRequest>() == 8,
    "CyServiceRequest is not 8-byte aligned"
);
const _: () = assert!(
    offset_of!(ffi::CyServiceRequest, struct_size) == 0,
    "CyServiceRequest::struct_size is not at byte 0"
);
const _: () = assert!(
    offset_of!(ffi::CyServiceRequest, schema_version) == 4,
    "CyServiceRequest::schema_version is not at byte 4"
);
const _: () = assert!(
    offset_of!(ffi::CyServiceRequest, request_id) == 8,
    "CyServiceRequest::request_id is not at byte 8"
);
const _: () = assert!(
    offset_of!(ffi::CyServiceRequest, operation) == 16,
    "CyServiceRequest::operation is not at byte 16"
);
const _: () = assert!(
    offset_of!(ffi::CyServiceRequest, payload) == 24,
    "CyServiceRequest::payload is not at byte 24"
);
const _: () = assert!(
    offset_of!(ffi::CyServiceRequest, payload_size) == 32,
    "CyServiceRequest::payload_size is not at byte 32"
);

const _: () = assert!(
    size_of::<ffi::CyServiceEvent>() == 40,
    "CyServiceEvent is not 40 bytes; the ABI description and rustc disagree"
);
const _: () = assert!(
    align_of::<ffi::CyServiceEvent>() == 8,
    "CyServiceEvent is not 8-byte aligned"
);
const _: () = assert!(
    offset_of!(ffi::CyServiceEvent, struct_size) == 0,
    "CyServiceEvent::struct_size is not at byte 0"
);
const _: () = assert!(
    offset_of!(ffi::CyServiceEvent, kind) == 4,
    "CyServiceEvent::kind is not at byte 4"
);
const _: () = assert!(
    offset_of!(ffi::CyServiceEvent, request_id) == 8,
    "CyServiceEvent::request_id is not at byte 8"
);
const _: () = assert!(
    offset_of!(ffi::CyServiceEvent, schema_version) == 16,
    "CyServiceEvent::schema_version is not at byte 16"
);
const _: () = assert!(
    offset_of!(ffi::CyServiceEvent, reserved) == 20,
    "CyServiceEvent::reserved is not at byte 20"
);
const _: () = assert!(
    offset_of!(ffi::CyServiceEvent, payload) == 24,
    "CyServiceEvent::payload is not at byte 24"
);
const _: () = assert!(
    offset_of!(ffi::CyServiceEvent, payload_size) == 32,
    "CyServiceEvent::payload_size is not at byte 32"
);

const _: () = assert!(
    size_of::<ffi::CyBehaviourVTable>() == 64,
    "CyBehaviourVTable is not 64 bytes; the ABI description and rustc disagree"
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
    offset_of!(ffi::CyBehaviourVTable, frame_update) == 56,
    "CyBehaviourVTable::frame_update is not at byte 56"
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
    size_of::<ffi::CyPose>() == 28,
    "CyPose is not 28 bytes; the ABI description and rustc disagree"
);
const _: () = assert!(
    align_of::<ffi::CyPose>() == 4,
    "CyPose is not 4-byte aligned"
);
const _: () = assert!(
    offset_of!(ffi::CyPose, position) == 0,
    "CyPose::position is not at byte 0"
);
const _: () = assert!(
    offset_of!(ffi::CyPose, rotation) == 12,
    "CyPose::rotation is not at byte 12"
);

const _: () = assert!(
    size_of::<ffi::CyRay>() == 28,
    "CyRay is not 28 bytes; the ABI description and rustc disagree"
);
const _: () = assert!(align_of::<ffi::CyRay>() == 4, "CyRay is not 4-byte aligned");
const _: () = assert!(
    offset_of!(ffi::CyRay, origin) == 0,
    "CyRay::origin is not at byte 0"
);
const _: () = assert!(
    offset_of!(ffi::CyRay, direction) == 12,
    "CyRay::direction is not at byte 12"
);
const _: () = assert!(
    offset_of!(ffi::CyRay, max_distance) == 24,
    "CyRay::max_distance is not at byte 24"
);

const _: () = assert!(
    size_of::<ffi::CyTime>() == 48,
    "CyTime is not 48 bytes; the ABI description and rustc disagree"
);
const _: () = assert!(
    align_of::<ffi::CyTime>() == 8,
    "CyTime is not 8-byte aligned"
);
const _: () = assert!(
    offset_of!(ffi::CyTime, struct_size) == 0,
    "CyTime::struct_size is not at byte 0"
);
const _: () = assert!(
    offset_of!(ffi::CyTime, phase) == 4,
    "CyTime::phase is not at byte 4"
);
const _: () = assert!(
    offset_of!(ffi::CyTime, tick) == 8,
    "CyTime::tick is not at byte 8"
);
const _: () = assert!(
    offset_of!(ffi::CyTime, fixed_delta) == 16,
    "CyTime::fixed_delta is not at byte 16"
);
const _: () = assert!(
    offset_of!(ffi::CyTime, frame_delta) == 24,
    "CyTime::frame_delta is not at byte 24"
);
const _: () = assert!(
    offset_of!(ffi::CyTime, interpolation) == 32,
    "CyTime::interpolation is not at byte 32"
);
const _: () = assert!(
    offset_of!(ffi::CyTime, flags) == 40,
    "CyTime::flags is not at byte 40"
);
const _: () = assert!(
    offset_of!(ffi::CyTime, reserved) == 44,
    "CyTime::reserved is not at byte 44"
);

const _: () = assert!(
    size_of::<ffi::CyInputActionState>() == 32,
    "CyInputActionState is not 32 bytes; the ABI description and rustc disagree"
);
const _: () = assert!(
    align_of::<ffi::CyInputActionState>() == 8,
    "CyInputActionState is not 8-byte aligned"
);
const _: () = assert!(
    offset_of!(ffi::CyInputActionState, struct_size) == 0,
    "CyInputActionState::struct_size is not at byte 0"
);
const _: () = assert!(
    offset_of!(ffi::CyInputActionState, flags) == 4,
    "CyInputActionState::flags is not at byte 4"
);
const _: () = assert!(
    offset_of!(ffi::CyInputActionState, value) == 8,
    "CyInputActionState::value is not at byte 8"
);
const _: () = assert!(
    offset_of!(ffi::CyInputActionState, press_count) == 20,
    "CyInputActionState::press_count is not at byte 20"
);
const _: () = assert!(
    offset_of!(ffi::CyInputActionState, release_count) == 22,
    "CyInputActionState::release_count is not at byte 22"
);
const _: () = assert!(
    offset_of!(ffi::CyInputActionState, tick) == 24,
    "CyInputActionState::tick is not at byte 24"
);

const _: () = assert!(
    size_of::<ffi::CyInputPointer>() == 48,
    "CyInputPointer is not 48 bytes; the ABI description and rustc disagree"
);
const _: () = assert!(
    align_of::<ffi::CyInputPointer>() == 4,
    "CyInputPointer is not 4-byte aligned"
);
const _: () = assert!(
    offset_of!(ffi::CyInputPointer, struct_size) == 0,
    "CyInputPointer::struct_size is not at byte 0"
);
const _: () = assert!(
    offset_of!(ffi::CyInputPointer, flags) == 4,
    "CyInputPointer::flags is not at byte 4"
);
const _: () = assert!(
    offset_of!(ffi::CyInputPointer, buttons) == 8,
    "CyInputPointer::buttons is not at byte 8"
);
const _: () = assert!(
    offset_of!(ffi::CyInputPointer, buttons_pressed) == 12,
    "CyInputPointer::buttons_pressed is not at byte 12"
);
const _: () = assert!(
    offset_of!(ffi::CyInputPointer, buttons_released) == 16,
    "CyInputPointer::buttons_released is not at byte 16"
);
const _: () = assert!(
    offset_of!(ffi::CyInputPointer, position) == 20,
    "CyInputPointer::position is not at byte 20"
);
const _: () = assert!(
    offset_of!(ffi::CyInputPointer, delta) == 28,
    "CyInputPointer::delta is not at byte 28"
);
const _: () = assert!(
    offset_of!(ffi::CyInputPointer, wheel) == 36,
    "CyInputPointer::wheel is not at byte 36"
);
const _: () = assert!(
    offset_of!(ffi::CyInputPointer, reserved) == 44,
    "CyInputPointer::reserved is not at byte 44"
);

const _: () = assert!(
    size_of::<ffi::CyCameraView>() == 68,
    "CyCameraView is not 68 bytes; the ABI description and rustc disagree"
);
const _: () = assert!(
    align_of::<ffi::CyCameraView>() == 4,
    "CyCameraView is not 4-byte aligned"
);
const _: () = assert!(
    offset_of!(ffi::CyCameraView, struct_size) == 0,
    "CyCameraView::struct_size is not at byte 0"
);
const _: () = assert!(
    offset_of!(ffi::CyCameraView, flags) == 4,
    "CyCameraView::flags is not at byte 4"
);
const _: () = assert!(
    offset_of!(ffi::CyCameraView, pose) == 8,
    "CyCameraView::pose is not at byte 8"
);
const _: () = assert!(
    offset_of!(ffi::CyCameraView, vertical_fov) == 36,
    "CyCameraView::vertical_fov is not at byte 36"
);
const _: () = assert!(
    offset_of!(ffi::CyCameraView, ortho_height) == 40,
    "CyCameraView::ortho_height is not at byte 40"
);
const _: () = assert!(
    offset_of!(ffi::CyCameraView, near_plane) == 44,
    "CyCameraView::near_plane is not at byte 44"
);
const _: () = assert!(
    offset_of!(ffi::CyCameraView, far_plane) == 48,
    "CyCameraView::far_plane is not at byte 48"
);
const _: () = assert!(
    offset_of!(ffi::CyCameraView, viewport) == 52,
    "CyCameraView::viewport is not at byte 52"
);

const _: () = assert!(
    size_of::<ffi::CyScreenPoint>() == 16,
    "CyScreenPoint is not 16 bytes; the ABI description and rustc disagree"
);
const _: () = assert!(
    align_of::<ffi::CyScreenPoint>() == 4,
    "CyScreenPoint is not 4-byte aligned"
);
const _: () = assert!(
    offset_of!(ffi::CyScreenPoint, position) == 0,
    "CyScreenPoint::position is not at byte 0"
);
const _: () = assert!(
    offset_of!(ffi::CyScreenPoint, depth) == 8,
    "CyScreenPoint::depth is not at byte 8"
);
const _: () = assert!(
    offset_of!(ffi::CyScreenPoint, flags) == 12,
    "CyScreenPoint::flags is not at byte 12"
);

const _: () = assert!(
    size_of::<ffi::CyCameraTarget>() == 48,
    "CyCameraTarget is not 48 bytes; the ABI description and rustc disagree"
);
const _: () = assert!(
    align_of::<ffi::CyCameraTarget>() == 8,
    "CyCameraTarget is not 8-byte aligned"
);
const _: () = assert!(
    offset_of!(ffi::CyCameraTarget, struct_size) == 0,
    "CyCameraTarget::struct_size is not at byte 0"
);
const _: () = assert!(
    offset_of!(ffi::CyCameraTarget, flags) == 4,
    "CyCameraTarget::flags is not at byte 4"
);
const _: () = assert!(
    offset_of!(ffi::CyCameraTarget, entity) == 8,
    "CyCameraTarget::entity is not at byte 8"
);
const _: () = assert!(
    offset_of!(ffi::CyCameraTarget, position) == 16,
    "CyCameraTarget::position is not at byte 16"
);
const _: () = assert!(
    offset_of!(ffi::CyCameraTarget, yaw) == 28,
    "CyCameraTarget::yaw is not at byte 28"
);
const _: () = assert!(
    offset_of!(ffi::CyCameraTarget, pitch) == 32,
    "CyCameraTarget::pitch is not at byte 32"
);
const _: () = assert!(
    offset_of!(ffi::CyCameraTarget, distance) == 36,
    "CyCameraTarget::distance is not at byte 36"
);
const _: () = assert!(
    offset_of!(ffi::CyCameraTarget, blend_seconds) == 40,
    "CyCameraTarget::blend_seconds is not at byte 40"
);
const _: () = assert!(
    offset_of!(ffi::CyCameraTarget, reserved) == 44,
    "CyCameraTarget::reserved is not at byte 44"
);

const _: () = assert!(
    size_of::<ffi::CyShape>() == 24,
    "CyShape is not 24 bytes; the ABI description and rustc disagree"
);
const _: () = assert!(
    align_of::<ffi::CyShape>() == 4,
    "CyShape is not 4-byte aligned"
);
const _: () = assert!(
    offset_of!(ffi::CyShape, kind) == 0,
    "CyShape::kind is not at byte 0"
);
const _: () = assert!(
    offset_of!(ffi::CyShape, radius) == 4,
    "CyShape::radius is not at byte 4"
);
const _: () = assert!(
    offset_of!(ffi::CyShape, half_height) == 8,
    "CyShape::half_height is not at byte 8"
);
const _: () = assert!(
    offset_of!(ffi::CyShape, half_extents) == 12,
    "CyShape::half_extents is not at byte 12"
);

const _: () = assert!(
    size_of::<ffi::CyQueryFilter>() == 32,
    "CyQueryFilter is not 32 bytes; the ABI description and rustc disagree"
);
const _: () = assert!(
    align_of::<ffi::CyQueryFilter>() == 8,
    "CyQueryFilter is not 8-byte aligned"
);
const _: () = assert!(
    offset_of!(ffi::CyQueryFilter, struct_size) == 0,
    "CyQueryFilter::struct_size is not at byte 0"
);
const _: () = assert!(
    offset_of!(ffi::CyQueryFilter, layer) == 4,
    "CyQueryFilter::layer is not at byte 4"
);
const _: () = assert!(
    offset_of!(ffi::CyQueryFilter, mask) == 8,
    "CyQueryFilter::mask is not at byte 8"
);
const _: () = assert!(
    offset_of!(ffi::CyQueryFilter, flags) == 12,
    "CyQueryFilter::flags is not at byte 12"
);
const _: () = assert!(
    offset_of!(ffi::CyQueryFilter, ignore) == 16,
    "CyQueryFilter::ignore is not at byte 16"
);
const _: () = assert!(
    offset_of!(ffi::CyQueryFilter, ignore_count) == 24,
    "CyQueryFilter::ignore_count is not at byte 24"
);
const _: () = assert!(
    offset_of!(ffi::CyQueryFilter, reserved) == 28,
    "CyQueryFilter::reserved is not at byte 28"
);

const _: () = assert!(
    size_of::<ffi::CyPhysicsHit>() == 48,
    "CyPhysicsHit is not 48 bytes; the ABI description and rustc disagree"
);
const _: () = assert!(
    align_of::<ffi::CyPhysicsHit>() == 8,
    "CyPhysicsHit is not 8-byte aligned"
);
const _: () = assert!(
    offset_of!(ffi::CyPhysicsHit, flags) == 0,
    "CyPhysicsHit::flags is not at byte 0"
);
const _: () = assert!(
    offset_of!(ffi::CyPhysicsHit, reserved) == 4,
    "CyPhysicsHit::reserved is not at byte 4"
);
const _: () = assert!(
    offset_of!(ffi::CyPhysicsHit, entity) == 8,
    "CyPhysicsHit::entity is not at byte 8"
);
const _: () = assert!(
    offset_of!(ffi::CyPhysicsHit, point) == 16,
    "CyPhysicsHit::point is not at byte 16"
);
const _: () = assert!(
    offset_of!(ffi::CyPhysicsHit, normal) == 28,
    "CyPhysicsHit::normal is not at byte 28"
);
const _: () = assert!(
    offset_of!(ffi::CyPhysicsHit, distance) == 40,
    "CyPhysicsHit::distance is not at byte 40"
);
const _: () = assert!(
    offset_of!(ffi::CyPhysicsHit, fraction) == 44,
    "CyPhysicsHit::fraction is not at byte 44"
);

const _: () = assert!(
    size_of::<ffi::CyNavPathRequest>() == 64,
    "CyNavPathRequest is not 64 bytes; the ABI description and rustc disagree"
);
const _: () = assert!(
    align_of::<ffi::CyNavPathRequest>() == 8,
    "CyNavPathRequest is not 8-byte aligned"
);
const _: () = assert!(
    offset_of!(ffi::CyNavPathRequest, struct_size) == 0,
    "CyNavPathRequest::struct_size is not at byte 0"
);
const _: () = assert!(
    offset_of!(ffi::CyNavPathRequest, world) == 4,
    "CyNavPathRequest::world is not at byte 4"
);
const _: () = assert!(
    offset_of!(ffi::CyNavPathRequest, start) == 8,
    "CyNavPathRequest::start is not at byte 8"
);
const _: () = assert!(
    offset_of!(ffi::CyNavPathRequest, end) == 20,
    "CyNavPathRequest::end is not at byte 20"
);
const _: () = assert!(
    offset_of!(ffi::CyNavPathRequest, extents) == 32,
    "CyNavPathRequest::extents is not at byte 32"
);
const _: () = assert!(
    offset_of!(ffi::CyNavPathRequest, node_budget) == 44,
    "CyNavPathRequest::node_budget is not at byte 44"
);
const _: () = assert!(
    offset_of!(ffi::CyNavPathRequest, area_mask) == 48,
    "CyNavPathRequest::area_mask is not at byte 48"
);
const _: () = assert!(
    offset_of!(ffi::CyNavPathRequest, capabilities) == 56,
    "CyNavPathRequest::capabilities is not at byte 56"
);

const _: () = assert!(
    size_of::<ffi::CyNavPathResult>() == 24,
    "CyNavPathResult is not 24 bytes; the ABI description and rustc disagree"
);
const _: () = assert!(
    align_of::<ffi::CyNavPathResult>() == 4,
    "CyNavPathResult is not 4-byte aligned"
);
const _: () = assert!(
    offset_of!(ffi::CyNavPathResult, struct_size) == 0,
    "CyNavPathResult::struct_size is not at byte 0"
);
const _: () = assert!(
    offset_of!(ffi::CyNavPathResult, flags) == 4,
    "CyNavPathResult::flags is not at byte 4"
);
const _: () = assert!(
    offset_of!(ffi::CyNavPathResult, point_count) == 8,
    "CyNavPathResult::point_count is not at byte 8"
);
const _: () = assert!(
    offset_of!(ffi::CyNavPathResult, state) == 12,
    "CyNavPathResult::state is not at byte 12"
);
const _: () = assert!(
    offset_of!(ffi::CyNavPathResult, cost) == 16,
    "CyNavPathResult::cost is not at byte 16"
);
const _: () = assert!(
    offset_of!(ffi::CyNavPathResult, length) == 20,
    "CyNavPathResult::length is not at byte 20"
);

const _: () = assert!(
    size_of::<ffi::CyNavAgentParams>() == 48,
    "CyNavAgentParams is not 48 bytes; the ABI description and rustc disagree"
);
const _: () = assert!(
    align_of::<ffi::CyNavAgentParams>() == 8,
    "CyNavAgentParams is not 8-byte aligned"
);
const _: () = assert!(
    offset_of!(ffi::CyNavAgentParams, struct_size) == 0,
    "CyNavAgentParams::struct_size is not at byte 0"
);
const _: () = assert!(
    offset_of!(ffi::CyNavAgentParams, world) == 4,
    "CyNavAgentParams::world is not at byte 4"
);
const _: () = assert!(
    offset_of!(ffi::CyNavAgentParams, radius) == 8,
    "CyNavAgentParams::radius is not at byte 8"
);
const _: () = assert!(
    offset_of!(ffi::CyNavAgentParams, height) == 12,
    "CyNavAgentParams::height is not at byte 12"
);
const _: () = assert!(
    offset_of!(ffi::CyNavAgentParams, max_speed) == 16,
    "CyNavAgentParams::max_speed is not at byte 16"
);
const _: () = assert!(
    offset_of!(ffi::CyNavAgentParams, max_acceleration) == 20,
    "CyNavAgentParams::max_acceleration is not at byte 20"
);
const _: () = assert!(
    offset_of!(ffi::CyNavAgentParams, arrival_distance) == 24,
    "CyNavAgentParams::arrival_distance is not at byte 24"
);
const _: () = assert!(
    offset_of!(ffi::CyNavAgentParams, priority) == 28,
    "CyNavAgentParams::priority is not at byte 28"
);
const _: () = assert!(
    offset_of!(ffi::CyNavAgentParams, area_mask) == 32,
    "CyNavAgentParams::area_mask is not at byte 32"
);
const _: () = assert!(
    offset_of!(ffi::CyNavAgentParams, capabilities) == 40,
    "CyNavAgentParams::capabilities is not at byte 40"
);

const _: () = assert!(
    size_of::<ffi::CyNavAgentState>() == 56,
    "CyNavAgentState is not 56 bytes; the ABI description and rustc disagree"
);
const _: () = assert!(
    align_of::<ffi::CyNavAgentState>() == 4,
    "CyNavAgentState is not 4-byte aligned"
);
const _: () = assert!(
    offset_of!(ffi::CyNavAgentState, struct_size) == 0,
    "CyNavAgentState::struct_size is not at byte 0"
);
const _: () = assert!(
    offset_of!(ffi::CyNavAgentState, status) == 4,
    "CyNavAgentState::status is not at byte 4"
);
const _: () = assert!(
    offset_of!(ffi::CyNavAgentState, flags) == 8,
    "CyNavAgentState::flags is not at byte 8"
);
const _: () = assert!(
    offset_of!(ffi::CyNavAgentState, reserved) == 12,
    "CyNavAgentState::reserved is not at byte 12"
);
const _: () = assert!(
    offset_of!(ffi::CyNavAgentState, position) == 16,
    "CyNavAgentState::position is not at byte 16"
);
const _: () = assert!(
    offset_of!(ffi::CyNavAgentState, velocity) == 28,
    "CyNavAgentState::velocity is not at byte 28"
);
const _: () = assert!(
    offset_of!(ffi::CyNavAgentState, target) == 40,
    "CyNavAgentState::target is not at byte 40"
);
const _: () = assert!(
    offset_of!(ffi::CyNavAgentState, remaining_distance) == 52,
    "CyNavAgentState::remaining_distance is not at byte 52"
);

const _: () = assert!(
    size_of::<ffi::CyAudioPlay>() == 56,
    "CyAudioPlay is not 56 bytes; the ABI description and rustc disagree"
);
const _: () = assert!(
    align_of::<ffi::CyAudioPlay>() == 8,
    "CyAudioPlay is not 8-byte aligned"
);
const _: () = assert!(
    offset_of!(ffi::CyAudioPlay, struct_size) == 0,
    "CyAudioPlay::struct_size is not at byte 0"
);
const _: () = assert!(
    offset_of!(ffi::CyAudioPlay, flags) == 4,
    "CyAudioPlay::flags is not at byte 4"
);
const _: () = assert!(
    offset_of!(ffi::CyAudioPlay, cue) == 8,
    "CyAudioPlay::cue is not at byte 8"
);
const _: () = assert!(
    offset_of!(ffi::CyAudioPlay, attach_to) == 16,
    "CyAudioPlay::attach_to is not at byte 16"
);
const _: () = assert!(
    offset_of!(ffi::CyAudioPlay, bus) == 24,
    "CyAudioPlay::bus is not at byte 24"
);
const _: () = assert!(
    offset_of!(ffi::CyAudioPlay, position) == 32,
    "CyAudioPlay::position is not at byte 32"
);
const _: () = assert!(
    offset_of!(ffi::CyAudioPlay, volume) == 44,
    "CyAudioPlay::volume is not at byte 44"
);
const _: () = assert!(
    offset_of!(ffi::CyAudioPlay, pitch) == 48,
    "CyAudioPlay::pitch is not at byte 48"
);
const _: () = assert!(
    offset_of!(ffi::CyAudioPlay, fade_in_seconds) == 52,
    "CyAudioPlay::fade_in_seconds is not at byte 52"
);

const _: () = assert!(
    size_of::<ffi::CySpawnParams>() == 56,
    "CySpawnParams is not 56 bytes; the ABI description and rustc disagree"
);
const _: () = assert!(
    align_of::<ffi::CySpawnParams>() == 8,
    "CySpawnParams is not 8-byte aligned"
);
const _: () = assert!(
    offset_of!(ffi::CySpawnParams, struct_size) == 0,
    "CySpawnParams::struct_size is not at byte 0"
);
const _: () = assert!(
    offset_of!(ffi::CySpawnParams, flags) == 4,
    "CySpawnParams::flags is not at byte 4"
);
const _: () = assert!(
    offset_of!(ffi::CySpawnParams, parent) == 8,
    "CySpawnParams::parent is not at byte 8"
);
const _: () = assert!(
    offset_of!(ffi::CySpawnParams, pose) == 16,
    "CySpawnParams::pose is not at byte 16"
);
const _: () = assert!(
    offset_of!(ffi::CySpawnParams, scale) == 44,
    "CySpawnParams::scale is not at byte 44"
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
    size_of::<ffi::CyInterface>() == 680,
    "CyInterface is not 680 bytes; the ABI description and rustc disagree"
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
    offset_of!(ffi::CyInterface, service_open) == 320,
    "CyInterface::service_open is not at byte 320"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, service_close) == 328,
    "CyInterface::service_close is not at byte 328"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, service_submit) == 336,
    "CyInterface::service_submit is not at byte 336"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, service_cancel) == 344,
    "CyInterface::service_cancel is not at byte 344"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, service_poll) == 352,
    "CyInterface::service_poll is not at byte 352"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, time_get) == 360,
    "CyInterface::time_get is not at byte 360"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, input_find_action) == 368,
    "CyInterface::input_find_action is not at byte 368"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, input_action_state) == 376,
    "CyInterface::input_action_state is not at byte 376"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, input_action_state_by_name) == 384,
    "CyInterface::input_action_state_by_name is not at byte 384"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, input_pointer) == 392,
    "CyInterface::input_pointer is not at byte 392"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, input_modifiers) == 400,
    "CyInterface::input_modifiers is not at byte 400"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, input_find_context) == 408,
    "CyInterface::input_find_context is not at byte 408"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, input_push_context) == 416,
    "CyInterface::input_push_context is not at byte 416"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, input_pop_context) == 424,
    "CyInterface::input_pop_context is not at byte 424"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, camera_active) == 432,
    "CyInterface::camera_active is not at byte 432"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, camera_view) == 440,
    "CyInterface::camera_view is not at byte 440"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, camera_screen_to_ray) == 448,
    "CyInterface::camera_screen_to_ray is not at byte 448"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, camera_world_to_screen) == 456,
    "CyInterface::camera_world_to_screen is not at byte 456"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, camera_set_target) == 464,
    "CyInterface::camera_set_target is not at byte 464"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, camera_set_pose) == 472,
    "CyInterface::camera_set_pose is not at byte 472"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, camera_clear_pose) == 480,
    "CyInterface::camera_clear_pose is not at byte 480"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, physics_raycast) == 488,
    "CyInterface::physics_raycast is not at byte 488"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, physics_raycast_all) == 496,
    "CyInterface::physics_raycast_all is not at byte 496"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, physics_shape_cast) == 504,
    "CyInterface::physics_shape_cast is not at byte 504"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, physics_overlap) == 512,
    "CyInterface::physics_overlap is not at byte 512"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, nav_find_path) == 520,
    "CyInterface::nav_find_path is not at byte 520"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, nav_request_path) == 528,
    "CyInterface::nav_request_path is not at byte 528"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, nav_poll_path) == 536,
    "CyInterface::nav_poll_path is not at byte 536"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, nav_cancel_path) == 544,
    "CyInterface::nav_cancel_path is not at byte 544"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, nav_agent_configure) == 552,
    "CyInterface::nav_agent_configure is not at byte 552"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, nav_agent_move_to) == 560,
    "CyInterface::nav_agent_move_to is not at byte 560"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, nav_agent_stop) == 568,
    "CyInterface::nav_agent_stop is not at byte 568"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, nav_agent_state) == 576,
    "CyInterface::nav_agent_state is not at byte 576"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, audio_find_cue) == 584,
    "CyInterface::audio_find_cue is not at byte 584"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, audio_play) == 592,
    "CyInterface::audio_play is not at byte 592"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, audio_stop) == 600,
    "CyInterface::audio_stop is not at byte 600"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, audio_voice_playing) == 608,
    "CyInterface::audio_voice_playing is not at byte 608"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, audio_find_bus) == 616,
    "CyInterface::audio_find_bus is not at byte 616"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, audio_set_bus_volume) == 624,
    "CyInterface::audio_set_bus_volume is not at byte 624"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, spawn_resolve) == 632,
    "CyInterface::spawn_resolve is not at byte 632"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, spawn_instantiate) == 640,
    "CyInterface::spawn_instantiate is not at byte 640"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, spawn_instantiate_many) == 648,
    "CyInterface::spawn_instantiate_many is not at byte 648"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, spawn_destroy) == 656,
    "CyInterface::spawn_destroy is not at byte 656"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, vfx_effect_parameter_set) == 664,
    "CyInterface::vfx_effect_parameter_set is not at byte 664"
);
const _: () = assert!(
    offset_of!(ffi::CyInterface, vfx_effect_parameter_get) == 672,
    "CyInterface::vfx_effect_parameter_get is not at byte 672"
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
        83,
        "CyInterface has a different number of function-pointer entries than the ABI description"
    );
}
