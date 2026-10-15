// SPDX-License-Identifier: MIT
// IlluminationTests.swift — the `Illumination` and `GIVolume` facades through `FakeEngine`.
// Issue #35.
//
// Each case checks that a facade call reaches its entry with the right arguments, converts the
// answer, and turns a refusal into `CyberdyneError.status`.

import CyberdyneABI
import CyberdyneCore
import XCTest

@testable import CyberdyneKit

/// What the fake entries saw.
private enum Seen {
    nonisolated(unsafe) static var settings = CyGiSettings()
    nonisolated(unsafe) static var volume = CyGiVolumeDesc()
    nonisolated(unsafe) static var updated: CyGiVolume = 0
    nonisolated(unsafe) static var destroyed: CyGiVolume = 0
    nonisolated(unsafe) static var klass: (CyEntity, UInt32) = (0, 99)
    nonisolated(unsafe) static var emission: (CyEntity, UInt64, UInt32) = (0, 0, 99)
    nonisolated(unsafe) static var position: [Float] = []
    nonisolated(unsafe) static var normal: [Float]? = nil
}

private let kTorch: CyEntity = 0x1_0000_0011
private let kStranger: CyEntity = 0x9_0000_0001

final class IlluminationTests: XCTestCase {
    override func setUp() {
        super.setUp()
        Seen.settings = CyGiSettings()
        Seen.volume = CyGiVolumeDesc()
        Seen.updated = 0
        Seen.destroyed = 0
        Seen.klass = (0, 99)
        Seen.emission = (0, 0, 99)
        Seen.position = []
        Seen.normal = nil
        FakeEngine.install { table in
            table.gi_get_settings = { _, out in
                guard let out else { return FakeEngine.fail(CY_RESULT_INVALID_ARGUMENT, "no out") }
                out.pointee.mode = CY_GI_MODE_HYBRID.rawValue
                out.pointee.quality = CY_GI_QUALITY_EPIC.rawValue
                out.pointee.tier = CY_GI_TIER_SOFTWARE_ONLY.rawValue
                out.pointee.flags = 0
                out.pointee.budget_ms = 1.5
                return CY_RESULT_OK
            }
            table.gi_set_settings = { _, settings in
                guard let settings, settings.pointee.budget_ms > 0 else {
                    return FakeEngine.fail(CY_RESULT_INVALID_ARGUMENT, "a budget is positive")
                }
                Seen.settings = settings.pointee
                return CY_RESULT_OK
            }
            table.gi_volume_create = { _, desc, out in
                guard let desc, let out else {
                    return FakeEngine.fail(CY_RESULT_INVALID_ARGUMENT, "no desc")
                }
                guard desc.pointee.bounds_max.0 > desc.pointee.bounds_min.0 else {
                    return FakeEngine.fail(CY_RESULT_INVALID_ARGUMENT, "the box is empty")
                }
                Seen.volume = desc.pointee
                out.pointee = 7
                return CY_RESULT_OK
            }
            table.gi_volume_update = { _, volume, desc in
                guard volume == 7, let desc else {
                    return FakeEngine.fail(CY_RESULT_NOT_FOUND, "no such volume")
                }
                Seen.updated = volume
                Seen.volume = desc.pointee
                return CY_RESULT_OK
            }
            table.gi_volume_destroy = { _, volume in
                guard volume == 7 else {
                    return FakeEngine.fail(CY_RESULT_NOT_FOUND, "no such volume")
                }
                Seen.destroyed = volume
                return CY_RESULT_OK
            }
            table.gi_object_set_class = { _, entity, klass in
                guard entity == kTorch else {
                    return FakeEngine.fail(CY_RESULT_NOT_FOUND, "not an object of the GI")
                }
                Seen.klass = (entity, klass)
                return CY_RESULT_OK
            }
            table.gi_object_class = { _, _, out in
                out?.pointee = CY_GI_CLASS_SLOW_DYNAMIC.rawValue
                return CY_RESULT_OK
            }
            table.gi_emission_set = { _, entity, material, mode in
                Seen.emission = (entity, material, mode)
                return CY_RESULT_OK
            }
            table.gi_sample_irradiance = { _, position, normal, out in
                guard let position, let out else {
                    return FakeEngine.fail(CY_RESULT_INVALID_ARGUMENT, "no position")
                }
                Seen.position = [position[0], position[1], position[2]]
                Seen.normal = normal.map { [$0[0], $0[1], $0[2]] }
                out.pointee.radiance = (0.25, 0.5, 0.75)
                out.pointee.luminance = 0.48
                out.pointee.confidence = 0.9
                out.pointee.source = CY_GI_SOURCE_RADIANCE_CACHE.rawValue
                out.pointee.flags = normal == nil ? CY_GI_SAMPLE_OMNIDIRECTIONAL : 0
                return CY_RESULT_OK
            }
            table.gi_status = { _, out in
                out?.pointee.world_tier = CY_GI_SOURCE_HARDWARE_TRACE.rawValue
                out?.pointee.convergence = 0.75
                out?.pointee.volumes = 2
                out?.pointee.promoted_lights = 3
                out?.pointee.disabled_emitters = 1
                out?.pointee.frame = 40
                return CY_RESULT_OK
            }
        }
    }

