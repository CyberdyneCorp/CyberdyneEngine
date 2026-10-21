// SPDX-License-Identifier: MIT
// Lighting.swift — the base's light, through ABI 1.9 (`Illumination`, `GIVolume`). Issue #35.
//
// The host hands the level to the engine's global illumination and runs it once a frame from the
// camera (host/illumination.h); every decision about it is here. The commander:
//
//   onCreate     places a GI volume over its base — the barracks and the beacon beside it — at the
//                best quality and twice the update rate, so the base converges first and best;
//   onUpdate     reads the light on a guard standing in front of the beacon, puts the beacon
//                out by its glass's material name, reads it again once the GI has settled, lights
//                it again, and reads a third time — the numbers the sample's report prints and its
//                test checks;
//   a fixed step asks once, and is refused: illumination is presentation, so the simulation never
//                reads it.
//
// The same calls drive the picture when the host renders (`--render`): the device GI the frame is
// lit with is this system, so the beacon going out is visible in the next frames.

import CyberdyneKit

struct BaseLighting {
    /// The base's box, and a guard standing in front of the beacon, facing it: the light on its
    /// chest is what is read.
    static let base = (min: Vec3(x: 3, y: 0, z: 22), max: Vec3(x: 16, y: 5, z: 31))
    static let guardSpot = Vec3(x: 12.5, y: 1.0, z: 24.4)
    static let facingBeacon = Vec3(x: 0, y: 0, z: 1)
    /// The beacon's glass, as the host registered its material.
    static let beacon = "beacon"
    /// The frames the light is read at and the beacon switched on.
    static let readLit: UInt64 = 150
    static let putOut: UInt64 = 151
    static let readDark: UInt64 = 250
    static let lightAgain: UInt64 = 251
    static let readRelit: UInt64 = 350

    private(set) var volume: GIVolume?
    /// Volumes the engine held after the base's was placed.
    private(set) var volumes: Float = 0
    /// The luminance on the guard's chest: lit, out, and lit again.
    private(set) var lit: Float = 0
    private(set) var dark: Float = 0
    private(set) var relit: Float = 0
    /// Calls the engine refused from a fixed step.
    private(set) var refused: Float = 0
    private var frames: UInt64 = 0

    /// `onCreate`: the base's volume. No phase, which ABI 1.9 accepts.
    mutating func create() throws {
        volume = try GIVolume.place(
            .init(
                bounds: Self.base, overrides: [.quality(.epic), .updateRate(2)], priority: 1,
                blendDistance: 2))
        volumes = Float(try Illumination.status.volumes)
    }

    /// `onUpdate`: the beacon out and on again, and the light read in between.
    mutating func frame() throws {
        frames += 1
        switch frames {
        case Self.readLit:
            lit = try Illumination.irradiance(at: Self.guardSpot, normal: Self.facingBeacon)
                .luminance
        case Self.putOut:
            try Illumination.setEmission(.off, ofMaterial: Self.beacon)
        case Self.readDark:
            dark = try Illumination.irradiance(at: Self.guardSpot, normal: Self.facingBeacon)
                .luminance
        case Self.lightAgain:
            try Illumination.setEmission(.automatic, ofMaterial: Self.beacon)
        case Self.readRelit:
            relit = try Illumination.irradiance(at: Self.guardSpot, normal: Self.facingBeacon)
                .luminance
        default:
            break
        }
    }

    /// `onFixedUpdate`, once: a fixed step that asks for the light is refused.
    mutating func askFromFixedStep() {
        guard refused == 0 else { return }
        do {
            _ = try Illumination.status
        } catch {
            refused += 1
        }
    }
}
