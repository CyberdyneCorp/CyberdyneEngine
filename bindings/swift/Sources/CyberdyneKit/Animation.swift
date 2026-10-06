// SPDX-License-Identifier: MIT
// Animation.swift — ABI 1.7's animation, as a game writes it. Issue #76 stage 4.
//
//     let worker = try Animator.attach(to: entity, rig: "worker")   // in onCreate
//     try worker.play("walk", crossfade: 0.2)                       // in onFixedUpdate
//     try worker.set("speed", to: 1.4)
//     try worker.fire("wave")
//     for event in try Animation.events(for: entity) where event.name == "footstep" { … }  // onUpdate
//
// The animator is the engine's `cy::animation::AnimationSystem` instance on the entity: the rig a
// host registered by name, its compiled state machine, its clips' events and its root motion.
// `play` crossfades to any state of the program whatever its own transitions say; `set` and `fire`
// raise the program's parameters, a fired trigger for exactly one tick.
//
// PHASES. What a character DOES — attach, play, stop, set, fire, choose where root motion goes — is
// simulation: `onFixedUpdate` or initialisation. Taking accumulated root motion is a fixed step's
// alone. The frame's events and a joint's pose are presentation, read in `onUpdate`. Everything
// else answers in every phase.
//
// NAMES. A state or an event comes back as an `AnimationName`: the 64-bit FNV-1a hash of its text,
// `CY_NAME_HASH`, which compares against a string literal directly — `state.state == "walk"`.

import CyberdyneABI
import CyberdyneCore

/// A state, parameter or event name as it crosses the boundary: `CY_NAME_HASH` of its text.
public struct AnimationName: Hashable, Sendable, ExpressibleByStringLiteral,
    CustomStringConvertible
{
    /// FNV-1a over the UTF-8 bytes, 64 bits — `cy::abi::game::name_hash`, value for value.
    public let hash: UInt64

    /// The name `text` hashes to.
    public init(_ text: String) {
        hash = AnimationName.hash(text)
    }

    /// A name as the engine handed it back.
    public init(hash: UInt64) {
        self.hash = hash
    }

    /// A literal name, so `state.state == "walk"` compares against the text it hashes.
    public init(stringLiteral value: String) {
        self.init(value)
    }

    /// `CY_NAME_HASH` of `text`.
    public static func hash(_ text: String) -> UInt64 {
        var value: UInt64 = 14_695_981_039_346_656_037
        for byte in text.utf8 {
            value ^= UInt64(byte)
            value = value &* 1_099_511_628_211
        }
        return value
    }

    /// The hash, in hex: the text does not cross the boundary, so it is not here to print.
    public var description: String { "AnimationName(0x\(String(hash, radix: 16)))" }
}

/// One event a clip fired, in the frame it is delivered in.
public struct AnimationEvent: Equatable, Sendable {
    /// The animator's entity.
    public var entity: Entity
    /// The event's authored name.
    public var name: AnimationName
    /// Where in its clip it sits, 0 to 1.
    public var normalisedTime: Float
    /// Its authored payload.
    public var parameter: Float

    /// From the ABI's spelling.
    public init(_ raw: CyAnimationEvent) {
        entity = Entity(bits: raw.entity)
        name = AnimationName(hash: raw.name)
        normalisedTime = raw.normalised_time
        parameter = raw.parameter
    }
}

/// The animator an entity owns.
public struct Animator: Hashable, Sendable {
    /// The entity whose animator this is.
    public let entity: Entity

    /// The animator `entity` already owns. Nothing is checked until a call is made.
    public init(_ entity: Entity) {
        self.entity = entity
    }

    /// Where the state machine is.
    public struct State: Equatable, Sendable {
        /// The state it is in.
        public var state: AnimationName
        /// The state a blend heads to, or nil with no blend in flight.
        public var target: AnimationName?
        /// How far the blend has come, 0 to 1; 0 with none in flight.
        public var blendWeight: Float
        /// Seconds since it entered `state`.
        public var stateTime: Float
        /// The blend in flight was asked for by `play` or `stop`.
        public var requested: Bool

        /// From the ABI's spelling.
        public init(_ raw: CyAnimatorState) {
            state = AnimationName(hash: raw.state)
            let blending = raw.flags & CY_ANIMATOR_BLENDING != 0
            target = blending ? AnimationName(hash: raw.target) : nil
            blendWeight = raw.blend_weight
            stateTime = raw.state_time
            requested = raw.flags & CY_ANIMATOR_REQUESTED != 0
        }

        /// In `name`, or blending into it.
        public func isPlaying(_ name: AnimationName) -> Bool {
            state == name || target == name
        }
    }

    /// Root motion: a delta in the character's own frame, and the running total.
    public struct RootMotion: Equatable, Sendable {
        /// The root's move over the interval, in the character's own frame, in metres.
        public var translation: Vec3
        /// The root's turn over the interval.
        public var rotation: Quat
        /// The motion curve's distance over the interval, in metres.
        public var distance: Float
        /// Bit 0 the left foot down, bit 1 the right.
        public var contacts: UInt32
        /// Everything since the animator was attached.
        public var travelled: Vec3

        /// From the ABI's spelling.
        public init(_ raw: CyRootMotion) {
            translation = Vec3(raw.translation)
            rotation = Quat(
                x: raw.rotation.0, y: raw.rotation.1, z: raw.rotation.2, w: raw.rotation.3)
            distance = raw.distance
            contacts = raw.contacts
            travelled = Vec3(raw.travelled)
        }
    }

