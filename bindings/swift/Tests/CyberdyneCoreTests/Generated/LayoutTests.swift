// GENERATED FILE — DO NOT EDIT.
//
// Written by tools/gen/swift/overlay_gen.py from src/abi/include/cy/abi/cy_abi.h, through the
// description tools/abi/abi_describe.py produces and tools/abi/abi_gate.py diffs against
// src/abi/abi_baseline.json. Edit the C header or the generator; regenerate with
// `just generate-swift`, and `just generate-swift --check` fails when this file is stale.

import CyberdyneABI
import XCTest

@testable import CyberdyneCore

/// Every ABI struct's size, alignment and member offsets, as Swift's C importer sees them.
///
/// The expected numbers come from the ABI description, which computes them from the declaration
/// rather than measuring them on this machine — see tools/abi/abi_describe.py for why. So a failure
/// here means one of two things, and both are worth stopping for: Swift's importer disagrees with
/// the layout model on this platform, or the header changed and the overlay was not regenerated.
final class GeneratedLayoutTests: XCTestCase {
    func testVarPayloadLayout() {
        XCTAssertEqual(MemoryLayout<CyVarPayload>.size, 16, "CyVarPayload size")
        XCTAssertEqual(MemoryLayout<CyVarPayload>.alignment, 8, "CyVarPayload alignment")
    }

    func testVarLayout() {
        XCTAssertEqual(MemoryLayout<CyVar>.size, 32, "CyVar size")
        XCTAssertEqual(MemoryLayout<CyVar>.alignment, 8, "CyVar alignment")
        XCTAssertEqual(MemoryLayout<CyVar>.offset(of: \CyVar.type), 0, "CyVar.type offset")
        XCTAssertEqual(MemoryLayout<CyVar>.offset(of: \CyVar.flags), 4, "CyVar.flags offset")
        XCTAssertEqual(MemoryLayout<CyVar>.offset(of: \CyVar.length), 8, "CyVar.length offset")
        XCTAssertEqual(MemoryLayout<CyVar>.offset(of: \CyVar.payload), 16, "CyVar.payload offset")
    }

    func testFieldDescLayout() {
        XCTAssertEqual(MemoryLayout<CyFieldDesc>.size, 24, "CyFieldDesc size")
        XCTAssertEqual(MemoryLayout<CyFieldDesc>.alignment, 8, "CyFieldDesc alignment")
        XCTAssertEqual(MemoryLayout<CyFieldDesc>.offset(of: \CyFieldDesc.struct_size), 0, "CyFieldDesc.struct_size offset")
        XCTAssertEqual(MemoryLayout<CyFieldDesc>.offset(of: \CyFieldDesc.type), 4, "CyFieldDesc.type offset")
        XCTAssertEqual(MemoryLayout<CyFieldDesc>.offset(of: \CyFieldDesc.offset), 8, "CyFieldDesc.offset offset")
        XCTAssertEqual(MemoryLayout<CyFieldDesc>.offset(of: \CyFieldDesc.size), 12, "CyFieldDesc.size offset")
        XCTAssertEqual(MemoryLayout<CyFieldDesc>.offset(of: \CyFieldDesc.name), 16, "CyFieldDesc.name offset")
    }

    func testComponentTypeDescLayout() {
        XCTAssertEqual(MemoryLayout<CyComponentTypeDesc>.size, 32, "CyComponentTypeDesc size")
        XCTAssertEqual(MemoryLayout<CyComponentTypeDesc>.alignment, 8, "CyComponentTypeDesc alignment")
        XCTAssertEqual(MemoryLayout<CyComponentTypeDesc>.offset(of: \CyComponentTypeDesc.struct_size), 0, "CyComponentTypeDesc.struct_size offset")
        XCTAssertEqual(MemoryLayout<CyComponentTypeDesc>.offset(of: \CyComponentTypeDesc.size), 4, "CyComponentTypeDesc.size offset")
        XCTAssertEqual(MemoryLayout<CyComponentTypeDesc>.offset(of: \CyComponentTypeDesc.alignment), 8, "CyComponentTypeDesc.alignment offset")
        XCTAssertEqual(MemoryLayout<CyComponentTypeDesc>.offset(of: \CyComponentTypeDesc.field_count), 12, "CyComponentTypeDesc.field_count offset")
        XCTAssertEqual(MemoryLayout<CyComponentTypeDesc>.offset(of: \CyComponentTypeDesc.name), 16, "CyComponentTypeDesc.name offset")
        XCTAssertEqual(MemoryLayout<CyComponentTypeDesc>.offset(of: \CyComponentTypeDesc.fields), 24, "CyComponentTypeDesc.fields offset")
    }

    func testComponentInfoLayout() {
        XCTAssertEqual(MemoryLayout<CyComponentInfo>.size, 24, "CyComponentInfo size")
        XCTAssertEqual(MemoryLayout<CyComponentInfo>.alignment, 8, "CyComponentInfo alignment")
        XCTAssertEqual(MemoryLayout<CyComponentInfo>.offset(of: \CyComponentInfo.struct_size), 0, "CyComponentInfo.struct_size offset")
        XCTAssertEqual(MemoryLayout<CyComponentInfo>.offset(of: \CyComponentInfo.size), 4, "CyComponentInfo.size offset")
        XCTAssertEqual(MemoryLayout<CyComponentInfo>.offset(of: \CyComponentInfo.alignment), 8, "CyComponentInfo.alignment offset")
        XCTAssertEqual(MemoryLayout<CyComponentInfo>.offset(of: \CyComponentInfo.field_count), 12, "CyComponentInfo.field_count offset")
        XCTAssertEqual(MemoryLayout<CyComponentInfo>.offset(of: \CyComponentInfo.name), 16, "CyComponentInfo.name offset")
    }

    func testChunkLayout() {
        XCTAssertEqual(MemoryLayout<CyChunk>.size, 40, "CyChunk size")
        XCTAssertEqual(MemoryLayout<CyChunk>.alignment, 8, "CyChunk alignment")
        XCTAssertEqual(MemoryLayout<CyChunk>.offset(of: \CyChunk.struct_size), 0, "CyChunk.struct_size offset")
        XCTAssertEqual(MemoryLayout<CyChunk>.offset(of: \CyChunk.entity_count), 4, "CyChunk.entity_count offset")
        XCTAssertEqual(MemoryLayout<CyChunk>.offset(of: \CyChunk.entities), 8, "CyChunk.entities offset")
        XCTAssertEqual(MemoryLayout<CyChunk>.offset(of: \CyChunk.data), 16, "CyChunk.data offset")
        XCTAssertEqual(MemoryLayout<CyChunk>.offset(of: \CyChunk.stride), 24, "CyChunk.stride offset")
        XCTAssertEqual(MemoryLayout<CyChunk>.offset(of: \CyChunk.archetype), 28, "CyChunk.archetype offset")
        XCTAssertEqual(MemoryLayout<CyChunk>.offset(of: \CyChunk.epoch), 32, "CyChunk.epoch offset")
    }

    func testServiceRequestLayout() {
        XCTAssertEqual(MemoryLayout<CyServiceRequest>.size, 40, "CyServiceRequest size")
        XCTAssertEqual(MemoryLayout<CyServiceRequest>.alignment, 8, "CyServiceRequest alignment")
        XCTAssertEqual(MemoryLayout<CyServiceRequest>.offset(of: \CyServiceRequest.struct_size), 0, "CyServiceRequest.struct_size offset")
        XCTAssertEqual(MemoryLayout<CyServiceRequest>.offset(of: \CyServiceRequest.schema_version), 4, "CyServiceRequest.schema_version offset")
        XCTAssertEqual(MemoryLayout<CyServiceRequest>.offset(of: \CyServiceRequest.request_id), 8, "CyServiceRequest.request_id offset")
        XCTAssertEqual(MemoryLayout<CyServiceRequest>.offset(of: \CyServiceRequest.operation), 16, "CyServiceRequest.operation offset")
        XCTAssertEqual(MemoryLayout<CyServiceRequest>.offset(of: \CyServiceRequest.payload), 24, "CyServiceRequest.payload offset")
        XCTAssertEqual(MemoryLayout<CyServiceRequest>.offset(of: \CyServiceRequest.payload_size), 32, "CyServiceRequest.payload_size offset")
    }

    func testServiceEventLayout() {
        XCTAssertEqual(MemoryLayout<CyServiceEvent>.size, 40, "CyServiceEvent size")
        XCTAssertEqual(MemoryLayout<CyServiceEvent>.alignment, 8, "CyServiceEvent alignment")
        XCTAssertEqual(MemoryLayout<CyServiceEvent>.offset(of: \CyServiceEvent.struct_size), 0, "CyServiceEvent.struct_size offset")
        XCTAssertEqual(MemoryLayout<CyServiceEvent>.offset(of: \CyServiceEvent.kind), 4, "CyServiceEvent.kind offset")
        XCTAssertEqual(MemoryLayout<CyServiceEvent>.offset(of: \CyServiceEvent.request_id), 8, "CyServiceEvent.request_id offset")
        XCTAssertEqual(MemoryLayout<CyServiceEvent>.offset(of: \CyServiceEvent.schema_version), 16, "CyServiceEvent.schema_version offset")
        XCTAssertEqual(MemoryLayout<CyServiceEvent>.offset(of: \CyServiceEvent.reserved), 20, "CyServiceEvent.reserved offset")
        XCTAssertEqual(MemoryLayout<CyServiceEvent>.offset(of: \CyServiceEvent.payload), 24, "CyServiceEvent.payload offset")
        XCTAssertEqual(MemoryLayout<CyServiceEvent>.offset(of: \CyServiceEvent.payload_size), 32, "CyServiceEvent.payload_size offset")
    }

