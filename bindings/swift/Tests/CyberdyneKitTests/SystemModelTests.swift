// SystemModelTests.swift — access derived from a signature, and a chunk loop that does not marshal.
// Task 3.3.
//
// `swift-scripting`: "Swift system is scheduled like a native one" — a Swift system declaring
// `Write<Velocity>` and a native one declaring `Read<Velocity>` are ordered by the same conflict
// rules — and "Bulk iteration does not marshal": the inner loop reads chunk arrays through borrowed
// buffers, with no `CyVar` conversion and no per-entity ABI call.
//
// The conflict rules are asserted here directly. The inner loop is asserted over a `ChunkSource`
// this file supplies, because ABI 1.0 has no entry that hands a module a chunk — Systems.swift's
// header comment says which append closes that, and nothing about the loop changes when it does.

import XCTest

@testable import CyberdyneKit

@Component
struct Velocity {
    var x: Float = 0
    var y: Float = 0
    var z: Float = 0
}

@Component
struct Mass {
    var value: Float = 1
}

@Component
struct Grounded {
    var since: Double = 0
}

/// A chunk source over arrays this test owns, so the loop under test is the real one.
final class ArrayChunkSource: ChunkSource {
    var entities: [CyEntity]
    var velocities: [Velocity]
    var masses: [Mass]
    private(set) var lastAccess: AccessSet?
    let escapeGuard = EscapeGuard(systemName: "test")

    init(count: Int) {
        entities = (0 ..< count).map { CyEntity($0 + 1) }
        velocities = Array(repeating: Velocity(), count: count)
        masses = (0 ..< count).map { Mass(value: Float($0 + 1)) }
    }

    func forEachChunk(matching access: AccessSet, _ body: (ChunkView) -> Void) {
        lastAccess = access
        entities.withUnsafeBufferPointer { entityBuffer in
            velocities.withUnsafeMutableBufferPointer { velocityBuffer in
                masses.withUnsafeMutableBufferPointer { massBuffer in
                    let view = ChunkView(
                        entities: entityBuffer,
                        bases: [Velocity.componentName: UnsafeMutableRawPointer(velocityBuffer.baseAddress!),
                                Mass.componentName: UnsafeMutableRawPointer(massBuffer.baseAddress!)],
                        strides: [Velocity.componentName: MemoryLayout<Velocity>.stride,
                                  Mass.componentName: MemoryLayout<Mass>.stride],
                        guardToken: escapeGuard)
                    body(view)
                }
            }
        }
        escapeGuard.invalidate()
    }
}

@System(stage: .simulation)
func applyGravity(_ query: Query<Write<Velocity>, Read<Mass>, Without<Grounded>>,
                  _ chunks: ChunkSource) {
    chunks.forEachChunk(matching: type(of: query).access) { chunk in
        guard let velocities = chunk.array(Velocity.self),
              let masses = chunk.array(Mass.self) else { return }
        for index in 0 ..< chunk.count {
            velocities[index].y -= 9.81 * masses[index].value
        }
    }
}

@System(stage: .simulation)
func readVelocity(_ query: Query<Read<Velocity>>, _ chunks: ChunkSource) {
    chunks.forEachChunk(matching: type(of: query).access) { _ in }
}

final class SystemModelTests: XCTestCase {
    override func tearDown() {
        Systems.forgetRegistrations()
        super.tearDown()
    }

    func testAccessIsDerivedFromTheQueryType() {
        let access = Query<Write<Velocity>, Read<Mass>, Without<Grounded>>.access
        XCTAssertEqual(access.writes, ["Velocity"])
        XCTAssertEqual(access.reads, ["Mass"])
        XCTAssertEqual(access.excludes, ["Grounded"])
    }

    /// The rule the engine's scheduler uses: a write conflicts with any other access to the same
    /// component, and two reads never conflict.
    func testConflictRules() {
        let write = Query<Write<Velocity>>.access
        let read = Query<Read<Velocity>>.access
        let unrelated = Query<Read<Mass>>.access

        XCTAssertTrue(write.conflicts(with: read))
        XCTAssertTrue(read.conflicts(with: write))
        XCTAssertTrue(write.conflicts(with: write))
        XCTAssertFalse(read.conflicts(with: read))
        XCTAssertFalse(write.conflicts(with: unrelated))
        XCTAssertFalse(Query<Without<Grounded>>.access.conflicts(with: write),
                       "an exclusion filter declares no access")
    }

    func testTheMacroRegistersTheSignatureItWasWritten() throws {
        try __CySystem_applyGravity.register()
        XCTAssertEqual(Systems.registered.map(\.name), ["applyGravity"])
        XCTAssertEqual(Systems.registered.first?.stage, .simulation)
        XCTAssertEqual(Systems.registered.first?.access.writes, ["Velocity"])
    }

    func testConflictingPairsAreReportedInAStage() throws {
        try __CySystem_applyGravity.register()
        try __CySystem_readVelocity.register()
        let pairs = Systems.conflictingPairs(in: .simulation)
        XCTAssertEqual(pairs.count, 1)
        XCTAssertEqual(pairs.first?.0, "applyGravity")
        XCTAssertEqual(pairs.first?.1, "readVelocity")
        XCTAssertTrue(Systems.conflictingPairs(in: .render).isEmpty)
    }

    /// The inner loop over chunk-contiguous storage. 100 000 entities, one pass, no `CyVar` and no
    /// per-entity call — which is the claim, and running it is the only way to hold it.
    func testBulkIterationWritesThroughBorrowedBuffers() throws {
        let source = ArrayChunkSource(count: 100_000)
        try __CySystem_applyGravity.register()
        Systems.run(stage: .simulation, over: source)

        XCTAssertEqual(source.velocities[0].y, -9.81 * 1, accuracy: 1e-3)
        XCTAssertEqual(source.velocities[99_999].y, -9.81 * 100_000, accuracy: 1e-1)
        XCTAssertEqual(source.lastAccess?.writes, ["Velocity"])
    }

    func testAComponentTheChunkDoesNotHoldAnswersNilRatherThanTrapping() {
        let source = ArrayChunkSource(count: 4)
        var sawNil = false
        source.forEachChunk(matching: AccessSet()) { chunk in
            sawNil = chunk.array(Grounded.self) == nil
        }
        XCTAssertTrue(sawNil)
    }

    func testTheMacroDerivesComponentFieldsAndOffsets() {
        XCTAssertEqual(Velocity.componentName, "Velocity")
        XCTAssertEqual(Velocity.componentFields.map(\.name), ["x", "y", "z"])
        XCTAssertEqual(Velocity.componentFields.map(\.type), [.f32, .f32, .f32])
        XCTAssertEqual(Velocity.componentFields.map(\.offset), [0, 4, 8])
        XCTAssertEqual(Velocity.componentFields.map(\.size), [4, 4, 4])
    }

    func testASelfConflictingSystemIsRefusedAtRegistration() {
        var access = AccessSet()
        access.insert(AccessTerm(name: "Health", mode: .read))
        access.insert(AccessTerm(name: "Health", mode: .write))
        XCTAssertTrue(access.isSelfConflicting)
        XCTAssertThrowsError(
            try Systems.register(SystemDescriptor(name: "bad", stage: .simulation, access: access))
            { _ in })
    }
}
