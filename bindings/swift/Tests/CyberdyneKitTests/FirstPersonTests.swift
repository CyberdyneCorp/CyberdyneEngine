// SPDX-License-Identifier: MIT
// FirstPersonTests.swift — the `FirstPersonCamera` facade through `FakeEngine`. ABI 1.13.
//
// Each case checks that a facade call reaches its entry with the right arguments — the settings,
// the flags, the struct size — converts the answer, and turns a refusal into
// `CyberdyneError.status`.

import CyberdyneABI
import CyberdyneCore
import XCTest

@testable import CyberdyneKit

/// What the fake entries saw.
private enum Seen {
    nonisolated(unsafe) static var configured = CyFirstPersonDesc()
    nonisolated(unsafe) static var input = CyFirstPersonInput()
    nonisolated(unsafe) static var kick: (Float, Float) = (0, 0)
    nonisolated(unsafe) static var weapon = CyViewmodelDesc()
    nonisolated(unsafe) static var released: CyCamera = 0
}

private let kPlayer: CyCamera = 0x1_0000_0002

final class FirstPersonTests: XCTestCase {
    override func setUp() {
        super.setUp()
        Seen.configured = CyFirstPersonDesc()
        Seen.input = CyFirstPersonInput()
        Seen.kick = (0, 0)
        Seen.weapon = CyViewmodelDesc()
        Seen.released = 0
        FakeEngine.install { table in
            table.first_person_configure = { _, camera, desc in
                guard camera == kPlayer, let desc else {
                    return FakeEngine.fail(CY_RESULT_INVALID_ARGUMENT, "no description")
                }
                guard desc.pointee.recoil_frequency > 0 else {
                    return FakeEngine.fail(CY_RESULT_INVALID_ARGUMENT, "no recoil spring")
                }
                Seen.configured = desc.pointee
                return CY_RESULT_OK
            }
            table.first_person_input = { _, camera, input in
                guard camera == kPlayer, let input else {
                    return FakeEngine.fail(CY_RESULT_NOT_FOUND, "no first-person camera")
                }
                Seen.input = input.pointee
                return CY_RESULT_OK
            }
            table.first_person_kick = { _, _, pitch, yaw in
                Seen.kick = (pitch, yaw)
                return CY_RESULT_OK
            }
            table.first_person_state = { _, _, out in
                guard let out,
                    out.pointee.struct_size == UInt32(MemoryLayout<CyFirstPersonState>.size)
                else { return FakeEngine.fail(CY_RESULT_INVALID_ARGUMENT, "a struct size") }
                out.pointee.yaw = 0.5
                out.pointee.aim = 1
                out.pointee.fov = 0.96
                out.pointee.viewmodel_fov = 0.7
                out.pointee.recoil = (0.02, -0.01)
                out.pointee.weapon.position = (0, -0.07, -0.28)
                out.pointee.weapon.rotation = (0, 0, 0, 1)
                return CY_RESULT_OK
            }
            table.first_person_release = { _, camera in
                Seen.released = camera
                return CY_RESULT_OK
            }
            table.viewmodel_configure = { _, _, desc in
                guard let desc else { return FakeEngine.fail(CY_RESULT_INVALID_ARGUMENT, "none") }
                Seen.weapon = desc.pointee
                return CY_RESULT_OK
            }
            table.viewmodel_get = { _, _, out in
                guard let out else { return FakeEngine.fail(CY_RESULT_INVALID_ARGUMENT, "none") }
                out.pointee.layers = Seen.weapon.layers
                out.pointee.fov = 0.7
                out.pointee.near_plane = Seen.weapon.near_plane
                out.pointee.far_plane = Seen.weapon.far_plane
                out.pointee.depth_fraction = Seen.weapon.depth_fraction
                out.pointee.flags = Seen.weapon.flags
                return CY_RESULT_OK
            }
        }
    }

    override func tearDown() {
        FakeEngine.uninstall()
        super.tearDown()
    }