    func testUiEventLayout() {
        XCTAssertEqual(MemoryLayout<CyUiEvent>.size, 40, "CyUiEvent size")
        XCTAssertEqual(MemoryLayout<CyUiEvent>.alignment, 8, "CyUiEvent alignment")
        XCTAssertEqual(MemoryLayout<CyUiEvent>.offset(of: \CyUiEvent.struct_size), 0, "CyUiEvent.struct_size offset")
        XCTAssertEqual(MemoryLayout<CyUiEvent>.offset(of: \CyUiEvent.kind), 4, "CyUiEvent.kind offset")
        XCTAssertEqual(MemoryLayout<CyUiEvent>.offset(of: \CyUiEvent.element), 8, "CyUiEvent.element offset")
        XCTAssertEqual(MemoryLayout<CyUiEvent>.offset(of: \CyUiEvent.owner), 16, "CyUiEvent.owner offset")
        XCTAssertEqual(MemoryLayout<CyUiEvent>.offset(of: \CyUiEvent.position), 24, "CyUiEvent.position offset")
        XCTAssertEqual(MemoryLayout<CyUiEvent>.offset(of: \CyUiEvent.button), 32, "CyUiEvent.button offset")
        XCTAssertEqual(MemoryLayout<CyUiEvent>.offset(of: \CyUiEvent.reserved), 36, "CyUiEvent.reserved offset")
    }

    func testBehaviourVTableLayout() {
        XCTAssertEqual(MemoryLayout<CyBehaviourVTable>.size, 112, "CyBehaviourVTable size")
        XCTAssertEqual(MemoryLayout<CyBehaviourVTable>.alignment, 8, "CyBehaviourVTable alignment")
        XCTAssertEqual(MemoryLayout<CyBehaviourVTable>.offset(of: \CyBehaviourVTable.struct_size), 0, "CyBehaviourVTable.struct_size offset")
        XCTAssertEqual(MemoryLayout<CyBehaviourVTable>.offset(of: \CyBehaviourVTable.schema_version), 4, "CyBehaviourVTable.schema_version offset")
        XCTAssertEqual(MemoryLayout<CyBehaviourVTable>.offset(of: \CyBehaviourVTable.create), 8, "CyBehaviourVTable.create offset")
        XCTAssertEqual(MemoryLayout<CyBehaviourVTable>.offset(of: \CyBehaviourVTable.destroy), 16, "CyBehaviourVTable.destroy offset")
        XCTAssertEqual(MemoryLayout<CyBehaviourVTable>.offset(of: \CyBehaviourVTable.fixed_update), 24, "CyBehaviourVTable.fixed_update offset")
        XCTAssertEqual(MemoryLayout<CyBehaviourVTable>.offset(of: \CyBehaviourVTable.serialize), 32, "CyBehaviourVTable.serialize offset")
        XCTAssertEqual(MemoryLayout<CyBehaviourVTable>.offset(of: \CyBehaviourVTable.deserialize), 40, "CyBehaviourVTable.deserialize offset")
        XCTAssertEqual(MemoryLayout<CyBehaviourVTable>.offset(of: \CyBehaviourVTable.user_data), 48, "CyBehaviourVTable.user_data offset")
        XCTAssertEqual(MemoryLayout<CyBehaviourVTable>.offset(of: \CyBehaviourVTable.frame_update), 56, "CyBehaviourVTable.frame_update offset")
        XCTAssertEqual(MemoryLayout<CyBehaviourVTable>.offset(of: \CyBehaviourVTable.enter_tree), 64, "CyBehaviourVTable.enter_tree offset")
        XCTAssertEqual(MemoryLayout<CyBehaviourVTable>.offset(of: \CyBehaviourVTable.ready), 72, "CyBehaviourVTable.ready offset")
        XCTAssertEqual(MemoryLayout<CyBehaviourVTable>.offset(of: \CyBehaviourVTable.enable), 80, "CyBehaviourVTable.enable offset")
        XCTAssertEqual(MemoryLayout<CyBehaviourVTable>.offset(of: \CyBehaviourVTable.disable), 88, "CyBehaviourVTable.disable offset")
        XCTAssertEqual(MemoryLayout<CyBehaviourVTable>.offset(of: \CyBehaviourVTable.exit_tree), 96, "CyBehaviourVTable.exit_tree offset")
        XCTAssertEqual(MemoryLayout<CyBehaviourVTable>.offset(of: \CyBehaviourVTable.ui_event), 104, "CyBehaviourVTable.ui_event offset")
    }

    func testBorrowLayout() {
        XCTAssertEqual(MemoryLayout<CyBorrow>.size, 16, "CyBorrow size")
        XCTAssertEqual(MemoryLayout<CyBorrow>.alignment, 8, "CyBorrow alignment")
        XCTAssertEqual(MemoryLayout<CyBorrow>.offset(of: \CyBorrow.data), 0, "CyBorrow.data offset")
        XCTAssertEqual(MemoryLayout<CyBorrow>.offset(of: \CyBorrow.epoch), 8, "CyBorrow.epoch offset")
    }

    func testPoseLayout() {
        XCTAssertEqual(MemoryLayout<CyPose>.size, 28, "CyPose size")
        XCTAssertEqual(MemoryLayout<CyPose>.alignment, 4, "CyPose alignment")
        XCTAssertEqual(MemoryLayout<CyPose>.offset(of: \CyPose.position), 0, "CyPose.position offset")
        XCTAssertEqual(MemoryLayout<CyPose>.offset(of: \CyPose.rotation), 12, "CyPose.rotation offset")
    }

    func testRayLayout() {
        XCTAssertEqual(MemoryLayout<CyRay>.size, 28, "CyRay size")
        XCTAssertEqual(MemoryLayout<CyRay>.alignment, 4, "CyRay alignment")
        XCTAssertEqual(MemoryLayout<CyRay>.offset(of: \CyRay.origin), 0, "CyRay.origin offset")
        XCTAssertEqual(MemoryLayout<CyRay>.offset(of: \CyRay.direction), 12, "CyRay.direction offset")
        XCTAssertEqual(MemoryLayout<CyRay>.offset(of: \CyRay.max_distance), 24, "CyRay.max_distance offset")
    }

    func testTimeLayout() {
        XCTAssertEqual(MemoryLayout<CyTime>.size, 48, "CyTime size")
        XCTAssertEqual(MemoryLayout<CyTime>.alignment, 8, "CyTime alignment")
        XCTAssertEqual(MemoryLayout<CyTime>.offset(of: \CyTime.struct_size), 0, "CyTime.struct_size offset")
        XCTAssertEqual(MemoryLayout<CyTime>.offset(of: \CyTime.phase), 4, "CyTime.phase offset")
        XCTAssertEqual(MemoryLayout<CyTime>.offset(of: \CyTime.tick), 8, "CyTime.tick offset")
        XCTAssertEqual(MemoryLayout<CyTime>.offset(of: \CyTime.fixed_delta), 16, "CyTime.fixed_delta offset")
        XCTAssertEqual(MemoryLayout<CyTime>.offset(of: \CyTime.frame_delta), 24, "CyTime.frame_delta offset")
        XCTAssertEqual(MemoryLayout<CyTime>.offset(of: \CyTime.interpolation), 32, "CyTime.interpolation offset")
        XCTAssertEqual(MemoryLayout<CyTime>.offset(of: \CyTime.flags), 40, "CyTime.flags offset")
        XCTAssertEqual(MemoryLayout<CyTime>.offset(of: \CyTime.reserved), 44, "CyTime.reserved offset")
    }

    func testInputActionStateLayout() {
        XCTAssertEqual(MemoryLayout<CyInputActionState>.size, 32, "CyInputActionState size")
        XCTAssertEqual(MemoryLayout<CyInputActionState>.alignment, 8, "CyInputActionState alignment")
        XCTAssertEqual(MemoryLayout<CyInputActionState>.offset(of: \CyInputActionState.struct_size), 0, "CyInputActionState.struct_size offset")
        XCTAssertEqual(MemoryLayout<CyInputActionState>.offset(of: \CyInputActionState.flags), 4, "CyInputActionState.flags offset")
        XCTAssertEqual(MemoryLayout<CyInputActionState>.offset(of: \CyInputActionState.value), 8, "CyInputActionState.value offset")
        XCTAssertEqual(MemoryLayout<CyInputActionState>.offset(of: \CyInputActionState.press_count), 20, "CyInputActionState.press_count offset")
        XCTAssertEqual(MemoryLayout<CyInputActionState>.offset(of: \CyInputActionState.release_count), 22, "CyInputActionState.release_count offset")
        XCTAssertEqual(MemoryLayout<CyInputActionState>.offset(of: \CyInputActionState.tick), 24, "CyInputActionState.tick offset")
    }

