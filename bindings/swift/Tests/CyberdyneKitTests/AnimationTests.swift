// SPDX-License-Identifier: MIT
// AnimationTests.swift — the `Animator` and `Animation` facades through `FakeEngine`. Issue #76
// stage 4.
//
// Each case checks that a facade call reaches its entry with the right arguments, converts the
// answer, and turns a refusal into `CyberdyneError.status`.

import CyberdyneABI
import CyberdyneCore
import XCTest

@testable import CyberdyneKit

/// What the fake entries saw.
private enum Seen {
    nonisolated(unsafe) static var desc = CyAnimatorDesc()
    nonisolated(unsafe) static var rig = ""
    nonisolated(unsafe) static var played: (CyEntity, String, Float) = (0, "", -1)
    nonisolated(unsafe) static var stopped: (CyEntity, Float) = (0, -1)
    nonisolated(unsafe) static var floats: [String: Float] = [:]
    nonisolated(unsafe) static var fired: [String] = []
    nonisolated(unsafe) static var mode: UInt32 = 99
    nonisolated(unsafe) static var detached: CyEntity = 0
    nonisolated(unsafe) static var events: [CyAnimationEvent] = []
}

private let kHero: CyEntity = 0x1_0000_0007
private let kOther: CyEntity = 0x1_0000_0009

final class AnimationTests: XCTestCase {
    override func setUp() {
        super.setUp()
        Seen.desc = CyAnimatorDesc()
        Seen.rig = ""
        Seen.played = (0, "", -1)
        Seen.floats = [:]
        Seen.fired = []
        Seen.mode = 99
        Seen.events = [
            CyAnimationEvent(
                entity: kHero, name: AnimationName("footstep").hash, normalised_time: 0.5,
                parameter: 0),
            CyAnimationEvent(
                entity: kOther, name: AnimationName("wave").hash, normalised_time: 1,
                parameter: 2),
        ]
        FakeEngine.install { table in
            table.animation_attach = { _, entity, desc in
                guard let desc, let rig = desc.pointee.rig else {
                    return FakeEngine.fail(CY_RESULT_INVALID_ARGUMENT, "no desc")
                }
                Seen.desc = desc.pointee
                Seen.rig = String(cString: rig)
                return Seen.rig == "worker"
                    ? CY_RESULT_OK : FakeEngine.fail(CY_RESULT_NOT_FOUND, "no rig of that name")
            }
            table.animation_detach = { _, entity in
                Seen.detached = entity
                return CY_RESULT_OK
            }
            table.animation_play = { _, entity, state, seconds in
                let name = state.map { String(cString: $0) } ?? ""
                guard name != "fly" else {
                    return FakeEngine.fail(CY_RESULT_NOT_FOUND, "the program has no such state")
                }
                Seen.played = (entity, name, seconds)
                return CY_RESULT_OK
            }
            table.animation_stop = { _, entity, seconds in
                Seen.stopped = (entity, seconds)
                return CY_RESULT_OK
            }
            table.animation_set_float = { _, _, parameter, value in
                Seen.floats[parameter.map { String(cString: $0) } ?? ""] = value
                return CY_RESULT_OK
            }
            table.animation_set_bool = { _, _, parameter, value in
                Seen.floats[parameter.map { String(cString: $0) } ?? ""] = value ? 1 : 0
                return CY_RESULT_OK
            }
            table.animation_fire_trigger = { _, _, parameter in
                Seen.fired.append(parameter.map { String(cString: $0) } ?? "")
                return CY_RESULT_OK
            }
            table.animation_get_float = { _, _, parameter, out in
                out?.pointee = Seen.floats[parameter.map { String(cString: $0) } ?? ""] ?? -1
                return CY_RESULT_OK
            }
            table.animation_state = { _, _, out in
                guard let out, out.pointee.struct_size == UInt32(MemoryLayout<CyAnimatorState>.size)
                else {
                    return FakeEngine.fail(CY_RESULT_INVALID_ARGUMENT, "struct_size")
                }
                out.pointee.flags = CY_ANIMATOR_BLENDING | CY_ANIMATOR_REQUESTED
                out.pointee.state = AnimationName("idle").hash
                out.pointee.target = AnimationName("walk").hash
                out.pointee.blend_weight = 0.25
                out.pointee.state_time = 3
                return CY_RESULT_OK
            }
            table.animation_events = { _, into, capacity, count in
                count?.pointee = UInt32(Seen.events.count)
                guard let into else { return CY_RESULT_OK }
                guard capacity >= UInt32(Seen.events.count) else {
                    return FakeEngine.fail(CY_RESULT_BUFFER_TOO_SMALL, "more events")
                }
                for (index, event) in Seen.events.enumerated() {
                    into[index] = event
                }
                return CY_RESULT_OK
            }
            table.animation_root_motion = { _, _, out in
                out?.pointee.translation = (0, 0, -0.5)
                out?.pointee.rotation = (0, 0, 0, 1)
                out?.pointee.distance = 0.5
                out?.pointee.contacts = 1
                out?.pointee.travelled = (0, 0, -4)
                return CY_RESULT_OK
            }
            table.animation_take_root_motion = { _, entity, out in
                guard entity == kHero else {
                    return FakeEngine.fail(CY_RESULT_NOT_FOUND, "does not accumulate")
                }
                out?.pointee.translation = (1, 0, 0)
                out?.pointee.rotation = (0, 0, 0, 1)
                return CY_RESULT_OK
            }
            table.animation_set_root_motion = { _, _, mode in
                Seen.mode = mode
                return CY_RESULT_OK
            }
            table.animation_joint_pose = { _, _, joint, out in
                guard let joint, String(cString: joint) == "hand" else {
                    return FakeEngine.fail(CY_RESULT_NOT_FOUND, "no joint")
                }
                out?.pointee.position = (1, 2, 3)
                out?.pointee.rotation = (0, 0, 0, 1)
                return CY_RESULT_OK
            }
        }
    }

