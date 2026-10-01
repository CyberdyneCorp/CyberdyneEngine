// SPDX-License-Identifier: MIT
// Characters.swift — ABI 1.5's character controllers, as a game writes them. `add-swift-m12-gaps`.
//
//     let hero = try CharacterController.create(on: entity,
//                                               CharacterController.Description(radius: 0.4))
//     try hero.move(velocity: input * speed, jump: jumpPressed ? 5 : nil)   // in onFixedUpdate
//     if try hero.state.isGrounded { … }
//
// The controller is the engine's `cy::physics::CharacterController`: a capsule moved by
// collide-and-slide over the physics server's queries, with slopes, stairs, a ceiling, pushing and
// moving platforms, and it walks the same over every physics backend. Its kinematic body carries
// the entity, so `Physics.raycast` hits the character and names it.
//
// PHASES. `create` and `destroy` are simulation (`onFixedUpdate` or initialisation). `move` is ONE
// FIXED STEP — fixed update only, and the step's length is the engine's fixed delta, so stair
// behaviour can never come to depend on the frame rate. `state` answers in every phase.
//
// DETERMINISM. A move is applied at the call and the state reads back at once; characters moved in
// one tick move in call order, which is script order.

import CyberdyneABI
import CyberdyneCore

/// The character controller an entity owns.
public struct CharacterController: Hashable, Sendable {
    /// The entity whose character this is.
    public let entity: Entity

    /// The controller `entity` already owns. Nothing is checked until a call is made.
    public init(_ entity: Entity) {
        self.entity = entity
    }

    /// How a character is built. A nil field is the engine's default (`cy::physics::
    /// CharacterDescription`): a 0.3 m by 1.8 m capsule, 45 degree slopes, 0.35 m steps.
    public struct Description: Equatable, Sendable {
        /// Capsule radius in metres.
        public var radius: Float?
        /// Total height, both caps included.
        public var height: Float?
        /// The steepest slope it can stand on, in radians.
        public var maxSlopeRadians: Float?
        /// The highest step it climbs without jumping, in metres.
        public var stepOffset: Float?
        /// A multiplier on world gravity for this character.
        public var gravityScale: Float?
        /// Six degrees of freedom: no gravity, no ground, no steps.
        public var floating: Bool
        /// Push dynamic bodies it walks into.
        public var pushesBodies: Bool
        /// The collision layer, 0 to 31, and the layers it collides with.
        public var layer: UInt32
        /// The layers it collides with, one bit per layer.
        public var mask: UInt32
        /// Where it starts.
        public var start: Pose

        /// A description; every field left out keeps the engine's default.
        public init(
            radius: Float? = nil, height: Float? = nil, maxSlopeRadians: Float? = nil,
            stepOffset: Float? = nil, gravityScale: Float? = nil, floating: Bool = false,
            pushesBodies: Bool = true, layer: UInt32 = 0, mask: UInt32 = .max,
            start: Pose = Pose()
        ) {
            self.radius = radius
            self.height = height
            self.maxSlopeRadians = maxSlopeRadians
            self.stepOffset = stepOffset
            self.gravityScale = gravityScale
            self.floating = floating
            self.pushesBodies = pushesBodies
            self.layer = layer
            self.mask = mask
            self.start = start
        }

        /// In the ABI's spelling, where zero means "the default".
        public var raw: CyCharacterDesc {
            var desc = CyCharacterDesc()
            desc.struct_size = UInt32(MemoryLayout<CyCharacterDesc>.size)
            desc.flags =
                (floating ? CY_CHARACTER_FLOATING : 0) | (pushesBodies ? 0 : CY_CHARACTER_NO_PUSH)
            desc.radius = radius ?? 0
            desc.height = height ?? 0
            desc.max_slope_radians = maxSlopeRadians ?? 0
            desc.step_offset = stepOffset ?? 0
            desc.gravity_scale = gravityScale ?? 0
            desc.layer = layer
            desc.mask = mask
            desc.start = start.raw
            return desc
        }
    }

    /// What the last move produced.
    public struct State: Equatable, Sendable {
        /// Whether it stood on ground, a steep slope or nothing after the move.
        public var ground: GroundState
        /// The entity owning what it stands on, or `.null`.
        public var groundEntity: Entity
        /// The capsule's centre, world space.
        public var position: Vec3
        /// Its velocity after gravity, collisions and the platform, world space, per second.
        public var velocity: Vec3
        /// The normal of what it stands on; meaningless when it is in the air.
        public var groundNormal: Vec3
        /// The platform's share of the motion, per second.
        public var platformVelocity: Vec3
        /// The move ended against something above it.
        public var touchingCeiling: Bool
        /// The move ended against something beside it.
        public var touchingWall: Bool
        /// Lifted onto a stair by the last move.
        public var steppedUp: Bool

        /// Standing on walkable ground, the usual test before a jump.
        public var isGrounded: Bool { ground == .grounded }

        /// From the ABI's spelling.
        public init(_ raw: CyCharacterState) {
            ground = GroundState(rawValue: raw.ground) ?? .inAir
            groundEntity = Entity(bits: raw.ground_entity)
            position = Vec3(raw.position)
            velocity = Vec3(raw.velocity)
            groundNormal = Vec3(raw.ground_normal)
            platformVelocity = Vec3(raw.platform_velocity)
            touchingCeiling = raw.flags & CY_CHARACTER_TOUCHING_CEILING != 0
            touchingWall = raw.flags & CY_CHARACTER_TOUCHING_WALL != 0
            steppedUp = raw.flags & CY_CHARACTER_STEPPED_UP != 0
        }
    }

    /// Give `entity` a character. Throws `.alreadyExists` when it has one.
    @discardableResult
    public static func create(
        on entity: Entity, _ description: Description = Description()
    )
        throws -> CharacterController
    {
        var desc = description.raw
        try GameServices.engine().characterCreate(entity: entity.bits, desc: &desc)
        return CharacterController(entity)
    }

    /// Remove it, and its body.
    public func destroy() throws {
        try GameServices.engine().characterDestroy(entity: entity.bits)
    }

    /// One fixed step at `velocity` (metres per second, world space; in grounded mode the vertical
    /// part is ignored unless jumping), jumping at `jump` metres per second when it is not nil.
    public func move(velocity: Vec3, jump: Float? = nil) throws {
        var input = CyCharacterInput()
        input.struct_size = UInt32(MemoryLayout<CyCharacterInput>.size)
        input.desired_velocity = velocity.tuple
        if let jump {
            input.flags = CY_CHARACTER_JUMP
            input.jump_speed = jump
        }
        try GameServices.engine().characterMove(entity: entity.bits, input: &input)
    }

    /// What the last move produced.
    public var state: State {
        get throws {
            var raw = CyCharacterState()
            raw.struct_size = UInt32(MemoryLayout<CyCharacterState>.size)
            try GameServices.engine().characterState(entity: entity.bits, into: &raw)
            return State(raw)
        }
    }
}