    func testInputPointerLayout() {
        XCTAssertEqual(MemoryLayout<CyInputPointer>.size, 48, "CyInputPointer size")
        XCTAssertEqual(MemoryLayout<CyInputPointer>.alignment, 4, "CyInputPointer alignment")
        XCTAssertEqual(MemoryLayout<CyInputPointer>.offset(of: \CyInputPointer.struct_size), 0, "CyInputPointer.struct_size offset")
        XCTAssertEqual(MemoryLayout<CyInputPointer>.offset(of: \CyInputPointer.flags), 4, "CyInputPointer.flags offset")
        XCTAssertEqual(MemoryLayout<CyInputPointer>.offset(of: \CyInputPointer.buttons), 8, "CyInputPointer.buttons offset")
        XCTAssertEqual(MemoryLayout<CyInputPointer>.offset(of: \CyInputPointer.buttons_pressed), 12, "CyInputPointer.buttons_pressed offset")
        XCTAssertEqual(MemoryLayout<CyInputPointer>.offset(of: \CyInputPointer.buttons_released), 16, "CyInputPointer.buttons_released offset")
        XCTAssertEqual(MemoryLayout<CyInputPointer>.offset(of: \CyInputPointer.position), 20, "CyInputPointer.position offset")
        XCTAssertEqual(MemoryLayout<CyInputPointer>.offset(of: \CyInputPointer.delta), 28, "CyInputPointer.delta offset")
        XCTAssertEqual(MemoryLayout<CyInputPointer>.offset(of: \CyInputPointer.wheel), 36, "CyInputPointer.wheel offset")
        XCTAssertEqual(MemoryLayout<CyInputPointer>.offset(of: \CyInputPointer.reserved), 44, "CyInputPointer.reserved offset")
    }

    func testCameraViewLayout() {
        XCTAssertEqual(MemoryLayout<CyCameraView>.size, 68, "CyCameraView size")
        XCTAssertEqual(MemoryLayout<CyCameraView>.alignment, 4, "CyCameraView alignment")
        XCTAssertEqual(MemoryLayout<CyCameraView>.offset(of: \CyCameraView.struct_size), 0, "CyCameraView.struct_size offset")
        XCTAssertEqual(MemoryLayout<CyCameraView>.offset(of: \CyCameraView.flags), 4, "CyCameraView.flags offset")
        XCTAssertEqual(MemoryLayout<CyCameraView>.offset(of: \CyCameraView.pose), 8, "CyCameraView.pose offset")
        XCTAssertEqual(MemoryLayout<CyCameraView>.offset(of: \CyCameraView.vertical_fov), 36, "CyCameraView.vertical_fov offset")
        XCTAssertEqual(MemoryLayout<CyCameraView>.offset(of: \CyCameraView.ortho_height), 40, "CyCameraView.ortho_height offset")
        XCTAssertEqual(MemoryLayout<CyCameraView>.offset(of: \CyCameraView.near_plane), 44, "CyCameraView.near_plane offset")
        XCTAssertEqual(MemoryLayout<CyCameraView>.offset(of: \CyCameraView.far_plane), 48, "CyCameraView.far_plane offset")
        XCTAssertEqual(MemoryLayout<CyCameraView>.offset(of: \CyCameraView.viewport), 52, "CyCameraView.viewport offset")
    }

    func testScreenPointLayout() {
        XCTAssertEqual(MemoryLayout<CyScreenPoint>.size, 16, "CyScreenPoint size")
        XCTAssertEqual(MemoryLayout<CyScreenPoint>.alignment, 4, "CyScreenPoint alignment")
        XCTAssertEqual(MemoryLayout<CyScreenPoint>.offset(of: \CyScreenPoint.position), 0, "CyScreenPoint.position offset")
        XCTAssertEqual(MemoryLayout<CyScreenPoint>.offset(of: \CyScreenPoint.depth), 8, "CyScreenPoint.depth offset")
        XCTAssertEqual(MemoryLayout<CyScreenPoint>.offset(of: \CyScreenPoint.flags), 12, "CyScreenPoint.flags offset")
    }

    func testCameraTargetLayout() {
        XCTAssertEqual(MemoryLayout<CyCameraTarget>.size, 48, "CyCameraTarget size")
        XCTAssertEqual(MemoryLayout<CyCameraTarget>.alignment, 8, "CyCameraTarget alignment")
        XCTAssertEqual(MemoryLayout<CyCameraTarget>.offset(of: \CyCameraTarget.struct_size), 0, "CyCameraTarget.struct_size offset")
        XCTAssertEqual(MemoryLayout<CyCameraTarget>.offset(of: \CyCameraTarget.flags), 4, "CyCameraTarget.flags offset")
        XCTAssertEqual(MemoryLayout<CyCameraTarget>.offset(of: \CyCameraTarget.entity), 8, "CyCameraTarget.entity offset")
        XCTAssertEqual(MemoryLayout<CyCameraTarget>.offset(of: \CyCameraTarget.position), 16, "CyCameraTarget.position offset")
        XCTAssertEqual(MemoryLayout<CyCameraTarget>.offset(of: \CyCameraTarget.yaw), 28, "CyCameraTarget.yaw offset")
        XCTAssertEqual(MemoryLayout<CyCameraTarget>.offset(of: \CyCameraTarget.pitch), 32, "CyCameraTarget.pitch offset")
        XCTAssertEqual(MemoryLayout<CyCameraTarget>.offset(of: \CyCameraTarget.distance), 36, "CyCameraTarget.distance offset")
        XCTAssertEqual(MemoryLayout<CyCameraTarget>.offset(of: \CyCameraTarget.blend_seconds), 40, "CyCameraTarget.blend_seconds offset")
        XCTAssertEqual(MemoryLayout<CyCameraTarget>.offset(of: \CyCameraTarget.reserved), 44, "CyCameraTarget.reserved offset")
    }

    func testShapeLayout() {
        XCTAssertEqual(MemoryLayout<CyShape>.size, 24, "CyShape size")
        XCTAssertEqual(MemoryLayout<CyShape>.alignment, 4, "CyShape alignment")
        XCTAssertEqual(MemoryLayout<CyShape>.offset(of: \CyShape.kind), 0, "CyShape.kind offset")
        XCTAssertEqual(MemoryLayout<CyShape>.offset(of: \CyShape.radius), 4, "CyShape.radius offset")
        XCTAssertEqual(MemoryLayout<CyShape>.offset(of: \CyShape.half_height), 8, "CyShape.half_height offset")
        XCTAssertEqual(MemoryLayout<CyShape>.offset(of: \CyShape.half_extents), 12, "CyShape.half_extents offset")
    }

    func testQueryFilterLayout() {
        XCTAssertEqual(MemoryLayout<CyQueryFilter>.size, 32, "CyQueryFilter size")
        XCTAssertEqual(MemoryLayout<CyQueryFilter>.alignment, 8, "CyQueryFilter alignment")
        XCTAssertEqual(MemoryLayout<CyQueryFilter>.offset(of: \CyQueryFilter.struct_size), 0, "CyQueryFilter.struct_size offset")
        XCTAssertEqual(MemoryLayout<CyQueryFilter>.offset(of: \CyQueryFilter.layer), 4, "CyQueryFilter.layer offset")
        XCTAssertEqual(MemoryLayout<CyQueryFilter>.offset(of: \CyQueryFilter.mask), 8, "CyQueryFilter.mask offset")
        XCTAssertEqual(MemoryLayout<CyQueryFilter>.offset(of: \CyQueryFilter.flags), 12, "CyQueryFilter.flags offset")
        XCTAssertEqual(MemoryLayout<CyQueryFilter>.offset(of: \CyQueryFilter.ignore), 16, "CyQueryFilter.ignore offset")
        XCTAssertEqual(MemoryLayout<CyQueryFilter>.offset(of: \CyQueryFilter.ignore_count), 24, "CyQueryFilter.ignore_count offset")
        XCTAssertEqual(MemoryLayout<CyQueryFilter>.offset(of: \CyQueryFilter.reserved), 28, "CyQueryFilter.reserved offset")
    }

