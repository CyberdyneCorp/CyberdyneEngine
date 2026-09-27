// SPDX-License-Identifier: MIT
// PhysicsTests.swift — the `Physics` facade through `FakeEngine`. `add-swift-game-api`, task 2.4.
//
// Each case installs the entries it exercises and asserts three things: the facade sent the right
// arguments (the ray, the filter with its ignore list, the shape), converted the answer (hits and
// entities, in the order the engine gave them), and turned a refusal into a thrown
// `CyberdyneError.status`. The engine's own ordering and filtering are proven in C++.

import CyberdyneABI
import CyberdyneCore
import XCTest

@testable import CyberdyneKit

/// What the fake entries saw, since a C function pointer cannot capture.
private enum Seen {
    nonisolated(unsafe) static var ray = CyRay()
    nonisolated(unsafe) static var filter = CyQueryFilter()
    nonisolated(unsafe) static var ignored: [CyEntity] = []
    nonisolated(unsafe) static var shape = CyShape()
    nonisolated(unsafe) static var direction: [Float] = []
    nonisolated(unsafe) static var calls = 0
    nonisolated(unsafe) static var total: UInt32 = 3

    static func record(_ filter: UnsafePointer<CyQueryFilter>?) {
        calls += 1
        guard let filter else { return }
        self.filter = filter.pointee
        ignored = (0..<Int(filter.pointee.ignore_count)).map { filter.pointee.ignore![$0] }
    }

    static func hit(_ entity: CyEntity) -> CyPhysicsHit {
        var hit = CyPhysicsHit()
        hit.entity = entity
        hit.distance = Float(entity)
        hit.point = (1, 2, 3)
        hit.normal = (0, 1, 0)
        hit.flags = CY_HIT_TRIGGER
        return hit
    }

    static func reset() {
        ray = CyRay()
        filter = CyQueryFilter()
        ignored = []
        shape = CyShape()
        direction = []
        calls = 0
        total = 3
    }
}

final class PhysicsTests: XCTestCase {
    override func setUp() {
        super.setUp()
        Seen.reset()
    }

    override func tearDown() {
        FakeEngine.uninstall()
        super.tearDown()
    }

    private func installQueries() {
        FakeEngine.install { table in
            table.physics_raycast = { _, ray, filter, hit, hasHit in
                Seen.record(filter)
                Seen.ray = ray!.pointee
                hit!.pointee = Seen.hit(42)
                hasHit!.pointee = Seen.total > 0
                return CY_RESULT_OK
            }
            table.physics_raycast_all = { _, _, filter, hits, capacity, count in
                Seen.record(filter)
                count!.pointee = Seen.total
                guard let hits else { return CY_RESULT_OK }
                for index in 0..<min(capacity, Seen.total) {
                    hits[Int(index)] = Seen.hit(CyEntity(index + 1))
                }
                return capacity < Seen.total
                    ? FakeEngine.fail(CY_RESULT_BUFFER_TOO_SMALL, "more hits than the buffer holds")
                    : CY_RESULT_OK
            }
            table.physics_shape_cast = { _, shape, _, direction, _, filter, hit, hasHit in
                Seen.record(filter)
                Seen.shape = shape!.pointee
                Seen.direction = [direction![0], direction![1], direction![2]]
                hit!.pointee = Seen.hit(9)
                hasHit!.pointee = true
                return CY_RESULT_OK
            }
            table.physics_overlap = { _, shape, _, filter, entities, capacity, count in
                Seen.record(filter)
                Seen.shape = shape!.pointee
                count!.pointee = Seen.total
                guard let entities else { return CY_RESULT_OK }
                for index in 0..<min(capacity, Seen.total) {
                    entities[Int(index)] = CyEntity(100 + index)
                }
                return CY_RESULT_OK
            }
        }
    }

    func testARaycastSendsTheRayAndTheFilterAndReturnsTheHit() throws {
        installQueries()
        let ray = Ray(
            origin: Vec3(x: 1, y: 10, z: 2), direction: Vec3(x: 0, y: -1, z: 0), maxDistance: 50)
        let filter = Physics.Filter(layer: 2, mask: 1 << 3, options: .includeTriggers)
            .ignoring(Entity(bits: 7), Entity(bits: 8))
        let hit = try XCTUnwrap(Physics.raycast(ray, filter: filter))

        XCTAssertEqual(Ray(Seen.ray), ray)
        XCTAssertEqual(Seen.filter.struct_size, UInt32(MemoryLayout<CyQueryFilter>.size))
        XCTAssertEqual(Seen.filter.layer, 2)
        XCTAssertEqual(Seen.filter.mask, 1 << 3)
        XCTAssertEqual(Seen.filter.flags, CY_QUERY_INCLUDE_TRIGGERS)
        XCTAssertEqual(Seen.ignored, [7, 8])

        XCTAssertEqual(hit.entity, Entity(bits: 42))
        XCTAssertEqual(hit.point, Vec3(x: 1, y: 2, z: 3))
        XCTAssertEqual(hit.normal, Vec3(x: 0, y: 1, z: 0))
        XCTAssertEqual(hit.distance, 42)
        XCTAssertTrue(hit.isTrigger)
        XCTAssertFalse(hit.startedPenetrating)
    }

