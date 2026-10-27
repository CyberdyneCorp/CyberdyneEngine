// SPDX-License-Identifier: MIT
// Ambience.swift — the hillside's sound: wind under everything, and birds in the trees.
//
//   onCreate       (no phase)    the wind's loop, unpositioned, quiet
//   onFixedUpdate  (fixed step)  a bird call every few seconds, somewhere in the woods round the
//                                courtyard — where and when from a fixed sequence, so two runs
//                                hear the same birds
//
// The host synthesised both sounds and named them; when and where they play is this file's.

import CyberdyneKit

@Behaviour(name: "Ambience", schema: 1)
final class Ambience: Behaviour {
    @Export(range: 0...1) var windVolume: Float = 0.35
    @Export(range: 0...1) var birdVolume: Float = 0.6
    /// Fixed ticks between calls, at the shortest, and the spread added to it.
    @Export var birdEvery: Float = 150
    @Export var birdSpread: Float = 120
    /// The woods: a ring round the courtyard, this far out and this high in the canopy.
    @Export var woodsRadius: Float = 22
    @Export var canopyHeight: Float = 6

    @Node("../Hero") var hero: Entity?

    private var started: Float = 0
    private var countdown: Float = 60
    private var sequence: UInt32 = 0x14B1_4D5E
    private var report = ComponentType(id: 0)
    private let reportAmbience = HeroReport.field("ambience")

    override func onCreate() throws {
        guard let world else { throw CyberdyneError.invalidHandle }
        report = try Components.register(HeroReport.self, in: world)
        if try Audio.play(Content.wind, options: PlayOptions(volume: windVolume, loop: true))
            != nil
        {
            started += 1
        }
    }

    override func onFixedUpdate(_ delta: Double) throws {
        countdown -= 1
        if countdown > 0 {
            return
        }
        countdown = birdEvery + Float(next() % UInt32(max(birdSpread, 1)))
        // A point on the ring, from the same sequence: an angle in 64ths of a turn.
        let turn = Float(next() % 64) / 64 * 2 * Float.pi
        let at = Vec3(
            x: woodsRadius * Ambience.sine(turn + Float.pi / 2), y: canopyHeight,
            z: woodsRadius * Ambience.sine(turn))
        if try Audio.play(Content.bird, at: at, options: PlayOptions(volume: birdVolume)) != nil {
            started += 1
        }
        guard let world, let hero, world.has(report, on: hero) else { return }
        try world.setFloat(started, hero, report, field: reportAmbience)
    }

    /// A linear congruential sequence: the same numbers on every machine.
    private func next() -> UInt32 {
        sequence = sequence &* 1_664_525 &+ 1_013_904_223
        return sequence >> 8
    }

    /// sin(x) by its Taylor series about the nearest quarter turn — enough for where a bird sings.
    static func sine(_ x: Float) -> Float {
        var angle = wrap(x)
        var sign: Float = 1
        if angle > Float.pi / 2 {
            angle = Float.pi - angle
        } else if angle < -Float.pi / 2 {
            angle = -Float.pi - angle
        }
        if angle < 0 {
            angle = -angle
            sign = -1
        }
        let a2 = angle * angle
        return sign * angle * (1 - a2 / 6 * (1 - a2 / 20 * (1 - a2 / 42)))
    }
}