    func testPhysicsHitLayout() {
        XCTAssertEqual(MemoryLayout<CyPhysicsHit>.size, 48, "CyPhysicsHit size")
        XCTAssertEqual(MemoryLayout<CyPhysicsHit>.alignment, 8, "CyPhysicsHit alignment")
        XCTAssertEqual(MemoryLayout<CyPhysicsHit>.offset(of: \CyPhysicsHit.flags), 0, "CyPhysicsHit.flags offset")
        XCTAssertEqual(MemoryLayout<CyPhysicsHit>.offset(of: \CyPhysicsHit.reserved), 4, "CyPhysicsHit.reserved offset")
        XCTAssertEqual(MemoryLayout<CyPhysicsHit>.offset(of: \CyPhysicsHit.entity), 8, "CyPhysicsHit.entity offset")
        XCTAssertEqual(MemoryLayout<CyPhysicsHit>.offset(of: \CyPhysicsHit.point), 16, "CyPhysicsHit.point offset")
        XCTAssertEqual(MemoryLayout<CyPhysicsHit>.offset(of: \CyPhysicsHit.normal), 28, "CyPhysicsHit.normal offset")
        XCTAssertEqual(MemoryLayout<CyPhysicsHit>.offset(of: \CyPhysicsHit.distance), 40, "CyPhysicsHit.distance offset")
        XCTAssertEqual(MemoryLayout<CyPhysicsHit>.offset(of: \CyPhysicsHit.fraction), 44, "CyPhysicsHit.fraction offset")
    }

    func testNavPathRequestLayout() {
        XCTAssertEqual(MemoryLayout<CyNavPathRequest>.size, 64, "CyNavPathRequest size")
        XCTAssertEqual(MemoryLayout<CyNavPathRequest>.alignment, 8, "CyNavPathRequest alignment")
        XCTAssertEqual(MemoryLayout<CyNavPathRequest>.offset(of: \CyNavPathRequest.struct_size), 0, "CyNavPathRequest.struct_size offset")
        XCTAssertEqual(MemoryLayout<CyNavPathRequest>.offset(of: \CyNavPathRequest.world), 4, "CyNavPathRequest.world offset")
        XCTAssertEqual(MemoryLayout<CyNavPathRequest>.offset(of: \CyNavPathRequest.start), 8, "CyNavPathRequest.start offset")
        XCTAssertEqual(MemoryLayout<CyNavPathRequest>.offset(of: \CyNavPathRequest.end), 20, "CyNavPathRequest.end offset")
        XCTAssertEqual(MemoryLayout<CyNavPathRequest>.offset(of: \CyNavPathRequest.extents), 32, "CyNavPathRequest.extents offset")
        XCTAssertEqual(MemoryLayout<CyNavPathRequest>.offset(of: \CyNavPathRequest.node_budget), 44, "CyNavPathRequest.node_budget offset")
        XCTAssertEqual(MemoryLayout<CyNavPathRequest>.offset(of: \CyNavPathRequest.area_mask), 48, "CyNavPathRequest.area_mask offset")
        XCTAssertEqual(MemoryLayout<CyNavPathRequest>.offset(of: \CyNavPathRequest.capabilities), 56, "CyNavPathRequest.capabilities offset")
    }

    func testNavPathResultLayout() {
        XCTAssertEqual(MemoryLayout<CyNavPathResult>.size, 24, "CyNavPathResult size")
        XCTAssertEqual(MemoryLayout<CyNavPathResult>.alignment, 4, "CyNavPathResult alignment")
        XCTAssertEqual(MemoryLayout<CyNavPathResult>.offset(of: \CyNavPathResult.struct_size), 0, "CyNavPathResult.struct_size offset")
        XCTAssertEqual(MemoryLayout<CyNavPathResult>.offset(of: \CyNavPathResult.flags), 4, "CyNavPathResult.flags offset")
        XCTAssertEqual(MemoryLayout<CyNavPathResult>.offset(of: \CyNavPathResult.point_count), 8, "CyNavPathResult.point_count offset")
        XCTAssertEqual(MemoryLayout<CyNavPathResult>.offset(of: \CyNavPathResult.state), 12, "CyNavPathResult.state offset")
        XCTAssertEqual(MemoryLayout<CyNavPathResult>.offset(of: \CyNavPathResult.cost), 16, "CyNavPathResult.cost offset")
        XCTAssertEqual(MemoryLayout<CyNavPathResult>.offset(of: \CyNavPathResult.length), 20, "CyNavPathResult.length offset")
    }

    func testNavAgentParamsLayout() {
        XCTAssertEqual(MemoryLayout<CyNavAgentParams>.size, 48, "CyNavAgentParams size")
        XCTAssertEqual(MemoryLayout<CyNavAgentParams>.alignment, 8, "CyNavAgentParams alignment")
        XCTAssertEqual(MemoryLayout<CyNavAgentParams>.offset(of: \CyNavAgentParams.struct_size), 0, "CyNavAgentParams.struct_size offset")
        XCTAssertEqual(MemoryLayout<CyNavAgentParams>.offset(of: \CyNavAgentParams.world), 4, "CyNavAgentParams.world offset")
        XCTAssertEqual(MemoryLayout<CyNavAgentParams>.offset(of: \CyNavAgentParams.radius), 8, "CyNavAgentParams.radius offset")
        XCTAssertEqual(MemoryLayout<CyNavAgentParams>.offset(of: \CyNavAgentParams.height), 12, "CyNavAgentParams.height offset")
        XCTAssertEqual(MemoryLayout<CyNavAgentParams>.offset(of: \CyNavAgentParams.max_speed), 16, "CyNavAgentParams.max_speed offset")
        XCTAssertEqual(MemoryLayout<CyNavAgentParams>.offset(of: \CyNavAgentParams.max_acceleration), 20, "CyNavAgentParams.max_acceleration offset")
        XCTAssertEqual(MemoryLayout<CyNavAgentParams>.offset(of: \CyNavAgentParams.arrival_distance), 24, "CyNavAgentParams.arrival_distance offset")
        XCTAssertEqual(MemoryLayout<CyNavAgentParams>.offset(of: \CyNavAgentParams.priority), 28, "CyNavAgentParams.priority offset")
        XCTAssertEqual(MemoryLayout<CyNavAgentParams>.offset(of: \CyNavAgentParams.area_mask), 32, "CyNavAgentParams.area_mask offset")
        XCTAssertEqual(MemoryLayout<CyNavAgentParams>.offset(of: \CyNavAgentParams.capabilities), 40, "CyNavAgentParams.capabilities offset")
    }

    func testNavAgentStateLayout() {
        XCTAssertEqual(MemoryLayout<CyNavAgentState>.size, 56, "CyNavAgentState size")
        XCTAssertEqual(MemoryLayout<CyNavAgentState>.alignment, 4, "CyNavAgentState alignment")
        XCTAssertEqual(MemoryLayout<CyNavAgentState>.offset(of: \CyNavAgentState.struct_size), 0, "CyNavAgentState.struct_size offset")
        XCTAssertEqual(MemoryLayout<CyNavAgentState>.offset(of: \CyNavAgentState.status), 4, "CyNavAgentState.status offset")
        XCTAssertEqual(MemoryLayout<CyNavAgentState>.offset(of: \CyNavAgentState.flags), 8, "CyNavAgentState.flags offset")
        XCTAssertEqual(MemoryLayout<CyNavAgentState>.offset(of: \CyNavAgentState.reserved), 12, "CyNavAgentState.reserved offset")
        XCTAssertEqual(MemoryLayout<CyNavAgentState>.offset(of: \CyNavAgentState.position), 16, "CyNavAgentState.position offset")
        XCTAssertEqual(MemoryLayout<CyNavAgentState>.offset(of: \CyNavAgentState.velocity), 28, "CyNavAgentState.velocity offset")
        XCTAssertEqual(MemoryLayout<CyNavAgentState>.offset(of: \CyNavAgentState.target), 40, "CyNavAgentState.target offset")
        XCTAssertEqual(MemoryLayout<CyNavAgentState>.offset(of: \CyNavAgentState.remaining_distance), 52, "CyNavAgentState.remaining_distance offset")
    }

    func testAudioPlayLayout() {
        XCTAssertEqual(MemoryLayout<CyAudioPlay>.size, 56, "CyAudioPlay size")
        XCTAssertEqual(MemoryLayout<CyAudioPlay>.alignment, 8, "CyAudioPlay alignment")
        XCTAssertEqual(MemoryLayout<CyAudioPlay>.offset(of: \CyAudioPlay.struct_size), 0, "CyAudioPlay.struct_size offset")
        XCTAssertEqual(MemoryLayout<CyAudioPlay>.offset(of: \CyAudioPlay.flags), 4, "CyAudioPlay.flags offset")
        XCTAssertEqual(MemoryLayout<CyAudioPlay>.offset(of: \CyAudioPlay.cue), 8, "CyAudioPlay.cue offset")
        XCTAssertEqual(MemoryLayout<CyAudioPlay>.offset(of: \CyAudioPlay.attach_to), 16, "CyAudioPlay.attach_to offset")
        XCTAssertEqual(MemoryLayout<CyAudioPlay>.offset(of: \CyAudioPlay.bus), 24, "CyAudioPlay.bus offset")
        XCTAssertEqual(MemoryLayout<CyAudioPlay>.offset(of: \CyAudioPlay.position), 32, "CyAudioPlay.position offset")
        XCTAssertEqual(MemoryLayout<CyAudioPlay>.offset(of: \CyAudioPlay.volume), 44, "CyAudioPlay.volume offset")
        XCTAssertEqual(MemoryLayout<CyAudioPlay>.offset(of: \CyAudioPlay.pitch), 48, "CyAudioPlay.pitch offset")
        XCTAssertEqual(MemoryLayout<CyAudioPlay>.offset(of: \CyAudioPlay.fade_in_seconds), 52, "CyAudioPlay.fade_in_seconds offset")
    }

