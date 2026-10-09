// SPDX-License-Identifier: MIT
import CyberdyneKit
import Foundation

/// Plays the project's baked `hero` rig on its node and reports the animation events the author
/// placed on the Animation panel's timeline, issue #29: the graph `hero.cyanimgraph`, baked by
/// `animation.bake`, attached here by its rig name, and its events read where a game reads them —
/// `Animation.events(for:)` in `onUpdate`, the frame callback.
///
/// What it saw is written into the node's translation, so a test reads it back through the world:
/// `x` counts the `footstep` events, `y` is the fixed tick the first footstep arrived on and `z`
/// the tick the first `land` did. `smoke.editor_animation_events` requires them to arrive on the
/// ticks the authored times fall in.
@Behaviour(name: "AnimatedHero", schema: 1)
final class AnimatedHero: Behaviour {
    private var transform = ComponentType.invalid
    private var ticks: Float = 0
    private var footsteps: Float = 0
    private var footstepTick: Float = 0
    private var landTick: Float = 0

    override func onCreate() throws {
        guard let world else { throw AnimatedHeroError.missingWorld }
        transform = world.find(component: "cy::scene::LocalTransform")
        guard transform.isValid else { throw AnimatedHeroError.missingTransform }
        try Animator.attach(to: entity, rig: "hero")
    }

    override func onFixedUpdate(_ delta: Double) throws {
        ticks += 1
    }

    override func onUpdate(_ delta: Double) throws {
        guard let world else { return }
        for event in try Animation.events(for: entity) {
            if event.name == "footstep" {
                footsteps += 1
                if footstepTick == 0 { footstepTick = ticks }
            } else if event.name == "land", landTick == 0 {
                landTick = ticks
            }
        }
        // The reflected LocalTransform fields are rotation x, y, z, w, then translation x, y, z.
        try world.setFloat(footsteps, entity, transform, field: 4)
        try world.setFloat(footstepTick, entity, transform, field: 5)
        try world.setFloat(landTick, entity, transform, field: 6)
    }
}

private enum AnimatedHeroError: Error {
    case missingWorld
    case missingTransform
}
