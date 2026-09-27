// SPDX-License-Identifier: MIT
// NavigationTests.swift — the `Navigation`, `PathQuery` and `NavAgent` facades through
// `FakeEngine`. `add-swift-game-api`, task 2.4.
//
// Each case installs the entries it exercises and asserts what the facade sent, what it made of
// the answer — including the retry a too-small point buffer asks for, and a poll that is still
// pending — and that a refusal (a queue call outside fixed update) arrives as a thrown
// `CyberdyneError.status`. Path search, queue latency and the crowd are proven in C++.

import CyberdyneABI
import CyberdyneCore
import XCTest

@testable import CyberdyneKit

/// What the fake entries saw and what they answer, since a C function pointer cannot capture.
private enum Nav {
    nonisolated(unsafe) static var request = CyNavPathRequest()
    nonisolated(unsafe) static var params = CyNavAgentParams()
    nonisolated(unsafe) static var target: [Float] = []
    nonisolated(unsafe) static var entity: CyEntity = 0
    nonisolated(unsafe) static var points: UInt32 = 3
    nonisolated(unsafe) static var calls = 0
    nonisolated(unsafe) static var ready = false
    nonisolated(unsafe) static var stopped = false

    static func reset() {
        request = CyNavPathRequest()
        params = CyNavAgentParams()
        target = []
        entity = 0
        points = 3
        calls = 0
        ready = false
        stopped = false
    }

    /// Write a path of `points` points along +X, the sizing pattern included.
    static func path(
        _ buffer: UnsafeMutablePointer<Float>?, _ capacity: UInt32,
        _ result: UnsafeMutablePointer<CyNavPathResult>?
    ) -> CyResult {
        result!.pointee.flags = CY_NAV_PATH_FOUND
        result!.pointee.point_count = points
        result!.pointee.state = NavQueryState.ready.rawValue
        result!.pointee.length = Float(points - 1)
        guard let buffer else { return CY_RESULT_OK }
        for index in 0..<min(capacity, points) {
            buffer[Int(index) * 3] = Float(index)
            buffer[Int(index) * 3 + 1] = 0
            buffer[Int(index) * 3 + 2] = 5
        }
        return capacity < points
            ? FakeEngine.fail(CY_RESULT_BUFFER_TOO_SMALL, "the path has more points than that")
            : CY_RESULT_OK
    }
}

final class NavigationTests: XCTestCase {
    override func setUp() {
        super.setUp()
        Nav.reset()
    }

    override func tearDown() {
        FakeEngine.uninstall()
        super.tearDown()
    }

    func testFindPathSendsTheRequestAndReturnsThePoints() throws {
        FakeEngine.install { table in
            table.nav_find_path = { _, request, buffer, capacity, result in
                Nav.calls += 1
                Nav.request = request!.pointee
                return Nav.path(buffer, capacity, result)
            }
        }
        let path = try Navigation.findPath(
            from: Vec3(x: 1, y: 0, z: 2), to: Vec3(x: 9, y: 0, z: 8),
            options: .init(world: 2, nodeBudget: 500, areaMask: 0b101))
        XCTAssertEqual(Nav.request.struct_size, UInt32(MemoryLayout<CyNavPathRequest>.size))
        XCTAssertEqual(Nav.request.world, 2)
        XCTAssertEqual(Vec3(Nav.request.start), Vec3(x: 1, y: 0, z: 2))
        XCTAssertEqual(Vec3(Nav.request.end), Vec3(x: 9, y: 0, z: 8))
        XCTAssertEqual(Nav.request.node_budget, 500)
        XCTAssertEqual(Nav.request.area_mask, 0b101)

        XCTAssertTrue(path.found)
        XCTAssertFalse(path.isPartial)
        XCTAssertEqual(path.length, 2)
        XCTAssertEqual(
            path.points,
            [Vec3(x: 0, y: 0, z: 5), Vec3(x: 1, y: 0, z: 5), Vec3(x: 2, y: 0, z: 5)])
        XCTAssertEqual(Nav.calls, 1)
    }

    func testALongPathIsFetchedAgainWithTheSizeTheEngineAskedFor() throws {
        FakeEngine.install { table in
            table.nav_find_path = { _, _, buffer, capacity, result in
                Nav.calls += 1
                return Nav.path(buffer, capacity, result)
            }
        }
        Nav.points = 100
        let path = try Navigation.findPath(from: Vec3(), to: Vec3(x: 99, y: 0, z: 0))
        XCTAssertEqual(path.points.count, 100)
        XCTAssertEqual(path.points.last, Vec3(x: 99, y: 0, z: 5))
        XCTAssertEqual(Nav.calls, 2)
    }