    func testTheDefaultFilterHitsEveryLayerAndIgnoresNothing() throws {
        installQueries()
        _ = try Physics.raycast(Ray(origin: Vec3(), direction: Vec3(x: 1, y: 0, z: 0)))
        XCTAssertEqual(Seen.filter.layer, 0)
        XCTAssertEqual(Seen.filter.mask, UInt32.max)
        XCTAssertEqual(Seen.filter.flags, 0)
        XCTAssertEqual(Seen.filter.ignore_count, 0)
        XCTAssertNil(Seen.filter.ignore)
    }

    func testNothingHitIsNil() throws {
        installQueries()
        Seen.total = 0
        XCTAssertNil(try Physics.raycast(Ray(origin: Vec3(), direction: Vec3(x: 1, y: 0, z: 0))))
    }

    func testRaycastAllSizesItsBufferAndKeepsTheEnginesOrder() throws {
        installQueries()
        Seen.total = 70
        let hits = try Physics.raycastAll(
            Ray(origin: Vec3(), direction: Vec3(x: 1, y: 0, z: 0)))
        XCTAssertEqual(hits.count, 70)
        XCTAssertEqual(hits.map(\.entity.bits), (1...70).map { CyEntity($0) })
        // One call to ask, one to fill.
        XCTAssertEqual(Seen.calls, 2)

        Seen.total = 0
        XCTAssertEqual(
            try Physics.raycastAll(Ray(origin: Vec3(), direction: Vec3(x: 1, y: 0, z: 0))), [])
    }

    func testAShapeCastSendsTheShapeAndTheSweep() throws {
        installQueries()
        let hit = try Physics.shapeCast(
            .capsule(radius: 0.5, halfHeight: 1), from: Pose(position: Vec3(x: 0, y: 1, z: 0)),
            direction: Vec3(x: 0, y: 0, z: 1), maxDistance: 10)
        XCTAssertEqual(hit?.entity, Entity(bits: 9))
        XCTAssertEqual(Seen.shape.kind, ShapeKind.capsule.rawValue)
        XCTAssertEqual(Seen.shape.radius, 0.5)
        XCTAssertEqual(Seen.shape.half_height, 1)
        XCTAssertEqual(Seen.direction, [0, 0, 1])
    }

    func testAnOverlapReturnsEntitiesInTheEnginesOrder() throws {
        installQueries()
        Seen.total = 4
        let entities = try Physics.overlap(
            .box(halfExtents: Vec3(x: 1, y: 2, z: 3)), at: Pose(),
            filter: Physics.Filter(mask: 0b10))
        XCTAssertEqual(entities.map(\.bits), [100, 101, 102, 103])
        XCTAssertEqual(Seen.shape.kind, ShapeKind.box.rawValue)
        XCTAssertEqual(Seen.shape.half_extents.2, 3)
        XCTAssertEqual(Seen.filter.mask, 0b10)
    }

    func testTheShapesCrossTheBoundaryAsDeclared() {
        let sphere = Physics.Shape.sphere(radius: 2).raw
        XCTAssertEqual(sphere.kind, CY_SHAPE_SPHERE.rawValue)
        XCTAssertEqual(sphere.radius, 2)
        let box = Physics.Shape.box(halfExtents: Vec3(x: 1, y: 2, z: 3)).raw
        XCTAssertEqual(box.kind, CY_SHAPE_BOX.rawValue)
        XCTAssertEqual(box.half_extents.1, 2)
    }

    func testARefusalMidStepThrowsUnavailable() {
        FakeEngine.install { table in
            table.physics_raycast = { _, _, _, _, _ in
                FakeEngine.fail(CY_RESULT_UNAVAILABLE, "the physics step is running")
            }
        }
        XCTAssertThrowsError(
            try Physics.raycast(Ray(origin: Vec3(), direction: Vec3(x: 1, y: 0, z: 0)))
        ) { error in
            XCTAssertEqual(
                error as? CyberdyneError,
                .status(.unavailable, message: "the physics step is running"))
        }
    }

    func testWithNoEngineAQueryThrowsRatherThanTraps() {
        FakeEngine.uninstall()
        XCTAssertThrowsError(try Physics.overlap(.sphere(radius: 1), at: Pose())) { error in
            guard case CyberdyneError.status(.unavailable, _) = error else {
                return XCTFail("expected .unavailable, got \(error)")
            }
        }
    }
}
