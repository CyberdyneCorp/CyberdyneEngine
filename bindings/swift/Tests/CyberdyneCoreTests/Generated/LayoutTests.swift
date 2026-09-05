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

    func testBehaviourVTableLayout() {
        XCTAssertEqual(MemoryLayout<CyBehaviourVTable>.size, 56, "CyBehaviourVTable size")
        XCTAssertEqual(MemoryLayout<CyBehaviourVTable>.alignment, 8, "CyBehaviourVTable alignment")
        XCTAssertEqual(MemoryLayout<CyBehaviourVTable>.offset(of: \CyBehaviourVTable.struct_size), 0, "CyBehaviourVTable.struct_size offset")
        XCTAssertEqual(MemoryLayout<CyBehaviourVTable>.offset(of: \CyBehaviourVTable.schema_version), 4, "CyBehaviourVTable.schema_version offset")
        XCTAssertEqual(MemoryLayout<CyBehaviourVTable>.offset(of: \CyBehaviourVTable.create), 8, "CyBehaviourVTable.create offset")
        XCTAssertEqual(MemoryLayout<CyBehaviourVTable>.offset(of: \CyBehaviourVTable.destroy), 16, "CyBehaviourVTable.destroy offset")
        XCTAssertEqual(MemoryLayout<CyBehaviourVTable>.offset(of: \CyBehaviourVTable.fixed_update), 24, "CyBehaviourVTable.fixed_update offset")
        XCTAssertEqual(MemoryLayout<CyBehaviourVTable>.offset(of: \CyBehaviourVTable.serialize), 32, "CyBehaviourVTable.serialize offset")
        XCTAssertEqual(MemoryLayout<CyBehaviourVTable>.offset(of: \CyBehaviourVTable.deserialize), 40, "CyBehaviourVTable.deserialize offset")
        XCTAssertEqual(MemoryLayout<CyBehaviourVTable>.offset(of: \CyBehaviourVTable.user_data), 48, "CyBehaviourVTable.user_data offset")
    }

    func testBorrowLayout() {
        XCTAssertEqual(MemoryLayout<CyBorrow>.size, 16, "CyBorrow size")
        XCTAssertEqual(MemoryLayout<CyBorrow>.alignment, 8, "CyBorrow alignment")
        XCTAssertEqual(MemoryLayout<CyBorrow>.offset(of: \CyBorrow.data), 0, "CyBorrow.data offset")
        XCTAssertEqual(MemoryLayout<CyBorrow>.offset(of: \CyBorrow.epoch), 8, "CyBorrow.epoch offset")
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
        XCTAssertEqual(MemoryLayout<CyInterface>.size, 256, "CyInterface size")
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
        XCTAssertEqual(MemoryLayout<CyInterface>.size, 256,
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
        XCTAssertEqual(ABI.entryNames.count, 30)
        XCTAssertEqual(ABI.entryNames.first, "log")
        XCTAssertEqual(ABI.entryNames.last, "behaviour_generation")
    }
}