    func testSpawnParamsLayout() {
        XCTAssertEqual(MemoryLayout<CySpawnParams>.size, 56, "CySpawnParams size")
        XCTAssertEqual(MemoryLayout<CySpawnParams>.alignment, 8, "CySpawnParams alignment")
        XCTAssertEqual(MemoryLayout<CySpawnParams>.offset(of: \CySpawnParams.struct_size), 0, "CySpawnParams.struct_size offset")
        XCTAssertEqual(MemoryLayout<CySpawnParams>.offset(of: \CySpawnParams.flags), 4, "CySpawnParams.flags offset")
        XCTAssertEqual(MemoryLayout<CySpawnParams>.offset(of: \CySpawnParams.parent), 8, "CySpawnParams.parent offset")
        XCTAssertEqual(MemoryLayout<CySpawnParams>.offset(of: \CySpawnParams.pose), 16, "CySpawnParams.pose offset")
        XCTAssertEqual(MemoryLayout<CySpawnParams>.offset(of: \CySpawnParams.scale), 44, "CySpawnParams.scale offset")
    }

    func testSystemAccessLayout() {
        XCTAssertEqual(MemoryLayout<CySystemAccess>.size, 8, "CySystemAccess size")
        XCTAssertEqual(MemoryLayout<CySystemAccess>.alignment, 4, "CySystemAccess alignment")
        XCTAssertEqual(MemoryLayout<CySystemAccess>.offset(of: \CySystemAccess.component), 0, "CySystemAccess.component offset")
        XCTAssertEqual(MemoryLayout<CySystemAccess>.offset(of: \CySystemAccess.mode), 4, "CySystemAccess.mode offset")
    }

    func testSystemDescLayout() {
        XCTAssertEqual(MemoryLayout<CySystemDesc>.size, 48, "CySystemDesc size")
        XCTAssertEqual(MemoryLayout<CySystemDesc>.alignment, 8, "CySystemDesc alignment")
        XCTAssertEqual(MemoryLayout<CySystemDesc>.offset(of: \CySystemDesc.struct_size), 0, "CySystemDesc.struct_size offset")
        XCTAssertEqual(MemoryLayout<CySystemDesc>.offset(of: \CySystemDesc.stage), 4, "CySystemDesc.stage offset")
        XCTAssertEqual(MemoryLayout<CySystemDesc>.offset(of: \CySystemDesc.name), 8, "CySystemDesc.name offset")
        XCTAssertEqual(MemoryLayout<CySystemDesc>.offset(of: \CySystemDesc.access), 16, "CySystemDesc.access offset")
        XCTAssertEqual(MemoryLayout<CySystemDesc>.offset(of: \CySystemDesc.access_count), 24, "CySystemDesc.access_count offset")
        XCTAssertEqual(MemoryLayout<CySystemDesc>.offset(of: \CySystemDesc.reserved), 28, "CySystemDesc.reserved offset")
        XCTAssertEqual(MemoryLayout<CySystemDesc>.offset(of: \CySystemDesc.run), 32, "CySystemDesc.run offset")
        XCTAssertEqual(MemoryLayout<CySystemDesc>.offset(of: \CySystemDesc.user_data), 40, "CySystemDesc.user_data offset")
    }

    func testCharacterDescLayout() {
        XCTAssertEqual(MemoryLayout<CyCharacterDesc>.size, 76, "CyCharacterDesc size")
        XCTAssertEqual(MemoryLayout<CyCharacterDesc>.alignment, 4, "CyCharacterDesc alignment")
        XCTAssertEqual(MemoryLayout<CyCharacterDesc>.offset(of: \CyCharacterDesc.struct_size), 0, "CyCharacterDesc.struct_size offset")
        XCTAssertEqual(MemoryLayout<CyCharacterDesc>.offset(of: \CyCharacterDesc.flags), 4, "CyCharacterDesc.flags offset")
        XCTAssertEqual(MemoryLayout<CyCharacterDesc>.offset(of: \CyCharacterDesc.radius), 8, "CyCharacterDesc.radius offset")
        XCTAssertEqual(MemoryLayout<CyCharacterDesc>.offset(of: \CyCharacterDesc.height), 12, "CyCharacterDesc.height offset")
        XCTAssertEqual(MemoryLayout<CyCharacterDesc>.offset(of: \CyCharacterDesc.max_slope_radians), 16, "CyCharacterDesc.max_slope_radians offset")
        XCTAssertEqual(MemoryLayout<CyCharacterDesc>.offset(of: \CyCharacterDesc.step_offset), 20, "CyCharacterDesc.step_offset offset")
        XCTAssertEqual(MemoryLayout<CyCharacterDesc>.offset(of: \CyCharacterDesc.skin_width), 24, "CyCharacterDesc.skin_width offset")
        XCTAssertEqual(MemoryLayout<CyCharacterDesc>.offset(of: \CyCharacterDesc.gravity_scale), 28, "CyCharacterDesc.gravity_scale offset")
        XCTAssertEqual(MemoryLayout<CyCharacterDesc>.offset(of: \CyCharacterDesc.mass), 32, "CyCharacterDesc.mass offset")
        XCTAssertEqual(MemoryLayout<CyCharacterDesc>.offset(of: \CyCharacterDesc.push_force), 36, "CyCharacterDesc.push_force offset")
        XCTAssertEqual(MemoryLayout<CyCharacterDesc>.offset(of: \CyCharacterDesc.layer), 40, "CyCharacterDesc.layer offset")
        XCTAssertEqual(MemoryLayout<CyCharacterDesc>.offset(of: \CyCharacterDesc.mask), 44, "CyCharacterDesc.mask offset")
        XCTAssertEqual(MemoryLayout<CyCharacterDesc>.offset(of: \CyCharacterDesc.start), 48, "CyCharacterDesc.start offset")
    }

    func testCharacterInputLayout() {
        XCTAssertEqual(MemoryLayout<CyCharacterInput>.size, 24, "CyCharacterInput size")
        XCTAssertEqual(MemoryLayout<CyCharacterInput>.alignment, 4, "CyCharacterInput alignment")
        XCTAssertEqual(MemoryLayout<CyCharacterInput>.offset(of: \CyCharacterInput.struct_size), 0, "CyCharacterInput.struct_size offset")
        XCTAssertEqual(MemoryLayout<CyCharacterInput>.offset(of: \CyCharacterInput.flags), 4, "CyCharacterInput.flags offset")
        XCTAssertEqual(MemoryLayout<CyCharacterInput>.offset(of: \CyCharacterInput.desired_velocity), 8, "CyCharacterInput.desired_velocity offset")
        XCTAssertEqual(MemoryLayout<CyCharacterInput>.offset(of: \CyCharacterInput.jump_speed), 20, "CyCharacterInput.jump_speed offset")
    }

    func testCharacterStateLayout() {
        XCTAssertEqual(MemoryLayout<CyCharacterState>.size, 72, "CyCharacterState size")
        XCTAssertEqual(MemoryLayout<CyCharacterState>.alignment, 8, "CyCharacterState alignment")
        XCTAssertEqual(MemoryLayout<CyCharacterState>.offset(of: \CyCharacterState.struct_size), 0, "CyCharacterState.struct_size offset")
        XCTAssertEqual(MemoryLayout<CyCharacterState>.offset(of: \CyCharacterState.ground), 4, "CyCharacterState.ground offset")
        XCTAssertEqual(MemoryLayout<CyCharacterState>.offset(of: \CyCharacterState.flags), 8, "CyCharacterState.flags offset")
        XCTAssertEqual(MemoryLayout<CyCharacterState>.offset(of: \CyCharacterState.reserved), 12, "CyCharacterState.reserved offset")
        XCTAssertEqual(MemoryLayout<CyCharacterState>.offset(of: \CyCharacterState.ground_entity), 16, "CyCharacterState.ground_entity offset")
        XCTAssertEqual(MemoryLayout<CyCharacterState>.offset(of: \CyCharacterState.position), 24, "CyCharacterState.position offset")
        XCTAssertEqual(MemoryLayout<CyCharacterState>.offset(of: \CyCharacterState.velocity), 36, "CyCharacterState.velocity offset")
        XCTAssertEqual(MemoryLayout<CyCharacterState>.offset(of: \CyCharacterState.ground_normal), 48, "CyCharacterState.ground_normal offset")
        XCTAssertEqual(MemoryLayout<CyCharacterState>.offset(of: \CyCharacterState.platform_velocity), 60, "CyCharacterState.platform_velocity offset")
    }

