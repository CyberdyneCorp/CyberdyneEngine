// SPDX-License-Identifier: MIT
// The Swift side of `integration.swift_illumination`: a game behaviour that drives the engine's
// global illumination through CyberdyneKit alone — ABI 1.9 over a real `IlluminationSystem`.
//
// Every result it reads back is exported as an integer (a luminance in millionths), so the C++
// suite reads it through the module's own `serialize` without decoding floats.
import CyberdyneKit

@Behaviour(name: "LightProbe", schema: 1)
final class LightProbe: Behaviour {
    /// The quality the GI reports after the probe asked for `.high`, as `GIQuality`'s raw value.
    @Export var quality: Int64 = -1
    /// GI volumes placed while the probe's volume existed, and after it was removed.
    @Export var volumesPlaced: Int64 = -1
    @Export var volumesAfter: Int64 = -1
    /// The indirect light where a guard stands, in millionths of a luminance unit: with the lamp
    /// on, and after the probe switched its glass off.
    @Export var litMicro: Int64 = -1
    @Export var darkMicro: Int64 = -1
    /// Promoted emitter lights after the glass was switched off.
    @Export var lightsAfterOff: Int64 = -1
    /// The cart's class after the probe marked it static, as `GIClass`'s raw value.
    @Export var cartClass: Int64 = -1
    /// Calls the engine refused in a frame, and calls a fixed step had refused.
    @Export var refusedInFrame: Int64 = 0
    @Export var refusedInStep: Int64 = 0
    /// Frames the probe has run.
    @Export var frames: Int64 = 0

    override func onUpdate(_ delta: Double) throws {
        frames += 1
        do {
            if frames == 1 {
                var settings = try Illumination.settings
                settings.quality = .high
                try Illumination.apply(settings)
                quality = Int64(try Illumination.settings.quality.rawValue)
                let volume = try GIVolume.place(
                    .init(
                        bounds: (min: Vec3(x: 1, y: -2, z: 1), max: Vec3(x: 3, y: 0, z: 3)),
                        overrides: [.diffuseIntensity(0.5)]))
                volumesPlaced = Int64(try Illumination.status.volumes)
                try volume.remove()
                volumesAfter = Int64(try Illumination.status.volumes)
                litMicro = micro(try Illumination.irradiance(at: Vec3(x: 2, y: -1.5, z: 2)))
                try Illumination.setEmission(.off, ofMaterial: "lamp-glass")
                lightsAfterOff = Int64(try Illumination.status.promotedLights)
                try Illumination.setClass(.static, of: Entity(bits: 0x1_0000_0043))
                cartClass = Int64(
                    try Illumination.illuminationClass(of: Entity(bits: 0x1_0000_0043)).rawValue)
            } else if frames == 2 {
                // The host converged the GI between the two frames.
                darkMicro = micro(try Illumination.irradiance(at: Vec3(x: 2, y: -1.5, z: 2)))
            }
        } catch {
            refusedInFrame += 1
        }
    }

    override func onFixedUpdate(_ delta: Double) throws {
        // Illumination is presentation: a fixed step is refused, so the simulation never reads it.
        do {
            _ = try Illumination.irradiance(at: Vec3(x: 2, y: -1.5, z: 2))
        } catch {
            refusedInStep += 1
        }
    }

    private func micro(_ light: Illumination.Irradiance) -> Int64 {
        Int64((Double(light.luminance) * 1_000_000).rounded())
    }
}

@GameModule
enum IlluminationFixture: GameModule {
    static let behaviours: [any BehaviourClass.Type] = [LightProbe.self]
}