    func testAQueuedPathIsPendingThenReadyThenSpent() throws {
        FakeEngine.install { table in
            table.nav_request_path = { _, request, query in
                Nav.request = request!.pointee
                query!.pointee = 0x1_0000_0001
                return CY_RESULT_OK
            }
            table.nav_poll_path = { _, query, buffer, capacity, result in
                guard query == 0x1_0000_0001, !Nav.stopped else {
                    return FakeEngine.fail(CY_RESULT_NOT_FOUND, "no such path query")
                }
                guard Nav.ready else {
                    result!.pointee.state = NavQueryState.pending.rawValue
                    return CY_RESULT_OK
                }
                let answer = Nav.path(buffer, capacity, result)
                if answer == CY_RESULT_OK { Nav.stopped = true }  // consumed
                return answer
            }
        }
        let query = try Navigation.requestPath(
            from: Vec3(x: 1, y: 0, z: 1), to: Vec3(x: 4, y: 0, z: 1))
        XCTAssertEqual(query.raw, 0x1_0000_0001)
        XCTAssertEqual(Vec3(Nav.request.end), Vec3(x: 4, y: 0, z: 1))

        XCTAssertEqual(try query.poll(), .pending)
        Nav.ready = true
        Nav.points = 40  // more than the first buffer: the poll must retry, not lose it
        guard case .ready(let path) = try query.poll() else {
            return XCTFail("expected a ready path")
        }
        XCTAssertEqual(path.points.count, 40)
        XCTAssertThrowsError(try query.poll()) { error in
            guard case CyberdyneError.status(.notFound, _) = error else {
                return XCTFail("expected .notFound, got \(error)")
            }
        }
    }

    func testACancelledQueryPollsAsCancelled() throws {
        FakeEngine.install { table in
            table.nav_cancel_path = { _, query in
                Nav.calls += 1
                return query == 5 ? CY_RESULT_OK : CY_RESULT_NOT_FOUND
            }
            table.nav_poll_path = { _, _, _, _, result in
                result!.pointee.state = NavQueryState.cancelled.rawValue
                return CY_RESULT_OK
            }
        }
        let query = PathQuery(raw: 5)
        try query.cancel()
        XCTAssertEqual(Nav.calls, 1)
        XCTAssertEqual(try query.poll(), .cancelled)
    }

    func testAQueueCallOutsideFixedUpdateThrowsPermissionDenied() {
        FakeEngine.install { table in
            table.nav_request_path = { _, _, _ in
                FakeEngine.fail(
                    CY_RESULT_PERMISSION_DENIED,
                    "nav_request_path may not be called during frame update")
            }
        }
        XCTAssertThrowsError(try Navigation.requestPath(from: Vec3(), to: Vec3(x: 1, y: 0, z: 0))) {
            error in
            XCTAssertEqual(
                error as? CyberdyneError,
                .status(
                    .permissionDenied,
                    message: "nav_request_path may not be called during frame update"))
        }
    }

    func testAnAgentIsConfiguredMovedStoppedAndRead() throws {
        FakeEngine.install { table in
            table.nav_agent_configure = { _, entity, params in
                Nav.entity = entity
                Nav.params = params!.pointee
                return CY_RESULT_OK
            }
            table.nav_agent_move_to = { _, entity, target in
                Nav.entity = entity
                Nav.target = [target![0], target![1], target![2]]
                return CY_RESULT_OK
            }
            table.nav_agent_stop = { _, entity in
                Nav.entity = entity
                Nav.stopped = true
                return CY_RESULT_OK
            }
            table.nav_agent_state = { _, _, state in
                state!.pointee.status = NavPathStatus.arrived.rawValue
                state!.pointee.flags = CY_NAV_AGENT_EVENT
                state!.pointee.position = (3, 0, 4)
                state!.pointee.target = (3, 0, 4)
                state!.pointee.remaining_distance = 0
                return CY_RESULT_OK
            }
        }
        let agent = NavAgent(Entity(bits: 77))
        try agent.configure(.init(radius: 0.4, maxSpeed: 6, priority: 3))
        XCTAssertEqual(Nav.entity, 77)
        XCTAssertEqual(Nav.params.struct_size, UInt32(MemoryLayout<CyNavAgentParams>.size))
        XCTAssertEqual(Nav.params.radius, 0.4)
        XCTAssertEqual(Nav.params.max_speed, 6)
        XCTAssertEqual(Nav.params.priority, 3)
        XCTAssertEqual(Nav.params.height, 0)  // zero: the engine's default

        try agent.move(to: Vec3(x: 3, y: 0, z: 4))
        XCTAssertEqual(Nav.target, [3, 0, 4])

        let state = try agent.state
        XCTAssertEqual(state.status, .arrived)
        XCTAssertTrue(state.justArrived)
        XCTAssertFalse(state.justFailed)
        XCTAssertEqual(state.position, Vec3(x: 3, y: 0, z: 4))

        try agent.stop()
        XCTAssertTrue(Nav.stopped)
    }

    func testAnEntityThatIsNotAnAgentThrowsNotFound() {
        FakeEngine.install { table in
            table.nav_agent_state = { _, _, _ in
                FakeEngine.fail(CY_RESULT_NOT_FOUND, "nav_agent_state: the entity is not an agent")
            }
        }
        XCTAssertThrowsError(try NavAgent(Entity(bits: 5)).state) { error in
            guard case CyberdyneError.status(.notFound, _) = error else {
                return XCTFail("expected .notFound, got \(error)")
            }
        }
    }
}