    func testTheSettingsCrossWholeWithTheirFlags() throws {
        var settings = FirstPersonSettings()
        settings.invertY = true
        settings.lookSensitivity = 0.003
        settings.motionReduction = 0.5
        settings.aimOffset = Vec3(x: 0, y: -0.05, z: -0.25)
        _ = try FirstPersonCamera(camera: Camera(raw: kPlayer), settings: settings)
        XCTAssertEqual(
            Seen.configured.struct_size, UInt32(MemoryLayout<CyFirstPersonDesc>.size))
        XCTAssertEqual(Seen.configured.flags, CY_FIRST_PERSON_INVERT_Y)
        XCTAssertEqual(Seen.configured.look_sensitivity, 0.003)
        XCTAssertEqual(Seen.configured.motion_reduction, 0.5)
        XCTAssertEqual(Seen.configured.aim_offset.2, -0.25)
        XCTAssertEqual(Seen.configured.recoil_frequency, 6)
    }

    func testFeedingCarriesTheLookTheSpeedAndTheFlags() throws {
        let eye = try FirstPersonCamera(
            camera: Camera(raw: kPlayer), settings: FirstPersonSettings())
        try eye.feed(look: Vec2(x: 12, y: -3), speed: 4, aiming: true, airborne: true)
        XCTAssertEqual(Seen.input.look.0, 12)
        XCTAssertEqual(Seen.input.look.1, -3)
        XCTAssertEqual(Seen.input.move_speed, 4)
        XCTAssertEqual(Seen.input.flags, CY_FIRST_PERSON_AIMING | CY_FIRST_PERSON_AIRBORNE)
        try eye.kick(pitch: 3, yaw: -0.5)
        XCTAssertEqual(Seen.kick.0, 3)
        XCTAssertEqual(Seen.kick.1, -0.5)
    }

    func testTheStateReadsBack() throws {
        let eye = try FirstPersonCamera(
            camera: Camera(raw: kPlayer), settings: FirstPersonSettings())
        let state = try eye.state()
        XCTAssertEqual(state.yaw, 0.5)
        XCTAssertEqual(state.aim, 1)
        XCTAssertEqual(state.fieldOfView, 0.96)
        XCTAssertEqual(state.weaponFieldOfView, 0.7)
        XCTAssertEqual(state.recoil.x, 0.02)
        XCTAssertEqual(state.weapon.position.z, -0.28)
        try eye.release()
        XCTAssertEqual(Seen.released, kPlayer)
    }

    func testTheWeaponViewCrossesAndFollowsTheSights() throws {
        let eye = try FirstPersonCamera(
            camera: Camera(raw: kPlayer), settings: FirstPersonSettings())
        try eye.setWeaponView(WeaponView(layers: 1 << 5, castsShadows: true))
        XCTAssertEqual(Seen.weapon.layers, 1 << 5)
        XCTAssertEqual(Seen.weapon.fov, 0)  // follows the sights
        XCTAssertEqual(Seen.weapon.flags, CY_VIEWMODEL_CAST_SHADOWS)
        let read = try eye.weaponView()
        XCTAssertEqual(read.fieldOfView, 0.7)
        XCTAssertTrue(read.castsShadows)
        XCTAssertTrue(read.receivesShadows)
        XCTAssertEqual(read.nearPlane, 0.01)
        XCTAssertEqual(
            WeaponView(layers: 1, receivesShadows: false).raw.flags, CY_VIEWMODEL_IGNORE_SHADOWS)
    }

    func testAnUnknownCameraAndABadSettingThrowTheirStatus() throws {
        var bad = FirstPersonSettings()
        bad.recoilFrequency = 0
        XCTAssertThrowsError(try FirstPersonCamera(camera: Camera(raw: kPlayer), settings: bad)) {
            error in
            guard case CyberdyneError.status(let status, _) = error else {
                return XCTFail("\(error)")
            }
            XCTAssertEqual(status, .invalidArgument)
        }
        let eye = try FirstPersonCamera(
            camera: Camera(raw: kPlayer), settings: FirstPersonSettings())
        let stranger = FirstPersonCamera(attachedTo: Camera(raw: 0x9))
        XCTAssertThrowsError(try stranger.feed(look: Vec2(), speed: 0)) { error in
            guard case CyberdyneError.status(let status, _) = error else {
                return XCTFail("\(error)")
            }
            XCTAssertEqual(status, .notFound)
        }
        _ = eye
    }
}