    override func tearDown() {
        FakeEngine.uninstall()
        super.tearDown()
    }

    func testAttachSendsTheRigTheTierAndTheRootMotionMode() throws {
        let worker = try Animator.attach(
            to: Entity(bits: kHero), rig: "worker", tier: .simplified, emitsEvents: false,
            rootMotion: .character, playRate: 1.5)
        XCTAssertEqual(worker.entity, Entity(bits: kHero))
        XCTAssertEqual(Seen.rig, "worker")
        XCTAssertEqual(Seen.desc.struct_size, UInt32(MemoryLayout<CyAnimatorDesc>.size))
        XCTAssertEqual(Seen.desc.tier, AnimationTier.simplified.rawValue)
        XCTAssertEqual(Seen.desc.root_motion, RootMotionMode.character.rawValue)
        XCTAssertEqual(Seen.desc.flags, CY_ANIMATOR_SUPPRESS_EVENTS)
        XCTAssertEqual(Seen.desc.play_rate, 1.5)
        try worker.detach()
        XCTAssertEqual(Seen.detached, kHero)
    }

    func testAnUnknownRigThrowsNotFound() {
        XCTAssertThrowsError(try Animator.attach(to: Entity(bits: kHero), rig: "dragon")) {
            guard case .status(let status, let message) = $0 as? CyberdyneError else {
                return XCTFail("expected a status, got \($0)")
            }
            XCTAssertEqual(status, .notFound)
            XCTAssertEqual(message, "no rig of that name")
        }
    }