    func testUiElementDescLayout() {
        XCTAssertEqual(MemoryLayout<CyUiElementDesc>.size, 24, "CyUiElementDesc size")
        XCTAssertEqual(MemoryLayout<CyUiElementDesc>.alignment, 8, "CyUiElementDesc alignment")
        XCTAssertEqual(MemoryLayout<CyUiElementDesc>.offset(of: \CyUiElementDesc.struct_size), 0, "CyUiElementDesc.struct_size offset")
        XCTAssertEqual(MemoryLayout<CyUiElementDesc>.offset(of: \CyUiElementDesc.kind), 4, "CyUiElementDesc.kind offset")
        XCTAssertEqual(MemoryLayout<CyUiElementDesc>.offset(of: \CyUiElementDesc.name), 8, "CyUiElementDesc.name offset")
        XCTAssertEqual(MemoryLayout<CyUiElementDesc>.offset(of: \CyUiElementDesc.owner), 16, "CyUiElementDesc.owner offset")
    }

    func testUiLayoutLayout() {
        XCTAssertEqual(MemoryLayout<CyUiLayout>.size, 144, "CyUiLayout size")
        XCTAssertEqual(MemoryLayout<CyUiLayout>.alignment, 4, "CyUiLayout alignment")
        XCTAssertEqual(MemoryLayout<CyUiLayout>.offset(of: \CyUiLayout.struct_size), 0, "CyUiLayout.struct_size offset")
        XCTAssertEqual(MemoryLayout<CyUiLayout>.offset(of: \CyUiLayout.model), 4, "CyUiLayout.model offset")
        XCTAssertEqual(MemoryLayout<CyUiLayout>.offset(of: \CyUiLayout.direction), 8, "CyUiLayout.direction offset")
        XCTAssertEqual(MemoryLayout<CyUiLayout>.offset(of: \CyUiLayout.justify), 12, "CyUiLayout.justify offset")
        XCTAssertEqual(MemoryLayout<CyUiLayout>.offset(of: \CyUiLayout.align), 16, "CyUiLayout.align offset")
        XCTAssertEqual(MemoryLayout<CyUiLayout>.offset(of: \CyUiLayout.self_align), 20, "CyUiLayout.self_align offset")
        XCTAssertEqual(MemoryLayout<CyUiLayout>.offset(of: \CyUiLayout.flags), 24, "CyUiLayout.flags offset")
        XCTAssertEqual(MemoryLayout<CyUiLayout>.offset(of: \CyUiLayout.gap), 28, "CyUiLayout.gap offset")
        XCTAssertEqual(MemoryLayout<CyUiLayout>.offset(of: \CyUiLayout.preferred), 32, "CyUiLayout.preferred offset")
        XCTAssertEqual(MemoryLayout<CyUiLayout>.offset(of: \CyUiLayout.minimum), 40, "CyUiLayout.minimum offset")
        XCTAssertEqual(MemoryLayout<CyUiLayout>.offset(of: \CyUiLayout.maximum), 48, "CyUiLayout.maximum offset")
        XCTAssertEqual(MemoryLayout<CyUiLayout>.offset(of: \CyUiLayout.margin), 56, "CyUiLayout.margin offset")
        XCTAssertEqual(MemoryLayout<CyUiLayout>.offset(of: \CyUiLayout.padding), 72, "CyUiLayout.padding offset")
        XCTAssertEqual(MemoryLayout<CyUiLayout>.offset(of: \CyUiLayout.flex_grow), 88, "CyUiLayout.flex_grow offset")
        XCTAssertEqual(MemoryLayout<CyUiLayout>.offset(of: \CyUiLayout.flex_shrink), 92, "CyUiLayout.flex_shrink offset")
        XCTAssertEqual(MemoryLayout<CyUiLayout>.offset(of: \CyUiLayout.aspect_ratio), 96, "CyUiLayout.aspect_ratio offset")
        XCTAssertEqual(MemoryLayout<CyUiLayout>.offset(of: \CyUiLayout.anchor_min), 100, "CyUiLayout.anchor_min offset")
        XCTAssertEqual(MemoryLayout<CyUiLayout>.offset(of: \CyUiLayout.anchor_max), 108, "CyUiLayout.anchor_max offset")
        XCTAssertEqual(MemoryLayout<CyUiLayout>.offset(of: \CyUiLayout.offset_min), 116, "CyUiLayout.offset_min offset")
        XCTAssertEqual(MemoryLayout<CyUiLayout>.offset(of: \CyUiLayout.offset_max), 124, "CyUiLayout.offset_max offset")
        XCTAssertEqual(MemoryLayout<CyUiLayout>.offset(of: \CyUiLayout.grid_column), 132, "CyUiLayout.grid_column offset")
        XCTAssertEqual(MemoryLayout<CyUiLayout>.offset(of: \CyUiLayout.grid_row), 134, "CyUiLayout.grid_row offset")
        XCTAssertEqual(MemoryLayout<CyUiLayout>.offset(of: \CyUiLayout.grid_column_span), 136, "CyUiLayout.grid_column_span offset")
        XCTAssertEqual(MemoryLayout<CyUiLayout>.offset(of: \CyUiLayout.grid_row_span), 138, "CyUiLayout.grid_row_span offset")
        XCTAssertEqual(MemoryLayout<CyUiLayout>.offset(of: \CyUiLayout.grid_columns), 140, "CyUiLayout.grid_columns offset")
        XCTAssertEqual(MemoryLayout<CyUiLayout>.offset(of: \CyUiLayout.reserved), 142, "CyUiLayout.reserved offset")
    }

    func testUiStyleLayout() {
        XCTAssertEqual(MemoryLayout<CyUiStyle>.size, 32, "CyUiStyle size")
        XCTAssertEqual(MemoryLayout<CyUiStyle>.alignment, 4, "CyUiStyle alignment")
        XCTAssertEqual(MemoryLayout<CyUiStyle>.offset(of: \CyUiStyle.struct_size), 0, "CyUiStyle.struct_size offset")
        XCTAssertEqual(MemoryLayout<CyUiStyle>.offset(of: \CyUiStyle.flags), 4, "CyUiStyle.flags offset")
        XCTAssertEqual(MemoryLayout<CyUiStyle>.offset(of: \CyUiStyle.background), 8, "CyUiStyle.background offset")
        XCTAssertEqual(MemoryLayout<CyUiStyle>.offset(of: \CyUiStyle.border_colour), 12, "CyUiStyle.border_colour offset")
        XCTAssertEqual(MemoryLayout<CyUiStyle>.offset(of: \CyUiStyle.accent), 16, "CyUiStyle.accent offset")
        XCTAssertEqual(MemoryLayout<CyUiStyle>.offset(of: \CyUiStyle.border_width), 20, "CyUiStyle.border_width offset")
        XCTAssertEqual(MemoryLayout<CyUiStyle>.offset(of: \CyUiStyle.corner_radius), 24, "CyUiStyle.corner_radius offset")
        XCTAssertEqual(MemoryLayout<CyUiStyle>.offset(of: \CyUiStyle.reserved), 28, "CyUiStyle.reserved offset")
    }

    func testAnimatorDescLayout() {
        XCTAssertEqual(MemoryLayout<CyAnimatorDesc>.size, 32, "CyAnimatorDesc size")
        XCTAssertEqual(MemoryLayout<CyAnimatorDesc>.alignment, 8, "CyAnimatorDesc alignment")
        XCTAssertEqual(MemoryLayout<CyAnimatorDesc>.offset(of: \CyAnimatorDesc.struct_size), 0, "CyAnimatorDesc.struct_size offset")
        XCTAssertEqual(MemoryLayout<CyAnimatorDesc>.offset(of: \CyAnimatorDesc.flags), 4, "CyAnimatorDesc.flags offset")
        XCTAssertEqual(MemoryLayout<CyAnimatorDesc>.offset(of: \CyAnimatorDesc.rig), 8, "CyAnimatorDesc.rig offset")
        XCTAssertEqual(MemoryLayout<CyAnimatorDesc>.offset(of: \CyAnimatorDesc.tier), 16, "CyAnimatorDesc.tier offset")
        XCTAssertEqual(MemoryLayout<CyAnimatorDesc>.offset(of: \CyAnimatorDesc.root_motion), 20, "CyAnimatorDesc.root_motion offset")
        XCTAssertEqual(MemoryLayout<CyAnimatorDesc>.offset(of: \CyAnimatorDesc.play_rate), 24, "CyAnimatorDesc.play_rate offset")
        XCTAssertEqual(MemoryLayout<CyAnimatorDesc>.offset(of: \CyAnimatorDesc.reserved), 28, "CyAnimatorDesc.reserved offset")
    }

