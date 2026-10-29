// SPDX-License-Identifier: MIT
// Ambience.swift — the hillside's sound: wind under everything, and birds in the trees.
//
//   onCreate       (no phase)    the wind: the host's STREAMED hillside bed when it registered
//                                one (issue #87, ABI 1.18), else the in-memory wind loop
//   onFixedUpdate  (fixed step)  a bird call every few seconds, somewhere in the woods round the
//                                courtyard — where and when from a fixed sequence, so two runs
//                                hear the same birds
//
// The host synthesised the loop and the birds and named them, and cooked the streamed bed; when
// and where they play is this file's. The bed fades in over two seconds: a stream that starts at
// full level mid-gust is a click on the first frame.

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
        if try startWind() {
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

    /// The streamed bed if the host has one, else the in-memory loop. Either is one ambient sound
    /// started, which is all the report counts.
    private func startWind() throws -> Bool {
        if let bed = try? Audio.track(Content.windBed) {
            return try Audio.stream(bed, options: StreamOptions(volume: windVolume, fadeIn: 2))
                != nil
        }
        return try Audio.play(Content.wind, options: PlayOptions(volume: windVolume, loop: true))
            != nil
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
