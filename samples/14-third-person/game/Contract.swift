// SPDX-License-Identifier: MIT
// Contract.swift — what the game and the host agree on: the names of the content the host loaded,
// one component the host reads to draw the hero, and the report it prints.
//
// Everything the game DOES goes through the engine's own entries: input actions, the character
// controller, the hero's animator (blend space, foot placement), the camera, audio and the scene
// tree. The host carries nothing between the game and a server. Two components cross, and both go
// from the game to the host:
//
//   HeroPose     which way the hero faces. The character controller is a capsule with no facing,
//                and ABI 1.14 has no entry that turns a scene node, so the host turns the hero's
//                node by it when it moves the node to the capsule. Presentation only.
//   HeroReport   what the game saw: steps, jumps, landings, the surfaces it walked on, how its feet
//                were placed and how the camera was pulled in. The sample prints it and its test
//                reads it; nothing in the host acts on it.

import CyberdyneKit

/// The content the host loaded, by the names the game asks for.
enum Content {
    /// The hero's rig: a skeleton, its clips and a program with a locomotion blend space through
    /// foot placement (host/hero_rig.h).
    static let rig = "hero"
    /// The program's states, and the parameters the game sets.
    static let locomotion = "locomotion"
    static let jump = "jump"
    static let fall = "fall"
    static let land = "land"
    static let speed = "speed"
    static let feet = "feet"
    /// The clips' events: a foot down, its parameter 0 for the left and 1 for the right.
    static let footstep: AnimationName = "footstep"

    /// The input actions (host/level.cpp declares and binds them).
    static let move = "hero.move"
    static let look = "camera.look"
    static let jumpAction = "hero.jump"
    static let sprint = "hero.sprint"

    /// The sounds (host/sounds.cpp synthesises them).
    static let stepStone = "step.stone"
    static let stepGrass = "step.grass"
    static let jumpCue = "hero.jump"
    static let landCue = "hero.land"
    static let wind = "ambience.wind"
    static let bird = "ambience.bird"

    /// The level's nodes.
    static let terrain = "/Level/Terrain"
}

/// Which way the hero faces, written by the game every fixed tick and read by the host to turn the
/// hero's node: radians about +Y, zero facing −Z. `speed` is the hero's own, over the ground, for
/// the camera director beside it; the host does not read it.
@Component
struct HeroPose {
    var facing: Float = 0
    var speed: Float = 0
}

/// The camera director's yaw, on the camera's node, which the hero reads to turn the player's
/// stick into a direction: a third-person control scheme moves the hero in the camera's frame.
/// Game to game; the host does not read it.
@Component
struct CameraAim {
    var yaw: Float = 0
}

/// What the game saw, for the sample's report and its test. Counters and extremes; the host reads
/// it once at the end of the run.
@Component
struct HeroReport {
    var ticks: Float = 0
    var groundedTicks: Float = 0
    var airborneTicks: Float = 0
    /// Ticks the controller lifted the capsule onto a stair.
    var steppedUp: Float = 0
    var jumps: Float = 0
    var landings: Float = 0
    /// Footfalls heard, by the surface under the hero when they fell.
    var footsteps: Float = 0
    var stoneSteps: Float = 0
    var grassSteps: Float = 0
    /// The highest the hero's feet reached, and the fastest it went over the ground.
    var highest: Float = 0
    var fastest: Float = 0
    /// Ticks the locomotion space was asked for more than a walk (a run), and for a walk.
    var runTicks: Float = 0
    var walkTicks: Float = 0
    /// Frames the feet were placed, frames they were placed with one foot raised or lowered more
    /// than four centimetres (on a stair or a slope), and the most the hips were lowered.
    var feetSolved: Float = 0
    var feetAdjusted: Float = 0
    var pelvisDrop: Float = 0
    /// The animation states the game played, as a bit set: locomotion 1, jump 2, fall 4, land 8.
    var states: Float = 0
    /// Frames the camera was pulled in front of where the game asked for it, and the closest it
    /// came, as a fraction of the distance asked for.
    var cameraPulled: Float = 0
    var cameraNearest: Float = 1
    /// The camera's yaw after the last frame, radians.
    var cameraYaw: Float = 0
    /// Ambient sounds started: the wind's loop and the birds.
    var ambience: Float = 0
    /// Sounds the mixer refused a voice for.
    var dropped: Float = 0
}

extension Component {
    /// The index of a field, by name, resolved once in `onCreate`.
    static func field(_ name: String) -> UInt32 {
        guard let index = componentFields.firstIndex(where: { $0.name == name }) else {
            Log.error("\(componentName) has no field named '\(name)'")
            return 0
        }
        return UInt32(index)
    }
}