    func testAnimatorStateLayout() {
        XCTAssertEqual(MemoryLayout<CyAnimatorState>.size, 32, "CyAnimatorState size")
        XCTAssertEqual(MemoryLayout<CyAnimatorState>.alignment, 8, "CyAnimatorState alignment")
        XCTAssertEqual(MemoryLayout<CyAnimatorState>.offset(of: \CyAnimatorState.struct_size), 0, "CyAnimatorState.struct_size offset")
        XCTAssertEqual(MemoryLayout<CyAnimatorState>.offset(of: \CyAnimatorState.flags), 4, "CyAnimatorState.flags offset")
        XCTAssertEqual(MemoryLayout<CyAnimatorState>.offset(of: \CyAnimatorState.state), 8, "CyAnimatorState.state offset")
        XCTAssertEqual(MemoryLayout<CyAnimatorState>.offset(of: \CyAnimatorState.target), 16, "CyAnimatorState.target offset")
        XCTAssertEqual(MemoryLayout<CyAnimatorState>.offset(of: \CyAnimatorState.blend_weight), 24, "CyAnimatorState.blend_weight offset")
        XCTAssertEqual(MemoryLayout<CyAnimatorState>.offset(of: \CyAnimatorState.state_time), 28, "CyAnimatorState.state_time offset")
    }

    func testAnimationEventLayout() {
        XCTAssertEqual(MemoryLayout<CyAnimationEvent>.size, 24, "CyAnimationEvent size")
        XCTAssertEqual(MemoryLayout<CyAnimationEvent>.alignment, 8, "CyAnimationEvent alignment")
        XCTAssertEqual(MemoryLayout<CyAnimationEvent>.offset(of: \CyAnimationEvent.entity), 0, "CyAnimationEvent.entity offset")
        XCTAssertEqual(MemoryLayout<CyAnimationEvent>.offset(of: \CyAnimationEvent.name), 8, "CyAnimationEvent.name offset")
        XCTAssertEqual(MemoryLayout<CyAnimationEvent>.offset(of: \CyAnimationEvent.normalised_time), 16, "CyAnimationEvent.normalised_time offset")
        XCTAssertEqual(MemoryLayout<CyAnimationEvent>.offset(of: \CyAnimationEvent.parameter), 20, "CyAnimationEvent.parameter offset")
    }

    func testRootMotionLayout() {
        XCTAssertEqual(MemoryLayout<CyRootMotion>.size, 52, "CyRootMotion size")
        XCTAssertEqual(MemoryLayout<CyRootMotion>.alignment, 4, "CyRootMotion alignment")
        XCTAssertEqual(MemoryLayout<CyRootMotion>.offset(of: \CyRootMotion.struct_size), 0, "CyRootMotion.struct_size offset")
        XCTAssertEqual(MemoryLayout<CyRootMotion>.offset(of: \CyRootMotion.contacts), 4, "CyRootMotion.contacts offset")
        XCTAssertEqual(MemoryLayout<CyRootMotion>.offset(of: \CyRootMotion.translation), 8, "CyRootMotion.translation offset")
        XCTAssertEqual(MemoryLayout<CyRootMotion>.offset(of: \CyRootMotion.rotation), 20, "CyRootMotion.rotation offset")
        XCTAssertEqual(MemoryLayout<CyRootMotion>.offset(of: \CyRootMotion.distance), 36, "CyRootMotion.distance offset")
        XCTAssertEqual(MemoryLayout<CyRootMotion>.offset(of: \CyRootMotion.travelled), 40, "CyRootMotion.travelled offset")
    }

    func testInterfaceHeaderLayout() {
        XCTAssertEqual(MemoryLayout<CyInterfaceHeader>.size, 16, "CyInterfaceHeader size")
        XCTAssertEqual(MemoryLayout<CyInterfaceHeader>.alignment, 4, "CyInterfaceHeader alignment")
        XCTAssertEqual(MemoryLayout<CyInterfaceHeader>.offset(of: \CyInterfaceHeader.abi_major), 0, "CyInterfaceHeader.abi_major offset")
        XCTAssertEqual(MemoryLayout<CyInterfaceHeader>.offset(of: \CyInterfaceHeader.abi_minor), 4, "CyInterfaceHeader.abi_minor offset")
        XCTAssertEqual(MemoryLayout<CyInterfaceHeader>.offset(of: \CyInterfaceHeader.abi_patch), 8, "CyInterfaceHeader.abi_patch offset")
        XCTAssertEqual(MemoryLayout<CyInterfaceHeader>.offset(of: \CyInterfaceHeader.table_size), 12, "CyInterfaceHeader.table_size offset")
    }

