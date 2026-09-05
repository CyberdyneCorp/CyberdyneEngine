// Level.swift — the level, authored in the game. Task 5.1.
//
// A list of axis-aligned boxes, each on an entity of its own with a `LevelBox` component. The host
// walks them once at bring-up and creates one static physics body per box; it never reads the list
// again and nothing in its C++ knows where a wall is.
//
// AXIS-ALIGNED BOXES ONLY, AND THAT IS A REAL LIMIT RATHER THAN A SIMPLIFICATION NOBODY MENTIONED.
// `LevelBox` carries a centre and half-extents and no orientation, so a ramp here is a flight of
// shallow steps rather than a rotated slab. That is enough to exercise what the milestone claims —
// the character is lifted onto a step below its step offset and blocked by one above it, and the
// difference between those two is a gameplay decision made in Character.swift — and it keeps the
// component the smallest thing that can express a level. A rotated box is a `Quat` field away and
// belongs to whichever sample first needs a slope.
//
// THE LEVEL IS CONTENT AND CONTENT IS THE GAME'S. samples/01 and samples/02 build their content in
// C++ because their content is the thing being demonstrated. Here the demonstration is that the C++
// has no stake in the game at all, and a host that knew where the walls were would have one.

import CyberdyneKit

@Behaviour(name: "Level", schema: 1)
final class Level: Behaviour {
    /// One box. A local type rather than a component: this is the AUTHORED form, and only the boxes
    /// it produces cross the boundary.
    private struct Box {
        let center: Vec3
        let half: Vec3
    }

    /// The level. Read it as a plan: a floor, a wall to walk into, a flight of four 0.3 m steps up
    /// to a platform, a pillar to slide around, and one step of 0.6 m that the character's 0.4 m
    /// step offset refuses.
    ///
    /// The 0.3 m and the 0.6 m are the pair that make the collision claim checkable: the first is
    /// below `Character.stepOffset` and is climbed, the second is above it and is not, and the number
    /// that separates them lives in Character.swift.
    ///
    /// EVERY PIECE IS SIX METRES DEEP IN Z, and that is a correction rather than a preference. The
    /// first version made the steps two metres deep, centred on the origin; the character walks into
    /// the wall at z = -4 and then strafes along z = -3.4, which is outside them — so it walked past
    /// the whole staircase at floor level and nothing failed. Content that the player cannot reach is
    /// content that is not being tested, and it looks exactly like content that works.
    private static let layout: [Box] = [
        // The floor: 60 m square, a half-metre thick, its top surface at y = 0.
        Box(center: Vec3(x: 0, y: -0.5, z: 0), half: Vec3(x: 30, y: 0.5, z: 30)),

        // Three walls, so the scripted walk is bounded on the sides it uses. A level with no edges is
        // a level a character walks off, and a run that ends in free fall measures the fall.
        Box(center: Vec3(x: 0, y: 1.0, z: -4), half: Vec3(x: 16, y: 1.0, z: 0.25)),
        Box(center: Vec3(x: -14, y: 1.0, z: 0), half: Vec3(x: 0.25, y: 1.0, z: 12)),
        Box(center: Vec3(x: 14, y: 1.0, z: 0), half: Vec3(x: 0.25, y: 1.0, z: 12)),

        // A pillar, so there is something to walk around rather than only into.
        Box(center: Vec3(x: 2.5, y: 1.5, z: -3.4), half: Vec3(x: 0.4, y: 1.5, z: 0.4)),

        // THE PAIR THAT MAKES THE COLLISION CLAIM CHECKABLE. A 0.25 m step is below
        // `Character.stepOffset` and is climbed; a 0.6 m one is above it and is not. The number that
        // separates them is in Character.swift, which is the point: whether this character can climb
        // this stair is a gameplay decision, and the level only has to contain one of each.
        Box(center: Vec3(x: -8.0, y: 0.125, z: 0), half: Vec3(x: 2.0, y: 0.125, z: 6)),
        Box(center: Vec3(x: 6.0, y: 0.30, z: 0), half: Vec3(x: 2.0, y: 0.30, z: 6)),
    ]

    /// Build the level, and **register the whole contract**.
    ///
    /// Registering all eight component types here rather than only `LevelBox` is what makes the
    /// host's negative control mean something. `--no-behaviours` creates this behaviour and neither
    /// of the deciding two, and the host must still be able to resolve the boundary by name — because
    /// what that run is testing is a host with nothing to decide for it, not a host with nothing to
    /// talk to. Registration is idempotent by name, so `Character` and `CameraDirector` asking for the
    /// same types afterwards get the same ids back.
    override func onCreate() throws {
        guard let world else { throw CyberdyneError.invalidHandle }
        let registry = Registry(world: world)
        for component: any Component.Type in [
            PlayerInput.self, CharacterState.self, CharacterSpec.self, CharacterDrive.self,
            CameraSpec.self, CameraIntent.self, AudioCue.self, LevelBox.self,
        ] {
            _ = try registry.type(component)
        }
        let box = try registry.type(LevelBox.self)
        let centerField = LevelBox.field("center")
        let halfField = LevelBox.field("halfExtents")

        for piece in Self.layout {
            // A `LevelBox` and nothing else, so the host's query matches exactly the level and never
            // the character — which carries five components and none of them this one.
            let node = try world.makeEntity()
            try world.add(box, to: node)
            try world.setVec3(piece.center, node, box, field: centerField)
            try world.setVec3(piece.half, node, box, field: halfField)
        }
        Log.info("Level built: \(Self.layout.count) static boxes")
    }
}