    /// Give `entity` an animator over the rig the host registered as `rig`. It exists when this
    /// returns, so it can be played at once. Throws `.alreadyExists` when it has one and
    /// `.notFound` for an unknown rig.
    @discardableResult
    public static func attach(
        to entity: Entity, rig: String, tier: AnimationTier = .full, emitsEvents: Bool = true,
        rootMotion: RootMotionMode = .ignore, playRate: Float = 1
    ) throws -> Animator {
        let engine = try GameServices.engine()
        try rig.withCString { name in
            var desc = CyAnimatorDesc()
            desc.struct_size = UInt32(MemoryLayout<CyAnimatorDesc>.size)
            desc.flags = emitsEvents ? 0 : CY_ANIMATOR_SUPPRESS_EVENTS
            desc.rig = name
            desc.tier = tier.rawValue
            desc.root_motion = rootMotion.rawValue
            desc.play_rate = playRate
            try engine.animationAttach(entity: entity.bits, desc: &desc)
        }
        return Animator(entity)
    }

    /// Remove it.
    public func detach() throws {
        try GameServices.engine().animationDetach(entity: entity.bits)
    }

    /// Crossfade to `state` over `crossfade` seconds, or cut with zero, whatever the program's own
    /// transitions say. The blend runs to completion.
    public func play(_ state: String, crossfade: Float = 0.2) throws {
        let engine = try GameServices.engine()
        try state.withCString {
            try engine.animationPlay(entity: entity.bits, state: $0, crossfade: crossfade)
        }
    }

    /// Blend back to the program's entry state.
    public func stop(blend: Float = 0.2) throws {
        try GameServices.engine().animationStop(entity: entity.bits, blend: blend)
    }

    /// Set a program parameter: a blend weight, a speed.
    public func set(_ parameter: String, to value: Float) throws {
        let engine = try GameServices.engine()
        try parameter.withCString {
            try engine.animationSetFloat(entity: entity.bits, parameter: $0, value: value)
        }
    }

    /// Set a condition parameter.
    public func set(_ parameter: String, to value: Bool) throws {
        let engine = try GameServices.engine()
        try parameter.withCString {
            try engine.animationSetBool(entity: entity.bits, parameter: $0, value: value)
        }
    }

    /// Raise a trigger: the parameter reads 1 for exactly the next tick, then 0.
    public func fire(_ trigger: String) throws {
        let engine = try GameServices.engine()
        try trigger.withCString {
            try engine.animationFireTrigger(entity: entity.bits, parameter: $0)
        }
    }

    /// A parameter's value.
    public func float(_ parameter: String) throws -> Float {
        let engine = try GameServices.engine()
        var value: Float = 0
        try parameter.withCString {
            try engine.animationGetFloat(entity: entity.bits, parameter: $0, into: &value)
        }
        return value
    }

    /// Where the state machine is.
    public var state: State {
        get throws {
            var raw = CyAnimatorState()
            raw.struct_size = UInt32(MemoryLayout<CyAnimatorState>.size)
            try GameServices.engine().animationState(entity: entity.bits, into: &raw)
            return State(raw)
        }
    }

    /// The last tick's root motion and the running total.
    public var rootMotion: RootMotion {
        get throws {
            var raw = CyRootMotion()
            raw.struct_size = UInt32(MemoryLayout<CyRootMotion>.size)
            try GameServices.engine().animationRootMotion(entity: entity.bits, into: &raw)
            return RootMotion(raw)
        }
    }

    /// Everything accumulated since the last take, and clear it. For an animator attached with
    /// `.accumulate`; fixed update only.
    public func takeRootMotion() throws -> RootMotion {
        var raw = CyRootMotion()
        raw.struct_size = UInt32(MemoryLayout<CyRootMotion>.size)
        try GameServices.engine().animationTakeRootMotion(entity: entity.bits, into: &raw)
        return RootMotion(raw)
    }

    /// Choose where root motion goes. `.character` moves the entity's character controller by it
    /// every tick, and needs one.
    public func setRootMotion(_ mode: RootMotionMode) throws {
        try GameServices.engine().animationSetRootMotion(entity: entity.bits, mode: mode.rawValue)
    }

    /// Joint `joint`'s placement in the world, from the pose last evaluated. Frame update only.
    public func jointPose(_ joint: String) throws -> Pose {
        let engine = try GameServices.engine()
        var raw = CyPose()
        try joint.withCString {
            try engine.animationJointPose(entity: entity.bits, joint: $0, into: &raw)
        }
        return Pose(raw)
    }
}

/// The events every animator's clips fired.
public enum Animation {
    /// The events of the ticks since the previous frame, in tick, rig and instance order — each
    /// delivered in exactly one frame. Frame update only.
    public static func events() throws -> [AnimationEvent] {
        let engine = try GameServices.engine()
        var count: UInt32 = 0
        try engine.animationEvents(into: nil, capacity: 0, count: &count)
        if count == 0 {
            return []
        }
        var raw = [CyAnimationEvent](repeating: CyAnimationEvent(), count: Int(count))
        try raw.withUnsafeMutableBufferPointer { buffer in
            try engine.animationEvents(
                into: buffer.baseAddress, capacity: UInt32(buffer.count), count: &count)
        }
        return raw.prefix(Int(count)).map(AnimationEvent.init)
    }

    /// This frame's events from `entity`'s animator.
    public static func events(for entity: Entity) throws -> [AnimationEvent] {
        try events().filter { $0.entity == entity }
    }
}
