// SPDX-License-Identifier: MIT
// SystemEngineTests.swift — a `@System` handed to the engine's scheduler through ABI 1.5's
// `register_system`, and run over `world_chunks`. `add-swift-m12-gaps`.
//
// What the engine does with the declaration — ordering it against native systems, running it in
// its stage's phase — is unit.abi and integration.rts_api_sample. This proves the Swift half against
// a fake table: the access set crosses as component ids resolved by name, sorted, with the stage
// the attribute named; a resource term or an unregistered component is refused before the engine
// is called; and the body, run through the C thunk the engine would call, sees exactly the chunks
// of archetypes holding every component it reads or writes and none it excludes, with one view per
// chunk and the columns joined by archetype.

import CyberdyneABI
import CyberdyneCore
import XCTest

@testable import CyberdyneKit

private enum World_ {
    nonisolated(unsafe) static let handle = OpaquePointer(bitPattern: 0xD0)!
    nonisolated(unsafe) static var desc = CySystemDesc()
    nonisolated(unsafe) static var terms: [CySystemAccess] = []
    nonisolated(unsafe) static var name = ""
    nonisolated(unsafe) static var registrations = 0

    // Three archetypes. 10: Velocity + Mass, in two chunks. 11: Velocity + Mass + Grounded (the
    // query excludes it). 12: Velocity alone (the query needs Mass too). Allocated, not arrays: a
    // chunk is a borrowed pointer, and `&array` is valid only for the call it is passed to.
    static func buffer<T>(_ values: [T]) -> UnsafeMutablePointer<T> {
        let pointer = UnsafeMutablePointer<T>.allocate(capacity: values.count)
        pointer.initialize(from: values, count: values.count)
        return pointer
    }

    nonisolated(unsafe) static let entities10a = buffer([CyEntity(1), 2])
    nonisolated(unsafe) static let entities10b = buffer([CyEntity(3)])
    nonisolated(unsafe) static let entities11 = buffer([CyEntity(4)])
    nonisolated(unsafe) static let entities12 = buffer([CyEntity(5)])
    nonisolated(unsafe) static let velocities10a = buffer([Velocity(), Velocity()])
    nonisolated(unsafe) static let velocities10b = buffer([Velocity()])
    nonisolated(unsafe) static let velocities11 = buffer([Velocity()])
    nonisolated(unsafe) static let velocities12 = buffer([Velocity()])
    nonisolated(unsafe) static let masses10a = buffer([Mass(value: 1), Mass(value: 2)])
    nonisolated(unsafe) static let masses10b = buffer([Mass(value: 3)])
    nonisolated(unsafe) static let masses11 = buffer([Mass(value: 4)])
    nonisolated(unsafe) static let grounded11 = buffer([Grounded()])

    static func ys(_ velocities: UnsafeMutablePointer<Velocity>, _ count: Int) -> [Float] {
        (0..<count).map { velocities[$0].y }
    }

    static func id(_ name: String) -> CyComponentTypeId {
        switch name {
        case Velocity.componentName: return 1
        case Mass.componentName: return 2
        case Grounded.componentName: return 3
        default: return CY_COMPONENT_TYPE_INVALID
        }
    }

    static func chunk<T>(
        _ archetype: UInt32, _ entities: UnsafeMutablePointer<CyEntity>, _ count: Int,
        _ data: UnsafeMutablePointer<T>
    ) -> CyChunk {
        var chunk = CyChunk()
        chunk.struct_size = UInt32(MemoryLayout<CyChunk>.size)
        chunk.entity_count = UInt32(count)
        chunk.entities = UnsafePointer(entities)
        chunk.data = UnsafeMutableRawPointer(data)
        chunk.stride = UInt32(MemoryLayout<T>.stride)
        chunk.archetype = archetype
        return chunk
    }

    /// The chunks holding one component, in archetype then chunk order — `world_chunks`' contract.
    static func chunks(_ component: CyComponentTypeId) -> [CyChunk] {
        switch component {
        case 1:
            return [
                chunk(10, entities10a, 2, velocities10a), chunk(10, entities10b, 1, velocities10b),
                chunk(11, entities11, 1, velocities11), chunk(12, entities12, 1, velocities12),
            ]
        case 2:
            return [
                chunk(10, entities10a, 2, masses10a), chunk(10, entities10b, 1, masses10b),
                chunk(11, entities11, 1, masses11),
            ]
        case 3:
            return [chunk(11, entities11, 1, grounded11)]
        default:
            return []
        }
    }

    static func reset() {
        desc = CySystemDesc()
        terms = []
        name = ""
        registrations = 0
        for velocities in [velocities10b, velocities11, velocities12] {
            velocities[0] = Velocity()
        }
        velocities10a[0] = Velocity()
        velocities10a[1] = Velocity()
    }
}

