// SPDX-License-Identifier: MIT
// TimeTests.swift — `Time`, and the frame callback the bridge registers, through `FakeEngine`.
// `add-swift-game-api`.
//
// OWNER: implementer C.

import CyberdyneABI
import CyberdyneCore
import XCTest

@testable import CyberdyneKit

@Behaviour
final class TimeTestUnit: Behaviour {
    private(set) var frames: [Double] = []

    override func onUpdate(_ delta: Double) {
        frames.append(delta)
    }
}

@Behaviour
final class TimeTestStatic: Behaviour {
    override func onFixedUpdate(_ delta: Double) {}
}

/// What the fake `register_behaviour` was handed.
private enum Registered {
    nonisolated(unsafe) static var vtable = CyBehaviourVTable()
}

final class TimeTests: XCTestCase {
    override func tearDown() {
        FakeEngine.uninstall()
        super.tearDown()
    }

    func testTimeNowConvertsEveryField() throws {
        FakeEngine.install { table in
            table.time_get = { _, time in
                guard let time, time.pointee.struct_size == UInt32(MemoryLayout<CyTime>.size)
                else { return FakeEngine.fail(CY_RESULT_INVALID_ARGUMENT, "struct_size") }
                time.pointee.phase = Phase.frameUpdate.rawValue
                time.pointee.tick = 900
                time.pointee.fixed_delta = 1.0 / 30.0
                time.pointee.frame_delta = 0.016
                time.pointee.interpolation = 0.5
                time.pointee.flags = CY_TIME_RESIMULATING
                return CY_RESULT_OK
            }
        }
        let now = try Time.now
        XCTAssertEqual(now.phase, .frameUpdate)
        XCTAssertEqual(now.tick, 900)
        XCTAssertEqual(now.fixedDelta, 1.0 / 30.0)
        XCTAssertEqual(now.frameDelta, 0.016)
        XCTAssertEqual(now.interpolation, 0.5)
        XCTAssertTrue(now.isResimulating)
        XCTAssertFalse(now.isPaused)
        XCTAssertEqual(try Time.phase, .frameUpdate)
    }

    func testTimeThrowsTheEnginesRefusal() {
        FakeEngine.install { table in
            table.time_get = { _, _ in FakeEngine.fail(CY_RESULT_INVALID_ARGUMENT, "bad output") }
        }
        XCTAssertThrowsError(try Time.now) { error in
            XCTAssertEqual(
                error as? CyberdyneError, .status(.invalidArgument, message: "bad output"))
        }
    }

    func testTimeWithNoEngineThrowsUnavailable() {
        FakeEngine.uninstall()
        XCTAssertThrowsError(try Time.now) { error in
            guard case CyberdyneError.status(let status, _) = error else {
                return XCTFail("expected a status error, got \(error)")
            }
            XCTAssertEqual(status, .unavailable)
        }
    }

    /// `onUpdate` is what the engine's `frame_update` reaches, and a class that did not write it
    /// registers no frame callback at all.
    func testTheFrameCallbackIsRegisteredOnlyForAClassWithOnUpdate() throws {
        FakeEngine.install { table in
            table.register_behaviour = { _, _, vtable in
                Registered.vtable = vtable?.pointee ?? CyBehaviourVTable()
                return OpaquePointer(bitPattern: 0xB0)
            }
        }
        try Behaviours.register(TimeTestStatic.self)
        XCTAssertNil(Registered.vtable.frame_update)
        XCTAssertNotNil(Registered.vtable.fixed_update)

        try Behaviours.register(TimeTestUnit.self)
        let vtable = Registered.vtable
        XCTAssertEqual(vtable.struct_size, UInt32(MemoryLayout<CyBehaviourVTable>.size))
        let frame = try XCTUnwrap(vtable.frame_update)
        let instance = try XCTUnwrap(vtable.create?(FakeEngine.handle, 5, vtable.user_data))
        frame(instance, 0.25, vtable.user_data)
        frame(instance, 0.5, vtable.user_data)
        let unit =
            Unmanaged<Behaviour>.fromOpaque(UnsafeRawPointer(instance))
            .takeUnretainedValue() as? TimeTestUnit
        XCTAssertEqual(unit?.frames, [0.25, 0.5])
        vtable.destroy?(instance, vtable.user_data)
    }
}