    override func tearDown() {
        FakeEngine.uninstall()
        super.tearDown()
    }

    func testSettingsReadAndApplyEveryField() throws {
        let read = try Illumination.settings
        XCTAssertEqual(read.mode, .hybrid)
        XCTAssertEqual(read.quality, .epic)
        XCTAssertEqual(read.tier, .softwareOnly)
        XCTAssertFalse(read.hardwareRayTracing)
        XCTAssertEqual(read.budgetMilliseconds, 1.5)

        var options = Illumination.Settings()
        options.quality = .high
        options.hardwareRayTracing = true
        options.budgetMilliseconds = 2
        try Illumination.apply(options)
        XCTAssertEqual(Seen.settings.struct_size, UInt32(MemoryLayout<CyGiSettings>.size))
        XCTAssertEqual(Seen.settings.quality, CY_GI_QUALITY_HIGH.rawValue)
        XCTAssertEqual(Seen.settings.mode, CY_GI_MODE_DYNAMIC.rawValue)
        XCTAssertEqual(Seen.settings.flags, CY_GI_HARDWARE_RAY_TRACING)
        XCTAssertEqual(Seen.settings.budget_ms, 2)

        options.budgetMilliseconds = 0
        XCTAssertThrowsError(try Illumination.apply(options)) { error in
            guard case CyberdyneError.status(let status, _) = error else {
                return XCTFail("\(error)")
            }
            XCTAssertEqual(status, .invalidArgument)
        }
    }

    func testAVolumeCarriesExactlyTheOverridesItNames() throws {
        let description = GIVolume.Description(
            bounds: (min: Vec3(x: -1, y: 0, z: -2), max: Vec3(x: 3, y: 4, z: 5)),
            overrides: [.quality(.epic), .updateRate(2), .farField(25)],
            priority: 4, blendDistance: 0.5, weight: 0.75)
        let volume = try GIVolume.place(description)
        XCTAssertEqual(volume.handle, 7)
        XCTAssertEqual(
            Seen.volume.sets,
            CY_GI_VOLUME_SETS_QUALITY | CY_GI_VOLUME_SETS_UPDATE_RATE | CY_GI_VOLUME_SETS_FAR_FIELD)
        XCTAssertEqual(Seen.volume.quality, CY_GI_QUALITY_EPIC.rawValue)
        XCTAssertEqual(Seen.volume.update_rate, 2)
        XCTAssertEqual(Seen.volume.far_field_metres, 25)
        XCTAssertEqual(Seen.volume.probe_density, 0)  // not named, not set
        XCTAssertEqual(Seen.volume.bounds_min.2, -2)
        XCTAssertEqual(Seen.volume.bounds_max.1, 4)
        XCTAssertEqual(Seen.volume.priority, 4)
        XCTAssertEqual(Seen.volume.blend_distance, 0.5)
        XCTAssertEqual(Seen.volume.weight, 0.75)

        var moved = description
        moved.overrides = [
            .diffuseIntensity(0.5), .specularIntensity(2), .tier(.worldOnly),
            .probeDensity(3),
        ]
        try volume.update(moved)
        XCTAssertEqual(Seen.updated, 7)
        XCTAssertEqual(
            Seen.volume.sets,
            CY_GI_VOLUME_SETS_DIFFUSE_INTENSITY | CY_GI_VOLUME_SETS_SPECULAR_INTENSITY
                | CY_GI_VOLUME_SETS_TIER | CY_GI_VOLUME_SETS_PROBE_DENSITY)
        XCTAssertEqual(Seen.volume.tier, CY_GI_TIER_WORLD_ONLY.rawValue)
        try volume.remove()
        XCTAssertEqual(Seen.destroyed, 7)
        XCTAssertThrowsError(try GIVolume(handle: 8).remove())

        let empty = GIVolume.Description(
            bounds: (min: Vec3(x: 1, y: 0, z: 0), max: Vec3(x: 0, y: 1, z: 1)), overrides: [])
        XCTAssertThrowsError(try GIVolume.place(empty))
    }