final class SystemEngineTests: XCTestCase {
    override func setUp() {
        super.setUp()
        World_.reset()
        FakeEngine.install { table in
            table.engine_world = { _ in World_.handle }
            table.world_find_component = { _, name in World_.id(String(cString: name!)) }
            table.register_system = { _, desc in
                guard let desc = desc?.pointee else {
                    return FakeEngine.fail(CY_RESULT_INVALID_ARGUMENT, "no desc")
                }
                World_.desc = desc
                World_.name = String(cString: desc.name!)
                World_.terms = (0..<Int(desc.access_count)).map { desc.access![$0] }
                World_.registrations += 1
                return CY_RESULT_OK
            }
            table.world_chunks = { _, component, out, capacity, count in
                let chunks = World_.chunks(component)
                count!.pointee = UInt32(chunks.count)
                guard let out else { return CY_RESULT_OK }
                for (index, chunk) in chunks.prefix(Int(capacity)).enumerated() {
                    out[index] = chunk
                }
                return capacity < chunks.count ? CY_RESULT_BUFFER_TOO_SMALL : CY_RESULT_OK
            }
        }
    }

    override func tearDown() {
        Systems.forgetRegistrations()
        FakeEngine.uninstall()
        super.tearDown()
    }

    func testTheQueryCrossesAsComponentIdsWithItsStage() throws {
        try __CySystem_applyGravity.register()
        XCTAssertEqual(World_.registrations, 1)
        XCTAssertEqual(World_.name, "applyGravity")
        XCTAssertEqual(World_.desc.stage, SystemStage.simulation.rawValue)
        XCTAssertEqual(World_.desc.struct_size, UInt32(MemoryLayout<CySystemDesc>.size))
        // Reads, then writes, then excludes; each set in name order.
        let terms = World_.terms.map { ($0.component, $0.mode) }
        XCTAssertEqual(terms.map(\.0), [2, 1, 3])
        XCTAssertEqual(
            terms.map(\.1),
            [AccessMode.read.rawValue, AccessMode.write.rawValue, AccessMode.exclude.rawValue])
        XCTAssertNotNil(World_.desc.run)
        XCTAssertNotNil(World_.desc.user_data)
    }

    func testTheEngineRunsTheBodyOverTheMatchingChunksOnly() throws {
        try __CySystem_applyGravity.register()
        let run = try XCTUnwrap(World_.desc.run)
        run(FakeEngine.handle, World_.handle, World_.desc.user_data)

        // Archetype 10, both chunks, each row by its own mass.
        XCTAssertEqual(World_.ys(World_.velocities10a, 2), [-9.81, -9.81 * 2])
        XCTAssertEqual(World_.ys(World_.velocities10b, 1), [-9.81 * 3])
        // Archetype 11 holds Grounded, which the query excludes; 12 lacks Mass.
        XCTAssertEqual(World_.ys(World_.velocities11, 1), [0])
        XCTAssertEqual(World_.ys(World_.velocities12, 1), [0])
    }

    func testAComponentTheWorldDoesNotHaveIsRefusedBeforeTheEngineIsCalled() {
        let access = AccessSet([AccessTerm(name: "Unregistered", mode: .write)])
        XCTAssertThrowsError(
            try Systems.register(
                SystemDescriptor(name: "orphan", stage: .simulation, access: access)) { _ in }
        ) { error in
            guard case CyberdyneError.status(.notFound, _) = error else {
                return XCTFail("expected notFound, got \(error)")
            }
        }
        XCTAssertEqual(World_.registrations, 0)
        XCTAssertTrue(Systems.registered.isEmpty)
    }

    func testAResourceTermIsRefusedBecauseTheABIHasNoResourceEntry() {
        let access = AccessSet([Res<Double>.accessTerm])
        XCTAssertThrowsError(
            try Systems.register(
                SystemDescriptor(name: "clock", stage: .simulation, access: access)) { _ in }
        ) { error in
            guard case CyberdyneError.notRepresentable = error else {
                return XCTFail("expected notRepresentable, got \(error)")
            }
        }
        XCTAssertEqual(World_.registrations, 0)
    }

    func testTheEnginesRefusalReachesTheGame() {
        FakeEngine.table.pointee.register_system = { _, _ in
            FakeEngine.fail(CY_RESULT_UNSUPPORTED, "a reload changed a scheduled system")
        }
        XCTAssertThrowsError(try __CySystem_readVelocity.register()) { error in
            XCTAssertEqual(
                error as? CyberdyneError,
                .status(.unsupported, message: "a reload changed a scheduled system"))
        }
        XCTAssertTrue(Systems.registered.isEmpty)
    }
}