    func testInterfaceLayout() {
        XCTAssertEqual(MemoryLayout<CyInterface>.size, 992, "CyInterface size")
        XCTAssertEqual(MemoryLayout<CyInterface>.alignment, 8, "CyInterface alignment")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.header), 0, "CyInterface.header offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.log), 16, "CyInterface.log offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.get_last_error), 24, "CyInterface.get_last_error offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.get_last_error_code), 32, "CyInterface.get_last_error_code offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.set_last_error), 40, "CyInterface.set_last_error offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.var_make_string), 48, "CyInterface.var_make_string offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.var_make_bytes), 56, "CyInterface.var_make_bytes offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.var_clone), 64, "CyInterface.var_clone offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.var_release), 72, "CyInterface.var_release offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.var_live_count), 80, "CyInterface.var_live_count offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.engine_world), 88, "CyInterface.engine_world offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.world_create_entity), 96, "CyInterface.world_create_entity offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.world_destroy_entity), 104, "CyInterface.world_destroy_entity offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.world_entity_alive), 112, "CyInterface.world_entity_alive offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.world_epoch), 120, "CyInterface.world_epoch offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.world_register_component), 128, "CyInterface.world_register_component offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.world_find_component), 136, "CyInterface.world_find_component offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.world_add_component), 144, "CyInterface.world_add_component offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.world_remove_component), 152, "CyInterface.world_remove_component offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.world_has_component), 160, "CyInterface.world_has_component offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.world_borrow_component), 168, "CyInterface.world_borrow_component offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.borrow_valid), 176, "CyInterface.borrow_valid offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.component_get_var), 184, "CyInterface.component_get_var offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.component_set_var), 192, "CyInterface.component_set_var offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.component_get_f32), 200, "CyInterface.component_get_f32 offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.component_set_f32), 208, "CyInterface.component_set_f32 offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.component_get_vec3), 216, "CyInterface.component_get_vec3 offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.component_set_vec3), 224, "CyInterface.component_set_vec3 offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.register_behaviour), 232, "CyInterface.register_behaviour offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.find_behaviour), 240, "CyInterface.find_behaviour offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.behaviour_generation), 248, "CyInterface.behaviour_generation offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.world_component_count), 256, "CyInterface.world_component_count offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.world_component_info), 264, "CyInterface.world_component_info offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.world_component_field), 272, "CyInterface.world_component_field offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.world_parent), 280, "CyInterface.world_parent offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.world_set_parent), 288, "CyInterface.world_set_parent offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.world_child_count), 296, "CyInterface.world_child_count offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.world_child), 304, "CyInterface.world_child offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.world_chunks), 312, "CyInterface.world_chunks offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.service_open), 320, "CyInterface.service_open offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.service_close), 328, "CyInterface.service_close offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.service_submit), 336, "CyInterface.service_submit offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.service_cancel), 344, "CyInterface.service_cancel offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.service_poll), 352, "CyInterface.service_poll offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.time_get), 360, "CyInterface.time_get offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.input_find_action), 368, "CyInterface.input_find_action offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.input_action_state), 376, "CyInterface.input_action_state offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.input_action_state_by_name), 384, "CyInterface.input_action_state_by_name offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.input_pointer), 392, "CyInterface.input_pointer offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.input_modifiers), 400, "CyInterface.input_modifiers offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.input_find_context), 408, "CyInterface.input_find_context offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.input_push_context), 416, "CyInterface.input_push_context offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.input_pop_context), 424, "CyInterface.input_pop_context offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.camera_active), 432, "CyInterface.camera_active offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.camera_view), 440, "CyInterface.camera_view offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.camera_screen_to_ray), 448, "CyInterface.camera_screen_to_ray offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.camera_world_to_screen), 456, "CyInterface.camera_world_to_screen offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.camera_set_target), 464, "CyInterface.camera_set_target offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.camera_set_pose), 472, "CyInterface.camera_set_pose offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.camera_clear_pose), 480, "CyInterface.camera_clear_pose offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.physics_raycast), 488, "CyInterface.physics_raycast offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.physics_raycast_all), 496, "CyInterface.physics_raycast_all offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.physics_shape_cast), 504, "CyInterface.physics_shape_cast offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.physics_overlap), 512, "CyInterface.physics_overlap offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.nav_find_path), 520, "CyInterface.nav_find_path offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.nav_request_path), 528, "CyInterface.nav_request_path offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.nav_poll_path), 536, "CyInterface.nav_poll_path offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.nav_cancel_path), 544, "CyInterface.nav_cancel_path offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.nav_agent_configure), 552, "CyInterface.nav_agent_configure offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.nav_agent_move_to), 560, "CyInterface.nav_agent_move_to offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.nav_agent_stop), 568, "CyInterface.nav_agent_stop offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.nav_agent_state), 576, "CyInterface.nav_agent_state offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.audio_find_cue), 584, "CyInterface.audio_find_cue offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.audio_play), 592, "CyInterface.audio_play offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.audio_stop), 600, "CyInterface.audio_stop offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.audio_voice_playing), 608, "CyInterface.audio_voice_playing offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.audio_find_bus), 616, "CyInterface.audio_find_bus offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.audio_set_bus_volume), 624, "CyInterface.audio_set_bus_volume offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.spawn_resolve), 632, "CyInterface.spawn_resolve offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.spawn_instantiate), 640, "CyInterface.spawn_instantiate offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.spawn_instantiate_many), 648, "CyInterface.spawn_instantiate_many offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.spawn_destroy), 656, "CyInterface.spawn_destroy offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.vfx_effect_parameter_set), 664, "CyInterface.vfx_effect_parameter_set offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.vfx_effect_parameter_get), 672, "CyInterface.vfx_effect_parameter_get offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.register_system), 680, "CyInterface.register_system offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.node_find), 688, "CyInterface.node_find offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.physics_apply_force), 696, "CyInterface.physics_apply_force offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.physics_apply_impulse), 704, "CyInterface.physics_apply_impulse offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.physics_apply_torque), 712, "CyInterface.physics_apply_torque offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.physics_set_velocity), 720, "CyInterface.physics_set_velocity offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.physics_get_velocity), 728, "CyInterface.physics_get_velocity offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.character_create), 736, "CyInterface.character_create offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.character_destroy), 744, "CyInterface.character_destroy offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.character_move), 752, "CyInterface.character_move offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.character_state), 760, "CyInterface.character_state offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.ui_root), 768, "CyInterface.ui_root offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.ui_create), 776, "CyInterface.ui_create offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.ui_destroy), 784, "CyInterface.ui_destroy offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.ui_set_layout), 792, "CyInterface.ui_set_layout offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.ui_set_style), 800, "CyInterface.ui_set_style offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.ui_set_text), 808, "CyInterface.ui_set_text offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.ui_set_image), 816, "CyInterface.ui_set_image offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.ui_set_progress), 824, "CyInterface.ui_set_progress offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.ui_set_visibility), 832, "CyInterface.ui_set_visibility offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.ui_set_opacity), 840, "CyInterface.ui_set_opacity offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.ui_element_rect), 848, "CyInterface.ui_element_rect offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.ui_hit_test), 856, "CyInterface.ui_hit_test offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.ui_focus), 864, "CyInterface.ui_focus offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.ui_set_focus), 872, "CyInterface.ui_set_focus offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.animation_attach), 880, "CyInterface.animation_attach offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.animation_detach), 888, "CyInterface.animation_detach offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.animation_play), 896, "CyInterface.animation_play offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.animation_stop), 904, "CyInterface.animation_stop offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.animation_set_float), 912, "CyInterface.animation_set_float offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.animation_set_bool), 920, "CyInterface.animation_set_bool offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.animation_fire_trigger), 928, "CyInterface.animation_fire_trigger offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.animation_get_float), 936, "CyInterface.animation_get_float offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.animation_state), 944, "CyInterface.animation_state offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.animation_events), 952, "CyInterface.animation_events offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.animation_root_motion), 960, "CyInterface.animation_root_motion offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.animation_take_root_motion), 968, "CyInterface.animation_take_root_motion offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.animation_set_root_motion), 976, "CyInterface.animation_set_root_motion offset")
        XCTAssertEqual(MemoryLayout<CyInterface>.offset(of: \CyInterface.animation_joint_pose), 984, "CyInterface.animation_joint_pose offset")
    }

    func testModuleInitLayout() {
        XCTAssertEqual(MemoryLayout<CyModuleInit>.size, 40, "CyModuleInit size")
        XCTAssertEqual(MemoryLayout<CyModuleInit>.alignment, 8, "CyModuleInit alignment")
        XCTAssertEqual(MemoryLayout<CyModuleInit>.offset(of: \CyModuleInit.struct_size), 0, "CyModuleInit.struct_size offset")
        XCTAssertEqual(MemoryLayout<CyModuleInit>.offset(of: \CyModuleInit.abi_major), 4, "CyModuleInit.abi_major offset")
        XCTAssertEqual(MemoryLayout<CyModuleInit>.offset(of: \CyModuleInit.abi_minor), 8, "CyModuleInit.abi_minor offset")
        XCTAssertEqual(MemoryLayout<CyModuleInit>.offset(of: \CyModuleInit.reserved), 12, "CyModuleInit.reserved offset")
        XCTAssertEqual(MemoryLayout<CyModuleInit>.offset(of: \CyModuleInit.initialize), 16, "CyModuleInit.initialize offset")
        XCTAssertEqual(MemoryLayout<CyModuleInit>.offset(of: \CyModuleInit.shutdown), 24, "CyModuleInit.shutdown offset")
        XCTAssertEqual(MemoryLayout<CyModuleInit>.offset(of: \CyModuleInit.user_data), 32, "CyModuleInit.user_data offset")
    }

    /// The table itself. `Interface` reads entries by name through the imported struct, so if Swift
    /// laid `CyInterface` out differently from the engine, every call would go to the wrong entry.
    func testInterfaceTableSize() {
        XCTAssertEqual(MemoryLayout<CyInterface>.size, 992,
                       "CyInterface size")
        XCTAssertEqual(Int(ABI.interfaceTableSize), MemoryLayout<CyInterface>.size,
                       "the generated table size and the imported one")
    }

    /// The math types, which have no C struct to mirror: the ABI carries them as the leading lanes
    /// of `CyVarPayload.as_f32x4`, so each must be exactly its lane count of contiguous floats.
    func testVectorLayout() {
        XCTAssertEqual(MemoryLayout<Quat>.size, 16, "Quat size")
        XCTAssertEqual(MemoryLayout<Quat>.alignment, 4, "Quat alignment")
        XCTAssertEqual(MemoryLayout<Quat>.offset(of: \Quat.x), 0, "Quat.x offset")
        XCTAssertEqual(MemoryLayout<Quat>.offset(of: \Quat.y), 4, "Quat.y offset")
        XCTAssertEqual(MemoryLayout<Quat>.offset(of: \Quat.z), 8, "Quat.z offset")
        XCTAssertEqual(MemoryLayout<Quat>.offset(of: \Quat.w), 12, "Quat.w offset")
        XCTAssertEqual(MemoryLayout<Vec2>.size, 8, "Vec2 size")
        XCTAssertEqual(MemoryLayout<Vec2>.alignment, 4, "Vec2 alignment")
        XCTAssertEqual(MemoryLayout<Vec2>.offset(of: \Vec2.x), 0, "Vec2.x offset")
        XCTAssertEqual(MemoryLayout<Vec2>.offset(of: \Vec2.y), 4, "Vec2.y offset")
        XCTAssertEqual(MemoryLayout<Vec3>.size, 12, "Vec3 size")
        XCTAssertEqual(MemoryLayout<Vec3>.alignment, 4, "Vec3 alignment")
        XCTAssertEqual(MemoryLayout<Vec3>.offset(of: \Vec3.x), 0, "Vec3.x offset")
        XCTAssertEqual(MemoryLayout<Vec3>.offset(of: \Vec3.y), 4, "Vec3.y offset")
        XCTAssertEqual(MemoryLayout<Vec3>.offset(of: \Vec3.z), 8, "Vec3.z offset")
        XCTAssertEqual(MemoryLayout<Vec4>.size, 16, "Vec4 size")
        XCTAssertEqual(MemoryLayout<Vec4>.alignment, 4, "Vec4 alignment")
        XCTAssertEqual(MemoryLayout<Vec4>.offset(of: \Vec4.x), 0, "Vec4.x offset")
        XCTAssertEqual(MemoryLayout<Vec4>.offset(of: \Vec4.y), 4, "Vec4.y offset")
        XCTAssertEqual(MemoryLayout<Vec4>.offset(of: \Vec4.z), 8, "Vec4.z offset")
        XCTAssertEqual(MemoryLayout<Vec4>.offset(of: \Vec4.w), 12, "Vec4.w offset")
        XCTAssertEqual(MemoryLayout<CyVarPayload>.size, 16, "the payload the vectors live in")
    }

    /// The overlay's version constants against the header's own macros, which the C importer brings
    /// across independently of the description. Two routes to the same three numbers; if they ever
    /// disagree, the overlay was generated from a different header than the one being compiled.
    func testVersionConstantsAgreeWithTheHeader() {
        XCTAssertEqual(ABI.major, CY_ABI_MAJOR)
        XCTAssertEqual(ABI.minor, CY_ABI_MINOR)
        XCTAssertEqual(ABI.patch, CY_ABI_PATCH)
    }

    /// The entry names, in order. A reorder in `CyInterface` is what `just quality-abi` refuses;
    /// this is the same claim from Swift's side, and it is what makes `ABI.entryNames` — which a
    /// diagnostic uses to say *which* entry a short table stops at — worth trusting.
    func testEntryNameCount() {
        XCTAssertEqual(ABI.entryNames.count, 122)
        XCTAssertEqual(ABI.entryNames.first, "log")
        XCTAssertEqual(ABI.entryNames.last, "animation_joint_pose")
    }
}