    func testPlayStopAndParametersReachTheirEntries() throws {
        let worker = Animator(Entity(bits: kHero))
        try worker.play("walk", crossfade: 0.3)
        XCTAssertEqual(Seen.played.0, kHero)
        XCTAssertEqual(Seen.played.1, "walk")
        XCTAssertEqual(Seen.played.2, 0.3)
        try worker.play("salute")
        XCTAssertEqual(Seen.played.2, 0.2)
        try worker.stop(blend: 0)
        XCTAssertEqual(Seen.stopped.1, 0)
        try worker.set("speed", to: 1.25)
        try worker.set("armed", to: true)
        try worker.fire("wave")
        XCTAssertEqual(Seen.floats["speed"], 1.25)
        XCTAssertEqual(Seen.floats["armed"], 1)
        XCTAssertEqual(Seen.fired, ["wave"])
        XCTAssertEqual(try worker.float("speed"), 1.25)
        XCTAssertThrowsError(try worker.play("fly"))
    }

    func testStateNamesCompareAgainstStrings() throws {
        let state = try Animator(Entity(bits: kHero)).state
        XCTAssertEqual(state.state, "idle")
        XCTAssertEqual(state.target, "walk")
        XCTAssertTrue(state.isPlaying("walk"))
        XCTAssertFalse(state.isPlaying("run"))
        XCTAssertTrue(state.requested)
        XCTAssertEqual(state.blendWeight, 0.25)
        XCTAssertEqual(state.stateTime, 3)
    }

    func testTheNameHashIsTheEnginesFnv1a() {
        // The published FNV-1a 64 vectors, which unit.abi holds `cy::abi::game::name_hash` to.
        XCTAssertEqual(AnimationName.hash(""), 14_695_981_039_346_656_037)
        XCTAssertEqual(AnimationName.hash("a"), 0xaf63_dc4c_8601_ec8c)
        XCTAssertEqual(AnimationName.hash("foobar"), 0x8594_4171_f739_67e8)
        XCTAssertNotEqual(AnimationName("walk"), AnimationName("run"))
    }

    func testEventsAreSizedThenCopiedAndFilteredByEntity() throws {
        let all = try Animation.events()
        XCTAssertEqual(all.count, 2)
        XCTAssertEqual(all[0].name, "footstep")
        XCTAssertEqual(all[0].normalisedTime, 0.5)
        XCTAssertEqual(all[1].parameter, 2)
        let mine = try Animation.events(for: Entity(bits: kHero))
        XCTAssertEqual(mine.count, 1)
        XCTAssertEqual(mine[0].entity, Entity(bits: kHero))
        Seen.events = []
        XCTAssertTrue(try Animation.events().isEmpty)
    }

    func testRootMotionIsReadTakenAndRouted() throws {
        let worker = Animator(Entity(bits: kHero))
        let motion = try worker.rootMotion
        XCTAssertEqual(motion.translation, Vec3(x: 0, y: 0, z: -0.5))
        XCTAssertEqual(motion.travelled.z, -4)
        XCTAssertEqual(motion.contacts, 1)
        XCTAssertEqual(try worker.takeRootMotion().translation.x, 1)
        XCTAssertThrowsError(try Animator(Entity(bits: kOther)).takeRootMotion())
        try worker.setRootMotion(.accumulate)
        XCTAssertEqual(Seen.mode, RootMotionMode.accumulate.rawValue)
    }

    func testAJointPoseIsAWorldPose() throws {
        let pose = try Animator(Entity(bits: kHero)).jointPose("hand")
        XCTAssertEqual(pose.position, Vec3(x: 1, y: 2, z: 3))
        XCTAssertThrowsError(try Animator(Entity(bits: kHero)).jointPose("tail"))
    }

    func testWithNoEngineEveryCallThrowsUnavailable() {
        FakeEngine.uninstall()
        XCTAssertThrowsError(try Animator(Entity(bits: kHero)).play("walk")) {
            guard case .status(let status, _) = $0 as? CyberdyneError else {
                return XCTFail("expected a status, got \($0)")
            }
            XCTAssertEqual(status, .unavailable)
        }
        XCTAssertThrowsError(try Animation.events())
    }
}
