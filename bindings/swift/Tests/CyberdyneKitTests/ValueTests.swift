// ValueTests.swift — `CyVar` marshalling with no engine. Tasks 3.1, 3.5.
//
// Only the INLINE kinds are exercised here. A string or a bytes value is allocated by the engine —
// `var_make_string` is an interface entry — so its round trip needs a host and is asserted in
// `integration.swift_reload`. Testing the inline path without an engine is not a compromise: the
// inline path is where a tag and a payload can be paired wrongly, and pairing them wrongly is the
// failure the hot-reload spike measured (a `String`'s bits read as 3.5e18).

import XCTest

@testable import CyberdyneKit

final class ValueTests: XCTestCase {
    func testTagsMatchTheGeneratedEnum() {
        XCTAssertEqual(Value.none.type, .nil)
        XCTAssertEqual(Value.bool(true).type, .bool)
        XCTAssertEqual(Value.i64(0).type, .i64)
        XCTAssertEqual(Value.f32(0).type, .f32)
        XCTAssertEqual(Value.f64(0).type, .f64)
        XCTAssertEqual(Value.vec2(.zero).type, .vec2)
        XCTAssertEqual(Value.vec3(.zero).type, .vec3)
        XCTAssertEqual(Value.vec4(.zero).type, .vec4)
        XCTAssertEqual(Value.quat(.zero).type, .quat)
        XCTAssertEqual(Value.string("").type, .string)
        XCTAssertEqual(Value.bytes([]).type, .bytes)
        XCTAssertEqual(Value.entity(.null).type, .entity)
    }

    func testInlineValuesSurviveACyVarRoundTrip() {
        let cases: [Value] = [
            .none, .bool(true), .bool(false), .i64(-1), .i64(Int64.max),
            .f32(1.5), .f64(-0.25),
            .vec2(Vec2(x: 1, y: 2)), .vec3(Vec3(x: 1, y: 2, z: 3)),
            .vec4(Vec4(x: 1, y: 2, z: 3, w: 4)), .quat(Quat(x: 0, y: 0, z: 0, w: 1)),
            .entity(Entity(bits: 0x0000_0003_0000_0009)),
        ]
        for value in cases {
            let variable = value.inlineCyVar
            XCTAssertEqual(variable.type, value.type.rawValue)
            XCTAssertEqual(variable.flags, 0, "an inline value is never owned")
            XCTAssertEqual(variable.length, 0, "only string and bytes carry a length")
            XCTAssertEqual(Value(reading: variable), value)
        }
    }

    func testAnUnknownTagIsRefusedRatherThanRead() {
        var variable = CyVar()
        variable.type = 9999
        XCTAssertNil(Value(reading: variable),
                     "a tag this module predates has an unknown payload shape")
    }

    func testExportableConversionsAreTotalAndTyped() {
        XCTAssertEqual(Float(cyValue: .f32(2.5)), 2.5)
        XCTAssertNil(Float(cyValue: .f64(2.5)), "a Double is not a Float across the boundary")
        XCTAssertEqual(Int32(cyValue: .i64(7)), 7)
        XCTAssertNil(Int32(cyValue: .i64(Int64(Int32.max) + 1)), "an out-of-range integer is refused")
        XCTAssertEqual(String(cyValue: .string("x")), "x")
        XCTAssertEqual(Vec3(cyValue: .vec3(Vec3(x: 1, y: 2, z: 3))), Vec3(x: 1, y: 2, z: 3))
        XCTAssertNil(Vec3(cyValue: .vec2(.zero)))
    }

    func testWithoutAnEngineAStringValueThrowsRatherThanTrapping() {
        XCTAssertThrowsError(try Value.string("hello").withCyVar { _ in }) { error in
            XCTAssertEqual(error as? CyberdyneError, .invalidHandle)
        }
    }

    func testTheRuntimeIsUnboundOutsideAModule() {
        XCTAssertFalse(Runtime.isBoundToEngine)
        XCTAssertNil(Runtime.engine)
        XCTAssertNil(Runtime.world)
    }
}

/// The names a registration hands the engine outlive it. Task 3.2.
///
/// REGRESSION. `cy_abi.h`: "Names must outlive the registration, because the engine stores the
/// pointer — a string literal or a static buffer, never a temporary." The first version of
/// `Behaviours.register` used `withCString`, whose pointer dies with the closure; registration
/// reported success and `find_behaviour` then answered null against freed memory. The engine-facing
/// half of this is `integration.swift_reload`'s first case; this is the half that can be checked
/// with no engine.
final class RetainedCStringTests: XCTestCase {
    func testAPointerHandedOverStaysValidAfterItsSourceIsGone() {
        let before = RetainedCString.count
        var pointer: UnsafePointer<CChar>?
        do {
            let name = String("SwiftCounter".reversed()).reversed().map(String.init).joined()
            pointer = RetainedCString.make(name)
        }
        XCTAssertEqual(RetainedCString.count, before + 1)
        // The `String` that produced it is out of scope and was built at run time, so nothing but
        // the retained copy keeps these bytes alive.
        XCTAssertEqual(String(cString: pointer!), "SwiftCounter")
    }

    func testEveryNameIsKept() {
        let before = RetainedCString.count
        for index in 0 ..< 8 {
            _ = RetainedCString.make("name\(index)")
        }
        XCTAssertEqual(RetainedCString.count, before + 8,
                       "nothing is ever freed: an image is never unloaded, so there is no later "
                           + "moment at which freeing one becomes safe")
    }
}