    func testClassesAndEmissionReachTheirEntries() throws {
        try Illumination.setClass(.fastDynamic, of: Entity(bits: kTorch))
        XCTAssertEqual(Seen.klass.0, kTorch)
        XCTAssertEqual(Seen.klass.1, CY_GI_CLASS_FAST_DYNAMIC.rawValue)
        try Illumination.setClass(.automatic, of: Entity(bits: kTorch))
        XCTAssertEqual(Seen.klass.1, CY_GI_CLASS_AUTOMATIC.rawValue)
        XCTAssertEqual(try Illumination.illuminationClass(of: Entity(bits: kTorch)), .slowDynamic)
        XCTAssertThrowsError(try Illumination.setClass(.static, of: Entity(bits: kStranger)))

        try Illumination.setEmission(.off, of: Entity(bits: kTorch))
        XCTAssertEqual(Seen.emission.0, kTorch)
        XCTAssertEqual(Seen.emission.1, 0)
        XCTAssertEqual(Seen.emission.2, CY_GI_EMISSION_OFF.rawValue)

        // A material crosses as CY_NAME_HASH of its name: FNV-1a, 64 bits.
        try Illumination.setEmission(.promote, ofMaterial: "neon")
        XCTAssertEqual(Seen.emission.0, 0)
        XCTAssertEqual(Seen.emission.1, AnimationName.hash("neon"))
        XCTAssertEqual(Seen.emission.2, CY_GI_EMISSION_PROMOTE.rawValue)
        try Illumination.setEmission(.cacheOnly, ofMaterial: "neon", on: Entity(bits: kTorch))
        XCTAssertEqual(Seen.emission.0, kTorch)
    }

    func testIrradianceAtABodyAndAtASurface() throws {
        let body = try Illumination.irradiance(at: Vec3(x: 1, y: 2, z: 3))
        XCTAssertEqual(Seen.position, [1, 2, 3])
        XCTAssertNil(Seen.normal)
        XCTAssertTrue(body.omnidirectional)
        XCTAssertEqual(body.radiance, Vec3(x: 0.25, y: 0.5, z: 0.75))
        XCTAssertEqual(body.luminance, 0.48)
        XCTAssertEqual(body.confidence, 0.9)
        XCTAssertEqual(body.source, .radianceCache)

        let floor = try Illumination.irradiance(at: Vec3(), normal: Vec3(x: 0, y: 1, z: 0))
        XCTAssertEqual(Seen.normal, [0, 1, 0])
        XCTAssertFalse(floor.omnidirectional)
    }

    func testStatusReadsTheLastFrame() throws {
        let status = try Illumination.status
        XCTAssertEqual(status.worldTier, .hardwareTrace)
        XCTAssertEqual(status.convergence, 0.75)
        XCTAssertEqual(status.volumes, 2)
        XCTAssertEqual(status.promotedLights, 3)
        XCTAssertEqual(status.disabledEmitters, 1)
        XCTAssertEqual(status.frame, 40)
    }

    func testNoEngineThrowsRatherThanTraps() {
        FakeEngine.uninstall()
        XCTAssertThrowsError(try Illumination.status)
        XCTAssertThrowsError(try Illumination.irradiance(at: Vec3()))
    }
}
